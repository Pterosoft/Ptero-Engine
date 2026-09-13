#include "pch.h"

#include "GeometryRaycaster.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

using namespace DirectX;

namespace
{
    // Leaves smaller than this are not worth splitting; the linear scan is
    // cheaper than the extra traversal.
    constexpr std::uint32_t kMaxTrianglesPerLeaf = 8;
    constexpr int           kMaxDepth            = 40;

    // Traversal stack depth.  kMaxDepth levels of a binary tree need at most
    // kMaxDepth+1 entries pending, so 64 leaves generous headroom.
    constexpr int kTraversalStackSize = 64;

    inline XMFLOAT3 Min3(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return XMFLOAT3((std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z));
    }

    inline XMFLOAT3 Max3(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return XMFLOAT3((std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z));
    }

    inline bool AabbOverlap(
        const XMFLOAT3& aMin, const XMFLOAT3& aMax,
        const XMFLOAT3& bMin, const XMFLOAT3& bMax)
    {
        return aMin.x <= bMax.x && aMax.x >= bMin.x
            && aMin.y <= bMax.y && aMax.y >= bMin.y
            && aMin.z <= bMax.z && aMax.z >= bMin.z;
    }

    // Slab test.  Returns the near intersection distance, or a negative value
    // when the ray misses.  invDir is precomputed once per query.
    inline float RayAabbDistance(
        const XMFLOAT3& origin,
        const XMFLOAT3& invDir,
        const XMFLOAT3& boundsMin,
        const XMFLOAT3& boundsMax,
        float           maxDistance)
    {
        const float tx1 = (boundsMin.x - origin.x) * invDir.x;
        const float tx2 = (boundsMax.x - origin.x) * invDir.x;
        float tMin = (std::min)(tx1, tx2);
        float tMax = (std::max)(tx1, tx2);

        const float ty1 = (boundsMin.y - origin.y) * invDir.y;
        const float ty2 = (boundsMax.y - origin.y) * invDir.y;
        tMin = (std::max)(tMin, (std::min)(ty1, ty2));
        tMax = (std::min)(tMax, (std::max)(ty1, ty2));

        const float tz1 = (boundsMin.z - origin.z) * invDir.z;
        const float tz2 = (boundsMax.z - origin.z) * invDir.z;
        tMin = (std::max)(tMin, (std::min)(tz1, tz2));
        tMax = (std::min)(tMax, (std::max)(tz1, tz2));

        if (tMax < (std::max)(tMin, 0.0f) || tMin > maxDistance)
            return -1.0f;

        return (std::max)(tMin, 0.0f);
    }
}

void GeometryRaycaster::Clear()
{
    mTriangles.clear();
    mTriangleIndices.clear();
    mNodes.clear();
    mCentroids.clear();
}

void GeometryRaycaster::Build(
    const std::vector<Entity>& entities,
    const XMFLOAT3&            regionMin,
    const XMFLOAT3&            regionMax)
{
    Clear();

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const Entity& entity = entities[i];

        // Terrain is queried analytically through the heightmap, and area
        // volumes are not surfaces, so neither belongs in the BVH.
        if (entity.HasTerrainComponent() || entity.HasVegetationAreaComponent())
            continue;

        if (!entity.HasMeshComponent() || !entity.Mesh->MeshAsset)
            continue;

        AppendEntityTriangles(entity, i, regionMin, regionMax);
    }

    if (mTriangles.empty())
        return;

    mTriangleIndices.resize(mTriangles.size());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mTriangles.size()); ++i)
        mTriangleIndices[i] = i;

    // A balanced binary tree over N leaves of >= 1 triangle needs at most
    // 2N-1 nodes; reserving up front keeps BuildNode's recursive push_back
    // from invalidating the reference it holds into mNodes.
    mNodes.reserve(mTriangles.size() * 2);

    BuildNode(0, static_cast<std::uint32_t>(mTriangleIndices.size()), 0);

    // Centroids are only needed while partitioning.
    mCentroids.clear();
    mCentroids.shrink_to_fit();
}

