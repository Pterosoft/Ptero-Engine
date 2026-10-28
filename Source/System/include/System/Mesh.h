#pragma once

#include <DirectXMath.h>

#include "Udim.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

struct CollisionHull
{
    std::vector<DirectX::XMFLOAT3> Vertices;
    std::vector<std::uint32_t> Indices;
};

struct Vertex
{
    DirectX::XMFLOAT3 Position{};
    DirectX::XMFLOAT3 Normal{};
    DirectX::XMFLOAT2 TexCoord{};
    DirectX::XMFLOAT4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// A run of a sub-mesh's indices whose triangles all lie in one UDIM tile.
struct SubMeshUdimTile
{
    std::uint32_t tile = 0;        // UDIM number, e.g. 1001
    std::uint32_t indexStart = 0;
    std::uint32_t indexCount = 0;
};

// Describes a contiguous range of the shared index buffer that corresponds to a single FBX material slot.
// materialId matches the zero-based material index assigned to the polygons in the original FBX mesh.
struct SubMesh
{
    std::uint32_t materialId = 0;
    std::uint32_t indexStart = 0;
    std::uint32_t indexCount = 0;
    // The sub-mesh's triangles grouped by UDIM tile, covering [indexStart, indexStart + indexCount).
    // Filled at load (Mesh::BuildUdimTiles) when every triangle lies inside the UDIM range; empty
    // otherwise. Only used when the material's texture paths contain <UDIM>.
    std::vector<SubMeshUdimTile> udimTiles;
};

struct MeshLod
{
    std::vector<Vertex> Vertices;
    std::vector<std::uint32_t> Indices;
    std::vector<SubMesh> SubMeshes;
};

class Mesh
{
public:
    Mesh(
        std::vector<Vertex> vertices,
        std::vector<std::uint32_t> indices,
        std::vector<CollisionHull> collisionHulls)
        : mCollisionHulls(std::move(collisionHulls))
    {
        mLods.push_back({ std::move(vertices), std::move(indices), {} });
    }

    Mesh(std::vector<Vertex> vertices, std::vector<std::uint32_t> indices)
    {
        mLods.push_back({ std::move(vertices), std::move(indices), {} });
    }

    Mesh(
        std::vector<Vertex> vertices,
        std::vector<std::uint32_t> indices,
        std::vector<SubMesh> subMeshes,
        std::vector<CollisionHull> collisionHulls)
        : mCollisionHulls(std::move(collisionHulls))
    {
        mLods.push_back({ std::move(vertices), std::move(indices), std::move(subMeshes) });
        BuildUdimTiles();
    }

    Mesh(std::vector<Vertex> vertices, std::vector<std::uint32_t> indices, std::vector<SubMesh> subMeshes)
    {
        mLods.push_back({ std::move(vertices), std::move(indices), std::move(subMeshes) });
        BuildUdimTiles();
    }

    Mesh(std::vector<MeshLod> lods, std::vector<CollisionHull> collisionHulls = {})
        : mLods(std::move(lods))
        , mCollisionHulls(std::move(collisionHulls))
    {
        if (mLods.empty())
        {
            mLods.push_back({});
        }
        BuildUdimTiles();
    }

    bool UploadToGpu()
    {
        // This is the system-side handoff point for a future renderer-facing mesh upload path.
        mIsUploadedToGpu = true;
        return true;
    }

    const std::vector<Vertex>& GetVertices() const
    {
        return mLods.front().Vertices;
    }

    const std::vector<std::uint32_t>& GetIndices() const
    {
        return mLods.front().Indices;
    }

    // Returns one SubMesh per FBX material slot.  When empty the whole index buffer belongs to material 0.
    const std::vector<SubMesh>& GetSubMeshes() const
    {
        return mLods.front().SubMeshes;
    }

    const std::vector<MeshLod>& GetLods() const
    {
        return mLods;
    }

    const MeshLod& GetLod(std::size_t lodIndex) const
    {
        if (lodIndex >= mLods.size())
        {
            lodIndex = mLods.size() - 1;
        }

        return mLods[lodIndex];
    }

    std::size_t GetLodCount() const
    {
        return mLods.size();
    }

    const std::vector<CollisionHull>& GetCollisionHulls() const
    {
        return mCollisionHulls;
    }

    bool HasCollisionHulls() const
    {
        return !mCollisionHulls.empty();
    }

    bool IsUploadedToGpu() const
    {
        return mIsUploadedToGpu;
    }

private:
    std::vector<MeshLod> mLods;
    std::vector<CollisionHull> mCollisionHulls;
    bool mIsUploadedToGpu = false;

    // Groups every sub-mesh's triangles by UDIM tile so a <UDIM> material can bind one tile's
    // textures per draw. Triangles are reordered only inside their own sub-mesh range (stable, so
    // the order within a tile is kept); sub-mesh ranges and the set of triangles never change.
    void BuildUdimTiles()
    {
        for (MeshLod& lod : mLods)
        {
            for (SubMesh& subMesh : lod.SubMeshes)
            {
                subMesh.udimTiles.clear();
                const std::uint32_t triCount = subMesh.indexCount / 3u;
                if (triCount == 0 || subMesh.indexCount % 3u != 0
                    || std::size_t(subMesh.indexStart) + subMesh.indexCount > lod.Indices.size())
                    continue;

                std::vector<std::uint32_t> triTiles(triCount);
                bool insideUdimRange = true;
                for (std::uint32_t t = 0; t < triCount && insideUdimRange; ++t)
                {
                    const std::uint32_t* tri = &lod.Indices[subMesh.indexStart + t * 3u];
                    float u = 0.0f, v = 0.0f;
                    for (int k = 0; k < 3; ++k)
                    {
                        if (tri[k] >= lod.Vertices.size()) { insideUdimRange = false; break; }
                        u += lod.Vertices[tri[k]].TexCoord.x;
                        v += lod.Vertices[tri[k]].TexCoord.y;
                    }
                    // The centroid decides the tile, so triangles whose corners sit on a tile border still land inside.
                    triTiles[t] = Udim::TileFromTexCoord(u / 3.0f, v / 3.0f);
                    insideUdimRange = insideUdimRange && triTiles[t] != 0;
                }
                if (!insideUdimRange)
                    continue;

                std::vector<std::uint32_t> order(triCount);
                for (std::uint32_t t = 0; t < triCount; ++t) order[t] = t;
                std::stable_sort(order.begin(), order.end(),
                    [&triTiles](std::uint32_t a, std::uint32_t b) { return triTiles[a] < triTiles[b]; });

                std::vector<std::uint32_t> sorted(subMesh.indexCount);
                for (std::uint32_t t = 0; t < triCount; ++t)
                {
                    for (int k = 0; k < 3; ++k)
                        sorted[t * 3u + k] = lod.Indices[subMesh.indexStart + order[t] * 3u + k];
                }
                std::copy(sorted.begin(), sorted.end(), lod.Indices.begin() + subMesh.indexStart);

                for (std::uint32_t t = 0; t < triCount; ++t)
                {
                    const std::uint32_t tile = triTiles[order[t]];
                    if (subMesh.udimTiles.empty() || subMesh.udimTiles.back().tile != tile)
                        subMesh.udimTiles.push_back({ tile, subMesh.indexStart + t * 3u, 0u });
                    subMesh.udimTiles.back().indexCount += 3u;
                }
            }
        }
    }
};
