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
Texture2D                 gVsmPool         : register(t3); // virtual shadow map page pool
StructuredBuffer<uint>    gVsmPageTable    : register(t4); // virtual shadow map pages -> pool tiles
Texture2D<float4>         gScreenGi        : register(t5); // RTGI / voxel GI diffuse output (irradiance / pi)
Texture3D<float4>         gGiVolume        : register(t6); // world-space GI built from it (VolumetricFogGiVolume.hlsl)
SamplerState              gGiVolumeSampler : register(s0); // trilinear, wrapping: the volume is toroidal

// A froxel spans several screen pixels, so its shadow pages can be that much coarser.
static const float kFogVsmFootprintScale = 8.0f;

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
    if (gClusterLights[lightIndex].CastShadows < 0.5f)
        return 1.0f;

    // Virtual shadow map: the shadow index is the light's slot in it. Air has no
    // normal, and whatever pages surfaces asked for are the ones resident; the lookup
    // falls back to coarser mips (every light keeps its coarsest one) where needed.
    [branch]
    if (gFogVsm.LocalEnabled != 0u)
    {
        const int slot = (int)gClusterLights[lightIndex].ShadowIndex;
        return slot < 0 ? 1.0f
            : PteroVsmLocalVisibility(gFogVsm, gVsmPageTable, gVsmPool, (uint)slot, worldPos, 0.0f.xxx,
                                      gCameraPos, kFogVsmFootprintScale);
    }

    if (gPointShadowLightCount <= 0)
        return 1.0f;

    const int shadowIndex = (int)gClusterLights[lightIndex].ShadowIndex;
    if (shadowIndex < 0 || shadowIndex >= gPointShadowLightCount)
        return 1.0f;

    const float3 toPoint = worldPos - gClusterLights[lightIndex].Position;
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
    const float lightRadius = max(gClusterLights[lightIndex].Radius, 1e-4f);
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

// Indirect light from the screen-space GI - RTGI's or voxel GI's diffuse output -
// read at the surfaces behind the froxel: the bounce light arriving there, as an
// average radiance, stands in for what the air in front of them receives. Four
// pixels across the froxel's footprint, skipping sky, so one thin object does not
// decide a whole froxel. It knows nothing of light the camera cannot see, which
// is the price of following the GI the level runs instead of keeping a probe grid
// alive for the fog alone.
float3 SampleScreenGiIndirect(uint2 froxel)
{
    const float2 froxelToFrame = float2(gFrameWidth, gFrameHeight) / float2(gFroxelWidth, gFroxelHeight);
    const uint2 frameMax = uint2(gFrameWidth, gFrameHeight) - 1u;
    float3 sum = 0.0f.xxx;
    float count = 0.0f;
    [unroll]
    for (uint i = 0; i < 4u; ++i)
    {
        const float2 sub = float2(i & 1u, i >> 1u) * 0.5f + 0.25f;
        const uint2 pixel = min(uint2((float2(froxel) + sub) * froxelToFrame), frameMax);
        const float depth = gSceneDepth.Load(int3(pixel, 0)).r;
        if (depth <= 0.0f || depth >= 1.0f)
            continue;
        const float3 gi = gScreenGi.Load(int3(pixel, 0)).rgb;
        if (any(isnan(gi)) || any(isinf(gi)))
            continue;
        sum += max(gi, 0.0f.xxx);
        count += 1.0f;
    }
    return count > 0.0f ? sum / count * gGiIntensity : 0.0f.xxx;
}

// The world-space GI volume at a point: radiance in rgb, and in a how far to trust
// it - its confidence, faded out over the last cells of the window.
float4 SampleGiVolume(float3 worldPos)
{
    if (gGiVolumeOrigin.w == 0)
        return 0.0f.xxxx;

    const float3 dims = float3(GiVolumeDims());
    const float3 cellPos = worldPos * gGiVolumeInvCellSize;
    const float3 local = cellPos - float3(gGiVolumeOrigin.xyz);
    const float3 edgeDistance = min(local, dims - local);
    const float edgeFade = saturate((min(edgeDistance.x, min(edgeDistance.y, edgeDistance.z)) - 1.0f) * 0.5f);
    if (edgeFade <= 0.0f)
        return 0.0f.xxxx;

    // Texel i spans cell coordinates [i, i + 1) of the world cells it holds, so the
    // continuous cell position over the dimensions is the wrapped texture coordinate.
    const float4 premultiplied = gGiVolume.SampleLevel(gGiVolumeSampler, cellPos / dims, 0.0f);
    const float confidence = premultiplied.a;
    if (confidence <= 1e-4f)
        return 0.0f.xxxx;
    return float4(premultiplied.rgb / confidence * gGiIntensity, saturate(confidence * 3.0f) * edgeFade);
}

