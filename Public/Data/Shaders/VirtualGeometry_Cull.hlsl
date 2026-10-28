// VirtualGeometry_Cull.hlsl
// GPU-driven culling and LOD selection for virtualized geometry.
//
// Per frame, for every view at once (camera, sun shadow, shadow-casting point lights):
//
//   VgInstanceCull     one thread per (instance, view): frustum-tests the instance and
//                      emits 64-cluster work chunks for only the clusters its distance
//                      could possibly select (the cluster pool is sorted by parent error,
//                      so those are a suffix found by binary search).
//   VgChunkArgs        turns the chunk count into VgClusterCull's dispatch size.
//   VgClusterCull      one thread per cluster: DAG cut, frustum, backface cone, and for
//                      the camera an occlusion test against last frame's HZB. Survivors
//                      become candidates, counted per bin (material, or shadow view);
//                      clusters that look occluded are deferred.
//   VgBuildBins        prefix-sums the bin counts into ranges and writes one indirect
//                      draw per bin.
//   VgScatter          moves every candidate into its bin's range.
//
// Then, once the camera's first batch is drawn, the occluded clusters get a second
// chance against an HZB built from this frame's depth (Nanite's two-pass occlusion):
//
//   VgHzbFromDepth / VgHzbDownsample   build the HZB.
//   VgOcclusionArgs / VgOcclusionRecull / VgBuildBins / VgScatter   phase two.
//
// Everything stays on the GPU; the CPU never learns a count, so nothing stalls.
// Kept to shader model 5 features so the startup shader cache compiles it as it
// guesses (cs_5_0).

#include "VirtualGeometry_Common.hlsli"

static const uint VG_MAX_CLUSTER_TRIANGLES = 124u;

// Nearest distance VG_VIEW_POINT_LOD measures from the light; matches the virtual
// shadow map's local-light near plane.
static const float VG_POINT_LOD_ZNEAR = 0.05f;

// Byte offsets into the counter buffer. Mirrors VirtualGeometryRenderer::Counter.
static const uint VG_COUNTER_CHUNKS               = 0u;
static const uint VG_COUNTER_CANDIDATES           = 4u;
static const uint VG_COUNTER_OCCLUDED             = 8u;
static const uint VG_COUNTER_PHASE1_CANDIDATE_END = 12u;
static const uint VG_COUNTER_PHASE1_VISIBLE_END   = 16u;
static const uint VG_COUNTER_SCATTER_START        = 20u;
static const uint VG_COUNTER_SCATTER_END          = 24u;
static const uint VG_COUNTER_MAIN_CLUSTERS        = 28u;
static const uint VG_COUNTER_MAIN_TRIANGLES       = 32u;
static const uint VG_COUNTER_SHADOW_CLUSTERS      = 36u;
static const uint VG_COUNTER_OVERFLOW             = 40u;
static const uint VG_COUNTER_RECOVERED_CLUSTERS   = 44u;

// Overflow bits, reported to the editor so a budget that is too small is visible.
static const uint VG_OVERFLOW_CHUNKS     = 1u;
static const uint VG_OVERFLOW_CANDIDATES = 2u;
static const uint VG_OVERFLOW_OCCLUDED   = 4u;

// Byte offsets of the indirect dispatch arguments this file writes for itself.
static const uint VG_DISPATCH_CLUSTER_CULL = 0u;
static const uint VG_DISPATCH_SCATTER      = 16u;
static const uint VG_DISPATCH_RECULL       = 32u;

// Cull flags.
static const uint VG_CULL_HZB_VALID   = 1u;  // the bound HZB may be used for occlusion
static const uint VG_CULL_MESH_SHADER = 2u;  // draw arguments are DispatchMesh, not DrawInstanced

