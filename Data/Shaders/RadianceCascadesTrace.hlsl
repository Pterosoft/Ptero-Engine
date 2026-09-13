#if __INTELLISENSE__
#define RayQuery(x) int
#define RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH 0
#define RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES 0
#define COMMITTED_TRIANGLE_HIT 1
#endif

#define MAX_RC_POINT_LIGHTS 16

// The light record and the emitter-shape resolution are shared with the
// deferred, GI and probe passes so all of them agree on the layout and on
// where a spot cone ends.
#include "LightShapes.hlsli"

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

struct SparseProbeEntry
{
    int CellX;
    int CellY;
    int CellZ;
    uint State;
    uint PackedRadianceR;
    uint PackedRadianceG;
    uint PackedRadianceB;
    uint SampleCount;
};

cbuffer RadianceCascadesConstants : register(b0)
{
    uint  gFrameWidth;
    uint  gFrameHeight;
    uint  gFrameIndex;
    uint  gCascadeCount;

    uint  gProbeSpacingBase;
    uint  gRaysPerProbe;
    int   gDebugView;
    float gRayLengthBase;

    float gRayLengthScale;
    float gIntervalLengthScale;
    float gHysteresis;
    float gGiIntensity;

    float4x4 gViewProjInv;
    float4x4 gCurrViewProj;
    float4x4 gPrevViewProj;
    float3 gSunDir;
    float gProbeSpacingBaseFloat;

    float3 gSunColor;
    int    gNumPointLights;

    float3 gSkyColor;
    float  gSceneMaxDistance;

    float3 gCameraPos;
    float  gColorBleedingStrength;

    uint  gSparseProbeTableCapacity;
    uint  gSparseProbeCount;
    uint  gSparseProbeCellSize;
    uint  gSparsePadding0;

    PteroLightData gPointLights[MAX_RC_POINT_LIGHTS];
};

Texture2D<float4> gAlbedo : register(t0);
Texture2D<float4> gNormalDepth : register(t1);
Texture2D<float4> gMaterial : register(t2);
Texture2D<float> gSceneDepth : register(t3);
RaytracingAccelerationStructure gSceneTlas : register(t4);
StructuredBuffer<GpuPackedVertex> gVertices : register(t5);
StructuredBuffer<uint> gIndices : register(t6);
StructuredBuffer<GpuInstanceInfo> gInstanceInfo : register(t7);
StructuredBuffer<GpuMaterialRange> gMaterialRanges : register(t8);
RWStructuredBuffer<SparseProbeEntry> gSparseProbes : register(u1);
RWStructuredBuffer<uint> gSparseProbeCounter : register(u2);
RWTexture2D<float4> gTraceOut : register(u0);
SamplerState gLinearClamp : register(s0);

static const float PI = 3.14159265f;
static const uint RC_SPARSE_PROBE_EMPTY = 0u;
static const uint RC_SPARSE_PROBE_OCCUPIED = 1u;

