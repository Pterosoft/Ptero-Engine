// VoxelGI_Update.hlsl
// Per-level clipmap update passes of the voxel GI (VoxelGIRenderer), run after
// a level has been voxelized and resolved (VoxelGI_Resolve.hlsl):
//
//   ClearVolumeCS  zeroes a whole volume (when the clipmap is (re)created)
//   InjectCS       lights the level's voxels - sun, local lights, and the last
//                  bounce of indirect light - into outgoing radiance, blended
//                  with what the voxel held before (gTemporalBlend). Where the
//                  level overlaps the finer one inside it, it takes the average
//                  of the eight finer voxels instead, so coarse voxels carry
//                  partial coverage and agree with the finer level.
//   BounceCS       cone traces the lit clipmap from each voxel, storing the
//                  irradiance it receives for the next InjectCS (multi-bounce).
//
// Radiance is stored premultiplied by opacity (rgb = radiance * a, a = opacity),
// which is what the cone tracer composites.

#include "VoxelGI_Common.hlsli"

// Shadow-map visibility (t40, t41), shared with RTGI. It reads the camera position
// under RTGI's name.
#define g_CameraPos gCameraPos
#include "RtGI_Vsm.hlsli"
#undef g_CameraPos

// Every scene light, clustered, in space 7 (LightClusterRenderer) - what InjectCS lights
// the voxels from when gUseLightClusters is set, as RTGI lights its bounce hits.
#include "LightClusters.hlsli"

#define VOXEL_GI_MAX_LIGHT_SAMPLES 4

Texture3D<float4> gAlbedo    : register(t0);   // rgb albedo, a coverage
Texture3D<float4> gNormal    : register(t1);   // xyz normal * 0.5 + 0.5
Texture3D<float4> gRadiance  : register(t2);   // premultiplied radiance + opacity, all levels
Texture3D<float4> gIndirect  : register(t3);   // received irradiance / pi, all levels

RWTexture3D<float4> gOutput  : register(u0);
RWTexture3D<float4> gOutput1 : register(u1);

SamplerState gLinearClamp : register(s0);

[numthreads(4, 4, 4)]
void ClearVolumeCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height, depth;
    gOutput.GetDimensions(width, height, depth);
    if (id.x >= width || id.y >= height || id.z >= depth)
        return;
    gOutput[id] = 0.0f.xxxx;
}

// Where light leaving the voxel's surface starts: the first point along the
// normal clear of solid voxels, up to three voxels out. A surface lying on a
// voxel boundary fills two layers, and the outer layer lit from just one voxel
// out would start inside the inner one, shadow itself and go black - which the
// filter then smears over the visible face.
float3 SurfaceExit(float3 center, float3 n, float voxelSize, uint level)
{
    [unroll]
    for (int k = 1; k <= 3; ++k)
    {
        const float3 p = center + n * (voxelSize * (float)k);
        const int3 v = WorldToVoxel(p, level);
        if (!VoxelInside(v) || gAlbedo.Load(StackedTexel(v, level)).a <= 0.0f)
            return p;
    }
    return center + n * (3.0f * voxelSize);
}

float3 SunLight(float3 exitPoint, float3 n, uint level)
{
    const float3 L = -gSunDir;
    const float nDotL = saturate(dot(n, L));
    if (nDotL <= 0.0f)
        return 0.0f.xxx;

    float visibility = 1.0f;
    if (gSunShadows != 0)
    {
        // The shadow map first, from the voxel's surface on the sun's side; the march
        // only where it has no fine enough page. A voxel is far coarser than a texel
        // the map is trusted at, so where it answers it answers better than the march.
        visibility = RtgiVsmSunVisibility(exitPoint, n);
        if (visibility < 0.0f)
            visibility = TraceVisibility(gAlbedo, gLinearClamp, exitPoint, L, 1.0e4f, level, 192);
    }
    return gSunColor * nDotL * visibility;
}

// Local light i as the voxels see it: from the clustered list - every scene light, its
// colour times its GI scale with the pre-exposure folded in - or the legacy 16.
PteroLightData VoxelLocalLight(uint i)
{
    if (gUseLightClusters != 0u)
    {
        PteroLightData light = gClusterLights[i];
        light.Color *= gClusterLightExtras[i].y;
        return light;
    }
    return gPointLights[i];
}

