// VolumetricCloudConstants.hlsli
// The single constant buffer shared by every volumetric cloud pass, plus the
// handful of helpers that passes without access to the noise volumes still need.
//
// Conventions
//   * World space is Z-up and measured in metres; ground sits at z = 0.
//   * The planet is a sphere centred at (0, 0, -PlanetRadius) so the cloud
//     layer curves down to the horizon the way a real layer does.
//   * gSunDirection is the light *travel* direction (from the sun towards the
//     scene), matching HosekWilkieResult, so the vector towards the sun is
//     -gSunDirection.
//   * gViewProjInv / gPrevViewProj are pre-transposed on the CPU so the shader
//     multiplies row-vector-first: mul(vector, matrix).
//
// The layout must stay byte-for-byte identical to
// VolumetricCloudRenderer::CloudConstants.

#ifndef VOLUMETRIC_CLOUD_CONSTANTS_INCLUDED
#define VOLUMETRIC_CLOUD_CONSTANTS_INCLUDED

cbuffer CloudConstants : register(b0)
{
    uint2    gTraceResolution;
    uint2    gFullResolution;

    float2   gInvTraceResolution;
    float2   gInvFullResolution;

    uint     gFrameIndex;
    uint     gResolutionDivisor;
    uint     gTemporalEnabled;
    uint     gDebugView;

    float3   gCameraPos;
    float    gTimeSeconds;

    float3   gSunDirection;
    float    gSunIntensityScale;

    float3   gSunColor;
    float    gAmbientIntensityScale;

    float3   gSkyColor;
    float    gGroundBounceScale;

    float3   gScatteringAlbedo;
    float    gExtinctionScale;

    float4x4 gViewProjInv;
    float4x4 gPrevViewProj;

    float    gPlanetRadius;
    float    gLayerBottomRadius;
    float    gLayerTopRadius;
    float    gLayerThickness;

    float    gCoverage;
    float    gCloudType;
    float    gDensityScale;
    float    gBaseNoiseFrequency;

    float    gDetailNoiseFrequency;
    float    gDetailStrength;
    float    gCurlStrength;
    float    gAnvilBias;

    float    gWeatherFrequency;
    float    gWeatherOffsetX;
    float    gWeatherOffsetY;
    float    gCloudTopOffset;

    float3   gWindOffset;
    float    gWindSkew;

    float3   gDetailWindOffset;
    float    gWindDirectionX;

    float    gWindDirectionY;
    float    gPhaseG0;
    float    gPhaseG1;
    float    gPhaseBlend;

    float    gPowderStrength;
    uint     gMsOctaves;
    float    gMsScatterFalloff;
    float    gMsExtinctionFalloff;

    float    gMsPhaseFalloff;
    uint     gMaxSteps;
    uint     gLightSteps;
    float    gLightMarchDistance;

    float    gMaxTraceDistance;
    float    gDistanceFadeStart;
    float    gDetailFadeDistance;
    float    gTemporalBlend;

    float    gShadowStepGrowth;
    float    gHistoryValid;
    float    gConeSpread;
    float    _CloudPad0;
};

static const float kPi = 3.14159265359f;
static const float kIsotropicPhase = 0.07957747f; // 1 / (4 pi)

float RemapClamped(float value, float inMin, float inMax, float outMin, float outMax)
{
    return saturate(outMin + (value - inMin) * (outMax - outMin) / max(inMax - inMin, 1e-5f));
}

float3 ReconstructWorldPosition(float2 uv, float deviceDepth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f);
    float4 worldPos = mul(ndc, gViewProjInv);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

#endif // VOLUMETRIC_CLOUD_CONSTANTS_INCLUDED
