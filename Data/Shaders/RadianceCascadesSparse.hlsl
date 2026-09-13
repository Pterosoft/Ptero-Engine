#if __INTELLISENSE__
#define RayQuery(x) int
#define RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH 0
#define RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES 0
#define COMMITTED_TRIANGLE_HIT 1
#endif

#define MAX_RC_POINT_LIGHTS 16
#define RC_SPARSE_PROBE_EMPTY 0u
#define RC_SPARSE_PROBE_OCCUPIED 1u
#define RC_SPARSE_PROBE_ALLOCATING 2u
#define RC_TRACE_GROUP_SIZE 64u
#define RC_INVALID_TEXTURE_INDEX 0xffffffffu
#define RC_SPARSE_PROBE_MAX_STALE_FRAMES 12u
#define RC_SPARSE_PROBE_TRACE_STALE_FRAMES 2u
#define RC_SPARSE_PROBE_GATHER_STALE_FRAMES 4u

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
    float3 Position;
    uint AnchorKey;
    float3 Normal;
    uint LastTouchedFrame;
    float4 Radiance;
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
    uint  gSparseProbeSearchSteps;

    float gSparseProbeReuseStrength;
    float gRayBias;
    float gSpatialFilterStrength;
    float gHistoryClampScale;

    float gHistoryDepthSensitivity;
    float gHistoryNormalThreshold;
    uint  gSparseFrameIndex;
    float gSparsePadding1;

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
StructuredBuffer<uint> gActiveProbeListIn : register(t9);
Texture2D<float4> gBaseTextures[32] : register(t10);
RWTexture2D<float4> gTraceOut : register(u0);
RWStructuredBuffer<SparseProbeEntry> gSparseProbes : register(u1);
RWStructuredBuffer<uint> gActiveProbeList : register(u2);
RWStructuredBuffer<uint> gActiveProbeCount : register(u3);
RWStructuredBuffer<uint> gIndirectArgs : register(u4);
SamplerState gLinearClamp : register(s0);

static const float PI = 3.14159265f;

