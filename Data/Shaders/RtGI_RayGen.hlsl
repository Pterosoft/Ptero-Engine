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

        // One-sided: hitNormal faces the ray that found this surface, so only light
        // arriving on that side counts. This was abs(), which lit the far side of every
        // closed mesh; with NEE its shadow ray then flipped between blocked and clear
        // facet by facet along the terminator, which is the blocky pattern in the GI.
        float pointNdotL = saturate(dot(hitNormal, lightDir));
        if (pointNdotL <= 0.0f)
            continue;
        // Only sizes the shadow-ray bias.
        float geoPointNdotL = max(abs(dot(hitGeoNormal, lightDir)), 1e-3f);

        float visibility = 1.0f;
        if (g_NextEventEstimation != 0)
        {
            const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoPointNdotL, 0.05f)));
            const float maxShadowDistance = max(lightDistance - shadowBias * 2.0f, 0.0f);
            if (maxShadowDistance > 0.0f)
            {
                // Offset along the geometric normal on the side the light is on. The
                // unsigned normal pushed the origin *into* the surface for lights behind it.
                const float3 lightSideNormal = hitGeoNormal * (dot(hitGeoNormal, lightDir) >= 0.0f ? 1.0f : -1.0f);
                const float3 shadowOrigin = hitPos + lightSideNormal * shadowBias + lightDir * shadowBias;
                visibility = TraceShadowRay(shadowOrigin, lightDir, maxShadowDistance) ? 1.0f : 0.0f;
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
    // One-sided, as for the point lights above.
    float sunNdotL = saturate(dot(hitNormal, sunDir));
    float geoSunNdotL = max(abs(dot(hitGeoNormal, sunDir)), 1e-3f);
    float sunVisibility = 1.0f;
    if (g_NextEventEstimation != 0 && sunNdotL > 0.0f)
    {
        // Use the geometric normal for the NEE visibility test to avoid the shadow
        // terminator problem from smooth shading normals falsely pushing the shadow
        // ray under the actual triangle surface.
        const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoSunNdotL, 0.05f)));
        // Offset along the geometric normal (on the sun's side) as well as toward the sun;
        // stepping only along sunDir leaves the origin under the triangle at grazing angles
        // on dense meshes, so the ray hits its own surface and visibility turns to noise.
        const float3 sunSideNormal = hitGeoNormal * (dot(hitGeoNormal, sunDir) >= 0.0f ? 1.0f : -1.0f);
        const float3 shadowOrigin = hitPos + sunSideNormal * shadowBias + sunDir * shadowBias;
        sunVisibility = TraceShadowRay(shadowOrigin, sunDir, max(1e4f - shadowBias * 2.0f, 1.0f)) ? 1.0f : 0.0f;

        // Binary: a blocked hit gets none of this light. There used to be a 0.35 floor here
        // to keep "energy" the unshadowed model had - but most of that energy was the far
        // side of every mesh being lit through abs(NdotL). With one-sided shading the floor
        // only leaked light into places the light cannot reach, and it is why toggling NEE
        // looked like it did nothing.
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

        // Lift the hit onto the smooth surface its vertex normals describe (Hanika,
        // "Hacking the Shadow Terminator"), so NEE shadow rays and the next bounce leave
        // from the curved surface the shading assumes rather than the flat facet, whose
        // neighbours would otherwise shadow it triangle by triangle.
        float3 shadePos = hitPos;
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
                if (dot(n, shadingGeoNormal) < 0.0f)
                    n = -n;
                shadePos -= weights[k] * min(0.0f, dot(hitPos - vertexPos[k], n)) * n;
            }
        }

        if (!recordedPrimaryHit)
        {
            result.hitPos = hitPos;
            result.normal = shadingNormal;
            result.geoNormal = shadingGeoNormal;
            result.hitDist = t;
            recordedPrimaryHit = true;
        }

        float3 hitAlbedo = ApplyColorLeakIntensity(ResolveHitAlbedo(info, primIdx));
        result.radiance += throughput * EvaluateSecondaryDirect(shadePos, shadingNormal, shadingGeoNormal, hitAlbedo);

        throughput *= hitAlbedo;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3f)
            break;

        if (bounce + 1u >= bounceCount)
            break;

        float2 xi = float2(RandFloat(rngState), RandFloat(rngState));
        float3 bounceDir = CosineSampleHemisphere(xi, shadingNormal);

        const float3 nextOrigin = shadePos + shadingGeoNormal * 0.01f + bounceDir * 0.01f;

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
// Where primary GI rays start, and the real plane they must stay above.
//
// The G-Buffer normal is the *shading* normal: interpolated vertex normals plus the normal
// map. On a mesh whose facets are coarse relative to its curvature it tilts away from the
// actual triangle, so rays leaving a flat facet near its edges run straight into the
// neighbouring facet and come back occluded - per-triangle banding in the GI, the
// "shadow terminator" problem applied to indirect light.
//
// Fix, after Hanika, "Hacking the Shadow Terminator" (Ray Tracing Gems II, ch. 4): find
// the actual triangle under the pixel with one short ray from the camera, then lift the
// point onto the smooth surface its vertex normals describe - project it onto each
// vertex's tangent plane (only ever outward) and blend by barycentrics. From there the
// neighbouring facets no longer stick up in front of the ray. The triangle's own normal
// also gives an exact geometric plane to keep every ray direction above. Depth-derived
// normals are not a substitute: device depth near 1.0 is far too coarse for them.
//
// Falls back to the old shading-normal offset when the camera ray does not land on the
// G-Buffer surface (alpha-tested or otherwise mismatched geometry).
void ComputePrimaryRayOrigin(float3 worldPos, float3 shadingNormal,
                             out float3 origin, out float3 geoNormal)
{
    origin = worldPos + shadingNormal * 0.002f;
    geoNormal = shadingNormal;

    const float3 toSurface = worldPos - g_CameraPos;
    const float  surfaceDistance = length(toSurface);
    if (surfaceDistance < 1e-4f)
        return;

    // How far the G-Buffer point and the traced hit may disagree: depth precision and
    // TAA jitter both grow with distance.
    const float tolerance = max(0.01f, surfaceDistance * 0.01f);

    RayDesc ray;
    ray.Origin    = g_CameraPos;
    ray.Direction = toSurface / surfaceDistance;
    ray.TMin      = max(surfaceDistance - tolerance, 0.0f);
    ray.TMax      = surfaceDistance + tolerance;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    while (rq.Proceed()) {}
    if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return;

    const GpuInstanceInfo info = t_InstanceInfo[rq.CommittedInstanceIndex()];
    const uint primIdx = rq.CommittedPrimitiveIndex();
    const float2 bary = rq.CommittedTriangleBarycentrics();
    const float3 b = float3(1.0f - bary.x - bary.y, bary.x, bary.y);

    const float3x4 o2w = rq.CommittedObjectToWorld3x4();
    const float3x3 o2wRot = float3x3(
        float3(o2w[0][0], o2w[1][0], o2w[2][0]),
        float3(o2w[0][1], o2w[1][1], o2w[2][1]),
        float3(o2w[0][2], o2w[1][2], o2w[2][2]));

    float3 vertexPos[3];
    float3 vertexNormal[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const GpuPackedVertex v = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + primIdx * 3 + k]];
        const float4 local = float4(v.px, v.py, v.pz, 1.0f);
        vertexPos[k] = float3(dot(o2w[0], local), dot(o2w[1], local), dot(o2w[2], local));
        vertexNormal[k] = mul(o2wRot, float3(v.nx, v.ny, v.nz));
    }

    float3 faceNormal = cross(vertexPos[1] - vertexPos[0], vertexPos[2] - vertexPos[0]);
    const float faceLengthSq = dot(faceNormal, faceNormal);
    if (faceLengthSq < 1e-20f)
        return;
    faceNormal *= rsqrt(faceLengthSq);
    // Face the camera side: that is the side the G-Buffer pixel shows.
    if (dot(faceNormal, ray.Direction) > 0.0f)
        faceNormal = -faceNormal;

    const float3 hitPos = ray.Origin + ray.Direction * rq.CommittedRayT();

    // Hanika's offset: P' = P - sum(b_i * min(0, dot(P - V_i, n_i)) * n_i).
    float3 lifted = hitPos;
    [unroll]
    for (uint j = 0; j < 3; ++j)
    {
        float3 n = vertexNormal[j];
        const float nLengthSq = dot(n, n);
        if (nLengthSq < 1e-12f)
            continue;
        n *= rsqrt(nLengthSq);
        if (dot(n, faceNormal) < 0.0f)
            n = -n;
        lifted -= b[j] * min(0.0f, dot(hitPos - vertexPos[j], n)) * n;
    }

    geoNormal = faceNormal;
    // A small epsilon on top, scaled with distance for floating-point headroom.
    origin = lifted + faceNormal * max(0.0005f, surfaceDistance * 1e-4f);
}

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
        else if (g_DebugView == 15)
        {
            // NEE sun visibility, fired from the visible surface with exactly the shadow
            // ray a bounce hit uses. Green: faces the sun and reaches it. Red: faces the
            // sun but the ray is blocked. Black: faces away. Compare against the shadow-
            // mapped sun in the lit view - red where the raster sun is lit means the RT
            // scene holds a blocker the raster one does not.
            float3 origin, primaryGeoNormal;
            ComputePrimaryRayOrigin(worldPos, surfaceNormal, origin, primaryGeoNormal);
            const float3 sunDir = -g_SunDir;
            diagnostic = float3(0.0f, 0.0f, 0.0f);
            if (dot(surfaceNormal, sunDir) > 0.0f)
            {
                const float geoSunNdotL = max(abs(dot(primaryGeoNormal, sunDir)), 1e-3f);
                const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoSunNdotL, 0.05f)));
                const float3 sunSideNormal = primaryGeoNormal * (dot(primaryGeoNormal, sunDir) >= 0.0f ? 1.0f : -1.0f);
                const float3 shadowOrigin = origin + sunSideNormal * shadowBias + sunDir * shadowBias;
                const bool visible = TraceShadowRay(shadowOrigin, sunDir, max(1e4f - shadowBias * 2.0f, 1.0f));
                diagnostic = visible ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
            }
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

    // ── Primary ray origin ───────────────────────────────────────────────────
    // Lifted onto the smooth surface; see ComputePrimaryRayOrigin for why.
    float3 rayOrigin, geoNormal;
    ComputePrimaryRayOrigin(worldPos, surfaceNormal, rayOrigin, geoNormal);

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

        // A direction below the real surface would only ever hit this surface. Mirror
        // it back above the geometric plane instead of discarding it, which keeps the
        // sample count and roughly preserves the lobe around the shading normal.
        const float belowSurface = dot(rayDir, geoNormal);
        if (belowSurface < 0.0f)
            rayDir = normalize(rayDir - 2.0f * belowSurface * geoNormal);

        // Origin offset along the geometric normal (see above) to avoid self-intersection.
        RayResult hit = TraceGIRay(rayOrigin, rayDir, rng);

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

