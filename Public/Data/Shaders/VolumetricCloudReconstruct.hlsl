// VolumetricCloudReconstruct.hlsl
// Upsamples the reduced-resolution cloud trace to full resolution and blends it
// with the reprojected result of the previous frame.
//
// Reprojection uses the traced cloud distance rather than the scene depth, so
// the history follows the cloud surface instead of whatever geometry happens to
// be behind it.  A neighbourhood clamp against the current trace keeps fast
// camera motion from smearing stale samples across the sky.

#include "VolumetricCloudConstants.hlsli"

Texture2D<float4>   gTraceColor   : register(t4);
Texture2D<float>    gTraceDepth   : register(t5);
Texture2D<float4>   gHistoryColor : register(t6);
Texture2D<float>    gHistoryDepth : register(t7);

RWTexture2D<float4> gOutputColor  : register(u0);
RWTexture2D<float>  gOutputDepth  : register(u1);

SamplerState gLinearClampSampler : register(s1);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gFullResolution.x || pixel.y >= gFullResolution.y)
        return;

    const float2 uv = (float2(pixel) + 0.5f) * gInvFullResolution;

    float4 current = gTraceColor.SampleLevel(gLinearClampSampler, uv, 0.0f);
    float currentDistance = gTraceDepth.SampleLevel(gLinearClampSampler, uv, 0.0f);

    float4 resolved = current;

    if (gTemporalEnabled != 0u && gHistoryValid > 0.5f)
    {
        // Reproject through the point on the cloud this pixel actually sampled.
        float3 rayDir = normalize(ReconstructWorldPosition(uv, 1.0f) - gCameraPos);
        float distance = (currentDistance > 1.0f) ? currentDistance : gDistanceFadeStart;
        float3 cloudWorldPos = gCameraPos + rayDir * distance;

        float4 previousClip = mul(float4(cloudWorldPos, 1.0f), gPrevViewProj);
        if (previousClip.w > 1e-4f)
        {
            float2 previousNdc = previousClip.xy / previousClip.w;
            float2 previousUv = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f);

            if (all(previousUv > 0.0f) && all(previousUv < 1.0f))
            {
                float4 history = gHistoryColor.SampleLevel(gLinearClampSampler, previousUv, 0.0f);

                // Clamp the history into the local colour range of the current
                // trace so disocclusions resolve in a frame or two.
                float4 minimum = current;
                float4 maximum = current;
                [unroll]
                for (int y = -1; y <= 1; ++y)
                {
                    [unroll]
                    for (int x = -1; x <= 1; ++x)
                    {
                        float2 neighbourUv = uv + float2(x, y) * gInvTraceResolution;
                        float4 neighbour = gTraceColor.SampleLevel(gLinearClampSampler, neighbourUv, 0.0f);
                        minimum = min(minimum, neighbour);
                        maximum = max(maximum, neighbour);
                    }
                }

                history = clamp(history, minimum, maximum);

                // Reject the history outright when the cloud we hit is at a very
                // different distance than the one it was blended from.
                float historyDistance = gHistoryDepth.SampleLevel(gLinearClampSampler, previousUv, 0.0f);
                float distanceDelta = abs(historyDistance - currentDistance);
                float relativeDelta = distanceDelta / max(currentDistance, 1.0f);
                float confidence = 1.0f - saturate((relativeDelta - 0.15f) / 0.35f);

                float blend = saturate(gTemporalBlend) * confidence;
                resolved = lerp(current, history, blend);
            }
        }
    }

    gOutputColor[pixel] = resolved;
    gOutputDepth[pixel] = currentDistance;
}
