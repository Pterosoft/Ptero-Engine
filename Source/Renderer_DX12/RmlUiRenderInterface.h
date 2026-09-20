#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Rml::RenderInterface implemented against the engine's own DX12 device.
//
// The RmlUi SDK ships a DirectX 12 backend under SDKs/RmlUI/Backends, but it creates and
// owns its own adapter, device, command queue and swap chain, so it cannot draw into a
// renderer that already has all of those. The core library is deliberately renderer
// agnostic, so the engine implements the interface itself and records into whichever
// command list the frame is already using.
//
// Only the eight pure-virtual entry points are strictly required; SetTransform is
// overridden as well so RmlUi's CSS transforms work. Layers, filters and shader effects
// keep the base class's no-op behaviour.
class RmlUiRenderInterface final : public Rml::RenderInterface
{
public:
    RmlUiRenderInterface() = default;
    ~RmlUiRenderInterface() override;

    RmlUiRenderInterface(const RmlUiRenderInterface&) = delete;
    RmlUiRenderInterface& operator=(const RmlUiRenderInterface&) = delete;

    // The upload command list is only needed for the one-off white fallback texture; the
    // engine hands in whichever list its own initialization is already recording into.
    bool Initialize(DXGI_FORMAT renderTargetFormat, ID3D12GraphicsCommandList* uploadCommandList);
    void Shutdown();

    bool IsInitialized() const { return mIsInitialized; }

    // The command list every draw and every texture upload is recorded into. RmlUi calls
    // back into this interface from inside Context::Render, so the list has to be set for
    // the frame before rendering starts and cleared again afterwards.
    void BeginFrame(ID3D12GraphicsCommandList* commandList, UINT viewportWidth, UINT viewportHeight);
    void EndFrame();

    // Directory that relative texture sources in .rml/.rcss documents resolve against.
    void SetDocumentDirectory(std::wstring directory) { mDocumentDirectory = std::move(directory); }

    const std::string& GetLastErrorMessage() const { return mLastError; }

    // -- Rml::RenderInterface --

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct alignas(256) RmlUiConstants
    {
        float Transform[16] = {};
        float TranslationX = 0.0f;
        float TranslationY = 0.0f;
        float Padding[2] = {};
    };

    // Vertex and index data live in upload-heap buffers: RmlUi compiles geometry once and
    // redraws it for many frames, so there is no per-frame staging traffic to avoid, and
    // this keeps release as simple as dropping the resource.
    struct CompiledGeometry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
        UINT IndexCount = 0;
    };

    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        // The upload buffer is kept alive for the texture's lifetime rather than tracked
        // per frame: UI atlases are uploaded once and are small enough that holding the
        // staging copy is cheaper than a deferred-release queue.
        Microsoft::WRL::ComPtr<ID3D12Resource> Upload;
        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle{};
    };

    bool CreateRootSignature();
    bool CreatePipelineState(DXGI_FORMAT renderTargetFormat);
    bool CreateConstantBuffer();
    bool CreateWhiteTexture();

    // Allocates an SRV slot, reusing one freed by an earlier ReleaseTexture. The engine's
    // shared descriptor heap is a bump allocator with no free list of its own, so
    // recycling here is what keeps a UI that swaps documents from exhausting it.
    bool AcquireSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle);
    void ReleaseSrvDescriptor(const Texture& texture);

    Rml::TextureHandle CreateTextureFromPixels(
        const void* pixels,
        UINT width,
        UINT height,
        UINT rowPitch);

    void ApplyScissor();
    void UploadConstants(const Rml::Vector2f& translation);

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    // Ring of constant-buffer slots: every draw needs its own translation, and the GPU is
    // still reading earlier draws when later ones are recorded.
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedConstants = nullptr;
    UINT mConstantSlotIndex = 0;

    Texture mWhiteTexture;

    std::unordered_map<std::uintptr_t, CompiledGeometry> mGeometry;
    std::unordered_map<std::uintptr_t, Texture> mTextures;
    std::uintptr_t mNextGeometryHandle = 1;
    std::uintptr_t mNextTextureHandle = 1;

    // Deferred release for UI geometry and textures.
    //
    // RmlUi frees these the moment a document closes or an element's markup is
    // rewritten, and game code does both from inside its Update - which the engine
    // calls at the top of the frame, with this frame's command list already open and
    // the previous two frames still executing. Dropping the buffers there hands back
    // memory that submitted draws are still reading, which the GPU reports as a page
    // fault in the middle of a run of DrawIndexedInstanced, with nothing in the log
    // to connect it to the document swap that caused it.
    //
    // It only shows up with game code that actually changes its UI: a game that just
    // moves the camera never releases anything, which is why this looked like a
    // renderer bug rather than a lifetime one.
    //
    // Holding each release for one more frame than the engine keeps in flight is
    // enough, and the count is ticked in BeginFrame. The SRV descriptor slot is
    // recycled at the same moment, not earlier - handing the slot to a new texture
    // while an in-flight frame still samples through it is the same bug one level up.
    static constexpr int kFramesInFlight = 3;
    struct PendingGeometryRelease
    {
        CompiledGeometry Geometry;
        int FramesRemaining = 0;
    };
    struct PendingTextureRelease
    {
        Texture TextureData;
        int FramesRemaining = 0;
    };
    std::vector<PendingGeometryRelease> mPendingGeometryReleases;
    std::vector<PendingTextureRelease> mPendingTextureReleases;
    void TickPendingReleases();

    std::vector<std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>> mFreeSrvDescriptors;

    ID3D12GraphicsCommandList* mCommandList = nullptr;
    UINT mViewportWidth = 0;
    UINT mViewportHeight = 0;

    bool mScissorEnabled = false;
    D3D12_RECT mScissorRect{};

    float mProjection[16] = {};
    float mTransform[16] = {};
    bool mHasTransform = false;

    std::wstring mDocumentDirectory;
    std::string mLastError;
    bool mIsInitialized = false;
};
