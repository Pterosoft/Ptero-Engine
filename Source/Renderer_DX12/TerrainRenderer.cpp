#include "pch.h"
#include "System/DataFiles.h"

#include "TerrainRenderer.h"

#include "HeightmapImporter.h"
#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    // 1x1 RGBA8 white pixel used as the terrain's fallback base-colour
    // texture.  Stays in the renderer so brushless / textureless terrain
    // patches still show up in the viewport.
    constexpr std::uint8_t kWhitePixel[4] = { 255, 255, 255, 255 };

    // Mirrors the fallback texture allocation used in EntityMeshRenderer so
    // the editor's descriptor-heap budget stays predictable.
    bool CreateCommittedBuffer(
        ID3D12Device* device,
        UINT64 byteSize,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = heapType;
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask  = 1;

        const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

        return SUCCEEDED(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource)));
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        // Walks up from the exe to Data/ - or, in a packaged game, the virtual Data
        // root the .ppak archives serve (see System/DataFiles.h).
        return DataFiles::FindDataDirectory();
    }

    std::filesystem::path ResolveDataRelativePath(const std::string& relativePath)
    {
        if (relativePath.empty())
            return {};

        std::filesystem::path path(relativePath);
        std::error_code errorCode;
        if (path.is_absolute() && DataFiles::Exists(path))
            return std::filesystem::weakly_canonical(path, errorCode);

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            path = (dataDirectory / path).lexically_normal();
            if (DataFiles::Exists(path))
                return std::filesystem::weakly_canonical(path, errorCode);
        }

        return {};
    }

    std::string ResolveTexturePathNearMaterial(
        const std::filesystem::path& materialFilePath,
        const std::string& texturePath)
    {
        if (texturePath.empty())
            return {};

        std::filesystem::path path(texturePath);
        std::error_code errorCode;
        if (path.is_absolute() && DataFiles::Exists(path))
            return std::filesystem::weakly_canonical(path, errorCode).string();

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            const std::filesystem::path dataCandidate = (dataDirectory / path).lexically_normal();
            if (DataFiles::Exists(dataCandidate))
                return std::filesystem::weakly_canonical(dataCandidate, errorCode).string();
        }

        const std::filesystem::path materialRelativeCandidate = (materialFilePath.parent_path() / path).lexically_normal();
        if (DataFiles::Exists(materialRelativeCandidate))
            return std::filesystem::weakly_canonical(materialRelativeCandidate, errorCode).string();

        return {};
    }

    bool IsRawHeightmapPath(const std::string& path)
    {
        std::string ext = std::filesystem::path(path).extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".raw";
    }

    bool LoadTerrainSamples(
        const TerrainComponent& tc,
        std::vector<std::uint16_t>& outSamples,
        std::string& outError)
    {
        outSamples.clear();
        outError.clear();

        if (tc.HeightmapRawPath.empty())
        {
            outError = "heightmap path is empty.";
            return false;
        }

        if (IsRawHeightmapPath(tc.HeightmapRawPath))
        {
            const std::string absoluteRaw = HeightmapImporter::ResolveDataRelativeToAbsolute(tc.HeightmapRawPath);
            return HeightmapImporter::LoadRaw16(absoluteRaw, tc.Width, tc.Height, outSamples, outError);
        }

        int imageWidth = 0;
        int imageHeight = 0;
        if (!HeightmapImporter::LoadImageToHeightmap(
                tc.HeightmapRawPath, outSamples, imageWidth, imageHeight, outError))
        {
            return false;
        }

        if (imageWidth != tc.Width || imageHeight != tc.Height)
        {
            outSamples.clear();
            outError = "image dimensions (" + std::to_string(imageWidth) + "x"
                + std::to_string(imageHeight) + ") do not match terrain component ("
                + std::to_string(tc.Width) + "x" + std::to_string(tc.Height) + ").";
            return false;
        }

        return true;
    }

    void BuildRenderSampleGrid(
        const std::vector<std::uint16_t>& sourceSamples,
        int sourceWidth,
        int sourceHeight,
        int targetWidth,
        int targetHeight,
        std::vector<std::uint16_t>& outSamples)
    {
        outSamples.assign(static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight), 0);
        for (int y = 0; y < targetHeight; ++y)
        {
            const int srcY = (y * (sourceHeight - 1)) / (targetHeight - 1);
            for (int x = 0; x < targetWidth; ++x)
            {
                const int srcX = (x * (sourceWidth - 1)) / (targetWidth - 1);
                const size_t srcIndex = static_cast<size_t>(srcY) * static_cast<size_t>(sourceWidth) + static_cast<size_t>(srcX);
                const size_t dstIndex = static_cast<size_t>(y) * static_cast<size_t>(targetWidth) + static_cast<size_t>(x);
                outSamples[dstIndex] = sourceSamples[srcIndex];
            }
        }
    }

    // Nearest-sample decimation of the per-sample splat weights, matching
    // BuildRenderSampleGrid so the decimated weights line up with the
    // decimated height samples.
    void BuildRenderWeightGrid(
        const std::vector<DirectX::XMFLOAT4>& sourceWeights,
        int sourceWidth,
        int sourceHeight,
        int targetWidth,
        int targetHeight,
        std::vector<DirectX::XMFLOAT4>& outWeights)
    {
        outWeights.assign(
            static_cast<size_t>(targetWidth) * static_cast<size_t>(targetHeight),
            DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f));
        for (int y = 0; y < targetHeight; ++y)
        {
            const int srcY = (y * (sourceHeight - 1)) / (targetHeight - 1);
            for (int x = 0; x < targetWidth; ++x)
            {
                const int srcX = (x * (sourceWidth - 1)) / (targetWidth - 1);
                const size_t srcIndex = static_cast<size_t>(srcY) * static_cast<size_t>(sourceWidth) + static_cast<size_t>(srcX);
                const size_t dstIndex = static_cast<size_t>(y) * static_cast<size_t>(targetWidth) + static_cast<size_t>(x);
                outWeights[dstIndex] = sourceWeights[srcIndex];
            }
        }
    }

    // ---- Splat map (paint-layer weights) persistence -----------------------
    // The splat lives in a sibling ".splat" file next to the heightmap so it
    // survives level reloads.  On-disk layout: a 16-byte header (magic,
    // width, height, layer count) followed by width*height*4 uint8 weights.
    constexpr std::uint32_t kSplatMagic = 0x314C5053u; // 'SPL1'

    void InitDefaultWeights(size_t sampleCount, std::vector<DirectX::XMFLOAT4>& outWeights)
    {
        // Layer 0 owns everything until the artist paints another layer in.
        outWeights.assign(sampleCount, DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f));
    }

    // Derive the absolute .splat path that sits next to the heightmap source.
    std::string DeriveSplatPath(const TerrainComponent& tc)
    {
        if (tc.HeightmapRawPath.empty())
            return {};
        const std::filesystem::path rawAbs(
            HeightmapImporter::ResolveDataRelativePath(tc.HeightmapRawPath));
        if (rawAbs.empty())
            return {};
        return (rawAbs.parent_path() / (rawAbs.stem().string() + ".splat")).generic_string();
    }

    bool LoadSplatFromDisk(
        const std::string& splatPath,
        int expectedWidth,
        int expectedHeight,
        std::vector<DirectX::XMFLOAT4>& outWeights)
    {
        if (splatPath.empty())
            return false;

        std::filesystem::path path(splatPath);
        std::error_code ec;
        if (!path.is_absolute() || !DataFiles::Exists(path))
        {
            path = HeightmapImporter::ResolveDataRelativePath(splatPath);
            if (path.empty() || !DataFiles::Exists(path))
                return false;
        }

        DataFiles::InputFile in(path, std::ios::binary);
        if (!in)
            return false;

        std::uint32_t header[4] = {};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(header)))
            return false;
        if (header[0] != kSplatMagic
            || static_cast<int>(header[1]) != expectedWidth
            || static_cast<int>(header[2]) != expectedHeight)
            return false;

        const size_t sampleCount = static_cast<size_t>(expectedWidth) * static_cast<size_t>(expectedHeight);
        std::vector<std::uint8_t> raw(sampleCount * 4u);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        if (in.gcount() != static_cast<std::streamsize>(raw.size()))
            return false;

        outWeights.resize(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            outWeights[i] = DirectX::XMFLOAT4(
                raw[i * 4 + 0] / 255.0f,
                raw[i * 4 + 1] / 255.0f,
                raw[i * 4 + 2] / 255.0f,
                raw[i * 4 + 3] / 255.0f);
        }
        return true;
    }

    bool SaveSplatToDisk(
        const std::string& splatPath,
        const std::vector<DirectX::XMFLOAT4>& weights,
        int width,
        int height)
    {
        if (splatPath.empty() || weights.empty())
            return false;
        if (static_cast<size_t>(width) * static_cast<size_t>(height) != weights.size())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(splatPath).parent_path(), ec);

        std::ofstream out(splatPath, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;

        const std::uint32_t header[4] = {
            kSplatMagic,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            static_cast<std::uint32_t>(kTerrainMaxLayers) };
        out.write(reinterpret_cast<const char*>(header), sizeof(header));

        std::vector<std::uint8_t> raw(weights.size() * 4u);
        for (size_t i = 0; i < weights.size(); ++i)
        {
            const float* c = &weights[i].x;
            for (int k = 0; k < 4; ++k)
            {
                const float v = (c[k] < 0.0f) ? 0.0f : (c[k] > 1.0f ? 1.0f : c[k]);
                raw[i * 4 + k] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
            }
        }
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        return static_cast<bool>(out);
    }

    // Ensure `weights` holds a valid splat for the given terrain, loading it
    // from disk when possible and falling back to a default (layer-0-only)
    // grid otherwise.
    void EnsureLayerWeights(
        const TerrainComponent& tc,
        std::vector<DirectX::XMFLOAT4>& weights)
    {
        const size_t expected = static_cast<size_t>(tc.Width) * static_cast<size_t>(tc.Height);
        if (weights.size() == expected && !weights.empty())
            return;
        if (!LoadSplatFromDisk(tc.SplatMapPath, tc.Width, tc.Height, weights)
            || weights.size() != expected)
        {
            InitDefaultWeights(expected, weights);
        }
    }
}

