// Dple_Temporal.hlsl
// Temporal reconstruction, scoped to the signals DPLE itself introduces.
//
// TAA or the upscaler has already resolved the image by the time this runs. What they
// cannot stabilise is a signal that did not exist when they ran: the AO and contact shadow
// terms are generated afterwards, from 8 jittered taps per pixel per frame, and own their
// temporal stability entirely.
//
// Also the pack pass: micro/contact/broad AO and contact shadow go in, one RGBA texture
// comes out, which is both the composite's input and next frame's history.

#include "Dple_Common.hlsli"

Texture2D<float4>   gOcclusion     : register(t4);
Texture2D<float>    gContactShadow : register(t5);
Texture2D<float4>   gGuide         : register(t6);
Texture2D<float4>   gHistory       : register(t7);
Texture2D<float4>   gHistoryGuide  : register(t8);
RWTexture2D<float4> gResolvedOut   : register(u0);

float4 LoadCurrent(int2 pixel)
{
    const int3 clamped = int3(clamp(pixel, int2(0, 0), int2(gWorkSize) - 1), 0);
    return float4(gOcclusion.Load(clamped).xyz, gContactShadow.Load(clamped));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gWorkSize))
        return;

    const float4 current = LoadCurrent(int2(id.xy));
    const float4 guide = gGuide[id.xy];
    const float centerDepth = guide.w;

    if (!DpleHasFlag(DPLE_FLAG_TEMPORAL) || gHistoryValid == 0u || centerDepth <= 0.0f)
    {
        gResolvedOut[id.xy] = current;
        return;
    }

    // Camera-only reprojection. Ptero has no per-object velocity outside the upscalers, and
    // what moves on its own is caught by disocclusion and the neighbourhood clamp below.
    // Both projections are jitter-free: the difference between them is pure camera motion,
    // so a still camera reprojects onto exactly the same texel every frame.
    const float2 viewUV = DpleWorkPixelToViewUV(id.xy);
    const float3 worldPos = DpleDepthWorldPosition(viewUV, DpleSampleDeviceZ(viewUV));
    const float3 currentProjection = DpleProjectToUV(worldPos, gViewProjNoJitter);
    const float3 previousProjection = DpleProjectToUV(worldPos, gPrevViewProjNoJitter);
    if (currentProjection.z <= 0.0f || previousProjection.z <= 0.0f)
    {
        gResolvedOut[id.xy] = current;
        return;
    }

    const float2 motionUV = previousProjection.xy - currentProjection.xy;
    const float2 previousViewUV = viewUV + motionUV;
    if (any(previousViewUV < 0.0f) || any(previousViewUV > 1.0f))
    {
        gResolvedOut[id.xy] = current;
        return;
    }

    // Below a quarter texel of motion, fetch history with Load at this texel rather than
    // resampling it. Bilinear resampling at a sub-texel offset is a low-pass filter, and
    // applied once a frame at ~90% history weight it compounds into a blur that eats
    // exactly the detail a moving camera shows. This makes accumulation at rest lossless.
    const float motionTexels = length(motionUV * float2(gWorkSize));
    const bool atRest = motionTexels < 0.25f;

    // Reject before blending: history from different geometry is not "less trustworthy",
    // it is wrong, and any weight on it is a ghost that takes frames to decay.
    const float4 historyGuide = atRest
        ? gHistoryGuide.Load(int3(id.xy, 0))
        : gHistoryGuide.SampleLevel(gLinearClamp, previousViewUV, 0.0f);

    const float relativeDepthDelta = abs(historyGuide.w - centerDepth) / max(centerDepth, 0.01f);
    const float normalAgreement = dot(historyGuide.xyz, guide.xyz);

    if (historyGuide.w <= 0.0f || relativeDepthDelta > gDisocclusionDepthTolerance || normalAgreement < 0.85f)
    {
        gResolvedOut[id.xy] = current;
        return;
    }

    float4 history = atRest
        ? gHistory.Load(int3(id.xy, 0))
        : gHistory.SampleLevel(gLinearClamp, previousViewUV, 0.0f);

    // Neighbourhood clamp on the current 3x3. Depth/normal validation catches
    // disocclusion but not a change in occlusion that leaves geometry alone - a door
    // swinging shut, a shadow moving across a wall.
    float4 mean = 0.0f;
    float4 meanSquared = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float4 neighbor = LoadCurrent(int2(id.xy) + int2(x, y));
            mean += neighbor;
            meanSquared += neighbor * neighbor;
        }
    }
    mean /= 9.0f;
    meanSquared /= 9.0f;

    const float4 deviation = sqrt(max(meanSquared - mean * mean, 0.0f));
    history = clamp(history, mean - gNeighborhoodClampScale * deviation, mean + gNeighborhoodClampScale * deviation);

    // Under motion the reprojection is less exact and the clamp box wider, so lean on the
    // current frame. Measured in output pixels, full effect at 8.
    const float motionPixels = length(motionUV * gOutputSize);
    const float weight = lerp(gHistoryWeightStable, gHistoryWeightMoving, saturate(motionPixels / 8.0f));

    gResolvedOut[id.xy] = saturate(lerp(current, history, weight));
}
