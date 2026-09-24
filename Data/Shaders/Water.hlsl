// Water.hlsl
// ----------
// Procedural water (WaterComponent), ported from tuxalin/water-shader
// (https://github.com/tuxalin/water-shader, MIT licence; original source kept
// in Source/SDKs/water-shader-master).  The shading model is the original's:
//
//   * displacement: two Gerstner + two sine waves modulated by simplex noise,
//     plus a tiling height map (GPU Gems ch.1)
//   * normals from four scrolling samples of a tiling normal map
//   * colour extinction by water depth (Wojciech Toman, "Rendering Water as a
//     Post-process Effect")
//   * Fresnel reflection/refraction, specular sun radiance, shore and
//     high-wave foam
//
// Engine adaptations (the parts the original leaves to its host):
//   * World is Z-up (the original is Y-up): horizontal is XY, height is Z.
//   * The original is driven by Unity's _Time.x, which is seconds / 20; gTime
//     here is seconds, converted below so the material values keep their
//     meaning.
//   * The original's compile-time switches (USE_DISPLACEMENT, USE_FOAM,
//     USE_FILTERING, BLINN_PHONG) are runtime flags in gFeatures.
//   * _ReflectionTexture: the original expects a planar reflection render.
//     The engine has none, so the same image is produced in screen space: a
//     mirror ray off the flat water plane is marched against the depth buffer
//     and the hit is looked up in the scene-colour copy (falling back to the
//     rendered sky, then to a sky gradient).  The original's distortion by the
//     wave normal is then applied to that lookup exactly as before.
//   * USE_MEAN_SKY_RADIANCE is not ported: the original implementation is an
//     unfinished placeholder (see its TODO) that needs a cube map the engine's
//     procedural sky does not provide.
//   * The scene is HDR, so the final saturate() is a max(0) instead, and the
//     material's flat colours (surface/shore/depth) are lit by the current
//     sun + sky so the water follows time of day instead of glowing at night.
//   * Occlusion against opaque geometry is done here against the scene depth
//     (the pass depth-tests only against its own private depth buffer).
//
// Root signature:
//   b0 VS+PS - camera / transform / sun / screen    t0 PS    - lit scene copy
//   b1 VS+PS - water material                       t1 PS    - opaque depth
//   t2 PS    - normal map     t3 VS - height map    t4 PS    - foam  t5 PS - shore foam
//   s0 linear-clamp, s1 point-clamp, s2 anisotropic-wrap

#include "Water/Noise.hlsli"
#include "Water/Bicubic.hlsli"
#include "Water/Normals.hlsli"
#include "Water/Waves.hlsli"
#include "Water/Displacement.hlsli"
#include "Water/Radiance.hlsli"
#include "Water/Depth.hlsli"
#include "Water/Foam.hlsli"

cbuffer WaterFrame : register(b0)
{
    float4x4 gViewProj;
    float4x4 gModel;
    float4x4 gInvViewProj;

    float3   gCameraPos;    float gTime;          // seconds
    float3   gSunDirection; float gScreenWidth;   // points FROM sun TOWARD scene
    float3   gSunColor;     float gScreenHeight;
    float3   gSkyColor;     int   gDebugMode;
};

