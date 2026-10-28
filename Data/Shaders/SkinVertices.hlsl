// SkinVertices.hlsl - EntityMeshRenderer's GPU skinning (EntityMeshSkinning.cpp).
//
// One thread per vertex of a skinned mesh's LOD 0: reads the bind-pose vertex and its four
// joint influences, blends the joints' skinning matrices and writes the posed vertex into
// the entity's own vertex buffer, which every geometry pass then draws instead of the
// mesh's. Root descriptors only - no descriptor heap slots.
//
// Layouts (all must match the C++ side):
//   gBindVertices - System/Mesh.h Vertex, 48 bytes: float3 position, float3 normal,
//                   float2 uv, float4 colour.
//   gInfluences   - MeshSkinVertex, 24 bytes: four uint16 joints, four float weights.
//   gPalette      - four float4 rows per joint: the rows of an XMFLOAT4X4 skinning matrix
//                   (row vectors, translation in row 3), not transposed.
// A vertex whose weights are all zero is not moved by any joint and keeps its bind pose.

cbuffer SkinConstants : register(b0)
{
    uint gVertexCount;
    uint gJointCount;
    uint2 gPad;
};

ByteAddressBuffer gBindVertices : register(t0);
ByteAddressBuffer gInfluences : register(t1);
StructuredBuffer<float4> gPalette : register(t2);
RWByteAddressBuffer gOutput : register(u0);

static const uint kVertexStride = 48;
static const uint kInfluenceStride = 24;

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint vertex = dispatchId.x;
    if (vertex >= gVertexCount)
        return;

    const uint base = vertex * kVertexStride;
    const float3 position = asfloat(gBindVertices.Load3(base));
    const float3 normal = asfloat(gBindVertices.Load3(base + 12));
    const uint4 uvAndColorXY = gBindVertices.Load4(base + 24);
    const uint2 colorZW = gBindVertices.Load2(base + 40);

    const uint influenceBase = vertex * kInfluenceStride;
    const uint2 packedJoints = gInfluences.Load2(influenceBase);
    const float4 weights = asfloat(gInfluences.Load4(influenceBase + 8));
    const uint4 joints = uint4(packedJoints.x & 0xffffu, packedJoints.x >> 16, packedJoints.y & 0xffffu, packedJoints.y >> 16);

    float3 skinnedPosition = position;
    float3 skinnedNormal = normal;
    if (weights.x + weights.y + weights.z + weights.w > 0.0f && gJointCount > 0)
    {
        skinnedPosition = 0.0f;
        skinnedNormal = 0.0f;
        [unroll]
        for (uint k = 0; k < 4; ++k)
        {
            const float weight = weights[k];
            if (weight <= 0.0f)
                continue;
            const uint row = min(joints[k], gJointCount - 1) * 4;
            const float3 r0 = gPalette[row + 0].xyz;
            const float3 r1 = gPalette[row + 1].xyz;
            const float3 r2 = gPalette[row + 2].xyz;
            const float3 r3 = gPalette[row + 3].xyz;
            skinnedPosition += weight * (position.x * r0 + position.y * r1 + position.z * r2 + r3);
            // Bones are rigid or uniformly scaled, so the upper 3x3 is good enough for normals.
            skinnedNormal += weight * (normal.x * r0 + normal.y * r1 + normal.z * r2);
        }
        const float normalLength = length(skinnedNormal);
        skinnedNormal = normalLength > 1e-8f ? skinnedNormal / normalLength : normal;
    }

    gOutput.Store3(base, asuint(skinnedPosition));
    gOutput.Store3(base + 12, asuint(skinnedNormal));
    gOutput.Store4(base + 24, uvAndColorXY);
    gOutput.Store2(base + 40, colorZW);
}