void GeometryRaycaster::AppendEntityTriangles(
    const Entity&   entity,
    std::size_t     entityIndex,
    const XMFLOAT3& regionMin,
    const XMFLOAT3& regionMax)
{
    const Mesh* mesh = entity.Mesh->MeshAsset.get();
    if (mesh == nullptr)
        return;

    // GetTransform() already folds in TransformComponent::MeshWorldScale, so
    // the triangles land in the same world space the renderer draws them at.
    const XMMATRIX world = entity.Transform.GetTransform();

    // Walk collision hulls when the asset has them — they are far cheaper than
    // the render mesh and are exactly the "where is the surface" approximation
    // we want.  Otherwise fall back to the coarsest LOD.
    if (mesh->HasCollisionHulls())
    {
        for (const CollisionHull& hull : mesh->GetCollisionHulls())
        {
            if (hull.Indices.size() < 3)
                continue;

            for (std::size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
            {
                const std::uint32_t i0 = hull.Indices[i + 0];
                const std::uint32_t i1 = hull.Indices[i + 1];
                const std::uint32_t i2 = hull.Indices[i + 2];

                if (i0 >= hull.Vertices.size() || i1 >= hull.Vertices.size() || i2 >= hull.Vertices.size())
                    continue;

                XMFLOAT3 p0{};
                XMFLOAT3 p1{};
                XMFLOAT3 p2{};
                XMStoreFloat3(&p0, XMVector3Transform(XMLoadFloat3(&hull.Vertices[i0]), world));
                XMStoreFloat3(&p1, XMVector3Transform(XMLoadFloat3(&hull.Vertices[i1]), world));
                XMStoreFloat3(&p2, XMVector3Transform(XMLoadFloat3(&hull.Vertices[i2]), world));

                const XMFLOAT3 triMin = Min3(p0, Min3(p1, p2));
                const XMFLOAT3 triMax = Max3(p0, Max3(p1, p2));
                if (!AabbOverlap(triMin, triMax, regionMin, regionMax))
                    continue;

                Triangle tri{};
                tri.V0 = p0;
                tri.Edge1 = XMFLOAT3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
                tri.Edge2 = XMFLOAT3(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
                XMStoreFloat3(&tri.Normal,
                    XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&tri.Edge1), XMLoadFloat3(&tri.Edge2))));
                tri.EntityIndex = static_cast<std::uint32_t>(entityIndex);
                mTriangles.push_back(tri);
            }
        }

        return;
    }

    const std::size_t lodIndex = mesh->GetLodCount() - 1;
    const MeshLod& lod = mesh->GetLod(lodIndex);

    for (std::size_t i = 0; i + 2 < lod.Indices.size(); i += 3)
    {
        const std::uint32_t i0 = lod.Indices[i + 0];
        const std::uint32_t i1 = lod.Indices[i + 1];
        const std::uint32_t i2 = lod.Indices[i + 2];

        if (i0 >= lod.Vertices.size() || i1 >= lod.Vertices.size() || i2 >= lod.Vertices.size())
            continue;

        XMFLOAT3 p0{};
        XMFLOAT3 p1{};
        XMFLOAT3 p2{};
        XMStoreFloat3(&p0, XMVector3Transform(XMLoadFloat3(&lod.Vertices[i0].Position), world));
        XMStoreFloat3(&p1, XMVector3Transform(XMLoadFloat3(&lod.Vertices[i1].Position), world));
        XMStoreFloat3(&p2, XMVector3Transform(XMLoadFloat3(&lod.Vertices[i2].Position), world));

        const XMFLOAT3 triMin = Min3(p0, Min3(p1, p2));
        const XMFLOAT3 triMax = Max3(p0, Max3(p1, p2));
        if (!AabbOverlap(triMin, triMax, regionMin, regionMax))
            continue;

        Triangle tri{};
        tri.V0 = p0;
        tri.Edge1 = XMFLOAT3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
        tri.Edge2 = XMFLOAT3(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
        XMStoreFloat3(&tri.Normal,
            XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&tri.Edge1), XMLoadFloat3(&tri.Edge2))));
        tri.EntityIndex = static_cast<std::uint32_t>(entityIndex);
        mTriangles.push_back(tri);
    }
}

