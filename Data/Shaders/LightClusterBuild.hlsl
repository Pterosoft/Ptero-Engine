// LightClusterBuild.hlsl
// Builds the light masks LightClusters.hlsli reads (LightClusterRenderer): one thread
// per view cluster (CSBuildView) or world grid cell (CSBuildGrid) tests every light's
// reach, as a sphere, against the cell's bounding box and writes one bit per light.
//
// The view cluster's box is the axis-aligned box, in view space, around its slice of
// the tile's frustum, so the test is conservative: a light near a cluster can be
// listed without reaching it, never the other way round. The shading passes still
// apply each light's own range test, so a spare bit only costs that test.

#include "LightClusters.hlsli"

RWStructuredBuffer<uint> gViewMasksOut : register(u0, space7);
RWStructuredBuffer<uint> gGridMasksOut : register(u1, space7);

bool SphereIntersectsBox(float3 centre, float radius, float3 boxMin, float3 boxMax)
{
    const float3 nearest = clamp(centre, boxMin, boxMax);
    const float3 d = centre - nearest;
    return dot(d, d) <= radius * radius;
}

// Writes the masks of one cell, given its box, in the space the light centres are
// transformed into (view space for clusters, world space for the grid).
void WriteMasks(RWStructuredBuffer<uint> masks, uint cellIndex, float3 boxMin, float3 boxMax, bool viewSpace)
{
    const uint base = cellIndex * PTERO_LIGHT_MASK_WORDS;
    [unroll]
    for (uint word = 0; word < PTERO_LIGHT_MASK_WORDS; ++word)
    {
        uint bits = 0u;
        const uint first = word * 32u;
        [loop]
        for (uint b = 0; b < 32u; ++b)
        {
            const uint i = first + b;
            if (i >= gLightClusters.LightCount)
                break;
            const PteroLightData light = gClusterLights[i];
            const float3 centre = viewSpace ? mul(float4(light.Position, 1.0f), gLightClusters.View).xyz : light.Position;
            if (SphereIntersectsBox(centre, PteroLightCullRadius(light), boxMin, boxMax))
                bits |= 1u << b;
        }
        masks[base + word] = bits;
    }
}

// View-space point at an NDC position and a view depth (the projected w).
float3 ViewPointAt(float2 ndc, float depth)
{
    const float z = depth / gLightClusters.ProjW;
    return float3((ndc.x * depth - z * gLightClusters.ProjOffsetX) / gLightClusters.ProjX,
                  (ndc.y * depth - z * gLightClusters.ProjOffsetY) / gLightClusters.ProjY,
                  z);
}

[numthreads(64, 1, 1)]
void CSBuildView(uint3 id : SV_DispatchThreadID)
{
    const uint clusterCount = gLightClusters.TilesX * gLightClusters.TilesY * gLightClusters.Slices;
    const uint cluster = id.x;
    if (cluster >= clusterCount)
        return;

    const uint tileX = cluster % gLightClusters.TilesX;
    const uint tileY = (cluster / gLightClusters.TilesX) % gLightClusters.TilesY;
    const uint slice = cluster / (gLightClusters.TilesX * gLightClusters.TilesY);

    const float2 uv0 = float2(tileX, tileY) / float2(gLightClusters.TilesX, gLightClusters.TilesY);
    const float2 uv1 = float2(tileX + 1u, tileY + 1u) / float2(gLightClusters.TilesX, gLightClusters.TilesY);
    const float2 ndc0 = float2(uv0.x * 2.0f - 1.0f, 1.0f - uv1.y * 2.0f);
    const float2 ndc1 = float2(uv1.x * 2.0f - 1.0f, 1.0f - uv0.y * 2.0f);

    // Slice 0 also holds everything nearer than NearDepth, the last everything past
    // FarDepth.
    const float depth0 = slice == 0u ? 1e-3f
        : gLightClusters.NearDepth * exp((float)slice / gLightClusters.SliceScale);
    const float depth1 = slice + 1u >= gLightClusters.Slices ? 1e7f
        : gLightClusters.NearDepth * exp((float)(slice + 1u) / gLightClusters.SliceScale);

    float3 boxMin = float3(1e30f, 1e30f, 1e30f);
    float3 boxMax = -boxMin;
    [unroll]
    for (uint corner = 0; corner < 8u; ++corner)
    {
        const float2 ndc = float2((corner & 1u) ? ndc1.x : ndc0.x, (corner & 2u) ? ndc1.y : ndc0.y);
        const float3 p = ViewPointAt(ndc, (corner & 4u) ? depth1 : depth0);
        boxMin = min(boxMin, p);
        boxMax = max(boxMax, p);
    }

    WriteMasks(gViewMasksOut, cluster, boxMin, boxMax, true);
}

[numthreads(64, 1, 1)]
void CSBuildGrid(uint3 id : SV_DispatchThreadID)
{
    const uint cellCount = gLightClusters.GridX * gLightClusters.GridY * gLightClusters.GridZ;
    const uint cellIndex = id.x;
    if (cellIndex >= cellCount)
        return;

    const uint3 cell = uint3(cellIndex % gLightClusters.GridX,
                             (cellIndex / gLightClusters.GridX) % gLightClusters.GridY,
                             cellIndex / (gLightClusters.GridX * gLightClusters.GridY));
    const float3 boxMin = gLightClusters.GridOrigin + float3(cell) * gLightClusters.GridCellSize;
    WriteMasks(gGridMasksOut, cellIndex, boxMin, boxMin + gLightClusters.GridCellSize, false);
}
