// SubsurfaceScattering_RT.hlsl  -  cs_6_5
// Ray-traced subsurface scattering. Produces, for every subsurface pixel, the scattered
// diffuse lighting plus light transmitted through the object; PSRayTracedComposite in
// SubsurfaceScattering.hlsl then swaps it into the scene the same way the screen-space
// blur does.
//
// Scattering: the diffusion profile (the same d'Eon/Jimenez profile the screen-space
// kernel is built from, Subsurface.hlsli) is importance sampled as a radius on a disc
// tangent to the surface. Each disc point is projected back onto the real geometry with
// a pair of probe rays along the normal - the "disc projection" of Christensen and King -
// so the sample lands where light actually enters the medium: around the curve of an
// ear, up the side of a nose, across a knuckle. A screen-space blur can only walk the
// depth buffer and gives up at silhouettes. Everything is world space: the light
// entering at each sample is evaluated there, from the sun and every scene light with
// ray-traced shadows (WorldSpaceIrradiance), never read back from the screen, so parts
// of the surface the camera cannot see scatter light just the same. The lighting pass
// hands over the pixel's direct diffuse only, and it is replaced by the scattered light;
// GI, ambient and AO stay as the lighting pass resolved them. A
// bilateral filter in the composite removes the remaining sampling noise.
//
// Channels are sampled with one-sample MIS (balance heuristic over the three channel
// profiles) and combined with a self-normalised estimator, which matches the screen-
// space kernel's normalisation and stays unbiased when a probe misses. The sample
// pattern rotates every frame; temporal anti-aliasing integrates it.
//
// Transmission: a ray from just inside the surface toward each light measures how much
// of the object the light has to cross - the thickness the screen-space mode can only
// estimate from shadow maps - and a shadow ray from where it enters decides whether
// anything else is in the way. A ray that leaves without re-entering anything means an
// open sheet (a leaf, an ear modelled as a shell), which counts as zero thickness.
//
// Entry points: main() is the plain per-pixel estimate. With ReSTIR on, the same probes
// feed CSRestirTemporal instead (see the section at the end), which reuses them across
// frames.

#define PTERO_SSS_CB_REGISTER b0
#include "Subsurface.hlsli"
#include "SurfaceSpecular.hlsli"
#include "RtTransform.hlsli"

Texture2D<float4>               gSssDiffuse     : register(t1); // lighting pass diffuse, alpha = mask
Texture2D<float4>               gSssNormalDepth : register(t2); // G-Buffer RT1
Texture2D<float4>               gSssAlbedo      : register(t3); // G-Buffer RT0
RaytracingAccelerationStructure gSssScene       : register(t4);
RWTexture2D<float4>             gSssOutput      : register(u0);

// Geometry pools shared with RTGI (RtGlobalIllumination::BuildTlas); must match the
// structs in RtGI_RayGen.hlsl.
struct GpuPackedVertex { float px, py, pz, nx, ny, nz, u, v; };
struct GpuInstanceInfo { uint vertexOffset, indexOffset, vertexCount, indexCount, materialRangeOffset, materialRangeCount, _pad0, _pad1; };
StructuredBuffer<GpuPackedVertex> gSssVertices     : register(t5);
StructuredBuffer<uint>            gSssIndices      : register(t6);
StructuredBuffer<GpuInstanceInfo> gSssInstanceInfo : register(t7);

// The lighting pass's shadow maps and comparison sampler.
Texture2D                gSssSunShadowMap    : register(t8);  // or the virtual shadow map's page pool (sun and local lights)
Texture2DArray           gSssPointShadowMaps : register(t9);
StructuredBuffer<uint>   gSssVsmPageTable    : register(t10); // virtual shadow map pages -> pool tiles
SamplerComparisonState   gSssShadowSampler   : register(s0);

// The five Gaussians of the reflectance profile. The SDK drops the narrowest (0.233 at
// variance 0.0064) as directly bounced light accounted for by the strength parameter.
static const uint  kProfileTerms = 5u;
static const float kProfileVariance[5] = { 0.0484f, 0.187f, 0.567f, 1.99f, 7.41f };
static const float kProfileWeight[5]   = { 0.100f,  0.118f, 0.113f, 0.358f, 0.078f };
// Kernel range in profile millimetres; matches the screen-space kernel's RANGE.
static const float kProfileRangeMm = 3.0f;
static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
uint PcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float NextRandom(inout uint rng)
{
    rng = PcgHash(rng);
    return (rng >> 8) * (1.0f / 16777216.0f);
}

float3 DecodeOctNormal(float2 encoded)
{
    const float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 ReconstructWorldPosition(uint2 pixel, float deviceDepth)
{
    const float2 uv = (float2(pixel) + 0.5f) * gSssInvRenderSize;
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f);
    const float4 world = mul(ndc, gSssInvViewProj);
    return world.xyz / world.w;
}

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    const float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// Fraction of Gaussian term k (variance v, in falloff-scaled units) inside the range.
float TermMassInRange(uint k, float falloff)
{
    const float rangeScaled = kProfileRangeMm / (0.001f + falloff);
    return 1.0f - exp(-rangeScaled * rangeScaled / (2.0f * kProfileVariance[k]));
}

// Radial 2D density of one channel's profile at r (millimetres), truncated to the range
// and normalised to integrate to one over the disc.
float ChannelDensity(float rMm, float falloff)
{
    if (rMm > kProfileRangeMm)
        return 0.0f;
    const float f = 0.001f + falloff;
    const float rr = rMm / f;
    float density = 0.0f;
    float mass = 0.0f;
    [unroll]
    for (uint k = 0u; k < kProfileTerms; ++k)
    {
        density += kProfileWeight[k] * exp(-rr * rr / (2.0f * kProfileVariance[k]))
                 / (2.0f * kPi * kProfileVariance[k]);
        mass += kProfileWeight[k] * TermMassInRange(k, falloff);
    }
    return density / (f * f * max(mass, 1e-8f));
}

