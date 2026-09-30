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
#include <limits>
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

    // Terrain root signature: two CBVs, then one single-SRV table per texture
    // (see CreatePipeline).  Textures are grouped by map, four layers each:
    // t{map * 4 + layer}.
    enum TerrainTextureMap
    {
        kTerrainMapBase = 0,
        kTerrainMapNormal,
        kTerrainMapRoughness,   // or packed metallic-roughness
        kTerrainMapMetallic,
        kTerrainMapAo,
        kTerrainMapHeight,      // also read by the domain shader
        kTerrainMapCount
    };
    constexpr int kTerrainRootTextureBase = 2;
    constexpr int kTerrainTextureCount    = kTerrainMapCount * kTerrainMaxLayers;
    static_assert(kTerrainMaxLayers == 4, "Terrain.hlsl declares four layers per map");

    constexpr int TerrainTextureIndex(int map, int layer) { return map * kTerrainMaxLayers + layer; }

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

        // Sculpted heights win over the source; a sculpt file that no longer
        // matches the component (source swapped or resized) is ignored.
        if (!tc.SculptedHeightmapPath.empty())
        {
            std::string sculptError;
            if (HeightmapImporter::LoadRaw16(tc.SculptedHeightmapPath, tc.Width, tc.Height, outSamples, sculptError))
                return true;
            OutputDebugStringA(("TerrainRenderer: ignoring sculpted heightmap: " + sculptError + "\n").c_str());
            outSamples.clear();
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

    // Data-relative form of an absolute path under Data/, so level files stay
    // portable and packaged games (which serve Data/ from .ppak) resolve it.
    std::string MakeDataRelative(const std::filesystem::path& absolutePath)
    {
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        if (!dataDirectory.empty())
        {
            std::error_code ec;
            const std::filesystem::path relative = std::filesystem::relative(absolutePath, dataDirectory, ec);
            if (!ec && !relative.empty() && relative.begin()->string() != "..")
                return relative.generic_string();
        }
        return absolutePath.generic_string();
    }

    // "<Data>/Textures/Desert.png" -> "<Data>/Textures/Desert.sculpt.raw"
    std::filesystem::path DeriveSculptPath(const TerrainComponent& tc)
    {
        const std::filesystem::path source(HeightmapImporter::ResolveDataRelativePath(tc.HeightmapRawPath));
        if (source.empty())
            return {};
        return source.parent_path() / (source.stem().string() + ".sculpt.raw");
    }

    bool SaveRaw16(const std::filesystem::path& path, const std::vector<std::uint16_t>& samples)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(samples.data()),
                  static_cast<std::streamsize>(samples.size() * sizeof(std::uint16_t)));
        return static_cast<bool>(out);
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
    mRetiredResources.clear();
    mMaterialCache.clear();

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
            // Detect parameter changes that require a full rebuild.  A load
            // that already failed with these exact parameters is not retried:
            // that used to re-decode the whole heightmap image every frame.
            TerrainGpuState& state = it->second;
            const bool parametersChanged =
                state.BuiltWidth        != tc.Width  ||
                state.BuiltHeight       != tc.Height ||
                state.BuiltWorldSize    != tc.WorldSize ||
                state.BuiltHeightScale  != tc.HeightScale ||
                state.BuiltHeightOffset != tc.HeightOffset ||
                state.BuiltHeightmapPath != tc.HeightmapRawPath ||
                state.BuiltWithLayers   != !tc.PaintLayers.empty();

            if (parametersChanged || (state.Samples.empty() && !state.LoadFailed))
            {
                state.Dirty = true;
            }
        }

        mActiveIndices.push_back(i);
    }

    // Forget terrains that are gone (entity deleted, component removed, or an
    // earlier deletion shifted the index).  Their buffers may still be named
    // by frames in flight, so they are retired rather than freed.
    for (auto it = mGpuStates.begin(); it != mGpuStates.end(); )
    {
        if (std::find(mActiveIndices.begin(), mActiveIndices.end(), it->first) == mActiveIndices.end())
        {
            RetireGpuMesh(it->second);
            it = mGpuStates.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void TerrainRenderer::MarkTerrainDirty(std::size_t entityIndex)
{
    // The material file may have been re-picked or edited as well.
    mMaterialCache.clear();

    auto it = mGpuStates.find(entityIndex);
    if (it != mGpuStates.end())
    {
        it->second.Dirty = true;
        it->second.LoadFailed = false;
        // WorldSize/HeightScale/HeightOffset edits move the surface, so any
        // vegetation resting on this patch has to be re-scattered too.
        ++mTerrainRevision;
    }
}

void TerrainRenderer::FillPaintLayer(std::size_t entityIndex, int layer)
{
    if (mEntities == nullptr || entityIndex >= mEntities->size())
        return;
    Entity& e = (*mEntities)[entityIndex];
    if (!e.HasTerrainComponent() || !e.Terrain.has_value())
        return;
    const TerrainComponent& tc = *e.Terrain;
    if (tc.Width <= 1 || tc.Height <= 1 || layer < 0 || layer >= kTerrainMaxLayers)
        return;

    auto it = mGpuStates.find(entityIndex);
    if (it == mGpuStates.end())
        return;
    TerrainGpuState& state = it->second;

    EnsureLayerWeights(tc, state.LayerWeights);
    DirectX::XMFLOAT4 oneHot(0.0f, 0.0f, 0.0f, 0.0f);
    (&oneHot.x)[layer] = 1.0f;
    std::fill(state.LayerWeights.begin(), state.LayerWeights.end(), oneHot);

    state.DirtySamples = TerrainGeometry::GridRect::Full(tc.Width, tc.Height);
    state.PendingSplatSave = true;
    EndBrushStroke();
}

void TerrainRenderer::RemovePaintLayerChannel(std::size_t entityIndex, int layer)
{
    if (mEntities == nullptr || entityIndex >= mEntities->size())
        return;
    Entity& e = (*mEntities)[entityIndex];
    if (!e.HasTerrainComponent() || !e.Terrain.has_value())
        return;
    const TerrainComponent& tc = *e.Terrain;
    if (tc.Width <= 1 || tc.Height <= 1 || layer < 0 || layer >= kTerrainMaxLayers)
        return;

    auto it = mGpuStates.find(entityIndex);
    if (it == mGpuStates.end())
        return;
    TerrainGpuState& state = it->second;

    // Loaded from disk if this session has not painted yet, so the shift
    // applies to what was saved.
    EnsureLayerWeights(tc, state.LayerWeights);
    for (DirectX::XMFLOAT4& weight : state.LayerWeights)
    {
        float* channels = &weight.x;
        for (int c = layer; c + 1 < kTerrainMaxLayers; ++c)
            channels[c] = channels[c + 1];
        channels[kTerrainMaxLayers - 1] = 0.0f;

        // Where only the removed layer was painted, fall back to layer 0.
        const float sum = channels[0] + channels[1] + channels[2] + channels[3];
        if (sum > 1e-5f)
        {
            for (int c = 0; c < kTerrainMaxLayers; ++c)
                channels[c] /= sum;
        }
        else
        {
            weight = DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    state.DirtySamples = TerrainGeometry::GridRect::Full(tc.Width, tc.Height);
    state.PendingSplatSave = true;
    EndBrushStroke();
}

void TerrainRenderer::ReloadTerrain(std::size_t entityIndex)
{
    auto it = mGpuStates.find(entityIndex);
    if (it == mGpuStates.end())
        return;
    TerrainGpuState& state = it->second;
    state.Samples.clear();
    state.DirtySamples = {};
    state.PendingHeightSave = false;
    state.LoadFailed = false;
    state.Dirty = true;
    ++mTerrainRevision;
}

bool TerrainRenderer::EnsureSamplesLoaded(const TerrainComponent& tc, TerrainGpuState& state, std::string& outError)
{
    const size_t expectedSampleCount = static_cast<size_t>(tc.Width) * static_cast<size_t>(tc.Height);
    if (!state.Samples.empty()
        && state.Samples.size() == expectedSampleCount
        && state.BuiltHeightmapPath == tc.HeightmapRawPath)
    {
        return true;
    }

    if (!LoadTerrainSamples(tc, state.Samples, outError))
        return false;

    // Fresh samples invalidate any splat grid sized for the old ones.
    if (state.LayerWeights.size() != expectedSampleCount)
        state.LayerWeights.clear();
    return true;
}

void TerrainRenderer::RetireResource(ComPtr<ID3D12Resource> resource)
{
    if (!resource)
        return;
    RetiredResource retired;
    // One extra frame of slack: the resource is retired part-way through a
    // frame that may already have recorded commands naming it.
    retired.FramesRemaining = static_cast<int>(kFramesInFlight) + 1;
    retired.Resource = std::move(resource);
    mRetiredResources.push_back(std::move(retired));
}

void TerrainRenderer::RetireGpuMesh(TerrainGpuState& state)
{
    RetireResource(std::move(state.VertexBuffer));
    RetireResource(std::move(state.IndexBuffer));
    state.VertexBuffer.Reset();
    state.IndexBuffer.Reset();
    state.IndexCount = 0;
}

void TerrainRenderer::ReleaseExpiredResources()
{
    for (auto it = mRetiredResources.begin(); it != mRetiredResources.end(); )
    {
        if (--it->FramesRemaining <= 0)
            it = mRetiredResources.erase(it);
        else
            ++it;
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

bool TerrainRenderer::ApplyBrushAt(
    const DirectX::XMFLOAT2& worldXZ,
    float deltaSeconds,
    std::string* outStatusMessage)
{
    if (mEntities == nullptr)
        return false;

    // A hitch must not dump a whole second of brush into one application.
    deltaSeconds = (std::max)(0.0f, (std::min)(deltaSeconds, 0.1f));

    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        Entity& e = (*mEntities)[i];
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        TerrainComponent& tc = *e.Terrain;
        if (tc.Width <= 1 || tc.Height <= 1)
            continue;

        const DirectX::XMFLOAT2 localPick(
            worldXZ.x - e.Transform.Position.x,
            worldXZ.y - e.Transform.Position.y);

        float cellX = 0.0f;
        float cellY = 0.0f;
        if (!TerrainGeometry::WorldToCell(localPick, tc.Width, tc.Height, tc.WorldSize, cellX, cellY))
            continue;

        auto it = mGpuStates.find(i);
        if (it == mGpuStates.end())
        {
            TerrainGpuState state;
            state.Dirty = true;
            it = mGpuStates.emplace(i, std::move(state)).first;
        }
        TerrainGpuState& state = it->second;

        std::string loadError;
        if (!EnsureSamplesLoaded(tc, state, loadError))
        {
            if (outStatusMessage) *outStatusMessage = "Failed to load heightmap: " + loadError;
            return false;
        }

        const TerrainGeometry::GridRect touched = TerrainGeometry::BrushSampleRect(
            tc.Width, tc.Height, tc.WorldSize, localPick, tc.BrushRadius);

        // Raise/lower/paint move Strength units per second.  Flatten and
        // smooth converge exponentially toward their target, Strength setting
        // the rate, so they behave the same at 20 fps and at 144 fps.
        const float amount = tc.BrushStrength * deltaSeconds;
        const float blend  = 1.0f - std::exp(-4.0f * tc.BrushStrength * deltaSeconds);

        // Material painting edits the per-sample splat weights, not the
        // heightmap.
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
                localPick, tc.BrushRadius, amount, tc.ActivePaintLayer);
            state.PendingSplatSave = true;

            if (outStatusMessage)
                *outStatusMessage = "Painting layer " + std::to_string(tc.ActivePaintLayer) + ".";
        }
        else
        {
            switch (tc.Brush)
            {
            case TerrainComponent::BrushType::Raise:
                TerrainGeometry::ApplyRaiseBrush(
                    state.Samples, tc.Width, tc.Height, tc.WorldSize,
                    localPick, tc.BrushRadius, amount, tc.HeightScale);
                break;
            case TerrainComponent::BrushType::Lower:
                TerrainGeometry::ApplyLowerBrush(
                    state.Samples, tc.Width, tc.Height, tc.WorldSize,
                    localPick, tc.BrushRadius, amount, tc.HeightScale);
                break;
            case TerrainComponent::BrushType::Flatten:
                // FlattenHeight is a world height; the samples live in the
                // entity's local frame.
                TerrainGeometry::ApplyFlattenBrush(
                    state.Samples, tc.Width, tc.Height, tc.WorldSize,
                    localPick, tc.BrushRadius, tc.FlattenHeight - e.Transform.Position.z,
                    tc.HeightScale, tc.HeightOffset, blend);
                break;
            case TerrainComponent::BrushType::Smooth:
                TerrainGeometry::ApplySmoothBrush(
                    state.Samples, tc.Width, tc.Height, tc.WorldSize,
                    localPick, tc.BrushRadius, tc.BrushSmoothingPasses, blend);
                break;
            case TerrainComponent::BrushType::Paint:
                break; // handled above; kept for exhaustiveness
            }
            state.PendingHeightSave = true;

            if (outStatusMessage)
            {
                *outStatusMessage = "Sculpting at (" + std::to_string(static_cast<int>(localPick.x)) + ", "
                                  + std::to_string(static_cast<int>(localPick.y)) + ").";
            }
        }

        // Only the touched rows are re-uploaded next frame (UploadDirtyRegion).
        state.DirtySamples.Merge(touched);
        return true;
    }

    return false;
}

void TerrainRenderer::EndBrushStroke(std::string* outStatusMessage)
{
    if (mEntities == nullptr)
        return;

    bool anyEdit = false;
    for (auto& entry : mGpuStates)
    {
        TerrainGpuState& state = entry.second;
        if (!state.PendingHeightSave && !state.PendingSplatSave)
            continue;
        if (entry.first >= mEntities->size())
            continue;
        Entity& e = (*mEntities)[entry.first];
        if (!e.HasTerrainComponent() || !e.Terrain.has_value())
            continue;
        TerrainComponent& tc = *e.Terrain;
        anyEdit = true;

        std::string status;
        if (state.PendingHeightSave)
        {
            state.PendingHeightSave = false;

            // The sculpted heights are what the terrain loads from from now
            // on; the source image is left untouched.
            const std::filesystem::path sculptPath = DeriveSculptPath(tc);
            if (!sculptPath.empty() && SaveRaw16(sculptPath, state.Samples))
            {
                tc.SculptedHeightmapPath = MakeDataRelative(sculptPath);
                status = "Saved sculpt to " + tc.SculptedHeightmapPath + ".";
            }
            else
            {
                status = "Could not write the sculpted heightmap next to '" + tc.HeightmapRawPath + "'.";
            }

            // Keep the sibling DDS in step for tools that sample the heightmap
            // on the GPU.
            const std::filesystem::path rawFsAbs(
                HeightmapImporter::ResolveDataRelativePath(tc.HeightmapRawPath));
            const std::filesystem::path ddsFsAbs = rawFsAbs.parent_path()
                / (rawFsAbs.stem().string() + ".dds");
            std::string ddsError;
            if (HeightmapImporter::WriteDdsR16(ddsFsAbs.wstring(), state.Samples, tc.Width, tc.Height, ddsError))
                tc.HeightmapDdsPath = ddsFsAbs.generic_string();
            else if (!ddsError.empty())
                status += " (DDS write warning: " + ddsError + ")";
        }

        if (state.PendingSplatSave)
        {
            state.PendingSplatSave = false;
            if (tc.SplatMapPath.empty())
                tc.SplatMapPath = DeriveSplatPath(tc);
            if (!SaveSplatToDisk(tc.SplatMapPath, state.LayerWeights, tc.Width, tc.Height))
                status += " (splat write failed)";
            else if (status.empty())
                status = "Saved paint layers.";
        }

        if (outStatusMessage)
            *outStatusMessage = status;
    }

    // Vegetation re-scatters once per stroke rather than every frame of it.
    if (anyEdit)
        ++mTerrainRevision;
}

bool TerrainRenderer::Raycast(
    const DirectX::XMFLOAT3& origin,
    const DirectX::XMFLOAT3& direction,
    float maxDistance,
    DirectX::XMFLOAT3& outHit) const
{
    using namespace DirectX;

    if (mEntities == nullptr || maxDistance <= 0.0f)
        return false;

    // March no coarser than one heightmap cell near the camera, so a click
    // cannot step over a ridge; far away the step grows with distance.
    float baseStep = (std::numeric_limits<float>::max)();
    for (const Entity& e : *mEntities)
    {
        if (e.HasTerrainComponent() && e.Terrain.has_value() && e.Terrain->Width > 1)
            baseStep = (std::min)(baseStep, e.Terrain->WorldSize / static_cast<float>(e.Terrain->Width - 1));
    }
    if (baseStep == (std::numeric_limits<float>::max)())
        return false;
    baseStep = (std::max)(baseStep, 0.05f);

    const XMVECTOR o = XMLoadFloat3(&origin);
    const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&direction));

    // Height of the ray above the terrain at distance t; false off-terrain.
    const auto heightAbove = [&](float t, float& outHeight) -> bool
    {
        XMFLOAT3 p;
        XMStoreFloat3(&p, XMVectorMultiplyAdd(d, XMVectorReplicate(t), o));
        float surface = 0.0f;
        if (!SampleHeightAt(XMFLOAT2(p.x, p.y), surface))
            return false;
        outHeight = p.z - surface;
        return true;
    };

    float previousT = 0.0f;
    float previousHeight = 0.0f;
    bool previousValid = heightAbove(0.0f, previousHeight);
    float step = baseStep;
    for (float t = step; previousT < maxDistance; t += step)
    {
        t = (std::min)(t, maxDistance);
        float height = 0.0f;
        const bool valid = heightAbove(t, height);
        if (valid && previousValid && previousHeight >= 0.0f && height < 0.0f)
        {
            float lo = previousT;
            float hi = t;
            for (int refine = 0; refine < 16; ++refine)
            {
                const float mid = 0.5f * (lo + hi);
                float midHeight = 0.0f;
                if (heightAbove(mid, midHeight) && midHeight >= 0.0f)
                    lo = mid;
                else
                    hi = mid;
            }
            XMStoreFloat3(&outHit, XMVectorMultiplyAdd(d, XMVectorReplicate(0.5f * (lo + hi)), o));
            return true;
        }
        previousT = t;
        previousHeight = height;
        previousValid = valid;
        step = (std::max)(baseStep, t * 0.002f);
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

    // Frames still in flight read the old buffer, so it is retired, not freed.
    if (mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        RetireResource(std::move(mConstantBuffer));
    }
    mMappedCB   = nullptr;
    mCBCapacity = 0;

    // One copy of every slot per frame in flight: the CPU records up to three
    // frames ahead, and a single copy was overwritten while the GPU still
    // read the previous frame's matrices (torn terrain depth while moving).
    const UINT64 byteSize = sizeof(TerrainConstants) * (requiredCount + 4) * kFramesInFlight;

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
        RetireResource(std::move(mMaterialCB));
    }
    mMappedMatCB   = nullptr;
    mMatCBCapacity = 0;

    const UINT64 byteSize = sizeof(TerrainMaterial) * (requiredCount + 4) * kFramesInFlight;

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
        materialInfo.NormalScale = source->value("normalScale", materialInfo.NormalScale);
        materialInfo.FlipNormalGreen = source->value("normalFlipGreen", materialInfo.FlipNormalGreen);
        materialInfo.UvRotationDegrees = source->value("uvRotationDegrees", materialInfo.UvRotationDegrees);
        materialInfo.UseTessellation = source->value("useTessellation", materialInfo.UseTessellation);
        materialInfo.TessMaxFactor = source->value("tessellationMaxFactor", materialInfo.TessMaxFactor);
        materialInfo.TessTargetPixels = source->value("tessellationTargetPixels", materialInfo.TessTargetPixels);
        materialInfo.TessFadeDistance = source->value("tessellationFadeDistance", materialInfo.TessFadeDistance);
        materialInfo.DisplacementScale = source->value("displacementScale", materialInfo.DisplacementScale);
        materialInfo.DisplacementMidLevel = source->value("displacementMidLevel", materialInfo.DisplacementMidLevel);

        const auto readFloat2 = [source](const char* key, DirectX::XMFLOAT2& out)
        {
            const auto it = source->find(key);
            if (it != source->end() && it->is_array() && it->size() >= 2)
                out = DirectX::XMFLOAT2((*it)[0].get<float>(), (*it)[1].get<float>());
        };
        readFloat2("uvTiling", materialInfo.UvTiling);
        readFloat2("uvOffset", materialInfo.UvOffset);

        const auto texturesIt = source->find("textures");
        if (texturesIt != source->end() && texturesIt->is_object())
        {
            const auto readTexture = [&](const char* key, std::string& out)
            {
                const auto it = texturesIt->find(key);
                if (it != texturesIt->end() && it->is_string() && !it->get<std::string>().empty())
                    out = ResolveTexturePathNearMaterial(materialFilePath, it->get<std::string>());
            };
            readTexture("baseColor",         materialInfo.BaseColorTexturePath);
            readTexture("normal",            materialInfo.NormalTexturePath);
            readTexture("roughness",         materialInfo.RoughnessTexturePath);
            readTexture("metallic",          materialInfo.MetallicTexturePath);
            readTexture("metallicRoughness", materialInfo.PackedMaterialTexturePath);
            if (materialInfo.PackedMaterialTexturePath.empty())
            {
                readTexture("orm", materialInfo.PackedMaterialTexturePath);
                materialInfo.PackedMaterialIsOrm = !materialInfo.PackedMaterialTexturePath.empty();
            }
            readTexture("ambientOcclusion",  materialInfo.AoTexturePath);
            readTexture("height",            materialInfo.HeightTexturePath);
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

const TerrainRenderer::TerrainMaterialInfo& TerrainRenderer::GetCachedTerrainMaterial(const std::string& materialPath)
{
    auto it = mMaterialCache.find(materialPath);
    if (it == mMaterialCache.end())
        it = mMaterialCache.emplace(materialPath, ResolveTerrainMaterial(materialPath)).first;
    return it->second;
}

bool TerrainRenderer::UploadToBuffer(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* destination,
    UINT64 destinationOffset,
    const void* data,
    UINT64 byteSize,
    D3D12_RESOURCE_STATES steadyState,
    bool destinationIsFresh)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || destination == nullptr || byteSize == 0)
        return false;

    ComPtr<ID3D12Resource> upload;
    if (!CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, upload))
    {
        mLastError = "TerrainRenderer: failed to create upload buffer.";
        return false;
    }

    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)))
    {
        mLastError = "TerrainRenderer: failed to map upload buffer.";
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(byteSize));
    upload->Unmap(0, nullptr);

    // A fresh buffer is created in COPY_DEST; a live one is moved there for
    // the copy.  The copy is ordered after earlier frames' draws on the
    // queue, so patching a live buffer in place is safe.
    if (!destinationIsFresh)
    {
        const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            destination, steadyState, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopy);
    }
    commandList->CopyBufferRegion(destination, destinationOffset, upload.Get(), 0, byteSize);
    const auto toSteady = CD3DX12_RESOURCE_BARRIER::Transition(
        destination, D3D12_RESOURCE_STATE_COPY_DEST, steadyState);
    commandList->ResourceBarrier(1, &toSteady);

    // The copy has only been recorded; the staging memory must outlive it.
    RetireResource(std::move(upload));
    return true;
}

