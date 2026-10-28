// LightClusters.hlsli
// Clustered light lists (LightClusterRenderer). The scene's lights - every one of
// them, up to PTERO_MAX_SCENE_LIGHTS, not the 16 the per-pass constant buffers hold
// - live in one structured buffer, and LightClusterBuild.hlsl records which of them
// can reach each cluster as a bitmask of PTERO_LIGHT_MASK_WORDS words:
//
//   View clusters  the camera frustum cut into TileSize-pixel tiles and Slices
//                  depth slices (exponential from NearDepth to FarDepth; the last
//                  one runs on to infinity). For anything on screen: the deferred
//                  pass and the volumetric fog.
//   World grid     GridX x GridY x GridZ cubes of GridCellSize metres around the
//                  camera. For points that need not be on screen: GI bounce hits.
//
// A point no cluster covers (behind the camera, off the grid) gets every light, and
// still pays only each light's own range test, exactly as before clustering. With
// Enabled == 0 every point does - the reference to compare clustering against.
//
// Every pass that uses this binds it in register space 7, so it cannot collide with
// the pass's own registers:
//   b0 space7  PteroLightClusterConstants
//   t0 space7  StructuredBuffer<PteroLightData>  the lights
//   t1 space7  StructuredBuffer<float4>          per light: x fog scale (0 = not in the
//                                                fog), y GI scale (pre-exposure folded in)
//   t2 space7  StructuredBuffer<uint>            view cluster masks
//   t3 space7  StructuredBuffer<uint>            world grid masks
//
// Iterating a point's lights:
//
//   const uint clusterBase = PteroViewClusterBase(worldPos);   // or PteroGridCellBase
//   [loop] for (uint word = 0; word < gLightClusters.WordCount; ++word)
//   {
//       uint bits = PteroViewClusterWord(clusterBase, word);   // or PteroGridCellWord
//       [loop] while (bits != 0u)
//       {
//           const uint i = word * 32u + firstbitlow(bits);
//           bits &= bits - 1u;
//           const PteroLightData light = gClusterLights[i];
//           ...
//       }
//   }
//
// Must stay in step with LightClusterGpuConstants in LightClusterRenderer.h.

#ifndef PTERO_LIGHT_CLUSTERS_HLSLI
#define PTERO_LIGHT_CLUSTERS_HLSLI

#include "LightShapes.hlsli"

#define PTERO_MAX_SCENE_LIGHTS 256
#define PTERO_LIGHT_MASK_WORDS (PTERO_MAX_SCENE_LIGHTS / 32)
#define PTERO_NO_CLUSTER       0xFFFFFFFFu

struct PteroLightClusterConstants
{
    float4x4 View;           // world -> view, mul(float4(p, 1), View)
    float    ProjX;          // projection _11, _22, _31, _32, _34: enough to go between
    float    ProjY;          // view space and NDC for any perspective projection
    float    ProjOffsetX;
    float    ProjOffsetY;
    float    ProjW;          // _34: +1 left-handed, -1 right-handed
    float    NearDepth;      // view depth where slice 0 starts
    float    SliceScale;     // Slices / log(FarDepth / NearDepth)
    float    FarDepth;       // view depth where the last slice starts going on forever
    uint     TilesX;
    uint     TilesY;
    uint     Slices;
    uint     LightCount;
    uint     WordCount;      // ceil(LightCount / 32)
    uint     Enabled;        // 0: every lookup answers every light
    uint     GridX;
    uint     GridY;
    float3   GridOrigin;     // world position of the grid's minimum corner
    float    GridCellSize;
    uint     GridZ;
    uint3    _Pad;
};

cbuffer PteroLightClusterBuffer : register(b0, space7)
{
    PteroLightClusterConstants gLightClusters;
};

StructuredBuffer<PteroLightData> gClusterLights      : register(t0, space7);
StructuredBuffer<float4>         gClusterLightExtras : register(t1, space7);
StructuredBuffer<uint>           gClusterViewMasks   : register(t2, space7);
StructuredBuffer<uint>           gClusterGridMasks   : register(t3, space7);

