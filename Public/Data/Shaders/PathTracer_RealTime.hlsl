// PathTracer_RealTime.hlsl  -  cs_6_5 (DXR inline RayQuery)
// Real-time path tracing: one diffuse and one specular path per pixel from the
// G-Buffer surface, written as NRD RELAX_DIFFUSE_SPECULAR's inputs.
//
// The first hit is the raster G-Buffer, not a camera ray: it already has normal
// maps, full-resolution textures, terrain, decals and the TAA jitter the upscalers
// expect, and costs no ray. From it:
//   - (g_SamplesPerFrame times) direct light from the sun and one scene light (PT_FLAG_ONE_LIGHT), split by lobe,
//     a subsurface material's diffuse scattered under the surface (PathTracer_Common.hlsli)
//   - one cosine-sampled diffuse path and one VNDF-sampled specular path, each
//     continued by TracePathFrom for the remaining bounces, and ended in the screen
//     radiance cache where it can be (PT_FLAG_SCREEN_CACHE)
// Each lobe's radiance is divided by its reflectance (the albedo for diffuse, the
// split-sum DFG for specular) before it reaches the denoiser, so texture and
// Fresnel detail never go through the filter; PathTracer_Composite.hlsl multiplies
// them back.

#define PT_SCREEN_CACHE
#include "PathTracer_Common.hlsli"

#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

Texture2D<float4>   t_GBufferAlbedo   : register(t39);   // rgb albedo
Texture2D<float4>   t_GBufferNormal   : register(t40);   // xy oct normal, z device depth, w specular/flags
Texture2D<float4>   t_GBufferMaterial : register(t41);   // r roughness, g metallic, b AO
Texture2D<float4>   t_GBufferPrincipled0 : register(t47); // Disney: subsurface, specular tint, sheen, sheen tint
Texture2D<float4>   t_GBufferPrincipled1 : register(t48); // Disney: clearcoat, clearcoat gloss, anisotropic, tangent

RWTexture2D<float4> u_DiffuseRadianceHitDist  : register(u2);   // IN_DIFF_RADIANCE_HITDIST
RWTexture2D<float4> u_SpecularRadianceHitDist : register(u3);   // IN_SPEC_RADIANCE_HITDIST
RWTexture2D<uint4>  u_NormalRoughness         : register(u4);   // IN_NORMAL_ROUGHNESS (R10G10B10A2 through a UINT view)
RWTexture2D<float>  u_ViewZ                   : register(u5);   // IN_VIEWZ
RWTexture2D<float2> u_MotionVectors           : register(u6);   // IN_MV

// What escaped rays report as their hit distance: far, but within half precision.
static const float kEscapedHitDistance = 1000.0f;
// The furthest a camera ray looks past the G-Buffer point for displaced terrain.
static const float kMaxTerrainReach = 2.0f;

void WriteNormalRoughness(uint2 pixel, float3 normal, float roughness)
{
    // NRD_NORMAL_ENCODING 2 = R10G10B10A2_UNORM, written through a UINT view because
    // that format has no typed UAV store (as RtGI_NrdPrepare.hlsl does).
    const float4 packed = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0);
    u_NormalRoughness[pixel] = uint4(
        (uint)(saturate(packed.x) * 1023.0f + 0.5f) & 0x3FFu,
        (uint)(saturate(packed.y) * 1023.0f + 0.5f) & 0x3FFu,
        (uint)(saturate(packed.z) * 1023.0f + 0.5f) & 0x3FFu,
        (uint)(saturate(packed.w) *    3.0f + 0.5f) & 0x3u);
}

