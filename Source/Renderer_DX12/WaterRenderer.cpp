#include "pch.h"

#include "WaterRenderer.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;

// Depth format for the water pass's private depth buffer (see WaterRenderer.h).
static constexpr DXGI_FORMAT kWaterDepthFormat = DXGI_FORMAT_D32_FLOAT;

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
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
            return {};

        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
                return std::filesystem::weakly_canonical(dataDirectory);

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
                break;
            currentPath = parentPath;
        }
        return {};
    }

    std::filesystem::path ResolveDataRelativePath(const std::string& relativePath)
    {
        if (relativePath.empty())
            return {};

        std::filesystem::path path(relativePath);
        std::error_code errorCode;
        if (path.is_absolute() && std::filesystem::exists(path, errorCode))
            return std::filesystem::weakly_canonical(path, errorCode);

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            path = (dataDirectory / path).lexically_normal();
            if (std::filesystem::exists(path, errorCode))
                return std::filesystem::weakly_canonical(path, errorCode);
        }
        return {};
    }

    DirectX::XMFLOAT4 ReadColor(
        const nlohmann::json& source,
        const char* key,
        const DirectX::XMFLOAT4& fallback)
    {
        const auto it = source.find(key);
        if (it != source.end() && it->is_array() && it->size() >= 3)
        {
            return DirectX::XMFLOAT4(
                (*it)[0].get<float>(),
                (*it)[1].get<float>(),
                (*it)[2].get<float>(),
                it->size() >= 4 ? (*it)[3].get<float>() : fallback.w);
        }
        return fallback;
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

    // The FFT ocean is optional: if it fails to initialise the water still
    // renders using the analytic Gerstner spectrum.
    mOceanReady = mOcean.Initialize();
    if (!mOceanReady && mOcean.GetLastErrorMessage())
    {
        mLastError = std::string("Ocean FFT unavailable, falling back to Gerstner waves: ")
                   + mOcean.GetLastErrorMessage();
    }

    return true;
}

void WaterRenderer::Shutdown()
{
    mGpuStates.clear();
    mActiveIndices.clear();

    if (mFrameCB)
    {
        mFrameCB->Unmap(0, nullptr);
        mFrameCB.Reset();
    }
    mMappedFrameCB   = nullptr;
    mFrameCBCapacity = 0;

    if (mMaterialCB)
    {
        mMaterialCB->Unmap(0, nullptr);
        mMaterialCB.Reset();
    }
    mMappedMatCB   = nullptr;
    mMatCBCapacity = 0;

    mRefractionTexture.Reset();
    mRefractionWidth  = 0;
    mRefractionHeight = 0;
    mRefractionFormat = DXGI_FORMAT_UNKNOWN;

    mWaterDepth.Reset();
    mWaterDsvHeap.Reset();
    mWaterDsvHandle   = {};
    mWaterDepthWidth  = 0;
    mWaterDepthHeight = 0;

    mOcean.Shutdown();
    mOceanReady = false;

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

bool WaterRenderer::EnsureFrameConstantBuffer(std::size_t requiredCount)
{
    if (mMappedFrameCB != nullptr && mFrameCBCapacity >= requiredCount)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (mFrameCB)
    {
        mFrameCB->Unmap(0, nullptr);
        mFrameCB.Reset();
    }
    mMappedFrameCB   = nullptr;
    mFrameCBCapacity = 0;

    const UINT64 byteSize = sizeof(WaterFrameConstants) * (requiredCount + 4);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mFrameCB))))
    {
        mLastError = "WaterRenderer: failed to allocate frame constant buffer.";
        return false;
    }
    if (FAILED(mFrameCB->Map(0, nullptr, reinterpret_cast<void**>(&mMappedFrameCB))))
    {
        mLastError = "WaterRenderer: failed to map frame constant buffer.";
        mFrameCB.Reset();
        return false;
    }

    mFrameCBCapacity = requiredCount + 4;
    return true;
}

