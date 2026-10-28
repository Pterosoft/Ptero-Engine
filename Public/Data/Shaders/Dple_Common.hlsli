// Dple_Common.hlsli
// Shared by every DPLE pass (Deterministic Photoreal Lighting Enhancer). Ported from the
// Unreal plugin; DpleRenderer.h has the pass chain and why it sits where it does.
//
// Coordinate spaces used throughout:
//   ViewUV    - 0..1 across the view, un-jittered. The image DPLE modifies is the output
//               of TAA or an upscaler, which has had the camera jitter resolved out of it,
//               and every working-resolution texture is laid out in this space too.
//   GBufferUV - 0..1 into the G-Buffer and depth. Those are still at render resolution AND
//               still carry this frame's sub-pixel jitter, so the bridge is ViewUV plus the
//               jitter (zero when DPLE is fed raw scene colour, which is jittered itself).
//
// Distances are metres. Linear "depth" is view-space distance along the camera forward.

#ifndef PTERO_DPLE_COMMON_HLSLI
#define PTERO_DPLE_COMMON_HLSLI

#include "SurfaceSpecular.hlsli"

// Mirrors DpleRenderer::DpleCbData.
cbuffer DpleConstants : register(b0)
{
    float4x4 gInvViewProj;           // clip -> world, jittered (matches the G-Buffer)
    float4x4 gViewProj;              // world -> clip, jittered
    float4x4 gViewProjNoJitter;      // this frame, jitter removed
    float4x4 gPrevViewProjNoJitter;  // last frame DPLE ran, jitter removed

    float3 gCameraPos;          float  gSunWeight;       // 0 when there is no sun to key off
    float3 gCameraForward;      float  gProjScaleX;      // projection _11
    float3 gSunDirection;       float  gProjScaleY;      // unit vector TOWARD the sun; projection _22
    float2 gJitterUV;           float2 gGBufferSize;
    float2 gInvGBufferSize;     float2 gOutputSize;
    float2 gInvOutputSize;      float2 gInvWorkSize;
    uint2  gWorkSize;           uint   gFrameIndex;      uint  gDebugView;
    uint   gFlags;              uint   gHistoryValid;    uint  gContactShadowSteps;  int gDenoiseRadius;

    float gMicroRadius;             float gContactRadius;           float gBroadRadius;             float gAOBias;
    float gAOPower;                 float gMicroAOIntensity;        float gContactAOIntensity;      float gBroadAOIntensity;
    float gMaxCombinedAO;           float gContactShadowLength;     float gContactShadowThickness;  float gContactShadowIntensity;
    float gSkinAOScale;             float gSkinWarmth;              float gFoliageAOScale;          float gFoliageSaturation;
    float gWetRoughnessThreshold;   float gWetResponseStrength;     float gSpecularOcclusionStrength; float gMicroSpecularStrength;
    float gMicroSpecularRoughnessMax; float gMicroSpecularDetailScale; float gIndirectFractionMin;  float gIndirectFractionMax;
    float gDirectLightWeight;       float gHistoryWeightStable;     float gHistoryWeightMoving;     float gDisocclusionDepthTolerance;
    float gNeighborhoodClampScale;  float gDenoiseDepthTolerance;   float gFineDetailStrength;      float gStructureStrength;
    float gMaxLuminanceChange;      float gDetailMotionScale;       float _DplePad0;                float _DplePad1;
};

// Per-dispatch values that differ between two dispatches of the same pass (root constants).
cbuffer DplePassConstants : register(b1)
{
    int2   gStepDirection;   // denoise: (1,0) horizontal, (0,1) vertical
    uint2  gDestSize;        // downsample: destination extent
    float2 gSourceSize;      // downsample: the source's valid region, in source pixels
    float2 gSourceInvSize;   // downsample: 1 / source texture size
};

#define DPLE_FLAG_AO               0x1u
#define DPLE_FLAG_CONTACT_SHADOWS  0x2u
#define DPLE_FLAG_TEMPORAL         0x4u
#define DPLE_FLAG_MATERIAL         0x8u
#define DPLE_FLAG_MICRO_SPECULAR   0x10u

#define DPLE_EPS      1e-6f
#define DPLE_MAX_HALF 65504.0f
#define DPLE_PI       3.14159265f

// Every pass sees the G-Buffer at the same registers, so the bridge helpers below can be
// shared. Passes add their own inputs from t4 up.
Texture2D<float>  gDepth          : register(t0);   // D32 as R32_FLOAT, standard Z, sky = 1
Texture2D<float4> gGBufferNormal  : register(t1);   // .xy oct normal, .z device depth, .w specular/SSS/foliage
Texture2D<float4> gGBufferMaterial: register(t2);   // .r roughness, .g metallic, .b ao, .a glass
Texture2D<float4> gGBufferAlbedo  : register(t3);

SamplerState gLinearClamp : register(s0);
SamplerState gPointClamp  : register(s1);

// ---------------------------------------------------------------------------
// Small numeric helpers
// ---------------------------------------------------------------------------

float DpleLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// -min(-x, 0) rather than max(x, 0): this also flushes NaN to zero, which max does not.
// A single NaN in the image would otherwise propagate through the bilateral weights and
// blow out a whole tile.
float3 DpleSafeColor(float3 color)
{
    return min(-min(-color, 0.0f), DPLE_MAX_HALF);
}

bool DpleHasFlag(uint flag)
{
    return (gFlags & flag) != 0u;
}

// Jorge Jimenez's interleaved gradient noise, offset per frame so temporal accumulation
// integrates new samples instead of re-averaging the same error.
float DpleInterleavedGradientNoise(float2 pixel, float frameId)
{
    pixel += frameId * (float2(47.0f, 17.0f) * 0.695f);
    const float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(pixel, magic.xy)));
}

float DpleSampleRotation(uint2 workPixel)
{
    return DpleInterleavedGradientNoise(float2(workPixel), float(gFrameIndex & 63u));
}

// ---------------------------------------------------------------------------
// Space conversions
// ---------------------------------------------------------------------------

float2 DpleViewUVToGBufferUV(float2 viewUV)
{
    return saturate(viewUV) + gJitterUV;
}

float2 DpleWorkPixelToViewUV(uint2 workPixel)
{
    return (float2(workPixel) + 0.5f) * gInvWorkSize;
}

// Same reconstruction DeferredLighting.hlsl and Ssr.hlsl use.
float3 DpleReconstructWorld(float2 gbufferUV, float deviceZ)
{
    const float4 ndc = float4(gbufferUV.x * 2.0f - 1.0f, 1.0f - gbufferUV.y * 2.0f, deviceZ, 1.0f);
    const float4 world = mul(ndc, gInvViewProj);
    return world.xyz / world.w;
}

float DpleViewDepth(float3 worldPos)
{
    return dot(worldPos - gCameraPos, gCameraForward);
}

float3 DpleDecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

// Projects a world point with a (row-vector) view-projection. xy = UV, z = clip w;
// a non-positive z means behind the camera and every caller has to reject it.
float3 DpleProjectToUV(float3 worldPos, float4x4 viewProj)
{
    const float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    if (clip.w <= DPLE_EPS)
        return float3(0.0f, 0.0f, -1.0f);
    const float2 ndc = clip.xy / clip.w;
    return float3(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, clip.w);
}

// ---------------------------------------------------------------------------
// Depth and G-Buffer access, always through the view-UV bridge
// ---------------------------------------------------------------------------

float DpleSampleDeviceZ(float2 viewUV)
{
    return gDepth.SampleLevel(gPointClamp, DpleViewUVToGBufferUV(viewUV), 0.0f);
}

// World position of whatever the depth buffer holds at viewUV. Callers reject sky first.
float3 DpleDepthWorldPosition(float2 viewUV, float deviceZ)
{
    return DpleReconstructWorld(DpleViewUVToGBufferUV(viewUV), deviceZ);
}

// The Unreal plugin keyed its material response off the G-Buffer's ShadingModelID. Ptero
// has no such field; these are what the G-Buffer does record per pixel.
#define DPLE_CLASS_DEFAULT  0u
#define DPLE_CLASS_SKIN     1u   // carries a subsurface-scattering profile
#define DPLE_CLASS_FOLIAGE  2u   // written by the vegetation shaders (negative normal.w)

struct DpleSurface
{
    float3 WorldPos;
    float3 Normal;
    float  ViewDepth;
    float3 Albedo;
    float  Roughness;
    float  Metallic;
    float  Specular;
    uint   Class;
};

// Loads the G-Buffer surface under viewUV. Returns false for every pixel DPLE must pass
// through untouched: sky, glass, and anything a forward pass drew over the G-Buffer.
//
// The forward test is what keeps water from being darkened by the occlusion of the lake
// bed under it. The water pass writes the depth buffer but not the G-Buffer, and the
// G-Buffer's normal target records the depth the G-Buffer pass itself wrote (.z). Where the
// two disagree, the surface on screen is not the one the G-Buffer describes.
bool DpleLoadSurface(float2 viewUV, out DpleSurface surface)
{
    surface = (DpleSurface)0;

    const float2 gbufferUV = DpleViewUVToGBufferUV(viewUV);
    const float deviceZ = gDepth.SampleLevel(gPointClamp, gbufferUV, 0.0f);
    if (deviceZ >= 1.0f)
        return false;   // sky

    const float4 normalSample = gGBufferNormal.SampleLevel(gPointClamp, gbufferUV, 0.0f);
    const float gbufferZ = normalSample.z;
    if (gbufferZ <= 0.0f || gbufferZ >= 1.0f)
        return false;   // nothing in the G-Buffer here

    surface.WorldPos = DpleReconstructWorld(gbufferUV, deviceZ);
    surface.ViewDepth = DpleViewDepth(surface.WorldPos);

    if (abs(gbufferZ - deviceZ) > 1e-7f)
    {
        // Compared as linear depth: device depth is so compressed near 1 that any fixed
        // epsilon there is either blind at range or trips on rounding up close.
        const float gbufferDepth = DpleViewDepth(DpleReconstructWorld(gbufferUV, gbufferZ));
        if (abs(gbufferDepth - surface.ViewDepth) > 0.002f * max(surface.ViewDepth, 0.01f))
            return false;
    }

    const float4 material = gGBufferMaterial.SampleLevel(gPointClamp, gbufferUV, 0.0f);
    // Glass repacks the material target (.a = encoded IOR, .b = dispersion) and draws its
    // own lighting; same test Ssr.hlsl uses.
    if (material.a > (1.0f / 255.0f) && material.b < 0.75f)
        return false;

    surface.Normal = DpleDecodeOctNormal(normalSample.xy);
    surface.Albedo = gGBufferAlbedo.SampleLevel(gPointClamp, gbufferUV, 0.0f).rgb;
    surface.Roughness = saturate(material.r);
    surface.Metallic = saturate(material.g);
    surface.Specular = PteroDecodeSurfaceSpecular(normalSample.w);

    if (PteroIsFoliageSurface(normalSample.w))
        surface.Class = DPLE_CLASS_FOLIAGE;
    else if (PteroDecodeSubsurfaceSlot(normalSample.w) != 0u)
        surface.Class = DPLE_CLASS_SKIN;
    else
        surface.Class = DPLE_CLASS_DEFAULT;

    return true;
}

