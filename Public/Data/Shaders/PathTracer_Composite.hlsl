// PathTracer_Composite.hlsl  -  cs_6_5 (shares PathTracer_Common.hlsli, which uses RayQuery)
// Real-time path tracing, last pass: NRD's denoised diffuse and specular lighting,
// multiplied back by the G-Buffer surface's reflectances, written over the image.
// Sky and glass pixels keep the raster image.

#include "PathTracer_Common.hlsli"

Texture2D<float4>   t_GBufferAlbedo   : register(t39);
Texture2D<float4>   t_GBufferNormal   : register(t40);
Texture2D<float4>   t_GBufferMaterial : register(t41);
Texture2D<float4>   t_DenoisedDiffuse  : register(t43);   // OUT_DIFF_RADIANCE_HITDIST
Texture2D<float4>   t_DenoisedSpecular : register(t44);   // OUT_SPEC_RADIANCE_HITDIST

RWTexture2D<float4> u_Output                  : register(u1);
RWTexture2D<float4> u_DiffuseRadianceHitDist  : register(u2);   // the noisy inputs, when not denoising
RWTexture2D<float4> u_SpecularRadianceHitDist : register(u3);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixel = dtid.xy;
    if (pixel.x >= g_Width || pixel.y >= g_Height)
        return;

    const float4 background = t_Background.Load(int3(pixel, 0));
    const float4 normalSample = t_GBufferNormal.Load(int3(pixel, 0));
    const float4 material = t_GBufferMaterial.Load(int3(pixel, 0));
    const float depth = normalSample.z;

    // The subsurface mask: what the traced surface under the pixel is, matched to the
    // G-Buffer point as PathTracer_RealTime.hlsl's GBufferSurface matches it, and only
    // where the pixel was drawn with a profile slot, as that pass requires.
    [branch]
    if ((g_Flags & PT_FLAG_SSS_MASK) != 0u)
    {
        float3 maskColor = float3(0.0f, 0.0f, 0.0f);
        if (depth > 0.0f && depth < 1.0f && PteroDecodeSubsurfaceSlot(normalSample.w) != 0u)
        {
            const float3 toSurface = ReconstructGBufferPosition(pixel, depth) - g_CameraPos;
            const float surfaceDistance = length(toSurface);
            const float tolerance = max(0.01f, surfaceDistance * 0.01f);
            const float3 dir = toSurface / max(surfaceDistance, 1e-4f);
            const HitRecord rec = TraceClosest(g_CameraPos, dir, max(surfaceDistance - tolerance, 0.0f), surfaceDistance + tolerance);
            if (rec.hit)
                maskColor = SubsurfaceMaskColor(ResolveSurface(rec, g_CameraPos, dir, 0.0f), normalSample.w);
        }
        u_Output[pixel] = float4(maskColor, 1.0f);
        return;
    }
    if (depth <= 0.0f || depth >= 1.0f || IsGlassMaterial(material))
    {
        u_Output[pixel] = background;
        return;
    }

    const float3 worldPos = ReconstructGBufferPosition(pixel, depth);
    const float3 normal = DecodeGBufferNormal(normalSample.xy);
    const float3 albedo = saturate(t_GBufferAlbedo.Load(int3(pixel, 0)).rgb);
    const float roughness = clamp(material.r, 0.04f, 1.0f);
    const float metallic = saturate(material.g);
    const float3 V = normalize(g_CameraPos - worldPos);

    // Exactly the reflectances PathTracer_RealTime.hlsl divided by.
    const float3 F0 = PteroComputeF0(albedo, metallic, PteroDecodeSurfaceSpecular(normalSample.w));
    const float3 diffuseReflectance = albedo * (1.0f - metallic);
    const float3 specularReflectance = EnvironmentBrdfApprox(F0, roughness, saturate(dot(normal, V)));

    const bool denoised = (g_Flags & PT_FLAG_DENOISED) != 0u;
    const float3 diffuseLighting = denoised ? t_DenoisedDiffuse.Load(int3(pixel, 0)).rgb : u_DiffuseRadianceHitDist[pixel].rgb;
    const float3 specularLighting = denoised ? t_DenoisedSpecular.Load(int3(pixel, 0)).rgb : u_SpecularRadianceHitDist[pixel].rgb;

    float3 color;
    switch (g_DebugView)
    {
    case 1:  color = albedo; break;
    case 2:  color = normal * 0.5f + 0.5f; break;
    case 3:  color = float3(roughness, metallic, 0.0f); break;
    // The lighting each lobe receives, before the surface colour: what NRD filtered.
    case 4:  color = diffuseLighting * g_InvPreExposure; break;
    case 5:  color = specularLighting * g_InvPreExposure; break;
    default:
        color = (diffuseReflectance * diffuseLighting + specularReflectance * specularLighting) * g_InvPreExposure;
        color = ApplyVolumetricFog(pixel, distance(g_CameraPos, worldPos), SanitizeRadiance(color));
        break;
    }
    u_Output[pixel] = float4(SanitizeRadiance(color), 1.0f);
}