// Draws a radius (millimetres) from one channel's truncated profile.
float SampleChannelRadius(float falloff, float u0, float u1)
{
    float mass = 0.0f;
    [unroll]
    for (uint k = 0u; k < kProfileTerms; ++k)
        mass += kProfileWeight[k] * TermMassInRange(k, falloff);

    // Pick a Gaussian term in proportion to its mass inside the range.
    float target = u0 * mass;
    uint term = kProfileTerms - 1u;
    [unroll]
    for (uint j = 0u; j < kProfileTerms; ++j)
    {
        const float termMass = kProfileWeight[j] * TermMassInRange(j, falloff);
        if (target < termMass)
        {
            term = j;
            break;
        }
        target -= termMass;
    }

    // Truncated 2D Gaussian: invert its radial CDF over [0, range].
    const float truncatedU = u1 * TermMassInRange(term, falloff);
    const float rr = sqrt(-2.0f * kProfileVariance[term] * log(max(1.0f - truncatedU, 1e-7f)));
    return min(rr * (0.001f + falloff), kProfileRangeMm);
}

bool TraceClosest(float3 origin, float3 direction, float tMin, float tMax, out float hitT)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = tMin;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gSssScene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}

    hitT = tMax;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hitT = query.CommittedRayT();
        return true;
    }
    return false;
}

// Shadow ray for the scattered light. Back faces are culled: an origin that ended up a
// hair inside the mesh (easy on scanned or terraced assets) would otherwise see the far
// side of the object from within and report a shadow, which punched whole pixels black.
// Same convention as the RTAO rays.
bool TraceShadowVisible(float3 origin, float3 direction, float tMax, float tMin)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = tMin;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES
           | RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gSssScene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

bool TraceVisible(float3 origin, float3 direction, float tMax, float tMin = 0.0f)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = tMin;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gSssScene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ---------------------------------------------------------------------------
// Surface probes against the real geometry
// ---------------------------------------------------------------------------
struct SurfaceHit
{
    float3 Position;      // lifted onto the smooth surface (Hanika)
    float3 RawPosition;   // the actual ray hit on the triangle
    float3 Normal;        // interpolated vertex normal, on the probe's side
    float3 GeoNormal;     // flat triangle normal, same side
};

// Closest hit along a ray, resolved to a surface point with its normals from the shared
// RTGI geometry pools. The hit is lifted onto the smooth surface its vertex normals
// describe (Hanika, "Hacking the Shadow Terminator"), so shadow rays leaving it are not
// caught by neighbouring facets or scan terraces of a coarse or noisy mesh.
bool TraceSurface(float3 origin, float3 direction, float tMax, float3 referenceNormal, out SurfaceHit hit)
{
    hit.Position = origin;
    hit.RawPosition = origin;
    hit.Normal = referenceNormal;
    hit.GeoNormal = referenceNormal;

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.0f;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gSssScene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {}
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;

    const GpuInstanceInfo info = gSssInstanceInfo[query.CommittedInstanceIndex()];
    const uint primitive = query.CommittedPrimitiveIndex();
    const float2 bary = query.CommittedTriangleBarycentrics();
    const float3 weights = float3(1.0f - bary.x - bary.y, bary.x, bary.y);

    const float3x4 o2w = query.CommittedObjectToWorld3x4();

    float3 vertexPos[3];
    float3 vertexNrm[3];
    [unroll]
    for (uint k = 0u; k < 3u; ++k)
    {
        const GpuPackedVertex v = gSssVertices[info.vertexOffset + gSssIndices[info.indexOffset + primitive * 3u + k]];
        const float4 local = float4(v.px, v.py, v.pz, 1.0f);
        vertexPos[k] = float3(dot(o2w[0], local), dot(o2w[1], local), dot(o2w[2], local));
        vertexNrm[k] = PteroObjectToWorldNormal(o2w, float3(v.nx, v.ny, v.nz));
    }

    float3 geo = cross(vertexPos[1] - vertexPos[0], vertexPos[2] - vertexPos[0]);
    const float geoLengthSq = dot(geo, geo);
    geo = geoLengthSq > 1e-20f ? geo * rsqrt(geoLengthSq) : referenceNormal;
    if (dot(geo, referenceNormal) < 0.0f)
        geo = -geo;

    const float3 position = origin + direction * query.CommittedRayT();
    float3 lifted = position;
    float3 normal = 0.0f.xxx;
    [unroll]
    for (uint j = 0u; j < 3u; ++j)
    {
        float3 n = vertexNrm[j];
        const float nLengthSq = dot(n, n);
        if (nLengthSq < 1e-12f)
            continue;
        n *= rsqrt(nLengthSq);
        if (dot(n, geo) < 0.0f)
            n = -n;
        lifted -= weights[j] * min(0.0f, dot(position - vertexPos[j], n)) * n;
        normal += weights[j] * n;
    }

    hit.Position = lifted;
    hit.RawPosition = position;
    hit.GeoNormal = geo;
    hit.Normal = dot(normal, normal) > 1e-12f ? normalize(normal) : geo;
    return true;
}

// Radiance from one scene light at a point, with its shape and distance falloff resolved
// as the deferred lighting pass does. Returns 0 outside the light's reach.
float3 LightRadianceAt(PteroLightData light, float3 position, out float3 L, out float dist)
{
    const PteroResolvedLight shape = PteroResolveLightShape(light, position);
    const float3 toLight = shape.Position - position;
    dist = max(length(toLight), 1e-4f);
    L = toLight / dist;
    if (dist >= light.Radius || shape.ShapeMask <= 0.0f)
        return 0.0f.xxx;

    const float normalizedDistance = saturate(dist / max(light.Radius, 1e-4f));
    float rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
    rangeMask *= rangeMask;
    return light.Color * rangeMask * shape.ShapeMask
         * pow(PteroLightFalloffDistance(light, dist), -max(light.FalloffExponent, 0.001f));
}