// The local lights that can reach `position`, 32 to a word: the world grid cell's
// list (LightClusters.hlsli; every light off the grid), or the legacy 16 in word 0.
uint VoxelLightWordCount()
{
    return gUseLightClusters != 0u ? gLightClusters.WordCount : 1u;
}

uint VoxelLightCell(float3 position)
{
    if (gUseLightClusters != 0u)
        return PteroGridCellBase(position);
    return PTERO_NO_CLUSTER;
}

uint VoxelLightWord(uint cell, uint word)
{
    if (gUseLightClusters != 0u)
        return PteroGridCellWord(cell, word);
    const uint count = (uint)clamp(gNumPointLights, 0, VOXEL_GI_MAX_POINT_LIGHTS);
    return (1u << count) - 1u;
}

uint VoxelLightCount()
{
    return gUseLightClusters != 0u ? gLightClusters.LightCount : (uint)clamp(gNumPointLights, 0, VOXEL_GI_MAX_POINT_LIGHTS);
}

// Local light i's contribution to a voxel before shadowing; false when it adds nothing.
bool LocalLightUnshadowed(PteroLightData light, float3 position, float3 n, out float3 contribution, out float3 lightPosition)
{
    contribution = 0.0f.xxx;
    lightPosition = 0.0f.xxx;

    const PteroResolvedLight shape = PteroResolveLightShape(light, position);
    if (shape.ShapeMask <= 0.0f)
        return false;

    const float3 toLight = shape.Position - position;
    const float distSq = dot(toLight, toLight);
    const float normDistSq = saturate(distSq * light.InvRadiusSq);
    if (normDistSq >= 1.0f)
        return false;

    const float distance = sqrt(max(distSq, 1.0e-6f));
    const float3 L = toLight / distance;
    const float nDotL = saturate(dot(n, L));
    if (nDotL <= 0.0f)
        return false;

    lightPosition = shape.Position;
    contribution = light.Color * nDotL * PteroLightDistanceFalloff(light, distance) * shape.ShapeMask;
    return any(contribution > 0.0f);
}

// A local light's visibility from a voxel, marched from `exitPoint` - after the
// shadow map, where it has a fine enough page.
float LocalLightVisibility(PteroLightData light, float3 lightPosition, float3 exitPoint, float3 n, float voxelSize, uint level)
{
    const float3 toLightFromExit = lightPosition - exitPoint;
    const float exitDistance = length(toLightFromExit);
    if (gLocalLightShadows == 0 || exitDistance <= 2.0f * voxelSize)
        return 1.0f;

    const float3 L = toLightFromExit / exitDistance;
    const float3 lightSideNormal = n * (dot(n, L) >= 0.0f ? 1.0f : -1.0f);
    const float visibility = RtgiVsmLocalVisibility(light, exitPoint, lightSideNormal);
    if (visibility >= 0.0f)
        return visibility;
    return TraceVisibility(gAlbedo, gLinearClamp, exitPoint, L, exitDistance - voxelSize, level, 96);
}

uint VoxelLightRng(uint3 id, uint level)
{
    uint state = id.x * 73856093u ^ id.y * 19349663u ^ id.z * 83492791u ^ level * 2654435761u ^ gLightSampleSeed;
    state = state * 747796405u + 2891336453u;
    state = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return ((state >> 22u) ^ state) | 1u;
}

float VoxelRandFloat(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) * (1.0f / float(1 << 24));
}

