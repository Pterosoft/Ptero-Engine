// VoxelGI_Resolve.hlsl
// Turns the voxelizer's keys (VoxelGI_Voxelize.hlsl) into one level of the
// voxel GI clipmap (VoxelGIRenderer):
//
//   ResolveKeysCS  unpacks the level's albedo and normal keys into its slices
//                  of the albedo and normal volumes - every voxel of the level,
//                  empty ones included, so nothing from the last fill survives -
//                  and zeroes the keys for the next voxelization
//   ClearKeysCS    zeroes the keys (when the clipmap is (re)created)
//
// The keys are one level, not stacked: the voxelizer fills a single level at a time.

#include "VoxelGI_Common.hlsli"

RWTexture3D<float4> gAlbedoOut : register(u0);   // stacked: rgb albedo, a coverage
RWTexture3D<float4> gNormalOut : register(u1);   // stacked: xyz normal * 0.5 + 0.5
RWTexture3D<uint>   gAlbedoKeys : register(u2);
RWTexture3D<uint>   gNormalKeys : register(u3);

[numthreads(4, 4, 4)]
void ResolveKeysCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution))
        return;

    const uint albedoKey = gAlbedoKeys[id];
    const uint normalKey = gNormalKeys[id];
    gAlbedoKeys[id] = 0u;
    gNormalKeys[id] = 0u;

    const int3 texel = StackedTexel(int3(id), gUpdateLevel).xyz;
    if (albedoKey == 0u)
    {
        gAlbedoOut[texel] = 0.0f.xxxx;
        gNormalOut[texel] = 0.0f.xxxx;
        return;
    }

    gAlbedoOut[texel] = UnpackVoxelAlbedoKey(albedoKey);
    gNormalOut[texel] = float4(UnpackVoxelNormalKey(normalKey) * 0.5f + 0.5f, 1.0f);
}

[numthreads(4, 4, 4)]
void ClearKeysCS(uint3 id : SV_DispatchThreadID)
{
    // Sized from the texture: it runs when the clipmap is created, before any
    // constants for the new resolution exist.
    uint width, height, depth;
    gAlbedoKeys.GetDimensions(width, height, depth);
    if (id.x >= width || id.y >= height || id.z >= depth)
        return;
    gAlbedoKeys[id] = 0u;
    gNormalKeys[id] = 0u;
}