float3 ApplyColorBleedingStrength(float3 albedo)
{
    const float luminance = max(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    return max(luminance.xxx + (albedo - luminance.xxx) * max(gColorBleedingStrength, 0.0f), 0.0f.xxx);
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

uint InitRng(uint a, uint b)
{
    return (a * 1973u + b * 9277u) | 1u;
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

float3 UniformSampleSphere(float2 xi)
{
    const float z = 1.0f - 2.0f * xi.x;
    const float r = sqrt(saturate(1.0f - z * z));
    const float phi = 2.0f * PI * xi.y;
    return float3(cos(phi) * r, sin(phi) * r, z);
}

bool TraceVisibility(float3 origin, float3 dir, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = max(gRayBias, 0.001f);
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(gSceneTlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

GpuMaterialRange ResolveMaterialRange(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = gMaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return range;
    }

    GpuMaterialRange fallback = (GpuMaterialRange)0;
    fallback.baseColorR = 1.0f;
    fallback.baseColorG = 1.0f;
    fallback.baseColorB = 1.0f;
    fallback.baseColorA = 1.0f;
    fallback.baseColorTextureIndex = RC_INVALID_TEXTURE_INDEX;
    fallback.opacityTextureIndex = RC_INVALID_TEXTURE_INDEX;
    return fallback;
}

float3 ResolveHitAlbedo(GpuInstanceInfo info, uint primIdx)
{
    const GpuMaterialRange range = ResolveMaterialRange(info, primIdx);
    return float3(range.baseColorR, range.baseColorG, range.baseColorB);
}

float3 EvaluateSky(float3 dir)
{
    float t = saturate(dir.z * 0.5f + 0.5f);
    return lerp(gSkyColor * 0.35f, gSkyColor, t);
}

float3 EvaluatePointLights(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
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
        float ndotl = abs(dot(hitNormal, L));
        float geoNdotL = abs(dot(hitGeoNormal, L));
        if (ndotl <= 0.0f || geoNdotL <= 0.0f)
            continue;

        float falloff = pow(saturate(1.0f - normDistSq), max(gPointLights[i].FalloffExponent, 1.0f)) * shape.ShapeMask;
        float visibility = 1.0f;
        if (gPointLights[i].CastShadows > 0.5f)
        {
            float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
            visibility = TraceVisibility(hitPos + hitGeoNormal * shadowBias + L * shadowBias, L, max(lightDist - shadowBias * 2.0f, 0.0f)) ? 1.0f : 0.0f;
            visibility = lerp(0.35f, 1.0f, visibility);
        }

        sum += hitAlbedo * gPointLights[i].Color * ndotl * falloff * visibility;
    }

    return sum;
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
    const float3 localPos0 = float3(v0.px, v0.py, v0.pz);
    const float3 localPos1 = float3(v1.px, v1.py, v1.pz);
    const float3 localPos2 = float3(v2.px, v2.py, v2.pz);

    const float3x4 o2w = rq.CommittedObjectToWorld3x4();
    const float3x3 o2wRot = float3x3(
        float3(o2w[0][0], o2w[1][0], o2w[2][0]),
        float3(o2w[0][1], o2w[1][1], o2w[2][1]),
        float3(o2w[0][2], o2w[1][2], o2w[2][2]));

    float3 hitPos = origin + dir * t;
    float3 worldNormal = normalize(mul(o2wRot, localNormal));
    if (dot(worldNormal, -dir) < 0.0f)
        worldNormal = -worldNormal;

    float3 worldPos0 = float3(dot(o2w[0], float4(localPos0, 1.0f)), dot(o2w[1], float4(localPos0, 1.0f)), dot(o2w[2], float4(localPos0, 1.0f)));
    float3 worldPos1 = float3(dot(o2w[0], float4(localPos1, 1.0f)), dot(o2w[1], float4(localPos1, 1.0f)), dot(o2w[2], float4(localPos1, 1.0f)));
    float3 worldPos2 = float3(dot(o2w[0], float4(localPos2, 1.0f)), dot(o2w[1], float4(localPos2, 1.0f)), dot(o2w[2], float4(localPos2, 1.0f)));
    float3 worldGeoNormal = normalize(cross(worldPos1 - worldPos0, worldPos2 - worldPos0));
    if (dot(worldGeoNormal, -dir) < 0.0f)
        worldGeoNormal = -worldGeoNormal;

    float3 albedo = ApplyColorBleedingStrength(saturate(ResolveHitAlbedo(info, primIdx)));
    float3 sunDir = -normalize(gSunDir);
    float sunNdotL = abs(dot(worldNormal, sunDir));
    float geoSunNdotL = abs(dot(worldGeoNormal, sunDir));
    float sunVisibility = 1.0f;
    if (sunNdotL > 0.0f && geoSunNdotL > 0.0f)
    {
        float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoSunNdotL, 0.05f)));
        sunVisibility = TraceVisibility(hitPos + sunDir * shadowBias, sunDir, max(gSceneMaxDistance - shadowBias * 2.0f, 1.0f)) ? 1.0f : 0.0f;
        sunVisibility = lerp(0.35f, 1.0f, sunVisibility);
    }
    float3 directSun = albedo * gSunColor * sunNdotL * sunVisibility;
    float3 directPoints = EvaluatePointLights(hitPos, worldNormal, worldGeoNormal, albedo);
    float3 directSky = albedo * EvaluateSky(worldNormal) * 0.25f;

    result.hit = true;
    result.hitPos = hitPos;
    result.hitNormal = worldNormal;
    result.albedo = albedo;
    result.radiance = directSun + directPoints + directSky;
    return result;
}

bool TraceCellSurfaceAnchor(float3 origin, float3 dir, float maxDistance, out float3 hitPos, out float3 hitNormal)
{
    hitPos = origin;
    hitNormal = float3(0.0f, 0.0f, 1.0f);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = max(gRayBias, 0.001f);
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(gSceneTlas, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}

    if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;

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

    hitPos = origin + dir * t;
    hitNormal = normalize(mul(o2wRot, localNormal));
    if (dot(hitNormal, -dir) < 0.0f)
        hitNormal = -hitNormal;

    return true;
}