// ---------------------------------------------------------------------------
// Shadow-map visibility, as the deferred lighting pass computes it
// ---------------------------------------------------------------------------
// The scatter samples are shadowed with the same maps and the same 3x3 PCF as the lit
// image, not with rays. Rays against the raw triangles resolve every facet and scan
// terrace of a mesh, so they shadowed assets that render perfectly well - the shadow maps
// are what the rest of the frame is shadowed by, and the scattered light has to agree
// with it. Rays are still used where they are the right tool: placing the samples on the
// real surface, and measuring transmission thickness.
float SunShadowVisibility(float3 worldPos, float3 normal)
{
    if (gSssHasSunShadow == 0u)
        return 1.0f;

    [branch]
    if (gSssVsm.Active != 0u)
    {
        int usedLevel;
        return gSssVsm.Enabled != 0u
            ? PteroVsmVisibility(gSssVsm, gSssVsmPageTable, gSssSunShadowMap, worldPos, normal, gSssCameraPos, usedLevel)
            : 1.0f;
    }

    const float4 lightClip = mul(float4(worldPos, 1.0f), gSssLightViewProj);
    const float3 projCoords = lightClip.xyz / lightClip.w;
    const float2 uv = float2(projCoords.x * 0.5f + 0.5f, -projCoords.y * 0.5f + 0.5f);
    if (any(uv < 0.0f.xx) || any(uv > 1.0f.xx) || projCoords.z < 0.0f || projCoords.z > 1.0f)
        return 1.0f;

    const float depth = projCoords.z - gSssShadowBias;
    const float texelSize = 1.0f / max(gSssShadowMapSize, 1.0f);
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
            visibility += gSssSunShadowMap.SampleCmpLevelZero(gSssShadowSampler, uv + float2(x, y) * texelSize, depth);
    }
    return visibility / 9.0f;
}

float PointShadowVisibility(PteroLightData light, float3 worldPos, float3 normal)
{
    if (light.CastShadows < 0.5f)
        return 1.0f;
    const int shadowIndex = (int)light.ShadowIndex;
    // Virtual shadow map: the shadow index is the light's slot in it.
    [branch]
    if (gSssVsm.LocalEnabled != 0u)
        return shadowIndex < 0 ? 1.0f
            : PteroVsmLocalVisibility(gSssVsm, gSssVsmPageTable, gSssSunShadowMap, (uint)shadowIndex, worldPos,
                                      normal, gSssCameraPos, 1.0f);
    if (gSssHasPointShadows == 0u || shadowIndex < 0 || shadowIndex >= 4)
        return 1.0f;

    const float radius = max(light.Radius, 1e-4f);
    const float mapSize = max(gSssPointShadowMapSize, 1.0f);
    const float texelSize = 1.0f / mapSize;
    // Normal offset as the lighting pass applies it (its default strength of 1 texel).
    const float3 offsetPos = worldPos + normal * (texelSize * radius);

    const float3 toPoint = offsetPos - light.Position;
    const float distanceToLight = length(toPoint);
    if (distanceToLight <= 1e-4f)
        return 1.0f;

    // The face that holds the point furthest from its edges.
    int bestFace = -1;
    float2 bestUv = 0.0f.xx;
    float bestEdge = -1.0f;
    [unroll]
    for (int face = 0; face < 6; ++face)
    {
        const float4 clip = mul(float4(offsetPos, 1.0f), gSssPointFaceViewProj[shadowIndex * 6 + face]);
        if (abs(clip.w) <= 1e-5f)
            continue;
        const float3 ndc = clip.xyz / clip.w;
        const float2 faceUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (ndc.z < 0.0f || ndc.z > 1.0f || any(faceUv < 0.0f.xx) || any(faceUv > 1.0f.xx))
            continue;
        const float2 edge2 = min(faceUv, 1.0f.xx - faceUv);
        const float edge = min(edge2.x, edge2.y);
        if (edge > bestEdge)
        {
            bestEdge = edge;
            bestFace = face;
            bestUv = faceUv;
        }
    }
    if (bestFace < 0)
        return 1.0f;

    const float3 L = -toPoint / distanceToLight;
    const float alignment = saturate(dot(normal, L));
    const float depthBias = gSssPointShadowBias / radius + (1.0f - alignment) * texelSize * 2.0f;
    const float reference = distanceToLight / radius - depthBias;

    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += gSssPointShadowMaps.SampleCmpLevelZero(gSssShadowSampler,
                float3(saturate(bestUv + float2(x, y) * texelSize), shadowIndex * 6 + bestFace), reference);
        }
    }
    return visibility / 9.0f;
}

// Direct irradiance arriving at a surface point from the sun and every scene light, each
// with a shadow-map shadow. This is the world-space signal the ray-traced mode scatters:
// it exists everywhere on the mesh - off screen, behind the silhouette, under the chin -
// not only where the camera happens to see.
//
// shadingNormal is the pixel's own surface normal without its normal map
// (PixelGeometricNormal), not the hit's interpolated vertex normal: the samples lie within
// the scatter radius of the pixel, and on a scanned or remeshed asset the vertex normals
// follow the scan terraces the normal map was baked to hide, which striped the result.
// Not the G-Buffer normal either: light scattered under the skin enters at the sample,
// with the surface's orientation there, so pore-scale normal detail is what scattering
// softens. Lit with the normal-mapped normal, every pore kept its own N.L through the
// scatter, and flipping the map's green channel changed the scattered light completely.
// The unscattered share (1 - strength) keeps the normal map. ignoreDistance skips occluders closer
// than that for the same reason - terrace steps shadowing each other - at a scale where a
// genuine contact shadow would not be resolvable anyway.
float3 WorldSpaceIrradiance(SurfaceHit surface, float3 shadingNormal, float ignoreDistance)
{
    // Start from the raw hit, pushed off along the pixel's normal. The Hanika lift follows
    // the vertex normals, and on a terraced scan those point sideways often enough to move
    // the origin into the next step.
    const float shadowBias = 0.002f;
    const float3 origin = surface.RawPosition + shadingNormal * shadowBias;
    float3 irradiance = 0.0f.xxx;

    const float3 L_sun = normalize(-gSssSunDirection);
    const float sunNdotL = saturate(dot(shadingNormal, L_sun));
    if (sunNdotL > 0.0f && dot(gSssSunColor, gSssSunColor) > 0.0f)
        irradiance += gSssSunColor * sunNdotL * SunShadowVisibility(surface.RawPosition, shadingNormal);

    [loop]
    for (int li = 0; li < gSssNumLights && li < PTERO_SSS_MAX_LIGHTS; ++li)
    {
        float3 L;
        float dist;
        const float3 radiance = LightRadianceAt(gSssLights[li], surface.Position, L, dist);
        const float NdotL = saturate(dot(shadingNormal, L));
        if (NdotL <= 0.0f || dot(radiance, radiance) <= 0.0f)
            continue;
        irradiance += radiance * NdotL * PointShadowVisibility(gSssLights[li], surface.RawPosition, shadingNormal);
    }
    return irradiance;
}

