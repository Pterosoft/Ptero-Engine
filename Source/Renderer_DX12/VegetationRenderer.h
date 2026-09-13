#pragma once

// VegetationRenderer
// ------------------
// Owns everything between a VegetationAreaComponent and pixels on screen:
//
//   1. Scatter.  Areas are re-scattered on a worker thread whenever their
//      rules, their transform or the surface underneath them changes.  The
//      result is a flat instance buffer per area, grouped by layer.
//   2. Upload.  Finished scatters are uploaded once into a DEFAULT-heap
//      structured buffer and then left alone; nothing per-instance is touched
//      per frame on the CPU.
//   3. Cull.  A compute pass frustum-culls, picks a LOD and writes compacted
//      visible-index lists plus DrawIndexedInstancedIndirect arguments.  The
//      CPU never learns the visible count, so there is no readback stall.
//   4. Draw.  One ExecuteIndirect per (layer, LOD) into the G-Buffer, and the
//      same again depth-only for shadows.
//   5. Interaction.  A camera-centred top-down map that grass bends away from,
//      maintained by its own small compute pass.
//
// Sits alongside TerrainRenderer and WaterRenderer in the geometry pass and
// follows the same conventions: DEFAULT-heap resources with upload staging,
// descriptors from the shared engine heap, and lazily created PSOs that are
// invalidated when a render-target format or MSAA count changes.

#include "Components.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "GeometryRaycaster.h"
#include "TerrainRenderer.h"
#include "TextureManager.h"
#include "VegetationScatter.h"
#include "WindSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
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

// Something in the world that presses vegetation aside this frame.  Submitted
// per frame by whatever owns it (the player controller, physics props, the
// editor camera in test mode); the renderer keeps no persistent list, so a
// caller that stops submitting simply stops affecting the grass.
struct VegetationInteractor
{
    DirectX::XMFLOAT3 Position{};
    float             Radius = 0.5f;
    DirectX::XMFLOAT3 Velocity{};
    // Scales how hard this interactor pushes; 1 is a walking character.
    float             Strength = 1.0f;
};

class VegetationRenderer
{
public:
    // Resolves a Data-relative mesh path to a loaded asset.  Injected rather
    // than reached for directly because the AssetManager instance lives in
    // DX12RendererAPI, and the renderer has no business knowing about it.
    using MeshResolver = std::function<std::shared_ptr<Mesh>(const std::string&)>;

    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Shutdown();

    void SetEntities(std::vector<Entity>* entities);
    void SetMeshResolver(MeshResolver resolver) { mMeshResolver = std::move(resolver); }

    // Terrain is the primary snap surface, and its revision counter tells us
    // when a brush stroke has invalidated a scatter.
    void SetTerrainRenderer(TerrainRenderer* terrainRenderer) { mTerrainRenderer = terrainRenderer; }

    void SetWindSettings(const WindSettings* wind) { mWindSettings = wind; }

    // Poll worker threads, start any scatter that has gone stale, and upload
    // finished results.  Call once per frame before the cull dispatch.
    void Update(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT3&   cameraPosition,
        float                      deltaSeconds);

    // Register an interactor for this frame.  Cleared automatically after the
    // interaction dispatch consumes it.
    void SubmitInteractor(const VegetationInteractor& interactor);

    // Update the interaction map.  Must run before the cull and draw passes.
    void DispatchInteraction(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT3&   cameraPosition,
        float                      deltaSeconds);

    // Frustum cull + LOD select.  viewProjection must be the non-jittered
    // camera matrix so culling does not shimmer with the TAA jitter.
    void DispatchCull(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMMATRIX&   viewProjection,
        const DirectX::XMFLOAT3&   cameraPosition);

    // G-Buffer geometry pass.  The G-Buffer render targets must already be
    // bound, exactly as for EntityMeshRenderer::Render.
    void Render(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMMATRIX&   viewProjection,
        const DirectX::XMFLOAT3&   cameraPosition,
        DXGI_FORMAT                albedoFormat,
        DXGI_FORMAT                normalFormat,
        DXGI_FORMAT                materialFormat,
        DXGI_FORMAT                depthFormat,
        UINT                       msaaSampleCount = 1);

    // Alpha-tested depth-only pass for the sun and point-light shadow maps.
    // lightViewProjection must be column-major (pre-transposed).
    void RenderShadowDepth(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT4X4& lightViewProjection,
        DXGI_FORMAT                depthFormat);