uint HashSparseProbeCell(int3 cell)
{
    uint x = asuint(cell.x) * 73856093u;
    uint y = asuint(cell.y) * 19349663u;
    uint z = asuint(cell.z) * 83492791u;
    return x ^ y ^ z;
}

uint SparseProbeAge(uint lastTouchedFrame)
{
    return gSparseFrameIndex >= lastTouchedFrame ? gSparseFrameIndex - lastTouchedFrame : 0u;
}

float EffectiveSparseProbeCellSize()
{
    return clamp(float(gSparseProbeCellSize) * 0.5f, 4.0f, 8.0f);
}

int3 WorldToSparseProbeCell(float3 worldPos)
{
    const float cellSize = EffectiveSparseProbeCellSize();
    return int3(floor(worldPos / cellSize));
}

float3 SparseProbeCellCenter(int3 cell)
{
    const float cellSize = EffectiveSparseProbeCellSize();
    return (float3(cell) + 0.5f.xxx) * cellSize;
}

bool ResolveStableSparseProbeAnchor(int3 cell, out float3 probePos, out float3 probeNormal)
{
    const float cellSize = EffectiveSparseProbeCellSize();
    const float3 center = SparseProbeCellCenter(cell);
    const float maxDistance = max(cellSize * 1.75f, 1.0f);

    static const float3 directions[6] =
    {
        float3(0.0f, 0.0f, -1.0f),
        float3(0.0f, 0.0f, 1.0f),
        float3(1.0f, 0.0f, 0.0f),
        float3(-1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f),
        float3(0.0f, -1.0f, 0.0f)
    };

    bool found = false;
    float bestDistance = maxDistance + 1.0f;
    probePos = center;
    probeNormal = float3(0.0f, 0.0f, 1.0f);

    [unroll]
    for (uint i = 0; i < 6u; ++i)
    {
        float3 hitPos;
        float3 hitNormal;
        if (!TraceCellSurfaceAnchor(center, directions[i], maxDistance, hitPos, hitNormal))
            continue;

        const float dist = length(hitPos - center);
        if (!found || dist < bestDistance)
        {
            found = true;
            bestDistance = dist;
            probePos = hitPos + hitNormal * max(gRayBias, 0.002f);
            probeNormal = hitNormal;
        }
    }

    return found;
}

uint FindSparseProbeSlot(int3 cell)
{
    if (gSparseProbeTableCapacity == 0u)
        return 0xffffffffu;

    uint hash = HashSparseProbeCell(cell);
    [loop]
    for (uint attempt = 0; attempt < min(gSparseProbeTableCapacity, max(gSparseProbeSearchSteps, 1u)); ++attempt)
    {
        uint slot = (hash + attempt) % gSparseProbeTableCapacity;
        SparseProbeEntry entry = gSparseProbes[slot];
        if (entry.State == RC_SPARSE_PROBE_EMPTY)
            continue;
        if (entry.State == RC_SPARSE_PROBE_ALLOCATING)
            continue;
        if (entry.CellX == cell.x && entry.CellY == cell.y && entry.CellZ == cell.z)
            return slot;
    }

    return 0xffffffffu;
}

float3 ReadSparseProbeRadiance(int3 cell, float3 worldPos, float3 surfaceNormal, inout float totalWeight)
{
    const uint slot = FindSparseProbeSlot(cell);
    if (slot == 0xffffffffu)
        return 0.0f.xxx;

    SparseProbeEntry entry = gSparseProbes[slot];
    if (entry.Radiance.a <= 0.0f)
        return 0.0f.xxx;
    if (SparseProbeAge(entry.LastTouchedFrame) > RC_SPARSE_PROBE_GATHER_STALE_FRAMES)
        return 0.0f.xxx;

    float normalWeight = 1.0f;
    if (dot(entry.Normal, entry.Normal) > 1e-4f)
        normalWeight = lerp(0.15f, 1.0f, saturate(dot(normalize(entry.Normal), surfaceNormal)));

    const float cellDistance = length(SparseProbeCellCenter(cell) - worldPos);
    const float cellSize = EffectiveSparseProbeCellSize();
    const float normalizedDistance = saturate(cellDistance / max(cellSize * 1.75f, 1.0f));
    const float weight = (1.0f - normalizedDistance) * (1.0f - normalizedDistance) * normalWeight;
    totalWeight += weight;
    return entry.Radiance.rgb * weight;
}

