#pragma once

// GeometryRaycaster
// -----------------
// A throwaway CPU BVH built over the static meshes in a region of the world,
// used by the vegetation scatter to drop instances onto arbitrary geometry
// rather than only onto terrain.
//
// The raycaster is rebuilt whenever a scatter runs, so it is optimised for
// build speed over query speed: triangles are flattened into world space up
// front and the tree is a plain median-split binary BVH.  A vegetation area
// typically covers a handful of meshes, so this stays in the low milliseconds.
//
// Triangles come from a mesh's collision hulls when the asset has them (the
// collision generator already produces a much cheaper approximation than the
// render mesh) and otherwise from its coarsest LOD.  Neither is the geometry
// the player sees up close, which is exactly what we want here — vegetation
// only needs to know where the surface roughly is.
//
// Everything is engine-agnostic and free of D3D so it can be unit-tested,
// matching how TerrainGeometry is structured.  World space is Z-up and
// left-handed, so "cast down" means -Z.

#include "Components.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct GeometryRayHit
{
    // Distance along the ray direction, in metres.
    float Distance = 0.0f;

    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };

    // Geometric normal of the hit triangle, always flipped to oppose the ray
    // so callers never have to care about mesh winding.
    DirectX::XMFLOAT3 Normal{ 0.0f, 0.0f, 1.0f };

    // Index into the entity vector the raycaster was built from.
    std::size_t EntityIndex = 0;
};

class GeometryRaycaster
{
public:
    // Collect every mesh entity whose world-space bounds overlap the given
    // region and build the BVH.  Entities that carry a vegetation area, water
    // or terrain component are skipped: terrain has its own analytic height
    // query, and an area volume is not a surface.
    void Build(
        const std::vector<Entity>& entities,
        const DirectX::XMFLOAT3&   regionMin,
        const DirectX::XMFLOAT3&   regionMax);

    void Clear();

    bool IsEmpty() const { return mTriangles.empty(); }

    std::size_t GetTriangleCount() const { return mTriangles.size(); }

    // Cast a ray and return the nearest hit.  `direction` must be normalised.
    // Triangles are treated as double-sided, because imported meshes cannot be
    // relied on to have consistent winding and a scatter that silently drops
    // half its candidates is very hard to diagnose.
    bool Raycast(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        float                    maxDistance,
        GeometryRayHit&          outHit) const;

    // Convenience wrapper for the scatter's common case: straight down from a
    // point above the world.
    bool RaycastDown(
        const DirectX::XMFLOAT2& worldXY,
        float                    startZ,
        float                    maxDistance,
        GeometryRayHit&          outHit) const;

private:
    // World-space triangle.  Edges are precomputed because the Moller-Trumbore
    // test needs them on every visit and the BVH revisits triangles often.
    struct Triangle
    {
        DirectX::XMFLOAT3 V0{};
        DirectX::XMFLOAT3 Edge1{};
        DirectX::XMFLOAT3 Edge2{};
        DirectX::XMFLOAT3 Normal{};
        std::uint32_t     EntityIndex = 0;
    };

    struct Node
    {
        DirectX::XMFLOAT3 BoundsMin{};
        DirectX::XMFLOAT3 BoundsMax{};
        // For an interior node this is the index of the left child and Count is
        // zero; the right child is always LeftFirst + 1.  For a leaf it is the
        // first index into mTriangleIndices.
        std::uint32_t     LeftFirst = 0;
        std::uint32_t     Count     = 0;

        bool IsLeaf() const { return Count > 0; }
    };

    // Recursively split [first, first+count) of mTriangleIndices.  Returns the
    // index of the node it produced.
    std::uint32_t BuildNode(std::uint32_t first, std::uint32_t count, int depth);

    void AppendEntityTriangles(
        const Entity&            entity,
        std::size_t              entityIndex,
        const DirectX::XMFLOAT3& regionMin,
        const DirectX::XMFLOAT3& regionMax);

    std::vector<Triangle>      mTriangles;
    // Indirection so the build can reorder triangles without moving them.
    std::vector<std::uint32_t> mTriangleIndices;
    std::vector<Node>          mNodes;
    // Centroid per triangle, cached during the build to keep partitioning cheap.
    std::vector<DirectX::XMFLOAT3> mCentroids;
};