void WriteNothing(uint2 pixel)
{
    // NRD ignores pixels whose view depth is beyond its denoising range.
    u_DiffuseRadianceHitDist[pixel]  = float4(0.0f, 0.0f, 0.0f, 0.0f);
    u_SpecularRadianceHitDist[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    u_ViewZ[pixel] = 1e6f;
    u_MotionVectors[pixel] = float2(0.0f, 0.0f);
    WriteNormalRoughness(pixel, float3(0.0f, 0.0f, 1.0f), 1.0f);
}

float2 ProjectUv(float3 worldPos, float4x4 viewProj)
{
    const float4 clip = mul(float4(worldPos, 1.0f), viewProj);
    float2 uv = clip.xy / clip.w * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    return uv;
}

// The G-Buffer surface as the integrator's Surface. Rays leave from the actual
// triangle under the pixel, found with one short camera ray and lifted onto the
// smooth surface (Hanika) by ResolveSurface; where that ray does not land on the
// G-Buffer point (geometry the TLAS does not hold, such as terrain), from the
// G-Buffer point itself.
Surface GBufferSurface(float3 worldPos, float3 normal, float3 albedo, float4 material, float packedSpecular,
                       PteroPrincipled principled)
{
    Surface s;
    s.position = worldPos + normal * 0.002f;
    s.normal = normal;
    s.geoNormal = normal;
    s.albedo = saturate(albedo);
    s.roughness = clamp(material.r, 0.04f, 1.0f);
    s.metallic = saturate(material.g);
    s.specular = PteroDecodeSurfaceSpecular(packedSpecular);
    s.emissive = float3(0.0f, 0.0f, 0.0f);
    s.principled = principled;
    s.terrainTrace = kInvalidIndex;
    s.displacement = 0.0f;
    s.sssStrength = float3(0.0f, 0.0f, 0.0f);
    s.sssFalloff = float3(0.0f, 0.0f, 0.0f);
    s.sssRadius = 0.0f;
    s.sssTranslucency = 0.0f;

    const float3 toSurface = worldPos - g_CameraPos;
    const float surfaceDistance = length(toSurface);
    if (surfaceDistance > 1e-4f)
    {
        // Depth precision and TAA jitter both grow with distance.
        const float tolerance = max(0.01f, surfaceDistance * 0.01f);
        const float3 dir = toSurface / surfaceDistance;
        // The first thing along the camera ray, if it is what the pixel shows: within
        // the depth tolerance, or terrain within its displacement reach. Displaced
        // terrain stands up to that reach off the traced mesh, so its G-Buffer point
        // can be above or below it. Starting the search at the G-Buffer depth instead
        // skipped the traced terrain when it was in front, and snapped to whatever
        // lay buried under it inside the window - a point inside the terrain, where
        // every ray is blocked: the black patches on displaced sand.
        HitRecord rec = TraceClosest(g_CameraPos, dir, 0.0f, surfaceDistance + tolerance + kMaxTerrainReach);
        bool matched = false;
        if (rec.hit)
        {
            const float reach = TerrainReach(rec);
            matched = abs(rec.t - surfaceDistance) <= tolerance
                   || (reach >= 0.0f && abs(rec.t - surfaceDistance)
                                        <= tolerance + PteroTerrainReachAlongRay(reach, HitFaceNormal(rec), dir));
        }
        // Something traced but not drawn is in front (or nothing matched): the old
        // window around the G-Buffer depth.
        if (!matched)
            rec = TraceClosest(g_CameraPos, dir, max(surfaceDistance - tolerance, 0.0f), surfaceDistance + tolerance);
        if (rec.hit)
        {
            const Surface traced = ResolveSurface(rec, g_CameraPos, dir, 0.0f);
            s.position = traced.position + traced.geoNormal * max(0.0005f, surfaceDistance * 1e-4f);
            s.geoNormal = traced.geoNormal;
            // Displaced terrain: shadow rays march its displacement from here.
            s.terrainTrace = traced.terrainTrace;
            s.displacement = traced.displacement;
            // Subsurface scattering: the traced material's profile, where the pixel was
            // drawn as a scattering surface (a G-Buffer profile slot; a blended surface,
            // or one that found no free slot, does not scatter on screen either).
            if (PteroDecodeSubsurfaceSlot(packedSpecular) != 0u)
            {
                s.sssStrength = traced.sssStrength;
                s.sssFalloff = traced.sssFalloff;
                s.sssRadius = traced.sssRadius;
                s.sssTranslucency = traced.sssTranslucency;
            }
        }
    }

    // A normal map can tilt the shading normal away from the camera; nothing can be
    // reflected from behind the surface, so bring it back to grazing.
    const float3 V = -normalize(toSurface);
    const float NdotV = dot(s.normal, V);
    if (NdotV < 0.01f)
        s.normal = normalize(s.normal + V * (0.01f - NdotV));
    return s;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixel = dtid.xy;
    if (pixel.x >= g_Width || pixel.y >= g_Height)
        return;

    const float4 normalSample = t_GBufferNormal.Load(int3(pixel, 0));
    const float4 material = t_GBufferMaterial.Load(int3(pixel, 0));
    const float depth = normalSample.z;
    [branch]
    if (depth <= 0.0f || depth >= 1.0f || IsGlassMaterial(material))
    {
        WriteNothing(pixel);
        return;
    }

    const float3 worldPos = ReconstructGBufferPosition(pixel, depth);
    const float3 gbufferNormal = DecodeGBufferNormal(normalSample.xy);
    const float3 albedo = t_GBufferAlbedo.Load(int3(pixel, 0)).rgb;
    PteroPrincipled principled = PteroPrincipledNone();
    if ((g_Flags & PT_FLAG_DISNEY_BRDF) != 0u)
        principled = PteroDecodePrincipled(t_GBufferPrincipled0.Load(int3(pixel, 0)), t_GBufferPrincipled1.Load(int3(pixel, 0)), gbufferNormal);
    const Surface s = GBufferSurface(worldPos, gbufferNormal, albedo, material, normalSample.w, principled);
    const float3 V = normalize(g_CameraPos - worldPos);

    uint rng = PcgHash(pixel.x ^ PcgHash(pixel.y ^ PcgHash(g_FrameSeed))) | 1u;

    const float NdotV = saturate(dot(s.normal, V));
    const float3 F0 = PteroComputeF0(s.albedo, s.metallic, s.specular);
    const float3 diffuseReflectance = s.albedo * (1.0f - s.metallic);
    const float3 specularReflectance = EnvironmentBrdfApprox(F0, s.roughness, NdotV);

    float3 diffuse = float3(0.0f, 0.0f, 0.0f);
    float3 specular = float3(0.0f, 0.0f, 0.0f);
    float diffuseHitDistance = 0.0f;
    float specularHitDistance = 0.0f;
    const uint samples = clamp(g_SamplesPerFrame, 1u, 4u);

    [loop]
    for (uint i = 0; i < samples; ++i)
    {
        // Direct light, by lobe.
        float3 directDiffuse, directSpecular;
        DirectLightingSplit(s, V, rng, directDiffuse, directSpecular);
        diffuse += directDiffuse;
        specular += directSpecular;

        float diffuseHit = kEscapedHitDistance;
        float specularHit = kEscapedHitDistance;
        [branch]
        if (g_MaxBounces > 0u)
        {
            // Diffuse path: cosine-sampled, so f * cos / pdf = (1 - F) (1 - metallic) albedo.
            if (Luminance(diffuseReflectance) > 1e-4f)
            {
                const float3 L = SampleDiffuseDirection(s, rng);
                if (dot(L, s.geoNormal) > 0.0f)
                {
                    bool hit;
                    float hitT;
                    const float3 incoming = TracePathFrom(OffsetAlong(s, L), L, 1u, rng, hit, hitT);
                    [branch]
                    if ((g_Flags & PT_FLAG_DISNEY_BRDF) != 0u)
                    {
                        // The principled diffuse and sheen over the cosine pdf (N.L / pi).
                        float3 lobeDiffuse, lobeSpecular;
                        EvalBrdfCosSplit(s, V, L, lobeDiffuse, lobeSpecular);
                        diffuse += lobeDiffuse * (kPi / max(dot(s.normal, L), 1e-4f)) * incoming;
                    }
                    else
                    {
                        const float3 H = normalize(V + L);
                        diffuse += (1.0f - FresnelSchlick(F0, dot(V, H))) * diffuseReflectance * incoming;
                    }
                    diffuseHit = hit ? hitT : kEscapedHitDistance;
                }
            }

            // Specular path: visible-normal sampled, so f * cos / pdf = F G2 / G1(V).
            {
                const float3 L = SampleSpecularDirection(s, V, rng);
                const float NdotL = dot(s.normal, L);
                if (NdotL > 0.0f && dot(L, s.geoNormal) > 0.0f)
                {
                    bool hit;
                    float hitT;
                    const float3 incoming = TracePathFrom(OffsetAlong(s, L), L, 1u, rng, hit, hitT);
                    [branch]
                    if ((g_Flags & PT_FLAG_DISNEY_BRDF) != 0u)
                    {
                        // The principled specular and clearcoat over the sampler's own pdf;
                        // the closed form below only holds for the isotropic GGX lobe.
                        float3 lobeDiffuse, lobeSpecular;
                        EvalBrdfCosSplit(s, V, L, lobeDiffuse, lobeSpecular);
                        specular += lobeSpecular / max(SpecularPdf(s, V, L), 1e-6f) * incoming;
                    }
                    else
                    {
                        const float3 H = normalize(V + L);
                        const float alpha = s.roughness * s.roughness;
                        const float alpha2 = alpha * alpha;
                        const float lambdaV = SmithLambda(max(NdotV, 1e-4f), alpha2);
                        const float weightG = (1.0f + lambdaV) / (1.0f + lambdaV + SmithLambda(NdotL, alpha2));
                        specular += FresnelSchlick(F0, dot(V, H)) * weightG * incoming;
                    }
                    specularHit = hit ? hitT : kEscapedHitDistance;
                }
            }
        }
        diffuseHitDistance += diffuseHit;
        specularHitDistance += specularHit;
    }
    diffuse /= float(samples);
    specular /= float(samples);
    diffuseHitDistance /= float(samples);
    specularHitDistance /= float(samples);

    // Demodulate: what the denoiser filters is lighting, not the surface. Scattered and
    // transmitted light carry this pixel's albedo too (post-scatter texturing), so they
    // divide out the same way.
    const float3 diffuseLighting = diffuse / max(diffuseReflectance, 1e-3f);
    const float3 specularLighting = specular / max(specularReflectance, 1e-3f);
    u_DiffuseRadianceHitDist[pixel]  = RELAX_FrontEnd_PackRadianceAndHitDist(SanitizeRadiance(diffuseLighting), diffuseHitDistance, true);
    u_SpecularRadianceHitDist[pixel] = RELAX_FrontEnd_PackRadianceAndHitDist(SanitizeRadiance(specularLighting), specularHitDistance, true);

    // NRD's guides: shading normal and roughness, view depth, and camera motion
    // (non-jittered; the jitter goes to NRD separately).
    WriteNormalRoughness(pixel, s.normal, s.roughness);
    u_ViewZ[pixel] = max(1e-6f, abs(mul(float4(worldPos, 1.0f), g_WorldToView).z));
    u_MotionVectors[pixel] = ProjectUv(worldPos, g_PrevViewProj) - ProjectUv(worldPos, g_CurrViewProj);
}
