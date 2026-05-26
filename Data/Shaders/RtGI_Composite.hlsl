// RtGI_Composite.hlsl
// RTGI Pass 3 – Temporal accumulation and final composite.
//
// Blends the current frame's ReSTIR GI output with the accumulated history
// (exponential moving average) to produce a stable, low-noise GI result.
// Supports a debug-view mode for inspecting intermediate data.

#include "RtGI_Common.hlsli"

// ─── Inputs ───────────────────────────────────────────────────────────────────
Texture2D<float4> t_GIRadiance   : register(t0);  // current ReSTIR GI output
Texture2D<float4> t_AccumHistory : register(t1);  // previous accumulated frame

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWTexture2D<float4> u_AccumOutput : register(u0);  // updated accumulation buffer
RWTexture2D<float4> u_FinalOutput : register(u1);  // final lit output for display

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    float4 giSample  = t_GIRadiance.Load(int3(pixel, 0));
    float4 histSample = t_AccumHistory.Load(int3(pixel, 0));

    // Exponential moving average:  accum = lerp(history, current, blend)
    // Lower g_AccumulationBlend = more history = smoother but slower to react.
    float blend = saturate(g_AccumulationBlend);

    // On the very first frame there is no history, so blend = 1.0.
    if (g_FrameIndex == 0)
        blend = 1.0f;

    float4 accumulated = lerp(histSample, giSample, blend);

    // Write updated accumulation for next frame.
    u_AccumOutput[pixel] = accumulated;

    // ── Debug views ───────────────────────────────────────────────────────
    float4 finalColor;
    if (g_DebugView == 1)
    {
        // Raw GI radiance (no accumulation).
        finalColor = float4(giSample.rgb, 1.0f);
    }
    else if (g_DebugView == 2)
    {
        // Luminance of the accumulated GI (grey-scale weight visualisation).
        float lum = Luminance(accumulated.rgb);
        finalColor = float4(lum, lum, lum, 1.0f);
    }
    else
    {
        // Normal path: output accumulated GI radiance.
        finalColor = float4(accumulated.rgb, 1.0f);
    }

    u_FinalOutput[pixel] = finalColor;
}
