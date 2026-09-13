// GBuffer.hlsl
// Geometry pass for the deferred renderer.
// Writes albedo, world-space normal, and roughness/metallic/AO into three
// separate render targets.  No lighting is computed here.

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
    int    gHasPackedMaterialMap;
    float2 _MatPad1;
    float  gOpacityFactor;
    float  gAlphaCutoff;
    int    gHasOpacityMap;
    int    gUseAlphaCutout;
    int    gUseTransparentBlend;
    float3 _MatPad2;
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

// Applies the material's UV transform to the mesh's authored texture coordinates.
float2 TransformUv(float2 uv)
{
    const float2 centered = uv - 0.5f;
    const float2 rotated = float2(
        centered.x * gUvRotationCos - centered.y * gUvRotationSin,
        centered.x * gUvRotationSin + centered.y * gUvRotationCos) + 0.5f;
    return rotated * gUvTiling + gUvOffset;
}

// Builds a tangent frame aligned with the UV layout, derived from screen-space
// derivatives so no tangent vertex attribute is needed. Parallax only makes sense in
// a UV-aligned frame: the height volume is addressed in texture space, so the marching
// direction has to be expressed there too. Meshes whose UVs are degenerate (a flat or
// missing UV set gives zero derivatives) fall back to an arbitrary frame around N.
void BuildTangentFrame(float3 worldPosition, float2 uv, float3 N, out float3 T, out float3 B)
{
    const float3 dpdx = ddx(worldPosition);
    const float3 dpdy = ddy(worldPosition);
    const float2 duvdx = ddx(uv);
    const float2 duvdy = ddy(uv);

    // Solve for the tangent vectors in the plane perpendicular to N.
    const float3 dpdyPerp = cross(dpdy, N);
    const float3 dpdxPerp = cross(N, dpdx);
    float3 tangent   = dpdyPerp * duvdx.x + dpdxPerp * duvdy.x;
    float3 bitangent = dpdyPerp * duvdx.y + dpdxPerp * duvdy.y;

    if (max(dot(tangent, tangent), dot(bitangent, bitangent)) < 1e-16f)
    {
        const float3 up = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        T = normalize(cross(up, N));
        B = cross(N, T);
        return;
    }

    T = normalize(tangent);
    B = normalize(bitangent);
}

// Ray marches the height field along the tangent-space view ray and returns the UV of
// the first point the ray hits. Steep-parallax march plus one secant refinement, which
// removes the stair-stepping a fixed-layer march leaves on shallow slopes.
float2 ParallaxOcclusionUv(
    float2 baseUv,
    float2 uvDdx,
    float2 uvDdy,
    float3 viewDirTangent,   // surface -> eye, tangent space
    float  heightScale,
    float  stepCount)
{
    const float layerStep = 1.0f / stepCount;

    // UV travelled by a ray crossing the full depth of the height volume. Grazing rays
    // (small |z|) sweep much further across the texture than head-on ones.
    const float2 uvSpan = (viewDirTangent.xy / max(abs(viewDirTangent.z), 1e-4f)) * heightScale;
    const float2 uvStep = uvSpan * layerStep;

    float  rayDepth = 0.0f;   // how far below the top plane the ray has travelled, 0..1
    float2 uv = baseUv;
    float  surfaceDepth = 1.0f - gHeightTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy).r;

    // SampleGrad, not Sample: the loop is dynamic, so the hardware cannot derive mip
    // derivatives itself. The gradients of the unoffset UV are the right ones to use.
    [loop]
    for (int marchStep = 0; marchStep < (int)stepCount && rayDepth < surfaceDepth; ++marchStep)
    {
        uv -= uvStep;
        rayDepth += layerStep;
        surfaceDepth = 1.0f - gHeightTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy).r;
    }

    // Interpolate between the last step outside the surface and the first one inside it.
    const float2 previousUv = uv + uvStep;
    const float depthAfter  = surfaceDepth - rayDepth;
    const float depthBefore = (1.0f - gHeightTexture.SampleGrad(gLinearSampler, previousUv, uvDdx, uvDdy).r)
                            - (rayDepth - layerStep);
    const float blend = depthAfter / max(depthAfter - depthBefore, 1e-5f);
    return lerp(uv, previousUv, saturate(blend));
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
    BuildTangentFrame(input.WorldPosition, uv, N, T, B);

    if (gUseParallaxOcclusion && gHasHeightMap)
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

                const float2 parallaxUv = ParallaxOcclusionUv(
                    uv, uvDdx, uvDdy, viewDirTangent,
                    gParallaxHeightScale * distanceFade, stepCount);

                uv = parallaxUv;
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
        float3 tsNormal = float3(tsNormalXY, sqrt(saturate(1.0f - dot(tsNormalXY, tsNormalXY))));
        tsNormal.xy *= gNormalScale;
        tsNormal.z = sqrt(saturate(1.0f - dot(tsNormal.xy, tsNormal.xy)));

        // Same UV-aligned frame the parallax march used, so the shading and the
        // displaced detail describe the same surface.
        N = normalize(T * tsNormal.x + B * tsNormal.y + N * tsNormal.z);
    }
    // Pack the normal target as oct-encoded world normal in XY plus scene depth in Z.
    // RTGI reads this surface texture as "normal + depth" for world-position
    // reconstruction and neighbourhood validation, while the deferred lighting pass
    // decodes the normal back from the XY oct representation.
    const float2 octNormal = EncodeOctNormal(N);
    output.Normal = float4(octNormal, input.Position.z, 0.0f);

    // --- Material (roughness, metallic, AO) ---
    float4 metallicSample = gMetallicTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);
    float4 roughnessSample = gRoughnessTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);
    float4 aoSample = gAoTexture.SampleGrad(gLinearSampler, uv, uvDdx, uvDdy);

    float roughness = gHasRoughnessMap
        ? (gHasPackedMaterialMap ? roughnessSample.r : roughnessSample.r) * gRoughnessFactor
        : gRoughnessFactor;

    float metallic = gHasMetallicMap
        ? (gHasPackedMaterialMap ? metallicSample.g : metallicSample.r) * gMetallicFactor
        : gMetallicFactor;

    float ao = gHasAoMap
        ? lerp(1.0f, gHasPackedMaterialMap ? aoSample.b : aoSample.r, gAoStrength)
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
