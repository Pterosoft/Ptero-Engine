// XeGTAO_PrefilterDepths.hlsl  –  cs_6_0
// Pass 0: Prefilter the scene NDC depth into 5 MIP levels of viewspace linear depth.
// Dispatched with (width/16 + 1) x (height/16 + 1) thread groups, 8x8 threads each,
// processing a 16x16 screen tile per group.

#include "XeGTAO_Common.hlsli"
#include "XeGTAO.hlsli"

Texture2D<float>    t_SourceDepth : register(t0);  // scene NDC depth (R32_FLOAT or R32_TYPELESS SRV)
SamplerState        s_PointClamp  : register(s0);

RWTexture2D<float>  u_OutDepth0   : register(u0);  // MIP 0
RWTexture2D<float>  u_OutDepth1   : register(u1);  // MIP 1
RWTexture2D<float>  u_OutDepth2   : register(u2);  // MIP 2
RWTexture2D<float>  u_OutDepth3   : register(u3);  // MIP 3
RWTexture2D<float>  u_OutDepth4   : register(u4);  // MIP 4

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, g_GTAOConsts,
        t_SourceDepth, s_PointClamp,
        u_OutDepth0, u_OutDepth1, u_OutDepth2, u_OutDepth3, u_OutDepth4);
}
