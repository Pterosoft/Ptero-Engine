// VolumetricFogInject.hlsl
// Per-froxel in-scattering. Writes sigma_s-weighted scattered radiance in RGB
// and the extinction coefficient sigma_t in A; VolumetricFogAccumulate then
// marches the froxel column.
//
// Every radiometric quantity here is the same one the deferred pass uses, with
// no pass-local multipliers: a light's falloff comes from LightShapes.hlsli,
// the phase function carries its own 1/4pi, and irradiance is converted to
// radiance by the phase or by 1/pi. That is what lets Density mean an
// extinction coefficient rather than a number tuned against the sun term.

#include "VolumetricFogCommon.hlsli"

Texture2D                 gSceneDepth     : register(t0);
StructuredBuffer<ProbeSH> gRadianceProbes : register(t1);
Texture2DArray            gPointShadowMaps : register(t2);

RWTexture3D<float4>       gLightingVolume : register(u0);

// Visibility of a froxel from one shadow-casting point light.
//
// Deliberately not SamplePointShadow() from the deferred pass: that one offsets
// along the surface normal and biases by the light's slope against it, and a
// point in a participating medium has no normal to do either with. The face
// selection and the normalised-distance comparison are the same, so a shaft
// lines up with the shadow on the wall behind it.
float SampleFogPointShadow(int lightIndex, float3 worldPos)
{
    if (gPointShadowLightCount <= 0)
        return 1.0f;
    if (gPointLights[lightIndex].CastShadows < 0.5f)
        return 1.0f;

    const int shadowIndex = (int)gPointLights[lightIndex].ShadowIndex;
    if (shadowIndex < 0 || shadowIndex >= gPointShadowLightCount)
        return 1.0f;

    const float3 toPoint = worldPos - gPointLights[lightIndex].Position;
    const float distanceToLight = length(toPoint);
    if (distanceToLight <= 1e-4f)
        return 1.0f;

    // Pick the cube face that holds the sample furthest from its own border, so
    // a froxel near a seam is resolved by the face that actually covers it.
    int faceIndex = -1;
    float2 uv = 0.0f.xx;
    float bestEdgeDistance = -1.0f;
    const int faceBaseIndex = shadowIndex * 6;

    [unroll]
    for (int candidateFace = 0; candidateFace < 6; ++candidateFace)
    {
        const float4 clipPos = mul(float4(worldPos, 1.0f), gPointShadowFaceViewProj[faceBaseIndex + candidateFace]);
        if (clipPos.w <= 1e-5f)
            continue;

        const float3 ndc = clipPos.xyz / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
            continue;

        const float2 candidateUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (any(candidateUv < 0.0f.xx) || any(candidateUv > 1.0f.xx))
            continue;

        const float2 edgeDistance2D = min(candidateUv, 1.0f.xx - candidateUv);
        const float edgeDistance = min(edgeDistance2D.x, edgeDistance2D.y);
        if (edgeDistance > bestEdgeDistance)
        {
            faceIndex = candidateFace;
            uv = candidateUv;
            bestEdgeDistance = edgeDistance;
        }
    }

    if (faceIndex < 0)
        return 1.0f;

    const float mapSize = max(gPointShadowMapSize, 1.0f);
    const float lightRadius = max(gPointLights[lightIndex].Radius, 1e-4f);
    const float currentDepth = distanceToLight / lightRadius;

    // One tap rather than the deferred pass's PCF kernel: this runs per froxel,
    // and the froxel grid is already an eighth of screen resolution and is read
    // back through a trilinear filter, so the kernel would cost far more than
    // the softening it buys.
    const int2 samplePixel = int2(saturate(uv) * (mapSize - 1.0f));
    const float storedDepth = gPointShadowMaps.Load(int4(samplePixel, faceBaseIndex + faceIndex, 0)).r;

    // No slope term is available without a normal, so lean on a slightly wider
    // constant bias than the surface path uses.
    const float depthBias = (gPointShadowBias / lightRadius) + (2.0f / mapSize);
    return ((currentDepth - depthBias) <= storedDepth) ? 1.0f : 0.0f;
}

