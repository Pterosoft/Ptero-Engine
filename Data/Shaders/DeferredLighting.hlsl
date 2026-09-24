// DeferredLighting.hlsl
// Fullscreen lighting-resolve pass for the deferred renderer.
// Reads the three G-Buffer textures (albedo, normal, material) plus the
// depth buffer, reconstructs world-space position, then evaluates the
// Hosek-Wilkie sun/sky directional light, PCF shadows, and all dynamic
// point lights to produce the final HDR colour.

// -------------------------------------------------------------------------
// Constant buffers
// -------------------------------------------------------------------------

// Per-frame camera and inverse-projection data used to reconstruct world
// position from the G-Buffer depth value.
cbuffer CameraConstants : register(b0)
{
    float4x4 gInvViewProj;  // inverse of viewProjection (row-major pre-transposed)
    float3   gCameraPos;    // world-space camera position
    float    _CamPad;
};

// Scene lighting – mirrors the layout in the former forward MeshEntity.hlsl.
#define MAX_POINT_LIGHTS 16

// The light record and the emitter-shape resolution are shared with the GI,
// probe and cascade passes so all of them agree on the layout and on where a
// spot's cone ends.
#include "LightShapes.hlsli"

// The probe SH packing and grid addressing are shared with the volumetric fog,
// which samples the same grid per froxel.
#include "RadianceProbeCommon.hlsli"

// The material reflectivity knob and the F0 it produces, shared with SSR and the
// ray-traced specular pass so all three agree on how reflective a surface is.
#include "SurfaceSpecular.hlsli"

// Subsurface scattering. When a subsurface material is on screen the resolve runs as a
// second pipeline variant that also writes the pixel's diffuse lighting to SV_Target1,
// which the subsurface pass scatters and swaps back in (SubsurfaceScattering.hlsl). In
// screen-space mode it also adds light transmitted through thin parts, with thickness
// measured from the shadow maps.
#ifndef PTERO_SSS_OUTPUT
#define PTERO_SSS_OUTPUT 0
#endif
#if PTERO_SSS_OUTPUT
#define PTERO_SSS_CB_REGISTER b4
#include "Subsurface.hlsli"
#endif

#define LIGHT_TYPE_RECT PTERO_LIGHT_TYPE_RECT

cbuffer SceneLighting : register(b1)
{
    float3 gSunDirection;   // world-space direction FROM sun TOWARD scene (normalised)
    float  _LightPad0;
    float3 gSunColor;
    float  _LightPad1;
    float3 gSkyAmbient;
    float  _LightPad2;
    int    gNumPointLights;
    float  gGiIntensity;    // 0 = GI disabled; >0 blends GI contribution
    float  gAoIntensity;    // 0 = RTAO disabled; >0 overrides G-Buffer AO
    int    gAoDebugView;    // 0 = normal lit output; >0 = standalone AO debug
    int    gVolumetricFogEnabled;
    float  gFogStartDistance;
    float  gFogMaxDistance;
    int    gFogDebugView;
    float  gSpecularIntensity; // 0 = specular reflections disabled; >0 blends ray-traced specular
    PteroLightData gPointLights[MAX_POINT_LIGHTS];
    int    gRtgiDebugView;
    int    gPointShadowDebugView;
    int    gPointShadowFilterRadius;
    float  gPointShadowSeamBlendDistance;
    float  gPointShadowNormalOffset;
};

// Shadow constants – same layout as ShadowData in the old MeshEntity.hlsl.
cbuffer ShadowData : register(b2)
{
    float4x4 gLightViewProj;
    float    gShadowMapSize;
    float    gShadowBias;
    float    gPointShadowMapSize;
    float    gPointShadowBias;
    float4x4 gPointShadowFaceViewProj[24];
};

cbuffer ProbeConstants : register(b3)
{
    uint   gProbeGridX;
    uint   gProbeGridY;
    uint   gProbeGridZ;
    float  gProbeSpacing;
    float3 gProbeOrigin;
    float  _ProbePad0;
};


// -------------------------------------------------------------------------
// Textures / samplers
// -------------------------------------------------------------------------
Texture2D    gGBufferAlbedo   : register(t0); // RT0: albedo (RGB) + unused (A)
Texture2D    gGBufferNormal   : register(t1); // RT1: oct normal (RG) + copied depth (B)
Texture2D    gGBufferMaterial : register(t2); // RT2: roughness/metallic/AO
Texture2D    gDepthBuffer     : register(t3); // scene depth, or resolved G-buffer depth when MSAA is active
Texture2D    gShadowMap       : register(t4); // sun shadow map
Texture2D    gGiAccumulation  : register(t5); // DXR GI accumulation buffer (RG11B10 HDR)
Texture2D    gRtaoTexture     : register(t6); // DXR ambient occlusion (R16F)
Texture3D    gVolumetricFog   : register(t7); // accumulated froxel fog (rgb=scattering, a=transmittance)
Texture2D    gSpecularReflect : register(t8); // DXR specular reflections (RGBA16F)
Texture2DArray gPointShadowMaps : register(t9);
StructuredBuffer<ProbeSH> gRadianceProbes : register(t10);