// Finds the real surface under a point on the tangent disc: probe down the normal first,
// then up, so the disc point may sit either just above or just below the curved surface.
bool ProbeSurface(float3 discPoint, float3 N, float reach, out SurfaceHit surface)
{
    const float epsilon = max(reach * 0.02f, 1e-4f);
    if (TraceSurface(discPoint + N * epsilon, -N, reach + epsilon, N, surface))
        return true;
    return TraceSurface(discPoint - N * epsilon, N, reach + epsilon, N, surface);
}

// Light crossing the object toward the viewer-facing side, with the thickness it crossed
// measured by a ray. `L` points toward the light; `maxDistance` is how far the light is.
float3 TracedTransmission(PteroSssProfile profile, float3 position, float3 N, float3 L,
                          float maxDistance, float3 lightRadiance, float3 albedo, float maxThickness)
{
    // Light from behind only (PteroSssTransmission).
    if (dot(L, N) >= 0.0f)
        return 0.0f.xxx;

    // Start well inside the surface, not just under it. A millimetre-scale start let the
    // ray catch the very next facet or scan terrace of the same side of the mesh, which
    // read as near-zero thickness and lit the shadowed side as if it were paper-thin.
    // The start depth is added back to the thickness; parts thinner than it are treated as
    // open sheets below, which transmit strongly anyway.
    const float startDepth = max(0.0015f, 0.1f * maxThickness);
    const float3 inside = position - N * startDepth;
    float hitT;
    float thickness = 0.0f;
    if (TraceClosest(inside, L, 0.0f, maxDistance, hitT))
    {
        thickness = hitT + startDepth;
        if (thickness > maxThickness)
            return 0.0f.xxx; // too thick, or something else fully in the way
        // Something past the entry point can still block the light.
        const float remaining = maxDistance - hitT;
        if (remaining > 0.002f && !TraceVisible(inside + L * (hitT + 0.002f), L, remaining))
            return 0.0f.xxx;
    }
    // A miss: the ray left through an open sheet and nothing else is in the way.

    return PteroSssTransmission(profile, thickness, N, L, lightRadiance, albedo);
}

// PteroSssDepthNormal at this pixel, from the G-Buffer depth; facing the camera like N.
float3 PixelGeometricNormal(uint2 pixel, float3 P, float3 N)
{
    const int2 maxPixel = int2(gSssRenderSize) - 1;
    float3 neighbours[4];
    const int2 offsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    [unroll]
    for (uint k = 0u; k < 4u; ++k)
    {
        const int2 q = clamp(int2(pixel) + offsets[k], int2(0, 0), maxPixel);
        neighbours[k] = ReconstructWorldPosition(uint2(q), gSssNormalDepth.Load(int3(q, 0)).z);
    }
    return PteroSssDepthNormal(P, neighbours[0], neighbours[1], neighbours[2], neighbours[3], N);
}

// Transmitted light from the sun and every scene light at a pixel, before fog. N is the
// surface's own normal (PixelGeometricNormal), not the normal-mapped one.
float3 PixelTransmission(PteroSssProfile profile, float3 P, float3 N, float3 albedo)
{
    const float maxThickness = PteroSssMaxTransmissionThickness(profile);
    if (gSssTransmission == 0u || maxThickness <= 0.0f)
        return 0.0f.xxx;

    const float3 L_sun = normalize(-gSssSunDirection);
    float3 transmitted = TracedTransmission(profile, P, N, L_sun, 1.0e4f, gSssSunColor, albedo, maxThickness);

    [loop]
    for (int li = 0; li < gSssNumLights && li < PTERO_SSS_MAX_LIGHTS; ++li)
    {
        const PteroLightData light = gSssLights[li];
        const PteroResolvedLight shape = PteroResolveLightShape(light, P);
        const float3 toLight = shape.Position - P;
        const float dist = max(length(toLight), 1e-4f);
        if (dist >= light.Radius || shape.ShapeMask <= 0.0f)
            continue;

        const float normalizedDistance = saturate(dist / max(light.Radius, 1e-4f));
        float rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
        rangeMask *= rangeMask;
        const float falloffTerm = rangeMask * shape.ShapeMask
            * pow(PteroLightFalloffDistance(light, dist), -max(light.FalloffExponent, 0.001f));

        transmitted += TracedTransmission(profile, P, N, toLight / dist, dist,
                                          light.Color * falloffTerm, albedo, maxThickness);
    }
    return transmitted;
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (any(pixel >= (uint2)gSssRenderSize))
        return;

    const float4 diffuseM = gSssDiffuse.Load(int3(pixel, 0));
    const float4 normalDepth = gSssNormalDepth.Load(int3(pixel, 0));
    const uint slot = PteroDecodeSubsurfaceSlot(normalDepth.w);

    if (diffuseM.a < 0.5f || !PteroSssSlotActive(slot))
    {
        gSssOutput[pixel] = float4(diffuseM.rgb, 0.0f);
        return;
    }

    const PteroSssProfile profile = gSssProfiles[slot];
    const float3 strength = saturate(profile.ColorRadius.rgb);
    const float3 falloff = max(profile.FalloffTranslucency.rgb, 0.0f.xxx);
    const float metresPerMm = profile.ColorRadius.a / kProfileRangeMm;

    const float3 P = ReconstructWorldPosition(pixel, normalDepth.z);
    float3 N = DecodeOctNormal(normalDepth.xy);
    const float3 V = normalize(gSssCameraPos - P);
    if (dot(N, V) < 0.0f)
        N = -N;
    const float3 albedo = gSssAlbedo.Load(int3(pixel, 0)).rgb;

    float3 T, B;
    BuildBasis(N, T, B);
    // What the scattered light is lit with (see WorldSpaceIrradiance).
    const float3 scatterNormal = PixelGeometricNormal(pixel, P, N);

    uint rng = PcgHash(pixel.x + pixel.y * 8192u) ^ PcgHash(gSssFrameIndex * 9781u + 17u);
    const uint sampleCount = clamp(gSssRtSamples, 1u, 64u);
    const float rotation = NextRandom(rng);
    const uint channelOffset = (uint)(NextRandom(rng) * 3.0f);

    // Occluders nearer than this are ignored by the samples' shadow rays (see
    // WorldSpaceIrradiance): a little over what one pixel spans at this distance, plus
    // half the scatter radius.
    const float shadowIgnoreDistance = 0.002f * PteroSssLinearDepth(normalDepth.z)
                                     + 0.5f * profile.ColorRadius.a;

    float3 weightedSum = 0.0f.xxx;
    float3 weightTotal = 0.0f.xxx;

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const uint channel = (i + channelOffset) % 3u;
        const float rMm = SampleChannelRadius(falloff[channel], NextRandom(rng), NextRandom(rng));
        // Stratified angle, rotated per pixel and per frame.
        const float phi = 2.0f * kPi * frac((i + 0.5f) / sampleCount + rotation);
        const float r = rMm * metresPerMm;

        // Disc point, then probe both ways along the normal for the real surface.
        const float3 discPoint = P + (T * cos(phi) + B * sin(phi)) * r;
        SurfaceHit surface;
        if (!ProbeSurface(discPoint, N, max(r * 1.5f, 0.002f), surface))
            continue; // the disc point is not over this surface (past an edge)
        const float3 hitPosition = surface.Position;

        // Profile over the actual distance travelled, as the screen-space kernel does.
        const float distanceMm = min(length(hitPosition - P) / max(metresPerMm, 1e-8f), kProfileRangeMm);
        const float3 channelDensity = float3(ChannelDensity(distanceMm, falloff.r),
                                             ChannelDensity(distanceMm, falloff.g),
                                             ChannelDensity(distanceMm, falloff.b));
        // One-sample MIS over the three channel strategies: pdf of the mixture.
        const float mixturePdf = (ChannelDensity(rMm, falloff.r)
                                + ChannelDensity(rMm, falloff.g)
                                + ChannelDensity(rMm, falloff.b)) / 3.0f;
        if (mixturePdf <= 0.0f)
            continue;

        const float3 weight = channelDensity / mixturePdf;
        weightedSum += WorldSpaceIrradiance(surface, scatterNormal, shadowIgnoreDistance) * weight;
        weightTotal += weight;
    }

    // In this mode the lighting pass hands over only the pixel's *direct* diffuse light
    // (already fogged; alpha = 0.5 + 0.5 * fog transmittance). That is exactly the light
    // being redistributed, so it is replaced outright by the scattered world-space light -
    // post-scatter texturing with this pixel's albedo - with no second estimate of the
    // pixel's own lighting to subtract. (A per-pixel ray-traced estimate used to be
    // subtracted here; its single binary shadow ray made every pixel flip between lit and
    // shadowed, which no sample count could average out.) GI, ambient and AO never enter
    // this pass, so they stay as the lighting pass resolved them. Strength blends between
    // leaving the light where it arrived and scattering all of it, as the SDK's kernel[0]
    // does; channels with no valid sample keep their original light.
    const float fogTransmittance = saturate(diffuseM.a * 2.0f - 1.0f);
    float3 scatteredDirect = diffuseM.rgb;
    [unroll]
    for (uint c = 0u; c < 3u; ++c)
    {
        if (weightTotal[c] > 0.0f)
            scatteredDirect[c] = albedo[c] * (weightedSum[c] / weightTotal[c]) * fogTransmittance;
    }
    float3 result = lerp(diffuseM.rgb, scatteredDirect, strength);

    // ---- Transmission ----
    result += PixelTransmission(profile, P, scatterNormal, albedo) * fogTransmittance;

    gSssOutput[pixel] = float4(max(result, 0.0f.xxx), 1.0f);
}

