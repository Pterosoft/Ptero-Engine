// LensFlare_Trace.hlsl
//
// Compute half of the lens flare, run every frame the flare is on:
//   CSOcclusion - how much of each light is visible (depth test, plus a sun-disc
//                 brightness test for the sun so clouds and fog dim its flare)
//   CSBounds    - per light and ghost, the part of the entrance plane whose rays survive
//                 to the sensor, from a coarse 16x16 trace over the whole plane
//   CSTrace     - the fine ray grid inside those bounds, per light, ghost and wavelength
// LensFlare_Draw.hlsl then rasterises the grids. See LensFlare_Common.hlsli.

#include "LensFlare_Common.hlsli"

Texture2D<float4> gInputColor : register(t0);
Texture2D<float>  gDepth      : register(t1);

RWStructuredBuffer<LensFlareGridEntry> gGridOut       : register(u0);
RWStructuredBuffer<float4>             gBoundsOut     : register(u1);
RWStructuredBuffer<float>              gVisibilityOut : register(u2);

// ---------------------------------------------------------------------------------
// Occlusion
// ---------------------------------------------------------------------------------

#define LF_OCCLUSION_TAPS 64

groupshared float2 sOcclusion[LF_OCCLUSION_TAPS];

[numthreads(LF_OCCLUSION_TAPS, 1, 1)]
void CSOcclusion(uint3 group : SV_GroupID, uint tap : SV_GroupIndex)
{
    const LensFlareLight light = gLights[group.x];

    // Golden-angle spiral over the light's disc.
    const float radius = sqrt((tap + 0.5f) / LF_OCCLUSION_TAPS) * light.Screen.z;
    const float angle = tap * 2.39996323f;
    const float2 uv = light.Screen.xy + float2(cos(angle) * radius * gStarburst.z, sin(angle) * radius);

    float visible = 1.0f;
    float weight = 0.0f;
    if (all(uv >= 0.0f) && all(uv < 1.0f))
    {
        weight = 1.0f;

        uint depthWidth, depthHeight;
        gDepth.GetDimensions(depthWidth, depthHeight);
        const float depth = gDepth.Load(int3(uv * float2(depthWidth, depthHeight), 0));

        if (light.Screen.w <= 0.0f)
        {
            // The sun: only the sky (cleared depth) lets it through.
            visible = depth >= 0.99999f ? 1.0f : 0.0f;
        }
        else if (depth < 0.99999f)
        {
            // Device depth -> view depth for a standard perspective projection.
            const float viewDepth = gOcclusion.z / (depth - gOcclusion.y);
            visible = viewDepth >= light.Screen.w - gOcclusion.x ? 1.0f : 0.0f;
        }

        if (visible > 0.0f && light.Extra.z > 0.0f)
        {
            uint colorWidth, colorHeight;
            gInputColor.GetDimensions(colorWidth, colorHeight);
            const float3 color = gInputColor.Load(int3(uv * float2(colorWidth, colorHeight), 0)).rgb;
            visible *= saturate(LensFlareLuminance(color) / light.Extra.z);
        }
    }

    sOcclusion[tap] = float2(visible * weight, weight);
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = LF_OCCLUSION_TAPS / 2; stride > 0; stride >>= 1)
    {
        if (tap < stride)
            sOcclusion[tap] += sOcclusion[tap + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (tap == 0)
    {
        // A light entirely off screen cannot be tested; the CPU fades it out at the edge.
        const float current = sOcclusion[0].y > 0.0f ? sOcclusion[0].x / sOcclusion[0].y : 1.0f;
        const uint slot = (uint)light.Extra.x;
        const float previous = gVisibilityOut[slot];
        gVisibilityOut[slot] = light.Extra.y > 0.5f ? current : lerp(previous, current, gOcclusion.w);
    }
}

// ---------------------------------------------------------------------------------
// Ghost bounds
// ---------------------------------------------------------------------------------

#define LF_BOUNDS_GRID 16

groupshared float4 sBounds[LF_BOUNDS_GRID * LF_BOUNDS_GRID];

[numthreads(LF_BOUNDS_GRID, LF_BOUNDS_GRID, 1)]
void CSBounds(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const uint ghostCount = gCounts.y;
    const uint lightIndex = group.z / ghostCount;
    const uint ghost = group.z % ghostCount;
    const LensFlareLight light = gLights[lightIndex];

    // Two scans: the whole front element, and the region the direct path gets through the
    // iris. A stopped-down lens has a pupil a few millimetres wide, which the first scan
    // alone can step straight over. Each hit carries one cell of its own scan as slack:
    // the true edge lies between samples.
    const float pupil = gLens.y;
    const uint channel = gCounts.z / 2;
    const uint2 surfaces = LensFlareGhostSurfaces(ghost);
    float4 bounds = float4(1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f);

    {
        const float cell = 2.0f * pupil / (LF_BOUNDS_GRID - 1);
        const float2 start = -pupil.xx + float2(thread.xy) * cell;
        const LensFlareTraceResult hit = LensFlareTraceRay(channel, surfaces, float3(start, gLens.x), light.DirectionLocal.xyz);
        // Slightly generous: dispersion moves the other wavelengths a little.
        if (hit.Valid && hit.RelRadius <= 1.05f && LensFlareApertureDistance(hit.Aperture) <= 1.05f)
            bounds = float4(start - cell, start + cell);
    }

    if (light.Pupil.x <= light.Pupil.z)
    {
        const float2 cell = (light.Pupil.zw - light.Pupil.xy) / (LF_BOUNDS_GRID - 1);
        const float2 start = light.Pupil.xy + float2(thread.xy) * cell;
        const LensFlareTraceResult hit = LensFlareTraceRay(channel, surfaces, float3(start, gLens.x), light.DirectionLocal.xyz);
        if (hit.Valid && hit.RelRadius <= 1.05f && LensFlareApertureDistance(hit.Aperture) <= 1.05f)
            bounds = float4(min(bounds.xy, start - cell), max(bounds.zw, start + cell));
    }

    sBounds[index] = bounds;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = (LF_BOUNDS_GRID * LF_BOUNDS_GRID) / 2; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            const float4 a = sBounds[index];
            const float4 b = sBounds[index + stride];
            sBounds[index] = float4(min(a.xy, b.xy), max(a.zw, b.zw));
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0)
    {
        float4 result = sBounds[0];
        if (result.x <= result.z)
        {
            result.xy = max(result.xy, -pupil.xx);
            result.zw = min(result.zw, pupil.xx);
        }
        gBoundsOut[group.z] = result;
    }
}