SamplerState             gPointSampler   : register(s0); // point-clamp for G-buffer reads
SamplerComparisonState   gShadowSampler  : register(s1); // PCF comparison sampler
SamplerState             gLinearSampler  : register(s2); // linear-clamp for volumetric fog filtering

// -------------------------------------------------------------------------
// Vertex / pixel structs
// -------------------------------------------------------------------------
struct VSInput
{
    uint VertexId : SV_VertexID;
};

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

// -------------------------------------------------------------------------
// Fullscreen triangle vertex shader
// Generates a single triangle that covers the entire viewport from three
// vertex IDs without needing a vertex buffer.
// -------------------------------------------------------------------------
PSInput VSMain(VSInput input)
{
    PSInput output;
    // Correct fullscreen triangle: NDC Y=+1 is top, UV Y=0 is top – must match.
    // Vertex 0 (id=0): NDC=(-1, 1), UV=(0,0) – top-left
    // Vertex 1 (id=1): NDC=(-1,-3), UV=(0,2) – off-screen bottom
    // Vertex 2 (id=2): NDC=( 3, 1), UV=(2,0) – off-screen right
    float2 ndc;
    ndc.x = (input.VertexId == 2) ?  3.0f : -1.0f;
    ndc.y = (input.VertexId == 1) ? -3.0f :  1.0f; // Y=+1 at top so UV and G-buffer align
    output.Position = float4(ndc, 0.0f, 1.0f);
    output.TexCoord.x = (input.VertexId == 2) ? 2.0f : 0.0f;
    output.TexCoord.y = (input.VertexId == 1) ? 2.0f : 0.0f;
    return output;
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

// Reconstruct world-space position from a depth buffer sample.
// gInvViewProj is stored as Transpose(inv(viewProj)) so HLSL mul(row, M) works correctly.
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    // UV (0,0) = top-left matches NDC (+x right, +y up).
    // u in [0,1] -> NDC x in [-1,+1]; v in [0,1] -> NDC y in [+1,-1] (flip Y)
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    }
    return normalize(n);
}

float3 ComputeFogUv(float2 uv, float viewDistance)
{
    float nearZ = max(gFogStartDistance, 0.001f);
    float farZ = max(gFogMaxDistance, nearZ + 0.001f);
    float fogZ = saturate(log(max(viewDistance, nearZ) / nearZ) / log(farZ / nearZ));
    return float3(uv, fogZ);
}

float4 SanitizeFogSample(float4 fogSample)
{
    if (any(isnan(fogSample)) || any(isinf(fogSample)))
    {
        fogSample = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    fogSample.rgb = max(fogSample.rgb, 0.0f.xxx);
    fogSample.a = saturate(fogSample.a);
    if (fogSample.a < 0.001f && dot(fogSample.rgb, fogSample.rgb) < 1e-8f)
    {
        fogSample.a = 1.0f;
    }

    return fogSample;
}

float4 CompositeFogOverPreservedTarget(float4 fogSample)
{
    fogSample = SanitizeFogSample(fogSample);

    if (gFogDebugView == 1)
    {
        return float4(fogSample.rgb, 1.0f);
    }
    if (gFogDebugView == 2)
    {
        return float4(fogSample.aaa, 1.0f);
    }

    // The lighting pass alpha-blends over the sky that was already rendered
    // into the scene target. Return colour premultiplied back to straight
    // alpha so blending produces: fogScattering + sky * transmittance.
    const float fogOpacity = saturate(1.0f - fogSample.a);
    if (fogOpacity <= 0.0001f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    return float4(fogSample.rgb / fogOpacity, fogOpacity);
}

// 3x3 PCF shadow factor; 1 = fully lit, 0 = fully shadowed.
float SampleShadowPCF(float3 worldPos)
{
    float4 lightClip  = mul(float4(worldPos, 1.0f), gLightViewProj);
    float3 projCoords = lightClip.xyz / lightClip.w;

    float2 uv;
    uv.x =  projCoords.x * 0.5f + 0.5f;
    uv.y = -projCoords.y * 0.5f + 0.5f;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ||
        projCoords.z < 0.0f || projCoords.z > 1.0f)
        return 1.0f;

    const float depth     = projCoords.z - gShadowBias;
    const float texelSize = 1.0f / gShadowMapSize;

    float shadow = 0.0f;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + offset, depth);
        }
    }
    return shadow / 9.0f;
}

