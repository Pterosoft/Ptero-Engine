#pragma once

// VirtualGeometryRenderer
// -----------------------
// Nanite-style virtualized geometry for entity meshes flagged
// MeshComponent::VirtualizedGeometry.
//
// Each such mesh is turned into a DAG of small clusters on a worker thread
// (VirtualGeometryBuilder, cached on disk), and from then on the GPU decides,
// per cluster and per frame, which level of the DAG to draw: the coarsest one
// whose simplification error stays under a pixel. Detail therefore tracks
// screen size continuously - a million-triangle scan costs about as many
// triangles as it covers pixels, at any distance - with no LOD bands to pop
// between and no per-object draw calls.
//
// A frame, driven by DX12SceneRenderer:
//
//   BeginFrame        poll builds, gather instances, (re)upload the geometry
//                     pools when the set of resident meshes changes.
//   CullViews         GPU culling for every view at once - camera, sun shadow
//                     and each shadow-casting point light: instance frustum
//                     test, per-cluster LOD cut, frustum, backface cone and,
//                     for the camera, occlusion against last frame's HZB.
//                     Survivors are binned by material.
//   RenderSunShadow / RenderPointShadowFace
//                     depth-only draws of the shadow views' clusters.
//   RenderGBuffer     one indirect draw per material bin into the G-Buffer,
//                     through GBuffer.hlsl's own pixel shader.
//   RunOcclusionPass  build an HZB from what was just drawn and give every
//                     cluster the first pass thought occluded a second chance
//                     (Nanite's two-pass occlusion); RenderGBuffer(phase 1)
//                     then draws the ones that were visible after all.
//   BuildHzb          HZB of the finished frame, for next frame's first pass.
//   RenderMotionVectors
//                     the camera's clusters into the motion vector target.
//
// Mesh shaders do the rasterisation when the GPU supports them; otherwise a
// vertex shader pulls the same clusters with no index buffer. Either way the
// CPU never learns what was visible, so nothing waits on the GPU.
//
// Entities that cannot be virtualized - still building, too small, a material
// that needs tessellation - are simply left to EntityMeshRenderer,
// which skips exactly the entities GetEntityMask() marks.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "Components.h"
#include "ShadowPageView.h"
#include "VirtualGeometryBuilder.h"
#include "VirtualGeometrySettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class EntityMeshRenderer;

class VirtualGeometryRenderer
{
public:
    // GetEntityMask() bits: which of the ordinary passes an entity is drawn by
    // this renderer instead.
    static constexpr std::uint8_t kDrawnInGBuffer = 1u << 0;  // also its motion vectors
    static constexpr std::uint8_t kDrawnInShadows = 1u << 1;

    bool Initialize();
    void Shutdown();

    VirtualGeometrySettings& GetSettings() { return mSettings; }
    const VirtualGeometrySettings& GetSettings() const { return mSettings; }

    // Whether this GPU can run the mesh shader path. The vertex shader path
    // runs everywhere, so this only decides which of the two is used.
    bool IsMeshShaderSupported() const { return mMeshShaderSupported; }

    struct FrameInputs
    {
        std::vector<Entity>* Entities = nullptr;
        // Model matrices of the previous frame by entity index, for motion
        // vectors and for testing clusters against last frame's HZB.
        const std::unordered_map<std::size_t, DirectX::XMFLOAT4X4>* PreviousTransforms = nullptr;

        DirectX::XMFLOAT3   CameraPosition{};
        float               FovYRadians = 1.0f;
        float               NearPlane = 0.1f;
        // Row-major, not transposed. Culling uses the unjittered matrix so the
        // selected set does not shimmer with the TAA jitter; the G-Buffer and
        // the HZB use the jittered one the depth buffer is actually drawn with.
        DirectX::XMFLOAT4X4 CullViewProjection{};
        DirectX::XMFLOAT4X4 RasterViewProjection{};

        UINT SceneWidth = 0;
        UINT SceneHeight = 0;
        UINT MsaaSampleCount = 1;
        bool WireframeEnabled = false;
        float TextureMipLodBias = 0.0f;
    };

    // Start of the frame, with the command list open and before any shadow or
    // geometry pass. Uploads new geometry, so it may record copies.
    void BeginFrame(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs, EntityMeshRenderer& materials);

