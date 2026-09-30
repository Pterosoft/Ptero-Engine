// Compiled without the precompiled header: pch.h pulls in windows.h without
// NOMINMAX, and clusterlod.h calls std::max unparenthesised.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "VirtualGeometryBuilder.h"

#include "System/DataFiles.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "..\SDKs\meshoptimizer-1.3\src\meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "..\SDKs\meshoptimizer-1.3\demo\clusterlod.h"

namespace VirtualGeometry
{
namespace
{
    static_assert(sizeof(Vertex) == 48, "The builder reads Vertex by stride; update the attribute offsets below.");
    static_assert(offsetof(Vertex, Normal) == 12 && offsetof(Vertex, TexCoord) == 24 && offsetof(Vertex, Color) == 32,
        "The attribute protect mask below assumes Normal, TexCoord, Color in that order.");

    // Attributes are read starting at Vertex::Normal, one float each:
    //   0-2 normal, 3-4 texcoord, 5-8 colour.
    // Only the normal steers simplification (weighted); the texcoord and
    // colour bits only protect their seams. Without that protection the
    // permissive simplifier clodDefaultConfig enables would happily collapse a
    // UV seam and smear a texture across it.
    constexpr float kNormalWeights[3] = { 0.5f, 0.5f, 0.5f };
    constexpr unsigned int kSeamProtectMask =
        (1u << 3) | (1u << 4)                           // texcoord
        | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8); // colour

    using SlotRange = MaterialSlot;

    // ------------------------------------------------------------------
    // Disk cache
    // ------------------------------------------------------------------

    // Word-at-a-time multiplicative hash. Not cryptographic; it only has to
    // tell apart meshes that differ, and it runs over the whole vertex and
    // index buffer on every load, so speed matters more than pedigree.
    struct Hasher
    {
        std::uint64_t State = 0xCBF29CE484222325ull;

        void Mix(std::uint64_t word)
        {
            State ^= word;
            State *= 0x9E3779B97F4A7C15ull;
            State ^= State >> 29;
        }

        void Bytes(const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            std::size_t offset = 0;
            for (; offset + 8 <= size; offset += 8)
            {
                std::uint64_t word;
                std::memcpy(&word, bytes + offset, 8);
                Mix(word);
            }

            std::uint64_t tail = 0;
            std::memcpy(&tail, bytes + offset, size - offset);
            Mix(tail ^ (static_cast<std::uint64_t>(size) << 56));
        }
    };

    std::uint64_t ComputeCacheKey(const MeshLod& lod)
    {
        Hasher hasher;
        hasher.Mix(kBuilderVersion);
        hasher.Mix(kMaxClusterVertices);
        hasher.Mix(kMaxClusterTriangles);
        hasher.Mix(MESHOPTIMIZER_VERSION);
        hasher.Mix(lod.Vertices.size());
        hasher.Mix(lod.Indices.size());
        hasher.Bytes(lod.Vertices.data(), lod.Vertices.size() * sizeof(Vertex));
        hasher.Bytes(lod.Indices.data(), lod.Indices.size() * sizeof(std::uint32_t));
        for (const SlotRange& slot : CollectMaterialSlots(lod))
        {
            hasher.Mix(slot.IndexStart);
            hasher.Mix(slot.IndexCount);
            hasher.Mix(slot.UdimTile);
        }
        return hasher.State;
    }

    // Beside the shader cache: <project>/Cache/VirtualGeometry, or next to the
    // exe in a packaged game (whose Data folder is virtual and read-only).
    std::filesystem::path CacheDirectory()
    {
        namespace fs = std::filesystem;

        fs::path root;
        if (DataFiles::IsPackaged())
        {
            root = DataFiles::PackagedRoot().parent_path();
        }
        else
        {
            const fs::path dataDirectory = DataFiles::FindDataDirectory();
            if (!dataDirectory.empty())
                root = dataDirectory.parent_path();
        }

        if (root.empty())
            return {};

        fs::path directory = root / L"Cache" / L"VirtualGeometry";
        std::error_code error;
        fs::create_directories(directory, error);
        return error ? fs::path{} : directory;
    }

