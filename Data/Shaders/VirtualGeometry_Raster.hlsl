// VirtualGeometry_Raster.hlsl
// Rasterises the clusters VirtualGeometry_Cull.hlsl selected.
//
// This file includes GBuffer.hlsl, and the G-Buffer pipeline pairs these front
// ends with that file's pixel shader, compiled from GBuffer.hlsl itself. A
// virtualized mesh therefore shades exactly like an ordinary one - same
// material constants, texture slots, parallax, rain and subsurface encoding -
// and only the way its triangles reach the rasteriser differs.
//
// Two front ends produce identical vertices:
//   VgMeshMain    mesh shader (SM 6.5): one 64-thread group per cluster, one
//                 vertex and up to two triangles per thread.
//   VgVertexMain  fallback for GPUs without mesh shaders: one instance per
//                 cluster and 372 vertices (124 triangle slots), pulled from
//                 the cluster buffers without an index buffer.
//
// Entry points are named so the startup shader precompiler's text scan skips
// this file, and the mesh shader's thread-group attribute is spelled through a
// macro for the same reason: it would otherwise be compiled as a cs_5_0
// compute shader and reported as a failure. VirtualGeometryRenderer compiles
// every entry here itself, with the profile each needs.

#include "GBuffer.hlsl"
#include "VirtualGeometry_Common.hlsli"

// Registers above the G-Buffer's b0-b2, and space1 for buffers, so nothing
// here collides with the material bindings the included shader declares.
cbuffer VgDrawConstants : register(b3)
{
    float4x4 gVgViewProjection;          // this draw's (pre-transposed) view-projection
    float4x4 gVgPreviousViewProjection;  // motion vectors: last frame's
    float3   gVgLightPosition;           // point-shadow faces
    float    gVgLightFarPlane;
    uint     gVgDebugMode;               // VG_DEBUG_*
    uint3    _VgDrawPad;
};

// Which bin this draw reads; set per draw as a root constant.
cbuffer VgBinConstants : register(b4)
{
    uint gVgBin;
};

StructuredBuffer<VgCluster>  gVgRasterClusters : register(t0, space1);
StructuredBuffer<VgInstance> gVgRasterInstances : register(t1, space1);
StructuredBuffer<uint>       gVgClusterVertices : register(t2, space1);
StructuredBuffer<uint>       gVgClusterTriangles : register(t3, space1);
ByteAddressBuffer            gVgVertices : register(t4, space1);
StructuredBuffer<uint2>      gVgVisibleClusters : register(t5, space1);
StructuredBuffer<uint2>      gVgBinRangesRead : register(t6, space1);

static const uint VG_DEBUG_OFF       = 0u;
static const uint VG_DEBUG_CLUSTERS  = 1u;
static const uint VG_DEBUG_INSTANCES = 2u;
static const uint VG_DEBUG_LOD       = 3u;

static const uint VG_VERTEX_STRIDE = 48u;  // System/Mesh.h Vertex

struct VgVertex
{
    float3 Position;
    float3 Normal;
    float2 TexCoord;
    float4 Color;
};

VgVertex VgLoadVertex(uint vertexIndex)
{
    const uint address = vertexIndex * VG_VERTEX_STRIDE;
    VgVertex vertex;
    vertex.Position = asfloat(gVgVertices.Load3(address));
    vertex.Normal   = asfloat(gVgVertices.Load3(address + 12u));
    vertex.TexCoord = asfloat(gVgVertices.Load2(address + 24u));
    vertex.Color    = asfloat(gVgVertices.Load4(address + 32u));
    return vertex;
}

uint3 VgUnpackTriangle(uint packed)
{
    return uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
}

// Stable, well-spread colour per id, for the debug views.
float3 VgHashColour(uint value)
{
    uint h = value * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(h & 0xFFu, (h >> 8) & 0xFFu, (h >> 16) & 0xFFu) * (0.75f / 255.0f) + 0.25f;
}

// DAG depth as a heat ramp: original triangles green, coarser through yellow
// and red towards magenta.
float3 VgDepthColour(uint depth)
{
    const float t = saturate(depth / 10.0f);
    return t < 0.5f
        ? lerp(float3(0.1f, 0.85f, 0.2f), float3(1.0f, 0.85f, 0.1f), t * 2.0f)
        : lerp(float3(1.0f, 0.85f, 0.1f), float3(0.9f, 0.1f, 0.8f), t * 2.0f - 1.0f);
}

