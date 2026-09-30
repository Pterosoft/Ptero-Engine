// Enable M_PI / M_E from <cmath> on MSVC.  Must come before any header
// that pulls in <cmath>, so we set it before the PCH.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include "pch.h"

#include "TerrainGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

// MSVC's <cmath> deliberately does not expose M_PI even with
// _USE_MATH_DEFINES (unlike the CRT <math.h>).  Provide a portable
// fallback so the file compiles on any toolchain.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TerrainGeometry
{
    namespace
    {
        // Convert (cellX, cellY) in [0, width-1] / [0, height-1] to a world
        // XY position centred on the local origin. Z is sampled separately
        // by the caller (brush ops need a flat ground-plane lookup).
        inline DirectX::XMFLOAT2 CellToWorld(int cellX, int cellY, int width, int height, float worldSize)
        {
            const float halfSize = worldSize * 0.5f;
            const float x = (static_cast<float>(cellX) / static_cast<float>(width  - 1)) * worldSize - halfSize;
            const float y = (static_cast<float>(cellY) / static_cast<float>(height - 1)) * worldSize - halfSize;
            return DirectX::XMFLOAT2(x, y);
        }

        // Smooth (cosine) falloff that goes 1 at the centre, 0 at the rim.
        // Squared for a softer edge.
        inline float Falloff(float distanceFromCentre, float radius)
        {
            if (radius <= 0.0f)
                return 0.0f;
            const float t = distanceFromCentre / radius;
            if (t >= 1.0f)
                return 0.0f;
            const float smooth = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * t);
            return smooth * smooth;
        }
    }

    GridRect BrushSampleRect(
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius)
    {
        GridRect rect;
        if (width <= 1 || height <= 1 || worldSize <= 0.0f)
            return rect;

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        // The brushes do not agree on which axis sizes the footprint (some use
        // the X cell size for both), which only matters on a non-square grid.
        // Take the larger radius on both axes so the rect covers all of them.
        const float radiusCells = (std::max)(
            brushRadius / (worldSize / static_cast<float>(width  - 1)),
            brushRadius / (worldSize / static_cast<float>(height - 1)));

        rect.MinX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCells)));
        rect.MaxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCells)));
        rect.MinY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCells)));
        rect.MaxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCells)));
        return (rect.MinX > rect.MaxX || rect.MinY > rect.MaxY) ? GridRect{} : rect;
    }

    GridRect SourceRectToMeshRect(
        const GridRect& sourceRect,
        int width,
        int height,
        int meshWidth,
        int meshHeight)
    {
        if (sourceRect.IsEmpty() || width <= 1 || height <= 1 || meshWidth <= 1 || meshHeight <= 1)
            return {};

        // Mesh vertex m samples the heightmap at m * (src-1)/(mesh-1) and its
        // bilinear footprint reaches one sample either side of that.
        const double toMeshX = static_cast<double>(meshWidth  - 1) / static_cast<double>(width  - 1);
        const double toMeshY = static_cast<double>(meshHeight - 1) / static_cast<double>(height - 1);

        GridRect rect;
        rect.MinX = static_cast<int>(std::floor((sourceRect.MinX - 1) * toMeshX)) - 1;
        rect.MaxX = static_cast<int>(std::ceil ((sourceRect.MaxX + 1) * toMeshX)) + 1;
        rect.MinY = static_cast<int>(std::floor((sourceRect.MinY - 1) * toMeshY)) - 1;
        rect.MaxY = static_cast<int>(std::ceil ((sourceRect.MaxY + 1) * toMeshY)) + 1;
        rect.MinX = (std::max)(0, rect.MinX);
        rect.MinY = (std::max)(0, rect.MinY);
        rect.MaxX = (std::min)(meshWidth  - 1, rect.MaxX);
        rect.MaxY = (std::min)(meshHeight - 1, rect.MaxY);
        return rect;
    }

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
        std::vector<TerrainVertex>& vertices)
    {
        if (width <= 1 || height <= 1 || meshWidth <= 1 || meshHeight <= 1 || meshRect.IsEmpty())
            return;
        if (static_cast<size_t>(width) * static_cast<size_t>(height) != samples.size())
            return;
        if (static_cast<size_t>(meshWidth) * static_cast<size_t>(meshHeight) != vertices.size())
            return;

        // Only consume the splat weights if they match the sample grid; a
        // mismatched buffer (e.g. mid-resize) falls back to white so we never
        // read out of bounds.
        const bool useWeights = layerWeights != nullptr
            && layerWeights->size() == samples.size();

        const float halfSize = worldSize * 0.5f;
        const float xStep = worldSize / static_cast<float>(meshWidth  - 1);
        const float yStep = worldSize / static_cast<float>(meshHeight - 1);
        const double toSourceX = static_cast<double>(width  - 1) / static_cast<double>(meshWidth  - 1);
        const double toSourceY = static_cast<double>(height - 1) / static_cast<double>(meshHeight - 1);
        const size_t stride = static_cast<size_t>(width);

        for (int y = meshRect.MinY; y <= meshRect.MaxY; ++y)
        {
            const double sourceY = y * toSourceY;
            const int y0 = (std::min)(height - 2, static_cast<int>(sourceY));
            const float ty = static_cast<float>(sourceY - y0);

            for (int x = meshRect.MinX; x <= meshRect.MaxX; ++x)
            {
                const double sourceX = x * toSourceX;
                const int x0 = (std::min)(width - 2, static_cast<int>(sourceX));
                const float tx = static_cast<float>(sourceX - x0);

                const size_t i00 = static_cast<size_t>(y0) * stride + static_cast<size_t>(x0);
                const size_t i10 = i00 + 1;
                const size_t i01 = i00 + stride;
                const size_t i11 = i01 + 1;

                const float h0 = static_cast<float>(samples[i00]) * (1.0f - tx) + static_cast<float>(samples[i10]) * tx;
                const float h1 = static_cast<float>(samples[i01]) * (1.0f - tx) + static_cast<float>(samples[i11]) * tx;
                const float normalisedHeight = (h0 * (1.0f - ty) + h1 * ty) / 65535.0f;

                TerrainVertex& v = vertices[static_cast<size_t>(y) * static_cast<size_t>(meshWidth) + static_cast<size_t>(x)];
                v.Position = DirectX::XMFLOAT3(
                    -halfSize + static_cast<float>(x) * xStep,
                    -halfSize + static_cast<float>(y) * yStep,
                    normalisedHeight * heightScale + heightOffset);
                v.TexCoord = DirectX::XMFLOAT2(
                    static_cast<float>(x) / static_cast<float>(meshWidth  - 1),
                    static_cast<float>(y) / static_cast<float>(meshHeight - 1));

                if (useWeights)
                {
                    const std::vector<DirectX::XMFLOAT4>& w = *layerWeights;
                    const DirectX::XMVECTOR row0 = DirectX::XMVectorLerp(
                        DirectX::XMLoadFloat4(&w[i00]), DirectX::XMLoadFloat4(&w[i10]), tx);
                    const DirectX::XMVECTOR row1 = DirectX::XMVectorLerp(
                        DirectX::XMLoadFloat4(&w[i01]), DirectX::XMLoadFloat4(&w[i11]), tx);
                    DirectX::XMStoreFloat4(&v.Color, DirectX::XMVectorLerp(row0, row1, ty));
                }
                else
                {
                    v.Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
                }
            }
        }
    }

    void ComputeMeshNormals(
        int meshWidth,
        int meshHeight,
        const GridRect& meshRect,
        std::vector<TerrainVertex>& vertices)
    {
        if (meshWidth <= 1 || meshHeight <= 1 || meshRect.IsEmpty())
            return;
        if (static_cast<size_t>(meshWidth) * static_cast<size_t>(meshHeight) != vertices.size())
            return;

        const auto at = [&](int x, int y) -> const DirectX::XMFLOAT3&
        {
            return vertices[static_cast<size_t>(y) * static_cast<size_t>(meshWidth) + static_cast<size_t>(x)].Position;
        };

        for (int y = meshRect.MinY; y <= meshRect.MaxY; ++y)
        {
            const int yU = (y > 0)              ? y - 1 : y;
            const int yD = (y < meshHeight - 1) ? y + 1 : y;
            for (int x = meshRect.MinX; x <= meshRect.MaxX; ++x)
            {
                const int xL = (x > 0)             ? x - 1 : x;
                const int xR = (x < meshWidth - 1) ? x + 1 : x;

                const DirectX::XMFLOAT3& pL = at(xL, y);
                const DirectX::XMFLOAT3& pR = at(xR, y);
                const DirectX::XMFLOAT3& pU = at(x, yU);
                const DirectX::XMFLOAT3& pD = at(x, yD);

                const float slopeX = (pR.z - pL.z) / (pR.x - pL.x);
                const float slopeY = (pD.z - pU.z) / (pD.y - pU.y);

                // Terrain is Z-up on the XY plane, so the gradient gives the
                // normal directly as (-dz/dx, -dz/dy, 1).
                DirectX::XMFLOAT3 normal(-slopeX, -slopeY, 1.0f);
                DirectX::XMStoreFloat3(&normal, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&normal)));
                vertices[static_cast<size_t>(y) * static_cast<size_t>(meshWidth) + static_cast<size_t>(x)].Normal = normal;
            }
        }
    }

    void BuildMeshIndices(
        int meshWidth,
        int meshHeight,
        std::vector<std::uint32_t>& outIndices)
    {
        outIndices.clear();
        if (meshWidth <= 1 || meshHeight <= 1)
            return;

        // Two triangles per quad, CCW when looking down -Z. Written through a raw
        // pointer: a full-size terrain is six million indices, and six million
        // push_backs are a second or more of the load frame in a Debug build.
        outIndices.resize(static_cast<size_t>(meshWidth - 1) * static_cast<size_t>(meshHeight - 1) * 6u);
        std::uint32_t* out = outIndices.data();
        for (int y = 0; y < meshHeight - 1; ++y)
        {
            for (int x = 0; x < meshWidth - 1; ++x)
            {
                const std::uint32_t i00 = static_cast<std::uint32_t>(y       * meshWidth + x);
                const std::uint32_t i10 = static_cast<std::uint32_t>(y       * meshWidth + x + 1);
                const std::uint32_t i01 = static_cast<std::uint32_t>((y + 1) * meshWidth + x);
                const std::uint32_t i11 = static_cast<std::uint32_t>((y + 1) * meshWidth + x + 1);

                *out++ = i00;
                *out++ = i01;
                *out++ = i11;
                *out++ = i00;
                *out++ = i11;
                *out++ = i10;
            }
        }
    }

    void BuildMesh(
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        float heightScale,
        float heightOffset,
        std::vector<TerrainVertex>& outVertices,
        std::vector<std::uint32_t>&  outIndices,
        const std::vector<DirectX::XMFLOAT4>* layerWeights)
    {
        outVertices.clear();
        outIndices.clear();

        if (width <= 1 || height <= 1 || samples.empty())
            return;
        if (static_cast<size_t>(width) * static_cast<size_t>(height) != samples.size())
            return;

        outVertices.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        const GridRect all = GridRect::Full(width, height);
        FillMeshVertices(samples, width, height, layerWeights, width, height,
                         worldSize, heightScale, heightOffset, all, outVertices);
        ComputeMeshNormals(width, height, all, outVertices);
        BuildMeshIndices(width, height, outIndices);
    }

    bool WorldToCell(
        const DirectX::XMFLOAT2& worldXZ,
        int width,
        int height,
        float worldSize,
        float& outCellX,
        float& outCellY)
    {
        if (width <= 1 || height <= 1)
            return false;

        const float halfSize = worldSize * 0.5f;
        if (worldXZ.x < -halfSize || worldXZ.x > halfSize
         || worldXZ.y < -halfSize || worldXZ.y > halfSize)
            return false;

        outCellX = ((worldXZ.x + halfSize) / worldSize) * static_cast<float>(width  - 1);
        outCellY = ((worldXZ.y + halfSize) / worldSize) * static_cast<float>(height - 1);
        return true;
    }

    void ClampCell(int width, int height, int& ioCellX, int& ioCellY)
    {
        ioCellX = (std::max)(0, (std::min)(ioCellX, width  - 1));
        ioCellY = (std::max)(0, (std::min)(ioCellY, height - 1));
    }

    void ApplyRaiseBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        float heightScale)
    {
        if (samples.empty() || width <= 0 || height <= 0)
            return;
        if (heightScale <= 0.0f)
            heightScale = 1.0f;

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        const float cellSizeX   = worldSize / static_cast<float>(width  - 1);
        const float cellSizeY   = worldSize / static_cast<float>(height - 1);
        const float radiusCellsX = brushRadius / cellSizeX;
        const float radiusCellsY = brushRadius / cellSizeY;

        const int minX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCellsX)));
        const int maxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCellsX)));
        const int minY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCellsY)));
        const int maxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCellsY)));

        const float deltaSamples = (strength / heightScale) * 65535.0f;

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float dx = static_cast<float>(x) - centreCellX;
                const float dy = static_cast<float>(y) - centreCellY;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float weight   = Falloff(distance, brushRadius / cellSizeX);
                if (weight <= 0.0f)
                    continue;

                // lround, not +0.5 and truncate: truncation rounds a negative
                // (Lower) delta toward zero, so the lower brush dug less than
                // the raise brush built up.
                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                int newValue = static_cast<int>(samples[index]) + static_cast<int>(std::lround(deltaSamples * weight));
                newValue = (std::max)(0, (std::min)(65535, newValue));
                samples[index] = static_cast<std::uint16_t>(newValue);
            }
        }
    }

    void ApplyLowerBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        float heightScale)
    {
        ApplyRaiseBrush(
            samples, width, height, worldSize,
            brushCenterWorld, brushRadius, -strength, heightScale);
    }

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
        float blend)
    {
        if (samples.empty() || width <= 0 || height <= 0)
            return;
        if (heightScale <= 0.0f)
            heightScale = 1.0f;
        blend = (std::max)(0.0f, (std::min)(1.0f, blend));

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        const float cellSizeX   = worldSize / static_cast<float>(width  - 1);
        const float radiusCells = brushRadius / cellSizeX;

        const int minX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCells)));
        const int maxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCells)));
        const int minY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCells)));
        const int maxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCells)));

        const int targetSample = static_cast<int>(WorldHeightToSample(flattenHeightWorld, heightScale, heightOffset) + 0.5f);

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float dx = static_cast<float>(x) - centreCellX;
                const float dy = static_cast<float>(y) - centreCellY;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float weight   = Falloff(distance, radiusCells) * blend;
                if (weight <= 0.0f)
                    continue;

                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                const float lerp = static_cast<float>(targetSample) * weight
                                 + static_cast<float>(samples[index]) * (1.0f - weight);
                samples[index] = static_cast<std::uint16_t>(lerp + 0.5f);
            }
        }
    }

    void ApplySmoothBrush(
        std::vector<std::uint16_t>& samples,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        int   passes,
        float blend)
    {
        if (samples.empty() || width <= 0 || height <= 0 || passes <= 0)
            return;
        blend = (std::max)(0.0f, (std::min)(1.0f, blend));
        if (blend <= 0.0f)
            return;

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        const float cellSizeX   = worldSize / static_cast<float>(width  - 1);
        const float radiusCells = brushRadius / cellSizeX;

        const int minX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCells)));
        const int maxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCells)));
        const int minY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCells)));
        const int maxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCells)));
        if (maxX < minX || maxY < minY)
            return;

        // Snapshot only the brushed region plus the one-sample border the 3x3
        // kernel reads.  Copying the whole heightmap per pass cost ~32 MB of
        // memory traffic per frame on a 4k terrain while the mouse was held.
        const int sx0 = (std::max)(0, minX - 1);
        const int sy0 = (std::max)(0, minY - 1);
        const int sx1 = (std::min)(width  - 1, maxX + 1);
        const int sy1 = (std::min)(height - 1, maxY + 1);
        const int scratchWidth = sx1 - sx0 + 1;
        std::vector<std::uint16_t> scratch(static_cast<size_t>(scratchWidth) * static_cast<size_t>(sy1 - sy0 + 1));

        for (int pass = 0; pass < passes; ++pass)
        {
            for (int y = sy0; y <= sy1; ++y)
            {
                std::copy_n(
                    samples.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(sx0)),
                    scratchWidth,
                    scratch.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y - sy0) * static_cast<size_t>(scratchWidth)));
            }

            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    const float dx = static_cast<float>(x) - centreCellX;
                    const float dy = static_cast<float>(y) - centreCellY;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    const float weight   = Falloff(distance, radiusCells) * blend;
                    if (weight <= 0.0f)
                        continue;

                    // 3x3 box average, clamped at the patch border.
                    int sum = 0;
                    int count = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        const int ry = (std::max)(0, (std::min)(height - 1, y + oy)) - sy0;
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int rx = (std::max)(0, (std::min)(width - 1, x + ox)) - sx0;
                            sum += static_cast<int>(scratch[static_cast<size_t>(ry) * static_cast<size_t>(scratchWidth) + static_cast<size_t>(rx)]);
                            ++count;
                        }
                    }
                    const float averaged = static_cast<float>(sum) / static_cast<float>(count);
                    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                    const float lerp = averaged * weight
                                     + static_cast<float>(samples[index]) * (1.0f - weight);
                    samples[index] = static_cast<std::uint16_t>(lerp + 0.5f);
                }
            }
        }
    }

    void ApplyPaintBrush(
        std::vector<DirectX::XMFLOAT4>& weights,
        int width,
        int height,
        float worldSize,
        const DirectX::XMFLOAT2& brushCenterWorld,
        float brushRadius,
        float strength,
        int   activeLayer)
    {
        if (weights.empty() || width <= 0 || height <= 0)
            return;
        if (static_cast<size_t>(width) * static_cast<size_t>(height) != weights.size())
            return;

        activeLayer = (std::max)(0, (std::min)(kTerrainMaxLayers - 1, activeLayer));

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        const float cellSizeX   = worldSize / static_cast<float>(width  - 1);
        const float radiusCells = brushRadius / cellSizeX;

        const int minX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCells)));
        const int maxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCells)));
        const int minY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCells)));
        const int maxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCells)));

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float dx = static_cast<float>(x) - centreCellX;
                const float dy = static_cast<float>(y) - centreCellY;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float weight   = Falloff(distance, radiusCells);
                if (weight <= 0.0f)
                    continue;

                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                float* channels = &weights[index].x; // XMFLOAT4 is 4 contiguous floats

                // Push the active layer up by the falloff-scaled strength,
                // then renormalise so all four channels sum to 1.
                channels[activeLayer] += strength * weight;

                float sum = 0.0f;
                for (int c = 0; c < kTerrainMaxLayers; ++c)
                {
                    channels[c] = (std::max)(0.0f, channels[c]);
                    sum += channels[c];
                }
                if (sum > 1e-5f)
                {
                    const float inv = 1.0f / sum;
                    for (int c = 0; c < kTerrainMaxLayers; ++c)
                        channels[c] *= inv;
                }
                else
                {
                    for (int c = 0; c < kTerrainMaxLayers; ++c)
                        channels[c] = (c == activeLayer) ? 1.0f : 0.0f;
                }
            }
        }
    }
}
