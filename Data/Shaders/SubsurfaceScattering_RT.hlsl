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

#define PTERO_SSS_CB_REGISTER b0
#include "Subsurface.hlsli"
#include "SurfaceSpecular.hlsli"

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
Texture2D                gSssSunShadowMap    : register(t8);
Texture2DArray           gSssPointShadowMaps : register(t9);
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
    const float3x3 o2wRot = float3x3(
        float3(o2w[0][0], o2w[1][0], o2w[2][0]),
        float3(o2w[0][1], o2w[1][1], o2w[2][1]),
        float3(o2w[0][2], o2w[1][2], o2w[2][2]));

    float3 vertexPos[3];
    float3 vertexNrm[3];
    [unroll]
    for (uint k = 0u; k < 3u; ++k)
    {
        const GpuPackedVertex v = gSssVertices[info.vertexOffset + gSssIndices[info.indexOffset + primitive * 3u + k]];
        const float4 local = float4(v.px, v.py, v.pz, 1.0f);
        vertexPos[k] = float3(dot(o2w[0], local), dot(o2w[1], local), dot(o2w[2], local));
        vertexNrm[k] = mul(o2wRot, float3(v.nx, v.ny, v.nz));
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
float SunShadowVisibility(float3 worldPos)
{
    if (gSssHasSunShadow == 0u)
        return 1.0f;

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
    if (gSssHasPointShadows == 0u || light.CastShadows < 0.5f)
        return 1.0f;
    const int shadowIndex = (int)light.ShadowIndex;
    if (shadowIndex < 0 || shadowIndex >= 4)
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
// shadingNormal is the pixel's G-Buffer normal (normal map included), not the hit's
// interpolated vertex normal: the samples lie within the scatter radius of the pixel, and
// on a scanned or remeshed asset the vertex normals follow the scan terraces the normal
// map was baked to hide, which striped the result. ignoreDistance skips occluders closer
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
        irradiance += gSssSunColor * sunNdotL * SunShadowVisibility(surface.RawPosition);

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
    // The SDK's wrap term: nothing comes through once the light is well in front.
    if (dot(L, N) >= 0.3f)
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
        weightedSum += WorldSpaceIrradiance(surface, N, shadowIgnoreDistance) * weight;
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
    const float maxThickness = PteroSssMaxTransmissionThickness(profile);
    if (gSssTransmission != 0u && maxThickness > 0.0f)
    {
        const float3 L_sun = normalize(-gSssSunDirection);
        result += TracedTransmission(profile, P, N, L_sun, 1.0e4f, gSssSunColor, albedo, maxThickness) * fogTransmittance;

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

            result += TracedTransmission(profile, P, N, toLight / dist, dist,
                                         light.Color * falloffTerm, albedo, maxThickness) * fogTransmittance;
        }
    }

    gSssOutput[pixel] = float4(max(result, 0.0f.xxx), 1.0f);
}
