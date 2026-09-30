// GBuffer.hlsl
// Geometry pass for the deferred renderer.
// Writes albedo, world-space normal, and roughness/metallic/AO into three
// separate render targets.  No lighting is computed here.

// Supplies the reflectivity knob's encoding; the deferred resolve, SSR and the ray-traced
// specular pass all decode it from the same header.
#include "SurfaceSpecular.hlsli"
#include "Tessellation.hlsli"
#include "TangentFrame.hlsli"

cbuffer EntityConstants : register(b0)
{
    float4x4 gMVP;      // pre-transposed model-view-projection
    float4x4 gModel;    // pre-transposed model-to-world matrix
};

// Per-draw material parameters, uploaded from the JSON material definition.
cbuffer MaterialConstants : register(b1)
{
    float4 gBaseColorTint;      // RGBA tint multiplied with base-color texture
    float3 gEmissiveColor;      float _MatPad0;
    float  gMetallicFactor;     // scalar multiplied with metallic texture (or used directly)
    float  gRoughnessFactor;    // scalar multiplied with roughness texture (or used directly)
    float  gNormalScale;        // tangent-space normal intensity
    float  gAoStrength;         // ambient-occlusion blend strength
    // Texture presence flags (1 = texture bound, 0 = use factor only).
    int    gHasNormalMap;
    int    gHasMetallicMap;
    int    gHasRoughnessMap;
    int    gHasAoMap;
    int    gHasEmissiveMap;
    // 0 = separate maps; 1 = one packed map, R = roughness, G = metallic, B = AO (RMA);
    // 2 = one packed map, R = AO, G = roughness, B = metallic (ORM, Unreal's layout).
    int    gHasPackedMaterialMap;
    // Reflectivity, 0.5 = neutral. Scales the surface's normal-incidence reflectance, so
    // it is what gives a metal a specular response when the scene offers it nothing to
    // mirror. Travels to the lighting pass in the normal target's W channel.
    float  gSpecularFactor;
    // Subsurface-scattering profile slot (Subsurface.hlsli), 0 = none. Packed above the
    // specular value in the same channel.
    int    gSubsurfaceSlot;
    float  gOpacityFactor;
    float  gAlphaCutoff;
    int    gHasOpacityMap;
    int    gUseAlphaCutout;
    int    gUseTransparentBlend;
    // 1 = negate the normal map's green channel: an OpenGL-convention map (green up)
    // read in the engine's DirectX convention (green down the texture).
    int    gFlipNormalGreen;
    float2 _MatPad2;
    // UV transform: rotate the source UVs about (0.5, 0.5), scale by tiling, then offset.
    // The rotation arrives pre-resolved as sin/cos so the shader does no trigonometry.
    float2 gUvTiling;
    float2 gUvOffset;
    float  gUvRotationSin;
    float  gUvRotationCos;
    // Parallax occlusion mapping.
    int    gHasHeightMap;
    int    gUseParallaxOcclusion;
    float  gParallaxHeightScale;   // depth of the height volume, in tiled UV units
    int    gParallaxMinSteps;      // steps when looking straight down at the surface
    int    gParallaxMaxSteps;      // steps at grazing angles, where the ray travels furthest
    float  gParallaxFadeDistance;  // metres; 0 disables the distance fade
    float3 gCameraPositionWS;
    // Height value that sits at the polygon surface. 1 suits a 0-1 height map, where
    // white is the top of the volume. Substance-style maps are signed around 0.5 and
    // never reach 1, so without this the whole surface sits half a volume deep and most
    // of the offset is a uniform slab shift instead of relief.
    float  gParallaxReferenceHeight;
    // Tessellation + displacement (only read by the tessellated pipeline, HSMain/DSMain).
    int    gUseTessellation;
    float  gTessMaxFactor;
    float  gTessTargetPixels;       // on-screen length of one tessellated edge
    float  gTessFadeDistance;       // metres; 0 = no fade
    float  gDisplacementScale;      // metres between black and white
    float  gDisplacementMidLevel;   // height value that stays on the surface
    float  gTessPixelScale;         // (viewport height / 2) / tan(fovY / 2)
    float  _MatPad3;
};

cbuffer RainSurfaceConstants : register(b2)
{
    float gRainWetnessIntensity;
    float gRainEnabled;
    float2 _RainPad0;
};