float SamplePointShadow(int lightIndex, float3 worldPos, float3 surfaceNormal)
{
    if (lightIndex < 0 || lightIndex >= MAX_POINT_LIGHTS)
        return 1.0f;
    if (gPointLights[lightIndex].CastShadows < 0.5f)
        return 1.0f;

    int shadowIndex = (int)gPointLights[lightIndex].ShadowIndex;
    if (shadowIndex < 0)
        return 1.0f;

    float3 toPoint = worldPos - gPointLights[lightIndex].Position;
    float distanceToLight = length(toPoint);
    if (distanceToLight <= 0.0001f)
        return 1.0f;

    float3 sampleDir = toPoint / distanceToLight;

    const float mapSize = max(gPointShadowMapSize, 1.0f);
    const float texelSize = 1.0f / mapSize;
    const float normalOffsetDistance = max(gPointShadowNormalOffset, 0.0f) * texelSize * max(gPointLights[lightIndex].Radius, 1e-4f);
    float3 offsetWorldPos = worldPos + normalize(surfaceNormal) * normalOffsetDistance;

    toPoint = offsetWorldPos - gPointLights[lightIndex].Position;
    distanceToLight = length(toPoint);
    if (distanceToLight <= 0.0001f)
        return 1.0f;
    sampleDir = toPoint / distanceToLight;

    int faceIndex = -1;
    int secondFaceIndex = -1;
    float2 uv = 0.0f.xx;
    float2 secondUv = 0.0f.xx;
    float bestEdgeDistance = -1.0f;
    float secondEdgeDistance = -1.0f;
    const int faceBaseIndex = shadowIndex * 6;
    [unroll]
    for (int candidateFace = 0; candidateFace < 6; ++candidateFace)
    {
        float4 clipPos = mul(float4(offsetWorldPos, 1.0f), gPointShadowFaceViewProj[faceBaseIndex + candidateFace]);
        if (abs(clipPos.w) <= 1e-5f)
            continue;

        float3 ndc = clipPos.xyz / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
            continue;

        float2 candidateUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (all(candidateUv >= 0.0f.xx) && all(candidateUv <= 1.0f.xx))
        {
            float2 edgeDistance2D = min(candidateUv, 1.0f.xx - candidateUv);
            float edgeDistance = min(edgeDistance2D.x, edgeDistance2D.y);
            if (edgeDistance > bestEdgeDistance)
            {
                secondFaceIndex = faceIndex;
                secondUv = uv;
                secondEdgeDistance = bestEdgeDistance;
                faceIndex = candidateFace;
                uv = candidateUv;
                bestEdgeDistance = edgeDistance;
            }
            else if (edgeDistance > secondEdgeDistance)
            {
                secondFaceIndex = candidateFace;
                secondUv = candidateUv;
                secondEdgeDistance = edgeDistance;
            }
        }
    }

    if (faceIndex < 0)
        return 1.0f;

    const float lightRadius = max(gPointLights[lightIndex].Radius, 1e-4f);
    float currentDepth = distanceToLight / lightRadius;
    const float3 lightDir = -sampleDir;
    const float normalAlignment = saturate(dot(normalize(surfaceNormal), normalize(lightDir)));
    const float slopeBias = (1.0f - normalAlignment) * texelSize * 2.0f;
    const float depthBias = (gPointShadowBias / lightRadius) + slopeBias;

    const int filterRadius = clamp(gPointShadowFilterRadius, 0, 4);

    float primaryVisibility = 0.0f;
    float primaryWeight = 0.0f;
    [loop]
    for (int y = -filterRadius; y <= filterRadius; ++y)
    {
        [loop]
        for (int x = -filterRadius; x <= filterRadius; ++x)
        {
            // Hardware comparison sampling: each tap is a bilinear-filtered depth test,
            // so the edge moves continuously with the surface. Loading whole texels
            // snapped every tap to the same grid, which left a texel-sized staircase no
            // matter how wide the filter was - very visible under a large rect light.
            float2 sampleUv = saturate(uv + float2(x, y) * texelSize);
            primaryVisibility += gPointShadowMaps.SampleCmpLevelZero(
                gShadowSampler, float3(sampleUv, shadowIndex * 6 + faceIndex), currentDepth - depthBias);
            primaryWeight += 1.0f;
        }
    }
    primaryVisibility /= max(primaryWeight, 1.0f);

    if (secondFaceIndex < 0 || gPointShadowSeamBlendDistance <= 0.0f)
        return primaryVisibility;

    float edgeDistance = min(uv.x, min(uv.y, min(1.0f - uv.x, 1.0f - uv.y)));
    float seamBlend = saturate(1.0f - (edgeDistance / gPointShadowSeamBlendDistance));
    if (seamBlend <= 0.0f)
        return primaryVisibility;

    float secondaryVisibility = 0.0f;
    float secondaryWeight = 0.0f;
    [loop]
    for (int y = -filterRadius; y <= filterRadius; ++y)
    {
        [loop]
        for (int x = -filterRadius; x <= filterRadius; ++x)
        {
            float2 sampleUv = saturate(secondUv + float2(x, y) * texelSize);
            secondaryVisibility += gPointShadowMaps.SampleCmpLevelZero(
                gShadowSampler, float3(sampleUv, shadowIndex * 6 + secondFaceIndex), currentDepth - depthBias);
            secondaryWeight += 1.0f;
        }
    }
    secondaryVisibility /= max(secondaryWeight, 1.0f);

    return lerp(primaryVisibility, secondaryVisibility, seamBlend);
}

