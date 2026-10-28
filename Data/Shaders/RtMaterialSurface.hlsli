// RtMaterialSurface.hlsli
// The surface response of a ray-traced hit: GpuMaterialSurface in
// RtGlobalIllumination.h, one per GpuMaterialRange and at the same index.

#ifndef PTERO_RT_MATERIAL_SURFACE_HLSLI
#define PTERO_RT_MATERIAL_SURFACE_HLSLI

#define PTERO_RT_SURFACE_FLIP_NORMAL_GREEN 1u
#define PTERO_RT_SURFACE_SUBSURFACE        2u   // useSubsurfaceScattering: the sss* fields hold its profile
#define PTERO_RT_NO_TEXTURE 0xffffffffu

struct GpuMaterialSurface
{
    // What RTGI and the probes read: the material with its maps averaged.
    float3 tint;               // baseColorTint alone
    float  roughness;          // factor x the roughness map's average
    float3 emissive;           // emissiveColor x the emissive map's average
    float  metallic;           // factor x the metallic map's average
    float  specular;           // specularFactor
    float3 baseColorAverage;   // the base texture's average colour

    // What a tracer that samples the maps needs (the path tracer).
    float3 emissiveColor;
    float  roughnessFactor;
    float  metallicFactor;
    float  normalScale;
    uint   packedLayout;       // 0 separate maps, 1 metallicRoughness (R rough, G metal), 2 ORM (G, B)
    uint   flags;              // PTERO_RT_SURFACE_*

    // Indices into the surface texture table, or PTERO_RT_NO_TEXTURE.
    uint   baseColorTexture;
    uint   normalTexture;
    uint   roughnessTexture;   // the packed map when packedLayout != 0
    uint   metallicTexture;
    uint   emissiveTexture;
    uint   opacityTexture;
    uint   terrainTrace;       // terrain: its GpuTerrainTrace record (PathTracer_Common.hlsli); else 0xffffffff
    float  sssRadius;          // subsurface: the profile's range in metres (subsurfaceRadiusMm / 1000)

    // Disney principled lobes (PrincipledBrdf.hlsli), the material's values; all off at 0.
    float  principledSubsurface;
    float  principledSpecularTint;
    float  principledSheen;
    float  principledSheenTint;
    float  principledClearcoat;
    float  principledClearcoatGloss;
    float  principledAnisotropic;
    uint   principledMaskTexture;        // R clearcoat, G sheen, B anisotropic, A flatten; or PTERO_RT_NO_TEXTURE
    uint   anisotropyDirectionTexture;   // RG direction in the UV frame; or PTERO_RT_NO_TEXTURE

    // The separable-SSS profile (Subsurface.hlsli's PteroSssProfile) of a material with
    // PTERO_RT_SURFACE_SUBSURFACE, for the path tracer's diffusion BSSRDF.
    float3 sssColor;           // scatter strength per channel, 0..1
    float3 sssFalloff;         // per-channel falloff
    float  sssTranslucency;    // 0..1
};

// Base colour at a hit, as the raster G-Buffer pass computes it: the tint times
// the texture where the RT texture table holds it, otherwise times the texture's
// average. GpuMaterialRange's baseColor is tint x average already - right where
// no texture is sampled, but multiplying a texture sample into it counted the
// average twice, which made textured bounces darker and more saturated.
float3 PteroHitBaseColor(GpuMaterialSurface surface, bool textured, float3 textureColor)
{
    return surface.tint * (textured ? textureColor : surface.baseColorAverage);
}

// The roughness and metallic channels of a sampled map, as GBuffer.hlsl reads them.
float PteroSurfaceRoughnessChannel(GpuMaterialSurface surface, float4 sample)
{
    return surface.packedLayout == 2u ? sample.g : sample.r;
}

float PteroSurfaceMetallicChannel(GpuMaterialSurface surface, float4 sample)
{
    return surface.packedLayout == 2u ? sample.b : (surface.packedLayout == 1u ? sample.g : sample.r);
}

// A tangent-space normal from a BC5-style map, as GBuffer.hlsl decodes it: XY from
// red and green, green optionally flipped, scaled, Z rebuilt.
float3 PteroSurfaceTangentNormal(GpuMaterialSurface surface, float2 encoded)
{
    float2 xy = encoded * 2.0f - 1.0f;
    if ((surface.flags & PTERO_RT_SURFACE_FLIP_NORMAL_GREEN) != 0u)
        xy.y = -xy.y;
    xy *= surface.normalScale;
    return float3(xy, sqrt(saturate(1.0f - dot(xy, xy))));
}

// How far apart, measured along a ray, the drawn (tessellated, displaced) terrain and its
// traced mesh can be. A terrain range's reach is measured along the surface normal, so a
// ray meeting the surface at a slant crosses the same gap over reach / cos. Comparing the
// ray distance against the bare reach rejected every deep trough seen at a low camera
// angle; the passes then started paths from the G-Buffer point, which lies under the
// traced mesh there, and every ray from it was blocked - black blobs in the dune troughs.
// The cosine is floored so a grazing ray cannot claim a match arbitrarily far away.
float PteroTerrainReachAlongRay(float reach, float3 faceNormal, float3 rayDirection)
{
    return reach / max(abs(dot(faceNormal, rayDirection)), 0.1f);
}

#endif // PTERO_RT_MATERIAL_SURFACE_HLSLI
