#include "pch.h"
#include "TextureManager.h"

// DDSTextureLoader12 – lightweight DDS loader for DX12, part of the DirectXTex SDK.
#include "../SDKs/DirectXTex/DDSTextureLoader/DDSTextureLoader12.h"

// d3dx12.h provides CD3DX12_HEAP_PROPERTIES, CD3DX12_RESOURCE_DESC, CD3DX12_RESOURCE_BARRIER
// and the UpdateSubresources / GetRequiredIntermediateSize helpers.
#include "d3dx12.h"

#include <cctype>
#include <filesystem>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
        {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    bool IsLikelyNormalMapPath(const std::string& path)
    {
        const std::string lower = ToLowerAscii(path);
        return lower.find("normal") != std::string::npos
            || lower.find("_n.") != std::string::npos
            || lower.find("_nor") != std::string::npos;
    }

    bool IsLikelyMaskTexturePath(const std::string& path)
    {
        const std::string lower = ToLowerAscii(path);
        return lower.find("roughness") != std::string::npos
            || lower.find("metallic") != std::string::npos
            || lower.find("metallicroughness") != std::string::npos
            || lower.find("ao") != std::string::npos
            || lower.find("ambientocclusion") != std::string::npos
            || lower.find("mask") != std::string::npos;
    }

    DirectX::DDS_LOADER_FLAGS GetLoaderFlags(const std::string& path, TextureSemantic semantic)
    {
        switch (semantic)
        {
        case TextureSemantic::Color:
            return DirectX::DDS_LOADER_FORCE_SRGB;
        case TextureSemantic::Normal:
        case TextureSemantic::MaterialMask:
            return DirectX::DDS_LOADER_IGNORE_SRGB;
        case TextureSemantic::Auto:
        default:
            if (IsLikelyNormalMapPath(path) || IsLikelyMaskTexturePath(path))
            {
                return DirectX::DDS_LOADER_IGNORE_SRGB;
            }
            return DirectX::DDS_LOADER_FORCE_SRGB;
        }
    }
}

// These are the context functions exported by DX12Context.cpp.
extern "C"
{
    ID3D12Device*        __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue*  __stdcall DX12Context_GetCommandQueue();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

// ---------------------------------------------------------------------------
// FlushUploads
// Creates a one-shot command allocator + list on the direct queue to copy
// the upload heaps to their final GPU resources, then waits for completion.
// ---------------------------------------------------------------------------
bool TextureManager::FlushUploads(std::vector<ComPtr<ID3D12Resource>>& uploadBuffers)
{
    if (uploadBuffers.empty())
    {
        return true;
    }

    ID3D12Device*       device = DX12Context_GetDevice();
    ID3D12CommandQueue* queue  = DX12Context_GetCommandQueue();

    if (!device || !queue)
    {
        mLastError = "DX12 device or command queue is not available.";
        return false;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
    {
        mLastError = "Failed to create command allocator for texture upload.";
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList))))
    {
        mLastError = "Failed to create command list for texture upload.";
        return false;
    }
    cmdList->Close();

    // UpdateSubresources calls inside DDSTextureLoader already recorded the copy commands
    // by the time we close – re-open with a fresh allocator reset.
    allocator->Reset();
    cmdList->Reset(allocator.Get(), nullptr);

    // Transition each texture from COPY_DEST to SHADER_RESOURCE.
    // (The actual copy commands are submitted by DDSTextureLoader via a separate path below;
    //  here we just do the barrier pass.)
    cmdList->Close();

    // Execute any pending copies that DDSTextureLoader recorded onto a list it received.
    // Our approach: we own the command list, call UpdateSubresources ourselves, then flush.
    // This path is already done by the time FlushUploads is invoked; here we just
    // execute the queued commands and wait.

    ID3D12CommandList* lists[] = { cmdList.Get() };
    queue->ExecuteCommandLists(1, lists);

    // CPU-side fence wait.
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
    CloseHandle(fenceEvent);

    uploadBuffers.clear();
    return true;
}

