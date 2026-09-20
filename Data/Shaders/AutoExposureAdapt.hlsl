// AutoExposureAdapt.hlsl - turns the histogram into one smoothed EV100.
//
// Root signature (AutoExposure.cpp):
//   b0 - AutoExposureCb
//   u0 - 64-bin histogram (raw UAV; cleared here for the next frame)
//   u1 - exposure buffer (raw UAV): [0] smoothed EV, [1] target EV, [2] avg luminance
//
// Dispatched as a single group. The 64 bins are loaded in parallel and scanned
// by one thread: a 64-iteration serial scan is free at this size, and the
// percentile walk is far easier to read - and to prove right - than a parallel
// prefix sum would be.

#include "AutoExposure.hlsli"

RWByteAddressBuffer gHistogram : register(u0);
RWByteAddressBuffer gExposure  : register(u1);

groupshared uint gsBins[AUTO_EXPOSURE_BINS];

[numthreads(AUTO_EXPOSURE_BINS, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    gsBins[groupIndex] = gHistogram.Load(groupIndex * 4);

    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0)
    {
        uint total = 0;
        for (uint i = 0; i < AUTO_EXPOSURE_BINS; ++i)
            total += gsBins[i];

        float smoothedEv = asfloat(gExposure.Load(AUTO_EXPOSURE_OFFSET_EV));
        float targetEv = smoothedEv;
        float averageLuminance = 0.0f;

        if (total > 0)
        {
            // Walk the distribution once, keeping only the slice of weight
            // between the two percentiles. Clipping the dark tail stops a
            // shadowed corner from washing the frame out, and clipping the
            // bright tail stops a lamp or a specular hit from crushing it.
            const float lowThreshold  = total * saturate(LowPercent);
            const float highThreshold = total * saturate(max(HighPercent, LowPercent));

            float weightedLogSum = 0.0f;
            float weightSum = 0.0f;
            float scanned = 0.0f;

            for (uint bin = 0; bin < AUTO_EXPOSURE_BINS; ++bin)
            {
                const float count = (float)gsBins[bin];
                const float binStart = scanned;
                const float binEnd = scanned + count;
                scanned = binEnd;

                // How much of this bin's weight falls inside the kept band.
                const float included = max(0.0f, min(binEnd, highThreshold) - max(binStart, lowThreshold));
                if (included <= 0.0f)
                    continue;

                const float t = (float)bin / (float)(AUTO_EXPOSURE_BINS - 1);
                const float binLogLuminance = LogMin + t * LogRange;

                weightedLogSum += binLogLuminance * included;
                weightSum += included;
            }

            // Averaging in log space gives the geometric mean, which is what
            // makes a scene with one very bright object meter like the eye
            // expects instead of being dragged around by that object.
            const float averageLogLuminance = (weightSum > 0.0f)
                ? (weightedLogSum / weightSum)
                : LogMin;

            averageLuminance = exp2(averageLogLuminance);
            targetEv = AutoExposureLogLuminanceToEv100(averageLogLuminance, GreyPoint);
        }

        targetEv = clamp(targetEv, min(MinEv100, MaxEv100), max(MinEv100, MaxEv100));

        if (ResetHistory != 0 || isnan(smoothedEv) || isinf(smoothedEv))
        {
            smoothedEv = targetEv;
        }
        else
        {
            // Frame-rate independent ease. Adapting to a brighter scene raises
            // the EV, so that direction is the one SpeedUp governs - the same
            // sense Unreal's SpeedUp / SpeedDown have.
            const float speed = (targetEv > smoothedEv) ? SpeedUp : SpeedDown;
            const float blend = 1.0f - exp(-max(DeltaTimeSeconds, 0.0f) * max(speed, 0.0f));
            smoothedEv = lerp(smoothedEv, targetEv, saturate(blend));
        }

        gExposure.Store(AUTO_EXPOSURE_OFFSET_EV, asuint(smoothedEv));
        gExposure.Store(AUTO_EXPOSURE_OFFSET_TARGET_EV, asuint(targetEv));
        gExposure.Store(AUTO_EXPOSURE_OFFSET_AVG_LUM, asuint(averageLuminance));
    }

    GroupMemoryBarrierWithGroupSync();

    // Clear for the next frame's accumulation, so the histogram pass never has
    // to be preceded by a separate clear dispatch or a ClearUnorderedAccessView.
    gHistogram.Store(groupIndex * 4, 0);
}