    // Per entity index: kDrawnInGBuffer / kDrawnInShadows. Valid from
    // BeginFrame until the next one.
    const std::vector<std::uint8_t>& GetEntityMask() const { return mEntityMask; }

    // True when at least one instance is drawn this frame.
    bool HasInstances() const { return !mInstances.empty(); }

    struct PointShadowLight
    {
        DirectX::XMFLOAT3 Position{};
        float Radius = 1.0f;
    };

    // Cull for the camera and every shadow view. sunViewProjection is
    // row-major, not transposed; pointLights are in the order the point
    // shadow renderer lays out its cubemaps. shadowPages are the pages of the
    // sun's virtual shadow map to render this frame (VirtualShadowMapRenderer),
    // one view each, culled to the page and cut at the page's own texel size.
    void CullViews(
        ID3D12GraphicsCommandList* commandList,
        bool sunShadowEnabled,
        const DirectX::XMFLOAT4X4& sunViewProjection,
        const std::vector<PointShadowLight>& pointLights,
        const std::vector<ShadowPageView>* shadowPages = nullptr);

    // Depth-only draws into a bound shadow depth target. The matrices are
    // pre-transposed, as the shadow renderers hand them out.
    void RenderSunShadow(ID3D12GraphicsCommandList* commandList, const DirectX::XMFLOAT4X4& lightViewProjection);
    // Page pageIndex of the shadowPages given to CullViews, into its viewport of the
    // bound depth target. Depth clamped rather than clipped, like the other page casters.
    void RenderShadowPage(
        ID3D12GraphicsCommandList* commandList,
        std::size_t pageIndex,
        const ShadowPageView& page,
        float slopeScaledDepthBias);
    void RenderPointShadowFace(
        ID3D12GraphicsCommandList* commandList,
        int lightIndex,
        const DirectX::XMFLOAT4X4& faceViewProjection,
        const DirectX::XMFLOAT3& lightPosition,
        float farPlane,
        float slopeScaledDepthBias);

    // G-Buffer draws, with the G-Buffer render targets and scene depth bound.
    // phase 0 is the first pass; phase 1 draws what RunOcclusionPass found.
    void RenderGBuffer(
        ID3D12GraphicsCommandList* commandList,
        EntityMeshRenderer& materials,
        int phase,
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT sampleCount);

    // Between the two G-Buffer phases: HZB from the depth drawn so far, then
    // re-cull the deferred clusters against it. Leaves the depth buffer in the
    // state it found it in (DEPTH_WRITE) but unbinds the render targets, so the
    // caller must rebind them before RenderGBuffer(phase 1). Returns false,
    // having recorded nothing, when occlusion culling is not active this frame.
    bool RunOcclusionPass(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* depthResource,
        D3D12_RESOURCE_STATES& depthState,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);

    // After the geometry pass: HZB of the complete frame for next frame's
    // first pass, and the statistics readback. The depth buffer is returned to
    // the state it was found in.
    void EndFrame(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* depthResource,
        D3D12_RESOURCE_STATES& depthState,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);

    // Into the bound motion vector target (no depth buffer, matching
    // MotionVectorRenderer). Matrices row-major, not transposed.
    void RenderMotionVectors(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT4X4& viewProjection,
        const DirectX::XMFLOAT4X4& previousViewProjection,
        DXGI_FORMAT targetFormat);

    // Virtualized geometry replaced ordinary geometry on screen (a build
    // finished, the feature was toggled), which invalidates temporal history.
    bool ConsumeSceneContentChanged()
    {
        const bool changed = mSceneContentChanged;
        mSceneContentChanged = false;
        return changed;
    }

    // For the editor: one line on how an asset is doing ("Building...",
    // "12,345 clusters, 9 levels", or why it cannot be virtualized).
    std::string DescribeAsset(const Mesh* mesh) const;

    // The finished cluster DAG of an asset, or null while it is queued, building,
    // failed or was never requested. The ray tracer builds its acceleration
    // structures from static cuts through it (RtGlobalIllumination::BuildTlas).
    std::shared_ptr<const VirtualGeometry::BuiltMesh> FindBuiltMesh(const Mesh* mesh) const;

