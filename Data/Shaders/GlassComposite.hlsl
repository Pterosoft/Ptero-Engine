// GlassComposite.hlsl
// Hybrid glass resolve:
//   1. Screen-space refraction against the scene depth buffer.
//   2. One stochastic Fresnel branch when SSR misses: reflection or refraction.
//   3. Inline ray-query fallback with FORCE_OPAQUE and optional thin-glass back-face culling.
//   4. Cheap dispersion by offsetting scene-color samples, never by firing extra rays.

cbuffer GlassConstants : register(b0)
{
    uint  gFrameWidth;
    uint  gFrameHeight;
    uint  gFrameIndex;
    uint  gTlasReady;
    float4x4 gInvViewProj;
    float4x4 gViewProj;
    float3 gCameraPos;
    float  gMaxRayDistance;
    float  gScreenTraceStride;
    float  gScreenTraceThickness;
    uint   gScreenTraceSteps;
    float  _Pad0;
};

Texture2D<float4> gSceneColor      : register(t0);
Texture2D<float4> gGBufferAlbedo   : register(t1);
Texture2D<float4> gGBufferNormal   : register(t2);
Texture2D<float4> gGBufferMaterial : register(t3);
Texture2D<float>  gDepthBuffer     : register(t4);
RaytracingAccelerationStructure gSceneTlas : register(t5);

struct GpuPackedVertex { float px, py, pz, nx, ny, nz, u, v; };
struct GpuInstanceInfo { uint vertexOffset, indexOffset, vertexCount, indexCount, materialRangeOffset, materialRangeCount, _pad0, _pad1; };
struct GpuMaterialRange
{
    uint  startPrimitive;
    uint  primitiveCount;
    float baseColorR;
    float baseColorG;
    float baseColorB;
    float baseColorA;
    float opacityFactor;
    float alphaCutoff;
    uint  baseColorTextureIndex;
    uint  opacityTextureIndex;
    uint  flags;
    float _pad0;
};

StructuredBuffer<GpuPackedVertex>  gVertices       : register(t6);
StructuredBuffer<uint>             gIndices        : register(t7);
StructuredBuffer<GpuInstanceInfo>  gInstanceInfo   : register(t8);
StructuredBuffer<GpuMaterialRange> gMaterialRanges : register(t9);
Texture2D<float4>                  gBaseTextures[32] : register(t10);

RWTexture2D<float4> gOutput : register(u0);
SamplerState gLinearClampSampler : register(s0);

static const uint INVALID_TEXTURE_INDEX = 0xffffffffu;
static const uint RT_MATERIAL_FLAG_GLASS = 4u;
static const float PI = 3.14159265f;

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 world = mul(ndc, gInvViewProj);
    return world.xyz / max(world.w, 1e-6f);
}

float3 SkyRadiance(float3 dir)
{
    float t = saturate(dir.z * 0.5f + 0.5f);
    return lerp(float3(0.02f, 0.025f, 0.035f), float3(0.45f, 0.58f, 0.75f), t);
}

