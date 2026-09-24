// Subsurface.hlsli
// Shared description of the subsurface-scattering profiles, and the per-frame constants
// every subsurface pass reads. Included by the deferred lighting resolve (which splits
// out the diffuse light to scatter and adds shadow-map transmission), by the screen-space
// separable blur, and by the ray-traced scatter pass.
//
// The profile model is Jimenez et al.'s Separable Subsurface Scattering
// (Source/SDKs/separable-sss-1.0): d'Eon's sum-of-Gaussians skin profile, using its red
// column for all three channels and stretching each channel by a per-material falloff.
// Kernels are built on the CPU (SubsurfaceScattering.cpp, a port of the SDK's
// calculateKernel) because they depend only on the material, not on the pixel.
//
// A pixel's profile travels through the G-Buffer as a slot number packed into the
// normal target's W channel (see SurfaceSpecular.hlsli). Slot 0 is "no scattering".
//
// Must match SubsurfaceGpuConstants in SubsurfaceScattering.h.

#ifndef PTERO_SUBSURFACE_HLSLI
#define PTERO_SUBSURFACE_HLSLI

#include "LightShapes.hlsli"

#ifndef PTERO_SSS_CB_REGISTER
#define PTERO_SSS_CB_REGISTER b0
#endif

#define PTERO_SSS_MAX_PROFILES   16
#define PTERO_SSS_MAX_KERNEL     25
#define PTERO_SSS_MAX_LIGHTS     16

struct PteroSssProfile
{
    float4 ColorRadius;          // rgb = scatter strength, a = kernel radius in metres
    float4 FalloffTranslucency;  // rgb = per-channel falloff, a = translucency 0..1
};

cbuffer SubsurfaceConstants : register(PTERO_SSS_CB_REGISTER)
{
    PteroSssProfile gSssProfiles[PTERO_SSS_MAX_PROFILES];
    // Per profile, gSssKernelSamples taps: rgb = weight, a = offset in kernel units
    // (-2..2 or -3..3, one unit = a third of the radius). Tap 0 is the centre.
    float4   gSssKernel[PTERO_SSS_MAX_PROFILES * PTERO_SSS_MAX_KERNEL];

    float4x4 gSssViewProj;       // jittered, transposed for mul(row, M)
    float4x4 gSssInvViewProj;

    float3   gSssCameraPos;
    uint     gSssKernelSamples;

    float2   gSssRenderSize;
    float2   gSssInvRenderSize;

    float    gSssProjScaleX;     // projection _11
    float    gSssProjScaleY;     // projection _22
    float    gSssDepthA;         // projection _33
    float    gSssDepthB;         // projection _43

    uint     gSssEnabled;
    uint     gSssMode;           // 0 = screen space, 1 = ray traced
    uint     gSssFollowSurface;
    uint     gSssTransmission;

    float    gSssTransmissionIntensity;
    uint     gSssRtSamples;
    uint     gSssFrameIndex;
    int      gSssDebugView;

    float3   gSssSunDirection;   // from the sun toward the scene
    float    _SssPad0;
    float3   gSssSunColor;
    float    _SssPad1;
    float3   gSssSkyAmbient;
    int      gSssNumLights;

    PteroLightData gSssLights[PTERO_SSS_MAX_LIGHTS];

    // The deferred lighting pass's shadow maps, which the ray-traced mode shadows its
    // scatter samples with (see SubsurfaceScattering_RT.hlsl).
    float4x4 gSssLightViewProj;          // sun, used as mul(float4(p, 1), M)
    float    gSssShadowMapSize;
    float    gSssShadowBias;
    float    gSssPointShadowMapSize;
    float    gSssPointShadowBias;
    float4x4 gSssPointFaceViewProj[24];  // 4 shadow-casting lights x 6 faces
    uint     gSssHasSunShadow;
    uint     gSssHasPointShadows;
    float2   _SssShadowPad;
};

bool PteroSssSlotActive(uint slot)
{
    return gSssEnabled != 0u && slot > 0u && slot < PTERO_SSS_MAX_PROFILES
        && gSssProfiles[slot].ColorRadius.a > 0.0f;
}

// Device depth -> view-space distance along the camera axis, in metres.
float PteroSssLinearDepth(float deviceDepth)
{
    return gSssDepthB / max(deviceDepth - gSssDepthA, 1e-7f);
}

// Transmittance through `thickness` metres of the medium, from the SDK's
// SSSSTransmittance. The SDK evaluates the full six-Gaussian skin profile with
// per-channel weights; here the red column is used for every channel and each channel is
// stretched by its falloff instead, the same generalisation the SDK applies to the
// reflectance kernel, so a green-falloff material transmits green rather than skin red.
float3 PteroSssTransmittanceProfile(PteroSssProfile profile, float thickness)
{
    const float translucency = saturate(profile.FalloffTranslucency.a);
    const float radius = max(profile.ColorRadius.a, 1e-5f);
    const float scale = 8.25f * (1.0f - min(translucency, 0.995f)) / radius;
    const float3 d = (scale * thickness) / (0.001f + profile.FalloffTranslucency.rgb);
    const float3 dd = -d * d;
    return 0.233f * exp(dd / 0.0064f)
         + 0.100f * exp(dd / 0.0484f)
         + 0.118f * exp(dd / 0.187f)
         + 0.113f * exp(dd / 0.567f)
         + 0.358f * exp(dd / 1.99f)
         + 0.078f * exp(dd / 7.41f);
}

// Thickness past which every channel's transmittance is effectively zero, so a caller can
// skip the work (and a ray can stop searching).
float PteroSssMaxTransmissionThickness(PteroSssProfile profile)
{
    const float translucency = saturate(profile.FalloffTranslucency.a);
    if (translucency <= 0.0f)
        return 0.0f;
    const float radius = max(profile.ColorRadius.a, 1e-5f);
    const float scale = 8.25f * (1.0f - min(translucency, 0.995f)) / radius;
    const float maxFalloff = max(profile.FalloffTranslucency.r,
                             max(profile.FalloffTranslucency.g, profile.FalloffTranslucency.b));
    // 0.078 * exp(-d^2 / 7.41) < 1e-3  <=>  d > ~5.7
    return 5.7f * (0.001f + maxFalloff) / scale;
}

// Light arriving through the back of the surface. `L` points from the surface to the
// light. The SDK's wrap term keeps a little transmission alive near the terminator.
float3 PteroSssTransmission(PteroSssProfile profile, float thickness, float3 N, float3 L,
                            float3 lightRadiance, float3 albedo)
{
    if (profile.FalloffTranslucency.a <= 0.0f)
        return 0.0f.xxx;
    return PteroSssTransmittanceProfile(profile, thickness)
         * saturate(0.3f + dot(L, -N))
         * lightRadiance * albedo * gSssTransmissionIntensity;
}

#endif // PTERO_SUBSURFACE_HLSLI