cbuffer VgCullConstants : register(b0)
{
    float4x4 gHzbViewProjection;  // the (pre-transposed) view-projection the HZB was rendered with
    float3   gLodCameraPosition;
    float    gLodFactor;          // (viewport height / 2) / tan(fovY / 2) / pixel error threshold
    float3   gConeCameraPosition;
    float    gLodZNear;
    uint     gInstanceCount;
    uint     gViewCount;
    uint     gPhase;              // 0 = first pass, 1 = occlusion second pass
    uint     gFlags;
    float2   gHzbSize;            // level 0, in texels
    uint     gHzbMipCount;
    uint     gBinCount;           // bins VgBuildBins lays out in this phase
    uint     gChunkCapacity;
    uint     gCandidateCapacity;
    uint     gOccludedCapacity;
    uint     gMaxBins;            // stride of one phase in the bin arrays
    uint2    gHzbSrcSize;         // HZB build: size of the level being read
    uint2    gHzbDstSize;         // HZB build: size of the level being written
};

StructuredBuffer<VgCluster>    gVgClusters     : register(t0);
StructuredBuffer<VgAsset>      gVgAssets       : register(t1);
StructuredBuffer<VgInstance>   gVgInstances    : register(t2);
StructuredBuffer<VgView>       gVgViews        : register(t3);
StructuredBuffer<uint>         gVgBinTable     : register(t4);
Texture2D<float>               gVgHzb          : register(t5);
Texture2D<float>               gVgDepth        : register(t6);

RWByteAddressBuffer            gVgCounters     : register(u0);
RWStructuredBuffer<uint2>      gVgChunks       : register(u1);
RWStructuredBuffer<uint4>      gVgCandidates   : register(u2);
RWStructuredBuffer<uint2>      gVgOccluded     : register(u3);
RWByteAddressBuffer            gVgBinCounts    : register(u4);
RWStructuredBuffer<uint2>      gVgBinRanges    : register(u5);
RWByteAddressBuffer            gVgDrawArgs     : register(u6);
RWByteAddressBuffer            gVgDispatchArgs : register(u7);
RWStructuredBuffer<uint2>      gVgVisible      : register(u8);
RWTexture2D<float>             gVgHzbSrc       : register(u9);
RWTexture2D<float>             gVgHzbDst       : register(u10);

uint3 VgGroupsFor(uint groupCount)
{
    return uint3(min(groupCount, VG_DISPATCH_WIDTH), (groupCount + VG_DISPATCH_WIDTH - 1u) / VG_DISPATCH_WIDTH, 1u);
}

// Records one surviving cluster for its bin. The candidate slot is claimed
// before the bin is counted, so a full buffer drops the cluster cleanly instead
// of leaving a counted hole in a bin that the draw would read garbage from.
void VgEmit(uint packedInstanceView, VgInstance instance, VgCluster cluster, uint clusterIndex, VgView view, uint phase)
{
    uint bin = view.Bin;
    if (bin == VG_MATERIAL_BINNED)
        bin = gVgBinTable[instance.BinTableOffset + VgClusterMaterialSlot(cluster)];
    if (bin >= gBinCount)
        return;

    uint candidate;
    gVgCounters.InterlockedAdd(VG_COUNTER_CANDIDATES, 1u, candidate);
    if (candidate >= gCandidateCapacity)
    {
        gVgCounters.InterlockedOr(VG_COUNTER_OVERFLOW, VG_OVERFLOW_CANDIDATES);
        return;
    }

    uint slot;
    gVgBinCounts.InterlockedAdd((phase * gMaxBins + bin) * 4u, 1u, slot);
    gVgCandidates[candidate] = uint4(packedInstanceView, clusterIndex, bin, slot);

    // View 0 is always the camera.
    if (VgUnpackView(packedInstanceView) == 0u)
    {
        gVgCounters.InterlockedAdd(VG_COUNTER_MAIN_CLUSTERS, 1u);
        gVgCounters.InterlockedAdd(VG_COUNTER_MAIN_TRIANGLES, VgClusterTriangleCount(cluster));
        if (phase != 0u)
            gVgCounters.InterlockedAdd(VG_COUNTER_RECOVERED_CLUSTERS, 1u);
    }
    else
    {
        gVgCounters.InterlockedAdd(VG_COUNTER_SHADOW_CLUSTERS, 1u);
    }
}

// ---------------------------------------------------------------------------
// Instance pass
// ---------------------------------------------------------------------------

