// Dple_Composite.hlsl
// Output-resolution composite: bilateral upsample of the working-resolution occlusion, a
// material-aware response, and occlusion applied to the indirect diffuse share only.
//
// The "indirect only" rule decides whether this reads as lighting or as a filter.
// Multiplying AO into the final image darkens direct sunlight, which nothing in the
// physical world does; it is the single most common reason post-process AO looks like dirt
// on the lens. The split is estimated from the sun's N.L and DPLE's own contact
// visibility - coarse, but it always errs toward doing less.

#include "Dple_Common.hlsli"

Texture2D<float4>   gSceneColor  : register(t4);   // the image being enhanced, output resolution
Texture2D<float4>   gOcclusion   : register(t5);   // micro, contact, broad, contact shadow (working res)
Texture2D<float4>   gGuide       : register(t6);
RWTexture2D<float4> gCompositeOut: register(u0);

// Per-material occlusion response.
struct DpleMaterialResponse
{
    float  MicroScale;
    float  ContactScale;
    float  BroadScale;
    float3 OcclusionTint;    // multiplied in proportion to how occluded the pixel is
    float  SaturationLift;   // added saturation in occluded areas
    float  Wetness;          // 0..1, inferred
};

DpleMaterialResponse GetMaterialResponse(DpleSurface surface)
{
    DpleMaterialResponse response;
    response.MicroScale = 1.0f;
    response.ContactScale = 1.0f;
    response.BroadScale = 1.0f;
    response.OcclusionTint = 1.0f.xxx;
    response.SaturationLift = 0.0f;
    response.Wetness = 0.0f;

    if (!DpleHasFlag(DPLE_FLAG_MATERIAL))
        return response;

    if (surface.Class == DPLE_CLASS_SKIN)
    {
        // Light that enters skin and leaves a few millimetres away fills in exactly the
        // creases screen-space AO wants to darken, and comes back red. Full-strength AO on
        // skin is the classic "dirty face" tell.
        response.MicroScale = gSkinAOScale;
        response.ContactScale = gSkinAOScale;
        response.BroadScale = gSkinAOScale;
        response.OcclusionTint = lerp(1.0f.xxx, float3(1.0f, 0.55f, 0.42f), gSkinWarmth);
    }
    else if (surface.Class == DPLE_CLASS_FOLIAGE)
    {
        // A leaf in shadow is lit through, not merely unlit, and what comes through is
        // green. Keep micro occlusion (vein and edge detail is real) but pull back the
        // larger radii and lift saturation where it is dark.
        response.ContactScale = gFoliageAOScale;
        response.BroadScale = gFoliageAOScale;
        response.SaturationLift = gFoliageSaturation;
    }

    // Wet surfaces have no tag, so they are inferred: a smooth non-metal. A heuristic -
    // a polished marble floor reads as wet to it.
    if (gWetRoughnessThreshold > 0.0f)
    {
        response.Wetness = saturate(1.0f - surface.Roughness / gWetRoughnessThreshold) * saturate(1.0f - surface.Metallic * 2.0f);
        response.MicroScale *= lerp(1.0f, 1.35f, response.Wetness);
    }

    return response;
}

// Depth- and normal-weighted upsample of the working-resolution occlusion. Without the two
// weights the upsample bleeds occlusion across silhouettes, which at half resolution is a
// two-pixel dark outline round every object - the artefact that makes cheap AO obvious.
float4 UpsampleOcclusion(float2 viewUV, float centerDepth, float3 centerNormal)
{
    const float2 workCoord = viewUV * float2(gWorkSize) - 0.5f;
    const int2 baseCoord = int2(floor(workCoord));
    const float2 fraction = workCoord - float2(baseCoord);

    float4 weightedSum = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int tapY = 0; tapY <= 1; ++tapY)
    {
        [unroll]
        for (int tapX = 0; tapX <= 1; ++tapX)
        {
            const int2 tapCoord = clamp(baseCoord + int2(tapX, tapY), int2(0, 0), int2(gWorkSize) - 1);
            const float4 tapGuide = gGuide.Load(int3(tapCoord, 0));
            if (tapGuide.w <= 0.0f)
                continue;

            const float bilinearWeight =
                (tapX == 0 ? 1.0f - fraction.x : fraction.x) *
                (tapY == 0 ? 1.0f - fraction.y : fraction.y);
            const float depthWeight = exp2(-16.0f * abs(tapGuide.w - centerDepth) / max(centerDepth, 0.01f));
            const float normalWeight = pow(saturate(dot(tapGuide.xyz, centerNormal)), 8.0f);

            const float weight = bilinearWeight * depthWeight * normalWeight + 1e-5f;
            weightedSum += gOcclusion.Load(int3(tapCoord, 0)) * weight;
            totalWeight += weight;
        }
    }

    // Every tap rejected means a lone sliver between depth discontinuities. "Unoccluded"
    // is the safe failure.
    return (totalWeight > 1e-4f) ? (weightedSum / totalWeight) : 1.0f.xxxx;
}