// ---------------------------------------------------------------------------------
// Ray grid
// ---------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSTrace(uint3 group : SV_GroupID, uint3 id : SV_DispatchThreadID)
{
    const uint gridSize = gCounts.x;
    if (id.x >= gridSize || id.y >= gridSize)
        return;

    const uint channelCount = gCounts.z;
    const uint ghostCount = gCounts.y;
    const uint channel = group.z % channelCount;
    const uint lightGhost = group.z / channelCount;
    const uint ghost = lightGhost % ghostCount;
    const LensFlareLight light = gLights[lightGhost / ghostCount];

    const uint entry = group.z * gridSize * gridSize + id.y * gridSize + id.x;
    const float4 bounds = gBoundsOut[lightGhost];

    LensFlareGridEntry result;
    result.Ndc = 0.0f.xx;
    result.Aperture = 0.0f.xx;
    result.Intensity = -1.0f;
    result.RelRadius = 2.0f;

    if (bounds.x <= bounds.z)
    {
        const float2 start = lerp(bounds.xy, bounds.zw, float2(id.xy) / (gridSize - 1));
        const LensFlareTraceResult hit = LensFlareTraceRay(
            channel, LensFlareGhostSurfaces(ghost), float3(start, gLens.x), light.DirectionLocal.xyz);

        if (hit.Valid)
        {
            result.Ndc = -LensFlareRotate(hit.Sensor, light.Rotation) * gTanHalf.zw;
            result.Aperture = LensFlareRotate(hit.Aperture, light.Rotation);
            result.Intensity = hit.Intensity;
            result.RelRadius = hit.RelRadius;
        }
    }

    gGridOut[entry] = result;
}