// Field names follow the original's uniforms (_SurfaceColor -> gSurfaceColor).
cbuffer WaterMaterial : register(b1)
{
    float4 gSurfaceColor;          // rgb
    float4 gShoreColor;            // rgb
    float4 gDepthColor;            // rgb
    // Horizontal extinction of the RGB channels, in world units.
    float4 gHorizontalExtinction;  // xyz
    // Displacement amplitude of multiple waves, x = smallest waves, w = largest waves
    float4 gWaveAmplitude;
    // Intensity of multiple waves, affects the frequency of specific waves
    float4 gWavesIntensity;
    // Noise of multiple waves, x = smallest waves, w = largest waves
    float4 gWavesNoise;
    // x = noise for shore, y = noise for outer, z/w = speed of the shore/outer noise
    float4 gFoamNoise;
    // x = range for shore foam, y = range for near shore foam,
    // z = height above the water level where wave-crest foam starts
    float4 gFoamRanges;            // xyz
    // xy = specular intensity values, z = shininess exponential factor
    float4 gSpecularValues;        // xyz

    // Wind direction in world XY, amplitude (speed) encoded as its length
    float2 gWindDirection;
    // x = index of refraction constant, y = refraction intensity
    float2 gRefractionValues;

    float2 gFoamTiling;
    float  gFoamSpeed;
    float  gFoamIntensity;

    float  gAmbientDensity;
    float  gDiffuseDensity;
    float  gHeightIntensity;
    float  gNormalIntensity;

    float  gTextureTiling;
    float  gWaveTiling;
    float  gWaveSteepness;
    float  gWaveAmplitudeFactor;

    float  gWaterClarity;
    float  gWaterTransparency;
    float  gShininess;
    float  gRefractionScale;

    float  gDistortion;
    float  gShoreFade;
    float  gWaterLevel;            // world Z of the undisplaced surface
    uint   gFeatures;              // WATER_FEATURE_* bits
};

#define WATER_FEATURE_DISPLACEMENT 1u
#define WATER_FEATURE_FOAM         2u
#define WATER_FEATURE_FILTERING    4u
#define WATER_FEATURE_BLINN_PHONG  8u

// Debug visualisation modes (WaterComponent::DebugMode).
#define WATER_DEBUG_OFF        0
#define WATER_DEBUG_NORMALS    1   // the original's DEBUG_NORMALS view
#define WATER_DEBUG_FRESNEL    2
#define WATER_DEBUG_DEPTH      3
#define WATER_DEBUG_REFRACTION 4
#define WATER_DEBUG_REFLECTION 5
#define WATER_DEBUG_SPECULAR   6
#define WATER_DEBUG_FOAM       7

Texture2D    gRefractionTexture : register(t0);
Texture2D    gDepthBuffer       : register(t1);
Texture2D    gNormalTexture     : register(t2);
Texture2D    gHeightTexture     : register(t3);
Texture2D    gFoamTexture       : register(t4);
Texture2D    gShoreTexture      : register(t5);
SamplerState gLinearClamp       : register(s0);
SamplerState gPointClamp        : register(s1);
SamplerState gWrap              : register(s2);

bool HasFeature(uint feature)
{
    return (gFeatures & feature) != 0u;
}

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VertexOutput
{
    float4 Position  : SV_Position;
    float2 UV        : TEXCOORD0;
    float3 Normal    : NORMAL;     // world normal
    float3 Tangent   : TANGENT;
    float3 Bitangent : BINORMAL;
    float3 WorldPos  : WORLDPOS;
    float  Timer     : TIMER;
    float4 Wind      : WIND;       // xy = normalized wind, zw = wind multiplied with timer
};

