// RtShadow_Spatial.hlsl  –  cs_6_5
// RT shadow pass 2: edge-aware blur sized to the penumbra.
//
// The penumbra a sun of half-angle a casts is about 2 * tan(a) * blockerDistance wide;
// divided by the pixel's footprint that is the blur radius in pixels, capped at
// g_MaxFilterRadius. Taps are weighted by how close they sit to the centre pixel's plane
// and by how well their normals agree, so shadows don't bleed across edges.

#include "RtShadow_Common.hlsli"

RWTexture2D<float2> u_Raw     : register(u0);
RWTexture2D<float4> u_History : register(u2);
RWTexture2D<float>  u_Output  : register(u3);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float4 normalDepth = t_NormalDepth[pixel];
    if (IsSkyDepth(normalDepth.b))
    {
        u_Output[pixel] = 1.0f;
        return;
    }
    if (g_DebugView == 2)
    {
        u_Output[pixel] = u_Raw[pixel].x;
        return;
    }

    const float4 center = u_History[pixel];
    const float3 centerPos = ReconstructWorldPos(pixel, normalDepth.b);
    const float3 centerN = DecodeOctNormal(normalDepth.rg);

    float radius = 0.0f;
    if (center.y < kRtShadowNoBlocker)
    {
        const float penumbra = 2.0f * g_SunTanHalfAngle * center.y;
        const float footprint = max(center.z * g_PixelAngle, 1e-5f);
        radius = penumbra / footprint;
    }
    // Inside a penumbra even a hard sun leaves one-ray noise worth a pixel of blur.
    if (center.x > 0.001f && center.x < 0.999f)
        radius = max(radius, 1.0f);
    // Freshly disoccluded pixels hold one frame of rays - binary noise wherever the
    // sun is partly covered - so they get a wider filter until the history fills.
    if (center.w < 4.0f && center.y < kRtShadowNoBlocker)
        radius = max(radius, 3.0f);
    radius = min(radius, float(g_MaxFilterRadius));

    if (radius < 0.5f)
    {
        u_Output[pixel] = saturate(center.x);
        return;
    }

    // 5x5 taps spread over the radius.
    const float step = max(radius * 0.5f, 1.0f);
    const float planeTolerance = max(center.z * 0.01f, 0.01f);
    const float invSigma2 = 1.0f / max(radius * radius * 0.5f, 1e-3f);

    float sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            const float2 offset = float2(x, y) * step;
            const int2 p = clamp(int2(pixel) + int2(round(offset)), int2(0, 0), int2(g_FrameWidth - 1, g_FrameHeight - 1));
            const float4 tapND = t_NormalDepth[p];
            if (IsSkyDepth(tapND.b))
                continue;

            const float3 tapPos = ReconstructWorldPos(uint2(p), tapND.b);
            const float planeDistance = abs(dot(centerN, tapPos - centerPos));
            const float wPlane = saturate(1.0f - planeDistance / planeTolerance);
            const float wNormal = pow(saturate(dot(centerN, DecodeOctNormal(tapND.rg))), 8.0f);
            const float wDist = exp(-dot(offset, offset) * invSigma2);
            const float w = wPlane * wNormal * wDist;

            sum += u_History[p].x * w;
            weightSum += w;
        }
    }

    u_Output[pixel] = weightSum > 1e-4f ? saturate(sum / weightSum) : saturate(center.x);
}
