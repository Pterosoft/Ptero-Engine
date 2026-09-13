#pragma once

// VegetationScatter
// -----------------
// Turns a VegetationAreaComponent into a concrete list of instance transforms.
//
// The distribution is fully deterministic: every candidate is derived from a
// hash of (seed, layer, cellX, cellY) and nothing carries state between
// candidates.  That has three consequences worth knowing about:
//
//   * The level file stores rules, not a baked list of a few hundred thousand
//     transforms.
//   * Re-running the scatter with the same inputs reproduces the same world,
//     so an artist can move an area away and back without the vegetation
//     rearranging itself.
//   * Because each cell is independent, a future streaming pass can generate
//     any subset of cells on demand without generating the rest.
//
// The one exception is self-thinning (VegetationLayer::CollisionRadius), which
// by definition depends on what has already been placed.  It is applied in a
// fixed cell iteration order so it stays deterministic.
//
// This file is engine-agnostic and free of D3D so it can be unit-tested,
// matching how TerrainGeometry is structured.  Terrain is reached through the
// IVegetationTerrainSource interface (TerrainRenderer implements it via a thin
// adapter) and static geometry through GeometryRaycaster.
//
// World space is Z-up and left-handed, so "down" is -Z.

#include "Components.h"
#include "GeometryRaycaster.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// What the terrain looks like at one ground-plane position.
struct VegetationSurfaceSample
{
    float             Height = 0.0f;
    DirectX::XMFLOAT3 Normal{ 0.0f, 0.0f, 1.0f };

    // Splat weights for terrain paint layers 0..3.  HasLayerWeights is false
    // on terrain that has never been painted, in which case a layer mask
    // cannot be evaluated and the scatter treats the mask as unsatisfied.
    DirectX::XMFLOAT4 LayerWeights{ 1.0f, 0.0f, 0.0f, 0.0f };
    bool              HasLayerWeights = false;
};

// Abstract terrain query, so the scatter does not need to know about
// TerrainRenderer (and therefore about D3D).
class IVegetationTerrainSource
{
public:
    virtual ~IVegetationTerrainSource() = default;

    // Returns false when the position lies outside every terrain patch.
    virtual bool SampleSurface(
        const DirectX::XMFLOAT2& worldXY,
        VegetationSurfaceSample& outSample) const = 0;
};

// A volume that removes vegetation, gathered from other entities in the scene
// whose area component has IsExclusionVolume set.  Stored pre-inverted because
// the scatter tests every surviving candidate against every exclusion.
struct VegetationExclusionVolume
{
    VegetationAreaShape Shape = VegetationAreaShape::Box;
    DirectX::XMMATRIX   WorldToLocal = DirectX::XMMatrixIdentity();
    float               ExtentX = 0.0f;
    float               ExtentY = 0.0f;
    float               ExtentZ = 0.0f;
    std::vector<DirectX::XMFLOAT2> PolygonPoints;
};

// One placed instance, in world space.
struct VegetationInstance
{
    DirectX::XMFLOAT3 Position{};

    // Yaw about world +Z, radians.
    float RotationZ = 0.0f;

    // The up axis the instance was planted along, after AlignToNormal has
    // blended between world up and the surface normal.  The renderer builds
    // the instance basis from this.
    DirectX::XMFLOAT3 UpAxis{ 0.0f, 0.0f, 1.0f };

    float Scale = 1.0f;

    // Stable identity used to match sparse artist overrides across a
    // regeneration.  See VegetationInstanceOverride.
    std::uint64_t InstanceId = 0;

    std::uint32_t LayerIndex = 0;
};

// Why candidates were discarded.  Surfaced in the editor because "nothing is
// spawning" is by far the most common question with area-based scattering, and
// without this breakdown the artist has no way to tell a too-steep slope from
// an unsatisfied paint mask.
struct VegetationScatterStats
{
    std::uint32_t CandidatesConsidered = 0;
    std::uint32_t RejectedOutsideShape = 0;
    std::uint32_t RejectedNoSurface    = 0;
    std::uint32_t RejectedSlope        = 0;
    std::uint32_t RejectedAltitude     = 0;
    std::uint32_t RejectedLayerMask    = 0;
    std::uint32_t RejectedSpacing      = 0;
    std::uint32_t RejectedExclusion    = 0;
    std::uint32_t RejectedOverride     = 0;
    std::uint32_t ClampedByInstanceCap = 0;
};

struct VegetationScatterResult
{
    // Sorted by LayerIndex so the renderer can issue one draw per layer
    // without a second pass.
    std::vector<VegetationInstance> Instances;

    // Prefix offsets into Instances, size = layer count + 1.  Layer i owns
    // [LayerOffsets[i], LayerOffsets[i+1]).
    std::vector<std::uint32_t> LayerOffsets;

    VegetationScatterStats Stats;
};

namespace VegetationScatter
{
    // Generate every instance for one area.
    //
    // `areaPosition` and `areaRotation` come from the entity's transform.  Note
    // that TransformComponent::Scale and its MeshWorldScale factor are
    // deliberately ignored: an area's size is its Extent fields, in metres, so
    // that resizing a volume does not also rescale the assets inside it.
    //
    // `terrain` and `geometry` may each be null; if both are, or if the area
    // has neither snap mode enabled, nothing is placed.
    VegetationScatterResult Generate(
        const VegetationAreaComponent&                 area,
        const DirectX::XMFLOAT3&                       areaPosition,
        const DirectX::XMFLOAT3&                       areaRotation,
        const IVegetationTerrainSource*                terrain,
        const GeometryRaycaster*                       geometry,
        const std::vector<VegetationExclusionVolume>&  exclusions);

    // Build the world-space AABB the area covers, so callers can size the
    // GeometryRaycaster build region to match.
    void ComputeWorldBounds(
        const VegetationAreaComponent& area,
        const DirectX::XMFLOAT3&       areaPosition,
        const DirectX::XMFLOAT3&       areaRotation,
        DirectX::XMFLOAT3&             outMin,
        DirectX::XMFLOAT3&             outMax);

    // Pack a stable per-instance identity from its layer and grid cell.  Cell
    // coordinates are biased so negative cells survive the pack.
    std::uint64_t MakeInstanceId(std::uint32_t layerIndex, std::int32_t cellX, std::int32_t cellY);
}
