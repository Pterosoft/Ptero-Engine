// RadianceProbes_Update.hlsl  –  cs_6_5
// One thread per probe.
// Fires g_RaysPerProbe uniform-sphere rays from the probe world position using
// inline RayQuery (cs_6_5) and accumulates radiance into L2 SH coefficients.
// The result is blended with the previous frame's SH via EMA (g_UpdateBlend).

#include "RadianceProbes_Common.hlsli"

// ─── Geometry pools (same as in RTGI RayGen) ────────────────────────────────
struct GpuPackedVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct GpuInstanceInfo
{
    uint vertexOffset, indexOffset, vertexCount, indexCount, materialRangeOffset, materialRangeCount, _pad0, _pad1;
};

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

StructuredBuffer<GpuPackedVertex>       t_Vertices      : register(t1);
StructuredBuffer<uint>                  t_Indices        : register(t2);
StructuredBuffer<GpuInstanceInfo>       t_InstanceInfo   : register(t3);
StructuredBuffer<GpuMaterialRange>      t_MaterialRanges : register(t4);
Texture2D<float4>                       t_BaseTextures[32] : register(t6);

// ─── TLAS ────────────────────────────────────────────────────────────────────
RaytracingAccelerationStructure t_TLAS : register(t5);

// ─── Probe SH buffers ────────────────────────────────────────────────────────
StructuredBuffer<ProbeSH>   t_ProbeSH_Prev : register(t0);   // previous frame SH (read)
RWStructuredBuffer<ProbeSH> u_ProbeSH      : register(u0);   // current frame SH (write)

SamplerState gLinearWrapSampler : register(s0);

static const uint INVALID_TEXTURE_INDEX = 0xffffffffu;

// ─────────────────────────────────────────────────────────────────────────────
// Shadow ray helper.
// ─────────────────────────────────────────────────────────────────────────────
bool ShadowRay(float3 origin, float3 dir, float maxDist)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.01f;
    ray.TMax      = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple diffuse shade at a probe hit.
// ─────────────────────────────────────────────────────────────────────────────
float3 ShadeProbeHit(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    const float3 sunDir = -g_SunDir;
    float ndotL = saturate(dot(hitNormal, sunDir));
    float geoNdotL = saturate(dot(hitGeoNormal, sunDir));
    float sunVis = 1.0f;
    if (ndotL > 0.0f && geoNdotL > 0.0f)
    {
        float bias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
        float3 org  = hitPos + hitGeoNormal * bias + sunDir * bias;
        sunVis = ShadowRay(org, sunDir, 1e4f) ? 1.0f : 0.0f;
        sunVis = lerp(0.35f, 1.0f, sunVis);
    }
    return hitAlbedo * g_SunColor * ndotL * sunVis;
}

float3 CosineSampleHemisphere(float2 xi, float3 N)
{
    float phi      = 6.28318530f * xi.x;
    float cosTheta = sqrt(1.0f - xi.y);
    float sinTheta = sqrt(xi.y);

    float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up    = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    return normalize(localDir.x * right + localDir.y * fwd + localDir.z * N);
}

