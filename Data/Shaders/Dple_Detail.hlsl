// Dple_Detail.hlsl
// Multiscale detail enhancement by frequency separation - DPLE's last pass, off by default.
//
// The risk, stated plainly: this re-amplifies exactly the frequency band TAA and the
// upscalers spent their budget suppressing. Running after them rather than before is what
// makes it survivable at all, and it still needs validating against a slow and a fast pan,
// not stills - which is why the motion suppression and a hard luminance clamp are on the
// critical path rather than optional.

#include "Dple_Common.hlsli"

Texture2D<float4>   gComposite     : register(t4);   // DPLE's composite: what detail is added TO
Texture2D<float4>   gDetailSource  : register(t5);   // the image as it arrived: where bands come FROM
Texture2D<float4>   gFineBase      : register(t6);   // 1/4 resolution of the detail source
Texture2D<float4>   gStructureBase : register(t7);   // 1/16 resolution of the detail source
RWTexture2D<float4> gDetailOut     : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= gOutputSize))
        return;

    const float4 source = gComposite.Load(int3(id.xy, 0));
    const float3 color = DpleSafeColor(source.rgb);
    const float2 viewUV = (float2(id.xy) + 0.5f) * gInvOutputSize;

    const float3 fineBase = DpleSafeColor(gFineBase.SampleLevel(gLinearClamp, viewUV, 0.0f).rgb);
    const float3 structureBase = DpleSafeColor(gStructureBase.SampleLevel(gLinearClamp, viewUV, 0.0f).rgb);

    // The bands come from the image as it arrived, not from the composite. Deriving them
    // from the composite made this an amplifier for DPLE's own output: AO, contact shadow
    // and micro-specular noise all land in the fine band and get multiplied on the way out.
    // The incoming image has been through TAA or the upscaler and is the most temporally
    // stable signal available at this point.
    const float3 detailSource = DpleSafeColor(gDetailSource.Load(int3(id.xy, 0)).rgb);
    const float3 fineBand = detailSource - fineBase;
    const float3 structureBand = fineBase - structureBase;

    // One motion scale for the whole view, computed on the CPU from camera angular speed.
    // Per-pixel screen velocity is inversely proportional to depth under translation, so
    // it would suppress the near half of the frame and not the far half and split the
    // image along a depth boundary.
    const float3 delta = (fineBand * gFineDetailStrength + structureBand * gStructureStrength) * gDetailMotionScale;
    float3 enhanced = color + delta;

    // Hard clamp on how far luminance may move, applied as a ratio so hue and saturation
    // survive. The backstop that keeps a mis-tuned strength from blowing highlights or
    // crushing shadows rather than merely looking wrong.
    const float sourceLuminance = max(DpleLuminance(color), 1e-5f);
    const float enhancedLuminance = DpleLuminance(enhanced);
    if (enhancedLuminance > 1e-5f)
    {
        const float ratio = clamp(enhancedLuminance / sourceLuminance,
                                  1.0f - gMaxLuminanceChange, 1.0f + gMaxLuminanceChange);
        enhanced *= (sourceLuminance * ratio) / enhancedLuminance;
    }
    else
    {
        enhanced = color;
    }

    gDetailOut[id.xy] = float4(DpleSafeColor(enhanced), source.a);
}