float4 VgVertexColour(VgVertex vertex, VgCluster cluster, uint clusterIndex, uint instanceIndex)
{
    switch (gVgDebugMode)
    {
    case VG_DEBUG_CLUSTERS:  return float4(VgHashColour(clusterIndex * 3u + instanceIndex * 7919u), 1.0f);
    case VG_DEBUG_INSTANCES: return float4(VgHashColour(instanceIndex), 1.0f);
    case VG_DEBUG_LOD:       return float4(VgDepthColour(VgClusterDepth(cluster)), 1.0f);
    default:                 return vertex.Color;
    }
}

// Exactly what GBuffer.hlsl's vertex stage produces for an ordinary mesh.
PSInput VgShadeVertex(VgInstance instance, VgCluster cluster, uint clusterIndex, uint instanceIndex, uint vertexIndex)
{
    const VgVertex vertex = VgLoadVertex(vertexIndex);
    const float3 world = VgTransformPoint(instance, vertex.Position);

    PSInput output;
    output.Position      = mul(float4(world, 1.0f), gVgViewProjection);
    output.WorldPosition = world;
    output.WorldNormal   = VgTransformVector(instance, vertex.Normal);
    output.TexCoord      = vertex.TexCoord;
    output.Color         = VgVertexColour(vertex, cluster, clusterIndex, instanceIndex);
    return output;
}

struct VgMotionVertex
{
    float4 Position         : SV_Position;
    float4 CurrentClip      : TEXCOORD0;
    float4 PreviousClip     : TEXCOORD1;
};

VgMotionVertex VgMotionShadeVertex(VgInstance instance, uint vertexIndex)
{
    const float3 local = VgLoadVertex(vertexIndex).Position;

    VgMotionVertex output;
    output.CurrentClip  = mul(float4(VgTransformPoint(instance, local), 1.0f), gVgViewProjection);
    output.PreviousClip = mul(float4(VgTransformPointPrevious(instance, local), 1.0f), gVgPreviousViewProjection);
    output.Position     = output.CurrentClip;
    return output;
}

// ---------------------------------------------------------------------------
// Mesh shader front end
// ---------------------------------------------------------------------------

#define VG_MESH_THREAD_GROUP numthreads(64, 1, 1)

// The cluster a mesh shader group draws, or false for a padding group: the
// group count is rounded up to whole rows of the folded dispatch, so the tail
// of the last row has no cluster. Root descriptors have no bounds checking, so
// nothing past the bin's range may be read. A padding group reports a zeroed
// cluster, whose counts are zero, because SetMeshOutputCounts must be reached
// exactly once and cannot sit behind an early return.
bool VgMeshGroupCluster(uint3 groupId, out uint2 entry, out VgCluster cluster)
{
    entry = uint2(0u, 0u);
    cluster = (VgCluster)0;

    const uint2 range = gVgBinRangesRead[gVgBin];
    const uint item = VgLinearGroup(groupId);
    if (item >= range.y)
        return false;

    entry = gVgVisibleClusters[range.x + item];
    cluster = gVgRasterClusters[entry.y];
    return true;
}

[VG_MESH_THREAD_GROUP]
[outputtopology("triangle")]
void VgMeshMain(
    uint3 groupId     : SV_GroupID,
    uint  threadIndex : SV_GroupIndex,
    out vertices PSInput outVertices[64],
    out indices  uint3   outTriangles[124])
{
    uint2 entry;
    VgCluster cluster;
    const bool drawn = VgMeshGroupCluster(groupId, entry, cluster);
    const uint vertexCount = VgClusterVertexCount(cluster);
    const uint triangleCount = VgClusterTriangleCount(cluster);

    SetMeshOutputCounts(vertexCount, triangleCount);
    if (!drawn)
        return;

    const uint instanceIndex = VgUnpackInstance(entry.x);
    const VgInstance instance = gVgRasterInstances[instanceIndex];

    if (threadIndex < vertexCount)
    {
        outVertices[threadIndex] = VgShadeVertex(instance, cluster, entry.y, instanceIndex,
            gVgClusterVertices[cluster.VertexOffset + threadIndex]);
    }

    for (uint tri = threadIndex; tri < triangleCount; tri += 64u)
        outTriangles[tri] = VgUnpackTriangle(gVgClusterTriangles[cluster.TriangleOffset + tri]);
}

