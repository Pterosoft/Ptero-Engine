// VoxelGI_Common.hlsli
// Shared by every voxel GI pass (VoxelGIRenderer): the constants, the clipmap
// layout and the cone tracer.
//
// The clipmap is a stack of LevelCount cubes of Resolution^3 voxels centred on
// the camera, each twice the voxel size of the one inside it. Every per-voxel
// quantity lives in one Texture3D per quantity with the levels stacked along Z
// (level L occupies slices [L * Resolution, (L + 1) * Resolution)), so a pass
// can reach any level through a single descriptor. Sampling clamps Z inside the
// level so the linear filter never blends one level into the next.
//
// Every level's origin is snapped to a multiple of its own voxel size on one
// world-anchored grid, so a coarse voxel always covers exactly 2x2x2 voxels of
// the level inside it.
//
// SM 5.0 throughout: the startup shader cache compiles these as cs_5_0, and
// this pass exists for GPUs that may have nothing newer.

#ifndef PTERO_VOXEL_GI_COMMON_HLSLI
#define PTERO_VOXEL_GI_COMMON_HLSLI

#include "LightShapes.hlsli"
#include "SkyModel.hlsli"
// PteroVsmConstants, for light visibility from the shadow map (RtGI_Vsm.hlsli).
#include "VirtualShadowMap.hlsli"

#ifndef VOXEL_GI_CB_REGISTER
#define VOXEL_GI_CB_REGISTER b0
#endif

#define VOXEL_GI_MAX_LEVELS 6
#define VOXEL_GI_MAX_POINT_LIGHTS 16

// Mirrors VoxelGIConstants in VoxelGIRenderer.h.
cbuffer VoxelGIConstants : register(VOXEL_GI_CB_REGISTER)
{
    // xyz: world position of the level's voxel (0,0,0) corner; w: voxel size.
    float4 gLevelOrigin[VOXEL_GI_MAX_LEVELS];
    // Where the level's indirect-light texture was last computed; w = 1 when valid.
    float4 gIndirectOrigin[VOXEL_GI_MAX_LEVELS];

    float4x4 gViewProjInv;              // transposed inverse jittered view-projection

    float3 gCameraPos;      float gMaxTraceDistance;
    float3 gSunDir;         float gSurfaceOffset;      // gSunDir points from the sun into the scene
    float3 gSunColor;       float gMultiBounce;        // pre-exposed
    float3 gSkyColor;       float gSpecularRoughnessThreshold;

    uint   gResolution;
    uint   gLevelCount;
    uint   gUpdateLevel;
    uint   gDiffuseCones;

    uint   gFullWidth;
    uint   gFullHeight;
    uint   gTraceWidth;
    uint   gTraceHeight;

    float  gInvResolution;
    uint   gSunShadows;
    uint   gLocalLightShadows;
    int    gDebugView;

    int    gNumPointLights;
    uint   gHalfResolution;
    uint   gSpecularEnabled;
    float  gSpecularIntensity;

    PteroLightData gPointLights[VOXEL_GI_MAX_POINT_LIGHTS];

    PteroSkyModel gSky;     // the sky escaped cones see, pre-exposed (SkyModel.hlsli)

    // The level being updated, where its previous content sat (xyz) and w = 1
    // when that content may be blended with (InjectCS's temporal smoothing).
    float4 gPrevLevelOrigin;
    float  gTemporalBlend;  // share of the new lighting a voxel takes this update; 1 = none kept
    float3 _TemporalPad;

    // Lighting the voxels, as RTGI lights its bounce hits (VoxelGISettings): light
    // visibility from the virtual shadow map before a march through the voxels, and
    // a few stochastically picked local lights rather than every one. Named as
    // RtGI_Vsm.hlsli expects them.
    float  g_VsmMaxTexelSize;   // coarsest shadow-map texel (metres) trusted at a voxel
    int    g_VsmVisibility;     // 1 = ask the shadow map before marching
    int    g_LightSamples;      // > 0: local lights a voxel tests, picked stochastically
    uint   gLightSampleSeed;    // changes every update, so the picks do too
    PteroVsmConstants g_Vsm;    // g_Vsm.Active == 0 when the map is not bound
};

