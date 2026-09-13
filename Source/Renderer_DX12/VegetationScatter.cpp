#include "pch.h"

#include "VegetationScatter.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>

using namespace DirectX;

namespace
{
    constexpr float kPi    = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;

    // Thomas Wang's 32-bit integer hash.  Cheap, and good enough that adjacent
    // grid cells produce visually uncorrelated offsets — which is the whole
    // reason the jittered grid reads as blue noise rather than as a grid.
    inline std::uint32_t Hash32(std::uint32_t x)
    {
        x = (x ^ 61u) ^ (x >> 16);
        x *= 9u;
        x ^= x >> 4;
        x *= 0x27d4eb2du;
        x ^= x >> 15;
        return x;
    }

    inline std::uint32_t HashCombine(std::uint32_t seed, std::uint32_t value)
    {
        return Hash32(seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2)));
    }

    // Uniform float in [0,1).  Uses the high bits, which are the best mixed.
    inline float RandFloat(std::uint32_t h)
    {
        return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    }

    inline XMMATRIX MakeAreaTransform(const XMFLOAT3& position, const XMFLOAT3& rotation)
    {
        // Deliberately no scale: an area's size lives in its Extent fields so
        // that scaling the entity does not silently rescale every asset the
        // area places.
        return PteroTransform::ComposeRotation(rotation)
             * XMMatrixTranslation(position.x, position.y, position.z);
    }

    // Even-odd crossing test on the XY plane.
    bool PointInPolygon(const std::vector<XMFLOAT2>& points, float x, float y)
    {
        if (points.size() < 3)
            return false;

        bool inside = false;
        for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        {
            const XMFLOAT2& a = points[i];
            const XMFLOAT2& b = points[j];

            if (((a.y > y) != (b.y > y))
                && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x))
            {
                inside = !inside;
            }
        }

        return inside;
    }

    // Containment test in the area's local space, ignoring Z.  Used to reject
    // grid candidates before the expensive surface raycast.
    bool IsInsideFootprint(const VegetationAreaComponent& area, float lx, float ly)
    {
        switch (area.Shape)
        {
        case VegetationAreaShape::Box:
            return std::fabs(lx) <= area.ExtentX && std::fabs(ly) <= area.ExtentY;

        case VegetationAreaShape::Sphere:
            return (lx * lx + ly * ly) <= (area.ExtentX * area.ExtentX);

        case VegetationAreaShape::Polygon:
            return PointInPolygon(area.PolygonPoints, lx, ly);
        }

        return false;
    }

    // Full containment test including Z, applied once the candidate has been
    // snapped onto a surface.
    bool IsInsideVolume(const VegetationAreaComponent& area, const XMFLOAT3& local)
    {
        switch (area.Shape)
        {
        case VegetationAreaShape::Box:
            return std::fabs(local.x) <= area.ExtentX
                && std::fabs(local.y) <= area.ExtentY
                && std::fabs(local.z) <= area.ExtentZ;

        case VegetationAreaShape::Sphere:
            return (local.x * local.x + local.y * local.y + local.z * local.z)
                 <= (area.ExtentX * area.ExtentX);

        case VegetationAreaShape::Polygon:
            return std::fabs(local.z) <= area.ExtentZ
                && PointInPolygon(area.PolygonPoints, local.x, local.y);
        }

        return false;
    }

    bool IsInsideExclusion(const VegetationExclusionVolume& volume, const XMFLOAT3& worldPosition)
    {
        XMFLOAT3 local{};
        XMStoreFloat3(&local, XMVector3Transform(XMLoadFloat3(&worldPosition), volume.WorldToLocal));

        switch (volume.Shape)
        {
        case VegetationAreaShape::Box:
            return std::fabs(local.x) <= volume.ExtentX
                && std::fabs(local.y) <= volume.ExtentY
                && std::fabs(local.z) <= volume.ExtentZ;

        case VegetationAreaShape::Sphere:
            return (local.x * local.x + local.y * local.y + local.z * local.z)
                 <= (volume.ExtentX * volume.ExtentX);

        case VegetationAreaShape::Polygon:
            return std::fabs(local.z) <= volume.ExtentZ
                && PointInPolygon(volume.PolygonPoints, local.x, local.y);
        }

        return false;
    }

    // Uniform spatial hash used only for self-thinning.  Cell size equals the
    // layer's collision radius, so a candidate only has to test the nine cells
    // around it.
    class SpacingGrid
    {
    public:
        explicit SpacingGrid(float cellSize)
            : mCellSize((cellSize > 1e-4f) ? cellSize : 1e-4f)
            , mInvCellSize(1.0f / ((cellSize > 1e-4f) ? cellSize : 1e-4f))
        {
        }

        bool IsClear(float x, float y, float radius) const
        {
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(x * mInvCellSize));
            const std::int32_t cy = static_cast<std::int32_t>(std::floor(y * mInvCellSize));
            const float radiusSq = radius * radius;

            for (std::int32_t oy = -1; oy <= 1; ++oy)
            {
                for (std::int32_t ox = -1; ox <= 1; ++ox)
                {
                    const auto it = mCells.find(Key(cx + ox, cy + oy));
                    if (it == mCells.end())
                        continue;

                    for (const XMFLOAT2& placed : it->second)
                    {
                        const float dx = placed.x - x;
                        const float dy = placed.y - y;
                        if (dx * dx + dy * dy < radiusSq)
                            return false;
                    }
                }
            }

            return true;
        }

        void Insert(float x, float y)
        {
            const std::int32_t cx = static_cast<std::int32_t>(std::floor(x * mInvCellSize));
            const std::int32_t cy = static_cast<std::int32_t>(std::floor(y * mInvCellSize));
            mCells[Key(cx, cy)].push_back(XMFLOAT2(x, y));
        }

    private:
        static std::uint64_t Key(std::int32_t x, std::int32_t y)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32)
                 |  static_cast<std::uint64_t>(static_cast<std::uint32_t>(y));
        }

        float mCellSize;
        float mInvCellSize;
        std::unordered_map<std::uint64_t, std::vector<XMFLOAT2>> mCells;
    };
}