GpuMaterialRange ResolveMaterialRange(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = t_MaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
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

float3 ResolveHitAlbedo(GpuMaterialRange range, float2 uv, float lod)
{
    float4 baseColor = float4(range.baseColorR, range.baseColorG, range.baseColorB, range.baseColorA);
    if (range.baseColorTextureIndex != INVALID_TEXTURE_INDEX)
    {
        baseColor *= t_BaseTextures[NonUniformResourceIndex(range.baseColorTextureIndex)].SampleLevel(gLinearWrapSampler, uv, lod);
    }

    return baseColor.rgb;
}

float3 ApplyColorLeakIntensity(float3 albedo)
{
    const float luminance = max(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    return saturate(luminance.xxx + (albedo - luminance.xxx) * max(g_ColorLeakIntensity, 0.0f));
}

float3 EvaluateProbePointLights(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    float3 pointLightSum = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int i = 0; i < min(g_NumPointLights, MAX_RADIANCE_PROBE_POINT_LIGHTS); ++i)
    {
        // Resolve the emitter shape so a spot's cone and a rect's facing bound
        // the probe's gather exactly as they bound the direct light.
        const PteroResolvedLight shape = PteroResolveLightShape(g_PointLights[i], hitPos);
        if (shape.ShapeMask <= 0.0f)
            continue;

        const float3 toLight = shape.Position - hitPos;
        const float distSq = dot(toLight, toLight);
        const float normDistSq = saturate(distSq * g_PointLights[i].InvRadiusSq);
        if (normDistSq >= 1.0f)
            continue;

        const float invDist = rsqrt(max(distSq, 1e-6f));
        const float lightDistance = rcp(invDist);
        const float3 lightDir = toLight * invDist;
        const float ndotL = saturate(dot(hitNormal, lightDir));
        const float geoNdotL = saturate(dot(hitGeoNormal, lightDir));
        if (ndotL <= 0.0f || geoNdotL <= 0.0f)
            continue;

        const float bias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
        const float maxShadowDistance = max(lightDistance - bias * 2.0f, 0.0f);
        float visibility = 1.0f;
        if (maxShadowDistance > 0.0f)
        {
            const float3 shadowOrigin = hitPos + hitGeoNormal * bias + lightDir * bias;
            visibility = ShadowRay(shadowOrigin, lightDir, maxShadowDistance) ? 1.0f : 0.0f;
            visibility = lerp(0.35f, 1.0f, visibility);
        }

        const float falloff = (1.0f - normDistSq) * (1.0f - normDistSq) * shape.ShapeMask;
        pointLightSum += hitAlbedo * g_PointLights[i].Color * ndotL * falloff * visibility;
    }

    return pointLightSum;
}

float3 EvaluateSecondaryDirect(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    return ShadeProbeHit(hitPos, hitNormal, hitGeoNormal, hitAlbedo)
         + EvaluateProbePointLights(hitPos, hitNormal, hitGeoNormal, hitAlbedo);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sky radiance for rays that miss all geometry.
// ─────────────────────────────────────────────────────────────────────────────
float3 SkyRadiance(float3 dir)
{
    float t = saturate(dir.z);
    return lerp(g_SkyColor * 0.7f, g_SkyColor, t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main compute entry.
// One thread per probe, dispatched linearly in groups of kProbeUpdateGroupSize.
//
// This used to be numthreads(1,1,1) with one group per probe, which put a single
// active lane in every wave and threw away almost all of the GPU's width. That is
// what made a higher ray count look unaffordable and left the update pinned at one
// ray per probe. The bounds check below already tolerates a partial final group.
// ─────────────────────────────────────────────────────────────────────────────
#define kProbeUpdateGroupSize 64

[numthreads(kProbeUpdateGroupSize, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint probeIdx = DTid.x;
    if (probeIdx >= g_TotalProbes)
        return;

    float3 probePos = ProbeCoordToWorld(ProbeIndexToCoord(probeIdx));

    // Accumulate a fresh SH from this frame's rays.
    ProbeSH freshSH;
    [unroll] for (int c = 0; c < 7; ++c)
        freshSH.c[c] = float4(0, 0, 0, 0);

    const float weight = 4.0f * 3.14159265f / float(g_RaysPerProbe); // sphere solid angle per sample

    [loop]
    for (uint i = 0; i < g_RaysPerProbe; ++i)
    {
        uint rng = ProbeRng(probeIdx, i, g_FrameIndex);
        float2 xi  = float2(ProbeRandFloat(rng), ProbeRandFloat(rng));
        float3 dir = UniformSphere(xi);

        float3 radiance = float3(0.0f, 0.0f, 0.0f);
        float3 throughput = float3(1.0f, 1.0f, 1.0f);
        float3 rayOrigin = probePos + dir * 0.05f;
        float3 rayDir = dir;
        const uint bounceCount = max((uint)g_MaxBounces, 1u);

        [loop]
        for (uint bounce = 0; bounce < bounceCount; ++bounce)
        {
            RayDesc ray;
            ray.Origin    = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin      = 0.0f;
            ray.TMax      = 1e4f;

            RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> rq;
            rq.TraceRayInline(t_TLAS, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
            while (rq.Proceed()) {}

            if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            {
                radiance += throughput * SkyRadiance(rayDir);
                break;
            }

            float t2  = rq.CommittedRayT();
            float3 hp = rayOrigin + rayDir * t2;

            uint instIdx = rq.CommittedInstanceIndex();
            uint primIdx = rq.CommittedPrimitiveIndex();
            float2 bary  = rq.CommittedTriangleBarycentrics();

            GpuInstanceInfo info = t_InstanceInfo[instIdx];
            uint i0 = t_Indices[info.indexOffset + primIdx * 3 + 0];
            uint i1 = t_Indices[info.indexOffset + primIdx * 3 + 1];
            uint i2 = t_Indices[info.indexOffset + primIdx * 3 + 2];

            GpuPackedVertex v0 = t_Vertices[info.vertexOffset + i0];
            GpuPackedVertex v1 = t_Vertices[info.vertexOffset + i1];
            GpuPackedVertex v2 = t_Vertices[info.vertexOffset + i2];

            float b0 = 1.0f - bary.x - bary.y;
            float3 localPos0 = float3(v0.px, v0.py, v0.pz);
            float3 localPos1 = float3(v1.px, v1.py, v1.pz);
            float3 localPos2 = float3(v2.px, v2.py, v2.pz);
            float2 uv = b0  * float2(v0.u, v0.v)
                      + bary.x * float2(v1.u, v1.v)
                      + bary.y * float2(v2.u, v2.v);
            float3 localNormal = normalize(b0  * float3(v0.nx, v0.ny, v0.nz)
                                         + bary.x * float3(v1.nx, v1.ny, v1.nz)
                                         + bary.y * float3(v2.nx, v2.ny, v2.nz));

            float3x4 o2w = rq.CommittedObjectToWorld3x4();
            float3x3 o2wRot = float3x3(
                float3(o2w[0][0], o2w[1][0], o2w[2][0]),
                float3(o2w[0][1], o2w[1][1], o2w[2][1]),
                float3(o2w[0][2], o2w[1][2], o2w[2][2]));
            float3 worldPos0 = float3(
                dot(o2w[0], float4(localPos0, 1.0f)),
                dot(o2w[1], float4(localPos0, 1.0f)),
                dot(o2w[2], float4(localPos0, 1.0f)));
            float3 worldPos1 = float3(
                dot(o2w[0], float4(localPos1, 1.0f)),
                dot(o2w[1], float4(localPos1, 1.0f)),
                dot(o2w[2], float4(localPos1, 1.0f)));
            float3 worldPos2 = float3(
                dot(o2w[0], float4(localPos2, 1.0f)),
                dot(o2w[1], float4(localPos2, 1.0f)),
                dot(o2w[2], float4(localPos2, 1.0f)));
            // Same degeneracy guard as the RTGI ray generation: a sliver
            // triangle collapses this cross product and an unguarded normalize
            // would hand NaN to the next bounce's ray origin and direction.
            float3 geoNormal = SafeNormalizeOr(
                cross(worldPos1 - worldPos0, worldPos2 - worldPos0), -rayDir);
            float3 worldNormal = SafeNormalizeOr(mul(o2wRot, localNormal), geoNormal);
            if (dot(worldNormal, -rayDir) < 0.0f)
                worldNormal = -worldNormal;
            float3 worldGeoNormal = dot(geoNormal, -rayDir) >= 0.0f ? geoNormal : -geoNormal;

            GpuMaterialRange materialRange = ResolveMaterialRange(info, primIdx);
            float textureLod = saturate(log2(max(t2 * 0.25f, 1e-3f)));
            float3 hitAlbedo = ApplyColorLeakIntensity(ResolveHitAlbedo(materialRange, uv, textureLod));
            radiance += throughput * EvaluateSecondaryDirect(hp, worldNormal, worldGeoNormal, hitAlbedo);

            throughput *= hitAlbedo;
            if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3f)
                break;

            if (bounce + 1u >= bounceCount)
                break;

            float2 bounceXi = float2(ProbeRandFloat(rng), ProbeRandFloat(rng));
            float3 bounceDir = CosineSampleHemisphere(bounceXi, worldNormal);

            const float3 nextOrigin = hp + worldNormal * 0.01f + bounceDir * 0.01f;
            if (!IsFinitePosition(nextOrigin) || !IsFinitePosition(bounceDir))
                break;

            rayOrigin = nextOrigin;
            rayDir = bounceDir;
        }

        radiance = SanitizeRadiance(radiance);

        if (max(radiance.r, max(radiance.g, radiance.b)) <= 0.0f)
        {
            radiance = SkyRadiance(dir);
        }

        SHAddSample(freshSH, dir, radiance, weight);
    }

    ProbeSH outSH;
    if (g_FrameIndex == 0)
    {
        [unroll] for (int c2 = 0; c2 < 7; ++c2)
            outSH.c[c2] = freshSH.c[c2];
    }
    else
    {
        ProbeSH prevSH = t_ProbeSH_Prev[probeIdx];
        float blend = saturate(g_UpdateBlend);
        [unroll] for (int c2 = 0; c2 < 7; ++c2)
            outSH.c[c2] = lerp(prevSH.c[c2], freshSH.c[c2], blend);
    }

    u_ProbeSH[probeIdx] = outSH;
}