float3 SampleRadianceProbeIrradiance(float3 worldPos, float3 normal)
{
    const uint3 gridSize = uint3(gProbeGridX, gProbeGridY, gProbeGridZ);
    if (!PteroProbeGridValid(gridSize, gProbeSpacing))
        return float3(0.0f, 0.0f, 0.0f);

    const PteroProbeGridTap tap = PteroProbeGridLookup(gridSize, gProbeOrigin, gProbeSpacing, worldPos);

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        irradiance += tap.Weight[corner] * PteroEvaluateProbeSH(gRadianceProbes[tap.Index[corner]], normal);
    }
    return irradiance;
}

// -------------------------------------------------------------------------
// Cook-Torrance BRDF
// Evaluates diffuse + specular for a single light direction L_in.
// lightColor  – pre-weighted colour (light colour × NdotL × shadow / falloff)
//               NOTE: NdotL is NOT pre-applied here; we compute it inside.
// V           – unit view direction (surface → camera)
// N           – unit surface normal
// albedo      – base colour
// metallic, roughness – PBR material parameters
// specularLevel – material reflectivity knob; 0.5 is neutral (see SurfaceSpecular.hlsli).
//                Named apart from the Cook-Torrance `specular` term computed below.
// -------------------------------------------------------------------------
// diffuseAccum – receives the diffuse half of the result on its own, which is the part
//                subsurface scattering redistributes.
float3 EvalBRDF(float3 L_in, float3 lightRadiance,
                float3 V, float3 N,
                float3 albedo, float metallic, float roughness, float specularLevel,
                inout float3 diffuseAccum)
{
    float NdotL = saturate(dot(N, L_in));
    if (NdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float3 H     = normalize(V + L_in);
    float  NdotV = max(dot(N, V), 0.0001f);
    float  NdotH = saturate(dot(N, H));
    float  HdotV = saturate(dot(H, V));

    // GGX NDF
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    float D = a2 / (3.14159265f * denom * denom);

    // Smith-GGX geometry (k remapped for direct lighting)
    float k   = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G_V = NdotV / (NdotV * (1.0f - k) + k);
    float G_L = NdotL / (NdotL * (1.0f - k) + k);
    float G   = G_V * G_L;

    // Schlick Fresnel
    float3 F0 = PteroComputeF0(albedo, metallic, specularLevel);
    float3 F  = F0 + (1.0f - F0) * pow(1.0f - HdotV, 5.0f);

    // Specular term (Cook-Torrance)
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);

    // Lambertian diffuse; metals have no diffuse.
    // No 1/pi division: real-time convention keeps albedo in [0,1] as the diffuse colour
    // without the physics-correct pi normalisation that would darken surfaces ~3x.
    float3 kD = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);
    float3 diffuseTerm = kD * albedo;

    diffuseAccum += diffuseTerm * lightRadiance * NdotL;
    return (diffuseTerm + specular) * lightRadiance * NdotL;
}

#if PTERO_SSS_OUTPUT
// How much of the surface lies between this point and the sun, in metres, read from the
// sun shadow map - the SDK's SSSSTransmittance thickness estimate. The shadow map stores
// the depth of the first surface the sun reaches; the gap between that and this point
// (pushed slightly inside) is the path the light took through the object. Anything else
// standing in front of the sun makes the gap large, which correctly kills transmission.
// Returns -1 where the shadow map has nothing to say.
float SunTransmissionThickness(float3 worldPos, float3 N)
{
    const float3 shrunkPos = worldPos - 0.005f * N;
    const float4 lightClip = mul(float4(shrunkPos, 1.0f), gLightViewProj);
    const float3 projCoords = lightClip.xyz / lightClip.w;
    const float2 uv = float2(projCoords.x * 0.5f + 0.5f, -projCoords.y * 0.5f + 0.5f);
    if (any(uv < 0.0f.xx) || any(uv > 1.0f.xx) || projCoords.z < 0.0f || projCoords.z > 1.0f)
        return -1.0f;

    const float storedDepth = gShadowMap.SampleLevel(gPointSampler, uv, 0.0f).r;
    // The sun projection is orthographic, so light-space depth is linear in world
    // distance along the sun direction; the length of its z row is that scale.
    const float depthPerMetre = length(float3(gLightViewProj._13, gLightViewProj._23, gLightViewProj._33));
    return max(projCoords.z - storedDepth, 0.0f) / max(depthPerMetre, 1e-8f);
}

