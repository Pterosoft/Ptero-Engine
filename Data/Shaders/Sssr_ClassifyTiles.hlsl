// Sssr_ClassifyTiles.hlsl
// FidelityFX SSSR pass 2: decides per pixel whether a reflection ray is needed and appends
// the ones that are to a compact ray list; lists every 8x8 tile holding glossy pixels for
// the denoiser. Ported from the SSSR 1.3 sample's ClassifyTiles.hlsl.
//
// It also extracts what the later passes (and next frame's reprojection) need from the
// G-Buffer into this frame's half of the ping-ponged denoiser inputs: perceptual
// roughness, a decoded world normal, and a copy of depth. Writing them here replaces the
// sample's end-of-frame history copies.
//
// Differences from the sample:
//   - Surfaces too rough to trace get zero, not an environment map lookup. The engine has
//     no reflection cubemap; its deferred resolve already applies an ambient specular term
//     to them, and SSSR adds on top of that.
//   - Glass and sky are never reflective here (glass draws its own reflections).
//
// Uses Wave intrinsics (WaveReadLaneAt, WavePrefixCountBits), so this needs SM6.

#include "Sssr_Common.hlsli"
#include "ffx_denoiser_reflections_common.h"

Texture2D<float4> gGBufferNormal   : register(t0);   // .xy oct normal
Texture2D<float4> gGBufferMaterial : register(t1);   // .r roughness
Texture2D<float>  gDepthBuffer     : register(t2);
Texture2D<float>  gVarianceHistory : register(t3);

RWTexture2D<float4> gIntersectionOutput : register(u0);
RWTexture2D<float>  gExtractedRoughness : register(u1);
RWTexture2D<float4> gDenoiserNormal     : register(u2);
RWTexture2D<float>  gDepthCopy          : register(u3);

RWStructuredBuffer<uint>                  gRayList           : register(u4);
globallycoherent RWStructuredBuffer<uint> gRayCounter        : register(u5);
RWStructuredBuffer<uint>                  gDenoiserTileList  : register(u6);

groupshared uint gTileCount;

bool IsBaseRay(uint2 dispatchThreadId, uint samplesPerQuad)
{
    switch (samplesPerQuad)
    {
    case 1:
        return ((dispatchThreadId.x & 1) | (dispatchThreadId.y & 1)) == 0; // one ray per quad
    case 2:
        return (dispatchThreadId.x & 1) == (dispatchThreadId.y & 1);        // keeps the diagonal
    default:
        return true;                                                         // four per quad
    }
}

void ClassifyTiles(uint2 dispatchThreadId, uint2 groupThreadId, float roughness, bool isReflectiveSurface)
{
    gTileCount = 0;

    const bool isFirstLaneOfWave = WaveIsFirstLane();

    bool needsRay = !(dispatchThreadId.x >= gBufferDimensions.x || dispatchThreadId.y >= gBufferDimensions.y);

    const bool isGlossyReflection = FFX_DNSR_Reflections_IsGlossyReflection(roughness);
    needsRay = needsRay && isGlossyReflection && isReflectiveSurface;

    // Mirrors resolve exactly from one ray, so they skip the denoiser.
    const bool needsDenoiser = needsRay && !FFX_DNSR_Reflections_IsMirrorReflection(roughness);

    const bool isBaseRay = IsBaseRay(dispatchThreadId, gSamplesPerQuad);
    needsRay = needsRay && (!needsDenoiser || isBaseRay); // never drop a mirror ray

    if (gTemporalVarianceGuidedTracingEnabled != 0 && needsDenoiser && !needsRay)
    {
        const bool hasTemporalVariance = gVarianceHistory.Load(int3(dispatchThreadId, 0)) > gTemporalVarianceThreshold;
        needsRay = needsRay || hasTemporalVariance;
    }

    GroupMemoryBarrierWithGroupSync(); // gTileCount cleared

    if (isGlossyReflection && isReflectiveSurface)
        InterlockedAdd(gTileCount, 1);

    // A base ray also fills in the quad neighbours that want a result but no ray of their own.
    const bool requireCopy = !needsRay && needsDenoiser;
    const bool copyHorizontal = (gSamplesPerQuad != 4) && isBaseRay && WaveReadLaneAt(requireCopy, WaveGetLaneIndex() ^ 0x1);
    const bool copyVertical   = (gSamplesPerQuad == 1) && isBaseRay && WaveReadLaneAt(requireCopy, WaveGetLaneIndex() ^ 0x2);
    const bool copyDiagonal   = (gSamplesPerQuad == 1) && isBaseRay && WaveReadLaneAt(requireCopy, WaveGetLaneIndex() ^ 0x3);

    // Compact the wave's rays and append them with one atomic.
    const uint localRayIndexInWave = WavePrefixCountBits(needsRay);
    const uint waveRayCount = WaveActiveCountBits(needsRay);
    uint baseRayIndex = 0;
    if (isFirstLaneOfWave)
    {
        InterlockedAdd(gRayCounter[0], waveRayCount, baseRayIndex);
    }
    baseRayIndex = WaveReadLaneFirst(baseRayIndex);
    if (needsRay)
    {
        gRayList[baseRayIndex + localRayIndexInWave] =
            PteroSssrPackRayCoords(dispatchThreadId, copyHorizontal, copyVertical, copyDiagonal);
    }

    // Everything starts out reflecting nothing; the intersection pass overwrites traced pixels.
    gIntersectionOutput[dispatchThreadId] = 0.0f.xxxx;

    GroupMemoryBarrierWithGroupSync(); // gTileCount complete

    if (all(groupThreadId == 0) && gTileCount > 0)
    {
        uint tileOffset;
        InterlockedAdd(gRayCounter[1], 1, tileOffset);
        gDenoiserTileList[tileOffset] = ((dispatchThreadId.y & 0xFFFFu) << 16) | (dispatchThreadId.x & 0xFFFFu);
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint2 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    // Lane remap keeps each 2x2 quad on four neighbouring lanes, which the Wave reads rely on.
    const uint2 groupThreadId = FFX_DNSR_Reflections_RemapLane8x8(groupIndex);
    const uint2 dispatchThreadId = groupId * 8 + groupThreadId;
    const int3 load = int3(dispatchThreadId, 0);

    const float  depth    = gDepthBuffer.Load(load);
    const float4 material = gGBufferMaterial.Load(load);
    const bool   isGlass  = PteroSssrIsGlass(material);

    // Sky and glass read as fully rough: never glossy, so nothing traces or denoises them.
    const bool  isReflectiveSurface = depth < 1.0f && !isGlass;
    const float roughness = isReflectiveSurface ? saturate(material.r) : 1.0f;

    ClassifyTiles(dispatchThreadId, groupThreadId, roughness, isReflectiveSurface);

    const float3 normal = isReflectiveSurface
        ? PteroSssrDecodeOctNormal(gGBufferNormal.Load(load).xy)
        : float3(0.0f, 0.0f, 1.0f);

    gExtractedRoughness[dispatchThreadId] = roughness;
    gDenoiserNormal[dispatchThreadId]     = float4(normal * 0.5f + 0.5f, 0.0f);
    gDepthCopy[dispatchThreadId]          = depth;
}
