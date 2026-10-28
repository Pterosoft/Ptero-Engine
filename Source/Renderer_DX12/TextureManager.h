#pragma once

#include "DX12Helper.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// A single GPU texture entry: the committed resource and the SRV descriptor handles.
struct GpuTexture
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle{};

    bool IsValid() const { return Resource != nullptr; }
};

enum class TextureSemantic
{
    Auto,
    Color,
    Normal,
    MaterialMask,
};

// TextureManager loads DDS files from disk into GPU memory (committed VRAM resource)
// using DDSTextureLoader12 and allocates an SRV slot from the shared descriptor heap.
// Results are cached by file path so each DDS is only uploaded once per session.
class TextureManager
{
public:
    // Load the DDS at ddsPath.  Returns a shared pointer to the cached entry,
    // or nullptr if loading failed.  Call LastError() for diagnostics.
    std::shared_ptr<GpuTexture> LoadDDS(const std::string& ddsPath, TextureSemantic semantic = TextureSemantic::Auto);

    const std::string& LastError() const { return mLastError; }

    // Whether ddsPath is already on the GPU, without touching the disk.
    bool IsCached(const std::string& ddsPath) const
    {
        const auto it = mCache.find(ddsPath);
        return it != mCache.end() && it->second.Texture != nullptr;
    }

    // Release all cached GPU resources (call before device destruction).
    void Shutdown();

private:
    struct CachedTextureEntry
    {
        std::shared_ptr<GpuTexture> Texture;
        std::filesystem::file_time_type LastWriteTime{};
        // When this entry's write time was last checked against disk.
        std::chrono::steady_clock::time_point LastCheckTime{};
    };

    // How long a cached texture is trusted before its write time is checked
    // again. Hot reloading a texture from an external editor still works; it
    // just takes up to this long to appear, which is imperceptible next to the
    // cost of stat-ing every texture of every sub-mesh every frame.
    static constexpr std::chrono::milliseconds kRevalidateInterval{ 250 };

    // Execute a one-shot copy command to flush upload heaps to the GPU.
    bool FlushUploads(std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadBuffers);

    // Upload machinery kept across loads. Every load waits for its copy before
    // returning, so one of each is always free again by the next load. Creating
    // them per texture meant a fresh upload heap (tens of MB for a 4K tile), command
    // allocator, list, fence and event for every file: a level with a couple of
    // hundred UDIM tiles churned gigabytes of kernel allocations back to back,
    // which was enough to make other applications' audio crackle while it loaded.
    bool EnsureUploadObjects(ID3D12Device* device, UINT64 uploadBytes);
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    mUploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mUploadCommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               mUploadFence;
    HANDLE                                            mUploadFenceEvent = nullptr;
    UINT64                                            mUploadFenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource>            mUploadBuffer;
    UINT64                                            mUploadBufferCapacity = 0;
    // The file's bytes, read into the same allocation each time for the same reason.
    std::vector<std::uint8_t>                         mReadBuffer;

    std::unordered_map<std::string, CachedTextureEntry> mCache;
    std::string mLastError;
};