bool TerrainRenderer::EnsureGpuMesh(
    ID3D12GraphicsCommandList* commandList,
    std::size_t entityIndex,
    const Entity& entity,
    TerrainGpuState& state)
{
    // On any failure path we clear Dirty so the rebuild is not re-queued
    // every frame.  The next time the artist changes the entity's
    // parameters, SyncFromEntities flips it back and we retry once.
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
    if (tc.Width <= 1 || tc.Height <= 1)
        return false;

    std::string loadError;
    const bool loaded = EnsureSamplesLoaded(tc, state, loadError);

    // Record what this build was for even when it fails, so SyncFromEntities
    // does not re-decode a bad heightmap every frame.
    state.BuiltWidth         = tc.Width;
    state.BuiltHeight        = tc.Height;
    state.BuiltWorldSize     = tc.WorldSize;
    state.BuiltHeightScale   = tc.HeightScale;
    state.BuiltHeightOffset  = tc.HeightOffset;
    state.BuiltHeightmapPath = tc.HeightmapRawPath;
    state.BuiltWithLayers    = !tc.PaintLayers.empty();

    if (!loaded)
    {
        mLastError = "TerrainRenderer: failed to load heightmap '"
            + tc.HeightmapRawPath + "': " + loadError;
        // Stop drawing the previous mesh: it no longer matches the settings,
        // and leaving it up made every later edit look ignored.
        state.LoadFailed = true;
        RetireGpuMesh(state);
        state.MeshVertices.clear();
        state.MeshWidth = 0;
        state.MeshHeight = 0;
        return false;
    }
    state.LoadFailed = false;

    // A full build is new surface for everything that samples the terrain. The first
    // load of a level's heightmap matters most: vegetation that scattered before it
    // had cached an empty terrain snapshot under the unchanged revision, and kept
    // reporting "no surface hit" for every candidate until the terrain was edited.
    ++mTerrainRevision;

    // Load (or default-initialise) the paint-layer splat weights so they can
    // be baked into the mesh vertex colour.  Only terrains that actually have
    // paint layers consume the weights; without layers the mesh keeps its
    // legacy white vertex colour.
    const bool hasPaintLayers = !tc.PaintLayers.empty();
    if (hasPaintLayers)
        EnsureLayerWeights(tc, state.LayerWeights);

    // Cap the mesh tessellation regardless of the source heightmap size.
    // 1025x1025 keeps the vertex buffer around 50 MB; a 4096x4096 heightmap
    // at full resolution would need ~800 MB.  The mesh resamples the
    // heightmap bilinearly (FillMeshVertices), and brushes still edit the
    // full-resolution samples.
    constexpr int kMaxMeshResolution = 1024; // hard cap on per-axis quad count
    const int meshWidth  = (std::min)(tc.Width,  kMaxMeshResolution + 1);
    const int meshHeight = (std::min)(tc.Height, kMaxMeshResolution + 1);

    state.MeshVertices.assign(static_cast<size_t>(meshWidth) * static_cast<size_t>(meshHeight), TerrainVertex{});
    const TerrainGeometry::GridRect all = TerrainGeometry::GridRect::Full(meshWidth, meshHeight);
    TerrainGeometry::FillMeshVertices(
        state.Samples, tc.Width, tc.Height,
        hasPaintLayers ? &state.LayerWeights : nullptr,
        meshWidth, meshHeight,
        tc.WorldSize, tc.HeightScale, tc.HeightOffset,
        all, state.MeshVertices);
    TerrainGeometry::ComputeMeshNormals(meshWidth, meshHeight, all, state.MeshVertices);
    // A full rebuild covers every pending brush edit.
    state.DirtySamples = {};

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "TerrainRenderer: device is null in EnsureGpuMesh.";
        return false;
    }

    const UINT64 vbSize = state.MeshVertices.size() * sizeof(TerrainVertex);

    // Vertex buffer.  Reuse it when the size is unchanged (height scale,
    // offset and world size edits); otherwise retire the old one - frames in
    // flight still draw from it - and allocate a new one.
    const bool reuseVertexBuffer = state.VertexBuffer
        && state.VertexBuffer->GetDesc().Width == vbSize;
    if (!reuseVertexBuffer)
    {
        RetireResource(std::move(state.VertexBuffer));
        if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, state.VertexBuffer))
        {
            mLastError = "TerrainRenderer: failed to create default-heap vertex buffer.";
            return false;
        }
    }
    if (!UploadToBuffer(commandList, state.VertexBuffer.Get(), 0,
            state.MeshVertices.data(), vbSize,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, !reuseVertexBuffer))
    {
        return false;
    }

    state.VertexBufferView.BufferLocation = state.VertexBuffer->GetGPUVirtualAddress();
    state.VertexBufferView.StrideInBytes  = sizeof(TerrainVertex);
    state.VertexBufferView.SizeInBytes    = static_cast<UINT>(vbSize);

    // Index buffer: the topology only depends on the mesh dimensions.
    if (!state.IndexBuffer || state.MeshWidth != meshWidth || state.MeshHeight != meshHeight)
    {
        std::vector<std::uint32_t> indices;
        TerrainGeometry::BuildMeshIndices(meshWidth, meshHeight, indices);
        const UINT64 ibSize = indices.size() * sizeof(std::uint32_t);

        RetireResource(std::move(state.IndexBuffer));
        if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, state.IndexBuffer))
        {
            mLastError = "TerrainRenderer: failed to create default-heap index buffer.";
            return false;
        }
        if (!UploadToBuffer(commandList, state.IndexBuffer.Get(), 0,
                indices.data(), ibSize, D3D12_RESOURCE_STATE_INDEX_BUFFER, true))
        {
            return false;
        }

        state.IndexBufferView.BufferLocation = state.IndexBuffer->GetGPUVirtualAddress();
        state.IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
        state.IndexBufferView.SizeInBytes    = static_cast<UINT>(ibSize);
        state.IndexCount = static_cast<std::uint32_t>(indices.size());
    }
    state.MeshWidth  = meshWidth;
    state.MeshHeight = meshHeight;

    state.Dirty = false;
    mLastError.clear();

    {
        std::ostringstream log;
        log << "TerrainRenderer: uploaded entityIndex=" << entityIndex
            << " (" << tc.Width << "x" << tc.Height
            << ", mesh " << meshWidth << "x" << meshHeight
            << ", idx="  << state.IndexCount << ")";
        OutputDebugStringA((log.str() + "\n").c_str());
    }

    return true;
}