// Texture slots – all optional; fallback textures are bound when absent.
Texture2D    gBaseColorTexture  : register(t0);  // diffuse / albedo
Texture2D    gNormalTexture     : register(t1);  // tangent-space normal map
Texture2D    gMetallicTexture   : register(t2);  // metallic (R channel)
Texture2D    gRoughnessTexture  : register(t3);  // roughness (R channel)
Texture2D    gAoTexture         : register(t4);  // ambient occlusion (R channel)
Texture2D    gEmissiveTexture   : register(t5);  // emissive (RGB)
Texture2D    gOpacityTexture    : register(t6);  // opacity (R)
Texture2D    gHeightTexture     : register(t7);  // height / displacement (R, white = high)
SamplerState gLinearSampler     : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

struct PSInput
{
    float4 Position      : SV_Position;
    float3 WorldPosition : WORLDPOS;
    float3 WorldNormal   : NORMAL;
    float2 TexCoord      : TEXCOORD;
    float4 Color         : COLOR;
};

// G-Buffer outputs:
//   SV_Target0 – Albedo  (RGB = base colour, A = unused)
//   SV_Target1 – Normal  (RGB = world-space normal encoded to [0,1])
//   SV_Target2 – Material (R = roughness, G = metallic, B = AO, A = unused)
struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;
};

float ComputeRainWetnessMask(float3 worldNormal)
{
    return saturate(dot(normalize(worldNormal), float3(0.0f, 0.0f, 1.0f)));
}

float2 OctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * (float2(v.xy >= 0.0f) * 2.0f - 1.0f);
}

float2 EncodeOctNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 oct = (n.z >= 0.0f) ? n.xy : OctWrap(n.xy);
    return oct * 0.5f + 0.5f;
}

// Largest distance, in height-map texels, that one parallax step is allowed to cover.
// The step budget is fixed, so this is what bounds how far a grazing ray may sweep.
static const float kParallaxMaxTexelsPerStep = 4.0f;

// Applies the material's UV transform to the mesh's authored texture coordinates.
float2 TransformUv(float2 uv)
{
    const float2 centered = uv - 0.5f;
    const float2 rotated = float2(
        centered.x * gUvRotationCos - centered.y * gUvRotationSin,
        centered.x * gUvRotationSin + centered.y * gUvRotationCos) + 0.5f;
    return rotated * gUvTiling + gUvOffset;
}

// Depth below the reference plane, so a height at or above the reference sits flush
// with the polygon and only what is below it is carved in.
float SampleSurfaceDepth(float2 uv, float2 uvDdx, float2 uvDdy)
{
    return saturate(gParallaxReferenceHeight
                  - gHeightTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy).r);
}

// Ray marches the height field along the tangent-space view ray and returns the UV of
// the first point the ray hits. Steep-parallax march plus one secant refinement, which
// removes the stair-stepping a fixed-layer march leaves on shallow slopes.
// uvSpan is the UV the ray travels while crossing the full depth of the height volume.
float2 ParallaxOcclusionUv(
    float2 baseUv,
    float2 uvDdx,
    float2 uvDdy,
    float2 uvSpan,
    float  stepCount)
{
    const float layerStep = 1.0f / stepCount;
    const float2 uvStep = uvSpan * layerStep;

    float  rayDepth = 0.0f;   // how far below the top plane the ray has travelled, 0..1
    float2 uv = baseUv;
    float  surfaceDepth = SampleSurfaceDepth(uv, uvDdx, uvDdy);

    // SampleGrad, not Sample: the loop is dynamic, so the hardware cannot derive mip
    // derivatives itself. The gradients of the unoffset UV are the right ones to use.
    [loop]
    for (int marchStep = 0; marchStep < (int)stepCount && rayDepth < surfaceDepth; ++marchStep)
    {
        uv -= uvStep;
        rayDepth += layerStep;
        surfaceDepth = SampleSurfaceDepth(uv, uvDdx, uvDdy);
    }

    // Interpolate between the last step outside the surface and the first one inside it.
    // Once the march has crossed, depthAfter is <= 0 and depthBefore is > 0, so the
    // denominator is strictly negative. Clamping it up towards +epsilon therefore
    // replaced it with +1e-5 on every single pixel, drove the blend negative, and left
    // saturate() returning 0 - so this refinement never ran and the march always handed
    // back its raw quantised step. Clamp from below instead, away from zero.
    const float2 previousUv = uv + uvStep;
    const float depthAfter  = surfaceDepth - rayDepth;
    const float depthBefore = SampleSurfaceDepth(previousUv, uvDdx, uvDdy)
                            - (rayDepth - layerStep);
    const float blend = saturate(depthAfter / min(depthAfter - depthBefore, -1e-6f));
    return lerp(uv, previousUv, blend);
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position      = mul(float4(input.Position, 1.0f), gMVP);
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.WorldNormal   = mul(float4(input.Normal,   0.0f), gModel).xyz;
    output.TexCoord      = input.TexCoord;
    output.Color         = input.Color;
    return output;
}

