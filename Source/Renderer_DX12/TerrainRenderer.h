#pragma once

// TerrainRenderer
// ---------------
// Owns one GPU mesh per TerrainComponent and draws it into the G-Buffer
// during the geometry pass using a terrain-specific PSO.  The CPU-side
// heightmap samples (uint16) are kept here too so brush operations can
// edit them in-place and re-upload the VB/IB and DDS without going through
// the editor's transient buffers.

#include "Components.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TerrainGeometry.h"
#include "TextureManager.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

class TerrainRenderer
{
public:
    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Shutdown();

    // Set the current scene's entity list so the renderer can upload/replace
    // per-entity terrain meshes and decide what to draw each frame.
    void SetEntities(std::vector<Entity>* entities);

    // Re-evaluate which entities have a TerrainComponent and (re)build the
    // GPU resources for any that changed since the previous call.  Cheap if
    // nothing changed; rebuilds the VB/IB on the next frame if a brush edit
    // (or a property change) flagged the terrain dirty.
    void SyncFromEntities();

    // Record draw calls for every entity that has a fully built terrain
    // into the G-Buffer (albedo, normal, material MRT).  The G-Buffer RTs
    // must already be bound as render targets.
    void Render(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMMATRIX& viewProjection,
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT msaaSampleCount = 1);

    // Apply one stroke of the currently configured brush at the given
    // world-space XY position. Marks the terrain dirty so the VB/IB gets
    // rebuilt on the next SyncFromEntities / Render pass.  The optional
    // `outStatusMessage` is populated with a short status text (e.g. for
    // the editor to display in the status bar).  Returns false if no
    // terrain exists at the picked position.
    bool ApplyBrushAt(
        const DirectX::XMFLOAT2& worldXZ,
        std::string* outStatusMessage = nullptr);

    // Recompute the GPU mesh from the CPU heightmap.  Called automatically
    // by Render() if anything flagged the terrain dirty; can also be called
    // explicitly after a brush stroke when you want to update immediately.
    void RebuildDirtyTerrains(ID3D12GraphicsCommandList* commandList);

    void SetWireframeEnabled(bool enabled)
    {
        if (mWireframeEnabled != enabled)
        {
            mWireframeEnabled = enabled;
            mPipelineReady = false;
        }
    }

    bool IsWireframeEnabled() const { return mWireframeEnabled; }

    void SetTextureMipLODBias(float bias)
    {
        if ((std::fabs)(mTextureMipLODBias - bias) > 0.001f)
        {
            mTextureMipLODBias = bias;
            mPipelineReady = false;
        }
    }

    // Helper for the editor: ray-pick the terrain under a world XY ground-plane position.
    // Returns true and fills outWorldY with the heightmap-derived world
    // height if the point is inside the terrain patch.
    bool SampleHeightAt(
        const DirectX::XMFLOAT2& worldXZ,
        float& outWorldY) const;

    // World-space surface normal at the given ground-plane position, derived
    // from central differences on the heightmap.  Used by the vegetation
    // scatter to align instances and to reject slopes that are too steep.
    // Returns false when the point lies outside every terrain patch.
    bool SampleNormalAt(
        const DirectX::XMFLOAT2& worldXZ,
        DirectX::XMFLOAT3& outNormal) const;

    // Bilinear sample of the per-sample paint-layer splat weights (RGBA =
    // layers 0..3) at the given ground-plane position.  This lets vegetation
    // spawn only where a given surface layer has been painted, which is how a
    // grass layer follows the painted grass without a second mask asset.
    // Returns false when the point lies outside every terrain patch or the
    // terrain has never been painted.
    bool SampleLayerWeightsAt(
        const DirectX::XMFLOAT2& worldXZ,
        DirectX::XMFLOAT4& outWeights) const;

    // Monotonic counter bumped whenever a brush stroke or rebuild changes the
    // heightmap or splat weights.  The vegetation system polls this so an area
    // sitting on edited terrain re-scatters instead of leaving instances
    // floating above or buried under the new surface.
    std::uint64_t GetTerrainRevision() const { return mTerrainRevision; }

    // Immutable copy of one terrain patch's CPU data.
    struct PatchData
    {
        std::size_t EntityIndex = 0;
        int    Width        = 0;
        int    Height       = 0;
        float  WorldSize    = 0.0f;
        float  HeightScale  = 0.0f;
        float  HeightOffset = 0.0f;
        DirectX::XMFLOAT3 Origin{};
        std::vector<std::uint16_t> Samples;
        std::vector<DirectX::XMFLOAT4> LayerWeights;  // empty when never painted
    };

    // Copy out every built terrain patch.  The vegetation scatter runs on a
    // worker thread, and the brush edits these buffers in place on the main
    // thread, so the scatter takes a snapshot rather than reading them live.
    void ExportPatches(std::vector<PatchData>& outPatches) const;

    // Force-rebuild the GPU mesh for the given terrain entity index on the
    // next sync (used by the editor when WorldSize/HeightScale/HeightOffset
    // change).
    void MarkTerrainDirty(std::size_t entityIndex);

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

