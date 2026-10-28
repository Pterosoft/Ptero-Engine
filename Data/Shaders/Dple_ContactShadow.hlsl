// Dple_ContactShadow.hlsl
// Screen-space contact shadows from the sun, traced in world space so the length and
// thickness settings are real metres rather than pixels.
//
// This does not replace the sun's shadow map. It runs on the resolved frame, where its job
// is the contact darkening a shadow map's texel size and bias cannot resolve.

#include "Dple_Common.hlsli"

Texture2D<float4>  gGuide            : register(t4);
RWTexture2D<float> gContactShadowOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gWorkSize))
        return;

    const float4 guide = gGuide[id.xy];
    const float centerDepth = guide.w;

    // No sun, pass-through pixel, or switched off: nothing to occlude.
    if (centerDepth <= 0.0f || gSunWeight <= 0.0f || !DpleHasFlag(DPLE_FLAG_CONTACT_SHADOWS))
    {
        gContactShadowOut[id.xy] = 1.0f;
        return;
    }

    const float3 centerNormal = guide.xyz;
    const float NdotL = dot(centerNormal, gSunDirection);

    // Facing away from the light is already fully shadowed by the lighting pass; adding
    // contact shadow there would double-darken the terminator.
    if (NdotL <= 0.0f)
    {
        gContactShadowOut[id.xy] = 1.0f;
        return;
    }

    const float2 viewUV = DpleWorkPixelToViewUV(id.xy);
    const float3 centerPos = DpleDepthWorldPosition(viewUV, DpleSampleDeviceZ(viewUV));

    const uint stepCount = max(gContactShadowSteps, 4u);
    const float stepLength = gContactShadowLength / stepCount;

    // Offset along the normal first, or the first step re-samples the origin surface and
    // every lit pixel shadows itself. Scaled with depth, as depth precision degrades.
    const float normalOffset = max(0.005f, 0.0015f * centerDepth);
    const float3 rayOrigin = centerPos + centerNormal * normalOffset;

    // Jittering the start trades banding for noise, which the temporal pass removes.
    const float jitter = DpleSampleRotation(id.xy);

    const float hitBias = max(0.0005f, 0.0005f * centerDepth);
    const float thicknessTolerance = gContactShadowThickness + stepLength;

    float visibility = 1.0f;

    [loop]
    for (uint stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        const float rayDistance = (stepIndex + jitter) * stepLength;
        const float3 samplePos = rayOrigin + gSunDirection * rayDistance;

        // The jittered projection lands directly in G-Buffer UV.
        const float3 projected = DpleProjectToUV(samplePos, gViewProj);
        if (projected.z <= 0.0f || any(projected.xy < 0.0f) || any(projected.xy > 1.0f))
            break;   // left the screen; screen space has nothing more to say

        const float surfaceZ = gDepth.SampleLevel(gPointClamp, projected.xy, 0.0f);
        if (surfaceZ >= 1.0f)
            continue;   // sky

        const float surfaceDepth = DpleViewDepth(DpleReconstructWorld(projected.xy, surfaceZ));
        const float depthDifference = DpleViewDepth(samplePos) - surfaceDepth;

        // Behind a surface, but only a hit while still inside that surface's assumed
        // thickness - past it the ray has merely passed behind unrelated background,
        // and counting that is what produces shadows floating in mid-air.
        if (depthDifference > hitBias && depthDifference < thicknessTolerance)
        {
            // Fade only over the last stretch of the trace. Grading by hit distance over
            // the whole ray turns the start jitter into visible noise.
            const float traceFraction = saturate(rayDistance / max(gContactShadowLength, DPLE_EPS));
            visibility = smoothstep(0.7f, 1.0f, traceFraction);
            break;
        }
    }

    // Fade out across the terminator, where geometric and shading normals disagree most.
    const float terminatorFade = saturate(NdotL * 4.0f);
    visibility = lerp(1.0f, visibility, terminatorFade * gSunWeight);

    gContactShadowOut[id.xy] = saturate(visibility);
}
