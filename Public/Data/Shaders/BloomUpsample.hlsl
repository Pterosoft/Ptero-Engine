// BloomUpsample.hlsl
// Physical mip-chain bloom – upsample (scatter) pass.
// Uses a 3x3 tent filter scaled by g_Radius for a smooth scatter.

cbuffer BloomConstants : register(b0)
{
    float   g_Threshold;
    float   g_Knee;
    float   g_Intensity;
    float   g_Radius;
    uint    g_SrcWidth;
    uint    g_SrcHeight;
    uint    g_DstWidth;
    uint    g_DstHeight;
    int     g_MipLevel;
    int     g_MaxMips;
    float   g_Pad0;
    float   g_Pad1;
};

Texture2D<float4>   g_SrcTexture : register(t0);
RWTexture2D<float4> g_DstTexture : register(u0);
SamplerState        g_LinearClamp : register(s0);

// 3x3 tent filter upsample.
float3 TentUpsample(Texture2D<float4> src, SamplerState smp, float2 uv, float2 texelSize, float radius)
{
    float2 o = texelSize * radius;

    float3 a  = src.SampleLevel(smp, uv + float2(-o.x,  o.y), 0).rgb;
    float3 b  = src.SampleLevel(smp, uv + float2( 0.0f,  o.y), 0).rgb;
    float3 c  = src.SampleLevel(smp, uv + float2( o.x,  o.y), 0).rgb;

    float3 d  = src.SampleLevel(smp, uv + float2(-o.x,  0.0f), 0).rgb;
    float3 e  = src.SampleLevel(smp, uv + float2( 0.0f,  0.0f), 0).rgb;
    float3 f  = src.SampleLevel(smp, uv + float2( o.x,  0.0f), 0).rgb;

    float3 g2 = src.SampleLevel(smp, uv + float2(-o.x, -o.y), 0).rgb;
    float3 h  = src.SampleLevel(smp, uv + float2( 0.0f, -o.y), 0).rgb;
    float3 i  = src.SampleLevel(smp, uv + float2( o.x, -o.y), 0).rgb;

    // Tent weights: corners=1, edges=2, center=4.
    float3 result = (a + c + g2 + i) * 1.0f
                  + (b + d + f + h)  * 2.0f
                  + e                * 4.0f;
    return result * (1.0f / 16.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_DstWidth || dispatchId.y >= g_DstHeight)
        return;

    float2 uv = (float2(dispatchId.xy) + 0.5f) / float2(g_DstWidth, g_DstHeight);
    float2 srcTexelSize = 1.0f / float2(g_SrcWidth, g_SrcHeight);

    float3 upsampled = TentUpsample(g_SrcTexture, g_LinearClamp, uv, srcTexelSize, g_Radius);

    // Additive accumulation into the destination mip.
    float3 existing = g_DstTexture[dispatchId.xy].rgb;
    g_DstTexture[dispatchId.xy] = float4(existing + upsampled, 1.0f);
}
