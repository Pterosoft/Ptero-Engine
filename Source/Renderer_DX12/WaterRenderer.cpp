#include "pch.h"
#include "System/DataFiles.h"

#include "WaterRenderer.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;

// Depth format for the water pass's private depth buffer (see WaterRenderer.h).
static constexpr DXGI_FORMAT kWaterDepthFormat = DXGI_FORMAT_D32_FLOAT;

// Must match the WATER_FEATURE_* bits in Water.hlsl.
static constexpr std::uint32_t kWaterFeatureDisplacement = 1u;
static constexpr std::uint32_t kWaterFeatureFoam         = 2u;
static constexpr std::uint32_t kWaterFeatureFiltering    = 4u;
static constexpr std::uint32_t kWaterFeatureBlinnPhong   = 8u;

// Highest WaterComponent::DebugMode the shader understands (WATER_DEBUG_*).
static constexpr int kWaterMaxDebugMode = 7;

// Fallback texels (RGBA8, little-endian) for textures that fail to load.
static constexpr std::uint32_t kFlatNormalTexel = 0xFFFF8080u; // (0.5, 0.5, 1) = +Z
static constexpr std::uint32_t kBlackTexel      = 0xFF000000u;

// Free grid-vertex struct with the same POD layout as WaterRenderer::WaterVertex
// so the CPU grid builder (a free helper) can produce it before the private
// nested type is in scope.  A static_assert in EnsureGpuMesh keeps them in sync.
struct WaterRenderer_Vertex
{
    DirectX::XMFLOAT3 Position{};
    DirectX::XMFLOAT3 Normal{ 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT2 TexCoord{};
};

namespace
{
    bool CreateCommittedBuffer(
        ID3D12Device* device,
        UINT64 byteSize,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState,
        ComPtr<ID3D12Resource>& outResource)
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