// Same falloff and shape handling as RTGI's bounce lighting, so local lights
// reach the voxels as far as they reach anything. Shadows are marched from
// `exitPoint`, falloff and shape from the voxel itself. Only the lights the voxel's
// world grid cell lists are considered, as for RTGI's hits.
//
// With more lights in reach than g_LightSamples, that many are picked in
// proportion to their unshadowed luminance and only those are shadow tested, each
// weighted by 1 / (probability x samples) - RTGI's stochastic light selection. The
// picks change every update and the temporal blend averages them.
float3 LocalLights(float3 position, float3 exitPoint, float3 n, float voxelSize, uint level, uint3 id)
{
    float3 sum = 0.0f.xxx;
    const uint cell = VoxelLightCell(position);
    const uint wordCount = VoxelLightWordCount();

    [branch]
    if (g_LightSamples > 0 && VoxelLightCount() > (uint)g_LightSamples)
    {
        uint rng = VoxelLightRng(id, level);
        const int samples = min(g_LightSamples, VOXEL_GI_MAX_LIGHT_SAMPLES);
        int picked[VOXEL_GI_MAX_LIGHT_SAMPLES] = { -1, -1, -1, -1 };
        float totalWeight = 0.0f;
        int candidates = 0;

        [loop]
        for (uint word = 0; word < wordCount; ++word)
        {
          uint bits = VoxelLightWord(cell, word);
          [loop]
          while (bits != 0u)
          {
            const uint i = word * 32u + firstbitlow(bits);
            bits &= bits - 1u;
            float3 contribution, lightPosition;
            if (!LocalLightUnshadowed(VoxelLocalLight(i), position, n, contribution, lightPosition))
                continue;
            const float weight = dot(contribution, float3(0.2126f, 0.7152f, 0.0722f));
            if (weight <= 0.0f)
                continue;
            ++candidates;
            totalWeight += weight;
            [unroll]
            for (int s = 0; s < VOXEL_GI_MAX_LIGHT_SAMPLES; ++s)
            {
                if (s < samples && VoxelRandFloat(rng) * totalWeight < weight)
                    picked[s] = (int)i;
            }
          }
        }

        // No more lights in reach here than samples: the exact loop below costs no more.
        [branch]
        if (candidates > samples)
        {
            [unroll]
            for (int s = 0; s < VOXEL_GI_MAX_LIGHT_SAMPLES; ++s)
            {
                if (s >= samples || picked[s] < 0)
                    continue;
                const PteroLightData light = VoxelLocalLight((uint)picked[s]);
                float3 contribution, lightPosition;
                LocalLightUnshadowed(light, position, n, contribution, lightPosition);
                const float weight = dot(contribution, float3(0.2126f, 0.7152f, 0.0722f));
                if (weight <= 0.0f)
                    continue;
                const float visibility = LocalLightVisibility(light, lightPosition, exitPoint, n, voxelSize, level);
                sum += contribution * (visibility * totalWeight / (weight * (float)samples));
            }
            return sum;
        }
    }

    [loop]
    for (uint word = 0; word < wordCount; ++word)
    {
      uint bits = VoxelLightWord(cell, word);
      [loop]
      while (bits != 0u)
      {
        const uint i = word * 32u + firstbitlow(bits);
        bits &= bits - 1u;
        const PteroLightData light = VoxelLocalLight(i);
        float3 contribution, lightPosition;
        if (!LocalLightUnshadowed(light, position, n, contribution, lightPosition))
            continue;
        sum += contribution * LocalLightVisibility(light, lightPosition, exitPoint, n, voxelSize, level);
      }
    }
    return sum;
}

// Whether the finer level inside `level` covers this voxel entirely, and if so
// the first of the 2x2x2 finer voxels it spans.
bool CoveredByFinerLevel(float3 center, uint level, out int3 childBase)
{
    childBase = int3(0, 0, 0);
    if (level == 0)
        return false;
    const uint fine = level - 1;
    // A coarse centre lies on a corner shared by its eight finer voxels.
    const float3 fineCoord = (center - gLevelOrigin[fine].xyz) / VoxelSizeOf(fine);
    childBase = int3(round(fineCoord)) - 1;
    return all(childBase >= 0) && all(childBase + 1 < (int)gResolution);
}

