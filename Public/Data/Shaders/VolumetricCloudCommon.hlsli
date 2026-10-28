// VolumetricCloudCommon.hlsli
// The cloud shape model and the participating-media scattering helpers used by
// the raymarch pass.  Passes that only need the constant buffer include
// VolumetricCloudConstants.hlsli directly instead.

#ifndef VOLUMETRIC_CLOUD_COMMON_INCLUDED
#define VOLUMETRIC_CLOUD_COMMON_INCLUDED

#include "VolumetricCloudConstants.hlsli"

Texture3D<float4> gBaseShapeNoise : register(t0);
Texture3D<float4> gDetailNoise    : register(t1);
Texture2D<float4> gCurlNoise      : register(t2);
Texture2D<float4> gWeatherMap     : register(t3);

SamplerState gLinearRepeatSampler : register(s0);
SamplerState gLinearClampSampler  : register(s1);

// ---------------------------------------------------------------------------
// Layer geometry
// ---------------------------------------------------------------------------

float3 GetPlanetCentre()
{
    return float3(0.0f, 0.0f, -gPlanetRadius);
}

float GetRadiusFromCentre(float3 worldPos)
{
    return length(worldPos - GetPlanetCentre());
}

// 0 at the base of the layer, 1 at the top.
float GetHeightFraction(float3 worldPos)
{
    return saturate((GetRadiusFromCentre(worldPos) - gLayerBottomRadius) / max(gLayerThickness, 1.0f));
}

// Returns both roots, sorted.  x may be negative when the origin is inside.
bool RaySphereIntersect(float3 rayOrigin, float3 rayDir, float3 centre, float radius, out float2 hits)
{
    float3 toOrigin = rayOrigin - centre;
    float b = dot(toOrigin, rayDir);
    float c = dot(toOrigin, toOrigin) - radius * radius;
    float discriminant = b * b - c;

    hits = float2(0.0f, 0.0f);
    if (discriminant < 0.0f)
        return false;

    float rootDelta = sqrt(discriminant);
    hits = float2(-b - rootDelta, -b + rootDelta);
    return true;
}

// Finds the visible span of the cloud shell along a view ray.  Handles the
// camera being below, inside or above the layer, and clips against the planet.
bool IntersectCloudLayer(float3 rayOrigin, float3 rayDir, out float tStart, out float tEnd)
{
    tStart = 0.0f;
    tEnd = 0.0f;

    const float3 centre = GetPlanetCentre();
    const float originRadius = GetRadiusFromCentre(rayOrigin);

    float2 innerHits;
    float2 outerHits;
    const bool hitInner = RaySphereIntersect(rayOrigin, rayDir, centre, gLayerBottomRadius, innerHits);
    if (!RaySphereIntersect(rayOrigin, rayDir, centre, gLayerTopRadius, outerHits))
        return false;

    if (originRadius < gLayerBottomRadius)
    {
        // Below the layer: the origin is inside both shells, so the far roots
        // are the entry and exit points.
        if (!hitInner || innerHits.y < 0.0f)
            return false;
        tStart = innerHits.y;
        tEnd = outerHits.y;
    }
    else if (originRadius > gLayerTopRadius)
    {
        // Above the layer: enter through the near root of the outer shell.
        if (outerHits.y < 0.0f)
            return false;
        tStart = max(outerHits.x, 0.0f);
        tEnd = (hitInner && innerHits.x > 0.0f) ? innerHits.x : outerHits.y;
    }
    else
    {
        // Inside the layer.
        tStart = 0.0f;
        tEnd = (hitInner && innerHits.x > 0.0f) ? innerHits.x : outerHits.y;
    }

    // Clip against the planet so downward rays stop at the ground.
    float2 planetHits;
    if (RaySphereIntersect(rayOrigin, rayDir, centre, gPlanetRadius, planetHits) && planetHits.x > 0.0f)
        tEnd = min(tEnd, planetHits.x);

    return tEnd > tStart;
}

// ---------------------------------------------------------------------------
// Cloud shape model
// ---------------------------------------------------------------------------

// Weather drifts with the base wind offset so storm cells travel across the sky
// at the same rate as the cloud mass inside them.
float4 SampleWeather(float2 worldXY)
{
    float2 uv = (worldXY + gWindOffset.xy) * gWeatherFrequency + float2(gWeatherOffsetX, gWeatherOffsetY);
    return gWeatherMap.SampleLevel(gLinearRepeatSampler, uv, 0.0f);
}

// Vertical density profiles, expressed as two smoothstep edges:
// (fadeInStart, fadeInEnd, fadeOutStart, fadeOutEnd) in height fraction.
float DensityHeightGradient(float heightFraction, float cloudType)
{
    const float4 stratus      = float4(0.02f, 0.12f, 0.22f, 0.36f);
    const float4 cumulus      = float4(0.00f, 0.16f, 0.50f, 0.78f);
    const float4 cumulonimbus = float4(0.00f, 0.08f, 0.82f, 1.00f);

    float4 gradient = lerp(
        lerp(stratus, cumulus, saturate(cloudType * 2.0f)),
        cumulonimbus,
        saturate(cloudType * 2.0f - 1.0f));

    return smoothstep(gradient.x, gradient.y, heightFraction)
         - smoothstep(gradient.z, gradient.w, heightFraction);
}