std::uint32_t GeometryRaycaster::BuildNode(std::uint32_t first, std::uint32_t count, int depth)
{
    const std::uint32_t nodeIndex = static_cast<std::uint32_t>(mNodes.size());
    mNodes.emplace_back();

    XMFLOAT3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    XMFLOAT3 centroidMin(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 centroidMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    if (mCentroids.size() != mTriangles.size())
        mCentroids.resize(mTriangles.size());

    for (std::uint32_t i = first; i < first + count; ++i)
    {
        const Triangle& tri = mTriangles[mTriangleIndices[i]];

        const XMFLOAT3 p0 = tri.V0;
        const XMFLOAT3 p1(p0.x + tri.Edge1.x, p0.y + tri.Edge1.y, p0.z + tri.Edge1.z);
        const XMFLOAT3 p2(p0.x + tri.Edge2.x, p0.y + tri.Edge2.y, p0.z + tri.Edge2.z);

        boundsMin = Min3(boundsMin, Min3(p0, Min3(p1, p2)));
        boundsMax = Max3(boundsMax, Max3(p0, Max3(p1, p2)));

        const XMFLOAT3 centroid(
            (p0.x + p1.x + p2.x) / 3.0f,
            (p0.y + p1.y + p2.y) / 3.0f,
            (p0.z + p1.z + p2.z) / 3.0f);
        mCentroids[mTriangleIndices[i]] = centroid;

        centroidMin = Min3(centroidMin, centroid);
        centroidMax = Max3(centroidMax, centroid);
    }

    mNodes[nodeIndex].BoundsMin = boundsMin;
    mNodes[nodeIndex].BoundsMax = boundsMax;

    const bool makeLeaf = count <= kMaxTrianglesPerLeaf || depth >= kMaxDepth;
    if (makeLeaf)
    {
        mNodes[nodeIndex].LeftFirst = first;
        mNodes[nodeIndex].Count     = count;
        return nodeIndex;
    }

    // Split on the widest centroid axis at the midpoint.  Median split would
    // be marginally better shaped but needs a partial sort per node; for the
    // few thousand triangles a vegetation area sees, midpoint wins on build
    // time and the query cost difference is not measurable.
    const XMFLOAT3 centroidExtent(
        centroidMax.x - centroidMin.x,
        centroidMax.y - centroidMin.y,
        centroidMax.z - centroidMin.z);

    int axis = 0;
    if (centroidExtent.y > centroidExtent.x) axis = 1;
    if (centroidExtent.z > ((axis == 0) ? centroidExtent.x : centroidExtent.y)) axis = 2;

    const float axisExtent = (axis == 0) ? centroidExtent.x : (axis == 1) ? centroidExtent.y : centroidExtent.z;

    // Every centroid coincides — no split can separate them.
    if (axisExtent < 1e-6f)
    {
        mNodes[nodeIndex].LeftFirst = first;
        mNodes[nodeIndex].Count     = count;
        return nodeIndex;
    }

    const float axisMin = (axis == 0) ? centroidMin.x : (axis == 1) ? centroidMin.y : centroidMin.z;
    const float splitPos = axisMin + axisExtent * 0.5f;

    std::uint32_t left = first;
    std::uint32_t right = first + count - 1;
    while (left <= right)
    {
        const XMFLOAT3& c = mCentroids[mTriangleIndices[left]];
        const float value = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;

        if (value < splitPos)
        {
            ++left;
        }
        else
        {
            std::swap(mTriangleIndices[left], mTriangleIndices[right]);
            if (right == first)
                break;
            --right;
        }
    }

    const std::uint32_t leftCount = left - first;

    // A degenerate partition would recurse forever; fall back to a leaf.
    if (leftCount == 0 || leftCount == count)
    {
        mNodes[nodeIndex].LeftFirst = first;
        mNodes[nodeIndex].Count     = count;
        return nodeIndex;
    }

    const std::uint32_t leftChild = BuildNode(first, leftCount, depth + 1);
    BuildNode(left, count - leftCount, depth + 1);

    mNodes[nodeIndex].LeftFirst = leftChild;
    mNodes[nodeIndex].Count     = 0;
    return nodeIndex;
}

bool GeometryRaycaster::Raycast(
    const XMFLOAT3& origin,
    const XMFLOAT3& direction,
    float           maxDistance,
    GeometryRayHit& outHit) const
{
    if (mNodes.empty() || mTriangles.empty())
        return false;

    // A zero component yields an infinity here, which the slab test handles
    // correctly for axis-parallel rays (the common straight-down case).
    const XMFLOAT3 invDir(
        1.0f / ((direction.x == 0.0f) ? 1e-20f : direction.x),
        1.0f / ((direction.y == 0.0f) ? 1e-20f : direction.y),
        1.0f / ((direction.z == 0.0f) ? 1e-20f : direction.z));

    float bestDistance = maxDistance;
    bool  hitAnything  = false;

    std::uint32_t stack[kTraversalStackSize];
    int stackSize = 0;
    stack[stackSize++] = 0;

    while (stackSize > 0)
    {
        const Node& node = mNodes[stack[--stackSize]];

        const float nodeDistance = RayAabbDistance(origin, invDir, node.BoundsMin, node.BoundsMax, bestDistance);
        if (nodeDistance < 0.0f || nodeDistance > bestDistance)
            continue;

        if (node.IsLeaf())
        {
            for (std::uint32_t i = node.LeftFirst; i < node.LeftFirst + node.Count; ++i)
            {
                const Triangle& tri = mTriangles[mTriangleIndices[i]];

                // Moller-Trumbore, without the backface early-out so imported
                // meshes with inconsistent winding still register.
                const XMVECTOR edge1 = XMLoadFloat3(&tri.Edge1);
                const XMVECTOR edge2 = XMLoadFloat3(&tri.Edge2);
                const XMVECTOR dir   = XMLoadFloat3(&direction);

                const XMVECTOR pvec = XMVector3Cross(dir, edge2);
                const float det = XMVectorGetX(XMVector3Dot(edge1, pvec));

                if (std::fabs(det) < 1e-8f)
                    continue;

                const float invDet = 1.0f / det;

                const XMVECTOR orig = XMLoadFloat3(&origin);
                const XMVECTOR v0   = XMLoadFloat3(&tri.V0);
                const XMVECTOR tvec = XMVectorSubtract(orig, v0);

                const float u = XMVectorGetX(XMVector3Dot(tvec, pvec)) * invDet;
                if (u < 0.0f || u > 1.0f)
                    continue;

                const XMVECTOR qvec = XMVector3Cross(tvec, edge1);
                const float v = XMVectorGetX(XMVector3Dot(dir, qvec)) * invDet;
                if (v < 0.0f || u + v > 1.0f)
                    continue;

                const float t = XMVectorGetX(XMVector3Dot(edge2, qvec)) * invDet;
                if (t < 1e-4f || t > bestDistance)
                    continue;

                bestDistance = t;
                hitAnything = true;

                outHit.Distance = t;
                outHit.Position = XMFLOAT3(
                    origin.x + direction.x * t,
                    origin.y + direction.y * t,
                    origin.z + direction.z * t);
                outHit.EntityIndex = tri.EntityIndex;

                // Present the normal facing back along the ray so callers can
                // treat every hit as a front face.
                const float facing = tri.Normal.x * direction.x
                                   + tri.Normal.y * direction.y
                                   + tri.Normal.z * direction.z;
                outHit.Normal = (facing > 0.0f)
                    ? XMFLOAT3(-tri.Normal.x, -tri.Normal.y, -tri.Normal.z)
                    : tri.Normal;
            }
        }
        else if (stackSize + 2 <= kTraversalStackSize)
        {
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }
    }

    return hitAnything;
}

bool GeometryRaycaster::RaycastDown(
    const XMFLOAT2& worldXY,
    float           startZ,
    float           maxDistance,
    GeometryRayHit& outHit) const
{
    return Raycast(
        XMFLOAT3(worldXY.x, worldXY.y, startZ),
        XMFLOAT3(0.0f, 0.0f, -1.0f),
        maxDistance,
        outHit);
}
