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
#include "RtGI_Vsm.hlsli"
#include "RtTransform.hlsli"
#include "RtMaterialSurface.hlsli"
#include "RtGI_RayDebug.hlsli"

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
    float terrainReach;   // GpuMaterialRange::terrainReach
};

StructuredBuffer<GpuPackedVertex> t_Vertices     : register(t3);
StructuredBuffer<uint>            t_Indices       : register(t4);
StructuredBuffer<GpuInstanceInfo> t_InstanceInfo  : register(t5);
StructuredBuffer<GpuMaterialRange> t_MaterialRanges : register(t6);
Texture2D<float4> t_Albedo      : register(t0);   // RGB albedo
Texture2D<float4> t_NormalDepth : register(t1);   // xy: oct-encoded normal, z: depth

// ─── TLAS ─────────────────────────────────────────────────────────────────────
RaytracingAccelerationStructure t_TLAS : register(t2);

// ─── Base-colour textures for secondary hits ─────────────────────────────────
// The table RtGlobalIllumination::RefreshMaterialTextureDescriptors fills; the
// radiance probes and the specular pass sample the same one.
Texture2D<float4> t_BaseTextures[32] : register(t7);
SamplerState      gLinearWrapSampler : register(s0);

// Tint and texture averages, one per material range (RtMaterialSurface.hlsli).
StructuredBuffer<GpuMaterialSurface> t_MaterialSurfaces : register(t42);

static const uint INVALID_TEXTURE_INDEX = 0xffffffffu;

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWStructuredBuffer<PackedGIReservoir> u_Reservoir : register(u0);
RWTexture2D<float4>                   u_GIOutput   : register(u1);

// Ray visualisation capture; only touched while g_RayDebugWrite is set.
RWStructuredBuffer<RtGIDebugRaySegment> u_DebugRays : register(u2);

void RecordRaySegment(uint slot, uint kind, uint bounce, float3 start, float3 end, float3 light, float3 albedo)
{
    RtGIDebugRaySegment segment;
    segment.start  = start;
    segment.kind   = kind;
    segment.end    = end;
    segment.bounce = bounce;
    segment.light  = light;
    segment._pad0  = 0.0f;
    segment.albedo = albedo;
    segment._pad1  = 0.0f;
    u_DebugRays[slot] = segment;
}

// The first slot this trace pixel records into, or kRayDebugNoSlot when none of the
// recorded frame pixels lies in its block this frame. At reduced resolution a block
// holds at most one of them (the grid spacing is wider than any block), and the trace
// pixel records its own path in that pixel's place.
uint RayDebugPixelSlot(uint2 tracePixel)
{
    if (g_RayDebugWrite == 0u)
        return kRayDebugNoSlot;

    const uint2 blockStart = tracePixel * g_TraceScale;
    const uint2 blockEnd   = blockStart + g_TraceScale;   // exclusive

    if (g_RayDebugPattern == 1u)
    {
        const uint2 target = uint2(g_FrameWidth, g_FrameHeight) / 2u;
        return all(target >= blockStart) && all(target < blockEnd) ? g_RayDebugSlotBase : kRayDebugNoSlot;
    }

    // Recorded pixels sit at cell * spacing + spacing / 2; take the last one at or
    // before the block's end and see whether it is inside the block.
    const uint spacing = max(g_RayDebugSpacing, 1u);
    const uint halfSpacing = spacing / 2u;
    if (any(blockEnd <= halfSpacing))
        return kRayDebugNoSlot;
    const uint2 cell = (blockEnd - 1u - halfSpacing) / spacing;
    if (any(cell * spacing + halfSpacing < blockStart))
        return kRayDebugNoSlot;
    return g_RayDebugSlotBase + (cell.y * g_RayDebugGridWidth + cell.x) * g_RayDebugSlotsPerPixel;
}

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

// Base colour at a secondary hit: the tint times its texture, as the G-Buffer pass
// has it (PteroHitBaseColor). This used to return the factor alone, which for a
// textured material is usually white - every bounce off sandstone came back as if
// off white plaster - and then the factor times the texture, which counted the
// texture's average twice and made textured bounces darker and more saturated.
float3 ResolveHitAlbedo(GpuInstanceInfo info, uint primIdx, float2 uv, float lod)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = t_MaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
        {
            const bool textured = range.baseColorTextureIndex != INVALID_TEXTURE_INDEX;
            float3 textureColor = float3(1.0f, 1.0f, 1.0f);
            if (textured)
            {
                textureColor = t_BaseTextures[NonUniformResourceIndex(range.baseColorTextureIndex)]
                    .SampleLevel(gLinearWrapSampler, uv, lod).rgb;
            }
            return PteroHitBaseColor(t_MaterialSurfaces[info.materialRangeOffset + r], textured, textureColor);
        }
    }

    return float3(1.0f, 1.0f, 1.0f);
}