bool TerrainRenderer::Initialize(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr)
    {
        mLastError = "TerrainRenderer::Initialize: commandList is null.";
        return false;
    }

    if (!CreateFallbackTexture(commandList))
    {
        return false;
    }

    mLastError.clear();
    return true;
}

void TerrainRenderer::Shutdown()
{
    mGpuStates.clear();
    mActiveIndices.clear();

    if (mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mConstantBuffer.Reset();
    }
    mMappedCB    = nullptr;
    mCBCapacity  = 0;

    if (mMaterialCB)
    {
        mMaterialCB->Unmap(0, nullptr);
        mMaterialCB.Reset();
    }
    mMappedMatCB    = nullptr;
    mMatCBCapacity  = 0;

    mRootSignature.Reset();
    mPipelineState.Reset();
    mFallbackTextureResource.Reset();
    mFallbackUploadBuffer.Reset();
    mTextureManager.Shutdown();
    mPipelineReady = false;
    mLastError.clear();
}

void TerrainRenderer::SetEntities(std::vector<Entity>* entities)
{
    mEntities = entities;
}

void TerrainRenderer::SyncFromEntities()
{
    if (mEntities == nullptr)
        return;

    mActiveIndices.clear();
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& e = (*mEntities)[i];
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;

        const TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 0 || tc.Height <= 0)
            continue;
        if (tc.HeightmapRawPath.empty())
            continue;

        auto it = mGpuStates.find(i);
        if (it == mGpuStates.end())
        {
            // First time we've seen this terrain -- mark it dirty so
            // RebuildDirtyTerrains() uploads the GPU buffers next Render.
            TerrainGpuState state;
            state.Dirty = true;
            mGpuStates.emplace(i, std::move(state));
        }
        else
        {
            // Detect parameter changes that require a full rebuild.
            TerrainGpuState& state = it->second;
            const bool dimensionsChanged =
                state.BuiltWidth        != tc.Width  ||
                state.BuiltHeight       != tc.Height ||
                state.BuiltWorldSize    != tc.WorldSize ||
                state.BuiltHeightScale  != tc.HeightScale ||
                state.BuiltHeightOffset != tc.HeightOffset ||
                state.BuiltHeightmapPath != tc.HeightmapRawPath;

            if (state.Samples.empty() || dimensionsChanged)
            {
                state.Dirty = true;
            }
        }

        mActiveIndices.push_back(i);
    }
}

void TerrainRenderer::MarkTerrainDirty(std::size_t entityIndex)
{
    auto it = mGpuStates.find(entityIndex);
    if (it != mGpuStates.end())
    {
        it->second.Dirty = true;
        // WorldSize/HeightScale/HeightOffset edits move the surface, so any
        // vegetation resting on this patch has to be re-scattered too.
        ++mTerrainRevision;
    }
}

