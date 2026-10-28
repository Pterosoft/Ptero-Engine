// Sssr_Reproject.hlsl
// FidelityFX reflection denoiser, pass 1 of 3: finds each pixel's reflection in last
// frame's result, either where the surface was or where the reflected hit was (parallax),
// estimates temporal variance and sample count, and writes an 8x8-averaged radiance for
// the later passes. Runs indirectly, one group per tile ClassifyTiles listed. Ported from
// the SSSR 1.3 sample's Reproject.hlsl around ffx_denoiser_reflections_reproject.h.
//
// The sample read a motion-vector target. This engine only produces motion vectors when an
// upscaler asks for them, and later in the frame, so the surface motion is reconstructed
// here from depth and the previous frame's view-projection instead. That captures camera
// motion; moving objects reproject as if static, which the denoiser's disocclusion and
// neighbourhood clipping then have to catch.
//
// The denoiser headers use Wave-era min16float math; the SSSR pass set compiles for SM6.

#include "Sssr_Common.hlsli"

Texture2D<float>  gDepthBuffer          : register(t0);
Texture2D<float>  gRoughness            : register(t1);
Texture2D<float3> gNormal               : register(t2);
Texture2D<float>  gDepthBufferHistory   : register(t3);
Texture2D<float>  gRoughnessHistory     : register(t4);
Texture2D<float3> gNormalHistory        : register(t5);
Texture2D<float4> gInRadiance           : register(t6);
Texture2D<float4> gRadianceHistory      : register(t7);
Texture2D<float>  gVarianceHistory      : register(t8);
Texture2D<float>  gSampleCountHistory   : register(t9);
StructuredBuffer<uint> gDenoiserTileList : register(t10);
StructuredBuffer<uint> gIndirectArgs : register(t11);   // layout in Sssr_Common.hlsli

SamplerState gLinearSampler : register(s0);

RWTexture2D<float3> gOutReprojectedRadiance : register(u0);
RWTexture2D<float3> gOutAverageRadiance     : register(u1);
RWTexture2D<float>  gOutVariance            : register(u2);
RWTexture2D<float>  gOutSampleCount         : register(u3);

float FFX_DNSR_Reflections_LoadDepth(int2 p) { return gDepthBuffer.Load(int3(p, 0)); }
float FFX_DNSR_Reflections_LoadDepthHistory(int2 p) { return gDepthBufferHistory.Load(int3(p, 0)); }
float FFX_DNSR_Reflections_SampleDepthHistory(float2 uv) { return gDepthBufferHistory.SampleLevel(gLinearSampler, uv, 0.0f); }
min16float3 FFX_DNSR_Reflections_LoadRadiance(int2 p) { return (min16float3)gInRadiance.Load(int3(p, 0)).xyz; }
min16float3 FFX_DNSR_Reflections_LoadRadianceHistory(int2 p) { return (min16float3)gRadianceHistory.Load(int3(p, 0)).xyz; }
min16float3 FFX_DNSR_Reflections_SampleRadianceHistory(float2 uv) { return (min16float3)gRadianceHistory.SampleLevel(gLinearSampler, uv, 0.0f).xyz; }
min16float3 FFX_DNSR_Reflections_LoadWorldSpaceNormal(int2 p) { return normalize(2.0 * (min16float3)gNormal.Load(int3(p, 0)) - 1.0); }
min16float3 FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(int2 p) { return normalize(2.0 * (min16float3)gNormalHistory.Load(int3(p, 0)) - 1.0); }
min16float3 FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(float2 uv) { return normalize(2.0 * (min16float3)gNormalHistory.SampleLevel(gLinearSampler, uv, 0.0f) - 1.0); }
min16float FFX_DNSR_Reflections_LoadRoughness(int2 p) { return (min16float)gRoughness.Load(int3(p, 0)); }
min16float FFX_DNSR_Reflections_SampleRoughnessHistory(float2 uv) { return (min16float)gRoughnessHistory.SampleLevel(gLinearSampler, uv, 0.0f); }
min16float FFX_DNSR_Reflections_LoadRoughnessHistory(int2 p) { return (min16float)gRoughnessHistory.Load(int3(p, 0)); }
min16float FFX_DNSR_Reflections_LoadRayLength(int2 p) { return (min16float)gInRadiance.Load(int3(p, 0)).w; }

// With no valid history (first frame, resize, re-enable) the sample count reads as zero,
// which makes the temporal resolve give the reprojected value no weight at all.
min16float FFX_DNSR_Reflections_SampleNumSamplesHistory(float2 uv)
{
    return gHistoryValid != 0 ? (min16float)gSampleCountHistory.SampleLevel(gLinearSampler, uv, 0.0f) : (min16float)0.0;
}
min16float FFX_DNSR_Reflections_SampleVarianceHistory(float2 uv)
{
    return gHistoryValid != 0 ? (min16float)gVarianceHistory.SampleLevel(gLinearSampler, uv, 0.0f) : (min16float)1.0;
}

// uv(this frame) - uv(last frame) of the surface under this pixel, from depth alone.
float2 FFX_DNSR_Reflections_LoadMotionVector(int2 p)
{
    const float2 uv = (float2(p) + 0.5f) * gInvBufferDimensions;
    const float3 world = PteroSssrScreenToWorld(float3(uv, gDepthBuffer.Load(int3(p, 0))));
    return uv - PteroSssrProjectPosition(world, gPrevViewProj).xy;
}

void FFX_DNSR_Reflections_StoreRadianceReprojected(int2 p, min16float3 value) { gOutReprojectedRadiance[p] = value; }
void FFX_DNSR_Reflections_StoreAverageRadiance(int2 p, min16float3 value) { gOutAverageRadiance[p] = value; }
void FFX_DNSR_Reflections_StoreVariance(int2 p, min16float value) { gOutVariance[p] = value; }
void FFX_DNSR_Reflections_StoreNumSamples(int2 p, min16float value) { gOutSampleCount[p] = value; }

#include "ffx_denoiser_reflections_reproject.h"

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

    FFX_DNSR_Reflections_Reproject(remappedDispatchThreadId, remappedGroupThreadId, gBufferDimensions, gTemporalStabilityFactor, 32);
}