    // Reads the first `count` numbers of a JSON array into `out`; leaves `out`
    // untouched (the default) if the key is missing or too short.
    void ReadFloats(const nlohmann::json& source, const char* key, float* out, std::size_t count)
    {
        const auto it = source.find(key);
        if (it == source.end() || !it->is_array() || it->size() < count)
            return;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!(*it)[i].is_number())
                return;
        }
        for (std::size_t i = 0; i < count; ++i)
            out[i] = (*it)[i].get<float>();
    }

    void ReadFloat2(const nlohmann::json& source, const char* key, DirectX::XMFLOAT2& value) { ReadFloats(source, key, &value.x, 2); }
    void ReadFloat3(const nlohmann::json& source, const char* key, DirectX::XMFLOAT3& value) { ReadFloats(source, key, &value.x, 3); }
    void ReadFloat4(const nlohmann::json& source, const char* key, DirectX::XMFLOAT4& value) { ReadFloats(source, key, &value.x, 4); }

    void ReadFloat(const nlohmann::json& source, const char* key, float& value)
    {
        const auto it = source.find(key);
        if (it != source.end() && it->is_number())
            value = it->get<float>();
    }

    void ReadBool(const nlohmann::json& source, const char* key, bool& value)
    {
        const auto it = source.find(key);
        if (it != source.end() && it->is_boolean())
            value = it->get<bool>();
    }

    void ReadString(const nlohmann::json& source, const char* key, std::string& value)
    {
        const auto it = source.find(key);
        if (it != source.end() && it->is_string())
            value = it->get<std::string>();
    }

    DirectX::XMFLOAT4 ToFloat4(const DirectX::XMFLOAT3& v)
    {
        return DirectX::XMFLOAT4(v.x, v.y, v.z, 0.0f);
    }

    // Decodes any WIC-readable image (PNG, JPG, TGA, ...) from memory to RGBA8.
    bool DecodeImageRgba8(
        const std::vector<std::uint8_t>& fileBytes,
        std::vector<std::uint8_t>& outPixels,
        UINT& outWidth,
        UINT& outHeight)
    {
        if (fileBytes.empty())
            return false;

        // A no-op when COM is already up on this thread; RPC_E_CHANGED_MODE
        // just means it was initialised with another model, which WIC accepts.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(comInit);

        bool decoded = false;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;

            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
                && SUCCEEDED(factory->CreateStream(&stream))
                && SUCCEEDED(stream->InitializeFromMemory(
                    const_cast<BYTE*>(fileBytes.data()), static_cast<DWORD>(fileBytes.size())))
                && SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
                && SUCCEEDED(decoder->GetFrame(0, &frame))
                && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))
                && SUCCEEDED(converter->GetSize(&outWidth, &outHeight))
                && outWidth > 0 && outHeight > 0)
            {
                const UINT rowPitch = outWidth * 4;
                outPixels.resize(static_cast<std::size_t>(rowPitch) * outHeight);
                decoded = SUCCEEDED(converter->CopyPixels(
                    nullptr, rowPitch, static_cast<UINT>(outPixels.size()), outPixels.data()));
            }
        }

        if (comInitialized)
            CoUninitialize();
        return decoded;
    }

    // Box-filtered mip chain for an RGBA8 image.  The water textures tile many
    // times across the surface, so without mips they alias into noise.
    std::vector<std::vector<std::uint8_t>> BuildMipChain(
        std::vector<std::uint8_t> level0, UINT width, UINT height)
    {
        std::vector<std::vector<std::uint8_t>> mips;
        mips.push_back(std::move(level0));

        while (width > 1 || height > 1)
        {
            const UINT nextWidth  = (std::max)(1u, width / 2);
            const UINT nextHeight = (std::max)(1u, height / 2);
            const std::vector<std::uint8_t>& src = mips.back();
            std::vector<std::uint8_t> dst(static_cast<std::size_t>(nextWidth) * nextHeight * 4);

            for (UINT y = 0; y < nextHeight; ++y)
            {
                const UINT y0 = (std::min)(y * 2, height - 1);
                const UINT y1 = (std::min)(y * 2 + 1, height - 1);
                for (UINT x = 0; x < nextWidth; ++x)
                {
                    const UINT x0 = (std::min)(x * 2, width - 1);
                    const UINT x1 = (std::min)(x * 2 + 1, width - 1);
                    for (UINT c = 0; c < 4; ++c)
                    {
                        const UINT sum =
                            src[(static_cast<std::size_t>(y0) * width + x0) * 4 + c] +
                            src[(static_cast<std::size_t>(y0) * width + x1) * 4 + c] +
                            src[(static_cast<std::size_t>(y1) * width + x0) * 4 + c] +
                            src[(static_cast<std::size_t>(y1) * width + x1) * 4 + c];
                        dst[(static_cast<std::size_t>(y) * nextWidth + x) * 4 + c] =
                            static_cast<std::uint8_t>((sum + 2) / 4);
                    }
                }
            }

            mips.push_back(std::move(dst));
            width  = nextWidth;
            height = nextHeight;
        }

        return mips;
    }

    // Build a flat, origin-centred grid on the local XY plane (Z = 0).  Waves
    // are applied in the vertex shader, so the CPU mesh stays flat.
    void BuildWaterGrid(
        float sizeX,
        float sizeY,
        int resolution,
        std::vector<WaterRenderer_Vertex>& outVertices,
        std::vector<std::uint32_t>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        const int res = std::clamp(resolution, 2, 512);
        const float halfX = sizeX * 0.5f;
        const float halfY = sizeY * 0.5f;
        const float stepX = sizeX / static_cast<float>(res - 1);
        const float stepY = sizeY / static_cast<float>(res - 1);

        outVertices.reserve(static_cast<size_t>(res) * res);
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                WaterRenderer_Vertex v;
                v.Position = DirectX::XMFLOAT3(
                    -halfX + stepX * static_cast<float>(x),
                    -halfY + stepY * static_cast<float>(y),
                    0.0f);
                v.Normal = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
                v.TexCoord = DirectX::XMFLOAT2(
                    static_cast<float>(x) / static_cast<float>(res - 1),
                    static_cast<float>(y) / static_cast<float>(res - 1));
                outVertices.push_back(v);
            }
        }

        outIndices.reserve(static_cast<size_t>(res - 1) * (res - 1) * 6);
        for (int y = 0; y < res - 1; ++y)
        {
            for (int x = 0; x < res - 1; ++x)
            {
                const std::uint32_t i0 = static_cast<std::uint32_t>(y * res + x);
                const std::uint32_t i1 = static_cast<std::uint32_t>(y * res + x + 1);
                const std::uint32_t i2 = static_cast<std::uint32_t>((y + 1) * res + x);
                const std::uint32_t i3 = static_cast<std::uint32_t>((y + 1) * res + x + 1);

                // CCW winding (matches the FrontCounterClockwise=TRUE PSO).
                outIndices.push_back(i0);
                outIndices.push_back(i2);
                outIndices.push_back(i1);

                outIndices.push_back(i1);
                outIndices.push_back(i2);
                outIndices.push_back(i3);
            }
        }
    }
}

bool WaterRenderer::Initialize()
{
    mLastError.clear();
    return true;
}

void WaterRenderer::Shutdown()
{
    mGpuStates.clear();
    mActiveIndices.clear();

    if (mDrawCB)
    {
        mDrawCB->Unmap(0, nullptr);
        mDrawCB.Reset();
    }
    mMappedDrawCB   = nullptr;
    mDrawCBCapacity = 0;

    mRetired.clear();
    mMaterialCache.clear();
    mTextures.clear();

    mRefractionTexture.Reset();
    mRefractionWidth  = 0;
    mRefractionHeight = 0;
    mRefractionFormat = DXGI_FORMAT_UNKNOWN;

    mWaterDepth.Reset();
    mWaterDsvHeap.Reset();
    mWaterDsvHandle   = {};
    mWaterDepthWidth  = 0;
    mWaterDepthHeight = 0;

    mRootSignature.Reset();
    mPipelineState.Reset();
    mPipelineReady = false;
    mLastError.clear();
}

void WaterRenderer::SetEntities(std::vector<Entity>* entities)
{
    mEntities = entities;
}