float2 ProjectWorldToUv(float3 worldPos, out float depth)
{
    float4 clip = mul(float4(worldPos, 1.0f), gViewProj);
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    depth = ndc.z;
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Rand01(uint2 pixel, uint frame)
{
    uint seed = pixel.x * 1973u + pixel.y * 9277u + frame * 26699u + 0x9e3779b9u;
    return (Hash(seed) & 0x00ffffffu) / 16777215.0f;
}

float SchlickFresnel(float cosTheta, float ior)
{
    float f0 = (ior - 1.0f) / (ior + 1.0f);
    f0 *= f0;
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 SampleDispersedScene(float2 uv, float distanceScale, float dispersion)
{
    float2 offset = float2(0.0015f, -0.0008f) * dispersion * saturate(distanceScale * 0.04f);
    float r = gSceneColor.SampleLevel(gLinearClampSampler, uv + offset, 0.0f).r;
    float g = gSceneColor.SampleLevel(gLinearClampSampler, uv, 0.0f).g;
    float b = gSceneColor.SampleLevel(gLinearClampSampler, uv - offset, 0.0f).b;
    return float3(r, g, b);
}

bool ScreenSpaceRefraction(float3 worldPos, float3 rayDir, float surfaceDepth, float dispersion, out float3 color)
{
    float3 start = worldPos + rayDir * 0.04f;
    float travel = gScreenTraceStride;

    [loop]
    for (uint i = 0; i < gScreenTraceSteps; ++i)
    {
        float3 p = start + rayDir * travel;
        float projectedDepth = 0.0f;
        float2 uv = ProjectWorldToUv(p, projectedDepth);
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f || projectedDepth <= 0.0f || projectedDepth >= 1.0f)
            break;

        float sceneDepth = gDepthBuffer.SampleLevel(gLinearClampSampler, uv, 0.0f);
        if (sceneDepth < 1.0f && projectedDepth >= sceneDepth + gScreenTraceThickness && abs(sceneDepth - surfaceDepth) > 0.0005f)
        {
            color = SampleDispersedScene(uv, travel, dispersion);
            return true;
        }

        travel += gScreenTraceStride * (1.0f + (float)i * 0.18f);
    }

    color = 0.0f.xxx;
    return false;
}

GpuMaterialRange ResolveMaterialRange(GpuInstanceInfo info, uint primitiveIndex)
{
    [loop]
    for (uint i = 0; i < info.materialRangeCount; ++i)
    {
        GpuMaterialRange range = gMaterialRanges[info.materialRangeOffset + i];
        if (primitiveIndex >= range.startPrimitive && primitiveIndex < range.startPrimitive + range.primitiveCount)
            return range;
    }

    GpuMaterialRange fallback = (GpuMaterialRange)0;
    fallback.baseColorR = 1.0f;
    fallback.baseColorG = 1.0f;
    fallback.baseColorB = 1.0f;
    fallback.baseColorA = 1.0f;
    fallback.baseColorTextureIndex = INVALID_TEXTURE_INDEX;
    return fallback;
}

float4 SampleMaterialTexture(uint textureIndex, float2 uv, float4 fallback)
{
    if (textureIndex == INVALID_TEXTURE_INDEX)
        return fallback;

    return gBaseTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(gLinearClampSampler, uv, 0.0f);
}

float3 ShadeRayHitData(uint instIdx, uint primIdx, float2 bary, float3 rayDir, float hitDistance, float3x4 objectToWorld)
{
    GpuInstanceInfo info = gInstanceInfo[instIdx];
    uint i0 = gIndices[info.indexOffset + primIdx * 3 + 0];
    uint i1 = gIndices[info.indexOffset + primIdx * 3 + 1];
    uint i2 = gIndices[info.indexOffset + primIdx * 3 + 2];

    GpuPackedVertex v0 = gVertices[info.vertexOffset + i0];
    GpuPackedVertex v1 = gVertices[info.vertexOffset + i1];
    GpuPackedVertex v2 = gVertices[info.vertexOffset + i2];

    float b0 = 1.0f - bary.x - bary.y;
    float3 localN = normalize(b0 * float3(v0.nx, v0.ny, v0.nz)
        + bary.x * float3(v1.nx, v1.ny, v1.nz)
        + bary.y * float3(v2.nx, v2.ny, v2.nz));
    float2 uv = b0 * float2(v0.u, v0.v)
        + bary.x * float2(v1.u, v1.v)
        + bary.y * float2(v2.u, v2.v);

    float3x4 o2w = objectToWorld;
    float3x3 o2wRot = float3x3(
        float3(o2w[0][0], o2w[1][0], o2w[2][0]),
        float3(o2w[0][1], o2w[1][1], o2w[2][1]),
        float3(o2w[0][2], o2w[1][2], o2w[2][2]));
    float3 worldN = normalize(mul(o2wRot, localN));
    if (dot(worldN, -rayDir) < 0.0f)
        worldN = -worldN;

    GpuMaterialRange range = ResolveMaterialRange(info, primIdx);
    if ((range.flags & RT_MATERIAL_FLAG_GLASS) != 0)
        return SkyRadiance(rayDir);

    float4 fallback = float4(range.baseColorR, range.baseColorG, range.baseColorB, range.baseColorA);
    float3 albedo = SampleMaterialTexture(range.baseColorTextureIndex, uv, fallback).rgb;

    float sky = saturate(worldN.z * 0.5f + 0.5f);
    float facing = saturate(dot(worldN, normalize(float3(0.35f, 0.25f, 0.9f))));
    float3 ambient = albedo * lerp(0.18f.xxx, float3(0.45f, 0.54f, 0.72f), sky);
    float3 sun = albedo * float3(1.0f, 0.92f, 0.78f) * facing * 1.8f;
    return max(ambient + sun, albedo * 0.12f) * (1.0f / (1.0f + hitDistance * 0.00002f));
}

float3 TraceFallbackRay(float3 origin, float3 rayDir, bool thinGlass)
{
    if (gTlasReady == 0)
        return SkyRadiance(rayDir);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = rayDir;
    ray.TMin = 0.001f;
    ray.TMax = gMaxRayDistance;

    uint flags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
    if (thinGlass)
        flags |= RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(gSceneTlas, flags, 0xff, ray);
    while (rq.Proceed()) {}

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        return ShadeRayHitData(
            rq.CommittedInstanceIndex(),
            rq.CommittedPrimitiveIndex(),
            rq.CommittedTriangleBarycentrics(),
            rayDir,
            rq.CommittedRayT(),
            rq.CommittedObjectToWorld3x4());
    }

    return SkyRadiance(rayDir);
}