uint ComputeAnchorKey(uint2 pixel, float3 worldPos, float3 cellCenter)
{
    const float cellSize = EffectiveSparseProbeCellSize();
    const float normalizedAnchorDistance = dot(worldPos - cellCenter, worldPos - cellCenter) / max(cellSize * cellSize, 1.0f);
    const uint distanceKey = min(uint(normalizedAnchorDistance * 65535.0f), 0xffffu);
    const uint pixelKey = (pixel.y * gFrameWidth + pixel.x) & 0xffffu;
    return (distanceKey << 16) | pixelKey;
}

[numthreads(RC_TRACE_GROUP_SIZE, 1, 1)]
void ClearSparseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint idx = dispatchThreadId.x;
    if (idx < gSparseProbeTableCapacity)
    {
        gActiveProbeList[idx] = 0u;

        if (gSparseProbeCount != 0u)
        {
            SparseProbeEntry entry = (SparseProbeEntry)0;
            entry.State = RC_SPARSE_PROBE_EMPTY;
            gSparseProbes[idx] = entry;
        }
        else if (gSparseProbes[idx].State == RC_SPARSE_PROBE_OCCUPIED)
        {
            SparseProbeEntry entry = gSparseProbes[idx];
            if (SparseProbeAge(entry.LastTouchedFrame) > RC_SPARSE_PROBE_MAX_STALE_FRAMES)
            {
                entry = (SparseProbeEntry)0;
                entry.State = RC_SPARSE_PROBE_EMPTY;
                gSparseProbes[idx] = entry;
            }
            else
            {
                entry.AnchorKey = 0xffffffffu;
                gSparseProbes[idx] = entry;
            }
        }
    }

    if (idx == 0u)
    {
        gActiveProbeCount[0] = 0u;
        gIndirectArgs[0] = 0u;
        gIndirectArgs[1] = 1u;
        gIndirectArgs[2] = 1u;
    }
}

[numthreads(8, 8, 1)]
void AllocateSparseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFrameWidth || dispatchThreadId.y >= gFrameHeight)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float4 normalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float depth = normalDepth.z;
    if (depth <= 0.0f || depth >= 1.0f)
        return;

    const float3 worldPos = ReconstructWorldPosition(pixel, depth);
    const float3 worldNormal = DecodeOctNormal(normalDepth.xy);
    const int3 cell = WorldToSparseProbeCell(worldPos);
    const float3 cellCenter = SparseProbeCellCenter(cell);
    const uint anchorKey = ComputeAnchorKey(pixel, worldPos, cellCenter);
    const uint hash = HashSparseProbeCell(cell);

    [loop]
    for (uint attempt = 0; attempt < min(gSparseProbeTableCapacity, max(gSparseProbeSearchSteps, 1u)); ++attempt)
    {
        uint slot = (hash + attempt) % gSparseProbeTableCapacity;
        uint previousState = RC_SPARSE_PROBE_EMPTY;
        InterlockedCompareExchange(gSparseProbes[slot].State, RC_SPARSE_PROBE_EMPTY, RC_SPARSE_PROBE_ALLOCATING, previousState);

        if (previousState == RC_SPARSE_PROBE_EMPTY)
        {
            gSparseProbes[slot].CellX = cell.x;
            gSparseProbes[slot].CellY = cell.y;
            gSparseProbes[slot].CellZ = cell.z;
            gSparseProbes[slot].Position = 0.0f.xxx;
            gSparseProbes[slot].AnchorKey = 0xffffffffu;
            gSparseProbes[slot].Normal = 0.0f.xxx;
            gSparseProbes[slot].LastTouchedFrame = gSparseFrameIndex;
            gSparseProbes[slot].Radiance = 0.0f.xxxx;
            DeviceMemoryBarrier();
            gSparseProbes[slot].State = RC_SPARSE_PROBE_OCCUPIED;
            InterlockedMin(gSparseProbes[slot].AnchorKey, anchorKey);
            return;
        }

        [loop]
        for (uint spin = 0; previousState == RC_SPARSE_PROBE_ALLOCATING && spin < 32u; ++spin)
        {
            DeviceMemoryBarrier();
            previousState = gSparseProbes[slot].State;
        }

        if (previousState == RC_SPARSE_PROBE_ALLOCATING)
            return;

        if (previousState == RC_SPARSE_PROBE_OCCUPIED &&
            gSparseProbes[slot].CellX == cell.x && gSparseProbes[slot].CellY == cell.y && gSparseProbes[slot].CellZ == cell.z)
        {
            gSparseProbes[slot].LastTouchedFrame = gSparseFrameIndex;
            InterlockedMin(gSparseProbes[slot].AnchorKey, anchorKey);
            return;
        }
    }
}

