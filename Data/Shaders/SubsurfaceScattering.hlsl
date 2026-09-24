// SubsurfaceScattering.hlsl
// Screen-space subsurface scattering: Jimenez et al.'s Separable SSS
// (Source/SDKs/separable-sss-1.0, SeparableSSS.h's SSSSBlurPS), ported to the engine's
// deferred pipeline, plus the composite shared with the ray-traced mode.
//
// The deferred lighting resolve writes each subsurface pixel's diffuse lighting to its
// own target (alpha = 1 marks the pixel) and the full lighting to the scene. This pass
//   1. CSBlurX           blurs that diffuse horizontally into a scratch target, then
//   2. PSBlurYComposite  blurs it vertically and adds (scattered - original) to the
//                        scene, which swaps the diffuse term for its scattered version
//                        and leaves specular, fog and everything else untouched.
// The ray-traced mode (SubsurfaceScattering_RT.hlsl) produces the scattered diffuse in
// one go and uses PSRayTracedComposite for step 2 instead.
//
// Differences from the SDK:
//   - one kernel per material profile, chosen per pixel by the slot packed in the
//     normal target's W channel, instead of one global kernel;
//   - the blur width is resolved from the real projection and a world-space radius,
//     rather than a fixed SSSS_FOVY;
//   - taps landing on a different profile, or off subsurface pixels altogether, fall
//     back to the centre colour the way SSSS_FOLLOW_SURFACE treats depth jumps, so the
//     blur never pulls in unrelated surfaces;
//   - taps are point loads, so pixels outside the mask never leak in bilinearly.

#define PTERO_SSS_CB_REGISTER b0
#include "Subsurface.hlsli"
#include "SurfaceSpecular.hlsli"

Texture2D<float4>   gSssInput       : register(t0); // pass input (diffuse, X-blurred, or RT result)
Texture2D<float4>   gSssDiffuse     : register(t1); // original diffuse, alpha = subsurface mask
Texture2D<float4>   gSssNormalDepth : register(t2); // G-Buffer RT1: oct normal, device depth, packed W
RWTexture2D<float4> gSssOutput      : register(u0);

// -------------------------------------------------------------------------
// Separable blur (SSSSBlurPS)
// -------------------------------------------------------------------------
int2 ClampPixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(gSssRenderSize) - 1);
}

float3 SeparableBlur(uint2 pixel, float2 direction)
{
    const float4 colorM = gSssInput.Load(int3(pixel, 0));
    const float4 normalDepthM = gSssNormalDepth.Load(int3(pixel, 0));
    const uint slot = PteroDecodeSubsurfaceSlot(normalDepthM.w);

    if (!PteroSssSlotActive(slot) || gSssDiffuse.Load(int3(pixel, 0)).a < 0.5f)
        return colorM.rgb;

    const PteroSssProfile profile = gSssProfiles[slot];
    const float radius = profile.ColorRadius.a;
    const float depthM = PteroSssLinearDepth(normalDepthM.z);

    // Pixels spanned by one kernel unit (a third of the radius) at this depth. This is
    // the SDK's `sssWidth * scale / 3`, resolved from the actual projection so it holds
    // for any field of view and aspect ratio.
    const float2 pixelsPerUnit = (radius / 3.0f) * 0.5f
                               * float2(gSssProjScaleX, gSssProjScaleY) * gSssRenderSize
                               / max(depthM, 1e-4f);
    const float2 stepPixels = direction * pixelsPerUnit;

    // A kernel narrower than a pixel has nothing to redistribute.
    if (dot(stepPixels, stepPixels) * 9.0f < 0.25f)
        return colorM.rgb;

    const uint kernelBase = slot * PTERO_SSS_MAX_KERNEL;
    float3 blurred = colorM.rgb * gSssKernel[kernelBase].rgb;

    // How quickly a depth difference pushes a tap back to the centre colour.
    const float depthFalloff = (gSssFollowSurface != 0u) ? 0.5f / max(radius, 1e-5f) : 0.0f;

    [loop]
    for (uint i = 1u; i < gSssKernelSamples; ++i)
    {
        const float4 k = gSssKernel[kernelBase + i];
        const int2 tap = ClampPixel(int2(floor(float2(pixel) + 0.5f + k.a * stepPixels)));

        float3 color = gSssInput.Load(int3(tap, 0)).rgb;
        const float4 normalDepth = gSssNormalDepth.Load(int3(tap, 0));

        float fallback = (PteroDecodeSubsurfaceSlot(normalDepth.w) != slot
                          || gSssDiffuse.Load(int3(tap, 0)).a < 0.5f) ? 1.0f : 0.0f;
        // SSSS_FOLLOW_SURFACE: a large jump in depth means the tap left the surface.
        fallback = max(fallback, saturate(abs(depthM - PteroSssLinearDepth(normalDepth.z)) * depthFalloff));

        color = lerp(color, colorM.rgb, fallback);
        blurred += k.rgb * color;
    }

    return blurred;
}