// ===========================================================================
// ReSTIR temporal reuse (gSssRestir != 0): CSRestirTemporal
// ===========================================================================
// main() averages a dozen fresh probes per pixel and forgets them. Here the same probes
// are candidates for reservoirs (Bitterli et al. 2020) that are merged with the pixel's
// reprojected history every frame, for the cost of its probes plus, per colour channel,
// one more probe and one lighting evaluation for the history's sample.
//
// Measured against main() on a GPU harness (5 cm sphere, skin-like 12 mm profile, a point
// light 1.7 cm off the surface, 8 probes, history length 5): per-frame noise is about half
// of main()'s under TAA jitter or an orbiting camera, about a third on a perfectly still
// one, with no measurable bias (under 0.2%), and a moving light shows from its first frame.
//
// Samples are disc points. A sample is stored as its offset u on the receiver's tangent
// disc - exactly what main() draws - and is re-projected onto the mesh from the current
// receiver every time it is reused: the history's u is a disc point of this pixel too, so
// the shift between the two receivers has a Jacobian of one and the history is judged
// against the profile around this pixel, not around wherever the previous frame's pixel
// was. Reusing the previous frame's surface point instead (an identity shift) fails once
// the receiver moves by more than the profile's core - for skin's green and blue under a
// millimetre - which TAA jitter alone does every frame.
//
// Everything is in disc measure, as in main(): the integrand at disc point u is
// f_c(u) = R_c(|x(u) - P|) E_c(x(u)), with x(u) the surface the probe finds. The history's
// sample is lit again at its new surface point with this pixel's normal and shadow maps,
// so the shading never lags; how much its irradiance changed since it was stored sets how
// far the history is trusted (a moving light, or a shift onto differently lit geometry,
// hands the fresh probes the weight they need - without it the history's M kept 95% of the
// weight on samples drawn for lighting that was gone).
//
// One reservoir per colour channel. The channel profiles differ by a factor of three in
// width (skin: red scatters furthest), and a single sample chosen for their mean is a poor
// sample for each of them - the harness measured a noise floor no amount of reuse moved.
// The three share the candidate stream (the same probes, the same M) and the receiver.
//
// Normalisation. main() divides by the sum of its sample weights, which keeps energy at
// mesh edges, where probes miss, and on curved surfaces, where the profile does not
// integrate to one over the real surface. Here the two halves are estimated separately:
// the numerator from the reservoirs, the denominator D_c = integral of R_c over the disc
// from the pixel's own fresh probes, as a control variate (see the candidate loop).
//
// Keep the history short. Every reuse rescales the history's estimate by the ratio of
// the new to the old target at its one sample; on a moving receiver - and with TAA jitter
// every receiver moves - those ratios chain over the history's lifetime into an error
// that is correlated over many frames rather than averaged away. With 20 frames of
// history the harness's noise under jitter was worse than main()'s; with 5 it is half.
// A perfectly still, unjittered view would profit from more (a third of the noise at 20).
//
// No spatial reuse. With the identity shift it was measured to add noise with every
// neighbour (their profiles barely overlap this one's); with the disc shift it costs a
// probe and a lighting evaluation per neighbour and channel, about what more probes cost.
//
// The history ping-pongs between two buffers: read gSssReservoirIn (the previous frame's
// reservoirs), write gSssReservoirOut.