// How far from its position a light can light anything: its radius, plus how far a
// spherical source pulls the sample point toward the surface, plus a rect panel's
// half-diagonal (it lights from the nearest point on the panel).
float PteroLightCullRadius(PteroLightData light)
{
    float reach = light.Radius + min(max(light.SourceRadius, 0.0f), light.Radius * 0.5f);
    if ((int)light.LightType == PTERO_LIGHT_TYPE_RECT)
        reach += length(float2(light.RectHalfWidth, light.RectHalfHeight));
    return reach;
}

// The bits of word `word` that name existing lights: what a point no cluster covers gets.
uint PteroAllLightsWord(uint word)
{
    const uint first = word * 32u;
    if (gLightClusters.LightCount <= first)
        return 0u;
    const uint inWord = gLightClusters.LightCount - first;
    return inWord >= 32u ? 0xFFFFFFFFu : ((1u << inWord) - 1u);
}

// The view slice of a view depth.
uint PteroClusterSlice(float viewDepth)
{
    const float slice = log(max(viewDepth, gLightClusters.NearDepth) / gLightClusters.NearDepth) * gLightClusters.SliceScale;
    return min((uint)max(slice, 0.0f), gLightClusters.Slices - 1u);
}

// First mask word of the view cluster around worldPos, or PTERO_NO_CLUSTER.
uint PteroViewClusterBase(float3 worldPos)
{
    if (gLightClusters.Enabled == 0u)
        return PTERO_NO_CLUSTER;
    const float3 v = mul(float4(worldPos, 1.0f), gLightClusters.View).xyz;
    const float w = v.z * gLightClusters.ProjW;
    if (w <= 1e-4f)
        return PTERO_NO_CLUSTER;
    const float2 ndc = float2(v.x * gLightClusters.ProjX + v.z * gLightClusters.ProjOffsetX,
                              v.y * gLightClusters.ProjY + v.z * gLightClusters.ProjOffsetY) / w;
    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    if (any(uv < 0.0f) || any(uv >= 1.0f))
        return PTERO_NO_CLUSTER;
    const uint2 tile = min(uint2(uv * float2(gLightClusters.TilesX, gLightClusters.TilesY)),
                           uint2(gLightClusters.TilesX, gLightClusters.TilesY) - 1u);
    const uint cluster = (PteroClusterSlice(w) * gLightClusters.TilesY + tile.y) * gLightClusters.TilesX + tile.x;
    return cluster * PTERO_LIGHT_MASK_WORDS;
}

// Not ternaries: both sides of one are evaluated, and a root SRV is not bounds checked.
uint PteroViewClusterWord(uint clusterBase, uint word)
{
    if (clusterBase == PTERO_NO_CLUSTER)
        return PteroAllLightsWord(word);
    return gClusterViewMasks[clusterBase + word];
}

// First mask word of the world grid cell around worldPos, or PTERO_NO_CLUSTER.
uint PteroGridCellBase(float3 worldPos)
{
    if (gLightClusters.Enabled == 0u)
        return PTERO_NO_CLUSTER;
    const float3 cellF = floor((worldPos - gLightClusters.GridOrigin) / gLightClusters.GridCellSize);
    if (any(cellF < 0.0f) || cellF.x >= (float)gLightClusters.GridX
        || cellF.y >= (float)gLightClusters.GridY || cellF.z >= (float)gLightClusters.GridZ)
        return PTERO_NO_CLUSTER;
    const uint3 cell = uint3(cellF);
    return ((cell.z * gLightClusters.GridY + cell.y) * gLightClusters.GridX + cell.x) * PTERO_LIGHT_MASK_WORDS;
}

uint PteroGridCellWord(uint cellBase, uint word)
{
    if (cellBase == PTERO_NO_CLUSTER)
        return PteroAllLightsWord(word);
    return gClusterGridMasks[cellBase + word];
}

#endif // PTERO_LIGHT_CLUSTERS_HLSLI
