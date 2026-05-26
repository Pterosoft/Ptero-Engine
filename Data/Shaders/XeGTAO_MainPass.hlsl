// XeGTAO_MainPass.hlsl  –  cs_6_0
// Pass 1: Main XeGTAO occlusion pass.
// Reads MIP 0 of the pre-filtered viewspace depth and viewspace normals,
// outputs packed AO+BentNormal term (u_WorkingAOTerm) and edge data (u_WorkingEdges).

#include "XeGTAO_Common.hlsli"
#include "XeGTAO.hlsli"

// Quality-level defines – injected via compiler defines at dispatch time:
//   XE_GTAO_QUALITY_LEVEL   0=low(1 slice, 2 steps), 1=medium(2,4), 2=high(3,3), 3=ultra(9,3)

Texture2D<float>        t_ViewspaceDepth : register(t0);  // MIP 0 of pre-filtered viewspace depth (lpfloat)
Texture2D<float4>       t_GBufferNormal  : register(t1);  // GBuffer RT1: oct-normal (RG), copied depth (B)
SamplerState            s_PointClamp     : register(s0);

// Bound as viewspace normal; computed from world-space oct-normal + view matrix in this shader
cbuffer ViewCB : register(b1)
{
    float4x4 g_WorldToView;  // row-major world-to-view matrix
}

RWTexture2D<uint>       u_WorkingAOTerm  : register(u0);
RWTexture2D<float>      u_WorkingEdges   : register(u1);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID >= uint2(g_GTAOConsts.ViewportSize)))
        return;

    // Decode world-space normal from GBuffer and transform to viewspace
    float4 normalSample = t_GBufferNormal[dispatchThreadID];
    float3 wsNormal = DecodeOctNormalWS(normalSample.rg);
    float3 vsNormal = normalize(mul(wsNormal, (float3x3)g_WorldToView));
    // Engine uses LH (+Z forward into screen); XeGTAO expects viewspace normal with
    // Z pointing AWAY from the camera (-Z forward), so negate Z.
    // REMOVED: vsNormal.z = -vsNormal.z was incorrectly flipping the normal,
    // causing it to face away from the view vector and producing max occlusion (black).

#if XE_GTAO_QUALITY_LEVEL <= 0
    const lpfloat sliceCount    = 1;
    const lpfloat stepsPerSlice = 2;
#elif XE_GTAO_QUALITY_LEVEL == 1
    const lpfloat sliceCount    = 2;
    const lpfloat stepsPerSlice = 2;
#elif XE_GTAO_QUALITY_LEVEL == 2
    const lpfloat sliceCount    = 3;
    const lpfloat stepsPerSlice = 3;
#else // ultra
    const lpfloat sliceCount    = 9;
    const lpfloat stepsPerSlice = 3;
#endif

    // Per-pixel noise from Hilbert curve + frame index (for TAA stability)
    uint hilbertIndex = HilbertIndex(dispatchThreadID.x % XE_HILBERT_WIDTH, dispatchThreadID.y % XE_HILBERT_WIDTH);
    uint noiseIndex   = (g_GTAOConsts.NoiseIndex + hilbertIndex) % XE_HILBERT_AREA;
    // R2 low-discrepancy sequence for noise
    const float2 localNoise = float2(
        frac(0.5f + noiseIndex * 0.7548776662f),
        frac(0.5f + noiseIndex * 0.5698402910f));

    XeGTAO_MainPass(dispatchThreadID, sliceCount, stepsPerSlice, localNoise,
        (lpfloat3)vsNormal, g_GTAOConsts,
        t_ViewspaceDepth, s_PointClamp,
        u_WorkingAOTerm, u_WorkingEdges);
}