    // Motion vectors for the bent geometry.  Without this, TAA and DLSS
    // reproject foliage using camera motion alone and the canopy smears
    // whenever the wind blows, so this is not an optional refinement.
    // The motion vector render target must already be bound.
    void RenderMotionVectors(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMMATRIX&   viewProjection,
        const DirectX::XMMATRIX&   previousViewProjection,
        const DirectX::XMFLOAT3&   cameraPosition,
        DXGI_FORMAT                targetFormat,
        DXGI_FORMAT                depthFormat);

    // Force a re-scatter of one area (the editor's Regenerate button), or of
    // every area (after a level load).
    void RequestRegenerate(std::size_t entityIndex);
    void RequestRegenerateAll();

    // Editor readouts.  Returns false when the entity has no scatter yet.
    bool GetAreaStats(
        std::size_t                entityIndex,
        std::uint32_t&             outInstanceCount,
        VegetationScatterStats&    outStats,
        bool&                      outScatterInProgress) const;

    // Instances generated across every area.  The count that actually survives
    // culling lives only on the GPU: reading it back would stall the frame,
    // and it changes every time the camera moves, so it is not reported.
    std::uint32_t GetTotalInstanceCount() const { return mTotalInstanceCount; }

    // One mesh plus every world transform it should appear at in the ray
    // tracing acceleration structure.
    struct RayTracingBatch
    {
        const Mesh* MeshAsset = nullptr;
        std::string MaterialPath;
        std::vector<DirectX::XMFLOAT4X4> Transforms;
    };

    // Gather the instances that should enter the TLAS.
    //
    // Only layers with ContributeToRayTracing set are considered, and only
    // within maxDistance of the camera.  Both filters exist for the same
    // reason: alpha-tested foliage forces any-hit shader evaluation on every
    // ray that touches it, so a field of grass in the acceleration structure
    // costs far more than it returns.  The intended use is a handful of hero
    // trees near the camera, with everything else lit by the probe grid.
    void CollectRayTracingBatches(
        const DirectX::XMFLOAT3&      cameraPosition,
        float                         maxDistance,
        std::vector<RayTracingBatch>& outBatches) const;

    // Whether any layer in the scene asked to be ray traced, so callers can
    // skip the gather entirely in the common case.
    bool HasRayTracedLayers() const { return mHasRayTracedLayers; }

    void SetWireframeEnabled(bool enabled)
    {
        if (mWireframeEnabled != enabled)
        {
            mWireframeEnabled = enabled;
            mPipelineReady = false;
        }
    }

    bool IsWireframeEnabled() const { return mWireframeEnabled; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

    // Hard limits, shared with the cull shader's buffer sizing.
    static constexpr std::uint32_t kMaxLodsPerLayer      = 4;

    // The shadow, colour and motion vector passes are all recorded into one
    // command list, so they cannot share constant buffer slots: the GPU only
    // ever sees whatever the CPU wrote last, and an earlier pass would end up
    // drawing with a later pass's matrices.  Each gets its own region, and
    // within a pass each LOD gets its own slot for the same reason.
    static constexpr std::uint32_t kPassColour = 0;
    static constexpr std::uint32_t kPassShadow = 1;
    static constexpr std::uint32_t kPassMotion = 2;
    static constexpr std::uint32_t kPassCount  = 3;
    static constexpr std::uint32_t kMaxInteractors       = 64;
    static constexpr std::uint32_t kInteractionMapSize   = 512;
    // Side length in metres of the region the interaction map covers.  Large
    // enough that grass reacts well before it is underfoot, small enough that
    // a 512 map still has roughly 8 cm texels.
    static constexpr float         kInteractionWorldSize = 48.0f;

private:
    // GPU layout of one instance, mirroring VegetationInstance in
    // Vegetation_Common.hlsli.  Exactly 32 bytes.
    struct GpuInstance
    {
        DirectX::XMFLOAT3 Position;
        float             RotationZ;
        DirectX::XMFLOAT3 UpAxis;
        float             Scale;
    };
    static_assert(sizeof(GpuInstance) == 32);

    struct alignas(256) PassConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT3   CameraPosition; float Time;
        DirectX::XMFLOAT4   WindParams0;
        DirectX::XMFLOAT4   WindParams1;
        // .w carries the previous frame's time, which the motion vector pass
        // needs to evaluate the bend one frame back.
        DirectX::XMFLOAT4   InteractionParams;
        DirectX::XMFLOAT4X4 PrevViewProj;
        std::byte           Padding[48]{};
    };
    static_assert(sizeof(PassConstants) == 256);

    struct alignas(256) LayerConstants
    {
        DirectX::XMFLOAT4 BaseColorTint{ 1.f, 1.f, 1.f, 1.f };

        float AlphaCutoff     = 0.5f;
        float RoughnessFactor = 1.0f;
        float MetallicFactor  = 0.0f;
        float NormalScale     = 1.0f;