[VG_MESH_THREAD_GROUP]
[outputtopology("triangle")]
void VgMeshMotion(
    uint3 groupId     : SV_GroupID,
    uint  threadIndex : SV_GroupIndex,
    out vertices VgMotionVertex outVertices[64],
    out indices  uint3          outTriangles[124])
{
    uint2 entry;
    VgCluster cluster;
    const bool drawn = VgMeshGroupCluster(groupId, entry, cluster);
    const uint vertexCount = VgClusterVertexCount(cluster);
    const uint triangleCount = VgClusterTriangleCount(cluster);

    SetMeshOutputCounts(vertexCount, triangleCount);
    if (!drawn)
        return;

    const VgInstance instance = gVgRasterInstances[VgUnpackInstance(entry.x)];

    if (threadIndex < vertexCount)
        outVertices[threadIndex] = VgMotionShadeVertex(instance, gVgClusterVertices[cluster.VertexOffset + threadIndex]);

    for (uint tri = threadIndex; tri < triangleCount; tri += 64u)
        outTriangles[tri] = VgUnpackTriangle(gVgClusterTriangles[cluster.TriangleOffset + tri]);
}

// ---------------------------------------------------------------------------
// Vertex shader front end (no mesh shader support)
// ---------------------------------------------------------------------------

// Resolves one of the 372 vertices of an instance to a cluster-vertex index,
// or returns false for a slot past the cluster's triangle count.
bool VgPullVertex(uint vertexId, uint instanceId, out uint2 entry, out uint vertexIndex)
{
    const uint2 range = gVgBinRangesRead[gVgBin];
    entry = gVgVisibleClusters[range.x + instanceId];
    vertexIndex = 0u;

    const VgCluster cluster = gVgRasterClusters[entry.y];
    const uint tri = vertexId / 3u;
    if (tri >= VgClusterTriangleCount(cluster))
        return false;

    const uint corner = vertexId - tri * 3u;
    const uint local = (gVgClusterTriangles[cluster.TriangleOffset + tri] >> (corner * 8u)) & 0xFFu;
    vertexIndex = gVgClusterVertices[cluster.VertexOffset + local];
    return true;
}

// Behind the near plane, so an unused triangle slot is clipped away whole.
static const float4 VG_CLIPPED_POSITION = float4(0.0f, 0.0f, -1.0f, 1.0f);

PSInput VgVertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint2 entry;
    uint vertexIndex;
    if (!VgPullVertex(vertexId, instanceId, entry, vertexIndex))
    {
        PSInput unused = (PSInput)0;
        unused.Position = VG_CLIPPED_POSITION;
        return unused;
    }

    const uint instanceIndex = VgUnpackInstance(entry.x);
    return VgShadeVertex(gVgRasterInstances[instanceIndex], gVgRasterClusters[entry.y], entry.y, instanceIndex, vertexIndex);
}

VgMotionVertex VgVertexMotion(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint2 entry;
    uint vertexIndex;
    if (!VgPullVertex(vertexId, instanceId, entry, vertexIndex))
    {
        VgMotionVertex unused = (VgMotionVertex)0;
        unused.Position = VG_CLIPPED_POSITION;
        return unused;
    }

    return VgMotionShadeVertex(gVgRasterInstances[VgUnpackInstance(entry.x)], vertexIndex);
}

// ---------------------------------------------------------------------------
// Pixel shaders the G-Buffer file does not already provide
// ---------------------------------------------------------------------------

// Debug views: flat cluster / instance / LOD colour, lit by the ordinary
// deferred pass so the shape still reads.
PSOutput VgPixelDebug(PSInput input)
{
    const float3 normal = normalize(input.WorldNormal);

    PSOutput output;
    output.Albedo   = float4(input.Color.rgb, 1.0f);
    output.Normal   = float4(EncodeOctNormal(normal), input.Position.z,
                             PteroEncodeSurfaceSpecularAndSubsurface(0.5f, 0u));
    output.Material = float4(0.6f, 0.0f, 1.0f, 0.0f);
    return output;
}

// Point-light shadow faces store linear distance to the light, like
// PointShadowDepth.hlsl.
float VgPixelPointShadow(PSInput input) : SV_Depth
{
    const float distanceToLight = distance(input.WorldPosition, gVgLightPosition);
    return saturate(distanceToLight / max(gVgLightFarPlane, 1e-4f));
}

float2 VgClipToUv(float4 clip)
{
    const float invW = rcp(max(abs(clip.w), 1e-6f));
    float2 uv = clip.xy * invW;
    uv.y = -uv.y;
    return uv * 0.5f + 0.5f;
}

// Screen-space motion, identical to MotionVectors.hlsl.
float4 VgPixelMotion(VgMotionVertex input) : SV_Target
{
    const float2 currentUv = VgClipToUv(input.CurrentClip);
    const float2 previousUv = VgClipToUv(input.PreviousClip);
    return float4(currentUv - previousUv, 0.0f, 0.0f);
}