void WaterRenderer::SyncFromEntities()
{
    if (mEntities == nullptr)
        return;

    mActiveIndices.clear();
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& e = (*mEntities)[i];
        if (!e.HasWaterComponent() || !e.Water.has_value())
            continue;

        const WaterComponent& wc = *e.Water;
        if (wc.SizeX <= 0.0f || wc.SizeY <= 0.0f)
            continue;

        auto it = mGpuStates.find(i);
        if (it == mGpuStates.end())
        {
            WaterGpuState state;
            state.Dirty = true;
            mGpuStates.emplace(i, std::move(state));
        }
        else
        {
            WaterGpuState& state = it->second;
            const int clampedRes = std::clamp(wc.Resolution, 2, 512);
            const bool changed =
                state.BuiltSizeX      != wc.SizeX ||
                state.BuiltSizeY      != wc.SizeY ||
                state.BuiltResolution != clampedRes;
            if (state.IndexCount == 0 || changed)
                state.Dirty = true;
        }

        mActiveIndices.push_back(i);
    }
}

void WaterRenderer::Retire(ComPtr<ID3D12Resource> resource)
{
    if (resource)
        mRetired.push_back({ std::move(resource), mFrameCounter + kFramesInFlight + 1 });
}

void WaterRenderer::ReleaseRetiredResources()
{
    mRetired.erase(
        std::remove_if(mRetired.begin(), mRetired.end(),
            [this](const RetiredResource& r) { return r.ReleaseAfterFrame <= mFrameCounter; }),
        mRetired.end());
}

bool WaterRenderer::EnsureDrawConstantBuffer(std::size_t requiredCount)
{
    if (mMappedDrawCB != nullptr && mDrawCBCapacity >= requiredCount)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Earlier frames may still be reading the old buffer.
    if (mDrawCB)
    {
        mDrawCB->Unmap(0, nullptr);
        Retire(std::move(mDrawCB));
    }
    mMappedDrawCB   = nullptr;
    mDrawCBCapacity = 0;

    const std::size_t capacity = requiredCount + 4;
    if (!CreateCommittedBuffer(device, sizeof(WaterDrawConstants) * capacity * kFramesInFlight,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, mDrawCB))
    {
        mLastError = "WaterRenderer: failed to allocate constant buffer.";
        return false;
    }
    if (FAILED(mDrawCB->Map(0, nullptr, reinterpret_cast<void**>(&mMappedDrawCB))))
    {
        mLastError = "WaterRenderer: failed to map constant buffer.";
        mDrawCB.Reset();
        return false;
    }

    mDrawCBCapacity = capacity;
    return true;
}

bool WaterRenderer::EnsureRefractionTexture(UINT width, UINT height, DXGI_FORMAT format)
{
    if (mRefractionTexture
        && mRefractionWidth == width
        && mRefractionHeight == height
        && mRefractionFormat == format)
    {
        return true;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "WaterRenderer: device is null in EnsureRefractionTexture.";
        return false;
    }

    Retire(std::move(mRefractionTexture));

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mRefractionTexture))))
    {
        mLastError = "WaterRenderer: failed to create refraction texture.";
        return false;
    }

    if (!mRefractionSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mRefractionSrvCpu, &mRefractionSrvGpu))
        {
            mLastError = "WaterRenderer: failed to allocate refraction SRV.";
            mRefractionTexture.Reset();
            return false;
        }
        mRefractionSrvAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(mRefractionTexture.Get(), &srvDesc, mRefractionSrvCpu);

    mRefractionWidth  = width;
    mRefractionHeight = height;
    mRefractionFormat = format;
    mRefractionState  = D3D12_RESOURCE_STATE_COPY_DEST; // freshly created
    return true;
}

bool WaterRenderer::EnsureDepthBuffer(UINT width, UINT height)
{
    if (mWaterDepth && mWaterDepthWidth == width && mWaterDepthHeight == height)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "WaterRenderer: device is null in EnsureDepthBuffer.";
        return false;
    }

    Retire(std::move(mWaterDepth));

    if (!mWaterDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mWaterDsvHeap))))
        {
            mLastError = "WaterRenderer: failed to create DSV heap.";
            return false;
        }
        mWaterDsvHandle = mWaterDsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width            = width;
    depthDesc.Height           = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels        = 1;
    depthDesc.Format           = kWaterDepthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format               = kWaterDepthFormat;
    clearValue.DepthStencil.Depth   = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&mWaterDepth))))
    {
        mLastError = "WaterRenderer: failed to create water depth buffer.";
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = kWaterDepthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(mWaterDepth.Get(), &dsvDesc, mWaterDsvHandle);

    mWaterDepthWidth  = width;
    mWaterDepthHeight = height;
    return true;
}

