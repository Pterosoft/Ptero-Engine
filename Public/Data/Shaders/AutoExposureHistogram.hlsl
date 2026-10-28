// AutoExposureHistogram.hlsl - builds the scene's log-luminance histogram.
//
// Root signature (AutoExposure.cpp):
//   b0 - AutoExposureCb
//   t0 - scene colour, HDR, pre-tonemap (SRV)
//   u0 - 64-bin histogram (raw UAV)
//
// Each group accumulates into groupshared bins first and merges once at the
// end, so the whole frame costs 64 global atomics per group rather than one per
// pixel. Kept to SM5 intrinsics on purpose: the startup shader precompiler
// picks a profile by scanning this text, and defaults compute to cs_5_0 - which
// is also what AutoExposure.cpp asks for at runtime. Introducing an SM6-only
// intrinsic here means changing both.

#include "AutoExposure.hlsli"

Texture2D<float4>  gInput     : register(t0);
RWByteAddressBuffer gHistogram : register(u0);

groupshared uint gsBins[AUTO_EXPOSURE_BINS];

[numthreads(AUTO_EXPOSURE_THREADS_X, AUTO_EXPOSURE_THREADS_Y, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex < AUTO_EXPOSURE_BINS)
        gsBins[groupIndex] = 0;

    GroupMemoryBarrierWithGroupSync();

    const uint stride = max(MeterStride, 1u);
    const uint2 pixel = dispatchId.xy * stride;

    if (pixel.x < InputWidth && pixel.y < InputHeight)
    {
        const float3 color = gInput.Load(int3(pixel, 0)).rgb;
        const float  luminance = dot(max(color, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));

        // A NaN from an upstream pass would otherwise poison the bin index and,
        // through it, every subsequent frame's adapted exposure.
        if (!isnan(luminance) && !isinf(luminance))
        {
            const float2 uv = (float2(pixel) + 0.5f) / float2(InputWidth, InputHeight);
            const float  weight = AutoExposureMeterWeight(uv);

            // Anything at or below the histogram floor lands in bin 0, which the
            // reduce pass treats as "black" and the low-percentile cut discards.
            const float logLuminance = log2(max(luminance, 1e-8f));
            const float t = saturate((logLuminance - LogMin) / max(LogRange, 1e-4f));
            const uint  bin = (uint)(t * (AUTO_EXPOSURE_BINS - 1) + 0.5f);

            // Fixed point: the mask is continuous but atomics are integer.
            const uint quantisedWeight = (uint)(saturate(weight) * 255.0f + 0.5f);
            if (quantisedWeight > 0)
                InterlockedAdd(gsBins[bin], quantisedWeight);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (groupIndex < AUTO_EXPOSURE_BINS && gsBins[groupIndex] > 0)
    {
        uint previous;
        gHistogram.InterlockedAdd(groupIndex * 4, gsBins[groupIndex], previous);
    }
}