    bool GetTerrainDebugInfo(
        std::size_t entityIndex,
        std::uint32_t& outIndexCount,
        int& outBuiltWidth,
        int& outBuiltHeight,
        bool& outDirty) const
    {
        const auto it = mGpuStates.find(entityIndex);
        if (it == mGpuStates.end())
            return false;

        outIndexCount = it->second.IndexCount;
        outBuiltWidth = it->second.BuiltWidth;
        outBuiltHeight = it->second.BuiltHeight;
        outDirty = it->second.Dirty;
        return true;
    }

private:
    // Per-entity GPU/CPU state for one terrain patch.
    struct TerrainGpuState
    {
        // CPU heightmap samples, kept in sync with the .raw source.
        std::vector<std::uint16_t> Samples;
        // Per-sample paint-layer weights (RGBA = layers 0..3), baked into the
        // mesh vertex colour.  Same row-major layout as Samples.
        std::vector<DirectX::XMFLOAT4> LayerWeights;
        // Last build parameters; the renderer rebuilds if any of these
        // change compared to the entity's current settings.
        int    BuiltWidth        = 0;
        int    BuiltHeight       = 0;
        float  BuiltWorldSize    = 0.0f;
        float  BuiltHeightScale  = 0.0f;
        float  BuiltHeightOffset = 0.0f;
        std::string BuiltHeightmapPath;
        bool   Dirty             = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUpload;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        std::uint32_t IndexCount = 0;
    };

    // Constant buffer layout – 256-byte aligned to match the rest of the engine.
    struct alignas(256) TerrainConstants
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4X4 Model;
        std::byte Padding[128]{};
    };
    static_assert(sizeof(TerrainConstants) == 256);

    // Material CB layout – 256-byte aligned, mirrors Terrain.hlsl b1.
    // Layout must match the HLSL cbuffer field-for-field (HLSL 16-byte
    // register packing rules).
    struct alignas(256) TerrainMaterial
    {
        DirectX::XMFLOAT4 BaseTint          = { 1.f, 1.f, 1.f, 1.f };
        float             Metallic           = 0.0f;
        float             Roughness          = 0.9f;
        float             AoStrength         = 1.0f;
        int               LayerCount         = 0;   // 0 = legacy single-material
        DirectX::XMFLOAT4 LayerTileScale     = { 16.f, 16.f, 16.f, 16.f };
        DirectX::XMINT4   LayerHasTex        = { 0, 0, 0, 0 };
        DirectX::XMFLOAT4 LayerTint[4]       = {
            { 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f },
            { 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f } };
        int               HasBaseMap         = 0;
        int               UseVertexColour    = 0;
        DirectX::XMINT2   _Pad0              = {};
        std::byte         Padding[112]{};
    };
    static_assert(sizeof(TerrainMaterial) == 256);

    struct TerrainMaterialInfo
    {
        std::string BaseColorTexturePath;
        DirectX::XMFLOAT4 BaseTint = { 0.78f, 0.48f, 0.16f, 1.0f };
        float Metallic = 0.0f;
        float Roughness = 0.9f;
        float AoStrength = 1.0f;
    };

    bool CreatePipeline(
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT msaaSampleCount = 1);
    bool CreateFallbackTexture(ID3D12GraphicsCommandList* commandList);
    bool EnsureConstantBuffer(std::size_t requiredCount);
    bool EnsureMaterialConstantBuffer(std::size_t requiredCount);
    TerrainMaterialInfo ResolveTerrainMaterial(const std::string& materialPath) const;
    bool EnsureGpuMesh(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const Entity& entity,
        TerrainGpuState& state);

    // Worker that actually creates the GPU resources; EnsureGpuMesh wraps
    // it so the dirty flag is always cleared on failure.
    bool EnsureGpuMeshImpl(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const Entity& entity,
        TerrainGpuState& state);

    std::vector<Entity>* mEntities = nullptr;
    std::unordered_map<std::size_t, TerrainGpuState> mGpuStates;
    // Indices (into *mEntities) the renderer is currently drawing; updated
    // each SyncFromEntities.
    std::vector<std::size_t> mActiveIndices;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    TerrainConstants*  mMappedCB       = nullptr;
    std::size_t        mCBCapacity     = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mMaterialCB;
    TerrainMaterial*   mMappedMatCB    = nullptr;
    std::size_t        mMatCBCapacity  = 0;

    // 1x1 white fallback used when the terrain has no base-colour texture.
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackTextureResource;
    D3D12_CPU_DESCRIPTOR_HANDLE mFallbackCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mFallbackGpuHandle{};
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackUploadBuffer;
    TextureManager mTextureManager;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    bool        mPipelineReady    = false;
    DXGI_FORMAT mAlbedoFormat     = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mNormalFormat     = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mMaterialFormat   = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthFormat      = DXGI_FORMAT_UNKNOWN;
    UINT        mMsaaSampleCount  = 1;
    bool        mWireframeEnabled = false;
    float       mTextureMipLODBias = -1.5f;
    std::string mLastError;

    // Bumped by every brush stroke and every rebuild; see GetTerrainRevision.
    std::uint64_t mTerrainRevision = 0;
};