VertexOutput VSMain(VSInput v)
{
    VertexOutput o = (VertexOutput)0;

    float2 windDir = gWindDirection;
    float windSpeed = max(length(gWindDirection), 1e-4);
    windDir /= windSpeed;
    // Unity's _Time.x is seconds / 20.
    float timer = (gTime * 0.05) * windSpeed * 10;

    float3 worldPos = mul(float4(v.Position, 1.0), gModel).xyz;
    float3 normal = float3(0, 0, 1);

    if (HasFeature(WATER_FEATURE_DISPLACEMENT))
    {
        float cameraDistance = length(gCameraPos - worldPos);
        float2 noise = GetNoise(worldPos.xy, timer * windDir * 0.5);

        float3 tangent;
        float4 waveSettings = float4(windDir, gWaveSteepness, gWaveTiling);
        float4 waveAmplitudes = gWaveAmplitude * gWaveAmplitudeFactor;
        worldPos = ComputeDisplacement(worldPos, cameraDistance, noise, timer,
            waveSettings, waveAmplitudes, gWavesIntensity, gWavesNoise,
            normal, tangent);

        // add extra noise height from a heightmap
        // (the original multiplies by the float4 _WaveAmplitude, which
        // truncates to its x component)
        float heightIntensity = gHeightIntensity * (1.0 - cameraDistance / 100.0) * gWaveAmplitude.x;
        float2 texCoord = worldPos.xy * 0.05 * gTextureTiling;
        if (heightIntensity > 0.02)
        {
            float height = ComputeNoiseHeight(gHeightTexture, gWrap, gWavesIntensity, gWavesNoise,
                texCoord, noise, timer.xx);
            worldPos.z += height * heightIntensity;
        }

        o.Tangent = tangent;
        // cross(tangent, normal) rather than the original's cross(normal,
        // tangent): swapping Y and Z to go Z-up flips the basis' handedness.
        o.Bitangent = cross(tangent, normal);
    }
    float2 uv = worldPos.xy;

    o.Timer = timer;
    o.Wind.xy = windDir;
    o.Wind.zw = windDir * timer;

    o.UV = uv * 0.05 * gTextureTiling;
    o.Position = mul(float4(worldPos, 1.0), gViewProj);
    o.WorldPos = worldPos;
    o.Normal = normal;

    return o;
}

// ---------------------------------------------------------------------------
// Engine helpers: depth reconstruction and the screen-space stand-in for the
// original's planar _ReflectionTexture.
// ---------------------------------------------------------------------------

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

bool IsSkyDepth(float depth)
{
    return depth >= 0.9999f;
}