[numthreads(64, 1, 1)]
void VgInstanceCull(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    const uint instanceIndex = dispatchId.x;
    const uint viewIndex = groupId.y;
    if (instanceIndex >= gInstanceCount || viewIndex >= gViewCount)
        return;

    const VgInstance instance = gVgInstances[instanceIndex];
    const VgAsset asset = gVgAssets[instance.AssetIndex];
    const VgView view = gVgViews[viewIndex];

    const float3 centre = VgTransformPoint(instance, asset.BoundsSphere.xyz);
    const float radius = asset.BoundsSphere.w * instance.MaxScale;
    if (!VgSphereInView(view, centre, radius))
        return;

    // A cluster is only drawn while the group replacing it projects to more
    // than a pixel of error. Nothing of this instance is nearer than dNear, so
    // any cluster whose parent error could not reach a pixel even at dNear is
    // unreachable from this viewpoint. The pool is sorted by parent error, so
    // those are exactly a prefix to skip. The small margin keeps rounding from
    // skipping a cluster the per-cluster test would have accepted.
    float minimumParentError;
    if ((view.Flags & VG_VIEW_TEXEL_LOD) != 0)
    {
        minimumParentError = 0.999f * view.LodTexelSize / instance.MaxScale;
    }
    else if ((view.Flags & VG_VIEW_POINT_LOD) != 0)
    {
        const float nearestToLight = max(length(centre - view.LightSphere.xyz) - radius, VG_POINT_LOD_ZNEAR);
        minimumParentError = 0.999f * nearestToLight * view.LodTexelSize / instance.MaxScale;
    }
    else
    {
        const float nearest = max(length(centre - gLodCameraPosition) - radius, gLodZNear);
        minimumParentError = 0.999f * nearest / (gLodFactor * instance.MaxScale);
    }

    uint lo = 0u;
    uint hi = asset.ClusterCount;
    [loop]
    while (lo < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (gVgClusters[asset.ClusterOffset + mid].ParentError > minimumParentError)
            hi = mid;
        else
            lo = mid + 1u;
    }

    const uint remaining = asset.ClusterCount - lo;
    if (remaining == 0u)
        return;

    const uint chunkCount = (remaining + 63u) / 64u;
    uint firstChunk;
    gVgCounters.InterlockedAdd(VG_COUNTER_CHUNKS, chunkCount, firstChunk);

    const uint packed = VgPackInstanceView(instanceIndex, viewIndex);
    [loop]
    for (uint chunk = 0u; chunk < chunkCount; ++chunk)
    {
        if (firstChunk + chunk >= gChunkCapacity)
        {
            gVgCounters.InterlockedOr(VG_COUNTER_OVERFLOW, VG_OVERFLOW_CHUNKS);
            break;
        }
        gVgChunks[firstChunk + chunk] = uint2(packed, lo + chunk * 64u);
    }
}

[numthreads(1, 1, 1)]
void VgChunkArgs()
{
    const uint chunks = min(gVgCounters.Load(VG_COUNTER_CHUNKS), gChunkCapacity);
    gVgDispatchArgs.Store3(VG_DISPATCH_CLUSTER_CULL, VgGroupsFor(chunks));
}

// ---------------------------------------------------------------------------
// Cluster pass
// ---------------------------------------------------------------------------