const WaterRenderer::WaterMaterialInfo& WaterRenderer::ResolveWaterMaterial(const std::string& materialPath)
{
    if (materialPath.empty())
        return mDefaultMaterial;

    const std::filesystem::path materialFilePath = ResolveDataRelativePath(materialPath);
    if (materialFilePath.empty())
        return mDefaultMaterial;

    // Re-parse only when the file changes, so edits in the material editor
    // still show up live.  A packaged game has no write times; its entry is
    // simply parsed once.
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(materialFilePath, timeError);

    auto cached = mMaterialCache.find(materialPath);
    if (cached != mMaterialCache.end() && (timeError || cached->second.WriteTime == writeTime))
        return cached->second.Info;

    WaterMaterialInfo info;
    try
    {
        DataFiles::InputFile materialFile(materialFilePath);
        if (materialFile.is_open())
        {
            nlohmann::json j;
            materialFile >> j;

            const nlohmann::json* w = &j;
            const auto waterIt = j.find("water");
            if (waterIt != j.end() && waterIt->is_object())
                w = &(*waterIt);

            ReadFloat3(*w, "surfaceColor",         info.SurfaceColor);
            ReadFloat3(*w, "shoreColor",           info.ShoreColor);
            ReadFloat3(*w, "depthColor",           info.DepthColor);
            ReadFloat3(*w, "horizontalExtinction", info.HorizontalExtinction);
            ReadFloat4(*w, "waveAmplitude",        info.WaveAmplitude);
            ReadFloat4(*w, "wavesIntensity",       info.WavesIntensity);
            ReadFloat4(*w, "wavesNoise",           info.WavesNoise);
            ReadFloat4(*w, "foamNoise",            info.FoamNoise);
            ReadFloat3(*w, "foamRanges",           info.FoamRanges);
            ReadFloat3(*w, "specularValues",       info.SpecularValues);
            ReadFloat2(*w, "windDirection",        info.WindDirection);
            ReadFloat2(*w, "refractionValues",     info.RefractionValues);
            ReadFloat2(*w, "foamTiling",           info.FoamTiling);

            ReadFloat(*w, "ambientDensity",      info.AmbientDensity);
            ReadFloat(*w, "diffuseDensity",      info.DiffuseDensity);
            ReadFloat(*w, "normalIntensity",     info.NormalIntensity);
            ReadFloat(*w, "textureTiling",       info.TextureTiling);
            ReadFloat(*w, "heightIntensity",     info.HeightIntensity);
            ReadFloat(*w, "waveTiling",          info.WaveTiling);
            ReadFloat(*w, "waveAmplitudeFactor", info.WaveAmplitudeFactor);
            ReadFloat(*w, "waveSteepness",       info.WaveSteepness);
            ReadFloat(*w, "waterClarity",        info.WaterClarity);
            ReadFloat(*w, "waterTransparency",   info.WaterTransparency);
            ReadFloat(*w, "refractionScale",     info.RefractionScale);
            ReadFloat(*w, "shininess",           info.Shininess);
            ReadFloat(*w, "distortion",          info.Distortion);
            ReadFloat(*w, "foamSpeed",           info.FoamSpeed);
            ReadFloat(*w, "foamIntensity",       info.FoamIntensity);
            ReadFloat(*w, "shoreFade",           info.ShoreFade);

            ReadBool(*w, "useDisplacement", info.UseDisplacement);
            ReadBool(*w, "useFoam",         info.UseFoam);
            ReadBool(*w, "useFiltering",    info.UseFiltering);
            ReadBool(*w, "blinnPhong",      info.BlinnPhong);

            ReadString(*w, "normalTexture", info.NormalTexture);
            ReadString(*w, "heightTexture", info.HeightTexture);
            ReadString(*w, "foamTexture",   info.FoamTexture);
            ReadString(*w, "shoreTexture",  info.ShoreTexture);

            // The shader divides by these.
            info.WaterTransparency = (std::max)(info.WaterTransparency, 1e-3f);
            info.HorizontalExtinction.x = (std::max)(info.HorizontalExtinction.x, 1e-3f);
            info.HorizontalExtinction.y = (std::max)(info.HorizontalExtinction.y, 1e-3f);
            info.HorizontalExtinction.z = (std::max)(info.HorizontalExtinction.z, 1e-3f);
            info.FoamRanges.x = (std::max)(info.FoamRanges.x, 1e-3f);
        }
    }
    catch (...)
    {
        info = WaterMaterialInfo{};
    }

    CachedMaterial& entry = mMaterialCache[materialPath];
    entry.Info = std::move(info);
    entry.WriteTime = writeTime;
    return entry.Info;
}

