// AutoExposure.hlsli
// Shared layout for the two auto-exposure compute passes.
//
// The design follows Unreal's histogram eye adaptation:
//   1. AutoExposureHistogram.hlsl bins the scene's log-luminance into 64 bins,
//      weighted by a centre-biased metering mask.
//   2. AutoExposureAdapt.hlsl discards the darkest LowPercent and brightest
//      (1 - HighPercent) of that distribution, averages what is left in log
//      space, converts it to an EV100, clamps it to [MinEv100, MaxEv100] and
//      eases the previous frame's value towards it.
//
// The adapted exposure never round-trips through the CPU: it lives in a small
// GPU buffer that AgxTonemap.hlsl reads directly. That keeps the tonemapper's
// constant buffer static frame to frame (it only changes when a slider moves),
// which matters here - a constant buffer rewritten every frame would need the
// three-deep ring every other per-frame upload buffer in this engine has.

#ifndef PTERO_AUTO_EXPOSURE_HLSLI
#define PTERO_AUTO_EXPOSURE_HLSLI

#define AUTO_EXPOSURE_BINS        64
#define AUTO_EXPOSURE_THREADS_X   16
#define AUTO_EXPOSURE_THREADS_Y   16

// Byte offsets into the exposure buffer (a raw buffer of floats).
#define AUTO_EXPOSURE_OFFSET_EV        0   // smoothed EV100 the tonemapper uses
#define AUTO_EXPOSURE_OFFSET_TARGET_EV 4   // this frame's unsmoothed EV100
#define AUTO_EXPOSURE_OFFSET_AVG_LUM   8   // metered average luminance, linear

cbuffer AutoExposureCb : register(b0)
{
    uint  InputWidth;      // dimensions of the metered image
    uint  InputHeight;
    uint  MeterStride;     // sample every Nth pixel on each axis
    uint  ResetHistory;    // 1 = snap to the target instead of easing to it

    float LogMin;          // histogram floor, in log2 luminance
    float LogRange;        // LogMax - LogMin
    float LowPercent;      // 0..1, fraction of the darkest weight to discard
    float HighPercent;     // 0..1, cumulative point to stop averaging at

    float MinEv100;        // clamp on the metered exposure
    float MaxEv100;
    float SpeedUp;         // stops/second when adapting to a brighter scene
    float SpeedDown;       // stops/second when adapting to a darker scene

    float DeltaTimeSeconds;
    float GreyPoint;       // scene luminance that should land on middle grey
    float MeteringMask;    // 0 = average the whole frame, 1 = full centre bias
    float _Pad0;
};

// The mask Unreal calls the metering mask: the centre of the frame is what the
// player is looking at, so a bright window in a corner should not stop the
// exposure down as hard as a bright wall dead ahead.
float AutoExposureMeterWeight(float2 uv)
{
    const float2 centred = (uv - 0.5f) * 2.0f;
    const float  falloff = saturate(1.0f - dot(centred, centred) * 0.5f);
    return lerp(1.0f, falloff, saturate(MeteringMask));
}

// EV100 whose exposure scale lands `luminance` on the grey point. Derived from
// the scale AgxTonemap.hlsl applies, 1 / (1.2 * 2^EV100):
//   1 / (1.2 * 2^ev) = grey / lum   ->   ev = log2(lum) - log2(1.2 * grey)
float AutoExposureLogLuminanceToEv100(float logLuminance, float greyPoint)
{
    return logLuminance - log2(1.2f * max(greyPoint, 1e-4f));
}

#endif // PTERO_AUTO_EXPOSURE_HLSLI
