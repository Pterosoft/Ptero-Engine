// RtShadow_LocalSpatial.hlsl  –  cs_6_5
// RT shadow pass 5: edge-aware blur of each local light's accumulated visibility, and
// the packing of the result into the pairs DeferredLighting reads (u4).
//
// Each light is blurred only with the same light's visibility in the neighbouring
// pixels (matched by index), weighted by how close they sit to this pixel's plane and
// how well their normals agree, as the sun's spatial pass does. Only penumbra pixels
// are filtered: a light fully seen or fully blocked has nothing to smooth. Freshly
// disoccluded pixels (few accumulated frames) get the widest filter.

#include "RtShadow_Common.hlsli"

RWTexture2D<uint4> u_LocalOutput : register(u4);
RWTexture2D<uint4> u_LocalRaw    : register(u5);
RWTexture2D<uint4> u_HistoryA    : register(u6);
RWTexture2D<uint4> u_HistoryB    : register(u7);

float FindVisibility(uint4 historyA, uint4 historyB, uint lightIndex)
{
    [unroll]
    for (uint s = 0u; s < PTERO_RT_LOCAL_LIGHTS; ++s)
    {
        const uint entry = RtLocalHistoryEntry(historyA, historyB, s);
        if (entry != kRtLocalEmptyHistory && (entry >> 24) == lightIndex)
            return float((entry >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
    }
    return -1.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float4 normalDepth = t_NormalDepth[pixel];
    if (IsSkyDepth(normalDepth.b))
    {
        u_LocalOutput[pixel] = uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
        return;
    }
    // Debug view 4: this frame's traced visibility, before any denoising.
    if (g_DebugView == 4)
    {
        u_LocalOutput[pixel] = u_LocalRaw[pixel];
        return;
    }

    const float3 centerPos = ReconstructWorldPos(pixel, normalDepth.b);
    const float3 centerN = DecodeOctNormal(normalDepth.rg);
    const float planeTolerance = max(length(centerPos - g_CameraPos) * 0.01f, 0.01f);
    const uint4 centerA = u_HistoryA[pixel];
    const uint4 centerB = u_HistoryB[pixel];

    uint pairs[PTERO_RT_LOCAL_LIGHTS];
    [unroll]
    for (uint i = 0u; i < PTERO_RT_LOCAL_LIGHTS; ++i)
        pairs[i] = kRtLocalEmptyPair;

    for (uint slot = 0u; slot < PTERO_RT_LOCAL_LIGHTS; ++slot)
    {
        const uint entry = RtLocalHistoryEntry(centerA, centerB, slot);
        if (entry == kRtLocalEmptyHistory)
            continue;

        const uint lightIndex = entry >> 24;
        float visibility = float((entry >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
        const float count = float(entry & 0xFFu);

        // Young history is filtered whatever its value: a pixel that just lost its
        // history (disoccluded, or moved on screen faster than reprojection keeps) holds
        // one frame of one-ray visibility, which is exactly 0 or 1 - the noisiest input
        // there is, and the one a "penumbra only" test would have let straight through.
        const bool youngHistory = count < 8.0f;
        [branch]
        if (youngHistory || (visibility > 0.002f && visibility < 0.998f))
        {
            // Wider while the history is young, narrower once it has converged.
            const float step = count < 4.0f ? 2.0f : 1.0f;
            const float invSigma2 = 1.0f / (2.0f * 2.0f * step * step);
            float sum = 0.0f;
            float weightSum = 0.0f;
            for (int y = -2; y <= 2; ++y)
            {
                for (int x = -2; x <= 2; ++x)
                {
                    const float2 offset = float2(x, y) * step;
                    const int2 p = clamp(int2(pixel) + int2(offset), int2(0, 0), int2(g_FrameWidth - 1, g_FrameHeight - 1));
                    const float4 tapND = t_NormalDepth[p];
                    if (IsSkyDepth(tapND.b))
                        continue;
                    const float tapVisibility = FindVisibility(u_HistoryA[p], u_HistoryB[p], lightIndex);
                    if (tapVisibility < 0.0f)
                        continue;

                    const float3 tapPos = ReconstructWorldPos(uint2(p), tapND.b);
                    const float wPlane = saturate(1.0f - abs(dot(centerN, tapPos - centerPos)) / planeTolerance);
                    const float wNormal = pow(saturate(dot(centerN, DecodeOctNormal(tapND.rg))), 8.0f);
                    const float wDist = exp(-dot(offset, offset) * invSigma2);
                    const float w = wPlane * wNormal * wDist;
                    sum += tapVisibility * w;
                    weightSum += w;
                }
            }
            if (weightSum > 1e-4f)
                visibility = sum / weightSum;
        }

        pairs[slot] = (lightIndex << 8) | (uint)round(saturate(visibility) * 255.0f);
    }

    u_LocalOutput[pixel] = uint4(
        pairs[0] | (pairs[1] << 16),
        pairs[2] | (pairs[3] << 16),
        pairs[4] | (pairs[5] << 16),
        pairs[6] | (pairs[7] << 16));
}