D3D12_GPU_DESCRIPTOR_HANDLE WaterRenderer::GetTexture(
    ID3D12GraphicsCommandList* commandList,
    const std::string& dataRelativePath,
    std::uint32_t fallbackRgba)
{
    // The fallback is part of the key: the same missing file used for a normal
    // map and a foam mask needs a different stand-in texel.
    const std::string key = dataRelativePath + "|" + std::to_string(fallbackRgba);
    auto found = mTextures.find(key);
    if (found != mTextures.end())
        return found->second.Srv;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return {};

    std::vector<std::vector<std::uint8_t>> mips;
    UINT width = 0;
    UINT height = 0;
    {
        std::vector<std::uint8_t> fileBytes;
        std::vector<std::uint8_t> pixels;
        const std::filesystem::path path = ResolveDataRelativePath(dataRelativePath);
        if (!path.empty()
            && DataFiles::ReadBytes(path, fileBytes)
            && DecodeImageRgba8(fileBytes, pixels, width, height))
        {
            mips = BuildMipChain(std::move(pixels), width, height);
        }
        else
        {
            mLastError = "WaterRenderer: failed to load texture '" + dataRelativePath + "'.";
            width = height = 1;
            std::vector<std::uint8_t> texel(4);
            std::memcpy(texel.data(), &fallbackRgba, 4);
            mips.push_back(std::move(texel));
        }
    }

    WaterTexture texture;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Linear UNORM: normal and height maps are data, not colour.
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = static_cast<UINT16>(mips.size());
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.Resource))))
    {
        mLastError = "WaterRenderer: failed to create texture for '" + dataRelativePath + "'.";
        return {};
    }

    const UINT mipCount = static_cast<UINT>(mips.size());
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, mipCount, 0, nullptr, nullptr, nullptr, &uploadSize);

    ComPtr<ID3D12Resource> upload;
    if (!CreateCommittedBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, upload))
    {
        mLastError = "WaterRenderer: failed to create texture upload buffer.";
        return {};
    }

    std::vector<D3D12_SUBRESOURCE_DATA> subresources(mipCount);
    UINT mipWidth = width;
    UINT mipHeight = height;
    for (UINT mip = 0; mip < mipCount; ++mip)
    {
        subresources[mip].pData      = mips[mip].data();
        subresources[mip].RowPitch   = static_cast<LONG_PTR>(mipWidth) * 4;
        subresources[mip].SlicePitch = static_cast<LONG_PTR>(mipWidth) * 4 * mipHeight;
        mipWidth  = (std::max)(1u, mipWidth / 2);
        mipHeight = (std::max)(1u, mipHeight / 2);
    }

    // Recorded on this frame's command list, ahead of the draws that use it.
    UpdateSubresources(commandList, texture.Resource.Get(), upload.Get(), 0, 0, mipCount, subresources.data());
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);
    Retire(std::move(upload));

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    if (!DX12Context_AllocateSrvDescriptor(&srvCpu, &texture.Srv))
    {
        mLastError = "WaterRenderer: failed to allocate texture SRV.";
        Retire(std::move(texture.Resource));
        return {};
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = texDesc.Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = mipCount;
    device->CreateShaderResourceView(texture.Resource.Get(), &srvDesc, srvCpu);

    const D3D12_GPU_DESCRIPTOR_HANDLE srv = texture.Srv;
    mTextures.emplace(key, std::move(texture));
    return srv;
}

bool WaterRenderer::EnsureGpuMesh(std::size_t /*entityIndex*/, const Entity& entity, WaterGpuState& state)
{
    if (!entity.HasWaterComponent() || !entity.Water.has_value())
        return false;
    const WaterComponent& wc = *entity.Water;
    if (wc.SizeX <= 0.0f || wc.SizeY <= 0.0f)
        return false;

    const int resolution = std::clamp(wc.Resolution, 2, 512);

    std::vector<WaterRenderer_Vertex> vertices;
    std::vector<std::uint32_t> indices;
    BuildWaterGrid(wc.SizeX, wc.SizeY, resolution, vertices, indices);

    state.BuiltSizeX      = wc.SizeX;
    state.BuiltSizeY      = wc.SizeY;
    state.BuiltResolution = resolution;

    if (vertices.empty() || indices.empty())
    {
        mLastError = "WaterRenderer: empty water grid produced.";
        state.Dirty = false;
        return false;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "WaterRenderer: device is null in EnsureGpuMesh.";
        return false;
    }

    static_assert(sizeof(WaterRenderer_Vertex) == sizeof(WaterVertex),
        "Grid vertex layout must match the renderer's WaterVertex.");

    const UINT64 vbSize = vertices.size() * sizeof(WaterRenderer_Vertex);
    const UINT64 ibSize = indices.size()  * sizeof(std::uint32_t);

    // A rebuilt grid replaces buffers earlier frames may still be drawing.
    Retire(std::move(state.VertexBuffer));
    Retire(std::move(state.VertexUpload));
    Retire(std::move(state.IndexBuffer));
    Retire(std::move(state.IndexUpload));
    state.IndexCount = 0;

    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, state.VertexBuffer))
    {
        mLastError = "WaterRenderer: failed to create default-heap vertex buffer.";
        return false;
    }
    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, state.VertexUpload))
    {
        mLastError = "WaterRenderer: failed to create upload-heap vertex buffer.";
        return false;
    }

    void* mappedVB = nullptr;
    if (FAILED(state.VertexUpload->Map(0, nullptr, &mappedVB)))
    {
        mLastError = "WaterRenderer: failed to map vertex upload buffer.";
        return false;
    }
    std::memcpy(mappedVB, vertices.data(), static_cast<std::size_t>(vbSize));
    state.VertexUpload->Unmap(0, nullptr);

    state.VertexBufferView.BufferLocation = state.VertexBuffer->GetGPUVirtualAddress();
    state.VertexBufferView.StrideInBytes  = sizeof(WaterRenderer_Vertex);
    state.VertexBufferView.SizeInBytes    = static_cast<UINT>(vbSize);

    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, state.IndexBuffer))
    {
        mLastError = "WaterRenderer: failed to create default-heap index buffer.";
        return false;
    }
    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, state.IndexUpload))
    {
        mLastError = "WaterRenderer: failed to create upload-heap index buffer.";
        return false;
    }

    void* mappedIB = nullptr;
    if (FAILED(state.IndexUpload->Map(0, nullptr, &mappedIB)))
    {
        mLastError = "WaterRenderer: failed to map index upload buffer.";
        return false;
    }
    std::memcpy(mappedIB, indices.data(), static_cast<std::size_t>(ibSize));
    state.IndexUpload->Unmap(0, nullptr);

    state.IndexBufferView.BufferLocation = state.IndexBuffer->GetGPUVirtualAddress();
    state.IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    state.IndexBufferView.SizeInBytes    = static_cast<UINT>(ibSize);
    state.IndexCount = static_cast<std::uint32_t>(indices.size());

    mLastError.clear();
    return true;
}

