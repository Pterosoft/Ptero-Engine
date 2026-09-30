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
    ray.TMin      = 0.0f;
    ray.TMax      = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diffuse shade at a probe hit: next-event estimation toward the sun.
//
// Kept identical to EvaluateSecondaryDirect in RtGI_RayGen.hlsl, so switching
// GI mode changes where irradiance is stored, not what it is. This used to
// floor a blocked shadow ray at 0.35, which RTGI dropped long ago: it lit every
// hit the sun cannot reach at a third of full sun, and an interior like Sponza
// is mostly such hits, so the probes glowed with light that never got in.
// ─────────────────────────────────────────────────────────────────────────────
float3 ShadeProbeHit(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    const float3 sunDir = -g_SunDir;
    // One-sided: hitNormal already faces the ray that found this surface.
    const float ndotL = saturate(dot(hitNormal, sunDir));
    if (ndotL <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    // Offset along the geometric normal on the sun's side as well as toward the
    // sun, so a grazing shadow ray does not start under its own triangle.
    const float geoNdotL = max(abs(dot(hitGeoNormal, sunDir)), 1e-3f);
    const float bias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
    const float3 sunSideNormal = hitGeoNormal * (dot(hitGeoNormal, sunDir) >= 0.0f ? 1.0f : -1.0f);
    const float3 origin = hitPos + sunSideNormal * bias + sunDir * bias;
    const float sunVis = ShadowRay(origin, sunDir, max(1e4f - bias * 2.0f, 1.0f)) ? 1.0f : 0.0f;

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
        if (ndotL <= 0.0f)
            continue;
        // Only sizes the shadow-ray bias, as in RTGI.
        const float geoNdotL = max(abs(dot(hitGeoNormal, lightDir)), 1e-3f);

        // Binary visibility - no 0.35 floor; see ShadeProbeHit.
        const float bias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
        const float maxShadowDistance = max(lightDistance - bias * 2.0f, 0.0f);
        float visibility = 1.0f;
        if (maxShadowDistance > 0.0f)
        {
            const float3 lightSideNormal = hitGeoNormal * (dot(hitGeoNormal, lightDir) >= 0.0f ? 1.0f : -1.0f);
            const float3 shadowOrigin = hitPos + lightSideNormal * bias + lightDir * bias;
            visibility = ShadowRay(shadowOrigin, lightDir, maxShadowDistance) ? 1.0f : 0.0f;
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

    const uint cascade = ProbeIndexToCascade(probeIdx);
    const uint localIdx = ProbeIndexToLocal(probeIdx);
    const uint3 coord = ProbeIndexToCoord(localIdx);
    float3 probePos = ProbeCoordToWorld(cascade, coord);

    // Each cascade follows the camera in whole cells of its own, so the probe
    // that held this slot last frame now stands somewhere else. Find the
    // history of the probe that was at this position.
    const int3 gridSize = int3(g_ProbeGridX, g_ProbeGridY, g_ProbeGridZ);
    const int3 historyShift = g_CascadeHistory[cascade].xyz;
    const int3 prevCoord = int3(coord) + historyShift;
    const bool hasHistory = g_FrameIndex != 0
        && all(prevCoord >= 0)
        && all(prevCoord < gridSize);
    const uint prevIdx = ProbeCoordToIndex(cascade, uint3(clamp(prevCoord, int3(0, 0, 0), gridSize - 1)));

    // Coarser cascades trace a share of their probes each frame, a whole
    // group at a time so a wave either traces or copies. A probe skipped this
    // frame carries its history across unchanged; one without history always
    // traces.
    const uint tracePeriod = (uint)max(g_CascadeHistory[cascade].w, 1);
    const bool traceThisFrame = !hasHistory
        || ((localIdx / kProbeUpdateGroupSize + g_FrameIndex) % tracePeriod) == 0u;
    if (!traceThisFrame)
    {
        u_ProbeSH[probeIdx] = t_ProbeSH_Prev[prevIdx];
        return;
    }

    // Accumulate a fresh SH from this frame's rays.
    ProbeSH freshSH;
    [unroll] for (int c = 0; c < 7; ++c)
        freshSH.c[c] = float4(0, 0, 0, 0);

    const float weight = 4.0f * 3.14159265f / float(g_RaysPerProbe); // sphere solid angle per sample

    // Primary rays that land on the inside of a surface. A probe that sees
    // mostly back faces sits inside a wall or a pillar, and whatever it gathers
    // there is not light the room around it receives.
    uint backfaceHits = 0;

    [loop]
    for (uint i = 0; i < g_RaysPerProbe; ++i)
    {
        uint rng = ProbeRng(probeIdx, i, g_FrameIndex);
        float2 xi  = float2(ProbeRandFloat(rng), ProbeRandFloat(rng));
        float3 dir = UniformSphere(xi);

        float3 radiance = float3(0.0f, 0.0f, 0.0f);
        float3 throughput = float3(1.0f, 1.0f, 1.0f);
        float3 rayOrigin = probePos;
        float3 rayDir = dir;
        const uint bounceCount = max((uint)g_MaxBounces, 1u);

        [loop]
        for (uint bounce = 0; bounce < bounceCount; ++bounce)
        {
            RayDesc ray;
            ray.Origin    = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin      = 0.001f;
            ray.TMax      = 1e4f;

            // No back-face culling, as in RTGI. Culling let a probe inside a
            // pillar look straight out through it, and let bounce rays leave a
            // room through any single-sided wall they met from behind.
            RayQuery<RAY_FLAG_NONE> rq;
            rq.TraceRayInline(t_TLAS, RAY_FLAG_NONE, 0xFF, ray);
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

            // The authored vertex normals point out of a closed mesh whatever
            // its winding, so they say which side the ray arrived from without
            // trusting the TLAS front-face convention.
            if (bounce == 0u && dot(worldNormal, rayDir) > 0.0f)
                ++backfaceHits;

            if (dot(worldNormal, -rayDir) < 0.0f)
                worldNormal = -worldNormal;
            float3 worldGeoNormal = dot(geoNormal, -rayDir) >= 0.0f ? geoNormal : -geoNormal;

            // Lift the hit onto the smooth surface its vertex normals describe
            // (Hanika, "Hacking the Shadow Terminator"), exactly as RTGI does, so
            // NEE and the next bounce leave from the surface the shading assumes
            // rather than a facet its neighbours shadow.
            float3 shadePos = hp;
            {
                const float3 vertexPos[3] = { worldPos0, worldPos1, worldPos2 };
                const float3 vertexNrm[3] = {
                    mul(o2wRot, float3(v0.nx, v0.ny, v0.nz)),
                    mul(o2wRot, float3(v1.nx, v1.ny, v1.nz)),
                    mul(o2wRot, float3(v2.nx, v2.ny, v2.nz)) };
                const float3 weights = float3(b0, bary.x, bary.y);
                [unroll]
                for (uint k = 0; k < 3; ++k)
                {
                    float3 n = vertexNrm[k];
                    const float nLengthSq = dot(n, n);
                    if (nLengthSq < 1e-12f)
                        continue;
                    n *= rsqrt(nLengthSq);
                    if (dot(n, worldGeoNormal) < 0.0f)
                        n = -n;
                    shadePos -= weights[k] * min(0.0f, dot(hp - vertexPos[k], n)) * n;
                }
            }

            GpuMaterialRange materialRange = ResolveMaterialRange(info, primIdx);
            float textureLod = saturate(log2(max(t2 * 0.25f, 1e-3f)));
            float3 hitAlbedo = ApplyColorLeakIntensity(ResolveHitAlbedo(materialRange, uv, textureLod));
            radiance += throughput * EvaluateSecondaryDirect(shadePos, worldNormal, worldGeoNormal, hitAlbedo);

            throughput *= hitAlbedo;
            if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3f)
                break;

            if (bounce + 1u >= bounceCount)
                break;

            float2 bounceXi = float2(ProbeRandFloat(rng), ProbeRandFloat(rng));
            float3 bounceDir = CosineSampleHemisphere(bounceXi, worldNormal);

            const float3 nextOrigin = shadePos + worldGeoNormal * 0.01f + bounceDir * 0.01f;
            if (!IsFinitePosition(nextOrigin) || !IsFinitePosition(bounceDir))
                break;

            rayOrigin = nextOrigin;
            rayDir = bounceDir;
        }

        // A black path is a real answer - the hit is in shadow, or the ray
        // never left the geometry. It used to be replaced with sky radiance,
        // which turned every shadowed direction into open sky and was the
        // single largest source of light in an enclosed scene.
        radiance = SanitizeRadiance(radiance);

        SHAddSample(freshSH, dir, radiance, weight);
    }

    // Validity rides in the one spare SH slot (see ProbeSH). It is blended
    // along with the coefficients, so a probe the grid scrolls into a wall
    // fades out over a few frames instead of popping.
    const float backfaceFraction = float(backfaceHits) / float(max(g_RaysPerProbe, 1u));
    freshSH.c[6].w = (backfaceFraction > PTERO_PROBE_MAX_BACKFACE_FRACTION) ? 0.0f : 1.0f;

    // A probe that has just scrolled in has no history. Starting it from this
    // frame's rays alone made the leading face of the grid a slab of noise
    // that settled over a dozen frames, and a camera that kept moving kept
    // dragging fresh slabs across the view. Its neighbour one cell inward has
    // converged and usually sees much the same light, so it starts from that
    // and moves quickly toward its own rays. A layout change or a teleport
    // shifts by the whole grid (see UploadConstants) and leaves no neighbour
    // worth borrowing from.
    const bool canSeed = !hasHistory
        && g_FrameIndex != 0
        && all(abs(historyShift) < gridSize);

    ProbeSH outSH;
    if (hasHistory || canSeed)
    {
        // prevIdx was clamped into the grid, so for a newcomer it is the
        // neighbour one cell inward.
        ProbeSH prevSH = t_ProbeSH_Prev[prevIdx];
        float blend = hasHistory ? saturate(g_UpdateBlend) : max(saturate(g_UpdateBlend), 0.5f);
        [unroll] for (int c2 = 0; c2 < 7; ++c2)
            outSH.c[c2] = lerp(prevSH.c[c2], freshSH.c[c2], blend);

        // Whether this probe is inside a wall is its own question; the
        // neighbour's answer says nothing about it.
        if (!hasHistory)
            outSH.c[6].w = freshSH.c[6].w;
    }
    else
    {
        [unroll] for (int c2 = 0; c2 < 7; ++c2)
            outSH.c[c2] = freshSH.c[c2];
    }

    u_ProbeSH[probeIdx] = outSH;
}
