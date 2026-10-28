// Dple_MultiscaleAO.hlsl
// Multiscale ambient occlusion: three independent world-space radii resolved in one pass.
//
// The one piece of DPLE that is genuinely additive rather than a re-presentation. A single
// radius AO (XeGTAO, RTAO) has to pick between resolving a pore and resolving an alcove.
// Keeping micro, contact and broad separate is what lets the composite respond to them
// differently per material, which one combined AO term cannot express.
//
// Alchemy-style estimator: cosine-weighted, inverse-square falloff over a golden-angle
// spiral, rotated per pixel and per frame so the temporal pass integrates new samples.

#include "Dple_Common.hlsli"

Texture2D<float4>   gGuide        : register(t4);
RWTexture2D<float4> gOcclusionOut : register(u0);

#define DPLE_AO_TAPS 8

// Raw occlusion in 0..1 for one world-space radius.
//
// Samples beyond twice the radius are rejected outright rather than merely down-weighted.
// A screen-space tap lands on whatever happens to be at that pixel, so a foreground
// silhouette would otherwise count as a large, close-in-screen, far-in-world occluder -
// exactly the dark halo that makes screen-space AO read as fake.
float ComputeOcclusion(float2 viewUV, float3 centerPos, float3 centerNormal, float centerDepth,
                       float worldRadius, float rotation, uint tapCount)
{
    // A radius covering most of the screen is a vignette, not an occlusion query.
    const float2 radiusUV = min(DpleWorldRadiusToViewUV(worldRadius, centerDepth), 0.08f);

    // Below roughly a texel the taps all land on the centre pixel and return noise.
    if (all(radiusUV * float2(gWorkSize) < 0.75f))
        return 0.0f;

    // Two failure modes to cover: self-occlusion from normal/depth disagreement on a curved
    // surface (scales with the radius) and depth quantisation at distance (with depth).
    const float biasWorld = gAOBias * (worldRadius + 0.01f * centerDepth);
    const float maxDistanceSq = 4.0f * worldRadius * worldRadius;

    float sum = 0.0f;

    [loop]
    for (uint tap = 0; tap < tapCount; ++tap)
    {
        const float2 sampleUV = viewUV + DpleSpiralTap(tap, tapCount, rotation) * radiusUV;
        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
            continue;

        const float sampleZ = DpleSampleDeviceZ(sampleUV);
        if (sampleZ >= 1.0f)
            continue;   // sky occludes nothing

        const float3 delta = DpleDepthWorldPosition(sampleUV, sampleZ) - centerPos;
        const float distanceSq = dot(delta, delta);
        if (distanceSq > maxDistanceSq)
            continue;

        // 5e-6 m^2 is the Unreal plugin's 0.05 cm^2 guard against a coincident tap.
        sum += max(0.0f, dot(delta, centerNormal) - biasWorld) / (distanceSq + 5e-6f);
    }

    // 2R/N makes the result dimensionless and radius-independent, so the three radii are
    // directly comparable and the intensities mean the same thing at every scale.
    return saturate((2.0f * worldRadius / tapCount) * sum);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gWorkSize))
        return;

    const float4 guide = gGuide[id.xy];
    const float centerDepth = guide.w;

    if (centerDepth <= 0.0f || !DpleHasFlag(DPLE_FLAG_AO))
    {
        gOcclusionOut[id.xy] = 1.0f.xxxx;
        return;
    }

    const float2 viewUV = DpleWorkPixelToViewUV(id.xy);
    const float3 centerNormal = guide.xyz;
    const float3 centerPos = DpleDepthWorldPosition(viewUV, DpleSampleDeviceZ(viewUV));
    const float rotation = DpleSampleRotation(id.xy);

    const float micro   = ComputeOcclusion(viewUV, centerPos, centerNormal, centerDepth, gMicroRadius,   rotation,         DPLE_AO_TAPS);
    const float contact = ComputeOcclusion(viewUV, centerPos, centerNormal, centerDepth, gContactRadius, rotation + 0.37f, DPLE_AO_TAPS);
    // The broad radius describes low-frequency room shape, which survives fewer taps plus
    // temporal accumulation better than the tight radii do.
    const float broad   = ComputeOcclusion(viewUV, centerPos, centerNormal, centerDepth, gBroadRadius,   rotation + 0.71f, DPLE_AO_TAPS - 2);

    const float3 visibility = 1.0f - pow(float3(micro, contact, broad), gAOPower.xxx);
    gOcclusionOut[id.xy] = float4(saturate(visibility), 1.0f);
}
