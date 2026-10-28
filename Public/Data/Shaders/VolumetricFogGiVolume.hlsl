// VolumetricFogGiVolume.hlsl
// The fog's world-space indirect light, built from whichever screen-space GI runs
// (RTGI's or voxel GI's diffuse output: irradiance / pi, demodulated, pre-exposed).
//
// SplatCS  - every few screen pixels, the GI arriving at the visible surface is
//            added to the grid cell just in front of it (half a cell along the
//            normal, out into the air), as fixed point through atomics.
// ResolveCS - per cell: a cell that received splats blends their average into its
//            history; a cell on screen that received none (air, or a surface hidden
//            behind another) takes the light of its neighbours, so it spreads from
//            the surfaces into the room a cell per frame; a cell off screen keeps
//            what it had, so turning the camera does not forget the room behind it.
//
// Each texel holds premultiplied radiance in rgb and a confidence in a, which the
// fog divides out after trilinear filtering so empty cells do not darken their
// neighbours.
//
// Root signature (VolumetricFogRenderer::CreateRootSignatures, GI volume):
//   b0 FogConstants, t0 scene depth, t1 normal G-Buffer, t2 screen GI,
//   u0 splat accumulation (raw), t3 last frame's volume, u1 this frame's volume.

#include "VolumetricFogCommon.hlsli"

Texture2D                 gSceneDepth    : register(t0);
Texture2D<float4>         gGBufferNormal : register(t1);   // xy: oct normal in [0, 1]
Texture2D<float4>         gScreenGi      : register(t2);
RWByteAddressBuffer       gGiAccum       : register(u0);   // per cell: r, g, b (fixed point), count
Texture3D<float4>         gGiPrevious    : register(t3);
RWTexture3D<float4>       gGiNext        : register(u1);

// Fixed point for the atomics: 1/4096 steps, values clamped to 64 (pre-exposed, so
// anything above is a firefly), and at most kSplatCap splats per cell per frame -
// enough for an average and well clear of overflow (64 * 4096 * 16384 < 2^32).
static const float kSplatScale = 4096.0f;
static const float kSplatMax   = 64.0f;
static const uint  kSplatCap   = 16384u;

// How fast a cell follows new light, and how much confidence a cell keeps when its
// light came from a neighbour rather than from a surface.
static const float kSplatBlend       = 0.3f;
static const float kPropagateBlend   = 0.25f;
static const float kPropagateFalloff = 0.85f;

float3 DecodeOctNormal01(float2 encoded)
{
    const float2 octN = encoded * 2.0f - 1.0f;
    const float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    const float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));
}

[numthreads(8, 8, 1)]
void SplatCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // A different pixel of each stride x stride block every frame, so over a few
    // frames every pixel contributes.
    const uint stride = max(gGiVolumeSplatStride, 1u);
    const uint2 offset = uint2(gGiVolumeFrame % stride, (gGiVolumeFrame / stride) % stride);
    const uint2 pixel = dispatchThreadId.xy * stride + offset;
    if (any(pixel >= uint2(gFrameWidth, gFrameHeight)))
        return;

    const float depth = gSceneDepth.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f || depth >= 1.0f)
        return;

    const float3 gi = gScreenGi.Load(int3(pixel, 0)).rgb;
    if (any(isnan(gi)) || any(isinf(gi)))
        return;

    const float2 uv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
    const float3 surface = ReconstructWorldPosition(uv, depth);
    const float3 normal = DecodeOctNormal01(gGBufferNormal.Load(int3(pixel, 0)).xy);
    const float3 air = surface + normal * (gGiVolumeCellSize * 0.5f);

    const int3 cell = int3(floor(air * gGiVolumeInvCellSize));
    if (!GiVolumeContains(gGiVolumeOrigin.xyz, cell))
        return;

    const uint address = GiVolumeLinearIndex(GiVolumeTexel(cell)) * 16u;
    uint previousCount;
    gGiAccum.InterlockedAdd(address + 12u, 1u, previousCount);
    if (previousCount >= kSplatCap)
        return;

    const uint3 fixedPoint = uint3(clamp(gi, 0.0f.xxx, kSplatMax.xxx) * kSplatScale + 0.5f);
    gGiAccum.InterlockedAdd(address + 0u, fixedPoint.x);
    gGiAccum.InterlockedAdd(address + 4u, fixedPoint.y);
    gGiAccum.InterlockedAdd(address + 8u, fixedPoint.z);
}