bool TerrainRenderer::SampleHeightAt(const DirectX::XMFLOAT2& worldXZ, float& outWorldY) const
{
    if (mEntities == nullptr)
        return false;

    for (const Entity& e : *mEntities)
    {
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        const TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 0 || tc.Height <= 0)
            continue;

        // Convert world XY to terrain-local XY using the entity's transform position.
        const DirectX::XMFLOAT2 localXZ(
            worldXZ.x - e.Transform.Position.x,
            worldXZ.y - e.Transform.Position.y);

        float cellX = 0.0f;
        float cellY = 0.0f;
        if (!TerrainGeometry::WorldToCell(localXZ, tc.Width, tc.Height, tc.WorldSize, cellX, cellY))
            continue;

        // Bilinear sample of the CPU heightmap.
        const auto it = mGpuStates.find(static_cast<std::size_t>(&e - mEntities->data()));
        if (it == mGpuStates.end() || it->second.Samples.empty())
            continue;

        const int x0 = (std::max)(0, (std::min)(tc.Width  - 1, static_cast<int>(std::floor(cellX))));
        const int y0 = (std::max)(0, (std::min)(tc.Height - 1, static_cast<int>(std::floor(cellY))));
        const int x1 = (std::min)(tc.Width  - 1, x0 + 1);
        const int y1 = (std::min)(tc.Height - 1, y0 + 1);
        const float tx = cellX - static_cast<float>(x0);
        const float ty = cellY - static_cast<float>(y0);

        const std::vector<std::uint16_t>& samples = it->second.Samples;
        const float h00 = static_cast<float>(samples[y0 * tc.Width + x0]) / 65535.0f;
        const float h10 = static_cast<float>(samples[y0 * tc.Width + x1]) / 65535.0f;
        const float h01 = static_cast<float>(samples[y1 * tc.Width + x0]) / 65535.0f;
        const float h11 = static_cast<float>(samples[y1 * tc.Width + x1]) / 65535.0f;

        const float h0 = h00 * (1.0f - tx) + h10 * tx;
        const float h1 = h01 * (1.0f - tx) + h11 * tx;
        const float normalisedHeight = h0 * (1.0f - ty) + h1 * ty;

        outWorldY = normalisedHeight * tc.HeightScale + tc.HeightOffset + e.Transform.Position.z;
        return true;
    }

    return false;
}

void TerrainRenderer::ExportPatches(std::vector<PatchData>& outPatches) const
{
    outPatches.clear();

    if (mEntities == nullptr)
        return;

    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& e = (*mEntities)[i];
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;

        const TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 0 || tc.Height <= 0)
            continue;

        const auto it = mGpuStates.find(i);
        if (it == mGpuStates.end() || it->second.Samples.empty())
            continue;

        const std::size_t expected =
            static_cast<std::size_t>(tc.Width) * static_cast<std::size_t>(tc.Height);
        if (it->second.Samples.size() != expected)
            continue;

        PatchData patch;
        patch.EntityIndex  = i;
        patch.Width        = tc.Width;
        patch.Height       = tc.Height;
        patch.WorldSize    = tc.WorldSize;
        patch.HeightScale  = tc.HeightScale;
        patch.HeightOffset = tc.HeightOffset;
        patch.Origin       = e.Transform.Position;
        patch.Samples      = it->second.Samples;

        if (it->second.LayerWeights.size() == expected)
            patch.LayerWeights = it->second.LayerWeights;

        outPatches.push_back(std::move(patch));
    }
}

bool TerrainRenderer::SampleNormalAt(
    const DirectX::XMFLOAT2& worldXZ,
    DirectX::XMFLOAT3& outNormal) const
{
    if (mEntities == nullptr)
        return false;

    for (const Entity& e : *mEntities)
    {
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        const TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 1 || tc.Height <= 1)
            continue;

        const DirectX::XMFLOAT2 localXZ(
            worldXZ.x - e.Transform.Position.x,
            worldXZ.y - e.Transform.Position.y);

        float cellX = 0.0f;
        float cellY = 0.0f;
        if (!TerrainGeometry::WorldToCell(localXZ, tc.Width, tc.Height, tc.WorldSize, cellX, cellY))
            continue;

        const auto it = mGpuStates.find(static_cast<std::size_t>(&e - mEntities->data()));
        if (it == mGpuStates.end() || it->second.Samples.empty())
            continue;

        const std::vector<std::uint16_t>& samples = it->second.Samples;

        // Central differences on the nearest cell.  Clamping at the border
        // makes the outermost row reuse its neighbour, which is the same
        // one-sided difference BuildMesh uses for its edge normals.
        const int cx = (std::max)(0, (std::min)(tc.Width  - 1, static_cast<int>(cellX + 0.5f)));
        const int cy = (std::max)(0, (std::min)(tc.Height - 1, static_cast<int>(cellY + 0.5f)));
        const int xm = (std::max)(0, cx - 1);
        const int xp = (std::min)(tc.Width  - 1, cx + 1);
        const int ym = (std::max)(0, cy - 1);
        const int yp = (std::min)(tc.Height - 1, cy + 1);

        const float heightScale = tc.HeightScale / 65535.0f;
        const float hL = static_cast<float>(samples[cy * tc.Width + xm]) * heightScale;
        const float hR = static_cast<float>(samples[cy * tc.Width + xp]) * heightScale;
        const float hD = static_cast<float>(samples[ym * tc.Width + cx]) * heightScale;
        const float hU = static_cast<float>(samples[yp * tc.Width + cx]) * heightScale;

        // Spacing between the two sampled columns/rows in metres.  This is two
        // cells wide except at the border, where the clamp collapses it to one.
        const float xStep = tc.WorldSize / static_cast<float>(tc.Width  - 1);
        const float yStep = tc.WorldSize / static_cast<float>(tc.Height - 1);
        const float dx = static_cast<float>(xp - xm) * xStep;
        const float dy = static_cast<float>(yp - ym) * yStep;

        // Terrain is Z-up on the XY plane, so the gradient gives the normal
        // directly as (-dh/dx, -dh/dy, 1).
        DirectX::XMFLOAT3 normal(
            (dx > 0.0f) ? -(hR - hL) / dx : 0.0f,
            (dy > 0.0f) ? -(hU - hD) / dy : 0.0f,
            1.0f);

        DirectX::XMStoreFloat3(&outNormal, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&normal)));
        return true;
    }

    return false;
}