    struct CacheHeader
    {
        char          Magic[4] = { 'P', 'V', 'G', 'C' };
        std::uint32_t Version = kBuilderVersion;
        std::uint64_t Key = 0;
        std::uint32_t ClusterCount = 0;
        std::uint32_t ClusterVertexCount = 0;
        std::uint32_t ClusterTriangleCount = 0;
        std::uint32_t SourceVertexCount = 0;
        std::uint32_t SourceTriangleCount = 0;
        std::uint32_t MaterialSlotCount = 0;
        std::uint32_t DagDepth = 0;
        std::uint32_t Reserved = 0;
        float         BoundsSphere[4] = {};
    };

    std::filesystem::path CacheFilePath(const std::filesystem::path& directory, std::uint64_t key)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.pvg", static_cast<unsigned long long>(key));
        return directory / name;
    }

    bool LoadFromCache(const std::filesystem::path& path, std::uint64_t key, const MeshLod& lod, BuiltMesh& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        CacheHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file
            || std::memcmp(header.Magic, "PVGC", 4) != 0
            || header.Version != kBuilderVersion
            || header.Key != key
            || header.SourceVertexCount != lod.Vertices.size()
            || header.ClusterCount == 0)
        {
            return false;
        }

        out.Clusters.resize(header.ClusterCount);
        out.ClusterVertices.resize(header.ClusterVertexCount);
        out.ClusterTriangles.resize(header.ClusterTriangleCount);
        file.read(reinterpret_cast<char*>(out.Clusters.data()), out.Clusters.size() * sizeof(GpuCluster));
        file.read(reinterpret_cast<char*>(out.ClusterVertices.data()), out.ClusterVertices.size() * sizeof(std::uint32_t));
        file.read(reinterpret_cast<char*>(out.ClusterTriangles.data()), out.ClusterTriangles.size() * sizeof(std::uint32_t));
        if (!file)
            return false;

        // A truncated or foreign file must not reach the GPU: every offset it
        // carries becomes a buffer address in a shader.
        for (const GpuCluster& cluster : out.Clusters)
        {
            const std::uint32_t vertexCount   = cluster.Packed & 0x7Fu;
            const std::uint32_t triangleCount = (cluster.Packed >> 7) & 0x7Fu;
            if (vertexCount > kMaxClusterVertices || triangleCount > kMaxClusterTriangles
                || static_cast<std::uint64_t>(cluster.VertexOffset) + vertexCount > out.ClusterVertices.size()
                || static_cast<std::uint64_t>(cluster.TriangleOffset) + triangleCount > out.ClusterTriangles.size())
            {
                return false;
            }
        }
        for (std::uint32_t vertexIndex : out.ClusterVertices)
        {
            if (vertexIndex >= header.SourceVertexCount)
                return false;
        }

        out.SourceVertexCount   = header.SourceVertexCount;
        out.SourceTriangleCount = header.SourceTriangleCount;
        out.MaterialSlotCount   = header.MaterialSlotCount;
        out.DagDepth            = header.DagDepth;
        out.BoundsSphere = { header.BoundsSphere[0], header.BoundsSphere[1], header.BoundsSphere[2], header.BoundsSphere[3] };
        return true;
    }

    void SaveToCache(const std::filesystem::path& path, std::uint64_t key, const BuiltMesh& mesh)
    {
        CacheHeader header;
        header.Key = key;
        header.ClusterCount         = static_cast<std::uint32_t>(mesh.Clusters.size());
        header.ClusterVertexCount   = static_cast<std::uint32_t>(mesh.ClusterVertices.size());
        header.ClusterTriangleCount = static_cast<std::uint32_t>(mesh.ClusterTriangles.size());
        header.SourceVertexCount    = mesh.SourceVertexCount;
        header.SourceTriangleCount  = mesh.SourceTriangleCount;
        header.MaterialSlotCount    = mesh.MaterialSlotCount;
        header.DagDepth             = mesh.DagDepth;
        header.BoundsSphere[0] = mesh.BoundsSphere.x;
        header.BoundsSphere[1] = mesh.BoundsSphere.y;
        header.BoundsSphere[2] = mesh.BoundsSphere.z;
        header.BoundsSphere[3] = mesh.BoundsSphere.w;

        // Written aside and renamed into place, so a reader never sees half a
        // file and two builds of the same mesh on different threads cannot
        // interleave their writes.
        std::ostringstream tempName;
        tempName << path.filename().string() << '.' << std::this_thread::get_id() << ".tmp";
        const std::filesystem::path tempPath = path.parent_path() / tempName.str();

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file)
                return;
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(mesh.Clusters.data()), mesh.Clusters.size() * sizeof(GpuCluster));
            file.write(reinterpret_cast<const char*>(mesh.ClusterVertices.data()), mesh.ClusterVertices.size() * sizeof(std::uint32_t));
            file.write(reinterpret_cast<const char*>(mesh.ClusterTriangles.data()), mesh.ClusterTriangles.size() * sizeof(std::uint32_t));
            if (!file)
            {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(tempPath, ignored);
                return;
            }
        }

        std::error_code error;
        std::filesystem::rename(tempPath, path, error);
        if (error)
            std::filesystem::remove(tempPath, error);
    }

    // ------------------------------------------------------------------
    // Build
    // ------------------------------------------------------------------

    DirectX::XMFLOAT4 ToSphere(const clodBounds& bounds)
    {
        return { bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius };
    }

    // Vertices whose position is shared by triangles of two material slots.
    //
    // Each slot gets its own DAG, because a cluster has to draw with a single
    // material. Where two slots meet, each DAG sees the seam as an open border,
    // and nothing would stop the two simplifying it differently - the classic
    // crack between materials at a distance. Locking those vertices keeps the
    // seam identical at every level of both DAGs.
    std::vector<unsigned char> LockMaterialSeams(
        const MeshLod& lod, const std::vector<SlotRange>& slots, const std::vector<unsigned int>& positionRemap)
    {
        std::vector<unsigned char> locks(lod.Vertices.size(), 0);
        if (slots.size() < 2)
            return locks;

        constexpr std::uint32_t kUnowned = 0xFFFFFFFFu;
        constexpr std::uint32_t kShared  = 0xFFFFFFFEu;
        std::vector<std::uint32_t> owner(lod.Vertices.size(), kUnowned);

        for (std::uint32_t slot = 0; slot < slots.size(); ++slot)
        {
            const SlotRange& range = slots[slot];
            for (std::uint32_t i = range.IndexStart; i < range.IndexStart + range.IndexCount; ++i)
            {
                std::uint32_t& state = owner[positionRemap[lod.Indices[i]]];
                if (state == kUnowned)
                    state = slot;
                else if (state != slot)
                    state = kShared;
            }
        }

        for (std::size_t v = 0; v < lod.Vertices.size(); ++v)
        {
            if (owner[positionRemap[v]] == kShared)
                locks[v] = meshopt_SimplifyVertex_Lock;
        }
        return locks;
    }

    bool Build(const MeshLod& lod, BuiltMesh& out, std::string& error)
    {
        const std::vector<SlotRange> slots = CollectMaterialSlots(lod);
        const std::size_t vertexCount = lod.Vertices.size();
        const float* positions = &lod.Vertices[0].Position.x;

        std::vector<unsigned int> positionRemap(vertexCount);
        meshopt_generatePositionRemap(positionRemap.data(), positions, vertexCount, sizeof(Vertex));
        const std::vector<unsigned char> locks = LockMaterialSeams(lod, slots, positionRemap);
        const bool anyLocked = std::any_of(locks.begin(), locks.end(), [](unsigned char lock) { return lock != 0; });

        clodConfig config = clodDefaultConfig(kMaxClusterTriangles);
        config.max_vertices = kMaxClusterVertices;
        // Bounds for culling are computed per cluster below, from its own
        // triangles; clod's optional pass would only duplicate that work.
        config.optimize_bounds = false;

        // One group's simplified bounds per clodBuild output, across every
        // slot. The callback returns the index into this, which is what clod
        // hands back to us as a later cluster's `refined`.
        struct GroupRecord
        {
            clodBounds Simplified;
            int        Depth = 0;
        };
        std::vector<GroupRecord> groups;

        // clodLocalIndices scratch; sized for the worst case a cluster can be.
        std::vector<unsigned int>  localVertices(kMaxClusterTriangles * 3);
        std::vector<unsigned char> localTriangles(kMaxClusterTriangles * 3);

        // Each slot is built over a compacted copy of just the vertices it
        // uses. clodBuild sizes its remap and lock tables by the vertex count
        // it is given, so handing every slot the whole buffer would redo
        // whole-mesh work once per material.
        constexpr std::uint32_t kNotInSlot = 0xFFFFFFFFu;
        std::vector<std::uint32_t> globalToSlot(vertexCount, kNotInSlot);
        std::vector<Vertex>        slotVertices;
        std::vector<std::uint32_t> slotToGlobal;
        std::vector<unsigned int>  slotIndices;
        std::vector<unsigned char> slotLocks;

        out = BuiltMesh{};
        out.SourceVertexCount = static_cast<std::uint32_t>(vertexCount);
        out.MaterialSlotCount = static_cast<std::uint32_t>(slots.size());

        bool clusterTooLarge = false;

        for (std::uint32_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex)
        {
            const SlotRange& slot = slots[slotIndex];
            if (slot.IndexCount < 3)
                continue;

            out.SourceTriangleCount += slot.IndexCount / 3;

            slotVertices.clear();
            slotToGlobal.clear();
            slotIndices.resize(slot.IndexCount);
            for (std::uint32_t i = 0; i < slot.IndexCount; ++i)
            {
                const std::uint32_t globalIndex = lod.Indices[slot.IndexStart + i];
                std::uint32_t& local = globalToSlot[globalIndex];
                if (local == kNotInSlot)
                {
                    local = static_cast<std::uint32_t>(slotVertices.size());
                    slotVertices.push_back(lod.Vertices[globalIndex]);
                    slotToGlobal.push_back(globalIndex);
                }
                slotIndices[i] = local;
            }
            // Reset only what this slot touched, ready for the next one.
            for (std::uint32_t globalIndex : slotToGlobal)
                globalToSlot[globalIndex] = kNotInSlot;

            slotLocks.resize(slotVertices.size());
            for (std::size_t v = 0; v < slotToGlobal.size(); ++v)
                slotLocks[v] = locks[slotToGlobal[v]];

            const float* slotPositions = &slotVertices[0].Position.x;
            const std::size_t slotVertexCount = slotVertices.size();

            clodMesh mesh{};
            mesh.indices                  = slotIndices.data();
            mesh.index_count              = slotIndices.size();
            mesh.vertex_count             = slotVertexCount;
            mesh.vertex_positions         = slotPositions;
            mesh.vertex_positions_stride  = sizeof(Vertex);
            mesh.vertex_attributes        = &slotVertices[0].Normal.x;
            mesh.vertex_attributes_stride = sizeof(Vertex);
            mesh.attribute_weights        = kNormalWeights;
            mesh.attribute_count          = std::size(kNormalWeights);
            mesh.attribute_protect_mask   = kSeamProtectMask;
            mesh.vertex_lock              = anyLocked ? slotLocks.data() : nullptr;

            clodBuild(config, mesh, [&](clodGroup group, const clodCluster* clusters, size_t clusterCount) -> int
            {
                const int groupId = static_cast<int>(groups.size());
                groups.push_back({ group.simplified, group.depth });
                out.DagDepth = (std::max)(out.DagDepth, static_cast<std::uint32_t>(group.depth) + 1u);

                for (size_t i = 0; i < clusterCount; ++i)
                {
                    const clodCluster& cluster = clusters[i];
                    const std::size_t triangleCount = cluster.index_count / 3;
                    if (triangleCount == 0)
                        continue;
                    if (triangleCount > kMaxClusterTriangles || cluster.vertex_count > kMaxClusterVertices)
                    {
                        clusterTooLarge = true;
                        continue;
                    }

                    const std::size_t uniqueVertices = clodLocalIndices(
                        localVertices.data(), localTriangles.data(), cluster.indices, cluster.index_count);

                    const meshopt_Bounds bounds = meshopt_computeClusterBounds(
                        cluster.indices, cluster.index_count, slotPositions, slotVertexCount, sizeof(Vertex));

                    GpuCluster record;
                    record.CullSphere = { bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius };
                    record.ConeApex   = { bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2] };
                    record.ConeAxis   = { bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2] };
                    // A cone this wide never proves anything; keep it from
                    // culling on rounding error when the camera sits on its axis.
                    record.ConeCutoff = bounds.cone_cutoff >= 0.999f ? 2.0f : bounds.cone_cutoff;

                    record.ParentSphere = ToSphere(group.simplified);
                    record.ParentError  = group.simplified.error;

                    if (cluster.refined >= 0 && cluster.refined < static_cast<int>(groups.size()))
                    {
                        record.LodSphere = ToSphere(groups[cluster.refined].Simplified);
                        record.LodError  = groups[cluster.refined].Simplified.error;
                    }
                    else
                    {
                        record.LodSphere = record.CullSphere;
                        record.LodError  = 0.0f;
                    }

                    record.VertexOffset   = static_cast<std::uint32_t>(out.ClusterVertices.size());
                    record.TriangleOffset = static_cast<std::uint32_t>(out.ClusterTriangles.size());
                    record.Packed = PackClusterCounts(
                        static_cast<std::uint32_t>(uniqueVertices),
                        static_cast<std::uint32_t>(triangleCount),
                        static_cast<std::uint32_t>((std::min)(group.depth, 63)),
                        slotIndex);

                    // Cluster-local vertices name slot vertices; the GPU reads
                    // the asset's own vertex buffer, so store source indices.
                    for (std::size_t v = 0; v < uniqueVertices; ++v)
                        out.ClusterVertices.push_back(slotToGlobal[localVertices[v]]);
                    for (std::size_t t = 0; t < triangleCount; ++t)
                    {
                        out.ClusterTriangles.push_back(
                            static_cast<std::uint32_t>(localTriangles[t * 3 + 0])
                            | (static_cast<std::uint32_t>(localTriangles[t * 3 + 1]) << 8)
                            | (static_cast<std::uint32_t>(localTriangles[t * 3 + 2]) << 16));
                    }

                    out.Clusters.push_back(record);
                }

                return groupId;
            });
        }

        if (clusterTooLarge)
        {
            error = "clusterizer produced a cluster larger than 64 vertices / 124 triangles";
            return false;
        }
        if (out.Clusters.empty())
        {
            error = "the mesh produced no clusters";
            return false;
        }

        std::stable_sort(out.Clusters.begin(), out.Clusters.end(),
            [](const GpuCluster& a, const GpuCluster& b) { return a.ParentError < b.ParentError; });

        // Enclosing sphere of every sphere a shader will ever measure a
        // distance to. The LOD and parent spheres are merged group bounds and
        // can stick out past the geometry, so they count as much as the cull
        // spheres do.
        DirectX::XMFLOAT3 minimum{ FLT_MAX, FLT_MAX, FLT_MAX };
        DirectX::XMFLOAT3 maximum{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        auto extend = [&](const DirectX::XMFLOAT4& sphere)
        {
            minimum.x = (std::min)(minimum.x, sphere.x - sphere.w);
            minimum.y = (std::min)(minimum.y, sphere.y - sphere.w);
            minimum.z = (std::min)(minimum.z, sphere.z - sphere.w);
            maximum.x = (std::max)(maximum.x, sphere.x + sphere.w);
            maximum.y = (std::max)(maximum.y, sphere.y + sphere.w);
            maximum.z = (std::max)(maximum.z, sphere.z + sphere.w);
        };
        for (const GpuCluster& cluster : out.Clusters)
        {
            extend(cluster.CullSphere);
            extend(cluster.LodSphere);
            extend(cluster.ParentSphere);
        }

        const DirectX::XMFLOAT3 center{
            (minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f, (minimum.z + maximum.z) * 0.5f };
        float radius = 0.0f;
        auto enclose = [&](const DirectX::XMFLOAT4& sphere)
        {
            const float dx = sphere.x - center.x, dy = sphere.y - center.y, dz = sphere.z - center.z;
            radius = (std::max)(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + sphere.w);
        };
        for (const GpuCluster& cluster : out.Clusters)
        {
            enclose(cluster.CullSphere);
            enclose(cluster.LodSphere);
            enclose(cluster.ParentSphere);
        }
        out.BoundsSphere = { center.x, center.y, center.z, radius };
        return true;
    }
}