float3 EnvironmentReflection(float3 reflDir, float3 sceneColor)
{
    float3 sky = SkyRadiance(reflDir);
    float horizon = saturate(1.0f - abs(reflDir.z));
    return lerp(sky, sceneColor, horizon * 0.25f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gFrameWidth || pixel.y >= gFrameHeight)
        return;

    float2 uv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
    float4 sceneColor = gSceneColor.Load(int3(pixel, 0));
    float4 material = gGBufferMaterial.Load(int3(pixel, 0));

    if (material.a <= 0.001f)
    {
        gOutput[pixel] = sceneColor;
        return;
    }

    float4 normalSample = gGBufferNormal.Load(int3(pixel, 0));
    float depth = normalSample.z;
    if (depth <= 0.0f || depth >= 1.0f)
    {
        gOutput[pixel] = sceneColor;
        return;
    }

    float4 albedo = gGBufferAlbedo.Load(int3(pixel, 0));
    float3 normal = DecodeOctNormal(normalSample.xy);
    float3 worldPos = ReconstructWorldPosition(uv, depth);
    float3 V = normalize(gCameraPos - worldPos);
    float ior = 1.0f + material.a * 1.5f;
    float roughness = saturate(material.r);
    bool thinGlass = material.g > 0.5f;
    float dispersion = material.b * 2.0f;
    float glassOpacity = saturate(albedo.a);
    float thickness = max(glassOpacity, 0.015f);

    if (dot(normal, V) < 0.0f)
        normal = -normal;

    float cosTheta = saturate(dot(V, normal));
    float fresnel = SchlickFresnel(cosTheta, ior);
    fresnel = saturate(lerp(fresnel, 1.0f, roughness * 0.25f));
    if (thinGlass)
        fresnel = min(fresnel, 0.16f + pow(1.0f - cosTheta, 5.0f) * 0.22f);

    float3 refrDir = refract(-V, normal, 1.0f / max(ior, 1.001f));
    bool totalInternalReflection = dot(refrDir, refrDir) < 1e-5f;
    if (totalInternalReflection)
        fresnel = 1.0f;

    float3 transmission = sceneColor.rgb;
    if (!totalInternalReflection)
    {
        refrDir = normalize(refrDir);
        float2 refractOffset = refrDir.xy * (thinGlass ? 0.0035f : 0.009f) * (1.0f + thickness);
        transmission = SampleDispersedScene(saturate(uv + refractOffset), thickness * 4.0f, dispersion * (thinGlass ? 0.25f : 1.0f));
    }

    float3 reflDir = normalize(reflect(-V, normal));
    float3 reflection = EnvironmentReflection(reflDir, sceneColor.rgb);

    float tintStrength = thinGlass ? saturate(glassOpacity * 0.08f) : saturate(glassOpacity * 0.45f);
    float3 tint = lerp(1.0f.xxx, saturate(albedo.rgb), tintStrength);
    float edgeBoost = pow(1.0f - cosTheta, 3.0f);
    float thinScale = thinGlass ? 0.35f : 1.0f;
    float3 rim = edgeBoost * float3(0.7f, 0.85f, 1.0f) * 0.12f * thinScale;
    float causticHint = saturate(thickness * 1.5f) * saturate(dot(normal, float3(0.0f, 0.0f, 1.0f))) * 0.04f * thinScale;

    float reflectionWeight = fresnel * (thinGlass ? 0.12f : 0.35f) * glassOpacity;
    float3 resolved = lerp(transmission * tint, reflection, reflectionWeight) + rim + causticHint.xxx;
    resolved = lerp(resolved, sceneColor.rgb, saturate(roughness * 0.25f));
    float compositeWeight = glassOpacity * (thinGlass ? 0.35f : 0.65f);
    gOutput[pixel] = float4(lerp(sceneColor.rgb, max(resolved, 0.0f.xxx), compositeWeight), sceneColor.a);
}