bool TerrainRenderer::SampleLayerWeightsAt(
    const DirectX::XMFLOAT2& worldXZ,
    DirectX::XMFLOAT4& outWeights) const
{
    if (mEntities == nullptr)
        return false;

    for (const Entity& e : *mEntities)
    {
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        const TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 0 || tc.Height <= 0)
            continue;

        const DirectX::XMFLOAT2 localXZ(
            worldXZ.x - e.Transform.Position.x,
            worldXZ.y - e.Transform.Position.y);

        float cellX = 0.0f;
        float cellY = 0.0f;
        if (!TerrainGeometry::WorldToCell(localXZ, tc.Width, tc.Height, tc.WorldSize, cellX, cellY))
            continue;

        const auto it = mGpuStates.find(static_cast<std::size_t>(&e - mEntities->data()));
        if (it == mGpuStates.end())
            continue;

        const std::vector<DirectX::XMFLOAT4>& weights = it->second.LayerWeights;
        if (weights.size() != static_cast<std::size_t>(tc.Width) * static_cast<std::size_t>(tc.Height))
            continue;

        const int x0 = (std::max)(0, (std::min)(tc.Width  - 1, static_cast<int>(std::floor(cellX))));
        const int y0 = (std::max)(0, (std::min)(tc.Height - 1, static_cast<int>(std::floor(cellY))));
        const int x1 = (std::min)(tc.Width  - 1, x0 + 1);
        const int y1 = (std::min)(tc.Height - 1, y0 + 1);
        const float tx = cellX - static_cast<float>(x0);
        const float ty = cellY - static_cast<float>(y0);

        const DirectX::XMFLOAT4& w00 = weights[y0 * tc.Width + x0];
        const DirectX::XMFLOAT4& w10 = weights[y0 * tc.Width + x1];
        const DirectX::XMFLOAT4& w01 = weights[y1 * tc.Width + x0];
        const DirectX::XMFLOAT4& w11 = weights[y1 * tc.Width + x1];

        const DirectX::XMVECTOR row0 = DirectX::XMVectorLerp(
            DirectX::XMLoadFloat4(&w00), DirectX::XMLoadFloat4(&w10), tx);
        const DirectX::XMVECTOR row1 = DirectX::XMVectorLerp(
            DirectX::XMLoadFloat4(&w01), DirectX::XMLoadFloat4(&w11), tx);

        DirectX::XMStoreFloat4(&outWeights, DirectX::XMVectorLerp(row0, row1, ty));
        return true;
    }

    return false;
}

bool TerrainRenderer::ApplyBrushAt(const DirectX::XMFLOAT2& worldXZ, std::string* outStatusMessage)
{
    if (mEntities == nullptr)
        return false;

    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        Entity& e = (*mEntities)[i];
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 0 || tc.Height <= 0)
            continue;

        const DirectX::XMFLOAT2 localXZ(
            worldXZ.x - e.Transform.Position.x,
            worldXZ.y - e.Transform.Position.y);

        float cellX = 0.0f;
        float cellY = 0.0f;
        if (!TerrainGeometry::WorldToCell(localXZ, tc.Width, tc.Height, tc.WorldSize, cellX, cellY))
            continue;

        auto it = mGpuStates.find(i);
        if (it == mGpuStates.end())
        {
            // Build CPU samples on demand if we haven't already.
            TerrainGpuState state;
            state.Dirty = true;
            it = mGpuStates.emplace(i, std::move(state)).first;
        }

        TerrainGpuState& state = it->second;
        const size_t expectedSampleCount = static_cast<size_t>(tc.Width) * static_cast<size_t>(tc.Height);
        if (state.Samples.empty()
            || state.Samples.size() != expectedSampleCount
            || state.BuiltHeightmapPath != tc.HeightmapRawPath)
        {
            std::string loadError;
            if (!LoadTerrainSamples(tc, state.Samples, loadError))
            {
                if (outStatusMessage) *outStatusMessage = "Failed to load heightmap: " + loadError;
                return false;
            }
        }

        const DirectX::XMFLOAT2 localPick = localXZ;

        // Material painting takes a different path: it edits the per-sample
        // splat weights (not the heightmap) and persists them to the sibling
        // .splat file rather than re-encoding the height DDS.
        if (tc.Brush == TerrainComponent::BrushType::Paint)
        {
            if (tc.PaintLayers.empty())
            {
                if (outStatusMessage)
                    *outStatusMessage = "Add a paint layer before painting materials.";
                return false;
            }

            EnsureLayerWeights(tc, state.LayerWeights);
            TerrainGeometry::ApplyPaintBrush(
                state.LayerWeights, tc.Width, tc.Height, tc.WorldSize,
                localPick, tc.BrushRadius, tc.BrushStrength, tc.ActivePaintLayer);

            if (tc.SplatMapPath.empty())
                tc.SplatMapPath = DeriveSplatPath(tc);
            std::string splatStatus;
            if (!SaveSplatToDisk(tc.SplatMapPath, state.LayerWeights, tc.Width, tc.Height))
                splatStatus = " (splat write failed)";

            if (outStatusMessage)
                *outStatusMessage = "Painted layer " + std::to_string(tc.ActivePaintLayer)
                                  + splatStatus + ".";

            state.Dirty = true;
            ++mTerrainRevision;
            return true;
        }

        switch (tc.Brush)
        {
        case TerrainComponent::BrushType::Raise:
            TerrainGeometry::ApplyRaiseBrush(
                state.Samples, tc.Width, tc.Height, tc.WorldSize,
                localPick, tc.BrushRadius, tc.BrushStrength, tc.HeightScale);
            break;
        case TerrainComponent::BrushType::Lower:
            TerrainGeometry::ApplyLowerBrush(
                state.Samples, tc.Width, tc.Height, tc.WorldSize,
                localPick, tc.BrushRadius, tc.BrushStrength, tc.HeightScale);
            break;
        case TerrainComponent::BrushType::Flatten:
            TerrainGeometry::ApplyFlattenBrush(
                state.Samples, tc.Width, tc.Height, tc.WorldSize,
                localPick, tc.BrushRadius, tc.FlattenHeight, tc.HeightScale, tc.HeightOffset);
            break;
        case TerrainComponent::BrushType::Smooth:
            TerrainGeometry::ApplySmoothBrush(
                state.Samples, tc.Width, tc.Height, tc.WorldSize,
                localPick, tc.BrushRadius, tc.BrushSmoothingPasses, tc.HeightScale);
            break;
        case TerrainComponent::BrushType::Paint:
            break; // handled above; kept for exhaustiveness
        }

        // Re-encode the DDS to disk so the rest of the engine (e.g. tools
        // that need a GPU heightmap sampler) can pick it up.  We don't
        // fail the brush stroke if this can't be written -- the mesh still
        // rebuilds correctly from the CPU samples.
        std::string ddsError;
        const std::filesystem::path rawFsAbs(
            HeightmapImporter::ResolveDataRelativePath(tc.HeightmapRawPath));
        const std::filesystem::path ddsFsAbs = rawFsAbs.parent_path()
            / (rawFsAbs.stem().string() + ".dds");
        HeightmapImporter::WriteDdsR16(
            ddsFsAbs.wstring(), state.Samples, tc.Width, tc.Height, ddsError);
        if (!ddsError.empty() && outStatusMessage)
        {
            *outStatusMessage = "Brush applied (DDS write warning: " + ddsError + ").";
        }
        else if (outStatusMessage)
        {
            *outStatusMessage = "Brush applied at (" + std::to_string(localPick.x) + ", "
                              + std::to_string(localPick.y) + ").";
        }
        tc.HeightmapDdsPath = ddsFsAbs.generic_string();

        state.Dirty = true;
        ++mTerrainRevision;
        return true;
    }

    return false;
}