// 48 bytes per pixel. Per channel c (dwords 3c .. 3c+2): the disc offset u (half2, in the
// receiver's BuildBasis frame), E_c as lit (half), W. Then the receiver's device depth,
// its normal (16-bit octahedral), and M (12 bits) with the profile slot (4 bits).
struct PackedSssReservoir
{
    uint4 A;
    uint4 B;
    uint4 C;
};
RWStructuredBuffer<PackedSssReservoir> gSssReservoirOut : register(u1);
RWStructuredBuffer<PackedSssReservoir> gSssReservoirIn  : register(u3);

static const uint  kSssMaxM = 4095u;
// The history is reused only if its receiver's normal is this close to this pixel's.
static const float kSssReuseNormalDot = 0.9f;

struct SssSample
{
    float2 Disc;        // offset on the receiver's tangent disc, metres (T, B)
    float3 Position;    // the surface point the receiver's probe found for it
    float  Irradiance;  // E_c there
    float  W;           // unbiased contribution weight, per disc mm^2 (profile units)
};

struct SssReservoir
{
    SssSample C[3];
    uint      M;
};

SssSample EmptySssSample()
{
    SssSample s;
    s.Disc = 0.0f.xx;
    s.Position = 0.0f.xxx;
    s.Irradiance = 0.0f;
    s.W = 0.0f;
    return s;
}

uint PackOct(float3 n, uint bits)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8f);
    const float2 oct = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    const float scale = (float)((1u << bits) - 1u);
    const uint2 q = uint2(round(saturate(oct * 0.5f + 0.5f) * scale));
    return q.x | (q.y << bits);
}

float3 UnpackOct(uint packed, uint bits)
{
    const uint mask = (1u << bits) - 1u;
    const float2 encoded = float2(packed & mask, (packed >> bits) & mask) / (float)mask;
    return DecodeOctNormal(encoded);
}

uint PackedWord(PackedSssReservoir p, uint i)
{
    return i < 4u ? p.A[i] : (i < 8u ? p.B[i - 4u] : p.C[i - 8u]);
}

uint PackedSlot(PackedSssReservoir p)             { return (p.C.w >> 12) & 0xFu; }
uint PackedM(PackedSssReservoir p)                { return p.C.w & kSssMaxM; }
float PackedReceiverDepth(PackedSssReservoir p)   { return asfloat(p.C.y); }
float3 PackedReceiverNormal(PackedSssReservoir p) { return UnpackOct(p.C.z, 16u); }

PackedSssReservoir PackSssReservoir(SssReservoir r, float3 receiverNormal, float receiverDepth, uint slot)
{
    uint words[12];
    [unroll]
    for (uint c = 0u; c < 3u; ++c)
    {
        const SssSample s = r.C[c];
        words[3u * c + 0u] = f32tof16(s.Disc.x) | (f32tof16(s.Disc.y) << 16);
        words[3u * c + 1u] = f32tof16(clamp(s.Irradiance, 0.0f, 65000.0f));
        words[3u * c + 2u] = asuint(isfinite(s.W) ? max(s.W, 0.0f) : 0.0f);
    }
    words[9] = asuint(receiverDepth);
    words[10] = PackOct(receiverNormal, 16u);
    words[11] = min(r.M, kSssMaxM) | ((slot & 0xFu) << 12);

    PackedSssReservoir p;
    p.A = uint4(words[0], words[1], words[2], words[3]);
    p.B = uint4(words[4], words[5], words[6], words[7]);
    p.C = uint4(words[8], words[9], words[10], words[11]);
    return p;
}

PackedSssReservoir EmptyPackedSssReservoir()
{
    PackedSssReservoir p;
    p.A = uint4(0u, 0u, 0u, 0u);
    p.B = uint4(0u, 0u, 0u, 0u);
    p.C = uint4(0u, 0u, 0u, 0u);
    return p;
}

// Positions are not stored: a reused sample is projected again from its new receiver.
SssReservoir UnpackSssReservoir(PackedSssReservoir p)
{
    SssReservoir r;
    [unroll]
    for (uint c = 0u; c < 3u; ++c)
    {
        const uint w0 = PackedWord(p, 3u * c);
        r.C[c].Disc = float2(f16tof32(w0), f16tof32(w0 >> 16));
        r.C[c].Position = 0.0f.xxx;
        r.C[c].Irradiance = f16tof32(PackedWord(p, 3u * c + 1u));
        r.C[c].W = asfloat(PackedWord(p, 3u * c + 2u));
    }
    r.M = PackedM(p);
    return r;
}

float3 ReconstructWorldPositionWith(float4x4 invViewProj, uint2 pixel, float deviceDepth)
{
    const float2 uv = (float2(pixel) + 0.5f) * gSssInvRenderSize;
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f);
    const float4 world = mul(ndc, invViewProj);
    return world.xyz / world.w;
}

// The per-channel profile of one material, with the parts of ChannelDensity that do not
// depend on the radius worked out once per pixel.
struct ProfileShape
{
    float3 Falloff;   // 0.001 + falloff
    float3 InvNorm;   // 1 / (falloff^2 * mass inside the range)
};

ProfileShape MakeProfileShape(float3 falloff)
{
    ProfileShape s;
    s.Falloff = 0.001f + falloff;
    float3 mass = 0.0f.xxx;
    [unroll]
    for (uint k = 0u; k < kProfileTerms; ++k)
    {
        mass += kProfileWeight[k] * float3(TermMassInRange(k, falloff.r),
                                           TermMassInRange(k, falloff.g),
                                           TermMassInRange(k, falloff.b));
    }
    s.InvNorm = 1.0f / (s.Falloff * s.Falloff * max(mass, 1e-8f.xxx));
    return s;
}

// ChannelDensity for all three channels at once (per mm^2), radius already clamped.
float3 ProfileDensityRgb(ProfileShape s, float rMm)
{
    const float3 rr = rMm / s.Falloff;
    const float3 rr2 = rr * rr;
    float3 density = 0.0f.xxx;
    [unroll]
    for (uint k = 0u; k < kProfileTerms; ++k)
        density += kProfileWeight[k] * exp(-rr2 / (2.0f * kProfileVariance[k])) / (2.0f * kPi * kProfileVariance[k]);
    return density * s.InvNorm;
}

