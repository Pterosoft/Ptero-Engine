// RtGI_RayGen.hlsl  –  cs_6_5  (compiled by DXC at runtime; IntelliSense warnings about RayQuery are expected)
// RTGI Pass 0: inline ray-traced GI using RayQuery.
//
// For each G-Buffer pixel:
//   1. Read world-position and normal from the G-Buffer.
//   2. Fire g_RaysPerPixel cosine-weighted hemisphere rays via RayQuery against the TLAS.
//   3. On a hit, optionally perform next-event estimation toward the sun and shade the secondary surface.
//   4. On a miss, return sky radiance.
//   5. Accumulate into a GIReservoir (WRS/RIS) and write reservoir + raw radiance output.

#include "RtGI_Common.hlsli"

// ─── Geometry buffers for secondary-hit normal interpolation ─────────────────
// These mirror the pools uploaded by RtGlobalIllumination::BuildTlas each frame.
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

StructuredBuffer<GpuPackedVertex> t_Vertices     : register(t3);
StructuredBuffer<uint>            t_Indices       : register(t4);
StructuredBuffer<GpuInstanceInfo> t_InstanceInfo  : register(t5);
StructuredBuffer<GpuMaterialRange> t_MaterialRanges : register(t6);
Texture2D<float4> t_Albedo      : register(t0);   // RGB albedo
Texture2D<float4> t_NormalDepth : register(t1);   // xy: oct-encoded normal, z: depth

// ─── TLAS ─────────────────────────────────────────────────────────────────────
RaytracingAccelerationStructure t_TLAS : register(t2);

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWStructuredBuffer<PackedGIReservoir> u_Reservoir : register(u0);
RWTexture2D<float4>                   u_GIOutput   : register(u1);

// ─────────────────────────────────────────────────────────────────────────────
// Simple diffuse shade at a secondary hit.
// Uses the sun as a directional light and the sky as uniform ambient.
// ─────────────────────────────────────────────────────────────────────────────
bool TraceShadowRay(float3 origin, float3 dir, float maxDistance)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}

    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

float3 ResolveHitAlbedo(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = t_MaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return float3(range.baseColorR, range.baseColorG, range.baseColorB);
    }

    return float3(1.0f, 1.0f, 1.0f);
}

float3 ApplyColorLeakIntensity(float3 albedo)
{
    const float luminance = max(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    return saturate(luminance.xxx + (albedo - luminance.xxx) * max(g_ColorLeakIntensity, 0.0f));
}

float3 EvaluateSecondaryPointLights(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    float3 pointLightSum = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int i = 0; i < min(g_NumPointLights, MAX_RTGI_POINT_LIGHTS); ++i)
    {
        // Resolve the emitter shape first: for a rect this moves the sample to
        // the nearest point on the panel, and for a spot it bounds the bounce
        // to the cone so indirect light cannot spill where direct light does
        // not reach.
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

        float pointNdotL = abs(dot(hitNormal, lightDir));
        float geoPointNdotL = abs(dot(hitGeoNormal, lightDir));
        if (pointNdotL <= 0.0f || geoPointNdotL <= 0.0f)
            continue;

        float visibility = 1.0f;
        if (g_NextEventEstimation != 0)
        {
            const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoPointNdotL, 0.05f)));
            const float maxShadowDistance = max(lightDistance - shadowBias * 2.0f, 0.0f);
            if (maxShadowDistance > 0.0f)
            {
                const float3 shadowOrigin = hitPos + hitGeoNormal * shadowBias + lightDir * shadowBias;
                visibility = TraceShadowRay(shadowOrigin, lightDir, maxShadowDistance) ? 1.0f : 0.0f;
                visibility = lerp(0.35f, 1.0f, visibility);
            }
        }

        const float falloff = (1.0f - normDistSq) * (1.0f - normDistSq) * shape.ShapeMask;
        pointLightSum += hitAlbedo * g_PointLights[i].Color * pointNdotL * falloff * visibility;
    }

    return pointLightSum;
}

