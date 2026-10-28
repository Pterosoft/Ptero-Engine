// VoxelGI_Voxelize.hlsl
// Rasterizes scene meshes into one level of the voxel GI clipmap.
//
// Each triangle is projected orthographically along whichever axis it faces
// most (the geometry shader picks it per triangle), onto a Resolution x
// Resolution viewport covering the level, so every covered pixel is one column
// of voxels and the interpolated depth names the voxel in it. Nothing is
// rendered to a target; the pixel shader folds the voxel's albedo and normal
// into one level of keys through atomic UAVs (PackVoxelAlbedoKey), which
// VoxelGI_Resolve.hlsl then writes into the clipmap.
//
// Bound on VoxelGIRenderer's voxelization root signature, which keeps the
// G-Buffer pass's parameter layout for b1 and t0-t9 so EntityMeshRenderer can
// bind each sub-material exactly as it does for the G-Buffer:
//   b0  root constants: model matrix
//   b1  MaterialConstants (GBuffer.hlsl)
//   b2  VoxelGIConstants
//   t0-t9 material textures (EntityMeshRenderer::kMaterialTextureSlotCount),
//   u0 albedo/opacity keys, u1 normal keys (R32_UINT, one level)

#define VOXEL_GI_CB_REGISTER b2
#include "VoxelGI_Common.hlsli"

cbuffer VoxelDrawConstants : register(b0)
{
    float4x4 gModel;    // pre-transposed model-to-world matrix
};

// Leading fields of GBuffer.hlsl's MaterialConstants, in the same order; the
// rest of that cbuffer is not read here, so it is not declared.
cbuffer MaterialConstants : register(b1)
{
    float4 gBaseColorTint;
    float3 gEmissiveColor;      float _MatPad0;
    float  gMetallicFactor;
    float  gRoughnessFactor;
    float  gNormalScale;
    float  gAoStrength;
    int    gHasNormalMap;
    int    gHasMetallicMap;
    int    gHasRoughnessMap;
    int    gHasAoMap;
    int    gHasEmissiveMap;
    int    gHasPackedMaterialMap;
    float  gSpecularFactor;
    int    gSubsurfaceSlot;
    float  gOpacityFactor;
    float  gAlphaCutoff;
    int    gHasOpacityMap;
    int    gUseAlphaCutout;
    int    gUseTransparentBlend;
    int    gFlipNormalGreen;
    float2 _MatPad2;
    float2 gUvTiling;
    float2 gUvOffset;
    float  gUvRotationSin;
    float  gUvRotationCos;
};

Texture2D    gBaseColorTexture : register(t0);
Texture2D    gOpacityTexture   : register(t6);
SamplerState gLinearWrap       : register(s0);

RWTexture3D<uint> gVoxelAlbedoKeys : register(u0);
RWTexture3D<uint> gVoxelNormalKeys : register(u1);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

struct GSInput
{
    float3 WorldPosition : WORLDPOS;
    float3 WorldNormal   : NORMAL;
    float2 TexCoord      : TEXCOORD;
    float4 Color         : COLOR;
};

struct PSInput
{
    float4 Position    : SV_Position;
    float3 VoxelCoord  : VOXELCOORD;   // continuous voxel coordinates in the level
    float3 WorldNormal : NORMAL;
    float2 TexCoord    : TEXCOORD;
    float4 Color       : COLOR;
};

GSInput VSMain(VSInput input)
{
    GSInput output;
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.WorldNormal   = mul(float4(input.Normal, 0.0f), gModel).xyz;
    output.TexCoord      = input.TexCoord;
    output.Color         = input.Color;
    return output;
}

[maxvertexcount(3)]
void GSMain(triangle GSInput input[3], inout TriangleStream<PSInput> stream)
{
    const float3 levelOrigin = gLevelOrigin[gUpdateLevel].xyz;
    const float  voxelSize   = gLevelOrigin[gUpdateLevel].w;

    float3 voxelCoord[3];
    [unroll]
    for (int i = 0; i < 3; ++i)
        voxelCoord[i] = (input[i].WorldPosition - levelOrigin) / voxelSize;

    // Project along the axis the triangle faces most, which gives it the
    // largest footprint and guarantees no step in depth exceeds one voxel per pixel.
    const float3 faceNormal = abs(cross(voxelCoord[1] - voxelCoord[0], voxelCoord[2] - voxelCoord[0]));
    const uint axis = (faceNormal.x > faceNormal.y && faceNormal.x > faceNormal.z) ? 0u
                    : (faceNormal.y > faceNormal.z) ? 1u : 2u;

    const float invResolution = gInvResolution;

    [unroll]
    for (int v = 0; v < 3; ++v)
    {
        const float3 c = voxelCoord[v];
        const float3 projected = axis == 0u ? c.yzx : (axis == 1u ? c.zxy : c.xyz);

        PSInput output;
        output.Position    = float4(projected.xy * (2.0f * invResolution) - 1.0f, projected.z * invResolution, 1.0f);
        output.VoxelCoord  = c;
        output.WorldNormal = input[v].WorldNormal;
        output.TexCoord    = input[v].TexCoord;
        output.Color       = input[v].Color;
        stream.Append(output);
    }
}

float2 TransformUv(float2 uv)
{
    const float2 centered = uv - 0.5f;
    const float2 rotated = float2(
        centered.x * gUvRotationCos - centered.y * gUvRotationSin,
        centered.x * gUvRotationSin + centered.y * gUvRotationCos) + 0.5f;
    return rotated * gUvTiling + gUvOffset;
}

void PSMain(PSInput input)
{
    const int3 voxel = int3(floor(input.VoxelCoord));
    if (!VoxelInside(voxel))
        return;

    // Implicit derivatives are per voxel here, so the base colour is fetched at
    // the mip whose texels are about one voxel across - the average a voxel holds.
    const float2 uv = TransformUv(input.TexCoord);
    const float4 texColor = gBaseColorTexture.Sample(gLinearWrap, uv);

    float opacity = texColor.a * input.Color.a * gBaseColorTint.a * saturate(gOpacityFactor);
    if (gHasOpacityMap)
        opacity *= gOpacityTexture.Sample(gLinearWrap, uv).r;
    if (gUseAlphaCutout && opacity < saturate(gAlphaCutoff))
        return;

    // Glass and other blended surfaces let light through; they keep only the
    // part of it they actually block.
    const bool transparent = gUseTransparentBlend
        || (gHasOpacityMap && !gUseAlphaCutout)
        || (!gUseAlphaCutout && gOpacityFactor < 0.9999f);
    const float coverage = transparent ? saturate(opacity) : 1.0f;
    if (coverage <= 0.01f)
        return;

    const float3 albedo = saturate(texColor.rgb * input.Color.rgb * gBaseColorTint.rgb);
    // A degenerate normal would pack as NaN and win every voxel it touches.
    const float normalLengthSq = dot(input.WorldNormal, input.WorldNormal);
    const float3 normal = normalLengthSq > 1.0e-12f ? input.WorldNormal * rsqrt(normalLengthSq) : float3(0.0f, 0.0f, 1.0f);

    InterlockedMax(gVoxelAlbedoKeys[voxel], PackVoxelAlbedoKey(albedo, coverage));
    InterlockedMax(gVoxelNormalKeys[voxel], PackVoxelNormalKey(normal));
}
