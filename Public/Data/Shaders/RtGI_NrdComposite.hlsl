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
//   param 2 : SRV  t1  normalDepth G-buffer (xy: oct normal, z: hw depth)
//   param 3 : UAV  u0  Final GI output texture (RGBA16F)

#include "RtGI_Common.hlsli"

#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

Texture2D<float4>   t_DenoisedDiff : register(t0);   // NRD OUT_DIFF_RADIANCE_HITDIST
Texture2D<float4>   t_NormalDepth  : register(t1);   // G-buffer normal + depth
RWTexture2D<float4> u_Output       : register(u0);   // final GI output

float3 SampleDenoisedRadiance(uint2 pixel)
{
    float4 packed = t_DenoisedDiff.Load(int3(pixel, 0));
    float4 unpacked = RELAX_BackEnd_UnpackRadiance(packed);
    return unpacked.rgb;
}

float3 DecodeGBufferNormal(float2 encoded)
{
    const float2 octN = encoded * 2.0f - 1.0f;
    const float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    const float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));
}

bool IsSkyDepth(float depth)
{
    return depth <= 0.0f || depth >= 1.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // A trace pixel. At reduced resolution u0 is the trace-grid result that
    // RtGI_Upsample.hlsl fills the frame from, rather than the frame itself.
    const uint2 pixel = DTid.xy;
    const uint2 traceSize = TraceSize();
    if (any(pixel >= traceSize))
        return;

    // RELAX_BackEnd_UnpackRadiance returns float4 (rgb=radiance, a=hit dist / history length).
    float3 giRadiance = SampleDenoisedRadiance(pixel);

    // Not on a reduced grid: its neighbours stand several frame pixels apart, often on
    // another surface, and sharpening against them made outliers the upsample then
    // spread over whole blocks - sparkles up close.
    //
    // Only against neighbours on the same surface. Across a silhouette the unsharp mask
    // pushed the background away from the object in front of it - the floor beside a
    // shaded table leg came out brighter than the floor around it, a glowing outline
    // around all geometry. Sky neighbours carry no GI at all and did the same at the
    // horizon.
    const float4 nd = t_NormalDepth.Load(int3(pixel, 0));
    if (g_NrdSharpenAmount > 0.0001f && g_TraceScale == 1u && !IsSkyDepth(nd.z))
    {
        const float3 normal   = DecodeGBufferNormal(nd.xy);
        const float  distance = length(ReconstructWorldPos(pixel, nd.z) - g_CameraPos);
        // A few percent of the distance, as in RtGI_Upsample.hlsl.
        const float  depthTolerance = max(distance * 0.03f, 0.01f);

        const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float3 neighborSum = 0.0f.xxx;
        float  weightSum   = 0.0f;
        [unroll]
        for (uint i = 0; i < 4u; ++i)
        {
            const uint2 tap = uint2(clamp(int2(pixel) + offsets[i], int2(0, 0), int2(traceSize) - 1));
            const float4 tapNd = t_NormalDepth.Load(int3(tap, 0));
            if (IsSkyDepth(tapNd.z))
                continue;
            const float tapDistance = length(ReconstructWorldPos(tap, tapNd.z) - g_CameraPos);
            const float facing = saturate(dot(normal, DecodeGBufferNormal(tapNd.xy)));
            const float weight = exp(-abs(tapDistance - distance) / depthTolerance) * facing * facing;
            neighborSum += weight * SampleDenoisedRadiance(tap);
            weightSum   += weight;
        }

        if (weightSum > 1e-3f)
        {
            const float3 detail = giRadiance - neighborSum / weightSum;
            // Fewer matching neighbours, less sharpening: at an edge the few that match
            // all sit on one side.
            giRadiance = max(giRadiance + detail * (g_NrdSharpenAmount * weightSum * 0.25f), 0.0f.xxx);
        }
    }

    u_Output[pixel] = float4(giRadiance, 1.0f);
}