float3 EvaluateSecondaryDirect(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo)
{
    // Sun: lambertian diffuse. g_SunDir points FROM sun TO scene, so negate for dot.
    const float3 sunDir = -g_SunDir;
    float sunNdotL = abs(dot(hitNormal, sunDir));
    float geoSunNdotL = abs(dot(hitGeoNormal, sunDir));
    float sunVisibility = 1.0f;
    if (g_NextEventEstimation != 0 && sunNdotL > 0.0f && geoSunNdotL > 0.0f)
    {
        // Use the geometric normal for the NEE visibility test to avoid the shadow
        // terminator problem from smooth shading normals falsely pushing the shadow
        // ray under the actual triangle surface.
        const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoSunNdotL, 0.05f)));
        const float3 shadowOrigin = hitPos + sunDir * shadowBias;
        sunVisibility = TraceShadowRay(shadowOrigin, sunDir, max(1e4f - shadowBias * 2.0f, 1.0f)) ? 1.0f : 0.0f;

        // The existing RTGI secondary shading is a heuristic, not a full MIS path tracer.
        // Hard binary NEE visibility removes too much energy relative to the legacy unshadowed
        // bounce approximation, so keep some fill from the old model instead of going fully black.
        sunVisibility = lerp(0.35f, 1.0f, sunVisibility);
    }
    return hitAlbedo * g_SunColor * sunNdotL * sunVisibility
         + EvaluateSecondaryPointLights(hitPos, hitNormal, hitGeoNormal, hitAlbedo);
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
// Cosine-weighted hemisphere sample in world space around N.
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// Trace a single GI ray with inline RayQuery (cs_6_5).
// Returns shaded radiance arriving at the ray origin from the hit or sky.
// ─────────────────────────────────────────────────────────────────────────────
struct RayResult
{
    float3 hitPos;    // world-space hit position (or far point on miss)
    float3 normal;    // surface normal at the hit
    float3 geoNormal; // geometric triangle normal at the hit
    float3 radiance;  // outgoing radiance toward the ray origin
    float  hitDist;   // ray distance used by NRD RELAX (stored in GI alpha)
};