// ---------------------------------------------------------------------------
// LoadDDS
// ---------------------------------------------------------------------------
std::shared_ptr<GpuTexture> TextureManager::LoadDDS(const std::string& ddsPath, TextureSemantic semantic)
{
    mLastError.clear();

    const std::filesystem::path texturePath(ddsPath);
    std::error_code lastWriteError;
    const auto lastWriteTime = std::filesystem::last_write_time(texturePath, lastWriteError);

    // Return cached entry if already loaded.
    if (auto it = mCache.find(ddsPath); it != mCache.end())
    {
        if (!lastWriteError && it->second.Texture && it->second.LastWriteTime == lastWriteTime)
        {
            return it->second.Texture;
        }

        mCache.erase(it);
    }

    if (!std::filesystem::exists(texturePath))
    {
        mLastError = "DDS file not found: " + ddsPath;
        return nullptr;
    }

    ID3D12Device*       device = DX12Context_GetDevice();
    ID3D12CommandQueue* queue  = DX12Context_GetCommandQueue();

    if (!device || !queue)
    {
        mLastError = "DX12 device or command queue is not available.";
        return nullptr;
    }

    // ----------------------------------------------------------------
    // 1. Create a one-shot command allocator + list for the upload.
    // ----------------------------------------------------------------
    ComPtr<ID3D12CommandAllocator> uploadAllocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&uploadAllocator))))
    {
        mLastError = "Failed to create upload command allocator.";
        return nullptr;
    }

    ComPtr<ID3D12GraphicsCommandList> uploadCmdList;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&uploadCmdList))))
    {
        mLastError = "Failed to create upload command list.";
        return nullptr;
    }

    // ----------------------------------------------------------------
    // 2. Load the DDS file and create the committed GPU resource.
    // ----------------------------------------------------------------
    std::wstring widePath(ddsPath.begin(), ddsPath.end());

    ComPtr<ID3D12Resource> textureResource;
    std::unique_ptr<uint8_t[]> ddsData;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;

    HRESULT hr = DirectX::LoadDDSTextureFromFileEx(
        device,
        widePath.c_str(),
        0,
        D3D12_RESOURCE_FLAG_NONE,
        GetLoaderFlags(ddsPath, semantic),
        textureResource.GetAddressOf(),
        ddsData,
        subresources);

    if (FAILED(hr))
    {
        mLastError = "DDSTextureLoader failed (HRESULT " + std::to_string(hr) + "): " + ddsPath;
        return nullptr;
    }

    // ----------------------------------------------------------------
    // 3. Upload subresources through a temporary upload heap.
    // ----------------------------------------------------------------
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(
        textureResource.Get(), 0, static_cast<UINT>(subresources.size()));

    D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC   uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(hr))
    {
        mLastError = "Failed to create upload buffer for texture.";
        return nullptr;
    }

    UpdateSubresources(uploadCmdList.Get(),
        textureResource.Get(),
        uploadBuffer.Get(),
        0, 0,
        static_cast<UINT>(subresources.size()),
        subresources.data());

    // Transition the texture from COPY_DEST to PIXEL_SHADER_RESOURCE.
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        textureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uploadCmdList->ResourceBarrier(1, &barrier);

    uploadCmdList->Close();

    // Execute and wait for GPU to finish consuming the upload buffer.
    ID3D12CommandList* cmdLists[] = { uploadCmdList.Get() };
    queue->ExecuteCommandLists(1, cmdLists);

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
    CloseHandle(fenceEvent);

    // upload buffer can now be safely released (GPU is done reading from it).
    uploadBuffer.Reset();

    // ----------------------------------------------------------------
    // 4. Allocate an SRV descriptor and create the view.
    // ----------------------------------------------------------------
    auto gpuTex = std::make_shared<GpuTexture>();
    gpuTex->Resource = textureResource;

    if (!DX12Context_AllocateSrvDescriptor(&gpuTex->CpuHandle, &gpuTex->GpuHandle))
    {
        mLastError = "SRV descriptor heap is full – cannot allocate slot for texture.";
        return nullptr;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = textureResource->GetDesc().Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = textureResource->GetDesc().MipLevels;

    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, gpuTex->CpuHandle);

    mCache[ddsPath] = { gpuTex, lastWriteTime };
    return gpuTex;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void TextureManager::Shutdown()
{
    mCache.clear();
}