// Single-exit with every `out` written up front: the compiler's flow analysis
// flags multi-return functions with out parameters as potentially uninitialised.
bool WorldToScreenUV(float3 worldPos, out float2 uv)
{
    const float4 clip = mul(float4(worldPos, 1.0f), gViewProj);

    uv = float2(0.0f, 0.0f);
    bool onScreen = false;

    if (clip.w > 0.0001f)
    {
        const float2 ndc = clip.xy / clip.w;
        uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        onScreen = (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
    }

    return onScreen;
}

// Sky used where the reflection finds nothing on screen, from the engine's
// Hosek-Wilkie sky colour.
float3 SkyGradient(float3 dir)
{
    const float up = saturate(dir.z);
    const float lum = dot(gSkyColor, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 horizon = lerp(gSkyColor, lum.xxx * 1.05f + 0.01f, 0.7f);
    return lerp(horizon, gSkyColor, pow(up, 0.45f));
}

// March the mirror ray against the depth buffer.  Returns the screen UV of the
// reflected point, which is where a planar reflection render would show it.
bool TraceMirrorRay(float3 origin, float3 dir, float surfaceDist, out float2 hitUV)
{
    hitUV = float2(0.0f, 0.0f);
    bool hit = false;

    float stride = 0.5f;
    float3 p = origin;

    [loop]
    for (int i = 0; i < 24; ++i)
    {
        p += dir * stride;
        stride *= 1.3f;                     // geometric march: cheap + long reach

        float2 uv;
        if (!WorldToScreenUV(p, uv))
            break;

        const float sceneDepth = gDepthBuffer.SampleLevel(gPointClamp, uv, 0).r;
        if (!IsSkyDepth(sceneDepth))
        {
            const float sceneDist = length(ReconstructWorldPosition(uv, sceneDepth) - gCameraPos);
            const float rayDist   = length(p - gCameraPos);

            if (rayDist > sceneDist && (rayDist - sceneDist) < 1.5f * stride)
            {
                // Geometry closer than the water occludes it; it is not a reflection.
                if (sceneDist > surfaceDist)
                {
                    hitUV = uv;
                    hit = true;
                }
                break;
            }
        }
    }

    return hit;
}

// Stand-in for sampling the original's planar _ReflectionTexture at
// ndcPos + _Distortion * normal.
float3 SampleReflection(float3 surfacePos, float3 eyeDir, float3 normal, float surfaceDist, bool bicubic)
{
    const float3 mirrorDir = reflect(-eyeDir, float3(0, 0, 1));

    float2 uv;
    bool found = TraceMirrorRay(surfacePos, mirrorDir, surfaceDist, uv);
    if (!found)
    {
        // The actual rendered sky, found by projecting a distant point along
        // the mirror ray; only accepted if that texel really is sky.
        found = WorldToScreenUV(surfacePos + mirrorDir * 20000.0f, uv)
             && IsSkyDepth(gDepthBuffer.SampleLevel(gPointClamp, uv, 0).r);
    }

    float3 color = SkyGradient(reflect(-eyeDir, normal));
    if (found)
    {
        const float2 dudv = uv + gDistortion * normal.xy;
        color = bicubic
            ? SampleBicubic(gRefractionTexture, gLinearClamp, float2(gScreenWidth, gScreenHeight), dudv).rgb
            : gRefractionTexture.SampleLevel(gLinearClamp, dudv, 0).rgb;
    }
    return color;
}

float4 PSMain(VertexOutput fs_in) : SV_Target0
{
    const bool useDisplacement = HasFeature(WATER_FEATURE_DISPLACEMENT);
    const bool useFiltering    = HasFeature(WATER_FEATURE_FILTERING);

    float timer = fs_in.Timer;
    float2 windDir = fs_in.Wind.xy;
    float2 timedWindDir = fs_in.Wind.zw;
    float2 ndcPos = fs_in.Position.xy / float2(gScreenWidth, gScreenHeight);
    float3 surfacePosition = fs_in.WorldPos;
    float  surfaceDist = length(gCameraPos - surfacePosition);
    float3 eyeDir = (gCameraPos - surfacePosition) / max(surfaceDist, 1e-4);
    float3 lightColor = gSunColor;
    float3 lightDir = normalize(-gSunDirection);

    // The original's colours are authored as plain albedo-like values; light
    // them with the current sun + sky so they respond to time of day.
    const float3 colorLight = gSkyColor + gSunColor * saturate(lightDir.z);
    const float  colorLightLum = dot(colorLight, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 surfaceColor = gSurfaceColor.rgb * colorLightLum;
    const float3 shoreColor   = gShoreColor.rgb * colorLightLum;
    const float3 depthColor   = gDepthColor.rgb * colorLightLum;

    // --- Opaque scene behind the water: occlusion + depth position ---
    float depth = gDepthBuffer.SampleLevel(gPointClamp, ndcPos, 0).r;
    float3 depthPosition;
    float opaqueDist;
    if (IsSkyDepth(depth))
    {
        // Nothing behind the water: treat it as very deep.
        depthPosition = surfacePosition - float3(0, 0, 1000.0);
        opaqueDist = 1e8;
    }
    else
    {
        depthPosition = ReconstructWorldPosition(ndcPos, depth);
        opaqueDist = length(depthPosition - gCameraPos);
    }
    clip(opaqueDist - surfaceDist + 0.05f);

    //wave normal
    float3 normal = ComputeNormal(gNormalTexture, gWrap, surfacePosition.xy, fs_in.UV,
        fs_in.Normal, fs_in.Tangent, fs_in.Bitangent, gWavesNoise, gWavesIntensity, timedWindDir,
        useDisplacement, useFiltering);
    normal = normalize(lerp(fs_in.Normal, normalize(normal), gNormalIntensity));

    // compute refracted color
    float waterDepth = surfacePosition.z - depthPosition.z;             // horizontal water depth
    float viewWaterDepth = length(surfacePosition - depthPosition);    // water depth from the view direction(water accumulation)
    float2 dudv = ndcPos;
    {
        // refraction based on water depth
        float refractionScale = gRefractionScale * min(waterDepth, 1.0f);
        float2 delta = float2(sin(timer + 3.0f * abs(depthPosition.z)),
                              sin(timer + 5.0f * abs(depthPosition.z)));
        dudv += windDir * delta * refractionScale;

        // Engine addition: don't pull in geometry that is in front of the water.
        const float refractDepth = gDepthBuffer.SampleLevel(gPointClamp, dudv, 0).r;
        if (!IsSkyDepth(refractDepth)
            && length(ReconstructWorldPosition(dudv, refractDepth) - gCameraPos) < surfaceDist - 0.05f)
        {
            dudv = ndcPos;
        }
    }
    float3 pureRefractionColor = gRefractionTexture.SampleLevel(gLinearClamp, dudv, 0).rgb;
    float2 waterTransparency = float2(gWaterClarity, gWaterTransparency);
    float2 waterDepthValues = float2(waterDepth, viewWaterDepth);
    float shoreRange = max(gFoamRanges.x, gFoamRanges.y) * 2.0;
    float3 refractionColor = DepthRefraction(waterTransparency, waterDepthValues, shoreRange, gHorizontalExtinction.xyz,
                                             pureRefractionColor, shoreColor, surfaceColor, depthColor);

    // compute ligths's reflected radiance
    float fresnel = FresnelValue(gRefractionValues, normal, eyeDir);
    float3 specularColor = ReflectedRadiance(gShininess, gSpecularValues.xyz, lightColor, lightDir, eyeDir, normal, fresnel,
                                             HasFeature(WATER_FEATURE_BLINN_PHONG));
    // Engine addition: the HDR target has no saturate() to clip a stray
    // near-mirror pixel, which would otherwise become a TAA firefly.
    specularColor = min(specularColor, 64.0f);

    // compute reflected color
    float3 reflectColor = SampleReflection(surfacePosition, eyeDir, normal, surfaceDist, useFiltering);

    // shore foam
    float foam = 0;
    if (HasFeature(WATER_FEATURE_FOAM))
    {
        float maxAmplitude = max(max(gWaveAmplitude.x, gWaveAmplitude.y), gWaveAmplitude.z);
        foam = FoamValue(gShoreTexture, gFoamTexture, gWrap, gFoamTiling,
                         gFoamNoise, gFoamSpeed * windDir, gFoamRanges.xyz, maxAmplitude,
                         surfacePosition, depthPosition, eyeDir, waterDepth, surfacePosition.z - gWaterLevel,
                         timedWindDir, timer);
        foam *= gFoamIntensity;
    }

    float shoreFade = saturate(waterDepth * gShoreFade);
    // ambient + diffuse
    float3 ambientColor = gSkyColor * gAmbientDensity + saturate(dot(normal, lightDir)) * gDiffuseDensity * lightColor;
    // refraction color with depth based color
    pureRefractionColor = lerp(pureRefractionColor, reflectColor, fresnel * saturate(waterDepth / (gFoamRanges.x * 0.4)));
    pureRefractionColor = lerp(pureRefractionColor, shoreColor, 0.30 * shoreFade);
    // compute final color
    float3 color = lerp(refractionColor, reflectColor, fresnel);
    color = max(ambientColor + color + max(specularColor, foam * lightColor), 0.0);
    color = lerp(pureRefractionColor + specularColor * shoreFade, color, shoreFade);

    if (gDebugMode != WATER_DEBUG_OFF)
    {
        if (gDebugMode == WATER_DEBUG_NORMALS)
            color = 0.5 + 2 * ambientColor + specularColor + saturate(dot(normal, lightDir)) * 0.5;
        else if (gDebugMode == WATER_DEBUG_FRESNEL)
            color = fresnel.xxx;
        else if (gDebugMode == WATER_DEBUG_DEPTH)
            color = saturate(waterDepth / max(shoreRange, 1e-3)).xxx;
        else if (gDebugMode == WATER_DEBUG_REFRACTION)
            color = refractionColor;
        else if (gDebugMode == WATER_DEBUG_REFLECTION)
            color = reflectColor;
        else if (gDebugMode == WATER_DEBUG_SPECULAR)
            color = specularColor;
        else if (gDebugMode == WATER_DEBUG_FOAM)
            color = foam.xxx;
    }

    return float4(color, 1.0);
}
