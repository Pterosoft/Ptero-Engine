// Sssr_Apply.hlsl
// FidelityFX SSSR, final pass: weights the denoised reflected radiance by the surface's
// environment BRDF and adds it to the scene colour. Written for this engine: the sample's
// ApplyReflections.hlsl was a raster pass reading Cauldron's BRDF LUT, which does not
// exist here, so the split-sum term uses Karis' analytic fit instead.
//
// The reflection is added, not blended: the radiance is premultiplied by hit confidence,
// so a surface where no ray found anything keeps exactly the shading the deferred resolve
// gave it. That matches how the classic SSR pass composites.
//
// Output goes to a private target that the caller copies back over the scene colour.
// Part of the SSSR pass set, which is compiled for SM6 because its other passes use Wave
// intrinsics.

#include "Sssr_Common.hlsli"
#include "SurfaceSpecular.hlsli"

Texture2D<float4> gSceneColor      : register(t0);
Texture2D<float4> gReflection      : register(t1);
Texture2D<float4> gGBufferNormal   : register(t2);   // .xy oct normal, .w specular
Texture2D<float4> gGBufferMaterial : register(t3);   // .r roughness, .g metallic
Texture2D<float4> gGBufferAlbedo   : register(t4);
Texture2D<float>  gDepthBuffer     : register(t5);

RWTexture2D<float4> gOutput : register(u0);

// Karis, "Physically Based Shading on Mobile" - analytic fit of the split-sum BRDF LUT.
float3 EnvBrdfApprox(float3 F0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    const float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gBufferDimensions.x || id.y >= gBufferDimensions.y)
        return;

    const int3 load = int3(id.xy, 0);
    const float3 sceneColor = gSceneColor.Load(load).rgb;
    const float  depth      = gDepthBuffer.Load(load);
    const float4 material   = gGBufferMaterial.Load(load);

    float3 reflection = 0.0f.xxx;

    if (depth < 1.0f && !PteroSssrIsGlass(material))
    {
        const float  roughness = saturate(material.r);
        const float  metallic  = material.g;
        const float4 normalSample = gGBufferNormal.Load(load);
        const float3 N = PteroSssrDecodeOctNormal(normalSample.xy);

        const float2 uv = (float2(id.xy) + 0.5f) * gInvBufferDimensions;
        const float3 P = PteroSssrScreenToWorld(float3(uv, depth));
        const float3 V = normalize(gCameraPos - P);
        const float  NoV = saturate(dot(N, V));

        const float3 albedo = gGBufferAlbedo.Load(load).rgb;
        const float3 F0 = PteroComputeF0(albedo, metallic, PteroDecodeSurfaceSpecular(normalSample.w));

        // Rays stop at the roughness threshold. Fade in over the last quarter below it, so a
        // surface with varying roughness does not show the cutoff as a seam.
        const float roughFade = 1.0f - smoothstep(gRoughnessThreshold * 0.75f, gRoughnessThreshold, roughness);

        const float3 radiance = max(gReflection.Load(load).rgb, 0.0f.xxx);
        reflection = radiance * EnvBrdfApprox(F0, roughness, NoV) * roughFade * gIntensity;
    }

    float3 result = sceneColor + reflection;
    if (gDebugView != 0)
        result = reflection;   // 1 = denoised reflection alone, 2 = same with the denoiser off

    gOutput[id.xy] = float4(result, 1.0f);
}
