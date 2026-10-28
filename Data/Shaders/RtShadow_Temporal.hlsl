// RtShadow_Temporal.hlsl  –  cs_6_5
// RT shadow pass 1: reprojects last frame's visibility and blends this frame's rays in.
//
// History taps are kept only where they saw the same surface (their stored camera
// distance matches the one this point had from last frame's camera), and the history
// is clamped to this frame's 3x3 neighbourhood so moving casters and a moving sun don't
// leave trails. A history sample count gives fresh pixels a fast start.

#include "RtShadow_Common.hlsli"

RWTexture2D<float2> u_Raw         : register(u0);
RWTexture2D<float4> u_PrevHistory : register(u1);
RWTexture2D<float4> u_History     : register(u2);

static const float kMaxHistoryCount = 64.0f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float depth = t_NormalDepth[pixel].b;
    if (IsSkyDepth(depth))
    {
        u_History[pixel] = float4(1.0f, kRtShadowNoBlocker, 0.0f, 0.0f);
        return;
    }

    const float2 raw = u_Raw[pixel];
    const float3 worldPos = ReconstructWorldPos(pixel, depth);
    const float cameraDistance = length(worldPos - g_CameraPos);

    // This frame's neighbourhood range, for clamping the history.
    float vMin = raw.x;
    float vMax = raw.x;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 p = clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(g_FrameWidth - 1, g_FrameHeight - 1));
            if (IsSkyDepth(t_NormalDepth[p].b))
                continue;
            const float v = u_Raw[p].x;
            vMin = min(vMin, v);
            vMax = max(vMax, v);
        }
    }

    float4 history = float4(0.0f, kRtShadowNoBlocker, 0.0f, 0.0f);
    float historyWeight = 0.0f;
    if (g_HistoryValid != 0u)
    {
        const float4 prevClip = mul(float4(worldPos, 1.0f), g_PrevViewProj);
        if (prevClip.w > 0.0f)
        {
            float2 prevUv = prevClip.xy / prevClip.w * 0.5f + 0.5f;
            prevUv.y = 1.0f - prevUv.y;
            const float2 prevPixel = prevUv * float2(g_FrameWidth, g_FrameHeight) - 0.5f;
            const int2 base = int2(floor(prevPixel));
            const float2 f = prevPixel - float2(base);
            const float expectedDistance = length(worldPos - g_PrevCameraPos);
            const float tolerance = expectedDistance * 0.03f + 0.02f;

            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                const int2 offset = int2(i & 1, i >> 1);
                const int2 p = base + offset;
                if (any(p < int2(0, 0)) || p.x >= int(g_FrameWidth) || p.y >= int(g_FrameHeight))
                    continue;
                const float4 tap = u_PrevHistory[p];
                if (tap.w <= 0.0f || abs(tap.z - expectedDistance) > tolerance)
                    continue;
                const float w = (offset.x ? f.x : 1.0f - f.x) * (offset.y ? f.y : 1.0f - f.y);
                history.x += tap.x * w;
                history.y = min(history.y, tap.y);
                history.w += tap.w * w;
                historyWeight += w;
            }
        }
    }

    float visibility = raw.x;
    float count = 1.0f;
    float blocker = raw.y;
    if (historyWeight > 0.05f)
    {
        const float prevVisibility = clamp(history.x / historyWeight, vMin, vMax);
        const float prevCount = history.w / historyWeight;
        const float alpha = max(g_TemporalBlend, 1.0f / (prevCount + 1.0f));
        visibility = lerp(prevVisibility, raw.x, alpha);
        count = min(prevCount + 1.0f, kMaxHistoryCount);
        // A ray that hit gives a fresh blocker distance; otherwise keep the one we had,
        // drifting toward "none" so a caster that moved away stops widening the filter.
        blocker = raw.y < kRtShadowNoBlocker ? lerp(history.y, raw.y, history.y < kRtShadowNoBlocker ? alpha : 1.0f)
                                             : (vMin >= 1.0f ? kRtShadowNoBlocker : history.y);
    }

    u_History[pixel] = float4(visibility, blocker, cameraDistance, count);
}
