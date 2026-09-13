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

        // Only consume the splat weights if they match the sample grid; a
        // mismatched buffer (e.g. mid-resize) falls back to white so we never
        // read out of bounds.
        const bool useWeights = layerWeights != nullptr
            && layerWeights->size() == samples.size();

        outVertices.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        const float halfSize = worldSize * 0.5f;
        const float xStep = worldSize / static_cast<float>(width  - 1);
        const float yStep = worldSize / static_cast<float>(height - 1);

        // First pass: positions + texcoords.  Normals are filled in a second
        // pass once all positions are known.
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                const float normalisedHeight = static_cast<float>(samples[index]) / 65535.0f;
                const float worldX = -halfSize + static_cast<float>(x) * xStep;
                const float worldY = -halfSize + static_cast<float>(y) * yStep;
                const float worldZ = normalisedHeight * heightScale + heightOffset;

                TerrainVertex& v = outVertices[index];
                v.Position = DirectX::XMFLOAT3(worldX, worldY, worldZ);
                v.Normal   = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
                v.TexCoord = DirectX::XMFLOAT2(
                    static_cast<float>(x) / static_cast<float>(width  - 1),
                    static_cast<float>(y) / static_cast<float>(height - 1));
                v.Color    = useWeights
                    ? (*layerWeights)[index]
                    : DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        // Second pass: indices.  Two triangles per quad, CCW when looking
        // down -Y (so cull mode = back keeps the top side visible).
        outIndices.reserve(static_cast<size_t>(width - 1) * static_cast<size_t>(height - 1) * 6u);
        for (int y = 0; y < height - 1; ++y)
        {
            for (int x = 0; x < width - 1; ++x)
            {
                const std::uint32_t i00 = static_cast<std::uint32_t>(y       * width + x);
                const std::uint32_t i10 = static_cast<std::uint32_t>(y       * width + x + 1);
                const std::uint32_t i01 = static_cast<std::uint32_t>((y + 1) * width + x);
                const std::uint32_t i11 = static_cast<std::uint32_t>((y + 1) * width + x + 1);

                // Triangle 1: i00, i01, i11
                outIndices.push_back(i00);
                outIndices.push_back(i01);
                outIndices.push_back(i11);
                // Triangle 2: i00, i11, i10
                outIndices.push_back(i00);
                outIndices.push_back(i11);
                outIndices.push_back(i10);
            }
        }

        // Third pass: per-vertex normals from neighbour heights.  Boundary
        // samples use one-sided differences.
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int xL = (x > 0)         ? x - 1 : x;
                const int xR = (x < width - 1)  ? x + 1 : x;
                const int yU = (y > 0)         ? y - 1 : y;
                const int yD = (y < height - 1) ? y + 1 : y;

                const size_t iL = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(xL);
                const size_t iR = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(xR);
                const size_t iU = static_cast<size_t>(yU) * static_cast<size_t>(width) + static_cast<size_t>(x);
                const size_t iD = static_cast<size_t>(yD) * static_cast<size_t>(width) + static_cast<size_t>(x);

                const float hL = static_cast<float>(samples[iL]) / 65535.0f * heightScale;
                const float hR = static_cast<float>(samples[iR]) / 65535.0f * heightScale;
                const float hU = static_cast<float>(samples[iU]) / 65535.0f * heightScale;
                const float hD = static_cast<float>(samples[iD]) / 65535.0f * heightScale;

                const float slopeX = (hR - hL) / ((xR - xL) * xStep);
                const float slopeY = (hD - hU) / ((yD - yU) * yStep);

                // Surface tangent along +X is (xStep, 0, slopeX); along +Y is (0, yStep, slopeY).
                // The unnormalised normal is the cross product.
                DirectX::XMFLOAT3 normal(
                    -slopeX * yStep,
                    -slopeY * xStep,
                     xStep  * yStep);
                const float length = std::sqrt(
                    normal.x * normal.x +
                    normal.y * normal.y +
                    normal.z * normal.z);
                if (length > std::numeric_limits<float>::epsilon())
                {
                    normal.x /= length;
                    normal.y /= length;
                    normal.z /= length;
                }
                else
                {
                    normal = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
                }

                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                outVertices[index].Normal = normal;
            }
        }
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

                const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                int newValue = static_cast<int>(samples[index]) + static_cast<int>(deltaSamples * weight + 0.5f);
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
        float heightOffset)
    {
        if (samples.empty() || width <= 0 || height <= 0)
            return;
        if (heightScale <= 0.0f)
            heightScale = 1.0f;

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
                const float weight   = Falloff(distance, radiusCells);
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
        float /*heightScale*/)
    {
        if (samples.empty() || width <= 0 || height <= 0 || passes <= 0)
            return;

        const float centreCellX = ((brushCenterWorld.x + worldSize * 0.5f) / worldSize) * static_cast<float>(width  - 1);
        const float centreCellY = ((brushCenterWorld.y + worldSize * 0.5f) / worldSize) * static_cast<float>(height - 1);
        const float cellSizeX   = worldSize / static_cast<float>(width  - 1);
        const float radiusCells = brushRadius / cellSizeX;

        const int minX = (std::max)(0, static_cast<int>(std::floor(centreCellX - radiusCells)));
        const int maxX = (std::min)(width  - 1, static_cast<int>(std::ceil (centreCellX + radiusCells)));
        const int minY = (std::max)(0, static_cast<int>(std::floor(centreCellY - radiusCells)));
        const int maxY = (std::min)(height - 1, static_cast<int>(std::ceil (centreCellY + radiusCells)));

        std::vector<std::uint16_t> scratch(samples);
        for (int pass = 0; pass < passes; ++pass)
        {
            std::copy(samples.begin(), samples.end(), scratch.begin());
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

                    // 3x3 box average, weighted towards the centre.
                    int   sum = 0;
                    int   count = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        const int sy = (std::max)(0, (std::min)(height - 1, y + oy));
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int sx = (std::max)(0, (std::min)(width - 1, x + ox));
                            const size_t sindex = static_cast<size_t>(sy) * static_cast<size_t>(width) + static_cast<size_t>(sx);
                            sum   += static_cast<int>(scratch[sindex]);
                            ++count;
                        }
                    }
                    const int averaged = sum / (count > 0 ? count : 1);
                    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                    const float lerp = static_cast<float>(averaged) * weight
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