    struct Statistics
    {
        std::uint32_t Instances = 0;
        std::uint32_t ResidentMeshes = 0;
        std::uint32_t ResidentClusters = 0;
        std::uint64_t ResidentTriangles = 0;   // every DAG level
        std::uint64_t PoolBytes = 0;
        // From the GPU, a few frames behind.
        std::uint32_t VisibleClusters = 0;
        std::uint32_t VisibleTriangles = 0;
        std::uint32_t ShadowClusters = 0;
        std::uint32_t OcclusionRecovered = 0;  // drawn by the second pass
        std::uint32_t OverflowFlags = 0;       // a GPU budget ran out
        bool          MeshShaders = false;
        bool          Occlusion = false;
    };
    const Statistics& GetStatistics() const { return mStatistics; }

    const char* GetLastErrorMessage() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    // --- GPU record layouts; mirror VirtualGeometry_Common.hlsli -------------
    struct GpuAsset
    {
        DirectX::XMFLOAT4 BoundsSphere{};
        std::uint32_t     ClusterOffset = 0;
        std::uint32_t     ClusterCount = 0;
        std::uint32_t     Pad[2]{};
    };
    static_assert(sizeof(GpuAsset) == 32);

    struct GpuInstance
    {
        DirectX::XMFLOAT4 World[3]{};
        DirectX::XMFLOAT4 PrevWorld[3]{};
        std::uint32_t     AssetIndex = 0;
        std::uint32_t     BinTableOffset = 0;
        float             MaxScale = 1.0f;
        std::uint32_t     Flags = 0;
    };
    static_assert(sizeof(GpuInstance) == 112);

    struct GpuView
    {
        DirectX::XMFLOAT4 Planes[6]{};
        DirectX::XMFLOAT4 LightSphere{};
        std::uint32_t     Bin = 0;
        std::uint32_t     Flags = 0;
        float             LodTexelSize = 0.0f;   // kViewTexelLod: metres; kViewPointLod: radians
        std::uint32_t     Pad = 0;
    };
    static_assert(sizeof(GpuView) == 128);

    struct alignas(256) CullConstants
    {
        DirectX::XMFLOAT4X4 HzbViewProjection{};
        DirectX::XMFLOAT3   LodCameraPosition{};
        float               LodFactor = 1.0f;
        DirectX::XMFLOAT3   ConeCameraPosition{};
        float               LodZNear = 0.1f;
        std::uint32_t       InstanceCount = 0;
        std::uint32_t       ViewCount = 0;
        std::uint32_t       Phase = 0;
        std::uint32_t       Flags = 0;
        DirectX::XMFLOAT2   HzbSize{};
        std::uint32_t       HzbMipCount = 1;
        std::uint32_t       BinCount = 0;
        std::uint32_t       ChunkCapacity = 0;
        std::uint32_t       CandidateCapacity = 0;
        std::uint32_t       OccludedCapacity = 0;
        std::uint32_t       MaxBins = 0;
        DirectX::XMUINT2    HzbSrcSize{};
        DirectX::XMUINT2    HzbDstSize{};
    };
    static_assert(sizeof(CullConstants) == 256);

    struct alignas(256) DrawConstants
    {
        DirectX::XMFLOAT4X4 ViewProjection{};
        DirectX::XMFLOAT4X4 PreviousViewProjection{};
        DirectX::XMFLOAT3   LightPosition{};
        float               LightFarPlane = 1.0f;
        std::uint32_t       DebugMode = 0;
        std::uint32_t       Pad[3]{};
    };
    static_assert(sizeof(DrawConstants) == 256);

    // --- Budgets ------------------------------------------------------------
    // Sized for a 4K frame of dense virtualized geometry with every shadow
    // view active. Running out drops clusters for that frame (reported in the
    // statistics) rather than corrupting anything.
    static constexpr std::uint32_t kChunkCapacity     = 1u << 17;  // 64 clusters each
    static constexpr std::uint32_t kCandidateCapacity = 1u << 19;
    static constexpr std::uint32_t kOccludedCapacity  = 1u << 18;
    // Bins per phase: material bins for the camera plus one per shadow view.
    // Views are packed into 8 bits of every list entry, hence at most 256: the
    // camera, the sun or up to six point lights, and virtual shadow map pages.
    static constexpr std::uint32_t kMaxBins           = 1024;
    static constexpr std::uint32_t kMaxViews          = 256;
    static constexpr std::uint32_t kMaxHzbMips        = 16;

