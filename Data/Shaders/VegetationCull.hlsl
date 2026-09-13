// VegetationCull.hlsl
// GPU culling and LOD selection for one vegetation layer.
//
// One thread per instance.  Survivors are appended to a compacted index list
// and the matching DrawIndexedInstancedIndirect argument's InstanceCount is
// bumped, so the CPU never learns how many instances survived and never has to
// read anything back.
//
// The visible index list is partitioned by LOD: LOD i owns the range starting
// at gLodOutputBase[i], sized for the worst case where every instance lands in
// that one LOD.  Trading that memory for a fixed layout means the draw for a
// LOD is a contiguous range and needs no indirection beyond this list.

#include "Vegetation_Common.hlsli"

cbuffer VegetationCullConstants : register(b0)
{
    // Frustum planes in world space, each (nx, ny, nz, d), pointing inward.
    float4 gFrustumPlanes[6];

    float3 gCameraPosition;
    float  gCullDistance;

    // Range of gInstances this layer owns.
    uint   gInstanceFirst;
    uint   gInstanceCount;
    uint   gLodCount;
    float  gFadeFraction;

    // Camera distance at which LOD i gives way to LOD i+1.
    float4 gLodDistances;

    // Start of each LOD's slice of the visible index buffer.
    uint4  gLodOutputBase;

    // Byte offset of each LOD's draw arguments within the indirect buffer.
    uint4  gLodArgOffset;

    // Local-space bounding radius of the layer's mesh, before instance scale.
    float  gBoundingRadius;
    float3 _CullPad0;
};

StructuredBuffer<VegetationInstance> gInstances      : register(t0);

RWStructuredBuffer<uint>             gVisibleIndices : register(u0);
// Draw arguments, 5 uints per LOD: IndexCountPerInstance, InstanceCount,
// StartIndexLocation, BaseVertexLocation, StartInstanceLocation.  Only
// InstanceCount is written here; the CPU refreshes the rest each frame.
RWByteAddressBuffer                  gIndirectArgs   : register(u1);

// Sphere-vs-frustum.  Conservative: an instance is kept if it is on the inner
// side of every plane, so a plant straddling the edge is drawn rather than
// popping out at the screen border.
bool IsInsideFrustum(float3 centre, float radius)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        if (dot(gFrustumPlanes[i].xyz, centre) + gFrustumPlanes[i].w < -radius)
            return false;
    }

    return true;
}

// Pick a LOD from camera distance.  Returns gLodCount when the instance is
// past its cull distance and should be dropped entirely.
uint SelectLod(float distance)
{
    for (uint lod = 0; lod < gLodCount - 1; ++lod)
    {
        if (distance < gLodDistances[lod])
            return lod;
    }

    return gLodCount - 1;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint localIndex = dispatchThreadId.x;
    if (localIndex >= gInstanceCount)
        return;

    const uint instanceIndex = gInstanceFirst + localIndex;
    const VegetationInstance instance = gInstances[instanceIndex];

    const float distance = length(instance.Position - gCameraPosition);
    if (distance > gCullDistance)
        return;

    // Scale the bound by the instance so a large tree is not culled using a
    // radius measured on the unscaled mesh.
    const float worldRadius = gBoundingRadius * instance.Scale;
    if (!IsInsideFrustum(instance.Position, worldRadius))
        return;

    // Fade out over the last FadeFraction of the cull distance so instances
    // dissolve at the far edge rather than vanishing.  The pixel shader turns
    // this into a screen-door dither.
    const float fadeStart = gCullDistance * (1.0f - gFadeFraction);
    const float fade = (gFadeFraction > 0.0f && distance > fadeStart)
        ? saturate((gCullDistance - distance) / max(gCullDistance - fadeStart, 1e-4f))
        : 1.0f;

    const uint lod = SelectLod(distance);

    // Claim a slot in this LOD's draw by bumping its InstanceCount.  The
    // returned value is the pre-increment count, which is exactly the slot
    // index within this LOD's slice.
    uint slot;
    gIndirectArgs.InterlockedAdd(gLodArgOffset[lod] + 4, 1, slot);

    // Pack the fade into the high byte.  Instance indices are capped at
    // kVegetationMaxInstances (500k), well inside 24 bits, so this costs
    // nothing and saves a second buffer.
    const uint fadeByte = (uint)(saturate(fade) * 255.0f + 0.5f);
    gVisibleIndices[gLodOutputBase[lod] + slot] = (instanceIndex & 0x00FFFFFFu) | (fadeByte << 24);
}