// Indirect light reaching the froxel, as average radiance over the sphere, from
// whichever GI the level runs: the radiance probe grid when the probes are the
// GI, otherwise the world-space volume built from the screen-space GI, with the
// screen-space GI itself where the volume has nothing yet or does not reach.
float3 SampleFogIndirect(float3 worldPos, uint2 froxel)
{
    if (gGiIntensity <= 0.0f)
        return 0.0f.xxx;
    if (gScreenGiEnabled != 0u)
    {
        const float4 volume = SampleGiVolume(worldPos);
        if (volume.a >= 0.999f)
            return volume.rgb;
        return lerp(SampleScreenGiIndirect(froxel), volume.rgb, volume.a);
    }
    if (!PteroProbeFieldValid(gProbeField))
        return 0.0f.xxx;

    // Finest cascade first, handing over outward; see RadianceProbeCommon.
    float3 result = 0.0f.xxx;
    float remaining = 1.0f;
    [loop]
    for (uint cascade = 0; cascade < gProbeField.CascadeCount; ++cascade)
    {
        const PteroProbeGridTap tap = PteroProbeGridLookup(gProbeField, cascade, worldPos);
        if (tap.EdgeFade <= 0.0f)
            continue;

        // A froxel has no surface to be behind, but it can sit next to a probe
        // buried in a wall; that one drops out and the rest are renormalised.
        float3 weighted = 0.0f.xxx;
        float3 plain = 0.0f.xxx;
        float weightSum = 0.0f;
        [unroll]
        for (uint corner = 0; corner < 8; ++corner)
        {
            const ProbeSH sh = gRadianceProbes[tap.Index[corner]];
            const float3 ambient = PteroEvaluateProbeAmbient(sh);
            const float w = tap.Weight[corner] * PteroProbeValidity(sh);
            weighted += w * ambient;
            weightSum += w;
            plain += tap.Weight[corner] * ambient;
        }
        const float3 ambient = (weightSum > 1e-4f) ? (weighted / weightSum) : plain;

        result += remaining * tap.EdgeFade * ambient;
        remaining *= 1.0f - tap.EdgeFade;
        if (remaining <= PTERO_PROBE_CASCADE_EPSILON)
            break;
    }
    return result * gGiIntensity;
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

    // Bounce light from the running GI, already an average radiance.
    inScatter += SampleFogIndirect(worldPos, dispatchThreadId.xy);

    // Point, spot and rect lights: the ones this froxel's cluster lists, among them the
    // ones the artist let into the medium (a fog scale above zero), each lit at its
    // fog intensity.
    const uint lightCluster = PteroViewClusterBase(worldPos);
    [loop]
    for (uint lightWord = 0; lightWord < gLightClusters.WordCount; ++lightWord)
    {
      uint lightBits = PteroViewClusterWord(lightCluster, lightWord);
      [loop]
      while (lightBits != 0u)
      {
        const uint i = lightWord * 32u + firstbitlow(lightBits);
        lightBits &= lightBits - 1u;
        const float fogScale = gClusterLightExtras[i].x;
        if (fogScale <= 0.0f)
            continue;
        PteroLightData light = gClusterLights[i];
        light.Color *= fogScale;

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
    }

    // Emission is radiance the medium produces itself, so it is not scaled by
    // sigma_s - only by how much medium is present.
    float3 emission = gEmissiveColor * gEmissiveIntensity * sigmaT;

    float3 scattered = sigmaS * inScatter + emission;
    gLightingVolume[dispatchThreadId] = float4(scattered, sigmaT);
}