// Indirect light reaching the froxel, as average radiance over the sphere.
//
// The probe grid is the only world-space irradiance field the engine keeps; the
// RTGI accumulation buffer is screen-space and has no value to give for a point
// the camera cannot see, which is why an interior lit entirely by bounce used
// to leave the medium black.
float3 SampleFogIndirect(float3 worldPos)
{
    const uint3 gridSize = uint3(gProbeGridX, gProbeGridY, gProbeGridZ);
    if (gGiIntensity <= 0.0f || !PteroProbeGridValid(gridSize, gProbeSpacing))
        return 0.0f.xxx;

    const PteroProbeGridTap tap = PteroProbeGridLookup(gridSize, gProbeOrigin, gProbeSpacing, worldPos);

    float3 ambient = 0.0f.xxx;
    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        ambient += tap.Weight[corner] * PteroEvaluateProbeAmbient(gRadianceProbes[tap.Index[corner]]);
    }
    return ambient * gGiIntensity;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFroxelWidth || dispatchThreadId.y >= gFroxelHeight || dispatchThreadId.z >= gDepthSlices)
        return;

    uint2 pixel = min(dispatchThreadId.xy * uint2(gFrameWidth, gFrameHeight) / uint2(gFroxelWidth, gFroxelHeight), uint2(gFrameWidth - 1, gFrameHeight - 1));
    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(gFroxelWidth, gFroxelHeight);
    float viewDepth = SliceToViewDepth((float)dispatchThreadId.z);

    float surfaceFade = 1.0f;
    float sceneDepth = gSceneDepth.Load(int3(pixel, 0)).r;
    if (sceneDepth < 1.0f)
    {
        float2 pixelUv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
        float3 surfaceWorldPos = ReconstructWorldPosition(pixelUv, sceneDepth);
        float surfaceViewDepth = length(surfaceWorldPos - gCameraPos);
        float fadeWidth = max(1.0f, surfaceViewDepth * 0.02f);
        surfaceFade = saturate((surfaceViewDepth - viewDepth) / fadeWidth);
    }

    float3 worldFar = ReconstructWorldPosition(uv, 1.0f);
    float3 rayDir = normalize(worldFar - gCameraPos);
    float3 worldPos = gCameraPos + rayDir * viewDepth;

    // sigma_t: how fast the medium removes light. sigma_s: how much of that is
    // scattered rather than absorbed. Splitting them is what gives grey haze
    // instead of the black absorption an albedo pinned to 1 produced.
    float sigmaT = max(gDensity, 0.0f) * ComputeHeightDensity(worldPos.z) * surfaceFade;
    float3 sigmaS = sigmaT * saturate(gScatteringAlbedo) * gFogColor;

    // ---- In-scattered radiance, before sigma_s -------------------------------
    // Sun. gSunColor is the directional irradiance the deferred pass shades
    // with, and the phase turns it into radiance toward the camera.
    float3 inScatter = gSunColor * HenyeyGreenstein(FogPhaseCosine(normalize(gSunDir), rayDir), gAnisotropy);

    // Sky. An ambient irradiance arrives from every direction at once, so the
    // phase function integrates to 1 and only the 1/pi conversion is left.
    inScatter += gSkyColor * (1.0f / 3.14159265f);

    // Bounce light from the radiance probe grid, already an average radiance.
    inScatter += SampleFogIndirect(worldPos);

    // Point, spot and rect lights.
    [loop]
    for (uint i = 0; i < gNumPointLights; ++i)
    {
        const PteroLightData light = gPointLights[i];

        // Identical to the deferred pass: the same nearest-point resolution for
        // a rect panel, the same squared cone ramp for a spot, the same
        // windowed inverse-square for the distance term. A light that lights
        // the wall this brightly now lights the air in front of it to match.
        const PteroResolvedLight resolved = PteroResolveLightShape(light, worldPos);

        const float3 toLight = resolved.Position - worldPos;
        const float distSq = dot(toLight, toLight);
        const float dist = sqrt(max(distSq, 1e-6f));

        const float normalizedDistance = saturate(dist / max(light.Radius, 1e-4f));
        float rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
        rangeMask *= rangeMask;
        if (rangeMask <= 0.0f)
            continue;

        const float falloffExponent = max(light.FalloffExponent, 0.001f);
        const float distanceFalloff = pow(PteroLightFalloffDistance(light, dist), -falloffExponent);
        const float falloff = rangeMask * distanceFalloff * resolved.ShapeMask;
        if (falloff <= 0.0f)
            continue;

        const float shadow = SampleFogPointShadow((int)i, worldPos);
        if (shadow <= 0.0f)
            continue;

        // The light travels from the emitter to this froxel.
        const float3 lightTravelDir = -toLight / dist;
        const float phase = HenyeyGreenstein(FogPhaseCosine(lightTravelDir, rayDir), gAnisotropy);

        inScatter += light.Color * (falloff * shadow * phase);
    }

    // Emission is radiance the medium produces itself, so it is not scaled by
    // sigma_s - only by how much medium is present.
    float3 emission = gEmissiveColor * gEmissiveIntensity * sigmaT;

    float3 scattered = sigmaS * inScatter + emission;
    gLightingVolume[dispatchThreadId] = float4(scattered, sigmaT);
}