RayResult TraceGIRay(float3 origin, float3 dir, inout uint rngState)
{
    RayResult result;
    result.hitPos = origin + dir * 1e4f;
    result.normal = -dir;
    result.geoNormal = -dir;
    result.radiance = float3(0.0f, 0.0f, 0.0f);
    result.hitDist = 1e4f;

    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 rayOrigin = origin;
    float3 rayDir = dir;
    bool recordedPrimaryHit = false;
    const uint bounceCount = max((uint)g_MaxBounces, 1u);

    [loop]
    for (uint bounce = 0; bounce < bounceCount; ++bounce)
    {
        RayDesc ray;
        ray.Origin    = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin      = 0.001f;
        ray.TMax      = 1e4f;

    RayQuery<0> rq;
    rq.TraceRayInline(t_TLAS, 0, 0xFF, ray);
        while (rq.Proceed()) {}

        if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            if (!recordedPrimaryHit)
            {
                result.hitPos = rayOrigin + rayDir * ray.TMax;
                result.normal = -rayDir;
                result.geoNormal = -rayDir;
                result.hitDist = ray.TMax;
            }

            result.radiance += throughput * SkyRadiance(rayDir);
            break;
        }

        float t = rq.CommittedRayT();
        float3 hitPos = rayOrigin + rayDir * t;

        uint instIdx  = rq.CommittedInstanceIndex();
        uint primIdx  = rq.CommittedPrimitiveIndex();
        float2 bary   = rq.CommittedTriangleBarycentrics();

        GpuInstanceInfo info = t_InstanceInfo[instIdx];

        uint i0 = t_Indices[info.indexOffset + primIdx * 3 + 0];
        uint i1 = t_Indices[info.indexOffset + primIdx * 3 + 1];
        uint i2 = t_Indices[info.indexOffset + primIdx * 3 + 2];

        GpuPackedVertex v0 = t_Vertices[info.vertexOffset + i0];
        GpuPackedVertex v1 = t_Vertices[info.vertexOffset + i1];
        GpuPackedVertex v2 = t_Vertices[info.vertexOffset + i2];

        float3 localPos0 = float3(v0.px, v0.py, v0.pz);
        float3 localPos1 = float3(v1.px, v1.py, v1.pz);
        float3 localPos2 = float3(v2.px, v2.py, v2.pz);

        float b0 = 1.0f - bary.x - bary.y;
        float3 localNormal = b0 * float3(v0.nx, v0.ny, v0.nz)
                           + bary.x * float3(v1.nx, v1.ny, v1.nz)
                           + bary.y * float3(v2.nx, v2.ny, v2.nz);

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
        // A sliver triangle collapses this cross product to zero and a plain
        // normalize would return NaN, which then becomes the next bounce's ray
        // origin and direction. Fall back to facing the incoming ray: wrong,
        // but bounded, and it keeps the path alive instead of poisoning it.
        float3 geoNormal = SafeNormalizeOr(
            cross(worldPos1 - worldPos0, worldPos2 - worldPos0), -rayDir);

        // Zero-length authored vertex normals get the same treatment, falling
        // back to the geometric normal, which is the best available answer.
        float3 worldNormal = SafeNormalizeOr(mul(o2wRot, localNormal), geoNormal);

        float3 shadingNormal = dot(worldNormal, -rayDir) >= 0.0f ? worldNormal : -worldNormal;
        float3 shadingGeoNormal = dot(geoNormal, -rayDir) >= 0.0f ? geoNormal : -geoNormal;

        if (!recordedPrimaryHit)
        {
            result.hitPos = hitPos;
            result.normal = shadingNormal;
            result.geoNormal = shadingGeoNormal;
            result.hitDist = t;
            recordedPrimaryHit = true;
        }

        float3 hitAlbedo = ApplyColorLeakIntensity(ResolveHitAlbedo(info, primIdx));
        result.radiance += throughput * EvaluateSecondaryDirect(hitPos, shadingNormal, shadingGeoNormal, hitAlbedo);

        throughput *= hitAlbedo;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3f)
            break;

        if (bounce + 1u >= bounceCount)
            break;

        float2 xi = float2(RandFloat(rngState), RandFloat(rngState));
        float3 bounceDir = CosineSampleHemisphere(xi, shadingNormal);

        const float3 nextOrigin = hitPos + shadingGeoNormal * 0.01f + bounceDir * 0.01f;

        // Last line of defence before spawning the next ray: a non-finite
        // origin or direction makes TraceRayInline undefined, and whatever it
        // returns would be accumulated as radiance. Stopping the path here
        // costs one bounce; continuing can cost the whole surface.
        if (!IsFinitePosition(nextOrigin) || !IsFinitePosition(bounceDir))
            break;

        rayOrigin = nextOrigin;
        rayDir = bounceDir;
    }

    // The path is only worth what it can be trusted to be.
    result.radiance = SanitizeRadiance(result.radiance);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    // ── Read G-Buffer ────────────────────────────────────────────────────────
    const float4 normalDepthSample = t_NormalDepth.Load(int3(pixel, 0));

    const float  depth  = normalDepthSample.z;

    // Sky pixels have no GI contribution.
    [branch]
    if (depth <= 0.0f || depth >= 1.0f)
    {
        u_GIOutput[pixel] = float4(0, 0, 0, 0);
        u_Reservoir[PixelIndex(pixel)] = PackReservoir(EmptyReservoir());
        return;
    }

    // Decode oct-encoded normal from the G-Buffer xy channels.
    float2 octN = normalDepthSample.xy * 2.0f - 1.0f;
    float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    float3 surfaceNormal = normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));

    const float3 worldPos = ReconstructWorldPos(pixel, depth);

    // ── Diagnostics ──────────────────────────────────────────────────────────
    // Debug views >= 10 replace the GI signal with one input to it, so a
    // flicker can be attributed to a stage instead of guessed at. Each writes a
    // full reservoir so the downstream passes stay well-formed.
    //
    // Read them with the RTGI denoiser OFF and the composite debug view set to
    // raw radiance, otherwise the denoiser will filter the very instability
    // being looked for.
    //
    //   10 - reconstructed world position, fractional part. Flickering here
    //        means the G-Buffer depth or g_ViewProjInv disagree between frames.
    //   11 - G-Buffer surface normal. Flickering means the G-Buffer itself is
    //        unstable and RTGI is only the messenger.
    //   12 - raw G-Buffer depth, steeply remapped so small changes are visible.
    //   13 - flat white. Nothing here depends on any per-frame input, so if
    //        THIS flickers the fault is not in ray generation at all - it is in
    //        dispatch, binding, or how the output is consumed.
    //   14 - deterministic probe. Traces ONE ray straight along the surface
    //        normal from a fixed rng seed, so the result depends only on where
    //        the surface is, never on the camera or the frame. Ordinary GI
    //        sampling cannot be judged by eye under motion - the rng is seeded
    //        per pixel, so a surface point that moves to a new pixel legitimately
    //        draws new directions, and that variance is the denoiser's job to
    //        absorb. This view removes it. Anything that still flickers here is
    //        real instability in traversal, hit attributes, or shading.
    [branch]
    if (g_DebugView >= 10)
    {
        float3 diagnostic = float3(1.0f, 1.0f, 1.0f);
        if (g_DebugView == 10)
            diagnostic = frac(worldPos);
        else if (g_DebugView == 11)
            diagnostic = surfaceNormal * 0.5f + 0.5f;
        else if (g_DebugView == 12)
        {
            // Linear distance from the camera, black at 0 m and white at 30 m -
            // a plain ramp, deliberately not frac(). A repeating fractional
            // pattern exposes quantisation well but reads as arbitrary stripes,
            // which is worse than useless when the question is "does this look
            // right". Quantisation still shows here, as visible steps in what
            // should be a smooth gradient.
            diagnostic = saturate(length(worldPos - g_CameraPos) / 30.0f).xxx;
        }
        else if (g_DebugView == 14)
        {
            // Constant seed, not pixel-derived: the whole point is that nothing
            // here varies with the camera or the frame index.
            uint probeRng = 0x9e3779b9u;
            const RayResult probe = TraceGIRay(
                worldPos + surfaceNormal * 0.002f, surfaceNormal, probeRng);
            diagnostic = probe.radiance;
        }

        u_GIOutput[pixel] = float4(diagnostic, 1.0f);

        GIReservoir diagnosticReservoir = EmptyReservoir();
        diagnosticReservoir.position  = worldPos;
        diagnosticReservoir.normal    = surfaceNormal;
        diagnosticReservoir.radiance  = diagnostic;
        diagnosticReservoir.weightSum = 1.0f;
        diagnosticReservoir.M         = 1;
        u_Reservoir[PixelIndex(pixel)] = PackReservoir(diagnosticReservoir);
        return;
    }

    // ── Fire GI rays ─────────────────────────────────────────────────────────
    uint rng = InitRng(pixel, g_FrameIndex);
    GIReservoir reservoir = EmptyReservoir();

    const uint numRays = clamp(g_RaysPerPixel, 1u, 8u);
    float3 giRadianceSum = float3(0, 0, 0);
    float  giHitDistSum  = 0.0f;

    [loop]
    for (uint i = 0; i < numRays; ++i)
    {
        float2 xi     = float2(RandFloat(rng), RandFloat(rng));
        float3 rayDir = CosineSampleHemisphere(xi, surfaceNormal);

        // Offset ray origin along the surface normal to avoid self-intersection.
        RayResult hit = TraceGIRay(worldPos + surfaceNormal * 0.002f, rayDir, rng);

        // Keep the RTGI / NRD signal demodulated from the primary-surface albedo.
        // This lets the denoiser smooth indirect lighting without blurring texture detail.
        // The primary albedo is applied later in deferred lighting.
        // Sanitize before anything else looks at it. Past this point the value
        // enters a reservoir that neighbouring pixels and later frames resample
        // from, so a bad sample admitted here does not stay local - it spreads.
        float3 radiance = SanitizeRadiance(hit.radiance);

        // Clamp to suppress fireflies. Note min() would not have filtered a NaN
        // on its own, which is why the sanitize above has to come first.
        [flatten]
        if (g_RadianceClamp > 0.0f)
            radiance = min(radiance, g_RadianceClamp);

        // RIS weight: target PDF = luminance, proposal PDF = cosine/pi (cancels with sample weight).
        float targetPdf = Luminance(radiance) * 3.14159265f;
        float risWeight = (targetPdf > 0.0f) ? (targetPdf / float(numRays)) : 0.0f;

        GIReservoir candidate;
        // Store the primary visible surface data in the reservoir so temporal
        // reprojection tracks the shaded pixel, not the selected secondary hit.
        // Reprojecting the bounce hit makes history camera-dependent and causes
        // dark speckles when the camera moves or rotates.
        candidate.position  = worldPos;
        candidate.normal    = surfaceNormal;
        candidate.radiance  = radiance;
        candidate.weightSum = 0.0f;
        candidate.M         = 1;
        candidate.age       = 0;

        UpdateReservoir(reservoir, candidate, risWeight, rng);
        giRadianceSum += radiance;
        giHitDistSum  += hit.hitDist;
    }

    // Finalize reservoir weight.
    float finalTargetPdf = Luminance(reservoir.radiance) * 3.14159265f;
    FinalizeReservoir(reservoir, finalTargetPdf);

    u_Reservoir[PixelIndex(pixel)] = PackReservoir(reservoir);

    // Write average GI radiance estimate as the initial (pre-ReSTIR) output.
    // Alpha carries the average hit distance so NRD RELAX can use a real guide signal
    // instead of the old placeholder value of 1.0 for every pixel.
    u_GIOutput[pixel] = float4(giRadianceSum / float(numRays), giHitDistSum / float(numRays));
}

