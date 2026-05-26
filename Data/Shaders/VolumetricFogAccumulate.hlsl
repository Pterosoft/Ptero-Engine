#include "VolumetricFogCommon.hlsli"

Texture3D<float4>    gLightingVolume : register(t0);
RWTexture3D<float4>  gIntegratedFog  : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFroxelWidth || dispatchThreadId.y >= gFroxelHeight)
        return;

    float3 scattering = 0.0f.xxx;
    float transmittance = 1.0f;
    float previousDepth = gStartDistance;

    [loop]
    for (uint z = 0; z < gDepthSlices; ++z)
    {
        float depth = SliceToViewDepth((float)z);
        float stepLength = max(depth - previousDepth, 0.0001f);
        previousDepth = depth;

        float4 sampleValue = gLightingVolume[uint3(dispatchThreadId.xy, z)];
        float density = max(sampleValue.a, 0.0f);
        float extinction = exp(-density * stepLength);
        scattering += transmittance * sampleValue.rgb * stepLength;
        transmittance *= extinction;

        gIntegratedFog[uint3(dispatchThreadId.xy, z)] = float4(scattering, transmittance);
    }
}