std::uint64_t VegetationScatter::MakeInstanceId(
    std::uint32_t layerIndex,
    std::int32_t  cellX,
    std::int32_t  cellY)
{
    // 8 bits of layer, then 28 bits each of biased cell coordinate.  28 bits
    // covers +/-134M cells, which at any sane density is far larger than any
    // level will ever be.
    const std::uint64_t biasedX = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(cellX + (1 << 27)) & 0x0FFFFFFFu);
    const std::uint64_t biasedY = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(cellY + (1 << 27)) & 0x0FFFFFFFu);

    return (static_cast<std::uint64_t>(layerIndex & 0xFFu) << 56)
         | (biasedX << 28)
         |  biasedY;
}

void VegetationScatter::ComputeWorldBounds(
    const VegetationAreaComponent& area,
    const XMFLOAT3&                areaPosition,
    const XMFLOAT3&                areaRotation,
    XMFLOAT3&                      outMin,
    XMFLOAT3&                      outMax)
{
    // Local half-extents of whatever shape this is, as a box.
    float hx = area.ExtentX;
    float hy = area.ExtentY;
    float hz = area.ExtentZ;

    if (area.Shape == VegetationAreaShape::Sphere)
    {
        hx = hy = hz = area.ExtentX;
    }
    else if (area.Shape == VegetationAreaShape::Polygon)
    {
        hx = 0.0f;
        hy = 0.0f;
        for (const XMFLOAT2& p : area.PolygonPoints)
        {
            hx = (std::max)(hx, std::fabs(p.x));
            hy = (std::max)(hy, std::fabs(p.y));
        }
    }

    const XMMATRIX transform = MakeAreaTransform(areaPosition, areaRotation);

    outMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    outMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // Transform all eight corners rather than the extents, so a rotated area
    // still gets a bound that actually contains it.
    for (int corner = 0; corner < 8; ++corner)
    {
        const XMFLOAT3 localCorner(
            (corner & 1) ? hx : -hx,
            (corner & 2) ? hy : -hy,
            (corner & 4) ? hz : -hz);

        XMFLOAT3 worldCorner{};
        XMStoreFloat3(&worldCorner, XMVector3Transform(XMLoadFloat3(&localCorner), transform));

        outMin.x = (std::min)(outMin.x, worldCorner.x);
        outMin.y = (std::min)(outMin.y, worldCorner.y);
        outMin.z = (std::min)(outMin.z, worldCorner.z);
        outMax.x = (std::max)(outMax.x, worldCorner.x);
        outMax.y = (std::max)(outMax.y, worldCorner.y);
        outMax.z = (std::max)(outMax.z, worldCorner.z);
    }
}