// Last frame's premultiplied value for a world cell, or zero where last frame's
// window did not hold it.
float4 PreviousCell(int3 worldCell)
{
    if (gGiVolumePrevOrigin.w == 0 || !GiVolumeContains(gGiVolumePrevOrigin.xyz, worldCell))
        return 0.0f.xxxx;
    return gGiPrevious.Load(int4(GiVolumeTexel(worldCell), 0));
}

bool CellOnScreen(int3 worldCell)
{
    const float3 centre = (float3(worldCell) + 0.5f) * gGiVolumeCellSize;
    const float4 clip = mul(float4(centre, 1.0f), gCurrViewProj);
    if (clip.w <= 0.0f)
        return false;
    const float3 ndc = clip.xyz / clip.w;
    return all(abs(ndc.xy) <= 1.0f);
}

[numthreads(4, 4, 4)]
void ResolveCS(uint3 texel : SV_DispatchThreadID)
{
    const int3 dims = GiVolumeDims();
    if (any(int3(texel) >= dims))
        return;

    // The world cell this texel holds in the current window.
    const int3 origin = gGiVolumeOrigin.xyz;
    const int3 worldCell = origin + ((int3(texel) - int3(GiVolumeTexel(origin)) + dims) % dims);

    // This frame's splats, read and cleared for the next frame.
    const uint address = GiVolumeLinearIndex(texel) * 16u;
    const uint4 splat = gGiAccum.Load4(address);
    gGiAccum.Store4(address, uint4(0u, 0u, 0u, 0u));

    const float4 history = PreviousCell(worldCell);
    float  confidence = history.a;
    float3 radiance = confidence > 1e-4f ? history.rgb / confidence : 0.0f.xxx;

    const uint count = min(splat.w, kSplatCap);
    if (count > 0u)
    {
        const float3 measured = float3(splat.xyz) / (float(count) * kSplatScale);
        radiance = confidence > 1e-4f ? lerp(radiance, measured, kSplatBlend) : measured;
        confidence = lerp(confidence, 1.0f, kSplatBlend);
    }
    else if (CellOnScreen(worldCell))
    {
        // Air, or a surface something else hides: the neighbours' light.
        static const int3 kNeighbours[6] =
        {
            int3(1, 0, 0), int3(-1, 0, 0), int3(0, 1, 0), int3(0, -1, 0), int3(0, 0, 1), int3(0, 0, -1)
        };
        float3 premultipliedSum = 0.0f.xxx;
        float  confidenceSum = 0.0f;
        float  confidenceMax = 0.0f;
        [unroll]
        for (uint i = 0; i < 6u; ++i)
        {
            const float4 neighbour = PreviousCell(worldCell + kNeighbours[i]);
            premultipliedSum += neighbour.rgb;
            confidenceSum += neighbour.a;
            confidenceMax = max(confidenceMax, neighbour.a);
        }
        if (confidenceSum > 1e-4f)
        {
            const float3 propagated = premultipliedSum / confidenceSum;
            radiance = confidence > 1e-4f ? lerp(radiance, propagated, kPropagateBlend) : propagated;
            confidence = lerp(confidence, confidenceMax * kPropagateFalloff, kPropagateBlend);
        }
        else
        {
            confidence *= 0.9f;
        }
    }
    // Off screen with nothing new: keep what it had.

    gGiNext[texel] = float4(radiance * confidence, confidence);
}