// Same estimate for a shadow-casting point, spot or rect light, from its cube shadow
// map, which stores linear distance / radius. Returns -1 when the light has no shadow map.
float PointTransmissionThickness(int lightIndex, float3 worldPos, float3 N)
{
    if (gPointLights[lightIndex].CastShadows < 0.5f)
        return -1.0f;
    const int shadowIndex = (int)gPointLights[lightIndex].ShadowIndex;
    if (shadowIndex < 0)
        return -1.0f;

    const float3 shrunkPos = worldPos - 0.005f * N;
    const float distanceToLight = length(shrunkPos - gPointLights[lightIndex].Position);
    const float mapSize = max(gPointShadowMapSize, 1.0f);

    [unroll]
    for (int face = 0; face < 6; ++face)
    {
        const float4 clipPos = mul(float4(shrunkPos, 1.0f), gPointShadowFaceViewProj[shadowIndex * 6 + face]);
        if (abs(clipPos.w) <= 1e-5f)
            continue;
        const float3 ndc = clipPos.xyz / clipPos.w;
        const float2 faceUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (ndc.z < 0.0f || ndc.z > 1.0f || any(faceUv < 0.0f.xx) || any(faceUv > 1.0f.xx))
            continue;

        const int2 texel = int2(faceUv * (mapSize - 1.0f));
        const float storedDistance = gPointShadowMaps.Load(int4(texel, shadowIndex * 6 + face, 0)).r
                                   * max(gPointLights[lightIndex].Radius, 1e-4f);
        return max(distanceToLight - storedDistance, 0.0f);
    }
    return -1.0f;
}
#endif