        float AoStrength           = 1.0f;
        float Translucency         = 0.0f;
        float WindInfluence        = 1.0f;
        float Stiffness            = 1.0f;

        float         FlutterAmount        = 1.0f;
        float         InteractionInfluence = 1.0f;
        int           BendModel            = 1;
        std::uint32_t InstanceOffset       = 0;

        int HasNormalMap    = 0;
        int HasMetallicMap  = 0;
        int HasRoughnessMap = 0;
        int HasAoMap        = 0;

        int   HasPackedMaterialMap = 0;
        // Card dimensions in mesh-local units, only read by the billboard pass.
        float BillboardHalfWidth   = 1.0f;
        float BillboardHeight      = 2.0f;
        float _Pad0                = 0.0f;

        std::byte Padding[144]{};
    };
    static_assert(sizeof(LayerConstants) == 256);

    struct alignas(256) CullConstants
    {
        DirectX::XMFLOAT4 FrustumPlanes[6];

        DirectX::XMFLOAT3 CameraPosition; float CullDistance;

        std::uint32_t InstanceFirst = 0;
        std::uint32_t InstanceCount = 0;
        std::uint32_t LodCount      = 1;
        float         FadeFraction  = 0.15f;

        DirectX::XMFLOAT4 LodDistances{};
        DirectX::XMUINT4  LodOutputBase{};
        DirectX::XMUINT4  LodArgOffset{};

        float  BoundingRadius = 1.0f;
        float  _Pad0[3]       = {};

        std::byte Padding[64]{};
    };
    static_assert(sizeof(CullConstants) == 256);

    struct alignas(256) InteractionConstants
    {
        DirectX::XMFLOAT2 Centre{};
        DirectX::XMFLOAT2 PreviousCentre{};

        float         WorldSize       = kInteractionWorldSize;
        float         Resolution      = static_cast<float>(kInteractionMapSize);
        float         DeltaTime       = 0.0f;
        std::uint32_t InteractorCount = 0;

        float RecoverySeconds = 1.5f;
        float MaxPush         = 0.6f;
        float _Pad0[2]        = {};

        std::byte Padding[208]{};
    };
    static_assert(sizeof(InteractionConstants) == 256);