[numthreads(8, 8, 1)]
void ResolveSparseAnchorsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFrameWidth || dispatchThreadId.y >= gFrameHeight)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float4 normalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float depth = normalDepth.z;
    if (depth <= 0.0f || depth >= 1.0f)
        return;

    const float3 worldPos = ReconstructWorldPosition(pixel, depth);
    const float3 worldNormal = DecodeOctNormal(normalDepth.xy);
    const int3 cell = WorldToSparseProbeCell(worldPos);
    const uint slot = FindSparseProbeSlot(cell);
    if (slot == 0xffffffffu)
        return;

    const uint anchorKey = ComputeAnchorKey(pixel, worldPos, SparseProbeCellCenter(cell));
    if (anchorKey != gSparseProbes[slot].AnchorKey)
        return;

    gSparseProbes[slot].Position = worldPos + worldNormal * max(gRayBias, 0.002f);
    gSparseProbes[slot].Normal = worldNormal;
    gSparseProbes[slot].LastTouchedFrame = gSparseFrameIndex;
}

[numthreads(RC_TRACE_GROUP_SIZE, 1, 1)]
void CompactSparseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint idx = dispatchThreadId.x;
    if (idx >= gSparseProbeTableCapacity)
        return;

    if (gSparseProbes[idx].State != RC_SPARSE_PROBE_OCCUPIED)
        return;

    if (gSparseProbes[idx].AnchorKey == 0xffffffffu && gSparseProbes[idx].Radiance.a <= 0.0f)
        return;
    if (SparseProbeAge(gSparseProbes[idx].LastTouchedFrame) > RC_SPARSE_PROBE_TRACE_STALE_FRAMES)
        return;

    uint outIndex = 0u;
    InterlockedAdd(gActiveProbeCount[0], 1u, outIndex);
    gActiveProbeList[outIndex] = idx;
}

[numthreads(1, 1, 1)]
void BuildIndirectArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint count = gActiveProbeCount[0];
    gIndirectArgs[0] = (count + RC_TRACE_GROUP_SIZE - 1u) / RC_TRACE_GROUP_SIZE;
    gIndirectArgs[1] = 1u;
    gIndirectArgs[2] = 1u;
}