bool WaterRenderer::EnsureMaterialConstantBuffer(std::size_t requiredCount)
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

    const UINT64 byteSize = sizeof(WaterMaterialConstants) * (requiredCount + 4);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mMaterialCB))))
    {
        mLastError = "WaterRenderer: failed to allocate material constant buffer.";
        return false;
    }
    if (FAILED(mMaterialCB->Map(0, nullptr, reinterpret_cast<void**>(&mMappedMatCB))))
    {
        mLastError = "WaterRenderer: failed to map material constant buffer.";
        mMaterialCB.Reset();
        return false;
    }

    mMatCBCapacity = requiredCount + 4;
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

    mRefractionTexture.Reset();

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

    mWaterDepth.Reset();

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

WaterRenderer::WaterMaterialInfo WaterRenderer::ResolveWaterMaterial(const std::string& materialPath) const
{
    WaterMaterialInfo info;
    if (materialPath.empty())
        return info;

    try
    {
        const std::filesystem::path materialFilePath = ResolveDataRelativePath(materialPath);
        if (materialFilePath.empty())
            return info;

        std::ifstream materialFile(materialFilePath);
        if (!materialFile.is_open())
            return info;

        nlohmann::json j;
        materialFile >> j;

        const nlohmann::json* w = &j;
        const auto waterIt = j.find("water");
        if (waterIt != j.end() && waterIt->is_object())
            w = &(*waterIt);

        info.ShallowTint  = ReadColor(*w, "shallowTint",  info.ShallowTint);
        info.Extinction   = ReadColor(*w, "extinction",   info.Extinction);
        info.FoamColor    = ReadColor(*w, "foamColor",    info.FoamColor);
        info.ScatterColor = ReadColor(*w, "scatterColor", info.ScatterColor);
        info.SssColor     = ReadColor(*w, "sssColor",     info.SssColor);

        info.Roughness          = w->value("roughness",          j.value("roughnessFactor", info.Roughness));
        info.Metallic           = w->value("metallic",           info.Metallic);
        info.BaseAmplitude      = w->value("waveAmplitude",      info.BaseAmplitude);
        info.BaseWavelength     = w->value("waveLength",         info.BaseWavelength);
        info.BaseSpeed          = w->value("waveSpeed",          info.BaseSpeed);
        info.Steepness          = w->value("steepness",          info.Steepness);
        info.Choppiness         = w->value("choppiness",         info.Choppiness);
        info.WaveCount          = w->value("waveCount",          info.WaveCount);
        info.DetailStrength     = w->value("detailStrength",     info.DetailStrength);
        info.FoamThreshold      = w->value("foamThreshold",      info.FoamThreshold);
        info.FresnelPower       = w->value("fresnelPower",       info.FresnelPower);
        info.RefractionStrength = w->value("refractionStrength", info.RefractionStrength);
        info.AbsorptionDistance = w->value("absorptionDistance", info.AbsorptionDistance);
        info.ShorelineFoam      = w->value("shorelineFoam",      info.ShorelineFoam);
        info.SunSpecPower       = w->value("sunSpecularIntensity", info.SunSpecPower);
        info.F0                 = w->value("fresnelF0",          info.F0);
        info.DirectionalSpread  = w->value("directionalSpread",  info.DirectionalSpread);
        info.SssPower           = w->value("scatterPower",       info.SssPower);
        info.SssDistortion      = w->value("scatterDistortion",  info.SssDistortion);
        info.DetailFadeDistance = w->value("detailFadeDistance", info.DetailFadeDistance);
        info.SsrStride          = w->value("reflectionStride",   info.SsrStride);
        info.SsrSteps           = w->value("reflectionSteps",    info.SsrSteps);
        info.SsrThickness       = w->value("reflectionThickness",info.SsrThickness);
        info.ReflectionStrength = w->value("reflectionStrength", info.ReflectionStrength);
        info.DistantRoughness   = w->value("distantRoughness",  info.DistantRoughness);
        info.DistantFlatten     = w->value("distantFlatten",    info.DistantFlatten);
        info.VolumeDesaturate   = w->value("volumeDesaturate",  info.VolumeDesaturate);

        info.UseFft               = w->value("useFFT",               info.UseFft);
        info.WindSpeed            = w->value("windSpeed",            info.WindSpeed);
        info.Fetch                = w->value("fetch",                info.Fetch);
        info.WaterDepth           = w->value("waterDepth",           info.WaterDepth);
        info.Swell                = w->value("swell",                info.Swell);
        info.FftNormalStrength    = w->value("fftNormalStrength",    info.FftNormalStrength);
        info.FftDisplacementScale = w->value("fftDisplacementScale", info.FftDisplacementScale);

        const auto patchIt = w->find("oceanPatchSizes");
        if (patchIt != w->end() && patchIt->is_array() && patchIt->size() >= 3)
        {
            info.PatchSizes = DirectX::XMFLOAT3(
                (*patchIt)[0].get<float>(),
                (*patchIt)[1].get<float>(),
                (*patchIt)[2].get<float>());
        }

        const auto windIt = w->find("windDirection");
        if (windIt != w->end() && windIt->is_array() && windIt->size() >= 2)
        {
            info.WindDir = DirectX::XMFLOAT2(
                (*windIt)[0].get<float>(),
                (*windIt)[1].get<float>());
        }

        const float len = std::sqrt(info.WindDir.x * info.WindDir.x + info.WindDir.y * info.WindDir.y);
        if (len > 1e-4f)
        {
            info.WindDir.x /= len;
            info.WindDir.y /= len;
        }
        else
        {
            info.WindDir = DirectX::XMFLOAT2(1.0f, 0.0f);
        }

        info.WaveCount = std::clamp(info.WaveCount, 1, 12);
        info.SsrSteps  = std::clamp(info.SsrSteps, 0, 32);
    }
    catch (...)
    {
        return WaterMaterialInfo{};
    }

    return info;
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

    // Root signature:
    //   b0 (ALL)  - frame constants        b1 (ALL)  - material constants
    //   t0 (PS)   - refraction scene copy   t1 (PS)   - opaque depth
    //   s0 linear-clamp, s1 point-clamp
    D3D12_DESCRIPTOR_RANGE refractRange{};
    refractRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    refractRange.NumDescriptors     = 1;
    refractRange.BaseShaderRegister = 0; // t0
    refractRange.RegisterSpace      = 0;
    refractRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors     = 1;
    depthRange.BaseShaderRegister = 1; // t1
    depthRange.RegisterSpace      = 0;
    depthRange.OffsetInDescriptorsFromTableStart = 0;

    // Six single-descriptor tables for the FFT cascades (3 displacement +
    // 3 normal).  Bound one per table rather than as contiguous ranges, matching
    // the approach TerrainRenderer uses for its paint layers.
    constexpr int kCascadeSlotBase = 4;
    D3D12_DESCRIPTOR_RANGE cascadeRanges[kOceanCascadeCount * 2]{};

    D3D12_ROOT_PARAMETER rootParams[kCascadeSlotBase + kOceanCascadeCount * 2]{};
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges   = &refractRange;
    rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges   = &depthRange;
    rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Cascade textures occupy t2..t7 (displacement first, then normals).
    for (int i = 0; i < kOceanCascadeCount * 2; ++i)
    {
        cascadeRanges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        cascadeRanges[i].NumDescriptors     = 1;
        cascadeRanges[i].BaseShaderRegister = static_cast<UINT>(2 + i);
        cascadeRanges[i].RegisterSpace      = 0;
        cascadeRanges[i].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER& param = rootParams[kCascadeSlotBase + i];
        param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges   = &cascadeRanges[i];
        // Displacement is sampled in the vertex shader, normals in the pixel
        // shader, so both sets need to be visible everywhere.
        param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
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

    // s2: linear WRAP, so the FFT cascades tile seamlessly across the surface.
    samplers[2] = samplers[0];
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

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    // Depth test + write against the pass's own depth buffer so the water
    // surface sorts against itself (without it, far crests overwrite near ones
    // and punch notches through the waves).  Occlusion against *opaque*
    // geometry is still done in the pixel shader from the scene depth SRV.
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

    SyncFromEntities();
    RebuildDirtyWater(commandList);

    if (mActiveIndices.empty())
        return;

    // --- Advance the FFT ocean (compute) before any render targets are bound ---
    // The sea state is global, so it is driven by the first water surface's
    // material rather than simulated per entity.
    bool fftActive = false;
    WaterMaterialInfo leadMaterial;
    {
        const Entity& lead = (*mEntities)[mActiveIndices.front()];
        if (lead.Water.has_value())
            leadMaterial = ResolveWaterMaterial(lead.Water->MaterialPath);

        if (mOceanReady && leadMaterial.UseFft)
        {
            OceanSpectrumSettings settings;
            settings.WindSpeed   = leadMaterial.WindSpeed;
            settings.WindAngle   = std::atan2(leadMaterial.WindDir.y, leadMaterial.WindDir.x);
            settings.Fetch       = leadMaterial.Fetch;
            settings.Depth       = leadMaterial.WaterDepth;
            settings.Swell       = leadMaterial.Swell;
            settings.SpreadBlend = leadMaterial.DirectionalSpread / 3.0f;
            settings.Amplitude   = leadMaterial.BaseAmplitude;
            settings.Choppiness  = leadMaterial.Choppiness;
            settings.FoamBias    = leadMaterial.FoamThreshold + 0.5f;
            settings.PatchSizes  = { leadMaterial.PatchSizes.x,
                                     leadMaterial.PatchSizes.y,
                                     leadMaterial.PatchSizes.z };

            mOcean.Update(commandList, settings, timeSeconds);
            fftActive = true;
        }
    }

    if (!mPipelineReady || sceneColorFormat != mSceneColorFormat)
    {
        if (!CreatePipeline(sceneColorFormat))
            return;
    }
    if (!EnsureRefractionTexture(width, height, sceneColorFormat))
        return;
    if (!EnsureDepthBuffer(width, height))
        return;
    if (!EnsureFrameConstantBuffer(mActiveIndices.size()))
        return;
    if (!EnsureMaterialConstantBuffer(mActiveIndices.size()))
        return;

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

    // Cascade tables must always be bound to something valid, even when the FFT
    // path is inactive: an unbound descriptor table is undefined behaviour even
    // if the shader never reads it.  The refraction SRV is a safe stand-in.
    for (int i = 0; i < kOceanCascadeCount; ++i)
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE displacement =
            fftActive ? mOcean.GetDisplacementSrv(i) : mRefractionSrvGpu;
        const D3D12_GPU_DESCRIPTOR_HANDLE normal =
            fftActive ? mOcean.GetNormalSrv(i) : mRefractionSrvGpu;
        commandList->SetGraphicsRootDescriptorTable(4 + i, displacement);
        commandList->SetGraphicsRootDescriptorTable(4 + kOceanCascadeCount + i, normal);
    }

    const DirectX::XMMATRIX viewProjTransposed = DirectX::XMMatrixTranspose(viewProjection);

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

        const DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
            entity.Transform.Scale.x, entity.Transform.Scale.y, entity.Transform.Scale.z);
        const DirectX::XMMATRIX rotationMatrix = PteroTransform::ComposeRotation(entity.Transform.Rotation);
        const DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(
            entity.Transform.Position.x, entity.Transform.Position.y, entity.Transform.Position.z);
        const DirectX::XMMATRIX modelMatrix = scaleMatrix * rotationMatrix * translationMatrix;

        WaterFrameConstants* frame = &mMappedFrameCB[slot];
        DirectX::XMStoreFloat4x4(&frame->ViewProj, viewProjTransposed);
        DirectX::XMStoreFloat4x4(&frame->Model, DirectX::XMMatrixTranspose(modelMatrix));
        frame->InvViewProj  = invViewProjTransposed;
        frame->CameraPos    = cameraPosition;
        frame->Time         = timeSeconds;
        frame->SunDirection = sunDirection;
        frame->ScreenWidth  = static_cast<float>(width);
        frame->SunColor     = sunColor;
        frame->ScreenHeight = static_cast<float>(height);
        frame->SkyColor     = skyColor;
        frame->DebugMode    = wc.DebugMode;

        const WaterMaterialInfo info = ResolveWaterMaterial(wc.MaterialPath);
        WaterMaterialConstants* mat = &mMappedMatCB[slot];
        mat->ShallowTint       = info.ShallowTint;
        mat->Extinction        = info.Extinction;
        mat->FoamColor         = info.FoamColor;
        mat->ScatterColor      = info.ScatterColor;
        mat->SssColor          = info.SssColor;
        mat->Roughness         = info.Roughness;
        mat->Metallic          = info.Metallic;
        mat->WaveScale         = (wc.WaveScale > 0.0f) ? wc.WaveScale : 1.0f;
        mat->BaseAmplitude     = info.BaseAmplitude;
        mat->BaseWavelength    = info.BaseWavelength;
        mat->BaseSpeed         = info.BaseSpeed;
        mat->Steepness         = info.Steepness;
        mat->Choppiness        = info.Choppiness;
        mat->WaveCount         = info.WaveCount;
        mat->DetailTiling      = (wc.DetailTiling > 0.0f) ? wc.DetailTiling : 24.0f;
        mat->DetailStrength    = info.DetailStrength;
        mat->FoamThreshold     = info.FoamThreshold;
        mat->WindDir           = info.WindDir;
        mat->FresnelPower      = info.FresnelPower;
        mat->RefractionStrength = info.RefractionStrength;
        mat->AbsorptionDistance = info.AbsorptionDistance;
        mat->ShorelineFoam      = info.ShorelineFoam;
        mat->SunSpecPower       = info.SunSpecPower;
        mat->F0                 = info.F0;
        mat->DirectionalSpread  = info.DirectionalSpread;
        mat->SssPower           = info.SssPower;
        mat->SssDistortion      = info.SssDistortion;
        mat->DetailFadeDistance = info.DetailFadeDistance;
        mat->SsrStride          = info.SsrStride;
        mat->SsrSteps           = info.SsrSteps;
        mat->SsrThickness       = info.SsrThickness;
        mat->ReflectionStrength = info.ReflectionStrength;
        // Band-limit the wave spectrum to what this entity's mesh can resolve.
        // Uses the largest of the two axis spacings so neither direction aliases.
        {
            const int   res = (wc.Resolution > 1) ? wc.Resolution : 2;
            const float spacingX = wc.SizeX / static_cast<float>(res - 1);
            const float spacingY = wc.SizeY / static_cast<float>(res - 1);
            mat->GridSpacing = (std::max)(spacingX, spacingY);
        }
        mat->DistantRoughness   = info.DistantRoughness;
        mat->DistantFlatten     = info.DistantFlatten;
        mat->VolumeDesaturate   = info.VolumeDesaturate;
        mat->OceanPatchSizes    = info.PatchSizes;
        // Only enable the FFT path for surfaces whose material matches the sea
        // state actually being simulated this frame.
        mat->FftEnabled         = (fftActive && info.UseFft) ? 1.0f : 0.0f;
        mat->FftNormalStrength  = info.FftNormalStrength;
        mat->FftDisplacementScale = info.FftDisplacementScale;

        commandList->SetGraphicsRootConstantBufferView(
            0, mFrameCB->GetGPUVirtualAddress() + slot * sizeof(WaterFrameConstants));
        commandList->SetGraphicsRootConstantBufferView(
            1, mMaterialCB->GetGPUVirtualAddress() + slot * sizeof(WaterMaterialConstants));

        commandList->IASetVertexBuffers(0, 1, &state.VertexBufferView);
        commandList->IASetIndexBuffer(&state.IndexBufferView);
        commandList->DrawIndexedInstanced(state.IndexCount, 1, 0, 0, 0);
    }
}
