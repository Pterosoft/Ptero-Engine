// Sssr_ResolveTemporal.hlsl
// FidelityFX reflection denoiser, pass 3 of 3: blends the prefiltered radiance with the
// reprojected history, clipping the history to the local neighbourhood so it cannot
// ghost. Its output is both this frame's reflection and next frame's history. Runs
// indirectly, one group per listed tile. Ported from the SSSR 1.3 sample's
// ResolveTemporal.hlsl around ffx_denoiser_reflections_resolve_temporal.h.
//
// Part of the SSSR pass set, which is compiled for SM6 because its other passes use Wave
// intrinsics.

#include "Sssr_Common.hlsli"

Texture2D<float>  gRoughness              : register(t0);
Texture2D<float3> gAverageRadiance        : register(t1);
Texture2D<float4> gInRadiance             : register(t2);
Texture2D<float4> gInReprojectedRadiance  : register(t3);
Texture2D<float>  gInVariance             : register(t4);
Texture2D<float>  gInSampleCount          : register(t5);
StructuredBuffer<uint> gDenoiserTileList  : register(t6);
StructuredBuffer<uint> gIndirectArgs : register(t7);   // layout in Sssr_Common.hlsli

SamplerState gLinearSampler : register(s0);

RWTexture2D<float4> gOutRadiance : register(u0);
RWTexture2D<float>  gOutVariance : register(u1);

min16float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv) { return (min16float3)gAverageRadiance.SampleLevel(gLinearSampler, uv, 0.0f).xyz; }
min16float3 FFX_DNSR_Reflections_LoadRadiance(int2 p) { return (min16float3)gInRadiance.Load(int3(p, 0)).xyz; }
min16float3 FFX_DNSR_Reflections_LoadRadianceReprojected(int2 p) { return (min16float3)gInReprojectedRadiance.Load(int3(p, 0)).xyz; }
min16float FFX_DNSR_Reflections_LoadRoughness(int2 p) { return (min16float)gRoughness.Load(int3(p, 0)); }
min16float FFX_DNSR_Reflections_LoadVariance(int2 p) { return (min16float)gInVariance.Load(int3(p, 0)).x; }
min16float FFX_DNSR_Reflections_LoadNumSamples(int2 p) { return (min16float)gInSampleCount.Load(int3(p, 0)).x; }

void FFX_DNSR_Reflections_StoreTemporalAccumulation(int2 p, min16float3 radiance, min16float variance)
{
    gOutRadiance[p] = radiance.xyzz;
    gOutVariance[p] = variance.x;
}

#include "ffx_denoiser_reflections_resolve_temporal.h"

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

    FFX_DNSR_Reflections_ResolveTemporal(remappedDispatchThreadId, remappedGroupThreadId, gBufferDimensions, gInvBufferDimensions, gTemporalStabilityFactor);
}
