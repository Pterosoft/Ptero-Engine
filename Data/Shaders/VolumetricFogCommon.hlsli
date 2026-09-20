#ifndef PTERO_VOLUMETRIC_FOG_COMMON_HLSLI
#define PTERO_VOLUMETRIC_FOG_COMMON_HLSLI

// The fog reads the scene's light array in the same layout every other pass
// uses, and resolves spot cones and rect panels through the same helpers. A
// private light record here is what let the fog's falloff drift away from the
// deferred pass's until a light lit the medium nothing like it lit the walls.
#include "LightShapes.hlsli"

// Probe SH decoding, shared with deferred shading so both read the grid the
// probe renderer writes with one definition of the packing.
#include "RadianceProbeCommon.hlsli"

// Matches DeferredLightingPass::kMaxPointLights, which is the engine-wide cap:
// with a slot per scene light the fog can never run out and silently drop one.
#define PTERO_FOG_MAX_LIGHTS 16

// Matches DeferredLightingPass::kMaxShadowCastingPointLights * 6.
#define PTERO_FOG_MAX_SHADOW_FACES 24

cbuffer FogConstants : register(b0)
{
    uint     gFrameWidth;
    uint     gFrameHeight;
    uint     gFroxelWidth;
    uint     gFroxelHeight;

    uint     gDepthSlices;
    uint     gDebugView;
    float    gNearPlane;
    float    gFarPlane;

    float    gStartDistance;
    float    gMaxDistance;
    float    gDensity;           // extinction coefficient sigma_t, per world unit
    float    gAnisotropy;

    float    gBaseHeight;
    float    gHeightFalloff;
    float    gScatteringAlbedo;  // sigma_s / sigma_t; below 1 the medium absorbs
    float    gGiIntensity;

    float3   gFogColor;
    float    _FogPad0;

    float3   gEmissiveColor;
    float    gEmissiveIntensity;

    float3   gCameraPos;
    float    _FogPad1;

    float3   gSunDir;            // travel direction, FROM the sun TOWARD the scene
    float    _FogPad2;

    float3   gSunColor;
    float    _FogPad3;

    float3   gSkyColor;
    float    _FogPad4;

    uint     gNumPointLights;
    uint     gProbeGridX;
    uint     gProbeGridY;
    uint     gProbeGridZ;

    float3   gProbeOrigin;
    float    gProbeSpacing;

    float    gPointShadowMapSize;
    float    gPointShadowBias;
    int      gPointShadowLightCount;
    float    _FogPad5;

    PteroLightData gPointLights[PTERO_FOG_MAX_LIGHTS];
    float4x4       gPointShadowFaceViewProj[PTERO_FOG_MAX_SHADOW_FACES];

    float4x4 gViewProjInv;
    float4x4 gCurrViewProj;
};

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gViewProjInv);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

float SliceToViewDepth(float sliceIndex)
{
    float z = saturate((sliceIndex + 0.5f) / max((float)gDepthSlices, 1.0f));
    float nearZ = max(gStartDistance, 0.001f);
    float farZ = max(gMaxDistance, nearZ + 0.001f);
    return nearZ * pow(farZ / nearZ, z);
}

// Normalised over the sphere: the 4*pi is already folded in, so a phase value
// multiplied by an irradiance gives scattered radiance directly and no pass
// needs a hand-tuned multiplier to bring it back to a sensible range.
float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - g2) / (12.56637061f * denom * sqrt(denom));
}

// cosTheta for the phase function, given where the light is travelling and
// where the camera is. Both are expressed as travel directions so forward
// scattering - a bright halo when you look toward a light through haze - comes
// out as cosTheta near +1 for every light type alike.
float FogPhaseCosine(float3 lightTravelDir, float3 cameraRayDir)
{
    // The camera ray points away from the eye; light that reaches the eye
    // travels back along it.
    return dot(lightTravelDir, -cameraRayDir);
}

float ComputeHeightDensity(float worldZ)
{
    if (gHeightFalloff <= 0.0f)
        return 1.0f;
    return exp(-max(0.0f, worldZ - gBaseHeight) * gHeightFalloff);
}

#endif // PTERO_VOLUMETRIC_FOG_COMMON_HLSLI
