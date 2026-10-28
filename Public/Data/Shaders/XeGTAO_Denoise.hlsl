// XeGTAO_Denoise.hlsl  –  cs_6_0
// Pass 2/3: Spatial denoiser for XeGTAO.
// Dispatched with ceil(width/2) x height thread groups of 8x8.
// Each thread outputs two horizontally-adjacent pixels.

#include "XeGTAO_Common.hlsli"
#include "XeGTAO.hlsli"

// XE_GTAO_DENOISE_LAST is set via define for the final denoise pass
Texture2D<uint>      t_SourceAOTerm  : register(t0);
Texture2D<float>     t_SourceEdges   : register(t1);
SamplerState         s_PointClamp    : register(s0);

RWTexture2D<uint>    u_OutputAOTerm  : register(u0);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    t_SourceAOTerm.GetDimensions(width, height);

    const uint2 pixCoordBase = uint2(dispatchThreadID.x * 2, dispatchThreadID.y);
    if (pixCoordBase.x >= width || pixCoordBase.y >= height)
        return;

#ifdef XE_GTAO_DENOISE_LAST
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts,
        t_SourceAOTerm, t_SourceEdges, s_PointClamp,
        u_OutputAOTerm, true);
#else
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts,
        t_SourceAOTerm, t_SourceEdges, s_PointClamp,
        u_OutputAOTerm, false);
#endif
}
