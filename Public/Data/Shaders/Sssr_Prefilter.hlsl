// Sssr_Prefilter.hlsl
// FidelityFX reflection denoiser, pass 2 of 3: an edge-stopped spatial filter over the
// traced radiance, weighted by normal, depth, and distance from the tile's average
// radiance. Runs indirectly, one group per listed tile. Ported from the SSSR 1.3 sample's
// Prefilter.hlsl around ffx_denoiser_reflections_prefilter.h.
//
// Part of the SSSR pass set, which is compiled for SM6 because its other passes use Wave
// intrinsics.

#include "Sssr_Common.hlsli"

Texture2D<float>  gDepthBuffer     : register(t0);
Texture2D<float>  gRoughness       : register(t1);
Texture2D<float3> gNormal          : register(t2);
Texture2D<float3> gAverageRadiance : register(t3);
Texture2D<float4> gInRadiance      : register(t4);
Texture2D<float>  gInVariance      : register(t5);
Texture2D<float>  gInSampleCount   : register(t6);
StructuredBuffer<uint> gDenoiserTileList : register(t7);
StructuredBuffer<uint> gIndirectArgs : register(t8);   // layout in Sssr_Common.hlsli

SamplerState gLinearSampler : register(s0);

RWTexture2D<float4> gOutRadiance    : register(u0);
RWTexture2D<float>  gOutVariance    : register(u1);
RWTexture2D<float>  gOutSampleCount : register(u2);

min16float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv)
{
    return (min16float3)gAverageRadiance.SampleLevel(gLinearSampler, uv, 0.0f).xyz;
}

min16float FFX_DNSR_Reflections_LoadRoughness(int2 p)
{
    return (min16float)gRoughness.Load(int3(p, 0));
}

void FFX_DNSR_Reflections_LoadNeighborhood(
    int2 p,
    out min16float3 radiance,
    out min16float variance,
    out min16float3 normal,
    out float depth,
    int2 screenSize)
{
    radiance = (min16float3)gInRadiance.Load(int3(p, 0)).xyz;
    variance = (min16float)gInVariance.Load(int3(p, 0)).x;
    normal   = normalize(2.0 * (min16float3)gNormal.Load(int3(p, 0)) - 1.0);

    const float2 uv = (p.xy + (0.5f).xx) / float2(screenSize.xy);
    depth = FFX_DNSR_Reflections_GetLinearDepth(uv, gDepthBuffer.Load(int3(p, 0)));
}

void FFX_DNSR_Reflections_StorePrefilteredReflections(int2 p, min16float3 radiance, min16float variance)
{
    gOutRadiance[p] = radiance.xyzz;
    gOutVariance[p] = variance.x;
}

#include "ffx_denoiser_reflections_prefilter.h"

[numthreads(8, 8, 1)]
void CSMain(int2 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex, uint2 groupId : SV_GroupID)
{
    // Whole groups leave together here, so the header's group barriers stay uniform.
    const uint tileIndex = PteroSssrFlattenGroupId(groupId);
    if (tileIndex >= gIndirectArgs[kPteroSssrArgsTileCount])
        return;

    const int2  dispatchThreadId         = PteroSssrTileDispatchThread(gDenoiserTileList[tileIndex], groupThreadId);
    const int2  dispatchGroupId          = dispatchThreadId / 8;
    const uint2 remappedGroupThreadId    = FFX_DNSR_Reflections_RemapLane8x8(groupIndex);
    const uint2 remappedDispatchThreadId = dispatchGroupId * 8 + remappedGroupThreadId;

    FFX_DNSR_Reflections_Prefilter(remappedDispatchThreadId, remappedGroupThreadId, gBufferDimensions);

    // The prefilter does not touch the sample count; carry it into the buffer the temporal
    // resolve reads, as the sample did by binding its output alongside.
    gOutSampleCount[remappedDispatchThreadId] = gInSampleCount.Load(int3(remappedDispatchThreadId, 0));
}