float3 AdjustSaturation(float3 color, float saturation)
{
    const float luma = DpleLuminance(color);
    return max(lerp(luma.xxx, color, saturation), 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= gOutputSize))
        return;

    const float4 sceneSample = gSceneColor.Load(int3(id.xy, 0));
    const float3 sceneColor = DpleSafeColor(sceneSample.rgb);
    const float2 viewUV = (float2(id.xy) + 0.5f) * gInvOutputSize;

    DpleSurface surface;
    if (!DpleLoadSurface(viewUV, surface))
    {
        // Sky, glass and forward-drawn surfaces pass straight through. They have no
        // indirect diffuse term for occlusion to modulate.
        float3 passthrough = sceneColor;
        if (gDebugView == 4u)
            passthrough = 0.0f.xxx;
        else if (gDebugView == 6u)
            passthrough = float3(DpleViewUVToGBufferUV(viewUV), 0.0f);
        gCompositeOut[id.xy] = float4(passthrough, sceneSample.a);
        return;
    }

    const float3 worldNormal = surface.Normal;
    const float4 occlusion = UpsampleOcclusion(viewUV, surface.ViewDepth, worldNormal);
    const float microVisibility = occlusion.x;
    const float contactVisibility = occlusion.y;
    const float broadVisibility = occlusion.z;
    const float contactShadowVisibility = occlusion.w;

    const DpleMaterialResponse material = GetMaterialResponse(surface);

    // Combine the three radii multiplicatively, then cap. Three individually defensible
    // intensities stack into crushed black creases otherwise.
    const float microOcclusion   = saturate((1.0f - microVisibility)   * gMicroAOIntensity   * material.MicroScale);
    const float contactOcclusion = saturate((1.0f - contactVisibility) * gContactAOIntensity * material.ContactScale);
    const float broadOcclusion   = saturate((1.0f - broadVisibility)   * gBroadAOIntensity   * material.BroadScale);

    float combinedOcclusion = 1.0f - (1.0f - microOcclusion) * (1.0f - contactOcclusion) * (1.0f - broadOcclusion);
    combinedOcclusion = min(combinedOcclusion, gMaxCombinedAO);

    const float ambientVisibility = 1.0f - combinedOcclusion;
    const float contactShadowOcclusion = (1.0f - contactShadowVisibility) * gContactShadowIntensity;

    // ---- indirect / direct split --------------------------------------------------
    const float NdotL = saturate(dot(worldNormal, gSunDirection));
    const float directLikelihood = saturate(gSunWeight * NdotL * contactShadowVisibility * gDirectLightWeight);
    const float estimatedIndirectFraction = lerp(gIndirectFractionMax, gIndirectFractionMin, directLikelihood);

    const float3 indirect = sceneColor * estimatedIndirectFraction;
    const float3 direct = max(sceneColor - indirect, 0.0f);

    // ---- apply --------------------------------------------------------------------
    const float3 occlusionTint = lerp(1.0f.xxx, material.OcclusionTint, combinedOcclusion);
    float3 newIndirect = indirect * ambientVisibility * occlusionTint;
    if (material.SaturationLift > 0.0f)
        newIndirect = AdjustSaturation(newIndirect, 1.0f + material.SaturationLift * combinedOcclusion);

    // Contact shadows occlude the direct term - the reason they are carried separately
    // from AO all the way through.
    float3 newDirect = direct * (1.0f - contactShadowOcclusion);

    if (material.Wetness > 0.0f)
    {
        // Wet reads as a darker diffuse under a hotter specular. Both halves matter:
        // darkening alone just looks like dirt.
        const float wetAmount = material.Wetness * gWetResponseStrength;
        newIndirect *= (1.0f - 0.5f * wetAmount);
        newDirect *= (1.0f + wetAmount);
    }

    float3 result = DpleSafeColor(newDirect + newIndirect);

    // ---- micro-specular response --------------------------------------------------
    // Everything above only darkens, and darkening alone is why surfaces still read flat.
    // Two things, both from data already in the G-Buffer, both applied to the pixel's
    // *specular share* so a matte surface is left alone however strong the setting:
    //   1. Specular occlusion - AO is a hemisphere quantity; a specular lobe only sees a
    //      cone. Without this the specular half of the frame is not occluded at all.
    //   2. Micro-specular gain - a relit GGX lobe against a normal tilted by the albedo
    //      gradient, which puts back the grain that mip filtering and specular
    //      anti-aliasing removed.
    float debugSpecularShare = 0.0f;
    float debugMicroGain = 1.0f;

    if (DpleHasFlag(DPLE_FLAG_MICRO_SPECULAR))
    {
        const float3 viewVector = gCameraPos - surface.WorldPos;
        const float3 V = normalize(viewVector);
        const float NoV = saturate(dot(worldNormal, V));
        const float roughness = surface.Roughness;

        // How much of this pixel's reflectance is specular. A property of the material, so
        // it can be derived exactly even though the lighting cannot be un-mixed. Same F0 the
        // deferred resolve uses.
        const float3 specularColor = PteroComputeF0(surface.Albedo, surface.Metallic, surface.Specular);
        const float specularLuma = DpleLuminance(DpleEnvBRDFApprox(specularColor, roughness, NoV));
        const float diffuseLuma = DpleLuminance(surface.Albedo * (1.0f - surface.Metallic));
        const float specularShare = specularLuma / max(specularLuma + diffuseLuma, DPLE_EPS);
        debugSpecularShare = specularShare;

        const float3 specularPart = result * specularShare;
        const float3 remainingPart = result - specularPart;

        const float specularOcclusion = lerp(1.0f,
            DpleSpecularOcclusion(NoV, ambientVisibility, roughness),
            saturate(gSpecularOcclusionStrength));

        // A band, not a low-pass. A mirror shows sub-pixel variation as reflection
        // distortion and a Lambertian surface scatters it away; it is mid-rough materials
        // that read flattest - weathered timber sits at 0.6-0.85.
        const float roughnessGate =
            smoothstep(0.0f, 0.12f, roughness) *
            (1.0f - smoothstep(gMicroSpecularRoughnessMax, 1.0f, roughness));

        // A wet or polished surface shows grain most.
        const float wetGate = 1.0f + material.Wetness;

        float microGain = 1.0f;

        // A ratio of two lobes rather than an added radiance: it needs no knowledge of the
        // sun's absolute intensity or the exposure, adds no net energy, and stays a
        // re-presentation of lighting the renderer already resolved.
        const float2 texelStep = gMicroSpecularDetailScale * gInvGBufferSize;
        const float directionalWeight = gSunWeight * roughnessGate * contactShadowVisibility;

        if (directionalWeight > 0.0f)
        {
            // Tangents from neighbouring depth rather than ddx/ddy - there are no quad
            // derivatives in compute, and a neighbour on other geometry is detected below.
            const float2 offsetUVx = viewUV + float2(texelStep.x, 0.0f);
            const float2 offsetUVy = viewUV + float2(0.0f, texelStep.y);
            const float neighborZx = DpleSampleDeviceZ(offsetUVx);
            const float neighborZy = DpleSampleDeviceZ(offsetUVy);

            float3 tangent = DpleDepthWorldPosition(offsetUVx, neighborZx) - surface.WorldPos;
            float3 bitangent = DpleDepthWorldPosition(offsetUVy, neighborZy) - surface.WorldPos;

            // A neighbour on different geometry describes the silhouette, not the surface,
            // and would put a bright rim on every object edge.
            const float maxNeighborDistance = 0.05f * surface.ViewDepth;
            const bool tangentFrameValid =
                neighborZx < 1.0f && neighborZy < 1.0f &&
                length(tangent) < maxNeighborDistance &&
                length(bitangent) < maxNeighborDistance;

            if (tangentFrameValid)
            {
                tangent -= worldNormal * dot(worldNormal, tangent);
                bitangent -= worldNormal * dot(worldNormal, bitangent);

                const float tangentLength = length(tangent);
                const float bitangentLength = length(bitangent);

                if (tangentLength > DPLE_EPS && bitangentLength > DPLE_EPS)
                {
                    tangent /= tangentLength;
                    bitangent /= bitangentLength;

                    const float2 gradient = DpleBaseColorGradient(viewUV, texelStep);

                    // Bound the tilt by the width of the lobe it perturbs, or this is a
                    // noise amplifier: a smooth surface's GGX lobe is a couple of degrees
                    // wide, and a fixed tilt would swing the ratio between its clamps from
                    // one pixel to the next.
                    const float lobeWidth = clamp(roughness * roughness, 0.05f, 1.0f);
                    const float2 boundedGradient = gradient * gMicroSpecularStrength * wetGate * lobeWidth;

                    const float3 perturbedNormal = normalize(
                        worldNormal - (boundedGradient.x * tangent + boundedGradient.y * bitangent));

                    const float3 L = gSunDirection;
                    const float3 H = normalize(L + V);

                    const float baseLobe =
                        DpleGGXDistribution(saturate(dot(worldNormal, H)), roughness) * saturate(dot(worldNormal, L));
                    const float microLobe =
                        DpleGGXDistribution(saturate(dot(perturbedNormal, H)), roughness) * saturate(dot(perturbedNormal, L));

                    // As N.L approaches zero so does the base lobe, and the ratio of two
                    // vanishing numbers is noise rather than a highlight.
                    const float terminatorFade = saturate(dot(worldNormal, L) * 4.0f);

                    // A tight clamp on purpose: anything wide enough to let a glint through
                    // lets a single noisy albedo texel through at the same amplitude.
                    const float lobeRatio = clamp(microLobe / max(baseLobe, 1e-4f), 0.5f, 2.0f);
                    microGain = lerp(1.0f, lobeRatio, saturate(directionalWeight * terminatorFade));
                }
            }
        }

        debugMicroGain = microGain;
        result = DpleSafeColor(remainingPart + specularPart * specularOcclusion * microGain);
    }

    // ---- debug views ----------------------------------------------------------------
    if (gDebugView == 1u)
    {
        result = ambientVisibility.xxx;
    }
    else if (gDebugView == 2u)
    {
        result = contactShadowVisibility.xxx;
    }
    else if (gDebugView == 3u)
    {
        const float fraction = DpleLuminance(indirect) / max(DpleLuminance(sceneColor), DPLE_EPS);
        result = float3(saturate(fraction), saturate(1.0f - fraction), 0.0f);
    }
    else if (gDebugView == 4u)
    {
        // Grey default, red skin, green foliage; blue creeps in with inferred wetness.
        float3 classColor = 0.5f.xxx;
        if (surface.Class == DPLE_CLASS_SKIN)
            classColor = float3(0.9f, 0.25f, 0.2f);
        else if (surface.Class == DPLE_CLASS_FOLIAGE)
            classColor = float3(0.2f, 0.8f, 0.25f);
        result = lerp(classColor, float3(0.15f, 0.35f, 1.0f), material.Wetness);
    }
    else if (gDebugView == 5u)
    {
        result = float3(microVisibility, contactVisibility, broadVisibility);
    }
    else if (gDebugView == 6u)
    {
        // The view/G-Buffer bridge drawn directly: red ramps left to right, green top to
        // bottom, both reaching the edge without flattening. A ramp that flattens partway
        // means every G-Buffer fetch past that point reads clamped edge texels.
        result = float3(DpleViewUVToGBufferUV(viewUV), 0.0f);
    }
    else if (gDebugView == 7u)
    {
        // How much of each pixel the micro-specular section may touch. Metal and wet
        // surfaces bright, chalk and raw plaster near black.
        result = debugSpecularShare.xxx;
    }
    else if (gDebugView == 8u)
    {
        // What the micro-specular gain is doing, amplified 4x so a working-but-subtle
        // setting still shows. Green brighter, red darker, flat grey = doing nothing.
        const float signedGain = (debugMicroGain - 1.0f) * 4.0f;
        result = float3(saturate(-signedGain), saturate(signedGain), 0.0f) + 0.15f;
    }

    gCompositeOut[id.xy] = float4(result, sceneSample.a);
}
