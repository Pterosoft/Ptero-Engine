// Dple_Downsample.hlsl
// One 2x box reduction for DPLE's detail enhancement. The chain is built by successive
// halvings rather than a wide blur at full resolution: the recombination needs 1/4 and 1/16
// base layers, and reaching them this way costs a third of the bandwidth of a separable
// blur wide enough to match.

#include "Dple_Common.hlsli"

Texture2D<float4>   gSource   : register(t4);
RWTexture2D<float4> gDestOut  : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gDestSize))
        return;

    // One bilinear tap at each 2x2 quad centre: four texels for the price of one fetch.
    // Clamped to the source's valid region, since a level's texture may be larger than
    // the part of it that holds this view.
    float2 sourcePixel = (float2(id.xy) + 0.5f) * 2.0f;
    sourcePixel = min(sourcePixel, gSourceSize - 0.5f);

    const float3 color = gSource.SampleLevel(gLinearClamp, sourcePixel * gSourceInvSize, 0.0f).rgb;
    gDestOut[id.xy] = float4(DpleSafeColor(color), 0.0f);
}
