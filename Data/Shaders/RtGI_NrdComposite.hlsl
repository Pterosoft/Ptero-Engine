// RtGI_NrdComposite.hlsl  –  cs_6_5
// Final pass after NRD RELAX_DIFFUSE denoising.
// Reads the denoised OUT_DIFF_RADIANCE_HITDIST texture from NRD and unpacks
// the diffuse radiance using RELAX_BackEnd_UnpackRadiance(), then writes
// the result into the final GI output texture consumed by the deferred
// lighting pass.
//
// Root signature binding (same 9-param layout):
//   param 0 : CBV  b0  RtGIConstants
//   param 1 : SRV  t0  NRD OUT_DIFF_RADIANCE_HITDIST (R11G11B10F, read as RGBA32F)
//   param 3 : UAV  u0  Final GI output texture (RGBA16F)

#include "RtGI_Common.hlsli"

#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

Texture2D<float4>   t_DenoisedDiff : register(t0);   // NRD OUT_DIFF_RADIANCE_HITDIST
RWTexture2D<float4> u_Output       : register(u0);   // final GI output

float3 SampleDenoisedRadiance(uint2 pixel)
{
    float4 packed = t_DenoisedDiff.Load(int3(pixel, 0));
    float4 unpacked = RELAX_BackEnd_UnpackRadiance(packed);
    return unpacked.rgb;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    // RELAX_BackEnd_UnpackRadiance returns float4 (rgb=radiance, a=hit dist / history length).
    float3 giRadiance = SampleDenoisedRadiance(pixel);

    if (g_NrdSharpenAmount > 0.0001f)
    {
        const uint2 leftPixel  = uint2((pixel.x > 0) ? (pixel.x - 1) : pixel.x, pixel.y);
        const uint2 rightPixel = uint2(min(pixel.x + 1, g_FrameWidth - 1), pixel.y);
        const uint2 upPixel    = uint2(pixel.x, (pixel.y > 0) ? (pixel.y - 1) : pixel.y);
        const uint2 downPixel  = uint2(pixel.x, min(pixel.y + 1, g_FrameHeight - 1));

        const float3 leftRadiance  = SampleDenoisedRadiance(leftPixel);
        const float3 rightRadiance = SampleDenoisedRadiance(rightPixel);
        const float3 upRadiance    = SampleDenoisedRadiance(upPixel);
        const float3 downRadiance  = SampleDenoisedRadiance(downPixel);

        const float3 neighborAverage = 0.25f * (leftRadiance + rightRadiance + upRadiance + downRadiance);
        const float3 detail = giRadiance - neighborAverage;
        giRadiance = max(giRadiance + detail * g_NrdSharpenAmount, 0.0f.xxx);
    }

    u_Output[pixel] = float4(giRadiance, 1.0f);
}
