#pragma once

#include "DX12Helper.h"

#include <d3d12.h>
#include <wrl/client.h>

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

    // Release all cached GPU resources (call before device destruction).
    void Shutdown();

private:
    struct CachedTextureEntry
    {
        std::shared_ptr<GpuTexture> Texture;
        std::filesystem::file_time_type LastWriteTime{};
    };

    // Execute a one-shot copy command to flush upload heaps to the GPU.
    bool FlushUploads(std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadBuffers);

    std::unordered_map<std::string, CachedTextureEntry> mCache;
    std::string mLastError;
};