static const float kVoxelPi = 3.14159265f;

float VoxelSizeOf(uint level)
{
    return gLevelOrigin[level].w;
}

// Integer voxel of `worldPos` in `level`; may be outside [0, Resolution).
int3 WorldToVoxel(float3 worldPos, uint level)
{
    return int3(floor((worldPos - gLevelOrigin[level].xyz) / VoxelSizeOf(level)));
}

float3 VoxelCenterWorld(int3 voxel, uint level)
{
    return gLevelOrigin[level].xyz + (float3(voxel) + 0.5f) * VoxelSizeOf(level);
}

bool VoxelInside(int3 voxel)
{
    return all(voxel >= 0) && all(voxel < (int)gResolution);
}

// Texel in the stacked texture.
int4 StackedTexel(int3 voxel, uint level)
{
    return int4(voxel.xy, voxel.z + (int)(level * gResolution), 0);
}

// Whether `worldPos` sits inside `level` with `marginVoxels` to spare, so a
// trilinear tap there never reaches past the level's edge.
bool InsideLevel(float3 worldPos, uint level, float marginVoxels)
{
    const float3 local = (worldPos - gLevelOrigin[level].xyz) / VoxelSizeOf(level);
    return all(local >= marginVoxels) && all(local <= (float)gResolution - marginVoxels);
}

// Finest level containing `worldPos`, or gLevelCount when none does.
uint FinestLevelContaining(float3 worldPos, float marginVoxels)
{
    [loop]
    for (uint level = 0; level < gLevelCount; ++level)
    {
        if (InsideLevel(worldPos, level, marginVoxels))
            return level;
    }
    return gLevelCount;
}

// Normalised coordinates of `worldPos` in the stacked texture, with Z clamped
// half a voxel inside the level so the filter stays in it.
float3 StackedUvw(float3 worldPos, uint level)
{
    float3 local = (worldPos - gLevelOrigin[level].xyz) / (VoxelSizeOf(level) * (float)gResolution);
    const float halfTexel = 0.5f * gInvResolution;
    local.z = clamp(local.z, halfTexel, 1.0f - halfTexel);
    return float3(local.xy, (local.z + (float)level) / (float)gLevelCount);
}

float4 SampleVoxelLevel(Texture3D<float4> volume, SamplerState linearClamp, float3 worldPos, uint level)
{
    return volume.SampleLevel(linearClamp, StackedUvw(worldPos, level), 0.0f);
}

// Samples at a fractional level of detail: a blend of the two levels around
// `lod`, never finer than `finest`, the finest level that contains the point
// (FinestLevelContaining, which must be < gLevelCount).
float4 SampleClipmap(Texture3D<float4> volume, SamplerState linearClamp, float3 worldPos, float lod, uint finest, out float voxelSize)
{
    lod = max(lod, (float)finest);
    const uint lower = min((uint)lod, gLevelCount - 1);
    const uint upper = min(lower + 1, gLevelCount - 1);
    const float blend = (upper == lower) ? 0.0f : saturate(lod - (float)lower);

    voxelSize = lerp(VoxelSizeOf(lower), VoxelSizeOf(upper), blend);
    const float4 a = SampleVoxelLevel(volume, linearClamp, worldPos, lower);
    if (blend <= 0.0f)
        return a;
    // The coarser level may not reach this far out even though the finer one
    // does; it then has nothing to add.
    if (!InsideLevel(worldPos, upper, 1.0f))
        return a;
    return lerp(a, SampleVoxelLevel(volume, linearClamp, worldPos, upper), blend);
}

// Same sky the RTGI miss shader returns - the one that is drawn (SkyModel.hlsli) -
// so the GI solutions agree on what an escaped ray sees.
float3 VoxelSkyRadiance(float3 dir)
{
    return PteroSkyRadiance(gSky, normalize(dir));
}