bool TerrainRenderer::EnsureConstantBuffer(std::size_t requiredCount)
{
    if (mMappedCB != nullptr && mCBCapacity >= requiredCount)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mConstantBuffer.Reset();
    }
    mMappedCB   = nullptr;
    mCBCapacity = 0;

    const UINT64 byteSize = sizeof(TerrainConstants) * (requiredCount + 4);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer))))
    {
        mLastError = "TerrainRenderer: failed to allocate constant buffer.";
        return false;
    }

    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCB))))
    {
        mLastError = "TerrainRenderer: failed to map constant buffer.";
        mConstantBuffer.Reset();
        return false;
    }

    mCBCapacity = requiredCount + 4;
    return true;
}

bool TerrainRenderer::EnsureMaterialConstantBuffer(std::size_t requiredCount)
{
    if (mMappedMatCB != nullptr && mMatCBCapacity >= requiredCount)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (mMaterialCB)
    {
        mMaterialCB->Unmap(0, nullptr);
        mMaterialCB.Reset();
    }
    mMappedMatCB   = nullptr;
    mMatCBCapacity = 0;

    const UINT64 byteSize = sizeof(TerrainMaterial) * (requiredCount + 4);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mMaterialCB))))
    {
        mLastError = "TerrainRenderer: failed to allocate material constant buffer.";
        return false;
    }

    if (FAILED(mMaterialCB->Map(0, nullptr, reinterpret_cast<void**>(&mMappedMatCB))))
    {
        mLastError = "TerrainRenderer: failed to map material constant buffer.";
        mMaterialCB.Reset();
        return false;
    }

    mMatCBCapacity = requiredCount + 4;
    return true;
}

TerrainRenderer::TerrainMaterialInfo TerrainRenderer::ResolveTerrainMaterial(const std::string& materialPath) const
{
    TerrainMaterialInfo materialInfo;
    if (materialPath.empty())
        return materialInfo;

    materialInfo.BaseTint = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

    try
    {
        const std::filesystem::path materialFilePath = ResolveDataRelativePath(materialPath);
        if (materialFilePath.empty())
            return materialInfo;

        DataFiles::InputFile materialFile(materialFilePath);
        if (!materialFile.is_open())
            return materialInfo;

        nlohmann::json materialJson;
        materialFile >> materialJson;

        const nlohmann::json* source = &materialJson;
        if (materialJson.value("type", std::string{}) == "MultiMaterial")
        {
            const auto subMaterialsIt = materialJson.find("subMaterials");
            if (subMaterialsIt != materialJson.end()
                && subMaterialsIt->is_array()
                && !subMaterialsIt->empty())
            {
                source = &subMaterialsIt->front();
            }
        }

        const auto tintIt = source->find("baseColorTint");
        if (tintIt != source->end() && tintIt->is_array() && tintIt->size() >= 4)
        {
            materialInfo.BaseTint = DirectX::XMFLOAT4(
                (*tintIt)[0].get<float>(),
                (*tintIt)[1].get<float>(),
                (*tintIt)[2].get<float>(),
                (*tintIt)[3].get<float>());
        }

        materialInfo.Metallic = source->value("metallicFactor", materialInfo.Metallic);
        materialInfo.Roughness = source->value("roughnessFactor", materialInfo.Roughness);
        materialInfo.Specular = source->value("specularFactor", materialInfo.Specular);
        materialInfo.AoStrength = source->value("ambientOcclusionStrength", materialInfo.AoStrength);

        const auto texturesIt = source->find("textures");
        if (texturesIt != source->end() && texturesIt->is_object())
        {
            const auto baseColorIt = texturesIt->find("baseColor");
            if (baseColorIt != texturesIt->end() && baseColorIt->is_string())
            {
                materialInfo.BaseColorTexturePath = ResolveTexturePathNearMaterial(
                    materialFilePath,
                    baseColorIt->get<std::string>());
            }
        }
    }
    catch (...)
    {
        return TerrainMaterialInfo{};
    }

    return materialInfo;
}

bool TerrainRenderer::CreateFallbackTexture(ID3D12GraphicsCommandList* commandList)
{
    if (mFallbackTextureResource)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "TerrainRenderer: device is null in CreateFallbackTexture.";
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment          = 0;
    texDesc.Width              = 1;
    texDesc.Height             = 1;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&mFallbackTextureResource))))
    {
        mLastError = "TerrainRenderer: failed to create fallback texture resource.";
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(kWhitePixel));
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mFallbackUploadBuffer))))
    {
        mLastError = "TerrainRenderer: failed to create fallback upload buffer.";
        mFallbackTextureResource.Reset();
        return false;
    }

    void* mapped = nullptr;
    if (FAILED(mFallbackUploadBuffer->Map(0, nullptr, &mapped)))
    {
        mLastError = "TerrainRenderer: failed to map fallback upload buffer.";
        mFallbackTextureResource.Reset();
        mFallbackUploadBuffer.Reset();
        return false;
    }
    std::memcpy(mapped, kWhitePixel, sizeof(kWhitePixel));
    mFallbackUploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcLocation{};
    srcLocation.pResource          = mFallbackUploadBuffer.Get();
    srcLocation.Type               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint    = { 0, { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 4u } };
    D3D12_TEXTURE_COPY_LOCATION dstLocation{};
    dstLocation.pResource        = mFallbackTextureResource.Get();
    dstLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mFallbackTextureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    if (!DX12Context_AllocateSrvDescriptor(&mFallbackCpuHandle, &mFallbackGpuHandle))
    {
        mLastError = "TerrainRenderer: failed to allocate SRV for fallback texture.";
        mFallbackTextureResource.Reset();
        mFallbackUploadBuffer.Reset();
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    device->CreateShaderResourceView(mFallbackTextureResource.Get(), &srvDesc, mFallbackCpuHandle);

    return true;
}