// ---------------------------------------------------------------------------
// Tessellated path (materials with Tessellation enabled and a height map).
// VSMainTess -> HSMain -> fixed-function tessellator -> DSMain -> PSMain.
// ---------------------------------------------------------------------------

struct TessControlPoint
{
    float3 LocalPosition : POSITION;
    float3 LocalNormal   : NORMAL;
    float2 TexCoord      : TEXCOORD;
    float4 Color         : COLOR;
    float3 WorldPosition : WORLDPOS;
    float4 ClipPosition  : CLIPPOS;
};

TessControlPoint VSMainTess(VSInput input)
{
    TessControlPoint output;
    output.LocalPosition = input.Position;
    output.LocalNormal   = input.Normal;
    output.TexCoord      = input.TexCoord;
    output.Color         = input.Color;
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.ClipPosition  = mul(float4(input.Position, 1.0f), gMVP);
    return output;
}

PteroTessPatchConstants HSConstants(InputPatch<TessControlPoint, 3> patch)
{
    PteroTessPatchConstants output;

    const float slack = abs(gDisplacementScale) * 3.0f;
    if (PteroTessPatchOutsideFrustum(patch[0].ClipPosition, patch[1].ClipPosition, patch[2].ClipPosition, slack))
    {
        // A zero factor discards the patch.
        output.Edges[0] = output.Edges[1] = output.Edges[2] = 0.0f;
        output.Inside = 0.0f;
        output.DisplacementMip = 0.0f;
        return output;
    }

    [unroll]
    for (int edge = 0; edge < 3; ++edge)
    {
        output.Edges[edge] = PteroTessEdgeFactor(
            patch[(edge + 1) % 3].WorldPosition, patch[(edge + 2) % 3].WorldPosition,
            gCameraPositionWS, gTessPixelScale, gTessTargetPixels, gTessMaxFactor, gTessFadeDistance);
    }
    output.Inside = max(output.Edges[0], max(output.Edges[1], output.Edges[2]));

    float heightWidth, heightHeight;
    gHeightTexture.GetDimensions(heightWidth, heightHeight);
    output.DisplacementMip = PteroDisplacementMip(
        TransformUv(patch[0].TexCoord), TransformUv(patch[1].TexCoord), TransformUv(patch[2].TexCoord),
        output.Inside, float2(heightWidth, heightHeight));
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConstants")]
[maxtessfactor(64.0)]
TessControlPoint HSMain(InputPatch<TessControlPoint, 3> patch, uint pointId : SV_OutputControlPointID)
{
    return patch[pointId];
}

[domain("tri")]
PSInput DSMain(
    PteroTessPatchConstants patchConstants,
    float3 barycentrics : SV_DomainLocation,
    const OutputPatch<TessControlPoint, 3> patch)
{
    float3 localPosition = patch[0].LocalPosition * barycentrics.x
                         + patch[1].LocalPosition * barycentrics.y
                         + patch[2].LocalPosition * barycentrics.z;
    const float3 localNormal = normalize(patch[0].LocalNormal * barycentrics.x
                                       + patch[1].LocalNormal * barycentrics.y
                                       + patch[2].LocalNormal * barycentrics.z);
    const float2 texCoord = patch[0].TexCoord * barycentrics.x
                          + patch[1].TexCoord * barycentrics.y
                          + patch[2].TexCoord * barycentrics.z;
    const float4 color = patch[0].Color * barycentrics.x
                       + patch[1].Color * barycentrics.y
                       + patch[2].Color * barycentrics.z;

    // Displace in object space.  gDisplacementScale is in world metres, so divide by
    // how long a unit object-space normal is in world space (exact for uniform scale).
    if (gUseTessellation != 0 && gHasHeightMap != 0)
    {
        const float height = gHeightTexture.SampleLevel(
            gLinearSampler, TransformUv(texCoord), patchConstants.DisplacementMip).r;
        const float worldPerLocal = max(length(mul(float4(localNormal, 0.0f), gModel).xyz), 1e-6f);
        localPosition += localNormal
                       * ((height - gDisplacementMidLevel) * gDisplacementScale / worldPerLocal);
    }

    PSInput output;
    output.Position      = mul(float4(localPosition, 1.0f), gMVP);
    output.WorldPosition = mul(float4(localPosition, 1.0f), gModel).xyz;
    // The interpolated normal is the undisplaced one; the normal map carries the
    // displaced detail, which is what a displacement map's companion normal map is for.
    output.WorldNormal   = mul(float4(localNormal, 0.0f), gModel).xyz;
    output.TexCoord      = texCoord;
    output.Color         = color;
    return output;
}

PSOutput PSMain(PSInput input)
{
    PSOutput output;

    float3 N = normalize(input.WorldNormal);

    // --- UV transform and parallax occlusion ---
    // Everything below samples through "uv", so tiling and the parallax offset apply to
    // every map at once and stay in agreement with each other.
    float2 uv = TransformUv(input.TexCoord);

    // Derivatives of the untouched UV drive mip selection throughout the parallax march;
    // they must be taken before the march so no derivative is read inside a loop.
    const float2 uvDdx = ddx(uv);
    const float2 uvDdy = ddy(uv);

    float3 T, B;
    float2 worldUnitsPerUv;
    BuildTangentFrame(input.WorldPosition, uv, N, T, B, worldUnitsPerUv);

    if (gUseParallaxOcclusion && gHasHeightMap && worldUnitsPerUv.x > 0.0f)
    {
        const float3 toEye = gCameraPositionWS - input.WorldPosition;
        const float  distanceToEye = length(toEye);

        // Past the fade distance a pixel covers more height texels than the march could
        // resolve, so the offset is worthless and the steps are pure cost.
        const float distanceFade = (gParallaxFadeDistance > 0.0f)
            ? saturate(1.0f - distanceToEye / gParallaxFadeDistance)
            : 1.0f;

        if (distanceFade > 0.0f && distanceToEye > 1e-4f)
        {
            const float3 V = toEye / distanceToEye;
            const float3 viewDirTangent = float3(dot(V, T), dot(V, B), dot(V, N));

            // Backfacing pixels (possible on double-sided materials) have no height
            // volume in front of them; marching there produces garbage offsets.
            if (viewDirTangent.z > 1e-3f)
            {
                // Grazing rays cross more texels, so they need more steps to stay stable.
                const float stepCount = clamp(
                    lerp((float)gParallaxMaxSteps, (float)gParallaxMinSteps, saturate(viewDirTangent.z)),
                    1.0f, 256.0f);

                // gParallaxHeightScale is a UV-space depth and has to stay one. Routing
                // it through world units and back per axis looks more rigorous but folds
                // the mesh's own UV density back into a value that was deliberately
                // independent of it, so a stretched UV island silently multiplies the
                // sweep - by up to 3.3x on the tavern wall bay, and differently on every
                // triangle. Keep the sweep purely in texture space.
                float2 uvSpan = (viewDirTangent.xy / viewDirTangent.z)
                              * gParallaxHeightScale * distanceFade;

                // Bound the sweep so one step never skips more than a few height texels.
                // The step budget is fixed, so without this the sweep grows without limit
                // as the angle steepens - hundreds of UV units at a few degrees of
                // grazing - and the march samples what is effectively noise, which drags
                // surface detail into long smears along the view direction. The limit is
                // approached smoothly: clamping the length outright pins every pixel past
                // the threshold to the same offset, and the boundary itself shows up as a
                // ring across the surface.
                float heightWidth, heightHeight;
                gHeightTexture.GetDimensions(heightWidth, heightHeight);
                const float maxSweep = stepCount * kParallaxMaxTexelsPerStep
                                     / max(max(heightWidth, heightHeight), 1.0f);
                // Fourth-order rolloff rather than second: it leaves the sweep
                // essentially untouched while the march can still resolve it, instead of
                // shaving a sixth off it at ordinary viewing angles, and still tends to
                // maxSweep as the ray goes tangential.
                const float sweepRatio = length(uvSpan) / max(maxSweep, 1e-9f);
                const float sweepRatioSq = sweepRatio * sweepRatio;
                uvSpan *= rsqrt(sqrt(1.0f + sweepRatioSq * sweepRatioSq));

                uv = ParallaxOcclusionUv(uv, uvDdx, uvDdy, uvSpan, stepCount);
            }
        }
    }

    // --- Albedo ---
    float4 texColor = gBaseColorTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);
    float opacity = texColor.a * input.Color.a * gBaseColorTint.a * saturate(gOpacityFactor);
    if (gHasOpacityMap)
    {
        opacity *= gOpacityTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy).r;
    }
    if (gUseAlphaCutout)
    {
        clip(opacity - saturate(gAlphaCutoff));
    }
    // Tint: per-vertex colour (from material slot) × JSON baseColorTint × texture.
    float3 baseAlbedo = texColor.rgb * input.Color.rgb * gBaseColorTint.rgb;

    // --- Normal ---
    if (gHasNormalMap)
    {
        // BC5 stores only XY; rebuild Z after remapping into tangent space.
        float2 tsNormalXY = gNormalTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy).rg * 2.0f - 1.0f;
        if (gFlipNormalGreen != 0)
            tsNormalXY.y = -tsNormalXY.y;
        float3 tsNormal = float3(tsNormalXY, sqrt(saturate(1.0f - dot(tsNormalXY, tsNormalXY))));
        tsNormal.xy *= gNormalScale;
        tsNormal.z = sqrt(saturate(1.0f - dot(tsNormal.xy, tsNormal.xy)));

        // Same UV-aligned frame the parallax march used, so the shading and the
        // displaced detail describe the same surface.
        N = normalize(T * tsNormal.x + B * tsNormal.y + N * tsNormal.z);
    }
    // Pack the normal target as oct-encoded world normal in XY plus scene depth in Z, and
    // the material's reflectivity (plus its subsurface profile slot) in W.
    // RTGI reads this surface texture as "normal + depth" for world-position
    // reconstruction and neighbourhood validation, while the deferred lighting pass
    // decodes the normal back from the XY oct representation.
    const float2 octNormal = EncodeOctNormal(N);
    output.Normal = float4(octNormal, input.Position.z,
        PteroEncodeSurfaceSpecularAndSubsurface(gSpecularFactor, (uint)max(gSubsurfaceSlot, 0)));

    // --- Material (roughness, metallic, AO) ---
    float4 metallicSample = gMetallicTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);
    float4 roughnessSample = gRoughnessTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);
    float4 aoSample = gAoTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);

    float roughness = gHasRoughnessMap
        ? (gHasPackedMaterialMap == 2 ? roughnessSample.g : roughnessSample.r) * gRoughnessFactor
        : gRoughnessFactor;

    float metallic = gHasMetallicMap
        ? (gHasPackedMaterialMap == 2 ? metallicSample.b : gHasPackedMaterialMap ? metallicSample.g : metallicSample.r) * gMetallicFactor
        : gMetallicFactor;

    float ao = gHasAoMap
        ? lerp(1.0f, gHasPackedMaterialMap == 2 ? aoSample.r : gHasPackedMaterialMap ? aoSample.b : aoSample.r, gAoStrength)
        : 1.0f;

    const float rainWetness = gRainWetnessIntensity * saturate(gRainEnabled) * ComputeRainWetnessMask(N);
    baseAlbedo *= lerp(1.0f, 0.6f, rainWetness);
    roughness = lerp(roughness, 0.02f, rainWetness);

    // Lowering the explicit opacity factor or assigning a dedicated opacity
    // texture should work immediately from the editor, including for older
    // multi-materials that did not persist the blend toggle. Base-colour alpha
    // remains opt-in so ordinary RGBA textures do not unexpectedly turn translucent.
    const bool useTransparentBlend = gUseTransparentBlend
        || (gHasOpacityMap && !gUseAlphaCutout)
        || (!gUseAlphaCutout && gOpacityFactor < 0.9999f);
    output.Albedo = float4(baseAlbedo, useTransparentBlend ? saturate(opacity) : 1.0f);

    output.Material = float4(roughness, metallic, ao, 0.0f);

    return output;
}