bool TerrainRenderer::UploadDirtyRegion(
    ID3D12GraphicsCommandList* commandList,
    const Entity& entity,
    TerrainGpuState& state)
{
    if (state.DirtySamples.IsEmpty())
        return true;
    if (!entity.HasTerrainComponent() || !entity.Terrain.has_value())
        return false;
    const TerrainComponent& tc = *entity.Terrain;

    const TerrainGeometry::GridRect dirty = state.DirtySamples;
    state.DirtySamples = {};

    const size_t expected = static_cast<size_t>(tc.Width) * static_cast<size_t>(tc.Height);
    if (!state.VertexBuffer || state.MeshVertices.empty() || state.Samples.size() != expected)
    {
        // Nothing to patch yet; fall back to a full build.
        state.Dirty = true;
        return false;
    }

    const TerrainGeometry::GridRect meshRect = TerrainGeometry::SourceRectToMeshRect(
        dirty, tc.Width, tc.Height, state.MeshWidth, state.MeshHeight);
    if (meshRect.IsEmpty())
        return true;

    const bool hasPaintLayers = !tc.PaintLayers.empty();
    TerrainGeometry::FillMeshVertices(
        state.Samples, tc.Width, tc.Height,
        hasPaintLayers ? &state.LayerWeights : nullptr,
        state.MeshWidth, state.MeshHeight,
        tc.WorldSize, tc.HeightScale, tc.HeightOffset,
        meshRect, state.MeshVertices);
    TerrainGeometry::ComputeMeshNormals(state.MeshWidth, state.MeshHeight, meshRect, state.MeshVertices);

    // Whole rows form one contiguous span of the vertex buffer.
    const size_t firstVertex = static_cast<size_t>(meshRect.MinY) * static_cast<size_t>(state.MeshWidth);
    const size_t vertexCount = static_cast<size_t>(meshRect.MaxY - meshRect.MinY + 1) * static_cast<size_t>(state.MeshWidth);
    return UploadToBuffer(commandList, state.VertexBuffer.Get(),
        firstVertex * sizeof(TerrainVertex),
        state.MeshVertices.data() + firstVertex,
        vertexCount * sizeof(TerrainVertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, false);
}

void TerrainRenderer::RebuildDirtyTerrains(ID3D12GraphicsCommandList* commandList)
{
    if (mEntities == nullptr)
        return;

    // At most one full rebuild per frame; the rest wait a frame.  Brush
    // strokes only patch the rows they touched, so those always go through.
    bool rebuiltThisFrame = false;
    for (auto& entry : mGpuStates)
    {
        if (entry.first >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[entry.first];
        TerrainGpuState& state = entry.second;

        if (state.Dirty)
        {
            if (!rebuiltThisFrame)
                rebuiltThisFrame = EnsureGpuMesh(commandList, entry.first, entity, state);
            continue;
        }
        if (!state.DirtySamples.IsEmpty())
            UploadDirtyRegion(commandList, entity, state);
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
    //   slot 0      – root CBV (b0, all stages) per-terrain MVP + model + camera
    //   slot 1      – root CBV (b1, all stages) blend settings + per-layer materials
    //   slot 2..25  – root descriptor tables t0..t23, one texture each, four
    //                 layers per map: t0..t3 base colour, t4..t7 normal,
    //                 t8..t11 roughness / packed, t12..t15 metallic,
    //                 t16..t19 AO, t20..t23 height (read by the domain shader).
    //                 Single-descriptor tables avoid allocating a contiguous
    //                 descriptor block per terrain -- each texture is bound
    //                 straight from its TextureManager handle in the shared heap.
    //                 26 root parameters cost 28 of the 64 root-signature DWORDs.
    //   b0, b1, the height maps and the sampler are visible to every stage
    //   because the tessellated pipeline's hull and domain shaders read them.
    D3D12_ROOT_PARAMETER rootParams[kTerrainRootTextureBase + kTerrainTextureCount]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // One SRV range per texture.  Kept in an array so each range's address
    // stays valid until D3D12SerializeRootSignature runs below.
    D3D12_DESCRIPTOR_RANGE srvRanges[kTerrainTextureCount]{};
    for (int texture = 0; texture < kTerrainTextureCount; ++texture)
    {
        srvRanges[texture].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[texture].NumDescriptors                    = 1;
        srvRanges[texture].BaseShaderRegister                = static_cast<UINT>(texture); // t{texture}
        srvRanges[texture].RegisterSpace                     = 0;
        srvRanges[texture].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER& param = rootParams[kTerrainRootTextureBase + texture];
        param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges   = &srvRanges[texture];
        param.ShaderVisibility                    = (texture >= TerrainTextureIndex(kTerrainMapHeight, 0))
            ? D3D12_SHADER_VISIBILITY_ALL
            : D3D12_SHADER_VISIBILITY_PIXEL;
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
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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

    // Tessellated variant.  Optional: if it cannot be built, a tessellated
    // material draws through the plain pipeline instead.
    mTessPipelineState.Reset();
    {
        const ShaderCompileRequest tessVsRequest{ L"Shaders\\Terrain.hlsl", L"VSMainTess", L"vs_5_0", ShaderStage::Vertex };
        const ShaderCompileRequest hsRequest    { L"Shaders\\Terrain.hlsl", L"HSMain",     L"hs_5_0", ShaderStage::Hull };
        const ShaderCompileRequest dsRequest    { L"Shaders\\Terrain.hlsl", L"DSMain",     L"ds_5_0", ShaderStage::Domain };
        if (mTessVertexShader.Compile(tessVsRequest)
            && mHullShader.Compile(hsRequest)
            && mDomainShader.Compile(dsRequest))
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC tessDesc = psoDesc;
            tessDesc.VS = mTessVertexShader.GetBytecode();
            tessDesc.HS = mHullShader.GetBytecode();
            tessDesc.DS = mDomainShader.GetBytecode();
            tessDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            if (FAILED(device->CreateGraphicsPipelineState(&tessDesc, IID_PPV_ARGS(&mTessPipelineState))))
            {
                mTessPipelineState.Reset();
                mLastError = "TerrainRenderer: tessellated pipeline creation failed.";
            }
        }
        else
        {
            mLastError = "TerrainRenderer: tessellation shaders failed to compile.";
        }
    }

    mAlbedoFormat   = albedoFormat;
    mNormalFormat   = normalFormat;
    mMaterialFormat = materialFormat;
    mDepthFormat    = depthFormat;
    mPipelineReady  = true;
    if (mTessPipelineState)
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

    // Called once per frame: advance the constant-buffer ring and free what
    // the GPU can no longer be using.
    ++mFrameCounter;
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    ReleaseExpiredResources();
    // Pick up edits to the material JSON without parsing it every frame.
    if ((mFrameCounter % 120) == 0)
        mMaterialCache.clear();

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
    // Switched per terrain when its material asks for tessellation.
    bool tessellatedPipelineBound = false;

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

        const std::size_t cbIndex  = mFrameSlot * mCBCapacity + slot;
        const std::size_t matIndex = mFrameSlot * mMatCBCapacity + slot;

        TerrainConstants* cb = &mMappedCB[cbIndex];
        DirectX::XMStoreFloat4x4(&cb->MVP,    mvp);
        DirectX::XMStoreFloat4x4(&cb->Model,  XMMatrixTranspose(modelMatrix));
        cb->CameraPositionWS = mCameraPosition;
        cb->TessPixelScale   = mTessPixelScale;

        TerrainMaterial* mat = &mMappedMatCB[matIndex];
        *mat = TerrainMaterial{};

        // Every slot must be bound to a valid descriptor even when unused, so
        // empty slots fall back to the 1x1 white texture.
        D3D12_GPU_DESCRIPTOR_HANDLE textureHandles[kTerrainTextureCount];
        for (D3D12_GPU_DESCRIPTOR_HANDLE& handle : textureHandles)
            handle = mFallbackGpuHandle;

        const auto loadTexture = [&](const std::string& path, TextureSemantic semantic, int map, int layer) -> int
        {
            if (path.empty())
                return 0;
            if (auto texture = mTextureManager.LoadDDS(path, semantic))
            {
                textureHandles[TerrainTextureIndex(map, layer)] = texture->GpuHandle;
                return 1;
            }
            if (!mTextureManager.LastError().empty())
                mLastError = "TerrainRenderer: texture load failed: " + mTextureManager.LastError();
            return 0;
        };

        // Fill one layer's constants and textures from a material.
        const auto applyMaterial = [&](int layer, const TerrainMaterialInfo& info, float tileSize,
                                       const DirectX::XMFLOAT4& tint)
        {
            TerrainLayerConstants& L = mat->Layers[layer];
            L.BaseTint    = DirectX::XMFLOAT4(info.BaseTint.x * tint.x, info.BaseTint.y * tint.y,
                                              info.BaseTint.z * tint.z, info.BaseTint.w * tint.w);
            L.UvTiling    = info.UvTiling;
            L.UvOffset    = info.UvOffset;
            const float uvRotationRadians = DirectX::XMConvertToRadians(info.UvRotationDegrees);
            L.UvRotationSin = std::sin(uvRotationRadians);
            L.UvRotationCos = std::cos(uvRotationRadians);
            L.TileSize    = (std::max)(tileSize, 0.01f);
            L.NormalScale = info.NormalScale;
            L.Roughness   = info.Roughness;
            L.Metallic    = info.Metallic;
            L.AoStrength  = info.AoStrength;
            L.Specular    = info.Specular;
            L.FlipNormalGreen = info.FlipNormalGreen ? 1 : 0;

            L.HasBaseMap   = loadTexture(info.BaseColorTexturePath, TextureSemantic::Color,  kTerrainMapBase,   layer);
            L.HasNormalMap = loadTexture(info.NormalTexturePath,    TextureSemantic::Normal, kTerrainMapNormal, layer);
            if (!info.PackedMaterialTexturePath.empty()
                && info.RoughnessTexturePath.empty()
                && info.MetallicTexturePath.empty()
                && info.AoTexturePath.empty())
            {
                // Terrain.hlsl reads 1 as RMA and 2 as ORM.
                if (loadTexture(info.PackedMaterialTexturePath, TextureSemantic::MaterialMask, kTerrainMapRoughness, layer))
                    L.HasPackedMaterialMap = info.PackedMaterialIsOrm ? 2 : 1;
            }
            else
            {
                L.HasRoughnessMap = loadTexture(info.RoughnessTexturePath, TextureSemantic::MaterialMask, kTerrainMapRoughness, layer);
                L.HasMetallicMap  = loadTexture(info.MetallicTexturePath,  TextureSemantic::MaterialMask, kTerrainMapMetallic,  layer);
                L.HasAoMap        = loadTexture(info.AoTexturePath,        TextureSemantic::MaterialMask, kTerrainMapAo,        layer);
            }
            L.HasHeightMap = loadTexture(info.HeightTexturePath, TextureSemantic::MaterialMask, kTerrainMapHeight, layer);

            // Only a material that asks for tessellation displaces; its height
            // map still drives height blending either way.
            if (info.UseTessellation && L.HasHeightMap != 0 && info.DisplacementScale != 0.0f)
            {
                L.DisplacementScale    = info.DisplacementScale;
                L.DisplacementMidLevel = info.DisplacementMidLevel;
                const float reach = std::fabs(info.DisplacementScale)
                                  * (std::max)(info.DisplacementMidLevel, 1.0f - info.DisplacementMidLevel);
                if (mat->UseTessellation == 0)
                {
                    mat->TessMaxFactor    = info.TessMaxFactor;
                    mat->TessTargetPixels = info.TessTargetPixels;
                    mat->TessFadeDistance = info.TessFadeDistance;
                }
                else
                {
                    // Several displacing layers: honour the most demanding.
                    mat->TessMaxFactor    = (std::max)(mat->TessMaxFactor, info.TessMaxFactor);
                    mat->TessTargetPixels = (std::min)(mat->TessTargetPixels, info.TessTargetPixels);
                    mat->TessFadeDistance = (mat->TessFadeDistance <= 0.0f || info.TessFadeDistance <= 0.0f)
                        ? 0.0f
                        : (std::max)(mat->TessFadeDistance, info.TessFadeDistance);
                }
                mat->UseTessellation = 1;
                mat->MaxDisplacement = (std::max)(mat->MaxDisplacement, reach);
            }
        };

        const int paintLayerCount = (std::min)(
            static_cast<int>(tc.PaintLayers.size()), kTerrainMaxLayers);
        if (paintLayerCount == 0)
        {
            // No paint layers: the terrain's own material is one implicit layer.
            mat->LayerCount = 1;
            mat->UseSplat   = 0;
            applyMaterial(0, GetCachedTerrainMaterial(tc.MaterialPath), tc.MaterialTileSize,
                          DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        else
        {
            mat->LayerCount = paintLayerCount;
            mat->UseSplat   = 1;
            for (int layer = 0; layer < paintLayerCount; ++layer)
            {
                const TerrainPaintLayer& pl = tc.PaintLayers[layer];
                const DirectX::XMFLOAT4 tint(pl.TintR, pl.TintG, pl.TintB, pl.TintA);
                const float tileSize = pl.EffectiveTileSize(tc.WorldSize);
                if (!pl.MaterialPath.empty())
                {
                    applyMaterial(layer, GetCachedTerrainMaterial(pl.MaterialPath), tileSize, tint);
                }
                else
                {
                    // Texture-only layer (the original paint layers): the texture
                    // is the base colour, the tint colours it, and the surface is a
                    // plain rough dielectric.
                    TerrainLayerConstants& L = mat->Layers[layer];
                    L.BaseTint   = tint;
                    L.TileSize   = (std::max)(tileSize, 0.01f);
                    L.HasBaseMap = loadTexture(pl.DiffuseTexturePath, TextureSemantic::Color, kTerrainMapBase, layer);
                }
            }
        }

        mat->BreakUpTiling        = tc.BreakUpTiling ? 1 : 0;
        mat->HeightBlend          = tc.HeightBlend ? 1 : 0;
        mat->HeightBlendSharpness = tc.HeightBlendSharpness;

        // Tessellation needs something to displace by.
        const bool tessellate = mat->UseTessellation != 0 && mTessPipelineState;
        if (tessellate != tessellatedPipelineBound)
        {
            tessellatedPipelineBound = tessellate;
            commandList->SetPipelineState(tessellate ? mTessPipelineState.Get() : mPipelineState.Get());
            commandList->IASetPrimitiveTopology(tessellate
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        commandList->SetGraphicsRootConstantBufferView(
            0, mConstantBuffer->GetGPUVirtualAddress() + cbIndex * sizeof(TerrainConstants));
        commandList->SetGraphicsRootConstantBufferView(
            1, mMaterialCB->GetGPUVirtualAddress() + matIndex * sizeof(TerrainMaterial));
        for (int texture = 0; texture < kTerrainTextureCount; ++texture)
            commandList->SetGraphicsRootDescriptorTable(kTerrainRootTextureBase + texture, textureHandles[texture]);

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