void WaterRenderer::RebuildDirtyWater(ID3D12GraphicsCommandList* commandList)
{
    if (mEntities == nullptr)
        return;

    // At most one water surface rebuilt per frame so dragging the size slider
    // doesn't stall the GPU (mirrors the terrain renderer's coalescing).
    for (auto& entry : mGpuStates)
    {
        if (!entry.second.Dirty)
            continue;
        if (entry.first >= mEntities->size())
            continue;

        const Entity& entity = (*mEntities)[entry.first];
        WaterGpuState& state = entry.second;

        if (!EnsureGpuMesh(entry.first, entity, state))
        {
            state.Dirty = false;
            continue;
        }

        commandList->CopyBufferRegion(
            state.VertexBuffer.Get(), 0, state.VertexUpload.Get(), 0,
            state.VertexBufferView.SizeInBytes);
        auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            state.VertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        commandList->ResourceBarrier(1, &vbBarrier);

        commandList->CopyBufferRegion(
            state.IndexBuffer.Get(), 0, state.IndexUpload.Get(), 0,
            state.IndexBufferView.SizeInBytes);
        auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            state.IndexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_INDEX_BUFFER);
        commandList->ResourceBarrier(1, &ibBarrier);

        state.Dirty = false;
        break; // one rebuild per frame
    }
}