// detailAmount fades the high frequency erosion out with distance so far away
// clouds stay stable under temporal accumulation and cost far less to march.
float SampleCloudDensity(float3 worldPos, float heightFraction, float4 weather, float detailAmount)
{
    // Wind advection, with a horizontal shear that grows towards the top of the
    // layer and a vertical offset that leans cloud tops downwind.
    float2 windDirection = float2(gWindDirectionX, gWindDirectionY);
    float3 samplePos = worldPos + gWindOffset;
    samplePos.xy += windDirection * (heightFraction * gWindSkew * gLayerThickness);
    samplePos.z += heightFraction * gCloudTopOffset;

    float4 lowFrequency = gBaseShapeNoise.SampleLevel(
        gLinearRepeatSampler, samplePos * gBaseNoiseFrequency, 0.0f);

    float lowFrequencyFbm = lowFrequency.g * 0.625f
                          + lowFrequency.b * 0.250f
                          + lowFrequency.a * 0.125f;

    float baseShape = RemapClamped(lowFrequency.r, lowFrequencyFbm - 1.0f, 1.0f, 0.0f, 1.0f);

    // The weather map decides the local character; the sliders bias it.
    float cloudType = saturate(gCloudType * 0.5f + weather.g * 0.5f);
    baseShape *= DensityHeightGradient(heightFraction, cloudType);

    float coverage = RemapClamped(weather.r, 1.0f - gCoverage, 1.0f, 0.0f, 1.0f);

    // Anvil: tall clouds spread out near their tops.
    float anvilExponent = RemapClamped(
        heightFraction, 0.65f, 0.95f, 1.0f, lerp(1.0f, 0.45f, gAnvilBias * cloudType));
    float anvilCoverage = pow(max(coverage, 1e-4f), max(anvilExponent, 0.05f));

    float cloud = RemapClamped(baseShape, 1.0f - anvilCoverage, 1.0f, 0.0f, 1.0f) * anvilCoverage;
    if (cloud <= 0.0f)
        return 0.0f;

    if (detailAmount > 0.01f)
    {
        // Swirl the detail lookup through the curl field so erosion reads as
        // turbulence rather than uniform noise subtraction.
        float2 curl = gCurlNoise.SampleLevel(
            gLinearRepeatSampler, samplePos.xy * gBaseNoiseFrequency * 4.0f, 0.0f).rg * 2.0f - 1.0f;

        float3 detailPos = samplePos + gDetailWindOffset;
        detailPos.xy += curl * ((1.0f - heightFraction) * gCurlStrength * gLayerThickness * 0.05f);

        float4 detailSample = gDetailNoise.SampleLevel(
            gLinearRepeatSampler, detailPos * gDetailNoiseFrequency, 0.0f);

        float detailFbm = detailSample.r * 0.625f
                        + detailSample.g * 0.250f
                        + detailSample.b * 0.125f;

        // Billowy at the base, wispy filaments at the top.
        float detailModifier = lerp(detailFbm, 1.0f - detailFbm, saturate(heightFraction * 4.0f));
        detailModifier = lerp(detailModifier, 1.0f - detailSample.a, 0.25f * heightFraction);
        detailModifier *= gDetailStrength * detailAmount;

        cloud = RemapClamped(cloud, detailModifier, 1.0f, 0.0f, 1.0f);
    }

    // Precipitating cells hold noticeably more water.
    cloud *= lerp(1.0f, 1.75f, weather.b);

    return saturate(cloud) * gDensityScale;
}

// ---------------------------------------------------------------------------
// Scattering
// ---------------------------------------------------------------------------

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denominator = max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - g2) / (4.0f * kPi * denominator * sqrt(denominator));
}

// Dual-lobe phase: a strong forward lobe for the silver lining plus a backward
// lobe that keeps the shadowed side from going flat.
float DualLobePhase(float cosTheta)
{
    float forward = HenyeyGreenstein(cosTheta, clamp(gPhaseG0, -0.99f, 0.99f));
    float backward = HenyeyGreenstein(cosTheta, clamp(gPhaseG1, -0.99f, 0.99f));
    return lerp(forward, backward, saturate(gPhaseBlend));
}

// Six-tap cone march towards the sun.  The cone widens with distance so the
// shadow term picks up neighbouring cloud mass instead of a single thin line,
// which is what gives cumulus their soft self-shadowing.
float SampleLightOpticalDepth(float3 worldPos, float3 toLight, float detailAmount)
{
    const float3 coneOffsets[6] =
    {
        float3( 0.38f,  0.35f, -0.20f),
        float3(-0.20f,  0.51f,  0.35f),
        float3( 0.15f, -0.42f,  0.60f),
        float3(-0.55f, -0.15f, -0.30f),
        float3( 0.62f, -0.30f,  0.18f),
        float3( 0.00f,  0.00f,  0.00f)
    };

    const uint lightSteps = clamp(gLightSteps, 1u, 6u);
    float stepSize = gLightMarchDistance / (float)lightSteps;
    float coneRadius = gConeSpread * gLayerThickness;

    float opticalDepth = 0.0f;
    float travelled = 0.0f;

    [loop]
    for (uint i = 0u; i < lightSteps; ++i)
    {
        travelled += stepSize * 0.5f;

        float coneScale = (travelled / max(gLightMarchDistance, 1.0f)) * coneRadius;
        float3 samplePos = worldPos + toLight * travelled + coneOffsets[i] * coneScale;

        float radius = GetRadiusFromCentre(samplePos);
        if (radius < gLayerTopRadius && radius > gLayerBottomRadius)
        {
            float heightFraction = GetHeightFraction(samplePos);
            float4 weather = SampleWeather(samplePos.xy);
            // Only the near taps pay for detail erosion; the far taps are a
            // low-frequency occlusion estimate.
            float tapDetail = (i < 2u) ? detailAmount : 0.0f;
            opticalDepth += SampleCloudDensity(samplePos, heightFraction, weather, tapDetail) * stepSize;
        }

        travelled += stepSize * 0.5f;
        stepSize *= gShadowStepGrowth;
    }

    return opticalDepth;
}

#endif // VOLUMETRIC_CLOUD_COMMON_INCLUDED