    // GPU buffers for one LOD of one mesh.
    struct LodBuffers
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUpload;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        std::uint32_t IndexCount = 0;
    };

    // A mesh asset uploaded once and shared by every layer that references the
    // same path, so ten areas planting the same tree cost one upload.
    struct SharedMesh
    {
        std::shared_ptr<Mesh> Asset;
        std::vector<LodBuffers> Lods;
        // Local-space bounding radius, used by the cull pass.
        float BoundingRadius = 1.0f;
        // Local-space extents used to size the billboard card so it covers the
        // same silhouette as the mesh it stands in for.
        float BillboardHalfWidth = 1.0f;
        float BillboardHeight    = 2.0f;
        bool  Ready = false;
    };

    // Resolved material for one layer: texture handles plus the scalar factors
    // the layer constant buffer needs.
    struct LayerMaterial
    {
        std::shared_ptr<GpuTexture> BaseColor;
        std::shared_ptr<GpuTexture> Normal;
        std::shared_ptr<GpuTexture> Metallic;
        std::shared_ptr<GpuTexture> Roughness;
        std::shared_ptr<GpuTexture> Ao;

        DirectX::XMFLOAT4 BaseColorTint{ 1.f, 1.f, 1.f, 1.f };
        float AlphaCutoff     = 0.5f;
        float MetallicFactor  = 0.0f;
        float RoughnessFactor = 1.0f;
        float NormalScale     = 1.0f;
        float AoStrength      = 1.0f;
        bool  HasPackedMaterialMap = false;
        bool  Resolved = false;
    };

    // Where one layer's instances live inside its area's buffer, and where its
    // cull output goes inside the shared visible-index buffer.
    struct LayerDrawRange
    {
        std::uint32_t InstanceFirst = 0;
        std::uint32_t InstanceCount = 0;
        std::uint32_t LodCount      = 1;
        // Index of this layer's first draw-argument slot.
        std::uint32_t ArgSlot       = 0;
        // Base offset into the visible-index buffer for LOD 0; each LOD's
        // slice is InstanceCount entries long.
        std::uint32_t VisibleBase   = 0;
        // When set, the final LOD in LodCount is a camera-facing card rather
        // than a mesh.
        bool          HasBillboard  = false;
        std::string   MeshPath;
        std::string   MaterialPath;
        std::string   BillboardTexturePath;
    };

    // Everything the renderer tracks for one VegetationAreaComponent.
    struct AreaState
    {
        // --- Scatter ---------------------------------------------------------
        std::future<VegetationScatterResult> ScatterJob;
        bool                   ScatterInProgress = false;
        VegetationScatterStats Stats{};

        // Hash of the inputs the last scatter was generated from; a mismatch
        // is what schedules a regeneration.
        std::uint64_t RuleHash        = 0;
        std::uint64_t TerrainRevision = 0;
        bool          ForceRegenerate = true;

        // --- GPU -------------------------------------------------------------
        // Bound as a root SRV, so no descriptor is needed per area.
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> InstanceUpload;
        std::uint32_t InstanceCapacity = 0;
        std::uint32_t InstanceCount    = 0;

        std::vector<LayerDrawRange> LayerRanges;

        // Pending upload waiting for a command list.
        std::vector<GpuInstance> PendingUpload;
        bool                     HasPendingUpload = false;

        // CPU copy of the instances, retained only when some layer in this
        // area is ray traced.  Building TLAS instance descriptors needs the
        // transforms on the CPU, but keeping them for a grass field would cost
        // tens of megabytes for data nothing reads, so it is opt-in.
        std::vector<GpuInstance> CpuInstances;
    };

    // --- Scatter plumbing ----------------------------------------------------
    void       PollScatterJobs();
    void       ScheduleStaleScatters();
    void       StartScatter(std::size_t entityIndex, AreaState& state, const Entity& entity);
    static std::uint64_t ComputeRuleHash(const VegetationAreaComponent& area, const TransformComponent& transform);
    std::vector<VegetationExclusionVolume> GatherExclusionVolumes() const;

    // --- Resource creation ---------------------------------------------------
    bool EnsureInstanceBuffer(AreaState& state, std::uint32_t instanceCount);
    void UploadPendingInstances(ID3D12GraphicsCommandList* commandList, AreaState& state);
    bool EnsureSharedMesh(ID3D12GraphicsCommandList* commandList, const std::string& meshPath);
    const LayerMaterial& ResolveLayerMaterial(const std::string& materialPath);
    bool EnsureCullResources(std::uint32_t totalInstances, std::uint32_t totalDrawSlots);
    bool EnsureConstantBuffers(std::size_t layerDrawCount);
    bool EnsureInteractionResources();
    bool CreateFallbackTexture(ID3D12GraphicsCommandList* commandList);

    // --- Pipelines -----------------------------------------------------------
    bool CreateGraphicsPipeline(
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT        msaaSampleCount);
    bool CreateShadowPipeline(DXGI_FORMAT depthFormat);
    bool CreateMotionPipeline(DXGI_FORMAT targetFormat, DXGI_FORMAT depthFormat);
    bool CreateBillboardPipeline(
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT        msaaSampleCount);
    bool CreateBillboardQuad(ID3D12GraphicsCommandList* commandList);
    bool CreateCullPipeline();
    bool CreateInteractionPipeline();
    bool CreateCommandSignature();

    // Recompute LayerRanges and the shared buffer offsets across every area.
    void RebuildDrawLayout();

    // Fill the CPU-side draw-argument template for every layer/LOD, so the
    // per-frame copy into the indirect buffer only has to reset the counts.
    void RefreshIndirectArgTemplate();

    void FillPassConstants(
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        PassConstants&           outConstants) const;

    // Fill the per-layer constants shared by every vegetation pass.  Factored
    // out because the shadow pass runs before the colour pass in the frame, so
    // it cannot simply reuse whatever the colour pass wrote.
    static void FillLayerConstants(
        const VegetationLayer& layer,
        const LayerMaterial&   material,
        const SharedMesh&      mesh,
        LayerConstants&        outConstants);

    // Slot of the per-layer constants for one (pass, layer, LOD) triple.
    std::size_t LayerCbSlot(std::uint32_t pass, std::size_t drawIndex, std::uint32_t lod) const
    {
        return ((static_cast<std::size_t>(pass) * mLayerCbStride) + drawIndex) * kMaxLodsPerLayer + lod;
    }

    std::vector<Entity>* mEntities = nullptr;
    TerrainRenderer*     mTerrainRenderer = nullptr;
    const WindSettings*  mWindSettings = nullptr;
    MeshResolver         mMeshResolver;

    std::unordered_map<std::size_t, AreaState>   mAreas;
    std::unordered_map<std::string, SharedMesh>  mSharedMeshes;
    std::unordered_map<std::string, LayerMaterial> mMaterials;

    // Flattened list of every layer that has instances, in the order their
    // draw arguments appear in the indirect buffer.
    struct FlatLayer
    {
        std::size_t   EntityIndex = 0;
        std::uint32_t LayerIndex  = 0;
    };
    std::vector<FlatLayer> mFlatLayers;

    // --- Cull / indirect draw -------------------------------------------------
    // Both buffers are bound as root descriptors (UAV during the cull pass,
    // SRV during the draw), so neither needs a heap descriptor.
    Microsoft::WRL::ComPtr<ID3D12Resource> mVisibleIndexBuffer;
    std::uint32_t mVisibleIndexCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgReset;   // UPLOAD heap template
    void*                                  mIndirectArgResetPtr = nullptr;
    std::uint32_t mIndirectArgSlotCapacity = 0;
    std::uint32_t mIndirectArgSlotCount    = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mCommandSignature;

    // --- Interaction map -------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mInteractionMap[2];
    D3D12_CPU_DESCRIPTOR_HANDLE mInteractionSrvCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mInteractionSrvGpu[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mInteractionUavCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mInteractionUavGpu[2]{};
    D3D12_RESOURCE_STATES       mInteractionState[2] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
    int  mInteractionWriteIndex = 0;
    bool mInteractionCleared    = false;
    DirectX::XMFLOAT2 mInteractionCentre{ 0.0f, 0.0f };
    DirectX::XMFLOAT2 mInteractionPreviousCentre{ 0.0f, 0.0f };

    // Rewritten every frame and read straight out of UPLOAD memory as root
    // SRVs; at kMaxInteractors entries a staging copy would cost more than the
    // read it saves.
    Microsoft::WRL::ComPtr<ID3D12Resource> mInteractorBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mInteractorVelocityBuffer;
    void* mInteractorBufferPtr = nullptr;
    void* mInteractorVelocityBufferPtr = nullptr;
    std::vector<VegetationInteractor> mPendingInteractors;

    // --- Constant buffers -------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mPassCb;
    PassConstants* mMappedPassCb = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mLayerCb;
    LayerConstants* mMappedLayerCb = nullptr;
    std::size_t     mLayerCbCapacity = 0;
    // Layers per pass region; see LayerCbSlot.
    std::size_t     mLayerCbStride = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mCullCb;
    CullConstants*  mMappedCullCb = nullptr;
    std::size_t     mCullCbCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mInteractionCb;
    InteractionConstants* mMappedInteractionCb = nullptr;

    // --- Pipelines ----------------------------------------------------------------
    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    DX12Shader mShadowVertexShader;
    DX12Shader mShadowPixelShader;
    DX12Shader mMotionVertexShader;
    DX12Shader mMotionPixelShader;
    DX12Shader mBillboardVertexShader;
    DX12Shader mBillboardPixelShader;
    DX12Shader mCullShader;
    DX12Shader mInteractionShader;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGraphicsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGraphicsPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mMotionPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBillboardPipelineState;

    // A single unit card shared by every billboard draw; only the per-instance
    // transform and the texture differ.
    Microsoft::WRL::ComPtr<ID3D12Resource> mBillboardVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBillboardVertexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBillboardIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBillboardIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW mBillboardVertexView{};
    D3D12_INDEX_BUFFER_VIEW  mBillboardIndexView{};
    static constexpr std::uint32_t kBillboardIndexCount = 6;

    // Billboard cards, cached by texture path.
    std::unordered_map<std::string, std::shared_ptr<GpuTexture>> mBillboardTextures;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mCullRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mCullPipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mInteractionRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mInteractionPipelineState;

    TextureManager mTextureManager;
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackTextureResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackUploadBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE mFallbackCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mFallbackGpuHandle{};

    bool        mInitialized      = false;
    bool        mPipelineReady    = false;
    bool        mShadowPipelineReady = false;
    bool        mMotionPipelineReady = false;
    bool        mBillboardPipelineReady = false;
    DXGI_FORMAT mMotionTargetFormat  = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mMotionDepthFormat   = DXGI_FORMAT_UNKNOWN;
    // Time the previous frame's bend was evaluated at, so the motion vector
    // pass can reproduce it exactly.
    float       mPreviousTimeSeconds = 0.0f;
    bool        mLayoutDirty      = true;
    DXGI_FORMAT mAlbedoFormat     = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mNormalFormat     = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mMaterialFormat   = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthFormat      = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mShadowDepthFormat = DXGI_FORMAT_UNKNOWN;
    UINT        mMsaaSampleCount  = 1;
    bool        mWireframeEnabled = false;

    float         mTimeSeconds         = 0.0f;
    std::uint32_t mTotalInstanceCount  = 0;
    bool          mHasRayTracedLayers  = false;

    std::string mLastError;
};