std::vector<MaterialSlot> CollectMaterialSlots(const MeshLod& lod)
{
    // A mesh with no sub-mesh table is one slot covering everything, exactly as
    // the ordinary renderer treats it.
    std::vector<MaterialSlot> slots;
    if (lod.SubMeshes.empty())
    {
        slots.push_back({ 0u, static_cast<std::uint32_t>(lod.Indices.size()), 0u, 0u });
        return slots;
    }

    // UDIM-tiled sub-meshes split per tile: a cluster has to draw with one
    // texture set, and a UDIM material has one per tile. The tiles are
    // contiguous in the index buffer already, so each is a plain range.
    for (const SubMesh& subMesh : lod.SubMeshes)
    {
        if (subMesh.udimTiles.empty())
        {
            slots.push_back({ subMesh.indexStart, subMesh.indexCount, subMesh.materialId, 0u });
            continue;
        }
        for (const SubMeshUdimTile& tile : subMesh.udimTiles)
            slots.push_back({ tile.indexStart, tile.indexCount, subMesh.materialId, tile.tile });
    }
    return slots;
}

bool CanVirtualize(const MeshLod& lod, std::string* reason)
{
    auto fail = [reason](const char* why)
    {
        if (reason != nullptr)
            *reason = why;
        return false;
    };

    if (lod.Vertices.empty() || lod.Indices.empty())
        return fail("the mesh is empty");
    if (lod.Indices.size() % 3 != 0)
        return fail("the index count is not a multiple of three");
    if (lod.Indices.size() / 3 < kMinSourceTriangles)
        return fail("the mesh is too small to benefit from clustering");
    if (CollectMaterialSlots(lod).size() > kMaxMaterialSlots)
        return fail("the mesh has more material slots than a cluster can address");

    for (const SubMesh& subMesh : lod.SubMeshes)
    {
        if (subMesh.indexCount % 3 != 0
            || static_cast<std::uint64_t>(subMesh.indexStart) + subMesh.indexCount > lod.Indices.size())
        {
            return fail("a sub-mesh range lies outside the index buffer");
        }
    }

    const std::size_t vertexCount = lod.Vertices.size();
    for (std::uint32_t index : lod.Indices)
    {
        if (index >= vertexCount)
            return fail("an index refers past the end of the vertex buffer");
    }
    return true;
}

BuildResult BuildOrLoad(const MeshLod& lod)
{
    const auto start = std::chrono::steady_clock::now();
    BuildResult result;

    std::string reason;
    if (!CanVirtualize(lod, &reason))
    {
        result.Error = reason;
        return result;
    }

    const std::uint64_t key = ComputeCacheKey(lod);
    const std::filesystem::path cacheDirectory = CacheDirectory();
    const std::filesystem::path cachePath = cacheDirectory.empty()
        ? std::filesystem::path{} : CacheFilePath(cacheDirectory, key);

    auto mesh = std::make_shared<BuiltMesh>();
    if (!cachePath.empty() && LoadFromCache(cachePath, key, lod, *mesh))
    {
        result.FromCache = true;
    }
    else
    {
        if (!Build(lod, *mesh, result.Error))
            return result;
        if (!cachePath.empty())
            SaveToCache(cachePath, key, *mesh);
    }

    result.Mesh = std::move(mesh);
    result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}