[numthreads(64, 1, 1)]
void VgClusterCull(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    const uint chunkIndex = VgLinearGroup(groupId);
    if (chunkIndex >= min(gVgCounters.Load(VG_COUNTER_CHUNKS), gChunkCapacity))
        return;

    const uint2 chunk = gVgChunks[chunkIndex];
    const VgInstance instance = gVgInstances[VgUnpackInstance(chunk.x)];
    const VgAsset asset = gVgAssets[instance.AssetIndex];

    const uint localCluster = chunk.y + threadIndex;
    if (localCluster >= asset.ClusterCount)
        return;

    const uint clusterIndex = asset.ClusterOffset + localCluster;
    const VgCluster cluster = gVgClusters[clusterIndex];

    const VgView view = gVgViews[VgUnpackView(chunk.x)];

    // Every view but a virtual shadow map page uses the camera's cut, so a shadow
    // is cast by exactly the triangles the camera sees and never by more detail
    // than it can show. A page is cached across camera moves, so it cuts at its
    // own texel size instead - which is about the camera's pixel size wherever
    // the page is read, since that is how the lighting picks the page's level.
    if ((view.Flags & VG_VIEW_TEXEL_LOD) != 0)
    {
        if (!VgClusterLodSelectedTexel(cluster, instance, view.LodTexelSize))
            return;
    }
    else if ((view.Flags & VG_VIEW_POINT_LOD) != 0)
    {
        // A local light's cube face: the same pixel-error cut, seen from the light
        // with one texel as the pixel.
        if (!VgClusterLodSelected(cluster, instance, view.LightSphere.xyz, 1.0f / max(view.LodTexelSize, 1e-9f), VG_POINT_LOD_ZNEAR))
            return;
    }
    else if (!VgClusterLodSelected(cluster, instance, gLodCameraPosition, gLodFactor, gLodZNear))
    {
        return;
    }
    const float radius = cluster.CullSphere.w * instance.MaxScale;
    if (!VgSphereInView(view, VgTransformPoint(instance, cluster.CullSphere.xyz), radius))
        return;

    if ((view.Flags & VG_VIEW_CONE) != 0 && VgClusterBackfacing(cluster, instance, gConeCameraPosition))
        return;

    if ((view.Flags & VG_VIEW_OCCLUSION) != 0 && (gFlags & VG_CULL_HZB_VALID) != 0)
    {
        // Last frame's HZB describes last frame's scene, so the cluster is
        // tested where it was then.
        const float3 previousCentre = VgTransformPointPrevious(instance, cluster.CullSphere.xyz);
        if (VgSphereOccluded(gVgHzb, gHzbSize, gHzbMipCount, gHzbViewProjection, previousCentre, radius))
        {
            uint deferred;
            gVgCounters.InterlockedAdd(VG_COUNTER_OCCLUDED, 1u, deferred);
            if (deferred < gOccludedCapacity)
            {
                gVgOccluded[deferred] = uint2(chunk.x, clusterIndex);
                return;
            }
            // No room to defer it: draw it now rather than lose it.
            gVgCounters.InterlockedOr(VG_COUNTER_OVERFLOW, VG_OVERFLOW_OCCLUDED);
        }
    }

    VgEmit(chunk.x, instance, cluster, clusterIndex, view, 0u);
}

// ---------------------------------------------------------------------------
// Occlusion second pass
// ---------------------------------------------------------------------------

[numthreads(1, 1, 1)]
void VgOcclusionArgs()
{
    const uint deferred = min(gVgCounters.Load(VG_COUNTER_OCCLUDED), gOccludedCapacity);
    gVgDispatchArgs.Store3(VG_DISPATCH_RECULL, VgGroupsFor((deferred + 63u) / 64u));
}

[numthreads(64, 1, 1)]
void VgOcclusionRecull(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    const uint index = VgLinearGroup(groupId) * 64u + threadIndex;
    if (index >= min(gVgCounters.Load(VG_COUNTER_OCCLUDED), gOccludedCapacity))
        return;

    const uint2 entry = gVgOccluded[index];
    const VgInstance instance = gVgInstances[VgUnpackInstance(entry.x)];
    const VgCluster cluster = gVgClusters[entry.y];
    const VgView view = gVgViews[VgUnpackView(entry.x)];

    // This frame's HZB, built from what the first pass drew, at this frame's position.
    const float3 centre = VgTransformPoint(instance, cluster.CullSphere.xyz);
    const float radius = cluster.CullSphere.w * instance.MaxScale;
    if (VgSphereOccluded(gVgHzb, gHzbSize, gHzbMipCount, gHzbViewProjection, centre, radius))
        return;

    VgEmit(entry.x, instance, cluster, entry.y, view, 1u);
}

// ---------------------------------------------------------------------------
// Binning
// ---------------------------------------------------------------------------