    static constexpr std::size_t   kFramesInFlight     = 3;
    static constexpr std::uint32_t kCullConstantSlots  = 40;
    // One per shadow page on top of the handful the other passes use.
    static constexpr std::uint32_t kDrawConstantSlots  = 40 + kMaxViews;
    static constexpr std::uint32_t kCounterBytes       = 64;

    // Must match the offsets in VirtualGeometry_Cull.hlsl.
    enum Counter : std::uint32_t
    {
        CounterChunks = 0, CounterCandidates = 4, CounterOccluded = 8,
        CounterMainClusters = 28, CounterMainTriangles = 32, CounterShadowClusters = 36,
        CounterOverflow = 40, CounterRecoveredClusters = 44,
    };
    static constexpr UINT64 kDispatchClusterCull = 0;
    static constexpr UINT64 kDispatchScatter     = 16;
    static constexpr UINT64 kDispatchRecull      = 32;

    // Cull flags; must match VirtualGeometry_Cull.hlsl.
    static constexpr std::uint32_t kCullHzbValid   = 1u;
    static constexpr std::uint32_t kCullMeshShader = 2u;
    // View flags; must match VirtualGeometry_Common.hlsli.
    static constexpr std::uint32_t kViewOcclusion  = 1u;
    static constexpr std::uint32_t kViewCone       = 2u;
    static constexpr std::uint32_t kViewSphere     = 4u;
    static constexpr std::uint32_t kViewTexelLod   = 8u;
    static constexpr std::uint32_t kViewPointLod   = 16u;
    static constexpr std::uint32_t kMaterialBinned = 0xFFFFFFFFu;

    // --- Assets -------------------------------------------------------------
    enum class AssetState { Queued, Building, Ready, Failed };

    struct AssetEntry
    {
        // Strong reference: the map is keyed by address, which is only unique
        // while the asset lives. See EntityMeshRenderer::EntityGpuMesh.
        std::shared_ptr<const Mesh> Owner;
        AssetState State = AssetState::Queued;
        std::future<VirtualGeometry::BuildResult> Job;
        std::shared_ptr<const VirtualGeometry::BuiltMesh> Data;
        std::string Error;
        double BuildSeconds = 0.0;
        bool FromCache = false;
        // Material id and UDIM tile (0 = none) of each slot, in the order of
        // VirtualGeometry::CollectMaterialSlots.
        std::vector<std::uint32_t> SlotMaterialIds;
        std::vector<std::uint32_t> SlotUdimTiles;
        // Built, but last seen with a material the cluster pipelines cannot draw.
        bool MaterialRejected = false;
        std::uint64_t LastUsedFrame = 0;
        // Index in the resident pools, or -1 while not resident.
        int PoolIndex = -1;
    };

    void PollBuilds();
    void StartQueuedBuilds();
    AssetEntry& RequestAsset(const std::shared_ptr<Mesh>& mesh);
    void EvictUnusedAssets();
    bool RebuildPoolsIfNeeded(ID3D12GraphicsCommandList* commandList);

    // --- Resources ------------------------------------------------------------
    bool CreateWorkBuffers();
    bool EnsureHzb(UINT sceneWidth, UINT sceneHeight);
    bool EnsureUploadCapacity(std::size_t instanceCount, std::size_t binTableEntries);
    bool CreateCullPipelines();
    bool CreateCommandSignatures();
    bool CreateRasterRootSignature(float mipLodBias);
    bool CompileRasterShaders();
    bool EnsureRasterPipelines(
        DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat, DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat, UINT sampleCount);
    bool EnsureShadowPipelines(float pointSlopeScaledDepthBias);
    bool EnsureMotionPipeline(DXGI_FORMAT targetFormat);

