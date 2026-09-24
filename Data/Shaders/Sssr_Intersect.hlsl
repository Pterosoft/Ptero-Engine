// Sssr_Intersect.hlsl
// FidelityFX SSSR pass 3: traces one GGX-sampled reflection ray per listed pixel through
// the min-depth hierarchy (ffx_sssr.h) and fetches the lit scene where it lands. Launched
// indirectly with one 64-lane group per 64 rays. Ported from the SSSR 1.3 sample's
// Intersect.hlsl.
//
// Differences from the sample:
//   - Rays are set up in world space (the sample used view space); ffx_sssr.h itself only
//     sees screen space, so the traversal is unchanged.
//   - A miss, or the part of a hit its confidence rejects, reflects nothing instead of an
//     environment map. The result is therefore premultiplied by confidence: the composite
//     adds it on top of the ambient specular the deferred resolve already applied.
//
// ffx_sssr.h's occupancy early-out uses WaveActiveCountBits, so this needs SM6.

#include "Sssr_Common.hlsli"

Texture2D<float4> gLitScene         : register(t0);
Texture2D<float>  gDepthHierarchy   : register(t1);
Texture2D<float4> gNormal           : register(t2);   // 0.5 * n + 0.5
Texture2D<float>  gRoughness        : register(t3);   // perceptual
Texture2D<float2> gBlueNoiseTexture : register(t4);
StructuredBuffer<uint> gRayList     : register(t5);
StructuredBuffer<uint> gIndirectArgs : register(t6);   // layout in Sssr_Common.hlsli

RWTexture2D<float4> gIntersectionOutput : register(u0);

#define M_PI 3.14159265358979f

float3 FFX_SSSR_LoadWorldSpaceNormal(int2 pixelCoordinate)
{
    return normalize(2.0f * gNormal.Load(int3(pixelCoordinate, 0)).xyz - 1.0f);
}

float FFX_SSSR_LoadDepth(int2 pixelCoordinate, int mip)
{
    return gDepthHierarchy.Load(int3(pixelCoordinate, mip));
}

// ValidateHit only measures a distance with this, so camera-relative world space serves.
float3 FFX_SSSR_ScreenSpaceToViewSpace(float3 screenSpacePosition)
{
    return FFX_DNSR_Reflections_ScreenSpaceToViewSpace(screenSpacePosition);
}

// http://jcgt.org/published/0007/04/01/paper.pdf by Eric Heitz
// Input Ve: view direction
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers
// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
float3 SampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // Section 3.2: transforming the view direction to the hemisphere configuration
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0 ? float3(-Vh.y, Vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    float r = sqrt(U1);
    float phi = 2.0 * M_PI * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto hemisphere
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
    return Ne;
}

float3x3 CreateTBN(float3 N)
{
    float3 U;
    if (abs(N.z) > 0.0)
    {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U.x = 0.0; U.y = -N.z / k; U.z = N.y / k;
    }
    else
    {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U.x = N.y / k; U.y = -N.x / k; U.z = 0.0;
    }

    float3x3 TBN;
    TBN[0] = U;
    TBN[1] = cross(N, U);
    TBN[2] = N;
    return transpose(TBN);
}

float3 SampleReflectionVector(float3 viewDirection, float3 normal, float alpha, int2 pixel)
{
    const float3x3 tbnTransform = CreateTBN(normal);
    const float3 viewDirectionTbn = mul(-viewDirection, tbnTransform);

    const float2 u = gBlueNoiseTexture.Load(int3(pixel % 128, 0));
    const float3 sampledNormalTbn = SampleGGXVNDF(viewDirectionTbn, alpha, alpha, u.x, u.y);
    const float3 reflectedDirectionTbn = reflect(-viewDirectionTbn, sampledNormalTbn);

    return mul(reflectedDirectionTbn, transpose(tbnTransform));
}

#include "ffx_sssr.h"

[numthreads(64, 1, 1)]
void CSMain(uint groupIndex : SV_GroupIndex, uint2 groupId : SV_GroupID)
{
    const uint rayIndex = PteroSssrFlattenGroupId(groupId) * 64 + groupIndex;
    if (rayIndex >= gIndirectArgs[kPteroSssrArgsRayCount])
        return;

    uint2 coords;
    bool copyHorizontal;
    bool copyVertical;
    bool copyDiagonal;
    PteroSssrUnpackRayCoords(gRayList[rayIndex], coords, copyHorizontal, copyVertical, copyDiagonal);

    const uint2  screenSize = gBufferDimensions;
    const float2 uv = (coords + 0.5f) * gInvBufferDimensions;

    const float3 worldNormal = FFX_SSSR_LoadWorldSpaceNormal(coords);
    const float  roughness   = gRoughness.Load(int3(coords, 0));
    const bool   isMirror    = FFX_DNSR_Reflections_IsMirrorReflection(roughness);

    const int    mostDetailedMip = isMirror ? 0 : (int)gMostDetailedMip;
    const float2 mipResolution   = FFX_SSSR_GetMipResolution(screenSize, mostDetailedMip);
    const float  z = FFX_SSSR_LoadDepth(uv * mipResolution, mostDetailedMip);

    const float3 screenUvSpaceRayOrigin = float3(uv, z);
    const float3 worldSpaceOrigin       = PteroSssrScreenToWorld(screenUvSpaceRayOrigin);
    const float3 viewDirection          = normalize(worldSpaceOrigin - gCameraPos);

    // GGX alpha is perceptual roughness squared.
    const float3 reflectedDirection = SampleReflectionVector(viewDirection, worldNormal, roughness * roughness, coords);
    const float3 screenSpaceRayDirection = PteroSssrProjectDirection(worldSpaceOrigin, reflectedDirection, screenUvSpaceRayOrigin);

    bool validHit = false;
    const float3 hit = FFX_SSSR_HierarchicalRaymarch(screenUvSpaceRayOrigin, screenSpaceRayDirection, isMirror,
        screenSize, mostDetailedMip, gMinTraversalOccupancy, gMaxTraversalIntersections, validHit);

    const float3 worldSpaceHit = PteroSssrScreenToWorld(hit);
    const float3 worldSpaceRay = worldSpaceHit - worldSpaceOrigin;

    const float confidence = validHit
        ? FFX_SSSR_ValidateHit(hit, uv, worldSpaceRay, screenSize, gDepthBufferThickness)
        : 0.0f;
    const float worldRayLength = max(0.0f, length(worldSpaceRay));

    float3 reflectionRadiance = 0.0f.xxx;
    if (confidence > 0.0f)
    {
        reflectionRadiance = gLitScene.Load(int3(screenSize * hit.xy, 0)).rgb * confidence;
    }

    // A NaN or Inf in the scene colour would spread through the whole denoiser footprint.
    if (any(isnan(reflectionRadiance)) || any(isinf(reflectionRadiance)))
        reflectionRadiance = 0.0f.xxx;

    const float4 newSample = float4(reflectionRadiance, worldRayLength);
    gIntersectionOutput[coords] = newSample;

    const uint2 copyTarget = coords ^ 0x1; // mirrored coords within the 2x2 quad
    if (copyHorizontal)
        gIntersectionOutput[uint2(copyTarget.x, coords.y)] = newSample;
    if (copyVertical)
        gIntersectionOutput[uint2(coords.x, copyTarget.y)] = newSample;
    if (copyDiagonal)
        gIntersectionOutput[copyTarget] = newSample;
}
