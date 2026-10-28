// RtAO_NrdComposite.hlsl  –  cs_6_5
// Converts the denoised NRD diffuse signal back into a scalar AO term.

#include "RtAO_Common.hlsli"

#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

Texture2D<float4> t_DenoisedDiff : register(t0);
RWTexture2D<float> u_Output      : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    float4 packed = t_DenoisedDiff.Load(int3(pixel, 0));
    float3 denoisedOcclusion = RELAX_BackEnd_UnpackRadiance(packed).rgb;
    float occlusion = saturate((denoisedOcclusion.r + denoisedOcclusion.g + denoisedOcclusion.b) * (1.0f / 3.0f));
    u_Output[pixel] = 1.0f - occlusion;
}