[numthreads(8, 8, 1)]
void CSBlurX(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= (uint2)gSssRenderSize))
        return;
    gSssOutput[dispatchId.xy] = float4(SeparableBlur(dispatchId.xy, float2(1.0f, 0.0f)), 1.0f);
}

// -------------------------------------------------------------------------
// Composite
// -------------------------------------------------------------------------
struct VSOutput
{
    float4 Position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    const float2 ndc = float2(vertexId == 2 ? 3.0f : -1.0f, vertexId == 1 ? -3.0f : 1.0f);
    output.Position = float4(ndc, 0.0f, 1.0f);
    return output;
}

float3 SlotDebugColor(uint slot)
{
    // Distinct, stable hues per profile slot.
    const float hue = frac(slot * 0.618034f);
    return saturate(abs(frac(hue + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
}

// Normal output is (scattered - original) under additive blending, which swaps the
// diffuse term inside the scene colour. Debug views replace the scene instead (the
// pipeline is switched to no blending for them).
float4 ComposeOutput(uint2 pixel, float3 scattered)
{
    const float4 original = gSssDiffuse.Load(int3(pixel, 0));
    const bool sssPixel = original.a >= 0.5f;

    if (gSssDebugView == 1)
        return float4(sssPixel ? scattered : 0.0f.xxx, 1.0f);
    if (gSssDebugView == 2)
    {
        const uint slot = PteroDecodeSubsurfaceSlot(gSssNormalDepth.Load(int3(pixel, 0)).w);
        return float4(sssPixel ? SlotDebugColor(slot) : 0.0f.xxx, 1.0f);
    }

    if (!sssPixel)
        discard;
    return float4(scattered - original.rgb, 0.0f);
}

float4 PSBlurYComposite(VSOutput input) : SV_Target
{
    const uint2 pixel = uint2(input.Position.xy);
    const bool sssPixel = gSssDiffuse.Load(int3(pixel, 0)).a >= 0.5f;
    const float3 scattered = sssPixel ? SeparableBlur(pixel, float2(0.0f, 1.0f)) : 0.0f.xxx;
    return ComposeOutput(pixel, scattered);
}

// The ray-traced pass takes a dozen stochastic samples per pixel; this cleans up what is
// left of their noise before the swap. A 5x5 cross-bilateral filter: only neighbours with
// the same profile, inside the subsurface mask, and at a depth within the profile's reach
// contribute, so it cannot blur across a silhouette or into another material. Scattering
// is itself a blur at least this wide wherever it is visible, so the filter costs no
// detail the effect would have kept.
float4 PSRayTracedComposite(VSOutput input) : SV_Target
{
    const int2 pixel = int2(input.Position.xy);
    const float4 centre = gSssInput.Load(int3(pixel, 0));
    const float4 normalDepthM = gSssNormalDepth.Load(int3(pixel, 0));
    const uint slot = PteroDecodeSubsurfaceSlot(normalDepthM.w);

    if (gSssDiffuse.Load(int3(pixel, 0)).a < 0.5f || !PteroSssSlotActive(slot))
        return ComposeOutput(uint2(pixel), centre.rgb);

    const float depthM = PteroSssLinearDepth(normalDepthM.z);
    const float depthReach = max(gSssProfiles[slot].ColorRadius.a, 0.002f + 0.002f * depthM);

    float3 sum = centre.rgb;
    float weightSum = 1.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            const int2 tap = ClampPixel(pixel + int2(x, y));
            const float4 normalDepth = gSssNormalDepth.Load(int3(tap, 0));
            if (PteroDecodeSubsurfaceSlot(normalDepth.w) != slot || gSssDiffuse.Load(int3(tap, 0)).a < 0.5f)
                continue;

            const float depthDelta = abs(PteroSssLinearDepth(normalDepth.z) - depthM) / depthReach;
            const float weight = exp(-0.5f * float(x * x + y * y) / 2.25f) * saturate(1.0f - depthDelta);
            sum += gSssInput.Load(int3(tap, 0)).rgb * weight;
            weightSum += weight;
        }
    }

    return ComposeOutput(uint2(pixel), sum / weightSum);
}
