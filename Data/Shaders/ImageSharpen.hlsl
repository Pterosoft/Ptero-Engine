// Standalone image sharpening pass. This deliberately runs outside TAA so the
// same sharpening control works with TAA, SMAA, DLSS, or raw scene color.

cbuffer ImageSharpenConstants : register(b0)
{
    float Strength;
    uint  FrameWidth;
    uint  FrameHeight;
    float _Pad0;
}

Texture2D<float4>   gInput  : register(t0);
RWTexture2D<float4> gOutput : register(u0);

float3 SanitizeColor(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
        return float3(0.0f, 0.0f, 0.0f);
    return max(color, 0.0f.xxx);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchId.xy;
    if (coord.x >= FrameWidth || coord.y >= FrameHeight)
        return;

    const int2 maxCoord = int2((int)FrameWidth - 1, (int)FrameHeight - 1);
    const float4 center = gInput[coord];

    float3 blurAccum = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 sampleCoord = clamp((int2)coord + int2(x, y), int2(0, 0), maxCoord);
            blurAccum += gInput[sampleCoord].rgb;
        }
    }

    const float3 blur = blurAccum / 9.0f;
    const float3 sharpened = center.rgb + Strength * (center.rgb - blur);
    gOutput[coord] = float4(SanitizeColor(sharpened), center.a);
}