bool WaterRenderer::CreatePipeline(DXGI_FORMAT sceneColorFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "WaterRenderer::CreatePipeline: device is null.";
        return false;
    }

    ShaderCompileRequest vsRequest{ L"Shaders\\Water.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    ShaderCompileRequest psRequest{ L"Shaders\\Water.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };

    if (!mVertexShader.Compile(vsRequest))
    {
        mLastError = std::string("Water VS compile failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }
    if (!mPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Water PS compile failed: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature (one single-descriptor table per texture, since each SRV
    // lives wherever the shared heap allocated it):
    //   0 b0 frame constants        1 b1 material constants
    //   2 t0 scene copy (PS)        3 t1 opaque depth (PS)
    //   4 t2 normal map (PS)        5 t3 height map (VS)
    //   6 t4 foam (PS)              7 t5 shore foam (PS)
    //   s0 linear-clamp, s1 point-clamp, s2 anisotropic-wrap
    struct TableSlot
    {
        UINT Register;
        D3D12_SHADER_VISIBILITY Visibility;
    };
    constexpr TableSlot kTables[] =
    {
        { 0, D3D12_SHADER_VISIBILITY_PIXEL },
        { 1, D3D12_SHADER_VISIBILITY_PIXEL },
        { 2, D3D12_SHADER_VISIBILITY_PIXEL },
        { 3, D3D12_SHADER_VISIBILITY_VERTEX },
        { 4, D3D12_SHADER_VISIBILITY_PIXEL },
        { 5, D3D12_SHADER_VISIBILITY_PIXEL },
    };
    constexpr UINT kTableCount = static_cast<UINT>(std::size(kTables));

    D3D12_DESCRIPTOR_RANGE ranges[kTableCount]{};
    D3D12_ROOT_PARAMETER rootParams[2 + kTableCount]{};
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    for (UINT i = 0; i < kTableCount; ++i)
    {
        ranges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors     = 1;
        ranges[i].BaseShaderRegister = kTables[i].Register;
        ranges[i].RegisterSpace      = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER& param = rootParams[2 + i];
        param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges   = &ranges[i];
        param.ShaderVisibility                    = kTables[i].Visibility;
    }

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0; // s0
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1] = samplers[0];
    samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1; // s1

    // s2: the tiling water textures.  Anisotropic because they are viewed at
    // grazing angles across the whole surface.
    samplers[2] = samplers[0];
    samplers[2].Filter           = D3D12_FILTER_ANISOTROPIC;
    samplers[2].MaxAnisotropy    = 8;
    samplers[2].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].ShaderRegister   = 2; // s2
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "WaterRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "WaterRenderer: CreateRootSignature failed.";
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = sceneColorFormat;
    psoDesc.DSVFormat             = kWaterDepthFormat;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = FALSE;
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ZERO;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = rtBlend;

    // Cull off, as in the original ("Cull False"), so the surface is visible
    // from below too.
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    // Depth test + write against the pass's own depth buffer so the water
    // surface sorts against itself.  Occlusion against *opaque* geometry is
    // done in the pixel shader from the scene depth SRV.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "WaterRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    mSceneColorFormat = sceneColorFormat;
    mPipelineReady    = true;
    mLastError.clear();
    return true;
}

void WaterRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRtv,
    ID3D12Resource* sceneColorResource,
    D3D12_RESOURCE_STATES sceneColorStateBefore,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
    const DirectX::XMMATRIX& viewProjection,
    const DirectX::XMFLOAT4X4& invViewProjTransposed,
    const DirectX::XMFLOAT3& cameraPosition,
    const DirectX::XMFLOAT3& sunDirection,
    const DirectX::XMFLOAT3& sunColor,
    const DirectX::XMFLOAT3& skyColor,
    float timeSeconds,
    UINT width,
    UINT height,
    DXGI_FORMAT sceneColorFormat)
{
    if (commandList == nullptr || mEntities == nullptr || sceneColorResource == nullptr)
        return;

    ++mFrameCounter;
    ReleaseRetiredResources();

    SyncFromEntities();
    RebuildDirtyWater(commandList);

    if (mActiveIndices.empty())
        return;

    if (!mPipelineReady || sceneColorFormat != mSceneColorFormat)
    {
        if (!CreatePipeline(sceneColorFormat))
            return;
    }
    if (!EnsureRefractionTexture(width, height, sceneColorFormat))
        return;
    if (!EnsureDepthBuffer(width, height))
        return;
    if (!EnsureDrawConstantBuffer(mActiveIndices.size()))
        return;

    // --- Resolve materials and textures (texture uploads are recorded here,
    //     before any render target is bound) ---
    struct DrawTextures
    {
        D3D12_GPU_DESCRIPTOR_HANDLE Normal{};
        D3D12_GPU_DESCRIPTOR_HANDLE Height{};
        D3D12_GPU_DESCRIPTOR_HANDLE Foam{};
        D3D12_GPU_DESCRIPTOR_HANDLE Shore{};
    };
    std::vector<const WaterMaterialInfo*> materials(mActiveIndices.size(), &mDefaultMaterial);
    std::vector<DrawTextures> textures(mActiveIndices.size());
    for (std::size_t slot = 0; slot < mActiveIndices.size(); ++slot)
    {
        const Entity& entity = (*mEntities)[mActiveIndices[slot]];
        if (entity.Water.has_value())
            materials[slot] = &ResolveWaterMaterial(entity.Water->MaterialPath);

        const WaterMaterialInfo& info = *materials[slot];
        textures[slot].Normal = GetTexture(commandList, info.NormalTexture, kFlatNormalTexel);
        textures[slot].Height = GetTexture(commandList, info.HeightTexture, kBlackTexel);
        textures[slot].Foam   = GetTexture(commandList, info.FoamTexture,   kBlackTexel);
        textures[slot].Shore  = GetTexture(commandList, info.ShoreTexture,  kBlackTexel);
    }

    // --- Copy the lit scene colour into the refraction scratch texture ---
    {
        // The scratch texture is created in COPY_DEST; only transition into it
        // when it is currently something else (i.e. was sampled last frame).
        D3D12_RESOURCE_BARRIER toCopy[2];
        UINT toCopyCount = 0;
        if (mRefractionState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            toCopy[toCopyCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                mRefractionTexture.Get(), mRefractionState, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        toCopy[toCopyCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            sceneColorResource, sceneColorStateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(toCopyCount, toCopy);

        commandList->CopyResource(mRefractionTexture.Get(), sceneColorResource);

        D3D12_RESOURCE_BARRIER toRender[2];
        toRender[0] = CD3DX12_RESOURCE_BARRIER::Transition(
            sceneColorResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        toRender[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            mRefractionTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(2, toRender);
        mRefractionState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // --- Bind the scene-colour RT + the pass's private depth buffer ---
    commandList->ClearDepthStencilView(mWaterDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(1, &sceneColorRtv, FALSE, &mWaterDsvHandle);
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* sharedSrvHeap = DX12Context_GetSrvDescriptorHeap();
    if (sharedSrvHeap == nullptr)
        return;
    commandList->SetDescriptorHeaps(1, &sharedSrvHeap);

    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootDescriptorTable(2, mRefractionSrvGpu);
    commandList->SetGraphicsRootDescriptorTable(3, sceneDepthSrv);

    const DirectX::XMMATRIX viewProjTransposed = DirectX::XMMatrixTranspose(viewProjection);

    const std::size_t frameRegion = static_cast<std::size_t>(mFrameCounter % kFramesInFlight) * mDrawCBCapacity;

    for (std::size_t slot = 0; slot < mActiveIndices.size(); ++slot)
    {
        const std::size_t entityIndex = mActiveIndices[slot];
        if (entityIndex >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[entityIndex];
        if (!entity.HasWaterComponent() || !entity.Water.has_value())
            continue;
        const WaterComponent& wc = *entity.Water;

        auto stateIt = mGpuStates.find(entityIndex);
        if (stateIt == mGpuStates.end() || stateIt->second.IndexCount == 0)
            continue;
        const WaterGpuState& state = stateIt->second;

        const DrawTextures& tex = textures[slot];
        if (tex.Normal.ptr == 0 || tex.Height.ptr == 0 || tex.Foam.ptr == 0 || tex.Shore.ptr == 0)
            continue;

        const DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
            entity.Transform.Scale.x, entity.Transform.Scale.y, entity.Transform.Scale.z);
        const DirectX::XMMATRIX rotationMatrix = PteroTransform::ComposeRotation(entity.Transform.Rotation);
        const DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(
            entity.Transform.Position.x, entity.Transform.Position.y, entity.Transform.Position.z);
        const DirectX::XMMATRIX modelMatrix = scaleMatrix * rotationMatrix * translationMatrix;

        const std::size_t drawIndex = frameRegion + slot;
        WaterDrawConstants& constants = mMappedDrawCB[drawIndex];

        WaterFrameConstants& frame = constants.Frame;
        DirectX::XMStoreFloat4x4(&frame.ViewProj, viewProjTransposed);
        DirectX::XMStoreFloat4x4(&frame.Model, DirectX::XMMatrixTranspose(modelMatrix));
        frame.InvViewProj  = invViewProjTransposed;
        frame.CameraPos    = cameraPosition;
        frame.Time         = timeSeconds;
        frame.SunDirection = sunDirection;
        frame.ScreenWidth  = static_cast<float>(width);
        frame.SunColor     = sunColor;
        frame.ScreenHeight = static_cast<float>(height);
        frame.SkyColor     = skyColor;
        frame.DebugMode    = std::clamp(wc.DebugMode, 0, kWaterMaxDebugMode);

        const WaterMaterialInfo& info = *materials[slot];
        WaterMaterialConstants& mat = constants.Material;
        mat.SurfaceColor         = ToFloat4(info.SurfaceColor);
        mat.ShoreColor           = ToFloat4(info.ShoreColor);
        mat.DepthColor           = ToFloat4(info.DepthColor);
        mat.HorizontalExtinction = ToFloat4(info.HorizontalExtinction);
        mat.WaveAmplitude        = info.WaveAmplitude;
        mat.WavesIntensity       = info.WavesIntensity;
        mat.WavesNoise           = info.WavesNoise;
        mat.FoamNoise            = info.FoamNoise;
        mat.FoamRanges           = ToFloat4(info.FoamRanges);
        mat.SpecularValues       = ToFloat4(info.SpecularValues);
        mat.WindDirection        = info.WindDirection;
        mat.RefractionValues     = info.RefractionValues;
        mat.FoamTiling           = info.FoamTiling;
        mat.FoamSpeed            = info.FoamSpeed;
        mat.FoamIntensity        = info.FoamIntensity;
        mat.AmbientDensity       = info.AmbientDensity;
        mat.DiffuseDensity       = info.DiffuseDensity;
        mat.HeightIntensity      = info.HeightIntensity;
        mat.NormalIntensity      = info.NormalIntensity;
        mat.TextureTiling        = info.TextureTiling;
        mat.WaveTiling           = info.WaveTiling;
        mat.WaveSteepness        = info.WaveSteepness;
        // The component's Wave Scale calms or roughens a placement without
        // authoring a new material.
        mat.WaveAmplitudeFactor  = info.WaveAmplitudeFactor * (std::max)(wc.WaveScale, 0.0f);
        mat.WaterClarity         = info.WaterClarity;
        mat.WaterTransparency    = info.WaterTransparency;
        mat.Shininess            = info.Shininess;
        mat.RefractionScale      = info.RefractionScale;
        mat.Distortion           = info.Distortion;
        mat.ShoreFade            = info.ShoreFade;
        mat.WaterLevel           = entity.Transform.Position.z;
        mat.Features             = (info.UseDisplacement ? kWaterFeatureDisplacement : 0u)
                                 | (info.UseFoam         ? kWaterFeatureFoam         : 0u)
                                 | (info.UseFiltering    ? kWaterFeatureFiltering    : 0u)
                                 | (info.BlinnPhong      ? kWaterFeatureBlinnPhong   : 0u);

        const D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
            mDrawCB->GetGPUVirtualAddress() + drawIndex * sizeof(WaterDrawConstants);
        commandList->SetGraphicsRootConstantBufferView(0, cbAddress + offsetof(WaterDrawConstants, Frame));
        commandList->SetGraphicsRootConstantBufferView(1, cbAddress + offsetof(WaterDrawConstants, Material));
        commandList->SetGraphicsRootDescriptorTable(4, tex.Normal);
        commandList->SetGraphicsRootDescriptorTable(5, tex.Height);
        commandList->SetGraphicsRootDescriptorTable(6, tex.Foam);
        commandList->SetGraphicsRootDescriptorTable(7, tex.Shore);

        commandList->IASetVertexBuffers(0, 1, &state.VertexBufferView);
        commandList->IASetIndexBuffer(&state.IndexBufferView);
        commandList->DrawIndexedInstanced(state.IndexCount, 1, 0, 0, 0);
    }
}