// Everything about a subsurface receiver pixel the pass needs.
struct ScatterReceiver
{
    PteroSssProfile Profile;
    ProfileShape    Shape;
    float3 P;
    float3 N;              // facing the camera
    float3 T;              // tangent disc frame (BuildBasis)
    float3 B;
    float  DeviceDepth;
    float  LinearDepth;
    float  MetresPerMm;
    float  RangeMetres;    // the profile range in metres
};

ScatterReceiver MakeReceiver(uint2 pixel, float4 normalDepth, uint slot)
{
    ScatterReceiver s;
    s.Profile = gSssProfiles[slot];
    s.Shape = MakeProfileShape(max(s.Profile.FalloffTranslucency.rgb, 0.0f.xxx));
    s.MetresPerMm = s.Profile.ColorRadius.a / kProfileRangeMm;
    s.RangeMetres = s.Profile.ColorRadius.a;
    s.DeviceDepth = normalDepth.z;
    s.LinearDepth = PteroSssLinearDepth(normalDepth.z);
    s.P = ReconstructWorldPosition(pixel, normalDepth.z);
    s.N = DecodeOctNormal(normalDepth.xy);
    if (dot(s.N, gSssCameraPos - s.P) < 0.0f)
        s.N = -s.N;
    BuildBasis(s.N, s.T, s.B);
    return s;
}

// The profile over the actual distance travelled, clamped to the range as main() does.
float3 ProfileAt(ScatterReceiver s, float3 x)
{
    const float distanceMm = min(length(x - s.P) / max(s.MetresPerMm, 1e-8f), kProfileRangeMm);
    return ProfileDensityRgb(s.Shape, distanceMm);
}

// Projects disc offset u onto the mesh from this receiver, with the probe pair main() fires.
bool ProjectDisc(ScatterReceiver s, float2 u, out SurfaceHit surface)
{
    const float3 discPoint = s.P + s.T * u.x + s.B * u.y;
    return ProbeSurface(discPoint, s.N, max(length(u) * 1.5f, 0.002f), surface);
}

// Weighted reservoir sampling step. Returns true when the candidate was taken.
bool StreamSample(inout float weightSum, float weight, inout uint rng)
{
    weightSum += weight;
    return weight > 0.0f && NextRandom(rng) * weightSum <= weight;
}

