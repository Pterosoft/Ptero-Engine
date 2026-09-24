// Sssr_Common.hlsli
// Shared by every pass of AMD FidelityFX Stochastic Screen Space Reflections (SSSR 1.3)
// and its reflection denoiser. The passes themselves are ports of the SSSR 1.3 sample
// shaders; this file is the part that had to be rewritten, because it is where the sample
// assumed Cauldron's conventions and this engine has its own:
//
//   - Matrices are uploaded transposed and used as mul(v, M), the way every other pass in
//     this engine does it, rather than the sample's mul(M, v).
//   - The denoiser asks for "view space" positions only to measure lengths and to go back
//     to world space. Camera-relative world space answers both identically without a view
//     matrix, so that is what the FFX_DNSR_Reflections_*ViewSpace callbacks use.
//   - Depth is the engine's non-inverted [0,1] device depth with the sky at 1, which is
//     the range ffx_sssr.h assumes by default (FFX_SSSR_INVERTED_DEPTH_RANGE stays off).
//   - Roughness is the G-Buffer's perceptual roughness. It drives every threshold and the
//     denoiser; only the GGX sampler squares it into alpha.
//
// The pass-internal normal textures store 0.5 * n + 0.5 (R10G10B10A2), which is the
// encoding the denoiser headers decode, so the callbacks can hand them through unchanged.

#ifndef PTERO_SSSR_COMMON_HLSLI
#define PTERO_SSSR_COMMON_HLSLI

// The denoiser headers are written in min16float and were built by the sample with
// -enable-16bit-types. Without that flag (the engine does not pass it) their isnan/isinf
// on min-precision values fail DXIL validation, so run them at full precision instead.
#define min16float  float
#define min16float2 float2
#define min16float3 float3
#define min16float4 float4

cbuffer SssrConstants : register(b0)
{
    float4x4 gViewProj;          // world -> clip, this frame (jittered, as rendered)
    float4x4 gInvViewProj;       // clip -> world, this frame
    float4x4 gPrevViewProj;      // world -> clip, previous frame
    float3   gCameraPos;         float gTemporalStabilityFactor;
    float3   gCameraForward;     float gDepthBufferThickness;
    uint2    gBufferDimensions;  float2 gInvBufferDimensions;
    float    gRoughnessThreshold;
    float    gTemporalVarianceThreshold;
    uint     gFrameIndex;
    uint     gMaxTraversalIntersections;
    uint     gMinTraversalOccupancy;
    uint     gMostDetailedMip;
    uint     gSamplesPerQuad;
    uint     gTemporalVarianceGuidedTracingEnabled;
    uint     gHistoryValid;      // 0 on the first frame after (re)creation or a gap
    float    gIntensity;
    uint     gDebugView;
    float    _SssrPad0;
};

// ------------------------------------------------------------------ G-Buffer decoding

// Matches DeferredLighting.hlsl / Ssr.hlsl: RT1.xy holds the oct-encoded world normal.
float3 PteroSssrDecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    }
    return normalize(n);
}

// Glass repacks the material target (.a = encoded IOR, .b = dispersion) and draws its own
// reflections, so its channels must not be read as roughness/metallic. Same test as Ssr.hlsl.
bool PteroSssrIsGlass(float4 material)
{
    return material.a > (1.0f / 255.0f) && material.b < 0.75f;
}

// ------------------------------------------------------------------ Space conversions

// uv in [0,1] with +v down, z = device depth -> world position.
float3 PteroSssrScreenToWorld(float3 screenUvz)
{
    const float4 ndc = float4(screenUvz.x * 2.0f - 1.0f, 1.0f - screenUvz.y * 2.0f, screenUvz.z, 1.0f);
    const float4 world = mul(ndc, gInvViewProj);
    return world.xyz / world.w;
}

// World position -> (uv, device depth) through the given world -> clip matrix.
float3 PteroSssrProjectPosition(float3 worldPos, float4x4 viewProj)
{
    float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    clip.xyz /= clip.w;
    return float3(clip.x * 0.5f + 0.5f, 0.5f - clip.y * 0.5f, clip.z);
}