bool TerrainRenderer::EnsureGpuMesh(
    ID3D12GraphicsCommandList* commandList,
    std::size_t entityIndex,
    const Entity& entity,
    TerrainGpuState& state)
{
    // On any failure path we clear Dirty so SyncFromEntities doesn't keep
    // re-queueing the rebuild every frame (which would be an allocation
    // storm for sources that don't fit in VRAM).  The next time the
    // artist changes the entity's parameters, the dirty flag will flip
    // back to true and the next render frame will retry once.
    const bool succeeded = EnsureGpuMeshImpl(commandList, entityIndex, entity, state);
    if (!succeeded)
    {
        state.Dirty = false;
    }
    return succeeded;
}

bool TerrainRenderer::EnsureGpuMeshImpl(
    ID3D12GraphicsCommandList* commandList,
    std::size_t entityIndex,
    const Entity& entity,
    TerrainGpuState& state)
{
    if (!entity.HasTerrainComponent() || !entity.Terrain.has_value())
        return false;
    const TerrainComponent& tc = *entity.Terrain;
    if (tc.Width <= 0 || tc.Height <= 0)
        return false;

    const size_t expectedSampleCount = static_cast<size_t>(tc.Width) * static_cast<size_t>(tc.Height);
    if (state.Samples.empty()
        || state.Samples.size() != expectedSampleCount
        || state.BuiltHeightmapPath != tc.HeightmapRawPath)
    {
        std::string loadError;
        if (!LoadTerrainSamples(tc, state.Samples, loadError))
        {
            mLastError = "TerrainRenderer: failed to load heightmap '"
                + tc.HeightmapRawPath + "': " + loadError;
            return false;
        }
    }

    // Cap the mesh tessellation regardless of the source heightmap size.
    // 1025x1025 (just over 1k) keeps the vertex buffer under ~60 MB even
    // on the D3D12 minimum feature level, where the single-resource cap
    // is 128 MB.  A 4096x4096 source heightmap decoded into R16 samples
    // would otherwise produce a ~900 MB vertex buffer that crashes
    // allocation.  The DDS itself keeps the full resolution for
    // downstream tools.
    // Load (or default-initialise) the paint-layer splat weights so they can
    // be baked into the mesh vertex colour.  Only terrains that actually have
    // paint layers consume the weights; without layers the mesh keeps its
    // legacy white vertex colour.
    const bool hasPaintLayers = !tc.PaintLayers.empty();
    if (hasPaintLayers)
        EnsureLayerWeights(tc, state.LayerWeights);

    constexpr int kMaxMeshResolution = 1024; // hard cap on per-axis vert count
    int meshWidth  = tc.Width;
    int meshHeight = tc.Height;
    const std::vector<std::uint16_t>* meshSamples = &state.Samples;
    const std::vector<DirectX::XMFLOAT4>* meshWeights = hasPaintLayers ? &state.LayerWeights : nullptr;
    std::vector<std::uint16_t> decimatedSamples;
    std::vector<DirectX::XMFLOAT4> decimatedWeights;
    if (meshWidth > kMaxMeshResolution + 1 || meshHeight > kMaxMeshResolution + 1)
    {
        meshWidth = (std::min)(meshWidth, kMaxMeshResolution + 1);
        meshHeight = (std::min)(meshHeight, kMaxMeshResolution + 1);
        BuildRenderSampleGrid(state.Samples, tc.Width, tc.Height, meshWidth, meshHeight, decimatedSamples);
        meshSamples = &decimatedSamples;
        if (hasPaintLayers)
        {
            BuildRenderWeightGrid(state.LayerWeights, tc.Width, tc.Height, meshWidth, meshHeight, decimatedWeights);
            meshWeights = &decimatedWeights;
        }
    }

    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t>  indices;
    TerrainGeometry::BuildMesh(
        *meshSamples, meshWidth, meshHeight,
        tc.WorldSize, tc.HeightScale, tc.HeightOffset,
        vertices, indices, meshWeights);

    // Track the *source* dimensions, not the decimation result, so a
    // second EnsureGpuMesh on the same heightmap won't redundantly
    // decimate.  The actual mesh tessellation is fixed once samples are
    // loaded, so a re-upload from the same samples is cheap.
    state.BuiltWidth        = tc.Width;
    state.BuiltHeight       = tc.Height;
    state.BuiltWorldSize    = tc.WorldSize;
    state.BuiltHeightScale  = tc.HeightScale;
    state.BuiltHeightOffset = tc.HeightOffset;
    state.BuiltHeightmapPath = tc.HeightmapRawPath;

    if (vertices.empty() || indices.empty())
    {
        mLastError = "TerrainRenderer: empty mesh produced from heightmap.";
        state.Dirty = false; // give up until the entity's parameters change
        return false;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "TerrainRenderer: device is null in EnsureGpuMesh.";
        return false;
    }

    const UINT64 vbSize = vertices.size() * sizeof(TerrainVertex);
    const UINT64 ibSize = indices.size()  * sizeof(std::uint32_t);

    // Vertex buffer: upload heap → default heap.
    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, state.VertexBuffer))
    {
        mLastError = "TerrainRenderer: failed to create default-heap vertex buffer.";
        return false;
    }
    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, state.VertexUpload))
    {
        mLastError = "TerrainRenderer: failed to create upload-heap vertex buffer.";
        return false;
    }

    void* mappedVB = nullptr;
    if (FAILED(state.VertexUpload->Map(0, nullptr, &mappedVB)))
    {
        mLastError = "TerrainRenderer: failed to map vertex upload buffer.";
        return false;
    }
    std::memcpy(mappedVB, vertices.data(), static_cast<std::size_t>(vbSize));
    state.VertexUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(state.VertexBuffer.Get(), 0, state.VertexUpload.Get(), 0, vbSize);
    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        state.VertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    commandList->ResourceBarrier(1, &vbBarrier);

    state.VertexBufferView.BufferLocation = state.VertexBuffer->GetGPUVirtualAddress();
    state.VertexBufferView.StrideInBytes  = sizeof(TerrainVertex);
    state.VertexBufferView.SizeInBytes    = static_cast<UINT>(vbSize);

    // Index buffer.
    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, state.IndexBuffer))
    {
        mLastError = "TerrainRenderer: failed to create default-heap index buffer.";
        return false;
    }
    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, state.IndexUpload))
    {
        mLastError = "TerrainRenderer: failed to create upload-heap index buffer.";
        return false;
    }

    void* mappedIB = nullptr;
    if (FAILED(state.IndexUpload->Map(0, nullptr, &mappedIB)))
    {
        mLastError = "TerrainRenderer: failed to map index upload buffer.";
        return false;
    }
    std::memcpy(mappedIB, indices.data(), static_cast<std::size_t>(ibSize));
    state.IndexUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(state.IndexBuffer.Get(), 0, state.IndexUpload.Get(), 0, ibSize);
    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        state.IndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    commandList->ResourceBarrier(1, &ibBarrier);

    state.IndexBufferView.BufferLocation = state.IndexBuffer->GetGPUVirtualAddress();
    state.IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    state.IndexBufferView.SizeInBytes    = static_cast<UINT>(ibSize);
    state.IndexCount = static_cast<std::uint32_t>(indices.size());

    state.Dirty = false;
    mLastError.clear();

    {
        std::ostringstream log;
        log << "TerrainRenderer: uploaded entityIndex=" << entityIndex
            << " (" << tc.Width << "x" << tc.Height
            << ", verts=" << vertices.size()
            << ", idx="  << indices.size() << ")";
        OutputDebugStringA((log.str() + "\n").c_str());
    }

    return true;
}

