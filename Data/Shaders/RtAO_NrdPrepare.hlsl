// RtAO_NrdPrepare.hlsl  –  cs_6_5
// Prepares NRD RELAX_DIFFUSE inputs from the raw AO result.

#include "RtAO_Common.hlsli"

#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

Texture2D<float4>  t_NormalDepth        : register(t0);
Texture2D<float>   t_SceneDepth         : register(t1);
Texture2D<float2>  t_RawAO              : register(t2);

RWTexture2D<float4> u_DiffRadianceHitDist : register(u0);
RWTexture2D<uint4>  u_NormalRoughness     : register(u1);
RWTexture2D<float>  u_ViewZ               : register(u2);
RWTexture2D<float2> u_MotionVectors       : register(u3);

float ComputeViewZ(uint2 pixel, float hwDepth)
{
    float3 worldPos = ReconstructWorldPos(pixel, hwDepth);
    float3 viewPos = mul(float4(worldPos, 1.0f), g_WorldToView).xyz;
    return max(1e-6f, abs(viewPos.z));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float4 normalDepthSample = t_NormalDepth.Load(int3(pixel, 0));
    const float hwDepth = t_SceneDepth.Load(int3(pixel, 0)).r;
    const bool isSky = (hwDepth <= 0.0f || hwDepth >= 1.0f);

    const float viewZ = isSky ? 1e6f : ComputeViewZ(pixel, hwDepth);
    u_ViewZ[pixel] = viewZ;

    float3 worldNormal = DecodeOctNormal(normalDepthSample.xy);
    if (isSky)
        worldNormal = float3(0, 0, 1);

    float4 packedNR = NRD_FrontEnd_PackNormalAndRoughness(worldNormal, 1.0f, 0);
    uint r10 = (uint)(saturate(packedNR.x) * 1023.0f + 0.5f) & 0x3FFu;
    uint g10 = (uint)(saturate(packedNR.y) * 1023.0f + 0.5f) & 0x3FFu;
    uint b10 = (uint)(saturate(packedNR.z) * 1023.0f + 0.5f) & 0x3FFu;
    uint a2  = (uint)(saturate(packedNR.w) * 3.0f + 0.5f) & 0x3u;
    u_NormalRoughness[pixel] = uint4(r10, g10, b10, a2);

    const float2 rawAoSample = t_RawAO.Load(int3(pixel, 0));
    const float ao = isSky ? 1.0f : saturate(rawAoSample.x);
    const float occlusion = 1.0f - ao;
    const float hitDist = isSky ? 0.0f : max(rawAoSample.y, 1e-4f);
    float4 packedDiff = RELAX_FrontEnd_PackRadianceAndHitDist(float3(occlusion, occlusion, occlusion), hitDist, true);
    u_DiffRadianceHitDist[pixel] = packedDiff;

    float2 motionVec = float2(0, 0);
    if (!isSky)
    {
        float3 worldPos = ReconstructWorldPos(pixel, hwDepth);
        float2 prevUV = ReprojectUV(worldPos);
        float2 currUV = ProjectCurrentUV(worldPos);
        if (all(prevUV >= 0.0f.xx) && all(prevUV <= 1.0f.xx)
            && all(currUV >= 0.0f.xx) && all(currUV <= 1.0f.xx))
        {
            motionVec = prevUV - currUV;
        }
    }
    u_MotionVectors[pixel] = motionVec;
}
