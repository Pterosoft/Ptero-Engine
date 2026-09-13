#pragma once

// TerrainGeometry
// ---------------
// Pure CPU helpers for converting a heightmap grid into a triangle mesh
// (matching the existing System/Mesh.h :: Vertex layout) and for applying
// the editor's brush operations (raise, lower, flatten, smooth) to the
// heightmap grid in-place.
//
// All functions are engine-agnostic so they can be unit-tested without
// touching D3D.

#include "Components.h"   // for the TerrainComponent::BrushType enum

#include <DirectXMath.h>

#include <cstdint>
#include <cstddef>
#include <vector>

struct TerrainVertex
{
    DirectX::XMFLOAT3 Position{};
    DirectX::XMFLOAT3 Normal{};
    DirectX::XMFLOAT2 TexCoord{};
    DirectX::XMFLOAT4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

namespace TerrainGeometry
{
    // Convert a row-major uint16 heightmap (0..65535) into world-space
    // vertices on a square patch centred on the local origin.  vertices is
    // laid out row-major: index = y * width + x.  indices is two triangles
    // per quad, total (width-1)*(height-1)*6 entries.
    // `layerWeights`, when non-null, must be row-major with one XMFLOAT4 per
    // sample (index = y*width + x).  The four channels are the per-sample
    // blend weights of terrain paint layers 0..3 and are copied into the
    // vertex COLOR channel so the terrain pixel shader can splat-blend the
    // layer textures.  When null the vertex colour defaults to white (the
    // legacy single-material path).
    void BuildMesh(
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        float heightScale,
        float heightOffset,
        std::vector<TerrainVertex>& outVertices,
        std::vector<std::uint32_t>&  outIndices,
        const std::vector<DirectX::XMFLOAT4>* layerWeights = nullptr);

    // Brush operations.  Each takes a brush position in world units on the
    // terrain ground plane (the terrain lives on the XY plane, centred on the origin) and
    // the brush radius/strength from the component.  samples is the in-out
    // heightmap (0..65535) that will be re-encoded into the DDS on save.
    //
    // raise/lower add/subtract `strength` to every sample inside the brush,
    // weighted by a smooth falloff that goes to zero at the rim.
    // flatten pulls every sample inside the brush toward `flattenHeight`
    // (in the same 0..65535 unit) with the same falloff.
    // smooth runs a small box-blur pass (BrushSmoothingPasses times) over
    // every sample inside the brush.
    void ApplyRaiseBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        float heightScale);

    void ApplyLowerBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        float heightScale);

    void ApplyFlattenBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float flattenHeightWorld,
        float heightScale,
        float heightOffset);

    void ApplySmoothBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        int   passes,
        float heightScale);

    // Paint the active layer into the per-sample splat weights.  Each weight
    // is an XMFLOAT4 (layers 0..3).  Inside the brush falloff the active
    // layer's channel is pushed up by `strength` and all four channels are
    // renormalised so they always sum to 1, which is what the pixel-shader
    // blend expects.  `activeLayer` is clamped to [0,3].
    void ApplyPaintBrush(
        std::vector<DirectX::XMFLOAT4>& weights,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        int   activeLayer);

    // Convert a world-space XZ point to a (col, row) cell index.  Returns
    // false if the point lies outside the terrain.  cellX/cellY are floats
    // in [0, width-1] / [0, height-1]; use CellIndexFromPoint for sampling.
    bool WorldToCell(
        const DirectX::XMFLOAT2& worldXZ,
        int width,
        int height,
        float worldSize,
        float& outCellX,
        float& outCellY);

    // Pick the nearest cell to a world XY ground-plane position; clamps to grid edges.
    void ClampCell(
        int width,
        int height,
        int& ioCellX,
        int& ioCellY);

    // Heightmap value -> world Y, in metres.  Uses the same formula as
    // BuildMesh, so the brush preview and the rendered mesh stay in sync.
    inline float SampleToWorldHeight(std::uint16_t sample, float heightScale, float heightOffset)
    {
        return (static_cast<float>(sample) / 65535.0f) * heightScale + heightOffset;
    }

    inline std::uint16_t WorldHeightToSample(float worldHeight, float heightScale, float heightOffset)
    {
        const float normalised = (worldHeight - heightOffset) / (heightScale <= 0.0f ? 1.0f : heightScale);
        const float clamped = (normalised < 0.0f) ? 0.0f : (normalised > 1.0f ? 1.0f : normalised);
        return static_cast<std::uint16_t>(clamped * 65535.0f + 0.5f);
    }
}