// Front-to-back cone march through premultiplied radiance (rgb) and opacity (a).
// `tanHalfAngle` sets how fast the footprint grows; `startDistance` is where the
// cone leaves its origin. Escaping cones pick up the sky in proportion to how
// much of them got out.
float4 ConeTrace(
    Texture3D<float4> radiance,
    SamplerState linearClamp,
    float3 origin,
    float3 dir,
    float tanHalfAngle,
    float startDistance,
    float maxDistance,
    bool addSky)
{
    const float finestVoxel = VoxelSizeOf(0);
    float4 accum = 0.0f.xxxx;
    float t = startDistance;

    [loop]
    for (uint step = 0; step < 128 && t < maxDistance && accum.a < 0.98f; ++step)
    {
        const float3 p = origin + dir * t;
        const uint finest = FinestLevelContaining(p, 1.0f);
        if (finest >= gLevelCount)
            break;

        const float diameter = max(2.0f * t * tanHalfAngle, finestVoxel);
        const float lod = log2(diameter / finestVoxel);

        float voxelSize;
        float4 s = SampleClipmap(radiance, linearClamp, p, lod, finest, voxelSize);

        // The stored opacity is per voxel; a step that covers more or less than
        // one voxel's depth sees correspondingly more or less of it.
        const float stepLength = max(diameter * 0.5f, voxelSize * 0.5f);
        if (s.a > 0.0f)
        {
            const float corrected = 1.0f - pow(saturate(1.0f - s.a), stepLength / voxelSize);
            s.rgb *= corrected / s.a;
            s.a = corrected;
        }

        accum.rgb += (1.0f - accum.a) * s.rgb;
        accum.a   += (1.0f - accum.a) * s.a;
        t += stepLength;
    }

    if (addSky)
        accum.rgb += (1.0f - accum.a) * VoxelSkyRadiance(dir);
    return accum;
}

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    const float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// Cosine-weighted average radiance over the hemisphere around `n` - the same
// quantity (irradiance / pi) the RTGI buffer holds - from 1, 5 or 9 cones.
// `rotation` turns the ring of side cones, so neighbouring pixels can use
// different ones.
float3 TraceDiffuse(
    Texture3D<float4> radiance,
    SamplerState linearClamp,
    float3 origin,
    float3 n,
    float startDistance,
    float maxDistance,
    uint coneCount,
    float rotation,
    out float occlusion)
{
    float3 t, b;
    BuildBasis(n, t, b);

    float3 sum = 0.0f.xxx;
    float weightSum = 0.0f;
    occlusion = 0.0f;

    if (coneCount <= 1)
    {
        // One wide cone: a 120 degree aperture covers most of the hemisphere.
        const float4 c = ConeTrace(radiance, linearClamp, origin, n, 1.73f, startDistance, maxDistance, true);
        occlusion = c.a;
        return c.rgb;
    }

    // Centre cone plus a ring tilted 50 degrees off the normal; 60 degree apertures
    // tile the hemisphere with little overlap. Nine cones split the ring in two.
    {
        const float4 c = ConeTrace(radiance, linearClamp, origin, n, 0.577f, startDistance, maxDistance, true);
        sum += c.rgb;
        occlusion += c.a;
        weightSum += 1.0f;
    }

    const uint ringCount = coneCount >= 9 ? 8u : 4u;
    const float tilt = coneCount >= 9 ? 0.96f : 0.87f;      // radians off the normal
    const float aperture = coneCount >= 9 ? 0.42f : 0.577f; // tan of half the cone angle
    const float cosTilt = cos(tilt), sinTilt = sin(tilt);

    [loop]
    for (uint i = 0; i < ringCount; ++i)
    {
        const float phi = rotation + (6.2831853f * (float)i) / (float)ringCount;
        const float3 dir = normalize(n * cosTilt + (t * cos(phi) + b * sin(phi)) * sinTilt);
        const float4 c = ConeTrace(radiance, linearClamp, origin, dir, aperture, startDistance, maxDistance, true);
        sum += c.rgb * cosTilt;
        occlusion += c.a * cosTilt;
        weightSum += cosTilt;
    }

    occlusion /= weightSum;
    return sum / weightSum;
}

