#pragma once

// VirtualGeometryBuilder
// ----------------------
// Turns one mesh asset into what the virtualized-geometry renderer draws: a DAG
// of small triangle clusters, built the way Nanite builds its hierarchy
// (Brian Karis, "Nanite: A Deep Dive", 2021):
//
//   1. Split the mesh into clusters of at most 64 vertices / 124 triangles.
//   2. Partition neighbouring clusters into groups, merge each group, simplify
//      it to half its triangles with the group's outer boundary locked, and
//      split the result into new clusters.
//   3. Repeat on the new clusters until one cluster is left.
//
// Each group records the error its simplification introduced, measured in mesh
// units. A cluster is drawn when the group it belongs to is too coarse for the
// view (its projected error is over the threshold) and the group it was
// simplified from is fine enough. Because every group's boundary was locked
// while it was simplified, that purely local test picks a crack-free cut
// through the DAG, so each cluster can decide its own detail on the GPU with
// no knowledge of its neighbours.
//
// The clustering and simplification are meshoptimizer's (clusterlod.h). This
// file adapts them to the engine's Vertex layout and material slots, lays the
// result out for the GPU, and caches it on disk: the build of a multi-million
// triangle scan takes seconds, and the cache makes that a one-time cost.

#include "System/Mesh.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VirtualGeometry
{
    // Cluster size. 64 vertices / 124 triangles fills a 64-thread mesh shader
    // group exactly (one vertex and up to two triangles per thread) and is the
    // size NVIDIA and AMD both recommend for mesh shader throughput.
    inline constexpr std::uint32_t kMaxClusterVertices  = 64;
    inline constexpr std::uint32_t kMaxClusterTriangles = 124;

    // A mesh this small gains nothing from clustering, and a DAG over it would
    // be a single cluster. The renderer draws it the ordinary way instead.
    inline constexpr std::uint32_t kMinSourceTriangles = kMaxClusterTriangles * 2;

    // Material slots a virtualized mesh may have; bounded by the bits the
    // cluster record keeps for the slot.
    inline constexpr std::uint32_t kMaxMaterialSlots = 4096;

    // Bump whenever the builder's output changes, so every disk-cache entry
    // written by an older builder is rebuilt rather than trusted.
    inline constexpr std::uint32_t kBuilderVersion = 2;

    // One run of triangles that draws with a single material binding: a
    // sub-mesh, or one UDIM tile of it when its triangles were grouped by tile
    // at load (Mesh::BuildUdimTiles). Every cluster belongs to exactly one, and
    // the renderer binds each slot's material, texture tile included, per bin.
    struct MaterialSlot
    {
        std::uint32_t IndexStart = 0;
        std::uint32_t IndexCount = 0;
        std::uint32_t MaterialId = 0;
        // UDIM number (1001...), or 0 when the sub-mesh was not tiled.
        std::uint32_t UdimTile = 0;
    };

    // The slots of a mesh's first LOD, in the order cluster records number them.
    std::vector<MaterialSlot> CollectMaterialSlots(const MeshLod& lod);

    // One cluster exactly as the GPU reads it; mirrors VgCluster in
    // Data/Shaders/VirtualGeometry_Common.hlsli. All spheres are in mesh space.
    struct GpuCluster
    {
        // Tight bounds of the cluster's own triangles, for frustum and
        // occlusion culling.
        DirectX::XMFLOAT4 CullSphere{};
        // Bounds the cluster's own error was measured over: the simplified
        // bounds of the finer group this cluster was produced from. Error 0
        // (the original triangles) always passes the "fine enough" half of the
        // test, whatever the sphere.
        DirectX::XMFLOAT4 LodSphere{};
        // Bounds of the group this cluster belongs to, i.e. of the coarser
        // clusters that replace it.
        DirectX::XMFLOAT4 ParentSphere{};
        // Backface cone (meshopt_computeClusterBounds): the cluster faces away
        // from the camera when dot(normalize(apex - camera), axis) >= cutoff.
        // A cutoff above 1 disables the test for this cluster.
        DirectX::XMFLOAT3 ConeApex{};
        float             ConeCutoff = 2.0f;
        DirectX::XMFLOAT3 ConeAxis{};
        float             LodError = 0.0f;
        // First entry of this cluster in ClusterVertices / ClusterTriangles.
        std::uint32_t     VertexOffset = 0;
        std::uint32_t     TriangleOffset = 0;
        // vertexCount | triangleCount << 7 | dagDepth << 14 | materialSlot << 20
        std::uint32_t     Packed = 0;
        // Error of the coarser group that replaces this cluster; FLT_MAX when
        // nothing does (the DAG root, or a group simplification got stuck on).
        float             ParentError = 0.0f;
    };
    static_assert(sizeof(GpuCluster) == 96, "GpuCluster must match VgCluster in VirtualGeometry_Common.hlsli");

    inline std::uint32_t PackClusterCounts(
        std::uint32_t vertexCount, std::uint32_t triangleCount, std::uint32_t depth, std::uint32_t materialSlot)
    {
        return (vertexCount & 0x7Fu)
            | ((triangleCount & 0x7Fu) << 7)
            | ((depth & 0x3Fu) << 14)
            | ((materialSlot & 0xFFFu) << 20);
    }

    struct BuiltMesh
    {
        // Sorted by ParentError, ascending. A cluster can only be drawn while
        // its parent's projected error is over the threshold, and the renderer
        // knows a lower bound on the distance to every cluster of an instance,
        // so for any instance the drawable clusters are a suffix of this array
        // it can find by binary search. For a distant instance of a huge mesh
        // that skips all but a handful of coarse clusters before any per-cluster
        // work is done.
        std::vector<GpuCluster>    Clusters;
        // Source vertex index of each cluster-local vertex.
        std::vector<std::uint32_t> ClusterVertices;
        // One entry per triangle: three 8-bit cluster-local vertex indices.
        std::vector<std::uint32_t> ClusterTriangles;

        // Encloses every cull, LOD and parent sphere above, which is what makes
        // it a valid lower bound on the distance to any of them.
        DirectX::XMFLOAT4 BoundsSphere{};

        std::uint32_t SourceVertexCount   = 0;
        std::uint32_t SourceTriangleCount = 0;
        // CollectMaterialSlots(lod).size().
        std::uint32_t MaterialSlotCount   = 0;
        // Levels in the DAG (the deepest group's depth + 1).
        std::uint32_t DagDepth            = 0;
    };

    struct BuildResult
    {
        std::shared_ptr<const BuiltMesh> Mesh;
        std::string Error;
        bool   FromCache = false;
        double Seconds   = 0.0;
    };

    // Builds the cluster DAG for the first LOD of a mesh, or loads it from the
    // disk cache when an entry for identical source data exists. A pure
    // function of `lod`, so it is safe to run on a worker thread while the
    // renderer keeps drawing the mesh the ordinary way.
    BuildResult BuildOrLoad(const MeshLod& lod);

    // Cheap test of whether BuildOrLoad could succeed, so an entity can fall
    // back to ordinary rendering without a build being attempted.
    bool CanVirtualize(const MeshLod& lod, std::string* reason = nullptr);
}
