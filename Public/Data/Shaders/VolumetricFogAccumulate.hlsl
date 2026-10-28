// VolumetricFogAccumulate.hlsl
// Marches each froxel column front to back, turning per-froxel scattering and
// extinction into the running (scattering, transmittance) pair the lighting
// pass composites with.

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
        float sigmaT = max(sampleValue.a, 0.0f);
        float sliceTransmittance = exp(-sigmaT * stepLength);

        // Analytic integral of scattered radiance over the slice, attenuated by
        // the medium inside the slice itself. The plain rectangle rule this
        // replaces - scattering * stepLength - has no upper bound, so a dense
        // slice or a long far slice kept adding light that could never have
        // escaped it, and raising Density brightened the fog instead of
        // thickening it.
        float3 sliceScattering = (sigmaT > 1e-6f)
            ? (sampleValue.rgb / sigmaT) * (1.0f - sliceTransmittance)
            : sampleValue.rgb * stepLength;

        scattering += transmittance * sliceScattering;
        transmittance *= sliceTransmittance;

        gIntegratedFog[uint3(dispatchThreadId.xy, z)] = float4(scattering, transmittance);
    }
}