// Fraction of light that gets from `origin` along `dir` within `maxDistance`,
// marched through the voxel opacity. Starts in `startLevel` (or the finest
// level containing the origin, if coarser) and steps half a voxel of whichever
// level the march is in: with whole-voxel steps a ray crossing a one-voxel
// wall can sample it only where the filter has faded to half, either side,
// and a quarter of the light gets through.
float TraceVisibility(
    Texture3D<float4> opacityVolume,
    SamplerState linearClamp,
    float3 origin,
    float3 dir,
    float maxDistance,
    uint startLevel,
    uint maxSteps)
{
    float transmittance = 1.0f;
    float t = VoxelSizeOf(startLevel);

    [loop]
    for (uint step = 0; step < maxSteps && t < maxDistance && transmittance > 0.02f; ++step)
    {
        const float3 p = origin + dir * t;
        const uint level = max(startLevel, FinestLevelContaining(p, 1.0f));
        if (level >= gLevelCount)
            break;
        const float occupancy = SampleVoxelLevel(opacityVolume, linearClamp, p, level).a;
        transmittance *= 1.0f - saturate(occupancy);
        t += 0.5f * VoxelSizeOf(level);
    }
    return transmittance;
}

// Karis' analytic fit of the split-sum environment BRDF - the same one the SSSR
// composite uses, so a reflection that SSR takes over from these voxels is
// weighted identically on both sides of the hand-over.
float3 VoxelEnvBrdfApprox(float3 F0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    const float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

float3 DecodeOctNormalVoxel(float2 encoded)
{
    const float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

// Inverse of DecodeOctNormalVoxel: a unit normal to [0, 1]^2.
float2 EncodeOctNormalVoxel(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
    float2 oct = n.xy;
    if (n.z < 0.0f)
        oct = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return saturate(oct * 0.5f + 0.5f);
}

// The voxelizer's per-voxel keys (VoxelGI_Voxelize.hlsl, VoxelGI_Resolve.hlsl).
// Every triangle covering a voxel InterlockedMax-es its key in, so the voxel
// ends up with the same surface whatever order the GPU rasterized them in.
// A plain store let the last writer win, and with no fixed order between
// overlapping triangles a voxel holding two - the two faces of a thin wall, a
// corner - swapped albedo and normal from one re-voxelization to the next, and
// the GI flickered with it. 0 is an empty voxel.
uint PackVoxelAlbedoKey(float3 albedo, float coverage)
{
    // Coverage in the top byte: the most opaque surface wins, then the brightest.
    const uint4 q = uint4(round(saturate(float4(albedo, coverage)) * 255.0f));
    return (max(q.a, 1u) << 24) | (q.b << 16) | (q.g << 8) | q.r;
}

float4 UnpackVoxelAlbedoKey(uint key)
{
    return float4(key & 0xffu, (key >> 8) & 0xffu, (key >> 16) & 0xffu, key >> 24) / 255.0f;
}

uint PackVoxelNormalKey(float3 n)
{
    const uint2 q = uint2(round(EncodeOctNormalVoxel(n) * 65535.0f));
    return (q.x << 16) | q.y;
}

float3 UnpackVoxelNormalKey(uint key)
{
    return DecodeOctNormalVoxel(float2(key >> 16, key & 0xffffu) / 65535.0f);
}

float3 ReconstructWorldPosVoxel(float2 uv, float depth)
{
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    const float4 world = mul(float4(ndc, depth, 1.0f), gViewProjInv);
    return world.xyz / world.w;
}

#endif // PTERO_VOXEL_GI_COMMON_HLSLI
