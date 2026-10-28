// VolumetricCloudRender.hlsl
// Primary volumetric cloud raymarch.  Runs at gTraceResolution (usually half
// the scene resolution) and writes:
//
//   gTraceColor : rgb = in-scattered luminance, a = transmittance along the ray
//   gTraceDepth : transmittance-weighted mean distance to the cloud, metres
//                 (0 when the ray never hit any cloud mass)
//
// Energy is integrated analytically per segment, so step size affects how much
// detail is resolved but not how bright the result is:
//
//   L += T_view * (S - S * exp(-sigma_t * dt)) / sigma_t
//   T_view *= exp(-sigma_t * dt)
//
// Multiple scattering uses Wrenninge's octave approximation: each octave scales
// scattering, extinction-towards-light and phase anisotropy by a constant
// factor, which recovers most of the energy a single-scattering march loses.

#include "VolumetricCloudCommon.hlsli"

Texture2D<float>    gSceneDepth : register(t4);

RWTexture2D<float4> gTraceColor : register(u0);
RWTexture2D<float>  gTraceDepth : register(u1);

// Interleaved gradient noise — a cheap, well distributed dither that decorrelates
// nicely once the frame index is folded in.
float InterleavedGradientNoise(float2 pixel, uint frameIndex)
{
    pixel += 5.588238f * (float)(frameIndex & 63u);
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gTraceResolution.x || pixel.y >= gTraceResolution.y)
        return;

    const float2 uv = (float2(pixel) + 0.5f) * gInvTraceResolution;

    const float3 rayDir = normalize(ReconstructWorldPosition(uv, 1.0f) - gCameraPos);

    // Opaque geometry clips the march.  The trace runs at reduced resolution, so
    // take the depth from the matching full-resolution texel.
    uint2 fullPixel = min(
        pixel * gResolutionDivisor + (gResolutionDivisor >> 1u),
        gFullResolution - 1u);

    float sceneDistance = gMaxTraceDistance;
    float sceneDepth = gSceneDepth.Load(int3(fullPixel, 0));
    if (sceneDepth < 1.0f)
    {
        float2 fullUv = (float2(fullPixel) + 0.5f) * gInvFullResolution;
        sceneDistance = length(ReconstructWorldPosition(fullUv, sceneDepth) - gCameraPos);
    }

    float tStart;
    float tEnd;
    if (!IntersectCloudLayer(gCameraPos, rayDir, tStart, tEnd))
    {
        gTraceColor[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gTraceDepth[pixel] = 0.0f;
        return;
    }

    tStart = max(tStart, 0.0f);
    tEnd = min(tEnd, min(sceneDistance, gMaxTraceDistance));
    if (tEnd <= tStart)
    {
        gTraceColor[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gTraceDepth[pixel] = 0.0f;
        return;
    }

    // Step count scales with how much of the shell the ray crosses: a vertical
    // ray gets gMaxSteps, a grazing ray gets proportionally more (capped).
    const float segmentLength = tEnd - tStart;
    const float referenceStep = gLayerThickness / max((float)gMaxSteps, 1.0f);
    const float stepCount = clamp(segmentLength / max(referenceStep, 1.0f), 32.0f, (float)gMaxSteps * 2.0f);
    const float fineStep = segmentLength / stepCount;
    const float coarseStep = fineStep * 3.0f;

    // Budget bound: an all-fine march costs stepCount, re-marching the step-back
    // after each cloud entry adds at most stepCount/3, and coarse searching adds
    // at most stepCount/3 more.  Doubling clears that with room to spare, and the
    // loop exits on t >= tEnd long before this in the common case.  Running out
    // here truncates the ray and leaves a hard edge in the sky, so it must not
    // happen in practice.
    const uint maxIterations = (uint)(stepCount * 2.0f) + 16u;

    const float3 toLight = -normalize(gSunDirection);
    const float basePhase = DualLobePhase(dot(rayDir, toLight));

    // Sun colour is an illuminance-like quantity in this engine's HDR scale.
    // Folding 4*pi in cancels the phase normalisation so an isotropic, optically
    // thick cloud converges on its albedo instead of albedo/(4*pi).
    const float3 sunIlluminance = gSunColor * gSunIntensityScale * (4.0f * kPi);
    const float3 ambientBase = gSkyColor * gAmbientIntensityScale;

    const float dither = InterleavedGradientNoise(float2(pixel), gFrameIndex);

    float3 scatteredLuminance = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;
    float depthWeightedSum = 0.0f;
    float depthWeight = 0.0f;

    float t = tStart + fineStep * dither;
    bool detailedMarch = false;
    uint emptySamples = 0u;

    [loop]
    for (uint iteration = 0u; iteration < maxIterations; ++iteration)
    {
        if (t >= tEnd || transmittance < 0.005f)
            break;

        float3 samplePos = gCameraPos + rayDir * t;
        float heightFraction = GetHeightFraction(samplePos);
        float4 weather = SampleWeather(samplePos.xy);

        // Detail erosion fades out with distance; it is sub-pixel there anyway.
        float detailAmount = 1.0f - saturate(t / max(gDetailFadeDistance, 1.0f));

        // The coarse search must evaluate exactly the density the fine march
        // will: if it detected cloud that erosion later removes, the fine march
        // would find only empties, revert to coarse, and re-detect the same
        // phantom cloud forever, burning the iteration budget in the wispy
        // regions where erosion bites hardest.  Sharing the function guarantees
        // the fine march reaches the detected sample within the step-back.
        float density = SampleCloudDensity(samplePos, heightFraction, weather, detailAmount);

        if (!detailedMarch)
        {
            // Coarse search for the cloud boundary.
            if (density > 0.0f)
            {
                detailedMarch = true;
                emptySamples = 0u;
                t = max(t - coarseStep, tStart);
            }
            else
            {
                t += coarseStep;
            }
            continue;
        }

        if (density <= 0.0f)
        {
            // Back to coarse stepping as soon as we are clearly outside the
            // cloud again, so leaving one bank does not starve the next.
            ++emptySamples;
            if (emptySamples > 4u)
            {
                detailedMarch = false;
                emptySamples = 0u;
            }
            t += fineStep;
            continue;
        }

        emptySamples = 0u;

        float extinction = max(density * gExtinctionScale, 1e-7f);
        float3 scattering = extinction * gScatteringAlbedo;

        float lightOpticalDepth = SampleLightOpticalDepth(samplePos, toLight, detailAmount) * gExtinctionScale;

        // Powder: the darkening seen on cloud faces turned towards the light.
        float powder = 1.0f - exp(-density * 8.0f);
        float powderTerm = lerp(1.0f, saturate(powder * 2.0f), gPowderStrength);

        // Sky light reaches the top of the layer freely and the base only
        // through the mass above it; the ground bounce fills in from below.
        float3 ambient = ambientBase * lerp(gGroundBounceScale, 1.0f, heightFraction);

        float3 sunScattering = float3(0.0f, 0.0f, 0.0f);
        float octaveScatter = 1.0f;
        float octaveExtinction = 1.0f;
        float octavePhase = 1.0f;

        const uint octaves = clamp(gMsOctaves, 1u, 4u);
        [loop]
        for (uint octave = 0u; octave < octaves; ++octave)
        {
            float phase = lerp(kIsotropicPhase, basePhase, octavePhase);
            float transmittanceToLight = exp(-lightOpticalDepth * octaveExtinction);
            sunScattering += sunIlluminance * transmittanceToLight * phase * octaveScatter;

            octaveScatter *= gMsScatterFalloff;
            octaveExtinction *= gMsExtinctionFalloff;
            octavePhase *= gMsPhaseFalloff;
        }

        float3 stepScattering = (sunScattering * powderTerm + ambient) * scattering;

        float stepTransmittance = exp(-extinction * fineStep);
        float3 integratedLuminance = (stepScattering - stepScattering * stepTransmittance) / extinction;

        scatteredLuminance += transmittance * integratedLuminance;

        float absorbed = transmittance * (1.0f - stepTransmittance);
        depthWeightedSum += t * absorbed;
        depthWeight += absorbed;

        transmittance *= stepTransmittance;
        t += fineStep;
    }

    // Fade the whole layer out towards the trace limit so the shell edge never
    // shows up as a hard line on the horizon.
    float distanceFade = 1.0f - RemapClamped(tStart, gDistanceFadeStart, gMaxTraceDistance, 0.0f, 1.0f);
    scatteredLuminance *= distanceFade;
    transmittance = lerp(1.0f, transmittance, distanceFade);

    float cloudDistance = (depthWeight > 1e-5f) ? (depthWeightedSum / depthWeight) : 0.0f;

    gTraceColor[pixel] = float4(scatteredLuminance, saturate(transmittance));
    gTraceDepth[pixel] = cloudDistance;
}
