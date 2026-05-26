#include "VolumetricFogCommon.hlsli"

Texture2D            gSceneDepth : register(t0);
RaytracingAccelerationStructure gSceneTlas : register(t1);
RWTexture3D<float4>  gLightingVolume : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFroxelWidth || dispatchThreadId.y >= gFroxelHeight || dispatchThreadId.z >= gDepthSlices)
        return;

    uint2 pixel = min(dispatchThreadId.xy * uint2(gFrameWidth, gFrameHeight) / uint2(gFroxelWidth, gFroxelHeight), uint2(gFrameWidth - 1, gFrameHeight - 1));
    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(gFroxelWidth, gFroxelHeight);
    float viewDepth = SliceToViewDepth((float)dispatchThreadId.z);

    float sceneDepth = gSceneDepth.Load(int3(pixel, 0)).r;
    if (sceneDepth < 1.0f)
    {
        float2 pixelUv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
        float3 surfaceWorldPos = ReconstructWorldPosition(pixelUv, sceneDepth);
        float surfaceViewDepth = length(surfaceWorldPos - gCameraPos);
        if (viewDepth > surfaceViewDepth)
        {
            gLightingVolume[dispatchThreadId] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            return;
        }
    }

    float3 worldFar = ReconstructWorldPosition(uv, 1.0f);
    float3 rayDir = normalize(worldFar - gCameraPos);
    float3 worldPos = gCameraPos + rayDir * viewDepth;

    float density = (gDensity * 0.2f) * ComputeHeightDensity(worldPos.z);

    float cosTheta = dot(rayDir, normalize(gSunDir));
    float phase = HenyeyGreenstein(cosTheta, gAnisotropy);
    float3 ambientScatter = gSkyColor * 0.12f;
    float3 sunScatter = gSunColor * (phase * 8.0f);
    float3 pointScatter = 0.0f.xxx;
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        float active = (i < gNumPointLights) ? 1.0f : 0.0f;
        float3 toLight = gPointLights[i].Position - worldPos;
        float distSq = dot(toLight, toLight);
        float normDistSq = saturate(distSq * gPointLights[i].InvRadiusSq);
        float falloff = (1.0f - normDistSq) * (1.0f - normDistSq);
        float3 lightToSample = normalize(worldPos - gPointLights[i].Position);
        float pointCosTheta = dot(lightToSample, -rayDir);
        float pointPhase = HenyeyGreenstein(pointCosTheta, gAnisotropy);
        pointScatter += gPointLights[i].Color * (pointPhase * 4.0f) * falloff * active;
    }
    float3 emissiveScatter = gEmissiveColor * gEmissiveIntensity;
    float3 scattered = (gFogColor * (ambientScatter + sunScatter + pointScatter) + emissiveScatter) * density;
    gLightingVolume[dispatchThreadId] = float4(scattered, density);
}