float3 ApplyColorLeakIntensity(float3 albedo)
{
    const float luminance = max(dot(albedo, float3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
    return saturate(luminance.xxx + (albedo - luminance.xxx) * max(g_ColorLeakIntensity, 0.0f));
}

// Light i's unshadowed contribution at a hit (before the albedo), or false when it
// adds nothing there.
bool SecondaryPointLightUnshadowed(uint i, float3 hitPos, float3 hitNormal,
                                   out float3 contribution, out float3 lightDir, out float lightDistance)
{
    contribution = float3(0.0f, 0.0f, 0.0f);
    lightDir = float3(0.0f, 0.0f, 1.0f);
    lightDistance = 0.0f;
    const PteroLightData light = RtgiSceneLight(i);

    // Resolve the emitter shape first: for a rect this moves the sample to
    // the nearest point on the panel, and for a spot it bounds the bounce
    // to the cone so indirect light cannot spill where direct light does
    // not reach.
    const PteroResolvedLight shape = PteroResolveLightShape(light, hitPos);
    if (shape.ShapeMask <= 0.0f)
        return false;

    const float3 toLight = shape.Position - hitPos;
    const float distSq = dot(toLight, toLight);
    const float normDistSq = saturate(distSq * light.InvRadiusSq);
    if (normDistSq >= 1.0f)
        return false;

    const float invDist = rsqrt(max(distSq, 1e-6f));
    lightDistance = rcp(invDist);
    lightDir = toLight * invDist;

    // One-sided: hitNormal faces the ray that found this surface, so only light
    // arriving on that side counts. This was abs(), which lit the far side of every
    // closed mesh; with NEE its shadow ray then flipped between blocked and clear
    // facet by facet along the terminator, which is the blocky pattern in the GI.
    const float pointNdotL = saturate(dot(hitNormal, lightDir));
    if (pointNdotL <= 0.0f)
        return false;

    const float falloff = PteroLightDistanceFalloff(light, lightDistance) * shape.ShapeMask;
    contribution = light.Color * pointNdotL * falloff;
    return any(contribution > 0.0f);
}

// Next-event visibility of light i from a hit, 1 = lit.
float SecondaryPointLightVisibility(uint i, float3 hitPos, float3 hitGeoNormal, float3 lightDir, float lightDistance)
{
    if (g_NextEventEstimation == 0)
        return 1.0f;

    // Only sizes the shadow-ray bias.
    const float geoPointNdotL = max(abs(dot(hitGeoNormal, lightDir)), 1e-3f);
    const float shadowBias = max(0.01f, 0.02f * rsqrt(max(geoPointNdotL, 0.05f)));
    const float maxShadowDistance = max(lightDistance - shadowBias * 2.0f, 0.0f);
    if (maxShadowDistance <= 0.0f)
        return 1.0f;

    // Offset along the geometric normal on the side the light is on. The
    // unsigned normal pushed the origin *into* the surface for lights behind it.
    const float3 lightSideNormal = hitGeoNormal * (dot(hitGeoNormal, lightDir) >= 0.0f ? 1.0f : -1.0f);
    // The shadow map first; the ray only where it has no fine enough page.
    float visibility = RtgiVsmLocalVisibility(gClusterLights[i], hitPos, lightSideNormal);
    if (visibility < 0.0f)
    {
        const float3 shadowOrigin = hitPos + lightSideNormal * shadowBias + lightDir * shadowBias;
        visibility = TraceShadowRay(shadowOrigin, lightDir, maxShadowDistance) ? 1.0f : 0.0f;
    }
    return visibility;
}

float3 EvaluateSecondaryPointLights(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo,
                                    inout uint rngState)
{
    float3 pointLightSum = float3(0.0f, 0.0f, 0.0f);

    // The lights the world grid cell around the hit lists (LightClusters.hlsli); a hit
    // off the grid gets every light.
    const uint cell = PteroGridCellBase(hitPos);

    // Stochastic light selection (RtGISettings::StochasticLights). Every light's
    // unshadowed contribution is cheap; its visibility - a shadow-map lookup or a ray -
    // is not. So with more lights in reach than samples, pick g_LightSamples of them in
    // proportion to their unshadowed luminance (one weighted reservoir per sample,
    // filled in a single pass) and test only those, each weighted by 1 / (probability x
    // samples). Unbiased; the noise it adds is only where lights of similar strength
    // overlap and one is blocked, and the GI denoiser absorbs most of it.
    [branch]
    if (g_NextEventEstimation != 0 && g_LightSamples > 0 && gLightClusters.LightCount > (uint)g_LightSamples)
    {
        const int samples = min(g_LightSamples, RTGI_MAX_LIGHT_SAMPLES);
        int picked[RTGI_MAX_LIGHT_SAMPLES] = { -1, -1, -1, -1 };
        float totalWeight = 0.0f;
        int candidates = 0;

        [loop]
        for (uint word = 0; word < gLightClusters.WordCount; ++word)
        {
          uint bits = PteroGridCellWord(cell, word);
          [loop]
          while (bits != 0u)
          {
            const uint i = word * 32u + firstbitlow(bits);
            bits &= bits - 1u;
            float3 contribution, lightDir;
            float lightDistance;
            if (!SecondaryPointLightUnshadowed(i, hitPos, hitNormal, contribution, lightDir, lightDistance))
                continue;
            const float weight = Luminance(contribution);
            if (weight <= 0.0f)
                continue;
            ++candidates;
            totalWeight += weight;
            [unroll]
            for (int s = 0; s < RTGI_MAX_LIGHT_SAMPLES; ++s)
            {
                if (s < samples && RandFloat(rngState) * totalWeight < weight)
                    picked[s] = (int)i;
            }
          }
        }

        // No more lights in reach here than samples: the exact loop below costs no more.
        [branch]
        if (candidates > samples)
        {
            [unroll]
            for (int s = 0; s < RTGI_MAX_LIGHT_SAMPLES; ++s)
            {
                if (s >= samples || picked[s] < 0)
                    continue;
                float3 contribution, lightDir;
                float lightDistance;
                SecondaryPointLightUnshadowed(picked[s], hitPos, hitNormal, contribution, lightDir, lightDistance);
                const float weight = Luminance(contribution);
                if (weight <= 0.0f)
                    continue;
                const float visibility = SecondaryPointLightVisibility(picked[s], hitPos, hitGeoNormal, lightDir, lightDistance);
                pointLightSum += hitAlbedo * contribution * (visibility * totalWeight / (weight * (float)samples));
            }
            return pointLightSum;
        }
    }

    [loop]
    for (uint word = 0; word < gLightClusters.WordCount; ++word)
    {
      uint bits = PteroGridCellWord(cell, word);
      [loop]
      while (bits != 0u)
      {
        const uint i = word * 32u + firstbitlow(bits);
        bits &= bits - 1u;
        float3 contribution, lightDir;
        float lightDistance;
        if (!SecondaryPointLightUnshadowed(i, hitPos, hitNormal, contribution, lightDir, lightDistance))
            continue;
        const float visibility = SecondaryPointLightVisibility(i, hitPos, hitGeoNormal, lightDir, lightDistance);
        pointLightSum += hitAlbedo * contribution * visibility;
      }
    }

    return pointLightSum;
}

// sunVisibilityTested: the sun's next-event visibility, or -1 when none was tested
// (NEE off, or the hit faces away from the sun) - for the ray visualisation.
float3 EvaluateSecondaryDirect(float3 hitPos, float3 hitNormal, float3 hitGeoNormal, float3 hitAlbedo,
                               inout uint rngState, out float sunVisibilityTested)
{
    // Sun: lambertian diffuse. g_SunDir points FROM sun TO scene, so negate for dot.
    const float3 sunDir = -g_SunDir;
    // One-sided, as for the point lights above.
    float sunNdotL = saturate(dot(hitNormal, sunDir));
    float geoSunNdotL = max(abs(dot(hitGeoNormal, sunDir)), 1e-3f);
    float sunVisibility = 1.0f;
    sunVisibilityTested = -1.0f;
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
        // The shadow map first; the ray only where it has no fine enough page.
        sunVisibility = RtgiVsmSunVisibility(hitPos, sunSideNormal);
        if (sunVisibility < 0.0f)
        {
            const float3 shadowOrigin = hitPos + sunSideNormal * shadowBias + sunDir * shadowBias;
            sunVisibility = TraceShadowRay(shadowOrigin, sunDir, max(1e4f - shadowBias * 2.0f, 1.0f)) ? 1.0f : 0.0f;
        }

        // Binary: a blocked hit gets none of this light. There used to be a 0.35 floor here
        // to keep "energy" the unshadowed model had - but most of that energy was the far
        // side of every mesh being lit through abs(NdotL). With one-sided shading the floor
        // only leaked light into places the light cannot reach, and it is why toggling NEE
        // looked like it did nothing.
        sunVisibilityTested = sunVisibility;
    }
    return hitAlbedo * g_SunColor * sunNdotL * sunVisibility
         + EvaluateSecondaryPointLights(hitPos, hitNormal, hitGeoNormal, hitAlbedo, rngState);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sky radiance for rays that miss all geometry: the sky that is drawn, sun- and
// moon-side brighter, rather than a flat colour (SkyModel.hlsli).
// ─────────────────────────────────────────────────────────────────────────────
float3 SkyRadiance(float3 dir)
{
    return PteroSkyRadiance(g_Sky, normalize(dir));
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

// debugSlot: where the ray visualisation records this path (bounce b at
// debugSlot + 2b, its shadow ray at + 1), or kRayDebugNoSlot.
RayResult TraceGIRay(float3 origin, float3 dir, inout uint rngState, uint debugSlot)
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

            const float3 sky = SkyRadiance(rayDir);
            result.radiance += throughput * sky;
            [branch]
            if (debugSlot != kRayDebugNoSlot)
                RecordRaySegment(debugSlot + bounce * 2u, kRayDebugBounceMiss, bounce,
                                 rayOrigin, rayOrigin + rayDir, sky, float3(0.0f, 0.0f, 0.0f));
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
        float2 uv = b0 * float2(v0.u, v0.v)
                  + bary.x * float2(v1.u, v1.v)
                  + bary.y * float2(v2.u, v2.v);
        float3 localNormal = b0 * float3(v0.nx, v0.ny, v0.nz)
                           + bary.x * float3(v1.nx, v1.ny, v1.nz)
                           + bary.y * float3(v2.nx, v2.ny, v2.nz);

        float3x4 o2w = rq.CommittedObjectToWorld3x4();
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
        float3 worldNormal = SafeNormalizeOr(PteroObjectToWorldNormal(o2w, localNormal), geoNormal);

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
                PteroObjectToWorldNormal(o2w, float3(v0.nx, v0.ny, v0.nz)),
                PteroObjectToWorldNormal(o2w, float3(v1.nx, v1.ny, v1.nz)),
                PteroObjectToWorldNormal(o2w, float3(v2.nx, v2.ny, v2.nz)) };
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

        // Same mip choice as the probes, so both GI modes see the same colour.
        float textureLod = saturate(log2(max(t * 0.25f, 1e-3f)));
        float3 hitAlbedo = ApplyColorLeakIntensity(ResolveHitAlbedo(info, primIdx, uv, textureLod));
        float sunVisibilityTested;
        const float3 hitDirect = EvaluateSecondaryDirect(shadePos, shadingNormal, shadingGeoNormal, hitAlbedo,
                                                         rngState, sunVisibilityTested);
        result.radiance += throughput * hitDirect;

        [branch]
        if (debugSlot != kRayDebugNoSlot)
        {
            RecordRaySegment(debugSlot + bounce * 2u, kRayDebugBounceHit, bounce,
                             rayOrigin, hitPos, hitDirect, hitAlbedo);
            if (sunVisibilityTested >= 0.0f)
                RecordRaySegment(debugSlot + bounce * 2u + 1u,
                                 sunVisibilityTested > 0.5f ? kRayDebugShadowLit : kRayDebugShadowBlocked, bounce,
                                 shadePos, shadePos - g_SunDir, g_SunColor * sunVisibilityTested, float3(0.0f, 0.0f, 0.0f));
        }

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
// The furthest the camera ray looks past the G-Buffer point for displaced terrain.
static const float kMaxTerrainReach = 2.0f;
static const uint  kMaterialTerrain = 4u;   // kRtMaterialFlagTerrain

// For a terrain hit, how far its tessellated surface can stand off the traced mesh;
// negative for anything else.
float TerrainReachAt(uint instanceIndex, uint primIdx)
{
    const GpuInstanceInfo info = t_InstanceInfo[instanceIndex];
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        const GpuMaterialRange range = t_MaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return (range.flags & kMaterialTerrain) != 0u ? max(range.terrainReach, 0.0f) : -1.0f;
    }
    return -1.0f;
}

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

    // The first thing along the camera ray, if it is what the pixel shows: within the
    // tolerance, or terrain within its displacement reach (displaced terrain stands
    // up to that far off the traced mesh, above or below). A window starting at the
    // G-Buffer depth skipped traced terrain in front of it and could snap to geometry
    // buried under it - an origin inside the terrain, whose GI is black.
    RayDesc ray;
    ray.Origin    = g_CameraPos;
    ray.Direction = toSurface / surfaceDistance;
    ray.TMin      = 0.0f;
    ray.TMax      = surfaceDistance + tolerance + kMaxTerrainReach;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS, RAY_FLAG_NONE, 0xFF, ray);
    while (rq.Proceed()) {}
    bool matched = false;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const float offset = abs(rq.CommittedRayT() - surfaceDistance);
        const float reach = TerrainReachAt(rq.CommittedInstanceIndex(), rq.CommittedPrimitiveIndex());
        matched = offset <= tolerance;
        if (!matched && reach >= 0.0f)
        {
            // The reach is along the surface normal; the ray crosses it at a slant.
            const GpuInstanceInfo hitInfo = t_InstanceInfo[rq.CommittedInstanceIndex()];
            const float3x4 hitO2w = rq.CommittedObjectToWorld3x4();
            float3 p[3];
            [unroll]
            for (uint k = 0; k < 3; ++k)
            {
                const GpuPackedVertex v = t_Vertices[hitInfo.vertexOffset
                    + t_Indices[hitInfo.indexOffset + rq.CommittedPrimitiveIndex() * 3 + k]];
                const float4 local = float4(v.px, v.py, v.pz, 1.0f);
                p[k] = float3(dot(hitO2w[0], local), dot(hitO2w[1], local), dot(hitO2w[2], local));
            }
            const float3 face = cross(p[1] - p[0], p[2] - p[0]);
            const float faceLengthSq = dot(face, face);
            if (faceLengthSq > 1e-20f)
                matched = offset <= tolerance + PteroTerrainReachAlongRay(reach, face * rsqrt(faceLengthSq), ray.Direction);
        }
    }
    if (!matched)
    {
        // Something traced but not drawn is in front: the window around the depth.
        ray.TMin = max(surfaceDistance - tolerance, 0.0f);
        ray.TMax = surfaceDistance + tolerance;
        rq.TraceRayInline(t_TLAS, RAY_FLAG_NONE, 0xFF, ray);
        while (rq.Proceed()) {}
        if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return;
    }

    const GpuInstanceInfo info = t_InstanceInfo[rq.CommittedInstanceIndex()];
    const uint primIdx = rq.CommittedPrimitiveIndex();
    const float2 bary = rq.CommittedTriangleBarycentrics();
    const float3 b = float3(1.0f - bary.x - bary.y, bary.x, bary.y);

    const float3x4 o2w = rq.CommittedObjectToWorld3x4();

    float3 vertexPos[3];
    float3 vertexNormal[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const GpuPackedVertex v = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + primIdx * 3 + k]];
        const float4 local = float4(v.px, v.py, v.pz, 1.0f);
        vertexPos[k] = float3(dot(o2w[0], local), dot(o2w[1], local), dot(o2w[2], local));
        vertexNormal[k] = PteroObjectToWorldNormal(o2w, float3(v.nx, v.ny, v.nz));
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
    // One thread per trace pixel; it shades the frame pixel `pixel` stands for, and
    // writes the reservoir and raw radiance at the trace pixel.
    const uint2 tracePixel = DTid.xy;
    if (any(tracePixel >= TraceSize()))
        return;
    const uint2 pixel = TraceToFramePixel(tracePixel);

    // ── Read G-Buffer ────────────────────────────────────────────────────────
    const float4 normalDepthSample = t_NormalDepth.Load(int3(pixel, 0));

    const float  depth  = normalDepthSample.z;

    // A recorded pixel empties its slots first, so a path that ends early - or
    // a pixel that shows sky - leaves nothing stale from the frame that last
    // wrote them.
    const uint debugPixelSlot = RayDebugPixelSlot(tracePixel);
    [branch]
    if (debugPixelSlot != kRayDebugNoSlot)
    {
        [loop]
        for (uint s = 0; s < g_RayDebugSlotsPerPixel; ++s)
            RecordRaySegment(debugPixelSlot + s, kRayDebugEmpty, 0u, 0.0f.xxx, 0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    }

    // Sky pixels have no GI contribution.
    [branch]
    if (depth <= 0.0f || depth >= 1.0f)
    {
        u_GIOutput[tracePixel] = float4(0, 0, 0, 0);
        u_Reservoir[PixelIndex(tracePixel)] = PackReservoir(EmptyReservoir());
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
        else if (g_DebugView == 16)
        {
            // Who answers the sun's next-event visibility at a bounce hit. One ray
            // along the normal; at its hit, green / red = the shadow map said lit /
            // shadowed, blue = it has no page fine enough there, so a shadow ray is
            // traced. Black: the ray escaped, or the hit faces away from the sun.
            // Blue is where Max Shadow Texel is saving a leak - or costing a ray.
            uint probeRng = 0x9e3779b9u;
            const RayResult probe = TraceGIRay(worldPos + surfaceNormal * 0.002f, surfaceNormal, probeRng, kRayDebugNoSlot);
            const float3 sunDir = -g_SunDir;
            diagnostic = float3(0.0f, 0.0f, 0.0f);
            if (probe.hitDist < 1e4f * 0.999f && dot(probe.geoNormal, sunDir) > 0.0f)
            {
                const float vsmVisibility = RtgiVsmSunVisibility(probe.hitPos, probe.geoNormal);
                diagnostic = vsmVisibility < 0.0f
                    ? float3(0.0f, 0.0f, 1.0f)
                    : float3(1.0f - vsmVisibility, vsmVisibility, 0.0f);
            }
        }
        else if (g_DebugView == 14)
        {
            // Constant seed, not pixel-derived: the whole point is that nothing
            // here varies with the camera or the frame index.
            uint probeRng = 0x9e3779b9u;
            const RayResult probe = TraceGIRay(
                worldPos + surfaceNormal * 0.002f, surfaceNormal, probeRng, kRayDebugNoSlot);
            diagnostic = probe.radiance;
        }

        u_GIOutput[tracePixel] = float4(diagnostic, 1.0f);

        GIReservoir diagnosticReservoir = EmptyReservoir();
        diagnosticReservoir.position  = worldPos;
        diagnosticReservoir.normal    = surfaceNormal;
        diagnosticReservoir.radiance  = diagnostic;
        diagnosticReservoir.weightSum = 1.0f;
        diagnosticReservoir.M         = 1;
        u_Reservoir[PixelIndex(tracePixel)] = PackReservoir(diagnosticReservoir);
        return;
    }

    // ── Primary ray origin ───────────────────────────────────────────────────
    // Lifted onto the smooth surface; see ComputePrimaryRayOrigin for why.
    float3 rayOrigin, geoNormal;
    ComputePrimaryRayOrigin(worldPos, surfaceNormal, rayOrigin, geoNormal);

    [branch]
    if (debugPixelSlot != kRayDebugNoSlot)
        RecordRaySegment(debugPixelSlot + g_RayDebugSlotsPerPixel - 1u, kRayDebugCamera, 0u,
                         g_CameraPos, rayOrigin, 0.0f.xxx, 0.0f.xxx);

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
        const uint debugRaySlot = (debugPixelSlot != kRayDebugNoSlot && i < g_RayDebugRays)
            ? debugPixelSlot + i * g_RayDebugSlotsPerRay
            : kRayDebugNoSlot;
        RayResult hit = TraceGIRay(rayOrigin, rayDir, rng, debugRaySlot);

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

    u_Reservoir[PixelIndex(tracePixel)] = PackReservoir(reservoir);

    // Write average GI radiance estimate as the initial (pre-ReSTIR) output.
    // Alpha carries the average hit distance so NRD RELAX can use a real guide signal
    // instead of the old placeholder value of 1.0 for every pixel.
    u_GIOutput[tracePixel] = float4(giRadianceSum / float(numRays), giHitDistSum / float(numRays));
}