VegetationScatterResult VegetationScatter::Generate(
    const VegetationAreaComponent&                area,
    const XMFLOAT3&                               areaPosition,
    const XMFLOAT3&                               areaRotation,
    const IVegetationTerrainSource*               terrain,
    const GeometryRaycaster*                      geometry,
    const std::vector<VegetationExclusionVolume>& exclusions)
{
    VegetationScatterResult result;
    result.LayerOffsets.assign(area.Layers.size() + 1, 0u);

    // An exclusion volume contributes nothing itself; it only subtracts from
    // other areas.
    if (area.IsExclusionVolume || area.Layers.empty())
        return result;

    const bool useTerrain  = area.SnapToTerrain  && terrain  != nullptr;
    const bool useGeometry = area.SnapToGeometry && geometry != nullptr && !geometry->IsEmpty();
    if (!useTerrain && !useGeometry)
        return result;

    const XMMATRIX areaTransform = MakeAreaTransform(areaPosition, areaRotation);
    const XMMATRIX worldToLocal  = XMMatrixInverse(nullptr, areaTransform);

    XMFLOAT3 boundsMin{};
    XMFLOAT3 boundsMax{};
    ComputeWorldBounds(area, areaPosition, areaRotation, boundsMin, boundsMax);

    // Index overrides once so the per-candidate lookup is a hash probe rather
    // than a linear scan of what can be a long list.
    std::unordered_map<std::uint64_t, const VegetationInstanceOverride*> overrides;
    overrides.reserve(area.Overrides.size());
    for (const VegetationInstanceOverride& ov : area.Overrides)
        overrides.emplace(ov.InstanceId, &ov);

    // Rays start just above the top of the area so a candidate cannot begin
    // underneath the surface it is meant to land on.
    const float rayStartZ = boundsMax.z + 1.0f;
    const float rayLength = (boundsMax.z - boundsMin.z) + area.SnapMaxDistance + 2.0f;

    std::vector<std::vector<VegetationInstance>> perLayer(area.Layers.size());

    for (std::size_t layerIndex = 0; layerIndex < area.Layers.size(); ++layerIndex)
    {
        const VegetationLayer& layer = area.Layers[layerIndex];
        if (!layer.Enabled || layer.Density <= 0.0f || layer.MeshPath.empty())
            continue;

        std::vector<VegetationInstance>& output = perLayer[layerIndex];

        // One candidate per cell, so cell area is the reciprocal of density.
        const float cellSize = 1.0f / std::sqrt(layer.Density);

        const std::int32_t cellMinX = static_cast<std::int32_t>(std::floor(boundsMin.x / cellSize));
        const std::int32_t cellMaxX = static_cast<std::int32_t>(std::ceil (boundsMax.x / cellSize));
        const std::int32_t cellMinY = static_cast<std::int32_t>(std::floor(boundsMin.y / cellSize));
        const std::int32_t cellMaxY = static_cast<std::int32_t>(std::ceil (boundsMax.y / cellSize));

        const float cosMaxSlope = std::cos(layer.MaxSlopeDegrees * (kPi / 180.0f));

        const bool selfThin = layer.CollisionRadius > 1e-4f;
        SpacingGrid spacingGrid(layer.CollisionRadius);

        const std::uint32_t layerSeed = HashCombine(area.Seed, static_cast<std::uint32_t>(layerIndex));

        for (std::int32_t cellY = cellMinY; cellY <= cellMaxY; ++cellY)
        {
            for (std::int32_t cellX = cellMinX; cellX <= cellMaxX; ++cellX)
            {
                if (result.Stats.CandidatesConsidered >= static_cast<std::uint32_t>(kVegetationMaxInstances) * 8u)
                {
                    // The density is high enough that we are burning time on
                    // candidates that can never fit under the instance cap.
                    ++result.Stats.ClampedByInstanceCap;
                    break;
                }

                ++result.Stats.CandidatesConsidered;

                const std::uint32_t cellHash = HashCombine(
                    HashCombine(layerSeed, static_cast<std::uint32_t>(cellX)),
                    static_cast<std::uint32_t>(cellY));

                // Jitter inside the cell.  This is what turns a regular lattice
                // into something that reads as natural scatter.
                const float jitterX = RandFloat(Hash32(cellHash ^ 0x1u));
                const float jitterY = RandFloat(Hash32(cellHash ^ 0x2u));

                const float worldX = (static_cast<float>(cellX) + jitterX) * cellSize;
                const float worldY = (static_cast<float>(cellY) + jitterY) * cellSize;

                const std::uint64_t instanceId =
                    MakeInstanceId(static_cast<std::uint32_t>(layerIndex), cellX, cellY);

                // An explicit removal short-circuits everything below it.
                const auto overrideIt = overrides.find(instanceId);
                const VegetationInstanceOverride* instanceOverride =
                    (overrideIt != overrides.end()) ? overrideIt->second : nullptr;

                if (instanceOverride != nullptr && instanceOverride->Removed)
                {
                    ++result.Stats.RejectedOverride;
                    continue;
                }

                // Cheap footprint reject before any raycasting.
                XMFLOAT3 candidateWorld(worldX, worldY, areaPosition.z);
                XMFLOAT3 candidateLocal{};
                XMStoreFloat3(&candidateLocal,
                    XMVector3Transform(XMLoadFloat3(&candidateWorld), worldToLocal));

                if (!IsInsideFootprint(area, candidateLocal.x, candidateLocal.y))
                {
                    ++result.Stats.RejectedOutsideShape;
                    continue;
                }

                // --- Resolve the surface under the candidate -----------------
                bool     foundSurface = false;
                float    surfaceZ     = 0.0f;
                XMFLOAT3 surfaceNormal(0.0f, 0.0f, 1.0f);
                XMFLOAT4 layerWeights(1.0f, 0.0f, 0.0f, 0.0f);
                bool     hasLayerWeights = false;

                if (useTerrain)
                {
                    VegetationSurfaceSample sample;
                    if (terrain->SampleSurface(XMFLOAT2(worldX, worldY), sample))
                    {
                        foundSurface    = true;
                        surfaceZ        = sample.Height;
                        surfaceNormal   = sample.Normal;
                        layerWeights    = sample.LayerWeights;
                        hasLayerWeights = sample.HasLayerWeights;
                    }
                }

                if (useGeometry)
                {
                    GeometryRayHit hit;
                    if (geometry->RaycastDown(XMFLOAT2(worldX, worldY), rayStartZ, rayLength, hit))
                    {
                        // With both sources active the higher surface wins, so
                        // vegetation sits on a rock resting on terrain rather
                        // than inside it.
                        if (!foundSurface || hit.Position.z > surfaceZ)
                        {
                            foundSurface  = true;
                            surfaceZ      = hit.Position.z;
                            surfaceNormal = hit.Normal;
                            // Geometry carries no splat weights; a layer mask
                            // therefore cannot be satisfied on a mesh surface.
                            hasLayerWeights = false;
                        }
                    }
                }

                if (!foundSurface)
                {
                    ++result.Stats.RejectedNoSurface;
                    continue;
                }

                // --- Filters --------------------------------------------------
                if (surfaceNormal.z < cosMaxSlope)
                {
                    ++result.Stats.RejectedSlope;
                    continue;
                }

                if (surfaceZ < layer.MinAltitude || surfaceZ > layer.MaxAltitude)
                {
                    ++result.Stats.RejectedAltitude;
                    continue;
                }

                if (layer.TerrainLayerMask >= 0)
                {
                    if (!hasLayerWeights)
                    {
                        ++result.Stats.RejectedLayerMask;
                        continue;
                    }

                    const float weight =
                        (layer.TerrainLayerMask == 0) ? layerWeights.x :
                        (layer.TerrainLayerMask == 1) ? layerWeights.y :
                        (layer.TerrainLayerMask == 2) ? layerWeights.z : layerWeights.w;

                    if (weight < layer.TerrainLayerThreshold)
                    {
                        ++result.Stats.RejectedLayerMask;
                        continue;
                    }
                }

                // Re-test containment now that we know the real height, so a
                // box area does not capture ground far below it.
                XMFLOAT3 placedWorld(worldX, worldY, surfaceZ);
                XMFLOAT3 placedLocal{};
                XMStoreFloat3(&placedLocal,
                    XMVector3Transform(XMLoadFloat3(&placedWorld), worldToLocal));

                if (!IsInsideVolume(area, placedLocal))
                {
                    ++result.Stats.RejectedOutsideShape;
                    continue;
                }

                bool excluded = false;
                for (const VegetationExclusionVolume& volume : exclusions)
                {
                    if (IsInsideExclusion(volume, placedWorld))
                    {
                        excluded = true;
                        break;
                    }
                }

                if (excluded)
                {
                    ++result.Stats.RejectedExclusion;
                    continue;
                }

                if (selfThin && !spacingGrid.IsClear(worldX, worldY, layer.CollisionRadius))
                {
                    ++result.Stats.RejectedSpacing;
                    continue;
                }

                // --- Build the instance ---------------------------------------
                VegetationInstance instance;
                instance.InstanceId = instanceId;
                instance.LayerIndex = static_cast<std::uint32_t>(layerIndex);

                const float scaleRand = RandFloat(Hash32(cellHash ^ 0x3u));
                instance.Scale = layer.MinScale + (layer.MaxScale - layer.MinScale) * scaleRand;

                instance.RotationZ = layer.RandomYaw
                    ? RandFloat(Hash32(cellHash ^ 0x4u)) * kTwoPi
                    : 0.0f;

                // Blend between world up and the surface normal.  Trees stay
                // vertical on a slope; ground cover follows it.
                const XMVECTOR worldUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
                XMVECTOR up = XMVector3Normalize(
                    XMVectorLerp(worldUp, XMLoadFloat3(&surfaceNormal),
                                 (std::clamp)(layer.AlignToNormal, 0.0f, 1.0f)));

                if (layer.MaxTiltDegrees > 0.0f)
                {
                    // Random tilt about a random horizontal axis, so the extra
                    // lean has no preferred direction.
                    const float tiltAngle = RandFloat(Hash32(cellHash ^ 0x5u))
                                          * layer.MaxTiltDegrees * (kPi / 180.0f);
                    const float tiltDir   = RandFloat(Hash32(cellHash ^ 0x6u)) * kTwoPi;
                    const XMVECTOR tiltAxis = XMVectorSet(std::cos(tiltDir), std::sin(tiltDir), 0.0f, 0.0f);
                    up = XMVector3Normalize(
                        XMVector3Rotate(up, XMQuaternionRotationAxis(tiltAxis, tiltAngle)));
                }

                XMStoreFloat3(&instance.UpAxis, up);

                // Sink along the resolved up axis so the base is buried rather
                // than the whole instance being pushed straight down.
                XMFLOAT3 upAxis = instance.UpAxis;
                instance.Position = XMFLOAT3(
                    worldX - upAxis.x * layer.SinkOffset,
                    worldY - upAxis.y * layer.SinkOffset,
                    surfaceZ - upAxis.z * layer.SinkOffset);

                // A transform override replaces the generated placement but
                // keeps the instance's identity, so the artist's edit survives
                // a density or seed change of a different layer.
                if (instanceOverride != nullptr && instanceOverride->HasTransform)
                {
                    instance.Position  = instanceOverride->Position;
                    instance.RotationZ = instanceOverride->RotationZ;
                    instance.Scale     = instanceOverride->Scale;
                }

                if (selfThin)
                    spacingGrid.Insert(worldX, worldY);

                output.push_back(instance);
            }
        }
    }

    // Flatten into one buffer, grouped by layer, so the renderer can draw a
    // layer as a contiguous instance range.
    std::size_t total = 0;
    for (const std::vector<VegetationInstance>& layerInstances : perLayer)
        total += layerInstances.size();

    if (total > static_cast<std::size_t>(kVegetationMaxInstances))
    {
        result.Stats.ClampedByInstanceCap =
            static_cast<std::uint32_t>(total - static_cast<std::size_t>(kVegetationMaxInstances));
    }

    result.Instances.reserve((std::min)(total, static_cast<std::size_t>(kVegetationMaxInstances)));

    for (std::size_t layerIndex = 0; layerIndex < perLayer.size(); ++layerIndex)
    {
        result.LayerOffsets[layerIndex] = static_cast<std::uint32_t>(result.Instances.size());

        for (const VegetationInstance& instance : perLayer[layerIndex])
        {
            if (result.Instances.size() >= static_cast<std::size_t>(kVegetationMaxInstances))
                break;

            result.Instances.push_back(instance);
        }
    }

    result.LayerOffsets.back() = static_cast<std::uint32_t>(result.Instances.size());
    return result;
}