// One thread; the bin count is at most a few hundred, so a serial prefix sum
// costs microseconds and needs no shared-memory scan.
[numthreads(1, 1, 1)]
void VgBuildBins()
{
    const uint candidateEnd = min(gVgCounters.Load(VG_COUNTER_CANDIDATES), gCandidateCapacity);

    uint visibleStart = 0u;
    uint candidateStart = 0u;
    if (gPhase != 0u)
    {
        visibleStart = gVgCounters.Load(VG_COUNTER_PHASE1_VISIBLE_END);
        candidateStart = gVgCounters.Load(VG_COUNTER_PHASE1_CANDIDATE_END);
    }

    uint running = visibleStart;
    [loop]
    for (uint bin = 0u; bin < gBinCount; ++bin)
    {
        const uint slot = gPhase * gMaxBins + bin;
        const uint count = gVgBinCounts.Load(slot * 4u);
        gVgBinRanges[slot] = uint2(running, count);

        uint4 args;
        if ((gFlags & VG_CULL_MESH_SHADER) != 0)
        {
            // DispatchMesh: one group per cluster, folded into two dimensions.
            args = uint4(VgGroupsFor(count), 0u);
        }
        else
        {
            // DrawInstanced: one instance per cluster, three vertices per
            // triangle slot; the vertex shader collapses slots past the
            // cluster's triangle count.
            args = uint4(VG_MAX_CLUSTER_TRIANGLES * 3u, count, 0u, 0u);
        }
        gVgDrawArgs.Store4(slot * 16u, args);
        running += count;
    }

    if (gPhase == 0u)
    {
        gVgCounters.Store(VG_COUNTER_PHASE1_VISIBLE_END, running);
        gVgCounters.Store(VG_COUNTER_PHASE1_CANDIDATE_END, candidateEnd);
    }

    gVgCounters.Store(VG_COUNTER_SCATTER_START, candidateStart);
    gVgCounters.Store(VG_COUNTER_SCATTER_END, candidateEnd);
    gVgDispatchArgs.Store3(VG_DISPATCH_SCATTER,
        VgGroupsFor((max(candidateEnd, candidateStart) - candidateStart + 63u) / 64u));
}

[numthreads(64, 1, 1)]
void VgScatter(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex)
{
    const uint index = gVgCounters.Load(VG_COUNTER_SCATTER_START) + VgLinearGroup(groupId) * 64u + threadIndex;
    if (index >= gVgCounters.Load(VG_COUNTER_SCATTER_END))
        return;

    const uint4 candidate = gVgCandidates[index];
    const uint2 range = gVgBinRanges[gPhase * gMaxBins + candidate.z];
    gVgVisible[range.x + candidate.w] = candidate.xy;
}

// ---------------------------------------------------------------------------
// Hierarchical Z
// ---------------------------------------------------------------------------

// Level 0 is the largest power of two that fits the depth buffer, so each of
// its texels covers between one and two depth texels per axis. Taking the
// maximum over the exact footprint keeps the HZB conservative, which a single
// bilinear max-reduction sample would not be at a non-power-of-two size.
[numthreads(8, 8, 1)]
void VgHzbFromDepth(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= gHzbDstSize))
        return;

    const float2 footprint = float2(gHzbSrcSize) / float2(gHzbDstSize);
    const uint2 first = uint2(floor(float2(dispatchId.xy) * footprint));
    const uint2 last = min(uint2(ceil(float2(dispatchId.xy + 1u) * footprint)), gHzbSrcSize);

    float farthest = 0.0f;
    [loop]
    for (uint y = first.y; y < last.y; ++y)
    {
        [loop]
        for (uint x = first.x; x < last.x; ++x)
            farthest = max(farthest, gVgDepth.Load(int3(x, y, 0)));
    }
    gVgHzbDst[dispatchId.xy] = farthest;
}

[numthreads(8, 8, 1)]
void VgHzbDownsample(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId.xy >= gHzbDstSize))
        return;

    const uint2 source = dispatchId.xy * 2u;
    const uint2 edge = gHzbSrcSize - 1u;
    float farthest = gVgHzbSrc[min(source, edge)];
    farthest = max(farthest, gVgHzbSrc[min(source + uint2(1u, 0u), edge)]);
    farthest = max(farthest, gVgHzbSrc[min(source + uint2(0u, 1u), edge)]);
    farthest = max(farthest, gVgHzbSrc[min(source + uint2(1u, 1u), edge)]);
    gVgHzbDst[dispatchId.xy] = farthest;
}