[numthreads(4, 4, 4)]
void InjectCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution))
        return;

    const uint level = gUpdateLevel;
    const int3 voxel = int3(id);
    const float3 center = VoxelCenterWorld(voxel, level);

    // Inside the finer level, the voxel is built from the eight it covers.
    // Coarse voxels voxelized on their own are solid wherever any triangle
    // passes through them, which makes the distant clipmap far too opaque.
    //
    // Opacity is not their plain average, though: a wall one fine voxel thick
    // would come out half as opaque per level, and a wide cone - which reads
    // the coarse levels - would see the sky straight through a closed room.
    // Instead each axis composites the children front to back in pairs and
    // averages the four pairs, which is how much of the voxel's face along that
    // axis is blocked; the most blocked axis wins. A wall, floor or ceiling -
    // even a diagonal one - stays opaque through every level, while scattered
    // geometry still thins out with distance. Radiance keeps its colour
    // (radiance / opacity) and takes the new opacity.
    int3 childBase;
    if (CoveredByFinerLevel(center, level, childBase))
    {
        float4 child[8];
        float4 sum = 0.0f.xxxx;
        [unroll]
        for (int c = 0; c < 8; ++c)
        {
            const int3 offset = int3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
            child[c] = gRadiance.Load(StackedTexel(childBase + offset, level - 1));
            sum += child[c];
        }

        // Children c and c ^ step are neighbours along the axis `step` selects.
        float coverage = 0.0f;
        [unroll]
        for (uint axis = 0; axis < 3; ++axis)
        {
            const uint step = 1u << axis;
            float blocked = 0.0f;
            [unroll]
            for (uint c = 0; c < 8; ++c)
            {
                if ((c & step) == 0u)
                    blocked += 1.0f - (1.0f - child[c].a) * (1.0f - child[c | step].a);
            }
            coverage = max(coverage, blocked * 0.25f);
        }

        const float4 average = sum * 0.125f;
        const float3 radiance = average.a > 1.0e-4f ? average.rgb * (coverage / average.a) : 0.0f.xxx;
        gOutput[id] = float4(radiance, coverage);
        return;
    }

    const int4 texel = StackedTexel(voxel, level);
    const float4 albedo = gAlbedo.Load(texel);
    if (albedo.a <= 0.0f)
    {
        gOutput[id] = 0.0f.xxxx;
        return;
    }

    const float3 n = normalize(gNormal.Load(texel).xyz * 2.0f - 1.0f);
    const float voxelSize = VoxelSizeOf(level);

    const float3 exitPoint = SurfaceExit(center, n, voxelSize, level);
    float3 irradiance = SunLight(exitPoint, n, level) + LocalLights(center, exitPoint, n, voxelSize, level, id);

    // Last update's bounce, looked up by world position: the level may have
    // scrolled since, so its texel need not be this one.
    if (gMultiBounce > 0.0f && gIndirectOrigin[level].w > 0.0f)
    {
        const int3 previous = int3(floor((center - gIndirectOrigin[level].xyz) / voxelSize));
        if (VoxelInside(previous))
            irradiance += gIndirect.Load(StackedTexel(previous, level)).rgb * gMultiBounce;
    }

    // Same convention as the deferred resolve and RTGI's hit shading: outgoing
    // radiance is albedo times the incoming light term.
    float3 radiance = albedo.rgb * irradiance;

    // Blend with what this voxel's world position held before. Without it each
    // update replaced the light outright, so the levels - refreshed on
    // different frames - caught flickering lights at different moments and
    // the multi-bounce feedback never settled. Only the colour is blended: the
    // opacity is this fill's, so geometry that moved leaves no ghost in the
    // cones, at most a short tint.
    if (gPrevLevelOrigin.w > 0.0f && gTemporalBlend < 1.0f)
    {
        const int3 previous = int3(floor((center - gPrevLevelOrigin.xyz) / voxelSize));
        if (VoxelInside(previous))
        {
            const float4 old = gRadiance.Load(StackedTexel(previous, level));
            if (old.a > 1.0e-3f)
                radiance = lerp(old.rgb / old.a, radiance, saturate(gTemporalBlend));
        }
    }

    gOutput[id] = float4(radiance * albedo.a, albedo.a);
}

[numthreads(4, 4, 4)]
void BounceCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution))
        return;

    const uint level = gUpdateLevel;
    const int3 voxel = int3(id);
    const int4 texel = StackedTexel(voxel, level);

    // Voxels InjectCS fills from the finer level never read their own bounce.
    int3 childBase;
    if (gAlbedo.Load(texel).a <= 0.0f || CoveredByFinerLevel(VoxelCenterWorld(voxel, level), level, childBase))
    {
        gOutput[texel.xyz] = 0.0f.xxxx;
        return;
    }

    const float3 n = normalize(gNormal.Load(texel).xyz * 2.0f - 1.0f);
    const float voxelSize = VoxelSizeOf(level);
    const float3 origin = SurfaceExit(VoxelCenterWorld(voxel, level), n, voxelSize, level);

    float occlusion;
    const float3 irradiance = TraceDiffuse(gRadiance, gLinearClamp, origin, n, 0.5f * voxelSize,
                                           gMaxTraceDistance, 5u, 0.0f, occlusion);
    gOutput[texel.xyz] = float4(irradiance, 1.0f);
}
