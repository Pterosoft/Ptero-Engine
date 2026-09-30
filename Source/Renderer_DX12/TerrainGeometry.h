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
    // Inclusive rectangle of grid cells (heightmap samples or mesh vertices).
    // Brush strokes record one so only the touched part of the mesh is rebuilt.
    struct GridRect
    {
        int MinX = 0;
        int MinY = 0;
        int MaxX = -1;
        int MaxY = -1;

        bool IsEmpty() const { return MaxX < MinX || MaxY < MinY; }

        void Merge(const GridRect& other)
        {
            if (other.IsEmpty())
                return;
            if (IsEmpty())
            {
                *this = other;
                return;
            }
            MinX = (MinX < other.MinX) ? MinX : other.MinX;
            MinY = (MinY < other.MinY) ? MinY : other.MinY;
            MaxX = (MaxX > other.MaxX) ? MaxX : other.MaxX;
            MaxY = (MaxY > other.MaxY) ? MaxY : other.MaxY;
        }

        static GridRect Full(int width, int height) { return { 0, 0, width - 1, height - 1 }; }
    };

    // The heightmap samples a brush at `brushCenterWorld` with `brushRadius`
    // can touch, clamped to the grid.  Empty when the brush misses the patch.
    GridRect BrushSampleRect(
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius);

    // The mesh vertices whose position depends on any source sample inside
    // `sourceRect`, when a (meshWidth x meshHeight) mesh is resampled from a
    // (width x height) heightmap.  Grown by one vertex so the normals around
    // the edit are recomputed too.
    GridRect SourceRectToMeshRect(
        const GridRect& sourceRect,
        int width,
        int height,
        int meshWidth,
        int meshHeight);

    // Fill positions, texcoords and colours of the mesh vertices inside
    // `meshRect`.  The mesh may be coarser than the heightmap (it is capped
    // for VRAM); each vertex then takes a bilinear sample of the heightmap at
    // its exact position rather than snapping to the nearest sample, which
    // would drop most of the source and alias the surface.  `vertices` must
    // already hold meshWidth*meshHeight entries.
    void FillMeshVertices(
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        const std::vector<DirectX::XMFLOAT4>* layerWeights,
        int meshWidth,
        int meshHeight,
        float worldSize,
        float heightScale,
        float heightOffset,
        const GridRect& meshRect,
        std::vector<TerrainVertex>& vertices);

    // Recompute the normals of the vertices inside `meshRect` from their
    // neighbours' positions (one-sided differences at the patch border).
    void ComputeMeshNormals(
        int meshWidth,
        int meshHeight,
        const GridRect& meshRect,
        std::vector<TerrainVertex>& vertices);

    // Two triangles per grid quad, (meshWidth-1)*(meshHeight-1)*6 indices.
    void BuildMeshIndices(
        int meshWidth,
        int meshHeight,
        std::vector<std::uint32_t>& outIndices);

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
    // Every brush is applied once per frame while the mouse is held, so the
    // amounts are per application (the caller scales them by frame time).
    // raise/lower add/subtract `strength` metres to every sample inside the
    // brush, weighted by a smooth falloff that goes to zero at the rim.
    // flatten moves every sample inside the brush `blend` of the way toward
    // `flattenHeightWorld` (scaled by the same falloff).
    // smooth runs a small box-blur pass (`passes` times) over every sample
    // inside the brush and blends `blend` of the way toward the result.
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
        float heightOffset,
        float blend);

    void ApplySmoothBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        int   passes,
        float blend);

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
