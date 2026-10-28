#pragma once

// VirtualGeometryCut
// ------------------
// A view-independent cut through a virtualized mesh's cluster DAG as ordinary indexed
// geometry, for the passes that need a simplified copy of the mesh rather than the
// per-view cut the raster path makes on the GPU: RTGI's BLAS (RtGlobalIllumination) and
// voxel GI's voxelization (EntityMeshRenderer::RenderVoxelization).
//
// Cuts are keyed by error in half-octave steps up from kCutErrorBase (mesh units), so
// instances whose scale and distance ask for about the same error share one cut, and a
// mesh can never collect more than kMaxCutStep + 1 of them.

#include "VirtualGeometryBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VirtualGeometryCut
{
    inline constexpr float        kCutErrorBase = 1.0e-4f;
    inline constexpr std::int16_t kMaxCutStep   = 60;

    // The coarsest step whose error does not exceed `meshError`, or -1 when even
    // the finest step is too coarse, in which case the source triangles are used.
    inline std::int16_t CutStepForError(float meshError)
    {
        if (!(meshError >= kCutErrorBase))
            return -1;
        const float step = std::floor(std::log2(meshError / kCutErrorBase) * 2.0f);
        return static_cast<std::int16_t>(std::clamp(step, 0.0f, static_cast<float>(kMaxCutStep)));
    }

    inline float CutStepError(std::int16_t step)
    {
        return kCutErrorBase * std::exp2(static_cast<float>(step) * 0.5f);
    }

    // A cut through the DAG as ordinary geometry: compacted vertices, triangles
    // grouped by material slot, and the slot of each group.
    struct CutGeometry
    {
        std::vector<Vertex>        Vertices;
        std::vector<std::uint32_t> Indices;
        struct Range { std::uint32_t StartPrimitive, PrimitiveCount, MaterialId, UdimTile; };
        std::vector<Range>         Ranges;
    };

    // The view-independent cut at `error` (mesh units): every cluster whose own
    // error is within it and whose parent's is not. The same test the GPU makes
    // per view with a projected error, so the cut is crack-free for the same
    // reason (VirtualGeometryBuilder.h).
    inline bool ExtractCut(const VirtualGeometry::BuiltMesh& dag, const MeshLod& lod, float error, CutGeometry& out)
    {
        if (dag.SourceVertexCount != lod.Vertices.size())
            return false;
        const std::vector<VirtualGeometry::MaterialSlot> slots = VirtualGeometry::CollectMaterialSlots(lod);

        // Sorted by ParentError: the clusters whose parent is too coarse are a suffix.
        const auto first = std::upper_bound(dag.Clusters.begin(), dag.Clusters.end(), error,
            [](float value, const VirtualGeometry::GpuCluster& cluster) { return value < cluster.ParentError; });

        std::vector<std::vector<std::uint32_t>> slotCorners(slots.size());
        for (auto it = first; it != dag.Clusters.end(); ++it)
        {
            const VirtualGeometry::GpuCluster& cluster = *it;
            if (cluster.LodError > error)
                continue;
            const std::uint32_t vertexCount   = cluster.Packed & 0x7Fu;
            const std::uint32_t triangleCount = (cluster.Packed >> 7) & 0x7Fu;
            const std::uint32_t slot          = (cluster.Packed >> 20) & 0xFFFu;
            if (slot >= slots.size()
                || static_cast<std::size_t>(cluster.VertexOffset) + vertexCount > dag.ClusterVertices.size()
                || static_cast<std::size_t>(cluster.TriangleOffset) + triangleCount > dag.ClusterTriangles.size())
                return false;

            std::vector<std::uint32_t>& corners = slotCorners[slot];
            for (std::uint32_t t = 0; t < triangleCount; ++t)
            {
                const std::uint32_t packed = dag.ClusterTriangles[cluster.TriangleOffset + t];
                for (std::uint32_t corner = 0; corner < 3; ++corner)
                {
                    const std::uint32_t local = (packed >> (corner * 8u)) & 0xFFu;
                    if (local >= vertexCount)
                        return false;
                    corners.push_back(dag.ClusterVertices[cluster.VertexOffset + local]);
                }
            }
        }

        constexpr std::uint32_t kUnmapped = 0xFFFFFFFFu;
        std::vector<std::uint32_t> remap(lod.Vertices.size(), kUnmapped);
        for (std::size_t slot = 0; slot < slots.size(); ++slot)
        {
            const std::vector<std::uint32_t>& corners = slotCorners[slot];
            if (corners.empty())
                continue;
            CutGeometry::Range range{};
            range.StartPrimitive = static_cast<std::uint32_t>(out.Indices.size() / 3);
            range.PrimitiveCount = static_cast<std::uint32_t>(corners.size() / 3);
            range.MaterialId     = slots[slot].MaterialId;
            range.UdimTile       = slots[slot].UdimTile;
            for (std::uint32_t source : corners)
            {
                if (source >= remap.size())
                    return false;
                std::uint32_t& compact = remap[source];
                if (compact == kUnmapped)
                {
                    compact = static_cast<std::uint32_t>(out.Vertices.size());
                    out.Vertices.push_back(lod.Vertices[source]);
                }
                out.Indices.push_back(compact);
            }
            out.Ranges.push_back(range);
        }
        return !out.Indices.empty();
    }
}
