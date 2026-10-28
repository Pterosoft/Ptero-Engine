// RtShadow_LocalTemporal.hlsl  –  cs_6_5
// RT shadow pass 4: accumulates each local light's visibility over frames.
//
// The raw trace (u5) holds this frame's one-or-few-ray visibility per light. Each
// light is looked up by index in last frame's history at the reprojected pixel, so
// lights entering or leaving a pixel's list never get each other's history. History is
// kept only where the previous frame saw the same surface: the sun history (u1) stores
// every pixel's camera distance, which is compared with the one this point had from
// last frame's camera, as the sun's temporal pass does.

#include "RtShadow_Common.hlsli"

RWTexture2D<float4> u_PrevSunHistory : register(u1); // .z = camera distance last frame
RWTexture2D<uint4>  u_LocalRaw       : register(u5);
RWTexture2D<uint4>  u_HistoryA       : register(u6);
RWTexture2D<uint4>  u_HistoryB       : register(u7);
RWTexture2D<uint4>  u_PrevHistoryA   : register(u8);
RWTexture2D<uint4>  u_PrevHistoryB   : register(u9);

static const float kMaxLocalHistoryCount = 32.0f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const uint4 kEmpty = uint4(kRtLocalEmptyHistory, kRtLocalEmptyHistory, kRtLocalEmptyHistory, kRtLocalEmptyHistory);
    const float depth = t_NormalDepth[pixel].b;
    if (IsSkyDepth(depth))
    {
        u_HistoryA[pixel] = kEmpty;
        u_HistoryB[pixel] = kEmpty;
        return;
    }

    // Last frame's entries at the reprojected pixel, if it saw this surface.
    uint4 prevA = kEmpty;
    uint4 prevB = kEmpty;
    if (g_HistoryValid != 0u && g_LocalHistoryValid != 0u)
    {
        const float3 worldPos = ReconstructWorldPos(pixel, depth);
        const float4 prevClip = mul(float4(worldPos, 1.0f), g_PrevViewProj);
        if (prevClip.w > 0.0f)
        {
            float2 prevUv = prevClip.xy / prevClip.w * 0.5f + 0.5f;
            prevUv.y = 1.0f - prevUv.y;
            const int2 prevPixel = int2(floor(prevUv * float2(g_FrameWidth, g_FrameHeight)));
            if (all(prevPixel >= int2(0, 0)) && prevPixel.x < int(g_FrameWidth) && prevPixel.y < int(g_FrameHeight))
            {
                const float expectedDistance = length(worldPos - g_PrevCameraPos);
                const float tolerance = expectedDistance * 0.03f + 0.02f;
                if (abs(u_PrevSunHistory[prevPixel].z - expectedDistance) <= tolerance)
                {
                    prevA = u_PrevHistoryA[prevPixel];
                    prevB = u_PrevHistoryB[prevPixel];
                }
            }
        }
    }

    const uint4 raw = u_LocalRaw[pixel];
    uint entries[PTERO_RT_LOCAL_LIGHTS];
    [unroll]
    for (uint slot = 0u; slot < PTERO_RT_LOCAL_LIGHTS; ++slot)
    {
        const uint pair = RtLocalPair(raw, slot);
        entries[slot] = kRtLocalEmptyHistory;
        if (pair == kRtLocalEmptyPair)
            continue;

        const uint lightIndex = pair >> 8;
        const float current = float(pair & 0xFFu) * (1.0f / 255.0f);
        float visibility = current;
        float count = 1.0f;

        [unroll]
        for (uint p = 0u; p < PTERO_RT_LOCAL_LIGHTS; ++p)
        {
            const uint prev = RtLocalHistoryEntry(prevA, prevB, p);
            if (prev == kRtLocalEmptyHistory || (prev >> 24) != lightIndex)
                continue;
            const float prevVisibility = float((prev >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
            const float prevCount = float(prev & 0xFFu);
            const float alpha = max(g_TemporalBlend, 1.0f / (prevCount + 1.0f));
            visibility = lerp(prevVisibility, current, alpha);
            count = min(prevCount + 1.0f, kMaxLocalHistoryCount);
            break;
        }

        entries[slot] = (lightIndex << 24)
                      | ((uint)round(saturate(visibility) * 65535.0f) << 8)
                      | (uint)count;
    }

    u_HistoryA[pixel] = uint4(entries[0], entries[1], entries[2], entries[3]);
    u_HistoryB[pixel] = uint4(entries[4], entries[5], entries[6], entries[7]);
}