float3 ApplyColorBleedingStrength(float3 albedo)
{
    const float luminance = max(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    return saturate(luminance.xxx + (albedo - luminance.xxx) * max(gColorBleedingStrength, 0.0f));
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 ReconstructWorldPosition(uint2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gViewProjInv);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

uint InitRng(uint2 pixel, uint frameIndex)
{
    return (pixel.x * 1973u + pixel.y * 9277u + frameIndex * 26699u) | 1u;
}

float RandFloat(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) * (1.0f / float(1 << 24));
}

float3 CosineSampleHemisphere(float2 xi, float3 N)
{
    float phi = 2.0f * PI * xi.x;
    float cosTheta = sqrt(1.0f - xi.y);
    float sinTheta = sqrt(xi.y);
    float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(localDir.x * tangent + localDir.y * bitangent + localDir.z * N);
}

bool TraceVisibility(float3 origin, float3 dir, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.001f;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(gSceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

float3 ResolveHitAlbedo(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = gMaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return float3(range.baseColorR, range.baseColorG, range.baseColorB);
    }

    return 1.0f.xxx;
}

float3 EvaluateSky(float3 dir)
{
    float t = saturate(dir.z * 0.5f + 0.5f);
    return lerp(gSkyColor * 0.35f, gSkyColor, t);
}

float3 EvaluatePointLights(float3 hitPos, float3 hitNormal, float3 hitAlbedo)
{
    float3 sum = 0.0f.xxx;
    [loop]
    for (int i = 0; i < min(gNumPointLights, MAX_RC_POINT_LIGHTS); ++i)
    {
        // Resolve the emitter shape so a spot cone and a rect facing bound the
        // cascade gather exactly as they bound the direct light.
        PteroResolvedLight shape = PteroResolveLightShape(gPointLights[i], hitPos);
        if (shape.ShapeMask <= 0.0f)
            continue;

        float3 toLight = shape.Position - hitPos;
        float distSq = dot(toLight, toLight);
        float normDistSq = saturate(distSq * gPointLights[i].InvRadiusSq);
        if (normDistSq >= 1.0f)
            continue;

        float invDist = rsqrt(max(distSq, 1e-6f));
        float lightDist = rcp(invDist);
        float3 L = toLight * invDist;
        float ndotl = saturate(dot(hitNormal, L));
        if (ndotl <= 0.0f)
            continue;

        float falloff = pow(saturate(1.0f - normDistSq), max(gPointLights[i].FalloffExponent, 1.0f)) * shape.ShapeMask;
        float visibility = 1.0f;
        if (gPointLights[i].CastShadows > 0.5f)
        {
            float shadowBias = 0.02f;
            visibility = TraceVisibility(hitPos + hitNormal * shadowBias, L, max(lightDist - shadowBias * 2.0f, 0.0f)) ? 1.0f : 0.0f;
        }

        sum += hitAlbedo * gPointLights[i].Color * ndotl * falloff * visibility;
    }

    return sum;
}

float3 EvaluateSurfaceLighting(float3 hitPos, float3 hitNormal, float3 hitAlbedo)
{
    float3 sunDir = -normalize(gSunDir);
    float sunNdotL = saturate(dot(hitNormal, sunDir));
    float sunVisibility = sunNdotL > 0.0f ? (TraceVisibility(hitPos + hitNormal * 0.02f, sunDir, gSceneMaxDistance) ? 1.0f : 0.0f) : 0.0f;
    float3 directSun = hitAlbedo * gSunColor * sunNdotL * sunVisibility;
    float3 directSky = hitAlbedo * EvaluateSky(hitNormal) * 0.5f;
    float3 directPoints = EvaluatePointLights(hitPos, hitNormal, hitAlbedo);
    return directSun + directSky + directPoints;
}

uint HashSparseProbeCell(int3 cell)
{
    uint x = asuint(cell.x) * 73856093u;
    uint y = asuint(cell.y) * 19349663u;
    uint z = asuint(cell.z) * 83492791u;
    return x ^ y ^ z;
}

int3 WorldToSparseProbeCell(float3 worldPos)
{
    const float cellSize = max(float(gSparseProbeCellSize), 1.0f);
    return int3(floor(worldPos / cellSize));
}

float3 UnpackSparseProbeRadiance(SparseProbeEntry entry)
{
    if (entry.SampleCount == 0u)
        return 0.0f.xxx;

    float invCount = rcp(float(entry.SampleCount));
    return float3(entry.PackedRadianceR, entry.PackedRadianceG, entry.PackedRadianceB) * (invCount / 1024.0f);
}

void AccumulateSparseProbe(float3 worldPos, float3 radiance)
{
    if (gSparseProbeTableCapacity == 0u)
        return;

    int3 cell = WorldToSparseProbeCell(worldPos);
    uint hash = HashSparseProbeCell(cell);

    [loop]
    for (uint attempt = 0; attempt < min(gSparseProbeTableCapacity, 32u); ++attempt)
    {
        uint slot = (hash + attempt) % gSparseProbeTableCapacity;
        uint previousState = RC_SPARSE_PROBE_EMPTY;
        InterlockedCompareExchange(gSparseProbes[slot].State, RC_SPARSE_PROBE_EMPTY, RC_SPARSE_PROBE_OCCUPIED, previousState);

        if (previousState == RC_SPARSE_PROBE_EMPTY)
        {
            gSparseProbes[slot].CellX = cell.x;
            gSparseProbes[slot].CellY = cell.y;
            gSparseProbes[slot].CellZ = cell.z;
            gSparseProbes[slot].PackedRadianceR = 0u;
            gSparseProbes[slot].PackedRadianceG = 0u;
            gSparseProbes[slot].PackedRadianceB = 0u;
            gSparseProbes[slot].SampleCount = 0u;
            InterlockedAdd(gSparseProbeCounter[0], 1u);
        }

        if (gSparseProbes[slot].State == RC_SPARSE_PROBE_OCCUPIED
            && gSparseProbes[slot].CellX == cell.x
            && gSparseProbes[slot].CellY == cell.y
            && gSparseProbes[slot].CellZ == cell.z)
        {
            uint packedR = (uint)min(radiance.r * 1024.0f, 65535.0f);
            uint packedG = (uint)min(radiance.g * 1024.0f, 65535.0f);
            uint packedB = (uint)min(radiance.b * 1024.0f, 65535.0f);
            InterlockedAdd(gSparseProbes[slot].PackedRadianceR, packedR);
            InterlockedAdd(gSparseProbes[slot].PackedRadianceG, packedG);
            InterlockedAdd(gSparseProbes[slot].PackedRadianceB, packedB);
            InterlockedAdd(gSparseProbes[slot].SampleCount, 1u);
            return;
        }
    }
}

float3 SampleSparseProbeRadiance(float3 worldPos)
{
    if (gSparseProbeTableCapacity == 0u)
        return 0.0f.xxx;

    int3 cell = WorldToSparseProbeCell(worldPos);
    uint hash = HashSparseProbeCell(cell);

    [loop]
    for (uint attempt = 0; attempt < min(gSparseProbeTableCapacity, 32u); ++attempt)
    {
        uint slot = (hash + attempt) % gSparseProbeTableCapacity;
        SparseProbeEntry entry = gSparseProbes[slot];
        if (entry.State == RC_SPARSE_PROBE_EMPTY)
            break;

        if (entry.CellX == cell.x && entry.CellY == cell.y && entry.CellZ == cell.z)
            return UnpackSparseProbeRadiance(entry);
    }

    return 0.0f.xxx;
}

struct RayResult
{
    bool hit;
    float3 hitPos;
    float3 hitNormal;
    float3 albedo;
    float3 radiance;
};

RayResult TraceBounce(float3 origin, float3 dir, float maxDistance)
{
    RayResult result;
    result.hit = false;
    result.hitPos = origin + dir * maxDistance;
    result.hitNormal = -dir;
    result.albedo = 1.0f.xxx;
    result.radiance = EvaluateSky(dir);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.001f;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(gSceneTlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}

    if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return result;

    const float t = rq.CommittedRayT();
    const uint instIdx = rq.CommittedInstanceIndex();
    const uint primIdx = rq.CommittedPrimitiveIndex();
    const float2 bary = rq.CommittedTriangleBarycentrics();
    const GpuInstanceInfo info = gInstanceInfo[instIdx];

    const uint i0 = gIndices[info.indexOffset + primIdx * 3 + 0];
    const uint i1 = gIndices[info.indexOffset + primIdx * 3 + 1];
    const uint i2 = gIndices[info.indexOffset + primIdx * 3 + 2];

    const GpuPackedVertex v0 = gVertices[info.vertexOffset + i0];
    const GpuPackedVertex v1 = gVertices[info.vertexOffset + i1];
    const GpuPackedVertex v2 = gVertices[info.vertexOffset + i2];

    const float b0 = 1.0f - bary.x - bary.y;
    const float3 localNormal = b0 * float3(v0.nx, v0.ny, v0.nz)
        + bary.x * float3(v1.nx, v1.ny, v1.nz)
        + bary.y * float3(v2.nx, v2.ny, v2.nz);

    const float3x4 o2w = rq.CommittedObjectToWorld3x4();
    const float3x3 o2wRot = float3x3(
        float3(o2w[0][0], o2w[1][0], o2w[2][0]),
        float3(o2w[0][1], o2w[1][1], o2w[2][1]),
        float3(o2w[0][2], o2w[1][2], o2w[2][2]));

    float3 hitPos = origin + dir * t;
    float3 worldNormal = normalize(mul(o2wRot, localNormal));
    if (dot(worldNormal, -dir) < 0.0f)
        worldNormal = -worldNormal;

    float3 albedo = ApplyColorBleedingStrength(saturate(ResolveHitAlbedo(info, primIdx)));

    result.hit = true;
    result.hitPos = hitPos;
    result.hitNormal = worldNormal;
    result.albedo = albedo;
    result.radiance = EvaluateSurfaceLighting(hitPos, worldNormal, albedo);
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFrameWidth || dispatchThreadId.y >= gFrameHeight)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float4 normalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float depth = normalDepth.z;
    if (depth <= 0.0f || depth >= 1.0f)
    {
        gTraceOut[pixel] = 0.0f.xxxx;
        return;
    }

    const float3 surfaceNormal = DecodeOctNormal(normalDepth.xy);
    const float3 worldPos = ReconstructWorldPosition(pixel, depth);

    uint rng = InitRng(pixel, gFrameIndex);
    const uint cascadeCount = max(gCascadeCount, 1u);
    const uint raysPerCascade = max(gRaysPerProbe, 1u);
    float3 indirect = 0.0f.xxx;
    float totalWeight = 0.0f;

    [loop]
    for (uint cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        float cascadeScale = pow(max(gRayLengthScale, 1.0f), cascadeIndex);
        float intervalScale = pow(max(gIntervalLengthScale, 1.0f), cascadeIndex);
        float cascadeDistance = gRayLengthBase * cascadeScale * max(intervalScale, 1.0f);
        float cascadeWeight = rcp(1.0f + float(cascadeIndex));

        [loop]
        for (uint rayIndex = 0; rayIndex < raysPerCascade; ++rayIndex)
        {
            float2 xi = float2(RandFloat(rng), RandFloat(rng));
            float3 rayDir = CosineSampleHemisphere(xi, surfaceNormal);
            RayResult bounce = TraceBounce(worldPos + surfaceNormal * 0.02f, rayDir, min(cascadeDistance, gSceneMaxDistance));

            float distanceAtten = bounce.hit ? rcp(1.0f + length(bounce.hitPos - worldPos) / max(cascadeDistance, 0.1f)) : 1.0f;
            float3 sampleRadiance = bounce.radiance * (PI * distanceAtten);
            if (bounce.hit)
            {
                AccumulateSparseProbe(worldPos, sampleRadiance * cascadeWeight);
                sampleRadiance = lerp(sampleRadiance, max(sampleRadiance, SampleSparseProbeRadiance(worldPos)), 0.35f);
            }

            indirect += sampleRadiance * cascadeWeight;
            totalWeight += cascadeWeight;
        }
    }

    float3 gi = totalWeight > 0.0f ? indirect / totalWeight : 0.0f.xxx;

    if (gDebugView == 1)
    {
        gTraceOut[pixel] = float4(gi, 1.0f);
        return;
    }

    if (gDebugView == 2)
    {
        float luma = dot(gi, float3(0.2126f, 0.7152f, 0.0722f));
        gTraceOut[pixel] = float4(luma.xxx, 1.0f);
        return;
    }

    gTraceOut[pixel] = float4(gi, 1.0f);
}