void TerrainRenderer::RebuildDirtyTerrains(ID3D12GraphicsCommandList* commandList)
{
    if (mEntities == nullptr)
        return;

    // Coalesce rebuilds: at most one terrain is rebuilt per frame.  If the
    // user is dragging a slider in the Properties panel the dimensions
    // change every frame; rebuilding all of them simultaneously would
    // stall the GPU for tens of milliseconds and starve Ui.  The
    // remaining dirty terrains get rebuilt on the next frame.
    bool rebuiltThisFrame = false;
    for (auto& entry : mGpuStates)
    {
        if (!entry.second.Dirty)
            continue;
        if (entry.first >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[entry.first];
        if (rebuiltThisFrame)
        {
            // Mark it still dirty so the next frame picks it up.
            // (SyncFromEntities won't re-set Dirty until the value
            // actually changes, so this preserves the work-queue
            // semantics without busy-spinning.)
            break;
        }
        const bool ok = EnsureGpuMesh(commandList, entry.first, entity, entry.second);
        rebuiltThisFrame = ok;
    }
}

bool TerrainRenderer::CreatePipeline(
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT msaaSampleCount)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "TerrainRenderer::CreatePipeline: device is null.";
        return false;
    }

    ShaderCompileRequest vsRequest
    {
        L"Shaders\\Terrain.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };
    ShaderCompileRequest psRequest
    {
        L"Shaders\\Terrain.hlsl",
        L"PSMain",
        L"ps_5_0",
        ShaderStage::Pixel
    };

    if (!mVertexShader.Compile(vsRequest))
    {
        mLastError = std::string("Terrain VS compile failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }
    if (!mPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Terrain PS compile failed: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature layout:
    //   slot 0     – root CBV (b0, VS) per-terrain MVP + model
    //   slot 1     – root CBV (b1, PS) per-terrain material + layer params
    //   slot 2..5  – root descriptor tables (t0..t3, PS), one per paint layer.
    //                Using four single-descriptor tables (rather than one
    //                four-descriptor table) avoids having to allocate a
    //                contiguous descriptor block per terrain -- each layer's
    //                texture can be bound straight from its TextureManager
    //                handle in the shared heap.
    constexpr int kLayerSlotBase = 2;
    D3D12_ROOT_PARAMETER rootParams[kLayerSlotBase + kTerrainMaxLayers]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // One SRV range per layer (t0..t3).  Kept in an array so each range's
    // address stays valid until D3D12SerializeRootSignature runs below.
    D3D12_DESCRIPTOR_RANGE srvRanges[kTerrainMaxLayers]{};
    for (int layer = 0; layer < kTerrainMaxLayers; ++layer)
    {
        srvRanges[layer].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[layer].NumDescriptors                    = 1;
        srvRanges[layer].BaseShaderRegister                = static_cast<UINT>(layer); // t{layer}
        srvRanges[layer].RegisterSpace                     = 0;
        srvRanges[layer].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER& param = rootParams[kLayerSlotBase + layer];
        param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges   = &srvRanges[layer];
        param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter           = D3D12_FILTER_ANISOTROPIC;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias       = mTextureMipLODBias;
    staticSampler.MaxAnisotropy    = 16;
    staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSampler.MinLOD           = 0.0f;
    staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister   = 0; // s0
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &staticSampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "TerrainRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "TerrainRenderer: CreateRootSignature failed.";
        return false;
    }

    // Input layout matches TerrainGeometry.h :: TerrainVertex (mirrors
    // System/Mesh.h :: Vertex so a renderer-wide vertex format change
    // only needs to be applied in one place).
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 3;
    psoDesc.RTVFormats[0]         = albedoFormat;
    psoDesc.RTVFormats[1]         = normalFormat;
    psoDesc.RTVFormats[2]         = materialFormat;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = msaaSampleCount;
    psoDesc.SampleDesc.Quality    = 0;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = FALSE;
    rtBlend.LogicOpEnable         = FALSE;
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ZERO;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < 3; ++i)
        psoDesc.BlendState.RenderTarget[i] = rtBlend;

    psoDesc.RasterizerState.FillMode              = mWireframeEnabled ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    // Culling is disabled on the terrain so a wrong winding order doesn't
    // hide the patch entirely.  Most level designs view the terrain from
    // above, so back-face culling would cull the visible top.  Leave it on
    // once a proper winding test confirms the orientation.
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "TerrainRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    mAlbedoFormat   = albedoFormat;
    mNormalFormat   = normalFormat;
    mMaterialFormat = materialFormat;
    mDepthFormat    = depthFormat;
    mPipelineReady  = true;
    mLastError.clear();
    return true;
}

void TerrainRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const DirectX::XMMATRIX& viewProjection,
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT msaaSampleCount)
{
    if (commandList == nullptr || mEntities == nullptr)
        return;

    // Sync / rebuild first so the very first frame after the entity is
    // added already has a populated mActiveIndices and a non-zero
    // IndexCount.  The old ordering (early-return on empty indices)
    // would have prevented any draw on frame 1 because SyncFromEntities
    // ran *after* the check.
    SyncFromEntities();
    RebuildDirtyTerrains(commandList);

    if (mActiveIndices.empty())
        return;

    if (!mPipelineReady
        || albedoFormat   != mAlbedoFormat
        || normalFormat   != mNormalFormat
        || materialFormat != mMaterialFormat
        || depthFormat    != mDepthFormat
        || msaaSampleCount != mMsaaSampleCount)
    {
        mAlbedoFormat = albedoFormat;
        mNormalFormat = normalFormat;
        mMaterialFormat = materialFormat;
        mDepthFormat = depthFormat;
        mMsaaSampleCount = msaaSampleCount;
        if (!CreatePipeline(albedoFormat, normalFormat, materialFormat, depthFormat, msaaSampleCount))
            return;
    }

    if (!EnsureConstantBuffer(mActiveIndices.size()))
        return;
    if (!EnsureMaterialConstantBuffer(mActiveIndices.size()))
        return;

    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* sharedSrvHeap = DX12Context_GetSrvDescriptorHeap();
    if (sharedSrvHeap == nullptr)
        return;

    commandList->SetDescriptorHeaps(1, &sharedSrvHeap);

    for (std::size_t slot = 0; slot < mActiveIndices.size(); ++slot)
    {
        const std::size_t entityIndex = mActiveIndices[slot];
        if (entityIndex >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[entityIndex];
        if (!entity.HasTerrainComponent() || !entity.Terrain.has_value())
            continue;
        const TerrainComponent& tc = *entity.Terrain;

        auto stateIt = mGpuStates.find(entityIndex);
        if (stateIt == mGpuStates.end() || stateIt->second.IndexCount == 0)
            continue;
        const TerrainGpuState& state = stateIt->second;

        // Build the model matrix directly from the transform fields,
        // intentionally bypassing TransformComponent::MeshWorldScale so the
        // terrain is sized in literal world units rather than the
        // 0.1×-scaled mesh space.
        const DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
            entity.Transform.Scale.x,
            entity.Transform.Scale.y,
            entity.Transform.Scale.z);
        const DirectX::XMMATRIX rotationMatrix = PteroTransform::ComposeRotation(entity.Transform.Rotation);
        const DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(
            entity.Transform.Position.x,
            entity.Transform.Position.y,
            entity.Transform.Position.z);
        const DirectX::XMMATRIX modelMatrix = scaleMatrix * rotationMatrix * translationMatrix;
        const DirectX::XMMATRIX mvp         = DirectX::XMMatrixTranspose(modelMatrix * viewProjection);

        TerrainConstants* cb = &mMappedCB[slot];
        DirectX::XMStoreFloat4x4(&cb->MVP,    mvp);
        DirectX::XMStoreFloat4x4(&cb->Model,  XMMatrixTranspose(modelMatrix));

        const TerrainMaterialInfo materialInfo = ResolveTerrainMaterial(tc.MaterialPath);
        TerrainMaterial* mat = &mMappedMatCB[slot];
        mat->BaseTint        = materialInfo.BaseTint;
        mat->Metallic        = materialInfo.Metallic;
        mat->Roughness       = materialInfo.Roughness;
        mat->Specular        = materialInfo.Specular;
        mat->AoStrength      = materialInfo.AoStrength;
        mat->HasBaseMap      = 0;

        const int layerCount = (std::min)(
            static_cast<int>(tc.PaintLayers.size()), kTerrainMaxLayers);
        mat->LayerCount      = layerCount;

        // In paint mode the per-layer tints do the colouring; a terrain
        // without an explicit material JSON otherwise gets ResolveTerrainMaterial's
        // brown default, which would wash every layer brown.  Neutralise it.
        if (layerCount > 0 && tc.MaterialPath.empty())
            mat->BaseTint = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        // When paint layers are active the vertex colour carries splat
        // weights (consumed by the shader's blend), so it must NOT be
        // multiplied into the albedo as a tint.
        mat->UseVertexColour = (layerCount > 0) ? 0 : 1;

        // Bind one texture per layer slot (t0..t3).  Every slot must be bound
        // to a valid descriptor even when unused, so empty slots fall back to
        // the 1x1 white texture.
        D3D12_GPU_DESCRIPTOR_HANDLE layerHandles[kTerrainMaxLayers];
        for (int layer = 0; layer < kTerrainMaxLayers; ++layer)
        {
            layerHandles[layer] = mFallbackGpuHandle;
            float* tileScale = &mat->LayerTileScale.x;
            int*   hasTex    = &mat->LayerHasTex.x;
            hasTex[layer]    = 0;
            tileScale[layer] = 16.0f;
            mat->LayerTint[layer] = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

            if (layer < layerCount)
            {
                const TerrainPaintLayer& pl = tc.PaintLayers[layer];
                tileScale[layer]      = (pl.TileScale > 0.0f) ? pl.TileScale : 1.0f;
                mat->LayerTint[layer] = DirectX::XMFLOAT4(pl.TintR, pl.TintG, pl.TintB, pl.TintA);
                if (!pl.DiffuseTexturePath.empty())
                {
                    if (auto texture = mTextureManager.LoadDDS(pl.DiffuseTexturePath, TextureSemantic::Color))
                    {
                        layerHandles[layer] = texture->GpuHandle;
                        hasTex[layer]       = 1;
                    }
                    else if (!mTextureManager.LastError().empty())
                    {
                        mLastError = "TerrainRenderer: layer texture load failed: " + mTextureManager.LastError();
                    }
                }
            }
        }

        // Legacy single-material path: with no paint layers, layer 0 doubles
        // as the material's base-colour texture so existing terrains render
        // exactly as before.
        if (layerCount == 0 && !materialInfo.BaseColorTexturePath.empty())
        {
            if (auto texture = mTextureManager.LoadDDS(materialInfo.BaseColorTexturePath, TextureSemantic::Color))
            {
                layerHandles[0] = texture->GpuHandle;
                mat->HasBaseMap = 1;
            }
            else if (!mTextureManager.LastError().empty())
            {
                mLastError = "TerrainRenderer: material texture load failed: " + mTextureManager.LastError();
            }
        }

        commandList->SetGraphicsRootConstantBufferView(
            0, mConstantBuffer->GetGPUVirtualAddress() + slot * sizeof(TerrainConstants));
        commandList->SetGraphicsRootConstantBufferView(
            1, mMaterialCB->GetGPUVirtualAddress() + slot * sizeof(TerrainMaterial));
        for (int layer = 0; layer < kTerrainMaxLayers; ++layer)
            commandList->SetGraphicsRootDescriptorTable(2 + layer, layerHandles[layer]);

        commandList->IASetVertexBuffers(0, 1, &state.VertexBufferView);
        commandList->IASetIndexBuffer(&state.IndexBufferView);
        commandList->DrawIndexedInstanced(state.IndexCount, 1, 0, 0, 0);

        // One-shot confirmation log per entity so artists can verify the
        // terrain is actually being submitted to the GPU.
        static std::unordered_set<std::size_t> loggedEntities;
        if (loggedEntities.insert(entityIndex).second)
        {
            std::ostringstream log;
            log << "TerrainRenderer: drew entity=" << entityIndex
                << " indexCount=" << state.IndexCount
                << " vbSize=" << state.VertexBufferView.SizeInBytes
                << " ibSize=" << state.IndexBufferView.SizeInBytes
                << " built=" << stateIt->second.BuiltWidth
                << "x" << stateIt->second.BuiltHeight
                << "\n";
            OutputDebugStringA(log.str().c_str());
        }
    }
}