    struct RasterShaders
    {
        D3D12_SHADER_BYTECODE Front{};   // mesh or vertex shader
        D3D12_SHADER_BYTECODE Pixel{};   // may be empty (depth only)
    };
    bool CreateRasterPipeline(
        const RasterShaders& shaders,
        const D3D12_RASTERIZER_DESC& rasterizer,
        const D3D12_DEPTH_STENCIL_DESC& depthStencil,
        UINT renderTargetCount,
        const DXGI_FORMAT* renderTargetFormats,
        DXGI_FORMAT depthFormat,
        UINT sampleCount,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPipeline,
        const char* what);

    void RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    void RetireExpiredResources();

    // --- Recording helpers ------------------------------------------------------
    void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES target);
    void UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource = nullptr);
    void BindCullRoot(ID3D12GraphicsCommandList* commandList, std::uint32_t constantSlot);
    void BindRasterRoot(ID3D12GraphicsCommandList* commandList, std::uint32_t drawConstantSlot);
    void DrawBins(ID3D12GraphicsCommandList* commandList, std::uint32_t firstSlot, std::uint32_t count);
    void BuildHzbFromDepth(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depthResource,
                           D3D12_RESOURCE_STATES& depthState, D3D12_GPU_DESCRIPTOR_HANDLE depthSrv);
    void BeginRasterReads(ID3D12GraphicsCommandList* commandList);
    CullConstants MakeCullConstants(std::uint32_t phase) const;
    std::uint32_t NextCullSlot();
    std::uint32_t NextDrawSlot();
    D3D12_GPU_VIRTUAL_ADDRESS CullConstantsAddress(std::uint32_t slot) const;
    D3D12_GPU_VIRTUAL_ADDRESS DrawConstantsAddress(std::uint32_t slot) const;
    bool UseMeshShaders() const { return mMeshShaderSupported && mSettings.MeshShaders; }
    bool IsReadyToDraw() const;

    VirtualGeometrySettings mSettings;
    bool mInitialized = false;
    bool mMeshShaderSupported = false;
    bool mSceneContentChanged = false;
    std::string mLastError;

    // Assets and builds.
    std::unordered_map<const Mesh*, AssetEntry> mAssets;
    std::uint64_t mFrameCounter = 0;
    std::size_t mMaxConcurrentBuilds = 2;
    std::size_t mActiveBuilds = 0;
    std::uint64_t mLastPoolRebuildFrame = 0;
    static constexpr std::uint64_t kAssetEvictionFrames = 600;

    // Resident pools, rebuilt whenever the set of resident assets changes.
    std::vector<const Mesh*> mPoolAssets;
    Microsoft::WRL::ComPtr<ID3D12Resource> mClusterPool;
    Microsoft::WRL::ComPtr<ID3D12Resource> mClusterVertexPool;
    Microsoft::WRL::ComPtr<ID3D12Resource> mClusterTrianglePool;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVertexPool;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAssetPool;

    // Per-frame state.
    std::vector<std::uint8_t> mEntityMask;
    std::vector<GpuInstance> mInstances;
    std::vector<std::uint32_t> mBinTable;
    struct MaterialBin
    {
        std::string   MaterialPath;
        std::uint32_t MaterialId = 0;
        std::uint32_t UdimTile = 0;   // 0 = not a UDIM tile
    };
    std::vector<MaterialBin> mMaterialBins;
    std::uint32_t mViewCount = 0;
    std::uint32_t mSunView = 0;          // 0 = none (view 0 is the camera)
    std::uint32_t mFirstPointView = 0;
    std::uint32_t mPointViewCount = 0;
    std::uint32_t mFirstPageView = 0;
    std::uint32_t mPageViewCount = 0;
    std::vector<GpuView> mViewScratch;
    bool mCulledThisFrame = false;
    bool mOcclusionThisFrame = false;
    bool mPhase2Recorded = false;

    FrameInputs mFrame{};
    // Frozen culling camera (FreezeCulling).
    bool mCullingFrozen = false;
    DirectX::XMFLOAT3   mFrozenCameraPosition{};
    DirectX::XMFLOAT4X4 mFrozenCullViewProjection{};
    float mLodFactor = 1.0f;

    // UPLOAD rings, kFramesInFlight copies each.
    std::size_t mFrameSlot = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mInstanceUpload;
    std::byte* mInstanceUploadMapped = nullptr;
    std::size_t mInstanceCapacity = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBinTableUpload;
    std::byte* mBinTableUploadMapped = nullptr;
    std::size_t mBinTableCapacity = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mViewUpload;
    std::byte* mViewUploadMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCullConstantUpload;
    std::byte* mCullConstantMapped = nullptr;
    std::uint32_t mCullSlotCursor = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawConstantUpload;
    std::byte* mDrawConstantMapped = nullptr;
    std::uint32_t mDrawSlotCursor = 0;
    std::uint32_t mGBufferDrawSlot = 0;
    std::uint32_t mCullSlotPhase1 = 0;

    // GPU work buffers.
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounters;
    D3D12_RESOURCE_STATES mCountersState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> mChunks;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCandidates;
    Microsoft::WRL::ComPtr<ID3D12Resource> mOccluded;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBinCounts;
    D3D12_RESOURCE_STATES mBinCountsState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBinRanges;
    D3D12_RESOURCE_STATES mBinRangesState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawArgs;
    D3D12_RESOURCE_STATES mDrawArgsState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDispatchArgs;
    D3D12_RESOURCE_STATES mDispatchArgsState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVisible;
    D3D12_RESOURCE_STATES mVisibleState = D3D12_RESOURCE_STATE_COMMON;
    // Never written after creation, so it is safe to copy from every frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> mZeroUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mStatsReadback;
    std::byte* mStatsReadbackMapped = nullptr;

    // Hierarchical Z.
    Microsoft::WRL::ComPtr<ID3D12Resource> mHzb;
    D3D12_RESOURCE_STATES mHzbState = D3D12_RESOURCE_STATE_COMMON;
    UINT mHzbWidth = 0;
    UINT mHzbHeight = 0;
    UINT mHzbMipCount = 0;
    UINT mHzbSourceWidth = 0;
    UINT mHzbSourceHeight = 0;
    bool mHzbValid = false;
    DirectX::XMFLOAT4X4 mHzbViewProjection{};
    // Allocated once from the shared heap, which never frees; a resize
    // rewrites them in place.
    bool mHzbDescriptorsAllocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE mHzbSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mHzbSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mHzbUavCpu[kMaxHzbMips]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mHzbUavGpu[kMaxHzbMips]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthSrvForCull{};

    // Pipelines.
    DX12Shader mCullShaders[9];
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mCullRootSignature;
    enum CullPass
    {
        PassInstanceCull, PassChunkArgs, PassClusterCull, PassOcclusionArgs, PassOcclusionRecull,
        PassBuildBins, PassScatter, PassHzbFromDepth, PassHzbDownsample, PassCount
    };
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mCullPipelines[PassCount];
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDispatchSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDispatchMeshSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDrawSignature;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRasterRootSignature;
    float mRasterMipLodBias = 0.0f;
    DX12Shader mMeshShader;
    DX12Shader mMeshMotionShader;
    DX12Shader mVertexShader;
    DX12Shader mVertexMotionShader;
    DX12Shader mGBufferPixelShader;
    DX12Shader mDebugPixelShader;
    DX12Shader mPointShadowPixelShader;
    DX12Shader mMotionPixelShader;
    bool mRasterShadersCompiled = false;
    bool mMeshShadersCompiled = false;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGBufferPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDebugPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSunShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPointShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPagePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mMotionPipeline;

    // What the pipelines above were built for; any change rebuilds them.
    struct RasterKey
    {
        DXGI_FORMAT Albedo = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT Normal = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT Material = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT Depth = DXGI_FORMAT_UNKNOWN;
        UINT SampleCount = 0;
        bool Wireframe = false;
        bool MeshShaders = false;
        bool operator==(const RasterKey&) const = default;
    };
    RasterKey mGBufferKey{};
    bool mShadowPipelinesMeshShaders = false;
    float mPointShadowSlopeBias = -1.0f;
    float mShadowPageSlopeBias = -1.0f;
    bool mMotionPipelineMeshShaders = false;
    DXGI_FORMAT mMotionTargetFormat = DXGI_FORMAT_UNKNOWN;

    // Released a few frames after they are replaced; see
    // EntityMeshRenderer::RetireBuffer for why nothing is freed in place.
    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        int FramesRemaining = 0;
    };
    std::vector<RetiredResource> mRetired;

    Statistics mStatistics;
};