// -------------------------------------------------------------------------
// Pixel shader
// -------------------------------------------------------------------------
// sssDiffuse – for a subsurface pixel, receives the diffuse lighting the subsurface pass
//              should scatter (rgb) and 1 in alpha; left at zero everywhere else.
float4 ResolveLighting(PSInput input, inout float4 sssDiffuse)
{
    float2 uv = input.TexCoord;
    uint2 pixel = uint2(input.Position.xy);

    // Read G-Buffer.
    float4 albedoSample   = gGBufferAlbedo.Load(int3(pixel, 0));
    float4 normalSample   = gGBufferNormal.Load(int3(pixel, 0));
    float4 materialSample = gGBufferMaterial.Load(int3(pixel, 0));
    float  rawDepth       = gDepthBuffer.Load(int3(pixel, 0)).r;

    // Sky / skybox pixels have depth == 1.0 after clear. The sky pass already
    // wrote colour into the scene target, so only blend volumetric fog over it.
    [branch]
    if (rawDepth >= 1.0f)
    {
        if (gVolumetricFogEnabled != 0)
        {
            float3 fogUv = ComputeFogUv(uv, gFogMaxDistance);
            return CompositeFogOverPreservedTarget(gVolumetricFog.SampleLevel(gLinearSampler, fogUv, 0.0f));
        }

        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Decode G-Buffer values.
    float3 albedo    = albedoSample.rgb;
    float3 N         = DecodeOctNormal(normalSample.xy);

    // Glass materials repack the material G-buffer:
    //   R = roughness, G = thin-glass flag, B = dispersion, A = encoded IOR.
    // Shade them as transparent dielectric surfaces instead of opaque PBR.
    const bool isGlassMaterial = materialSample.a > (1.0f / 255.0f) && materialSample.b < 0.75f;
    [branch]
    if (isGlassMaterial)
    {
        float glassRoughness = max(materialSample.r, 0.02f);
        float thinGlass = materialSample.g > 0.5f ? 1.0f : 0.0f;
        float glassOpacity = saturate(albedoSample.a);
        float glassAlpha = glassOpacity;

        float3 worldPos = ReconstructWorldPosition(uv, rawDepth);
        float3 V = normalize(gCameraPos - worldPos);
        float facing = saturate(dot(N, V));
        float fresnel = 0.04f + 0.96f * pow(1.0f - facing, 5.0f);

        float shadowFactor = SampleShadowPCF(worldPos);
        float3 L_sun = normalize(-gSunDirection);
        float sunFacing = saturate(dot(N, L_sun));
        float3 glassTint = lerp(float3(0.96f, 0.99f, 1.0f), saturate(albedo), glassOpacity * 0.18f);
        float3 glassLit = glassTint * gSunColor * sunFacing * shadowFactor * (0.12f + fresnel * 0.55f);
        glassLit += glassTint * gSkyAmbient * (0.10f + fresnel * 0.22f);

        [unroll]
        for (int i = 0; i < MAX_POINT_LIGHTS; ++i)
        {
            float active = (i < gNumPointLights) ? 1.0f : 0.0f;
            PteroResolvedLight glassShape = PteroResolveLightShape(gPointLights[i], worldPos);
            float3 toLight = glassShape.Position - worldPos;
            float distSq = dot(toLight, toLight);
            float dist = sqrt(max(distSq, 1e-6f));
            float3 L_pt = toLight / dist;
            float normalizedDistance = saturate(dist / max(gPointLights[i].Radius, 1e-4f));
            float rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
            rangeMask *= rangeMask;
            float falloff = rangeMask
                          * pow(PteroLightFalloffDistance(gPointLights[i], dist),
                                -max(gPointLights[i].FalloffExponent, 0.001f))
                          * glassShape.ShapeMask;
            float pointFacing = saturate(dot(N, L_pt));
            float3 H = normalize(V + L_pt);
            float sparkle = pow(saturate(dot(N, H)), lerp(96.0f, 24.0f, glassRoughness));
            glassLit += gPointLights[i].Color * falloff * active * (pointFacing * 0.05f + sparkle * (0.25f + fresnel));
        }

        glassLit *= lerp(0.35f, 1.0f, glassOpacity);
        return float4(max(glassLit, 0.0f.xxx), saturate(glassAlpha));
    }

    float  roughness = max(materialSample.r, 0.04f); // G-buffer R
    float  metallic  = materialSample.g;             // G-buffer G
    // Reflectivity rides in the normal target's W channel; 0 means the pass that wrote
    // this pixel does not carry the channel, which decodes to the neutral default.
    float  specular  = PteroDecodeSurfaceSpecular(normalSample.w);
    // Override the baked G-Buffer AO with the ray-traced AO when available.
    float  ao        = materialSample.b;             // G-buffer B
    float  rtao      = ao;
    if (gAoIntensity > 0.0f)
    {
        rtao = gRtaoTexture.Load(int3(pixel, 0)).r;
        ao = lerp(ao, rtao, gAoIntensity);
    }

    if (gAoDebugView > 0)
    {
        float debugAo = (gAoIntensity > 0.0f) ? rtao : ao;
        return float4(debugAo.xxx, 1.0f);
    }

    // Reconstruct world-space position and view direction.
    float3 worldPos = ReconstructWorldPosition(uv, rawDepth);
    float3 V        = normalize(gCameraPos - worldPos);

    // ---- Sun directional light with PCF shadow ----
    // Diffuse light gathered separately for subsurface scattering: the direct part (sun
    // and scene lights, through EvalBRDF) and the indirect part (ambient, GI, shadow-map
    // transmission) apart, because the ray-traced mode replaces only the direct part.
    float3 diffuseLit      = float3(0.0f, 0.0f, 0.0f);
    float3 indirectDiffuse = float3(0.0f, 0.0f, 0.0f);
    float  fogTransmittance = 1.0f;

#if PTERO_SSS_OUTPUT
    const uint sssSlot = PteroDecodeSubsurfaceSlot(normalSample.w);
    const bool sssPixel = PteroSssSlotActive(sssSlot);
    // Screen-space mode measures transmission here from the shadow maps; the ray-traced
    // mode measures it through the mesh itself in its own pass.
    const bool sssShadowMapTransmission = sssPixel && gSssMode == 0u && gSssTransmission != 0u
        && gSssProfiles[sssSlot].FalloffTranslucency.a > 0.0f;
    float3 sssTransmitted = float3(0.0f, 0.0f, 0.0f);
#endif

    float  shadowFactor = SampleShadowPCF(worldPos);
    float3 L_sun        = normalize(-gSunDirection);
    float3 sunContrib   = EvalBRDF(L_sun, gSunColor * shadowFactor, V, N, albedo, metallic, roughness, specular, diffuseLit);

#if PTERO_SSS_OUTPUT
    [branch]
    if (sssShadowMapTransmission)
    {
        const float sunThickness = SunTransmissionThickness(worldPos, N);
        if (sunThickness >= 0.0f)
        {
            sssTransmitted += PteroSssTransmission(gSssProfiles[sssSlot], sunThickness, N, L_sun,
                                                   gSunColor, albedo * (1.0f - metallic));
        }
    }
#endif

    // ---- Sky ambient (hemisphere diffuse + rough-specular approximation) ----
    // Only upward-facing surfaces see the sky hemisphere.
    // We use the PCF shadow factor as a sky-visibility proxy: if the sun cannot
    // reach a surface, neither can open sky (good approximation for enclosed scenes).
    // A small minimum (0.05) provides a subtle fill so fully-shadowed areas are not
    // pitch-black even without GI enabled.
    float  skyWeight   = max(N.z, 0.0f);
    float  skyVis      = saturate(shadowFactor * 0.95f + 0.05f); // 5% in full shadow, 100% in sun
    float3 F0          = PteroComputeF0(albedo, metallic, specular);
    float3 ambientDiff = gSkyAmbient * skyWeight * skyVis * (1.0f - metallic) * albedo;
    float3 F_amb       = F0 + (max(float3(1,1,1) * (1.0f - roughness), F0) - F0)
                             * pow(1.0f - saturate(dot(N, V)), 5.0f);
    // The environment term is gathered along the *reflected* direction, not the normal.
    // Weighting it by N.z the way the diffuse hemisphere is weighted gives a vertical
    // surface no environment specular whatsoever, which is the reason a metal wall - which
    // has no diffuse term to fall back on - used to resolve to black. A rough surface
    // gathers over a wide lobe, so its weight is pulled back toward the hemisphere average.
    float3 R_env       = reflect(-V, N);
    float  envWeight   = lerp(saturate(R_env.z * 0.5f + 0.5f), 0.5f, roughness);
    float3 ambientSpec = gSkyAmbient * envWeight * skyVis * F_amb * (1.0f / (roughness * roughness + 1.0f));
    float3 ambient     = (ambientDiff + ambientSpec) * ao;
    indirectDiffuse   += ambientDiff * ao;

    // ---- Point lights ----
    float3 pointSum = float3(0.0f, 0.0f, 0.0f);
    float pointShadowDebug = 1.0f;
    [unroll]
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i)
    {
        float active = (i < gNumPointLights) ? 1.0f : 0.0f;

        PteroResolvedLight shape = PteroResolveLightShape(gPointLights[i], worldPos);
        float3 lightPos = shape.Position;

        // Spherical source: pull the sample point toward the surface by the
        // source radius so a large bulb lights like a ball rather than a
        // pinpoint. A rect light already has a real area, so it skips this.
        if (gPointLights[i].SourceRadius > 0.0001f &&
            (int)gPointLights[i].LightType != LIGHT_TYPE_RECT)
        {
            float3 toCenter = worldPos - lightPos;
            float  toCenterLen = length(toCenter);
            if (toCenterLen > 0.0001f)
            {
                lightPos += (toCenter / toCenterLen) * min(gPointLights[i].SourceRadius, gPointLights[i].Radius * 0.5f);
            }
        }

        float3 toLight = lightPos - worldPos;
        float  distSq  = dot(toLight, toLight);
        float  dist    = sqrt(max(distSq, 1e-6f));
        float3 L_pt    = toLight / dist;

        float  normalizedDistance = saturate(dist / max(gPointLights[i].Radius, 1e-4f));
        float  rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
        rangeMask *= rangeMask;
        float  falloffExponent = max(gPointLights[i].FalloffExponent, 0.001f);
        // Not max(dist, 1e-3): an area emitter has no singularity to guard
        // against, it has a size to stop inside of. See LightShapes.hlsli.
        float  distanceFalloff = pow(PteroLightFalloffDistance(gPointLights[i], dist), -falloffExponent);
        float  falloff = rangeMask * distanceFalloff * shape.ShapeMask;

        float pointShadow = SamplePointShadow(i, worldPos, N);
        pointShadowDebug = min(pointShadowDebug, pointShadow);
        pointSum += EvalBRDF(L_pt, gPointLights[i].Color * falloff * active * pointShadow, V, N, albedo, metallic, roughness, specular, diffuseLit);

#if PTERO_SSS_OUTPUT
        [branch]
        if (sssShadowMapTransmission && active > 0.0f && falloff > 0.0f)
        {
            const float pointThickness = PointTransmissionThickness(i, worldPos, N);
            if (pointThickness >= 0.0f)
            {
                sssTransmitted += PteroSssTransmission(gSssProfiles[sssSlot], pointThickness, N, L_pt,
                                                       gPointLights[i].Color * falloff, albedo * (1.0f - metallic));
            }
        }
#endif
    }

    if (gPointShadowDebugView > 0)
    {
        return float4(pointShadowDebug.xxx, 1.0f);
    }

    // ---- Final composite ----
    float3 lit = sunContrib + ambient + pointSum;

#if PTERO_SSS_OUTPUT
    // Transmitted light exits as diffuse light, so it is scattered along with the rest.
    lit        += sssTransmitted;
    indirectDiffuse += sssTransmitted;
#endif

    // Add DXR global illumination.
    // The RTGI / NRD path now stores demodulated diffuse irradiance so the denoiser
    // does not blur texture detail; remodulate by the visible surface albedo here.
    if (gGiIntensity > 0.0f)
    {
        float3 giDiffuseIrradiance = float3(0.0f, 0.0f, 0.0f);

        // Diagnostics live in the RTGI accumulation texture, so they must read
        // from there even when radiance probes would otherwise supply the
        // irradiance - sampling the probe grid instead would silently discard
        // the diagnostic and show ordinary indirect light.
        const bool diagnosticView = gRtgiDebugView >= 10;

        if (!diagnosticView && gProbeGridX > 0 && gProbeGridY > 0 && gProbeGridZ > 0)
        {
            giDiffuseIrradiance = SampleRadianceProbeIrradiance(worldPos, N);
            float probeLuma = dot(giDiffuseIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
            giDiffuseIrradiance = lerp(probeLuma.xxx, giDiffuseIrradiance, 1.35f) * 2.0f;
        }
        else
            giDiffuseIrradiance = gGiAccumulation.Load(int3(pixel, 0)).rgb;

        // Guard against NaN/Inf from the GI texture corrupting the final colour.
        if (!any(isnan(giDiffuseIrradiance)) && !any(isinf(giDiffuseIrradiance)))
        {
            // Diagnostic views (>= 10) carry a raw quantity, not radiance, so
            // they must replace the image rather than modulate it. Added to the
            // lit result and multiplied by albedo below, a greyscale diagnostic
            // is swamped by direct sun and firelight and reads as an ordinary
            // scene - which is exactly how the depth view looked unreadable.
            if (gRtgiDebugView == 15)
            {
                // NEE sun visibility (RtGI_RayGen writes green = reaches the sun, red =
                // blocked, black = faces away) checked against this pass's shadow map:
                //   green  both agree the sun reaches it
                //   red    shadow map says lit, the RT shadow ray is blocked - the RT scene
                //          holds a blocker the raster scene does not
                //   blue   RT reaches the sun, the shadow map says shadowed
                //   grey   both agree it is shadowed
                //   black  faces away from the sun
                if (dot(N, L_sun) <= 0.0f)
                    return float4(0.0f, 0.0f, 0.0f, 1.0f);
                const bool rtLit = giDiffuseIrradiance.g > giDiffuseIrradiance.r;
                const bool rasterLit = shadowFactor > 0.5f;
                if (rtLit && rasterLit)  return float4(0.0f, 1.0f, 0.0f, 1.0f);
                if (!rtLit && rasterLit) return float4(1.0f, 0.0f, 0.0f, 1.0f);
                if (rtLit && !rasterLit) return float4(0.0f, 0.2f, 1.0f, 1.0f);
                return float4(0.08f, 0.08f, 0.08f, 1.0f);
            }
            if (gRtgiDebugView >= 10 || gRtgiDebugView == 1)
                return float4(max(giDiffuseIrradiance, 0.0f.xxx), 1.0f);
            if (gRtgiDebugView == 2)
            {
                const float giLuma = dot(max(giDiffuseIrradiance, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
                return float4(giLuma.xxx, 1.0f);
            }
            const float3 giDiffuse = giDiffuseIrradiance * albedo * (1.0f - metallic) * gGiIntensity * ao;
            lit        += giDiffuse;
            indirectDiffuse += giDiffuse;
        }
    }

    // Add DXR specular reflections.
    // The specular buffer stores pre-weighted GGX reflected radiance.
    if (gSpecularIntensity > 0.0f)
    {
        float3 specRefl = gSpecularReflect.Load(int3(pixel, 0)).rgb;
        if (!any(isnan(specRefl)) && !any(isinf(specRefl)))
        {
            if (gRtgiDebugView == 3)
                return float4(specRefl, 1.0f);
            lit += specRefl * gSpecularIntensity;
        }
    }

    if (gVolumetricFogEnabled != 0)
    {
        float viewDistance = distance(gCameraPos, worldPos);
        float fogViewDistance = max(viewDistance - 0.05f, gFogStartDistance);
        float3 fogUv = ComputeFogUv(uv, fogViewDistance);
        float4 fogSample = SanitizeFogSample(gVolumetricFog.SampleLevel(gLinearSampler, fogUv, 0.0f));

        if (gFogDebugView == 1)
        {
            return float4(fogSample.rgb, 1.0f);
        }
        if (gFogDebugView == 2)
        {
            return float4(fogSample.aaa, 1.0f);
        }

        lit = (lit * fogSample.a) + fogSample.rgb;
        // The scene target holds the fogged value, so the part the subsurface pass
        // swaps out has to be the fogged diffuse as well.
        fogTransmittance = fogSample.a;
    }

#if PTERO_SSS_OUTPUT
    // Only opaque subsurface pixels are scattered: a blended surface's diffuse is mixed
    // with whatever is behind it in the scene target, so it cannot be swapped out.
    if (sssPixel && albedoSample.a >= 0.999f)
    {
        // Screen space blurs all of the diffuse light. The ray-traced mode re-evaluates
        // the direct light in world space and scatters that, so it is handed the direct
        // part alone and leaves the indirect part where the lighting pass put it. Alpha
        // marks the pixel (>= 0.5) and carries the fog transmittance the ray-traced result
        // has to be attenuated by: a = 0.5 + 0.5 * transmittance.
        const float3 sssSignal = (gSssMode == 1u) ? diffuseLit : (diffuseLit + indirectDiffuse);
        sssDiffuse = float4(max(sssSignal * fogTransmittance, 0.0f.xxx), 0.5f + 0.5f * saturate(fogTransmittance));
    }
#endif

    // The lighting PSO uses standard source-alpha blending over the sky/scene
    // target. Opaque materials store 1 here; transparent materials carry their
    // combined scalar/texture opacity in the albedo G-buffer alpha channel.
    return float4(lit, saturate(albedoSample.a));
}

struct PSOutput
{
    float4 Color      : SV_Target0;
#if PTERO_SSS_OUTPUT
    float4 SssDiffuse : SV_Target1;
#endif
};

PSOutput PSMain(PSInput input)
{
    float4 sssDiffuse = float4(0.0f, 0.0f, 0.0f, 0.0f);
    PSOutput output;
    output.Color = ResolveLighting(input, sssDiffuse);
#if PTERO_SSS_OUTPUT
    output.SssDiffuse = sssDiffuse;
#endif
    return output;
}
