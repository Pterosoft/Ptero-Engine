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

#include <cstddef>
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

    // Apply one frame's worth of the currently configured brush at the given
    // world-space XY position.  Called every frame while the mouse is held;
    // `deltaSeconds` scales the stroke so Strength means metres (or paint
    // weight) per second regardless of frame rate.  Only the touched part of
    // the mesh is re-uploaded; disk writes wait for EndBrushStroke.  The
    // optional `outStatusMessage` receives a short status text.  Returns
    // false if no terrain exists at the picked position.
    bool ApplyBrushAt(
        const DirectX::XMFLOAT2& worldXZ,
        float deltaSeconds,
        std::string* outStatusMessage = nullptr);

    // Finish the current stroke: write the edited heightmap DDS / splat map
    // to disk once and let dependent systems (vegetation) catch up.
    void EndBrushStroke(std::string* outStatusMessage = nullptr);

    // Ray against every terrain surface (world space).  Returns the nearest
    // hit within maxDistance.  Used to place the brush where the cursor
    // actually touches the terrain rather than on the z = 0 grid.
    bool Raycast(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        float maxDistance,
        DirectX::XMFLOAT3& outHit) const;
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

    // Camera data for adaptive tessellation; call before Render each frame.
    // pixelScale = (viewport height / 2) / tan(fovY / 2).
    void SetTessellationView(const DirectX::XMFLOAT3& cameraPosition, float pixelScale)
    {
        mCameraPosition = cameraPosition;
        mTessPixelScale = pixelScale;
    }

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

    // Paint layer `layer` over the whole patch and save the splat map.
    void FillPaintLayer(std::size_t entityIndex, int layer);

    // The editor is about to erase paint layer `layer`: drop its splat channel
    // and shift the higher layers' channels down so every remaining layer keeps
    // what was painted with it.  Saves the splat map.
    void RemovePaintLayerChannel(std::size_t entityIndex, int layer);

    // Throw away the in-memory heightmap (including unsaved brush edits) and
    // reload it from disk on the next frame.  Used after the component's
    // SculptedHeightmapPath is cleared to revert sculpting.
    void ReloadTerrain(std::size_t entityIndex);

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
        // change compared to the entity's current settings.  Recorded even
        // when the build fails, so a bad heightmap is not re-decoded every
        // frame until the artist changes something.
        int    BuiltWidth        = 0;
        int    BuiltHeight       = 0;
        float  BuiltWorldSize    = 0.0f;
        float  BuiltHeightScale  = 0.0f;
        float  BuiltHeightOffset = 0.0f;
        std::string BuiltHeightmapPath;
        bool   BuiltWithLayers   = false;
        bool   Dirty             = false;
        bool   LoadFailed        = false;

        // CPU copy of the uploaded vertices (mesh resolution, which is capped
        // below the heightmap resolution), so a brush stroke can patch the
        // touched rows instead of rebuilding a million vertices a frame.
        std::vector<TerrainVertex> MeshVertices;
        int    MeshWidth  = 0;
        int    MeshHeight = 0;
        // Heightmap samples edited since the last upload.
        TerrainGeometry::GridRect DirtySamples;
        // Edits not yet written to disk; flushed by EndBrushStroke.
        bool   PendingHeightSave = false;
        bool   PendingSplatSave  = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        std::uint32_t IndexCount = 0;
    };
    // Constant buffer layout – 256-byte aligned to match the rest of the engine.
    struct alignas(256) TerrainConstants
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4X4 Model;
        DirectX::XMFLOAT3   CameraPositionWS = { 0.f, 0.f, 0.f };
        float               TessPixelScale   = 1000.f;
        std::byte Padding[112]{};
    };
    static_assert(sizeof(TerrainConstants) == 256);

    // Material CB layout – mirrors Terrain.hlsl b1 field-for-field (HLSL
    // 16-byte register packing rules): a 64-byte header, then one 128-byte
    // block per paint layer.
    struct TerrainLayerConstants
    {
        DirectX::XMFLOAT4 BaseTint           = { 1.f, 1.f, 1.f, 1.f };
        DirectX::XMFLOAT2 UvTiling           = { 1.f, 1.f };
        DirectX::XMFLOAT2 UvOffset           = { 0.f, 0.f };
        float             UvRotationSin      = 0.0f;
        float             UvRotationCos      = 1.0f;
        float             TileSize           = 4.0f;    // metres per repeat
        float             NormalScale        = 1.0f;
        float             Roughness          = 0.9f;
        float             Metallic           = 0.0f;
        float             AoStrength         = 1.0f;
        float             Specular           = 0.5f;
        int               HasBaseMap         = 0;
        int               HasNormalMap       = 0;
        int               HasRoughnessMap    = 0;
        int               HasMetallicMap     = 0;
        int               HasAoMap           = 0;
        int               HasPackedMaterialMap = 0;
        int               HasHeightMap       = 0;
        int               FlipNormalGreen    = 0;
        float             DisplacementScale  = 0.0f;    // 0 = this layer does not displace
        float             DisplacementMidLevel = 0.5f;
        float             _Pad0[2]           = {};
        float             _Pad1[4]           = {};
    };
    static_assert(sizeof(TerrainLayerConstants) == 128);

    struct alignas(256) TerrainMaterial
    {
        int               LayerCount         = 1;
        int               UseSplat           = 0;   // 0 = one implicit layer (terrain material)
        int               BreakUpTiling      = 0;
        int               UseTessellation    = 0;
        int               HeightBlend        = 0;
        float             HeightBlendSharpness = 0.6f;
        float             MaxDisplacement    = 0.0f;
        float             _HeaderPad0        = 0.0f;
        float             TessMaxFactor      = 16.0f;
        float             TessTargetPixels   = 8.0f;
        float             TessFadeDistance   = 60.0f;
        float             _HeaderPad1        = 0.0f;
        float             _HeaderPad2[4]     = {};
        TerrainLayerConstants Layers[kTerrainMaxLayers];
    };
    static_assert(offsetof(TerrainMaterial, Layers) == 64);
    static_assert(sizeof(TerrainMaterial) == 768);

    // Everything the terrain uses from its material JSON (same keys as meshes).
    struct TerrainMaterialInfo
    {
        std::string BaseColorTexturePath;
        std::string NormalTexturePath;
        std::string RoughnessTexturePath;
        std::string MetallicTexturePath;
        std::string PackedMaterialTexturePath;   // "metallicRoughness", or "orm" when that is empty
        bool        PackedMaterialIsOrm = false; // channel layout of the above: ORM rather than RMA
        std::string AoTexturePath;
        std::string HeightTexturePath;
        DirectX::XMFLOAT4 BaseTint = { 0.78f, 0.48f, 0.16f, 1.0f };
        float Metallic = 0.0f;
        float Roughness = 0.9f;
        float Specular = 0.5f;
        float AoStrength = 1.0f;
        float NormalScale = 1.0f;
        bool  FlipNormalGreen = false;
        DirectX::XMFLOAT2 UvTiling = { 1.0f, 1.0f };
        DirectX::XMFLOAT2 UvOffset = { 0.0f, 0.0f };
        float UvRotationDegrees = 0.0f;
        bool  UseTessellation = false;
        float TessMaxFactor = 16.0f;
        float TessTargetPixels = 8.0f;
        float TessFadeDistance = 60.0f;
        float DisplacementScale = 0.05f;
        float DisplacementMidLevel = 0.5f;
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
    const TerrainMaterialInfo& GetCachedTerrainMaterial(const std::string& materialPath);
    bool EnsureGpuMesh(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const Entity& entity,
        TerrainGpuState& state);

    // Record a copy of `data` into `destination` through a staging buffer
    // that is retired once the frame completes.
    bool UploadToBuffer(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* destination,
        UINT64 destinationOffset,
        const void* data,
        UINT64 byteSize,
        D3D12_RESOURCE_STATES steadyState,
        bool destinationIsFresh);

    // Re-upload the vertex rows a brush stroke touched since the last frame.
    bool UploadDirtyRegion(
        ID3D12GraphicsCommandList* commandList,
        const Entity& entity,
        TerrainGpuState& state);

    // Keep a replaced GPU resource alive until every frame that may still
    // reference it has retired (see EntityMeshRenderer::RetireBuffer).
    void RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    void RetireGpuMesh(TerrainGpuState& state);
    void ReleaseExpiredResources();

    static constexpr std::size_t kFramesInFlight = 3;
    std::size_t mFrameSlot = 0;

    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        int FramesRemaining = 0;
    };
    std::vector<RetiredResource> mRetiredResources;

    // Parsed terrain material JSON, keyed by path.  Parsing it per terrain
    // per frame opened and read the file on the render thread every frame.
    // Flushed by MarkTerrainDirty and periodically, so edits to the file
    // still show up.
    std::unordered_map<std::string, TerrainMaterialInfo> mMaterialCache;
    std::uint64_t mFrameCounter = 0;

    // Worker that actually creates the GPU resources; EnsureGpuMesh wraps
    // it so the dirty flag is always cleared on failure.
    bool EnsureGpuMeshImpl(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const Entity& entity,
        TerrainGpuState& state);

    // Load (or reload) the CPU heightmap for a state; false with outError set
    // if the source cannot be read or does not match the component.
    bool EnsureSamplesLoaded(const TerrainComponent& tc, TerrainGpuState& state, std::string& outError);

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
    // Tessellated variant, used for terrains whose material enables it.
    DX12Shader mTessVertexShader;
    DX12Shader mHullShader;
    DX12Shader mDomainShader;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mTessPipelineState;
    DirectX::XMFLOAT3 mCameraPosition{};
    float mTessPixelScale = 1000.0f;

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