[numthreads(8, 8, 1)]
void CSRestirTemporal(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (any(pixel >= (uint2)gSssRenderSize))
        return;
    const uint index = pixel.y * (uint)gSssRenderSize.x + pixel.x;

    const float4 diffuseM = gSssDiffuse.Load(int3(pixel, 0));
    const float4 normalDepth = gSssNormalDepth.Load(int3(pixel, 0));
    const uint slot = PteroDecodeSubsurfaceSlot(normalDepth.w);
    if (diffuseM.a < 0.5f || !PteroSssSlotActive(slot))
    {
        gSssReservoirOut[index] = EmptyPackedSssReservoir();
        gSssOutput[pixel] = float4(diffuseM.rgb, 0.0f);
        return;
    }

    const ScatterReceiver s = MakeReceiver(pixel, normalDepth, slot);
    // What the scattered light is lit with (see WorldSpaceIrradiance).
    const float3 scatterNormal = PixelGeometricNormal(pixel, s.P, s.N);
    const float3 albedo = gSssAlbedo.Load(int3(pixel, 0)).rgb;

    uint rng = PcgHash(pixel.x + pixel.y * 8192u) ^ PcgHash(gSssFrameIndex * 9781u + 17u);
    const uint sampleCount = clamp(gSssRtSamples, 1u, 64u);
    const float rotation = NextRandom(rng);
    const uint channelOffset = (uint)(NextRandom(rng) * 3.0f);
    const float shadowIgnoreDistance = 0.002f * s.LinearDepth + 0.5f * s.RangeMetres;

    // ---- Candidates: the probes main() fires, streamed through three reservoirs ----
    SssReservoir r;
    float weightSum[3] = { 0.0f, 0.0f, 0.0f };
    float selectedTarget[3] = { 0.0f, 0.0f, 0.0f };
    [unroll]
    for (uint c0 = 0u; c0 < 3u; ++c0)
        r.C[c0] = EmptySssSample();
    // D_c - 1, as a control variate: the profile over the disc radius integrates to exactly
    // one (ChannelDensity is normalised over the disc), so only what the real surface does
    // differently is estimated - the mass of probes that missed (mesh edges) and the
    // difference between distance travelled and disc radius (curvature). Estimating D
    // directly was as noisy as the numerator for the narrow channels: the probes are drawn
    // for the mixture of all three profiles. And dividing a smooth reservoir numerator by
    // a noisy D is biased high (E[1/D] > 1/E[D]) - by 2-4% on the harness.
    float3 normalisation = 0.0f.xxx;

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        const uint channel = (i + channelOffset) % 3u;
        const float rMm = SampleChannelRadius(s.Shape.Falloff[channel] - 0.001f, NextRandom(rng), NextRandom(rng));
        const float phi = 2.0f * kPi * frac((i + 0.5f) / sampleCount + rotation);
        const float2 u = float2(cos(phi), sin(phi)) * (rMm * s.MetresPerMm);

        // Density of the disc point (per disc mm^2): one-sample MIS over the channels.
        // The per-channel profile at the disc radius is the control variate's integrand.
        const float3 sourcePdf3 = ProfileDensityRgb(s.Shape, rMm);
        const float sourcePdf = (sourcePdf3.r + sourcePdf3.g + sourcePdf3.b) / 3.0f;
        if (sourcePdf <= 0.0f)
            continue;

        SurfaceHit surface;
        if (!ProjectDisc(s, u, surface))
        {
            normalisation -= sourcePdf3 / sourcePdf;
            continue;
        }

        const float3 profile = ProfileAt(s, surface.Position);
        normalisation += (profile - sourcePdf3) / sourcePdf;

        const float3 irradiance = WorldSpaceIrradiance(surface, scatterNormal, shadowIgnoreDistance);
        const float3 target = profile * irradiance;
        [unroll]
        for (uint c = 0u; c < 3u; ++c)
        {
            if (StreamSample(weightSum[c], target[c] / sourcePdf, rng))
            {
                r.C[c].Disc = u;
                r.C[c].Position = surface.Position;
                r.C[c].Irradiance = irradiance[c];
                selectedTarget[c] = target[c];
            }
        }
    }
    r.M = sampleCount;
    normalisation = 1.0f + normalisation / (float)sampleCount;

    // ---- Temporal reuse ----
    bool historyValid = false;
    SssReservoir history = (SssReservoir)0;
    uint historyM = 0u;
    if (gSssRestirTemporal != 0u)
    {
        const float4 prevClip = mul(float4(s.P, 1.0f), gSssPrevViewProj);
        if (prevClip.w > 1e-5f)
        {
            const float2 prevNdc = prevClip.xy / prevClip.w;
            const float2 prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
            if (all(prevUv >= 0.0f.xx) && all(prevUv < 1.0f.xx))
            {
                const uint2 prevPixel = min(uint2(prevUv * gSssRenderSize), (uint2)gSssRenderSize - 1u);
                const PackedSssReservoir packed = gSssReservoirIn[prevPixel.y * (uint)gSssRenderSize.x + prevPixel.x];
                historyM = min(PackedM(packed), gSssRestirHistoryCap);
                if (historyM > 0u && PackedSlot(packed) == slot)
                {
                    // Same surface: the previous receiver is close and faces the same way.
                    const float3 prevP = ReconstructWorldPositionWith(gSssPrevInvViewProj, prevPixel, PackedReceiverDepth(packed));
                    const float tolerance = 0.5f * s.RangeMetres + 0.004f * s.LinearDepth;
                    historyValid = length(prevP - s.P) <= tolerance
                                && dot(PackedReceiverNormal(packed), s.N) > kSssReuseNormalDot;
                    if (historyValid)
                        history = UnpackSssReservoir(packed);
                }
            }
        }
    }

    // Candidates each channel's reservoir stands for after the merge.
    float channelM[3] = { (float)r.M, (float)r.M, (float)r.M };
    uint mergedM = r.M;
    if (historyValid)
    {
        // Shift each channel's history sample to this receiver and light it there.
        // Channels that kept the same disc point share one probe and one evaluation.
        float2 shiftedDisc = float2(1e30f, 1e30f);
        bool shiftedHit = false;
        SurfaceHit shifted = (SurfaceHit)0;
        float3 shiftedIrradiance = 0.0f.xxx;
        float3 shiftedProfile = 0.0f.xxx;
        float minConfidence = 1.0f;

        [unroll]
        for (uint c = 0u; c < 3u; ++c)
        {
            const SssSample past = history.C[c];
            // The history is trusted only as far as its sample's light is unchanged. A
            // history with no sample, or one that lands off this receiver's surface, can
            // say nothing about that, so it is dropped rather than counted as M candidates
            // that all found darkness: after a light moved onto a dark region, those M
            // divided the fresh probes' light by twenty.
            float confidence = 0.0f;
            if (past.W > 0.0f)
            {
                if (any(past.Disc != shiftedDisc))
                {
                    shiftedDisc = past.Disc;
                    shiftedHit = ProjectDisc(s, past.Disc, shifted);
                    if (shiftedHit)
                    {
                        shiftedIrradiance = WorldSpaceIrradiance(shifted, scatterNormal, shadowIgnoreDistance);
                        shiftedProfile = ProfileAt(s, shifted.Position);
                    }
                }
                if (shiftedHit)
                {
                    // Trust the history in proportion to how little the light at its
                    // sample changed since it was stored.
                    const float before = past.Irradiance;
                    const float now = shiftedIrradiance[c];
                    confidence = max(before, now) > 0.0f ? min(before, now) / max(before, now) : 1.0f;

                    // Streaming merge (Bitterli et al., Alg. 4): the fresh candidates are in
                    // weightSum already (p(y) W M of their reservoir); the history enters with
                    // its target here times its W and confidence-scaled M.
                    const float target = shiftedProfile[c] * now;
                    if (StreamSample(weightSum[c], target * past.W * (float)historyM * confidence, rng))
                    {
                        r.C[c].Disc = past.Disc;
                        r.C[c].Position = shifted.Position;
                        r.C[c].Irradiance = now;
                        selectedTarget[c] = target;
                    }
                }
            }
            channelM[c] += (float)historyM * confidence;
            minConfidence = min(minConfidence, confidence);
        }
        mergedM = r.M + (uint)round((float)historyM * minConfidence);
    }

    [unroll]
    for (uint c1 = 0u; c1 < 3u; ++c1)
        r.C[c1].W = selectedTarget[c1] > 0.0f ? weightSum[c1] / (channelM[c1] * selectedTarget[c1]) : 0.0f;
    r.M = min(mergedM, kSssMaxM);

    // ---- Shade: numerator from the reservoirs, denominator from the probes ----
    // Every chosen sample was lit this frame (a fresh probe, or a history sample just
    // re-projected and relit), so f(y) W is this frame's light.
    float3 numerator = 0.0f.xxx;
    [unroll]
    for (uint c2 = 0u; c2 < 3u; ++c2)
        numerator[c2] = selectedTarget[c2] * r.C[c2].W;

    gSssReservoirOut[index] = PackSssReservoir(r, s.N, s.DeviceDepth, slot);

    const float3 strength = saturate(s.Profile.ColorRadius.rgb);
    const float fogTransmittance = saturate(diffuseM.a * 2.0f - 1.0f);

    // As in main(): the pixel's direct diffuse is replaced by the scattered light, and a
    // channel whose probes found (almost) no surface keeps its own light.
    float3 scatteredDirect = diffuseM.rgb;
    [unroll]
    for (uint c3 = 0u; c3 < 3u; ++c3)
    {
        if (normalisation[c3] > 0.05f)
            scatteredDirect[c3] = albedo[c3] * (numerator[c3] / normalisation[c3]) * fogTransmittance;
    }
    float3 result = lerp(diffuseM.rgb, scatteredDirect, strength);
    result += PixelTransmission(s.Profile, s.P, scatterNormal, albedo) * fogTransmittance;

    gSssOutput[pixel] = float4(max(result, 0.0f.xxx), 1.0f);
}