// Screen-space direction of a world-space ray, as the difference of two projected points.
float3 PteroSssrProjectDirection(float3 worldOrigin, float3 worldDirection, float3 screenOrigin)
{
    return PteroSssrProjectPosition(worldOrigin + worldDirection, gViewProj) - screenOrigin;
}

// ------------------------------------------------- FFX_DNSR_Reflections_ overrides

bool FFX_DNSR_Reflections_IsGlossyReflection(float roughness)
{
    return roughness < gRoughnessThreshold;
}

bool FFX_DNSR_Reflections_IsMirrorReflection(float roughness)
{
    return roughness < 0.0001f;
}

// "View space" = camera-relative world space; see the header comment.
float3 FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3 screenUvz)
{
    return PteroSssrScreenToWorld(screenUvz) - gCameraPos;
}

float3 FFX_DNSR_Reflections_ViewSpaceToWorldSpace(float4 viewSpaceCoord)
{
    return viewSpaceCoord.xyz + gCameraPos;
}

float3 FFX_DNSR_Reflections_WorldSpaceToScreenSpacePrevious(float3 worldPos)
{
    return PteroSssrProjectPosition(worldPos, gPrevViewProj);
}

// Planar view depth, which is what the denoiser's edge-stopping weights were tuned on.
float FFX_DNSR_Reflections_GetLinearDepth(float2 uv, float depth)
{
    return abs(dot(PteroSssrScreenToWorld(float3(uv, depth)) - gCameraPos, gCameraForward));
}

uint FFX_DNSR_Reflections_RoundedDivide(uint value, uint divisor)
{
    return (value + divisor - 1) / divisor;
}

// ------------------------------------------------------------------ Ray list packing

uint PteroSssrPackRayCoords(uint2 rayCoord, bool copyHorizontal, bool copyVertical, bool copyDiagonal)
{
    const uint rayX15 = rayCoord.x & 0x7FFFu;
    const uint rayY14 = rayCoord.y & 0x3FFFu;
    return (copyDiagonal ? (1u << 31) : 0u)
         | (copyVertical ? (1u << 30) : 0u)
         | (copyHorizontal ? (1u << 29) : 0u)
         | (rayY14 << 15)
         | rayX15;
}

void PteroSssrUnpackRayCoords(uint packed, out uint2 rayCoord, out bool copyHorizontal, out bool copyVertical, out bool copyDiagonal)
{
    rayCoord.x     = packed & 0x7FFFu;
    rayCoord.y     = (packed >> 15) & 0x3FFFu;
    copyHorizontal = ((packed >> 29) & 1u) != 0u;
    copyVertical   = ((packed >> 30) & 1u) != 0u;
    copyDiagonal   = ((packed >> 31) & 1u) != 0u;
}

// ------------------------------------------------------------ Indirect dispatch layout
// Written by Sssr_PrepareIndirectArgs.hlsl, read as ExecuteIndirect arguments and, through
// a root SRV, by the passes it launches:
//   [0..2] intersection dispatch   [3..5] denoiser dispatch
//   [6]    ray count               [7]    tile count
// A 1D dispatch caps at 65535 groups, which a 4K frame exceeds in tiles alone, so large
// counts are split into rows of kPteroSssrMaxGroupsX and flattened back here.
static const uint kPteroSssrMaxGroupsX = 65535u;
static const uint kPteroSssrArgsRayCount  = 6u;
static const uint kPteroSssrArgsTileCount = 7u;

uint PteroSssrFlattenGroupId(uint2 groupId)
{
    return groupId.y * kPteroSssrMaxGroupsX + groupId.x;
}

// The denoiser passes only run on 8x8 tiles that ClassifyTiles listed.
int2 PteroSssrTileDispatchThread(uint packedTile, int2 groupThreadId)
{
    return int2(packedTile & 0xFFFFu, (packedTile >> 16) & 0xFFFFu) + groupThreadId;
}

#endif // PTERO_SSSR_COMMON_HLSLI