[numthreads(RC_TRACE_GROUP_SIZE, 1, 1)]
void TraceSparseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint probeIndex = dispatchThreadId.x;
    uint activeCount = gActiveProbeCount[0];
    if (probeIndex >= activeCount)
        return;

    uint slot = gActiveProbeListIn[probeIndex];
    SparseProbeEntry entry = gSparseProbes[slot];
    int3 cell = int3(entry.CellX, entry.CellY, entry.CellZ);
    float3 worldPos = entry.Position;
    float3 probeNormal = dot(entry.Normal, entry.Normal) > 1e-4f ? normalize(entry.Normal) : float3(0.0f, 0.0f, 1.0f);
    float3 stableProbePos;
    float3 stableProbeNormal;
    bool hasStableAnchor = ResolveStableSparseProbeAnchor(cell, stableProbePos, stableProbeNormal);
    const bool anchorChanged = hasStableAnchor
        && (length(stableProbePos - worldPos) > max(EffectiveSparseProbeCellSize() * 0.25f, 0.25f)
            || dot(stableProbeNormal, probeNormal) < 0.85f);
    if (hasStableAnchor)
    {
        worldPos = stableProbePos;
        probeNormal = stableProbeNormal;
    }

    float cascadeDistance = gRayLengthBase;
    [loop]
    for (uint cascadeIndex = 1; cascadeIndex < max(gCascadeCount, 1u); ++cascadeIndex)
        cascadeDistance = max(cascadeDistance, gRayLengthBase * pow(max(gRayLengthScale, 1.0f), cascadeIndex) * pow(max(gIntervalLengthScale, 1.0f), cascadeIndex));

    uint rng = InitRng(HashSparseProbeCell(cell), asuint(cell.x * 92837111 ^ cell.y * 689287499 ^ cell.z * 283923481));
    const uint rayCount = max(gRaysPerProbe, 1u);
    float3 sampleRadiance = 0.0f.xxx;
    [loop]
    for (uint rayIndex = 0; rayIndex < rayCount; ++rayIndex)
    {
        float2 xi = float2(RandFloat(rng), RandFloat(rng));
        float3 rayDir = CosineSampleHemisphere(xi, probeNormal);
        RayResult bounce = TraceBounce(worldPos + probeNormal * max(gRayBias, 0.001f), rayDir, min(cascadeDistance, gSceneMaxDistance));
        if (bounce.hit)
        {
            sampleRadiance += bounce.radiance;
        }
        else
        {
            const float3 skyRadiance = EvaluateSky(rayDir);
            const float skyLuma = dot(skyRadiance, float3(0.2126f, 0.7152f, 0.0722f));
            sampleRadiance += lerp(skyLuma.xxx, skyRadiance, 0.25f) * 0.35f;
        }
    }
    sampleRadiance /= float(rayCount);

    const float probeHistory = entry.Radiance.a > 0.0f && !anchorChanged ? saturate(gHysteresis) : 0.0f;
    const float3 blendedRadiance = lerp(sampleRadiance, entry.Radiance.rgb, probeHistory);
    entry.Position = worldPos;
    entry.Normal = probeNormal;
    entry.Radiance = float4(blendedRadiance, 1.0f);
    gSparseProbes[slot] = entry;
}

[numthreads(8, 8, 1)]
void GatherSparseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
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

    const float3 worldPos = ReconstructWorldPosition(pixel, depth);
    const float3 surfaceNormal = DecodeOctNormal(normalDepth.xy);
    const int3 cell = WorldToSparseProbeCell(worldPos);
    float totalWeight = 0.0f;
    float3 radiance = ReadSparseProbeRadiance(cell, worldPos, surfaceNormal, totalWeight);

    const float neighborStrength = saturate(gSparseProbeReuseStrength) * 0.55f;
    if (neighborStrength > 0.0f)
    {
        [loop]
        for (int z = 0; z <= 0; ++z)
        {
            [loop]
            for (int y = -1; y <= 1; ++y)
            {
                [loop]
                for (int x = -1; x <= 1; ++x)
                {
                    if (x == 0 && y == 0 && z == 0)
                        continue;

                    const int3 offset = int3(x, y, z);
                    const float offsetDistance = length(float3(offset));
                    const float reuseWeight = neighborStrength / (1.0f + offsetDistance);
                    float neighborWeight = 0.0f;
                    radiance += ReadSparseProbeRadiance(cell + offset, worldPos, surfaceNormal, neighborWeight) * reuseWeight;
                    totalWeight += neighborWeight * reuseWeight;
                }
            }
        }
    }

    gTraceOut[pixel] = totalWeight > 0.0f ? float4(radiance / totalWeight, 1.0f) : float4(0.035f.xxx, 1.0f);
}