// ---------------------------------------------------------------------------
// Specular response
// ---------------------------------------------------------------------------

// Analytic split-sum environment BRDF (Karis, mobile fit). Used only to work out what
// *fraction* of a pixel's reflectance is specular - never to light anything.
float3 DpleEnvBRDFApprox(float3 specularColor, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    const float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return specularColor * AB.x + AB.y;
}

// Frostbite's specular occlusion from an ambient occlusion term: narrows the diffuse AO
// by roughness and view angle, so a smooth surface in a crevice keeps its reflection while
// a rough one loses it.
float DpleSpecularOcclusion(float NoV, float ambientVisibility, float roughness)
{
    return saturate(pow(abs(NoV + ambientVisibility), exp2(-16.0f * roughness - 1.0f)) - 1.0f + ambientVisibility);
}

// GGX normal distribution. Only the lobe *shape* is needed here.
float DpleGGXDistribution(float NoH, float roughness)
{
    const float a = max(roughness * roughness, 1e-3f);
    const float a2 = a * a;
    const float d = (NoH * a2 - NoH) * NoH + 1.0f;
    return a2 / max(DPLE_PI * d * d, 1e-8f);
}

// Central-difference gradient of base-colour luminance, normalised by the local mean.
// A gradient, not a high-pass: a Laplacian has no direction and can only modulate
// contrast, while a gradient says "the surface slopes this way here", which is what a
// specular lobe needs to shift a highlight. Bilinear rather than point taps, because the
// G-Buffer is jittered and a point tap would flicker between texels on a still surface.
float2 DpleBaseColorGradient(float2 viewUV, float2 texelStep)
{
    const float2 uv = DpleViewUVToGBufferUV(viewUV);
    const float left  = DpleLuminance(gGBufferAlbedo.SampleLevel(gLinearClamp, uv - float2(texelStep.x, 0.0f), 0.0f).rgb);
    const float right = DpleLuminance(gGBufferAlbedo.SampleLevel(gLinearClamp, uv + float2(texelStep.x, 0.0f), 0.0f).rgb);
    const float up    = DpleLuminance(gGBufferAlbedo.SampleLevel(gLinearClamp, uv - float2(0.0f, texelStep.y), 0.0f).rgb);
    const float down  = DpleLuminance(gGBufferAlbedo.SampleLevel(gLinearClamp, uv + float2(0.0f, texelStep.y), 0.0f).rgb);

    const float mean = max((left + right + up + down) * 0.25f, 0.02f);
    return clamp(float2(right - left, down - up) / (2.0f * mean), -1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Sampling patterns
// ---------------------------------------------------------------------------

// Screen-space radius, in ViewUV units, subtended by a world-space radius at a depth.
// ScreenPos spans 2 units across the viewport and ViewUV spans 1, hence the 0.5.
float2 DpleWorldRadiusToViewUV(float worldRadius, float viewDepth)
{
    return 0.5f * worldRadius * float2(abs(gProjScaleX), abs(gProjScaleY)) / max(viewDepth, 0.01f);
}

// Golden-angle spiral tap, rotated per pixel so the noise is dithered rather than banded.
float2 DpleSpiralTap(uint tapIndex, uint tapCount, float rotation)
{
    const float goldenAngle = 2.39996323f;
    const float angle = tapIndex * goldenAngle + rotation * 2.0f * DPLE_PI;
    const float radius = sqrt((tapIndex + 0.5f) / tapCount);
    float s, c;
    sincos(angle, s, c);
    return float2(c, s) * radius;
}

#endif // PTERO_DPLE_COMMON_HLSLI
