// Dple_Denoise.hlsl
// Bilateral spatial denoise for the packed occlusion signals, run as two separable passes
// at working resolution (gStepDirection picks the axis).
//
// The AO estimator takes 8 jittered taps per pixel per frame - noisy by construction, so
// temporal accumulation converges from new samples. But history is rejected wherever depth
// or normals disagree, which in foliage, thin geometry and along every silhouette is most
// of the time, and wherever that fires the raw estimate reaches the screen. This covers
// those pixels, guided by the same depth/normal buffer the temporal pass rejects on, so it
// only borrows from neighbours that are the same surface.
//
// Deliberately run *after* the temporal result is kept as history: feeding a blurred
// signal back into its own history compounds the blur every frame.

#include "Dple_Common.hlsli"

Texture2D<float4>   gOcclusion : register(t4);
Texture2D<float4>   gGuide     : register(t5);
RWTexture2D<float4> gDenoised  : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gWorkSize))
        return;

    const float4 centerGuide = gGuide.Load(int3(id.xy, 0));
    const float centerDepth = centerGuide.w;
    const float4 centerValue = gOcclusion.Load(int3(id.xy, 0));

    // Pass-through pixels carry no occlusion, and their guide entries would poison the
    // weights of anything that sampled them.
    if (centerDepth <= 0.0f)
    {
        gDenoised[id.xy] = centerValue;
        return;
    }

    float4 weightedSum = centerValue;
    float totalWeight = 1.0f;

    const int radius = clamp(gDenoiseRadius, 0, 4);

    [loop]
    for (int offset = -radius; offset <= radius; ++offset)
    {
        if (offset == 0)
            continue;

        const int2 tapPixel = clamp(int2(id.xy) + gStepDirection * offset, int2(0, 0), int2(gWorkSize) - 1);
        const float4 tapGuide = gGuide.Load(int3(tapPixel, 0));
        if (tapGuide.w <= 0.0f)
            continue;

        // Gaussian falloff on distance, then the two bilateral terms. Relative depth, so
        // the filter behaves the same on a wall 2 m away and a hillside 200 m away.
        const float distanceWeight = exp2(-2.0f * float(offset * offset) / max(float(radius * radius), 1.0f));
        const float relativeDepthDelta = abs(tapGuide.w - centerDepth) / max(centerDepth, 0.01f);
        const float depthWeight = (relativeDepthDelta < gDenoiseDepthTolerance) ? exp2(-32.0f * relativeDepthDelta) : 0.0f;
        const float normalWeight = pow(saturate(dot(tapGuide.xyz, centerGuide.xyz)), 16.0f);

        const float weight = distanceWeight * depthWeight * normalWeight;
        weightedSum += gOcclusion.Load(int3(tapPixel, 0)) * weight;
        totalWeight += weight;
    }

    gDenoised[id.xy] = weightedSum / max(totalWeight, DPLE_EPS);
}
