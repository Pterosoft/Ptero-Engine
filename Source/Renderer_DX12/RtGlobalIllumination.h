#pragma once

// RtGlobalIllumination.h
// Custom Ray Traced Global Illumination renderer for Ptero-Engine.
//
// Four compute passes using DXR inline ray-tracing (cs_6_5 RayQuery):
//   Pass 0 - RayGen    : fire GI rays against the TLAS, write initial reservoirs + radiance
//   Pass 1 - Temporal  : ReSTIR temporal resampling against previous frame
//   Pass 2 - Spatial   : ReSTIR spatial resampling from screen neighbours
//   Pass 3 - Composite : temporal accumulation + final colour output

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "RtGISettings.h"
#include "DeferredLightingPass.h"
#include "Components.h"         // Entity, Mesh
#include "NrdDenoiser.h"        // NVIDIA NRD RELAX_DIFFUSE wrapper
#include "TextureManager.h"
#include "VegetationRenderer.h" // RayTracingBatch

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include <array>
#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// Per-vertex data uploaded into the global geometry SRV buffer.
// Stride = 24 bytes.
// -----------------------------------------------------------------------
struct GpuPackedVertex
{
    float px, py, pz;  // object-space position
    float nx, ny, nz;  // object-space normal
    float u, v;        // object-space UV0
};

// Per-instance lookup record that maps a TLAS instance index to its slice
// of the global vertex and index buffers.  Indexed by CommittedInstanceIndex().
struct GpuInstanceInfo
{
    uint32_t vertexOffset;  // first vertex index inside mVertexBuffer
    uint32_t indexOffset;   // first index inside mIndexBuffer
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t materialRangeOffset;
    uint32_t materialRangeCount;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct GpuMaterialRange
{
    uint32_t startPrimitive;
    uint32_t primitiveCount;
    float    baseColorR;
    float    baseColorG;
    float    baseColorB;
    float    baseColorA;
    float    opacityFactor;
    float    alphaCutoff;
    uint32_t baseColorTextureIndex;
    uint32_t opacityTextureIndex;
    uint32_t flags;
    float    _pad0;
};

struct RtMaterialSlotInfo
{
    std::string BaseColorPath;
    std::string OpacityPath;
    float       BaseColorTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float       OpacityFactor    = 1.0f;
    float       AlphaCutoff      = 0.5f;
    bool        UseAlphaCutout   = false;
    bool        DoubleSided      = false;
};

// -----------------------------------------------------------------------
// GPU constant buffer shared across all four RTGI shaders (256 bytes).
// -----------------------------------------------------------------------
struct alignas(256) RtGIConstants
{
    uint32_t FrameWidth;
    uint32_t FrameHeight;
    uint32_t FrameIndex;
    uint32_t RaysPerPixel;

    uint32_t MaxHistoryLength;
    uint32_t SpatialSamples;
    float    SpatialRadius;
    float    DepthThreshold;

    float    NormalThreshold;
    float    RadianceClamp;
    float    AccumulationBlend;
    int      MaxBounces;

    int      DebugView;
    int      TemporalReuseEnabled;
    int      SpatialReuseEnabled;
    int      NextEventEstimation;

    float    NrdSharpenAmount;
    float    ColorLeakIntensity;
    float    SpecularRoughnessThreshold;
    float    _pad0;

    float    ViewProjInv[16];   // row-major inverse view-projection (world-pos reconstruction)
    float    CurrViewProj[16];  // row-major current non-jittered view-projection (NRD motion vectors)
    float    PrevViewProj[16];  // row-major previous-frame view-projection (temporal reprojection)
    // Current non-jittered world-to-view matrix in the same row-major convention.
    // Used by the NRD prepare pass to compute true camera-space viewZ.
    float    WorldToView[16];

    float    CameraPos[3];
    float    _pad1;

    float    SunDirX, SunDirY, SunDirZ;    // world-space direction FROM sun TOWARD scene (Z-up)
    float    _pad2;

    float    SunColorR, SunColorG, SunColorB;  // scaled Hosek-Wilkie sun colour
    float    _pad3;

    float    SkyColorR, SkyColorG, SkyColorB;  // scaled Hosek-Wilkie sky colour
    float    _pad4;

    int      NumPointLights;
    float    _pad5[3]{};
    DeferredLightingPass::PointLightGpu PointLights[DeferredLightingPass::kMaxPointLights]{};
};

// -----------------------------------------------------------------------
// One element in the per-pixel GI reservoir structured buffer.
// -----------------------------------------------------------------------
struct RtGIPackedReservoir
{
    float    posX, posY, posZ;
    uint32_t packedNormal;      // oct-encoded 2x snorm16

    float    radR, radG, radB;
    float    weight;            // W = w_sum / (M * p_hat)

    uint32_t M;                 // candidate count
    uint32_t age;               // temporal age of the chosen sample
    uint32_t _pad0, _pad1;
};

class RtGlobalIllumination
{
public:
    static constexpr uint32_t kMaxRtMaterialTextures = 32;

    RtGlobalIllumination()  = default;
    ~RtGlobalIllumination() { Shutdown(); }

    // Initialize (or re-initialize) for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Resize resolution-dependent resources (no-op if size unchanged).
    bool EnsureSize(UINT width, UINT height);

    // Build / update the TLAS from the current entity list.  Call once per frame before Dispatch.
    // Build / update the TLAS from the current entity list, plus any vegetation
    // instances whose layer opted into ray tracing (see
    // VegetationRenderer::CollectRayTracingBatches).  Vegetation is passed in
    // rather than pulled from the entity list because its instances are
    // procedural and have no entity of their own.
    void BuildTlas(
        ID3D12GraphicsCommandList4* cmdList,
        const std::vector<Entity>&  entities,
        const std::vector<VegetationRenderer::RayTracingBatch>* vegetationBatches = nullptr);

    // Release all GPU resources.
    void Shutdown();

    // Dispatch all RTGI compute passes for this frame.
    // When settings.UseNrdDenoiser is true the custom temporal/spatial/composite
    // passes are replaced by NVIDIA NRD RELAX_DIFFUSE denoising.
    //
    // worldToViewMatrix / viewToClipMatrix: column-major, NON-jittered, for NRD.
    // prevWorldToViewMatrix / prevViewToClipMatrix: same for the previous frame.
    void Dispatch(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferMaterialSrv,
        const RtGISettings&          settings,
        const float                  viewProjInv[16],
        const float                  currViewProj[16],
        const float                  prevViewProj[16],
        const float                  cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t                     numPointLights,
        // NRD-specific matrices (non-jittered, column-major XMFLOAT4X4 layout).
        // Pass nullptr to disable NRD even when settings.UseNrdDenoiser is true.
        const float worldToViewMatrix[16]     = nullptr,
        const float viewToClipMatrix[16]      = nullptr,
        const float prevWorldToViewMatrix[16] = nullptr,
        const float prevViewToClipMatrix[16]  = nullptr,
        const float cameraJitter[2]           = nullptr,
        const float prevCameraJitter[2]       = nullptr,
        float       timeDeltaMs               = 0.0f);

    // Shader-visible SRV GPU handle for the final accumulated GI output (RGBA16F).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv()   const { return mOutputSrvGpu; }

    // Shader-visible SRV GPU handle for the specular reflections output (RGBA16F).
    D3D12_GPU_DESCRIPTOR_HANDLE GetSpecularSrv() const { return mSpecularSrvGpu; }

    // Shader-visible SRV GPU handle for the TLAS built by BuildTlas().
    // Used by the RTAO pass so it can share the same acceleration structure without rebuilding it.
    D3D12_GPU_DESCRIPTOR_HANDLE GetTlasSrv()        const { return mTlasSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetVertexSrv()      const { return mVertexSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetIndexSrv()       const { return mIndexSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetInstanceInfoSrv() const { return mInstanceInfoSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetMaterialRangeSrv() const { return mMaterialRangeSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetBaseTextureTableSrv() const { return mBaseTextureTableGpu; }
    bool                        IsTlasReady()  const { return mTlasReady; }

    bool        IsInitialized() const { return mIsInitialized; }
    bool        HasInitFailed() const { return mInitFailed; }
    const char* GetLastError()  const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignature();
    bool CreateSpecularRootSignature();
    bool CreatePipelines();
    bool CreateResolutionBuffers();
    bool CreateDescriptors();
    void RefreshMaterialTextureDescriptors();
    // Create/re-create NRD resources and (re-)initialize NrdDenoiser.
    bool InitNrd();

    void UploadConstants(
        const RtGISettings& s,
        const float viewProjInv[16],
        const float currViewProj[16],
        const float prevViewProj[16],
        const float worldToView[16],
        const float cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t numPointLights);

    // ---- GPU helpers shared with TLAS build ----
    static bool CreateGpuBuffer(ID3D12Device* dev, UINT64 size,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>& out);
    static bool CreateUploadBuf(ID3D12Device* dev, UINT64 size,
        ComPtr<ID3D12Resource>& out);

    bool     mIsInitialized         = false;
    bool     mInitFailed            = false;
    UINT     mWidth                 = 0;
    UINT     mHeight                = 0;
    uint32_t mFrameIndex            = 0;
    std::string mLastError;

    ComPtr<ID3D12RootSignature>  mRootSignature;

    // One PSO per compute pass.
    ComPtr<ID3D12PipelineState>  mPSO_RayGen;
    ComPtr<ID3D12PipelineState>  mPSO_Temporal;
    ComPtr<ID3D12PipelineState>  mPSO_Spatial;
    ComPtr<ID3D12PipelineState>  mPSO_Composite;
    // NRD path: prepare-inputs and final-composite shaders.
    ComPtr<ID3D12PipelineState>  mPSO_NrdPrepare;
    ComPtr<ID3D12PipelineState>  mPSO_NrdComposite;
    // Extended root signature for NrdPrepare (4 UAV outputs at u0-u3).
    ComPtr<ID3D12RootSignature>  mRootSignatureNrd;

    // Specular reflection pass (GGX ray-traced, separate PSO + root sig).
    ComPtr<ID3D12PipelineState>  mPSO_Specular;
    ComPtr<ID3D12RootSignature>  mRootSignatureSpecular;

    // Persistently-mapped 256-byte upload constant buffer.
    // One RtGIConstants block per frame in flight, in a single upload buffer.
    //
    // A single block was being overwritten by the CPU while the GPU was still
    // executing an earlier frame's RTGI passes, so the shaders could read a
    // mixture of two frames' camera matrices. Standing still that is harmless -
    // consecutive frames hold identical matrices - but under camera motion the
    // inconsistent ViewProjInv and CameraPos put reconstructed world positions
    // off the surface, and the GI rays then either self-intersect or escape.
    // That showed up as black and bright patches that flickered only while the
    // camera moved, and only once the CPU was fast enough to run ahead of the
    // GPU; before that, CPU stalls had been hiding it by acting as a
    // synchroniser.
    ComPtr<ID3D12Resource>  mConstantBuffer;
    uint8_t*                mMappedCb = nullptr;
    uint32_t                mConstantFrameSlot = 0;
    // Offset between consecutive blocks; constant buffers need 256-byte alignment.
    static constexpr UINT64 kConstantStride = (sizeof(RtGIConstants) + 255ull) & ~255ull;

    // Byte offset of the block this frame's passes should bind.
    UINT64 CurrentConstantOffset() const
    {
        return static_cast<UINT64>(mConstantFrameSlot) * kConstantStride;
    }

    // GPU address of that block, for SetComputeRootConstantBufferView.
    D3D12_GPU_VIRTUAL_ADDRESS CurrentConstantAddress() const
    {
        return mConstantBuffer->GetGPUVirtualAddress() + CurrentConstantOffset();
    }

    // Ping-pong reservoir structured buffers (read/write swap each frame).
    ComPtr<ID3D12Resource>  mReservoirBuffer[2];

    // Intermediate GI radiance buffer (RGBA16F), written by RayGen, read by Temporal/Spatial.
    ComPtr<ID3D12Resource>  mRadianceBuffer;

    // Temporal accumulation history (RGBA16F), read and written by Composite each frame.
    ComPtr<ID3D12Resource>  mAccumBuffer;

    // Final composited GI output (RGBA16F), read by the deferred lighting pass.
    ComPtr<ID3D12Resource>  mOutputBuffer;

    // Specular reflection output (RGBA16F), read by the deferred lighting pass.
    ComPtr<ID3D12Resource>  mSpecularBuffer;

    // Non-shader-visible CPU heap for internal UAV operations.
    ComPtr<ID3D12DescriptorHeap>  mCpuUavHeap;

    // ---- NRD RELAX_DIFFUSE resources ----------------------------------------
    // These textures carry the NRD inputs (prepared by NrdPrepare shader) and
    // the NRD output (consumed by NrdComposite shader).
    ComPtr<ID3D12Resource>  mNrdDiffRadianceHitDist; // IN_DIFF_RADIANCE_HITDIST (RGBA16F)
    ComPtr<ID3D12Resource>  mNrdMotionVectors;        // IN_MV                    (RG16F)
    ComPtr<ID3D12Resource>  mNrdNormalRoughness;      // IN_NORMAL_ROUGHNESS      (RGBA8)
    ComPtr<ID3D12Resource>  mNrdViewZ;                // IN_VIEWZ                 (R32F)
    ComPtr<ID3D12Resource>  mNrdOutDiff;              // OUT_DIFF_RADIANCE_HITDIST (R11G11B10F)

    // SRV/UAV descriptors for the NRD input textures (written by NrdPrepare).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdViewZUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdViewZUavGpu = {};

    // SRV descriptor for the denoised NRD output (read by NrdComposite).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvGpu = {};
    // SRV descriptor for the NRD radiance input (read by NrdComposite as SRV).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdDiffRadianceSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdDiffRadianceSrvGpu = {};

    // The NRD denoiser instance — created/destroyed with the RTGI renderer.
    NrdDenoiser  mNrdDenoiser;
    bool         mNrdDescriptorsAllocated = false;

    // Shader-visible SRV/UAV descriptors allocated from the shared heap.
    D3D12_CPU_DESCRIPTOR_HANDLE mReservoirSrvCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mReservoirSrvGpu[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mReservoirUavCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mReservoirUavGpu[2] = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mRadianceSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRadianceSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mRadianceUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRadianceUavGpu = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mAccumSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mAccumSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mAccumUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mAccumUavGpu = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu = {};

    // Specular reflection buffer descriptors.
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularUavGpu = {};
    bool                        mSpecularBufferInSrvState = false;

    // TLAS SRV (bound at t2 in RayGen shader).
    D3D12_CPU_DESCRIPTOR_HANDLE mTlasSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mTlasSrvGpu = {};

    // Global geometry SRVs (bound at t3/t4/t5 in RayGen shader).
    // These allow the shader to interpolate true per-vertex normals at secondary hits.
    D3D12_CPU_DESCRIPTOR_HANDLE mVertexSrvCpu       = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mVertexSrvGpu       = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mIndexSrvCpu        = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mIndexSrvGpu        = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mInstanceInfoSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mInstanceInfoSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mMaterialRangeSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mMaterialRangeSrvGpu = {};

    bool     mDescriptorsAllocated = false;
    uint32_t mReservoirWriteIdx    = 0;
    bool     mOutputBufferInSrvState = false;  // true after first Dispatch; false until then

    // CPU-side geometry pools – grown as new BLASes are built, never shrunk.
    std::vector<GpuPackedVertex> mCpuVertices;
    std::vector<uint32_t>        mCpuIndices;
    bool                         mGeometryDirty = false;  // true when the pools grew this frame

    // Folds everything that affects the acceleration structure's contents into
    // one value, so an unchanged scene can be detected before any work is done.
    std::uint64_t ComputeSceneSignature(
        const std::vector<Entity>& entities,
        const std::vector<VegetationRenderer::RayTracingBatch>* vegetationBatches) const;

    // Signature of the scene the current TLAS was built from: entity count,
    // mesh identity, world transforms and material assignment, plus the same
    // for any ray-traced vegetation.
    //
    // A top-level acceleration structure only describes where instances are, so
    // rebuilding it when nothing has moved produces a bit-identical result at
    // full cost. An editor scene is static most of the time - the camera moves,
    // the geometry does not - so comparing this signature first turns the
    // common case into no work at all. Zero means "nothing built yet".
    std::uint64_t                mSceneSignature = 0;

    // Per-frame instance info rebuilt every BuildTlas call.
    std::vector<GpuInstanceInfo> mCpuInstanceInfo;
    std::vector<GpuMaterialRange> mCpuMaterialRanges;

    // GPU buffers for the geometry pools (UPLOAD heap, persistently mapped, recreated on growth).
    ComPtr<ID3D12Resource> mVertexBuffer;        // StructuredBuffer<GpuPackedVertex>  t3
    ComPtr<ID3D12Resource> mIndexBuffer;         // StructuredBuffer<uint>              t4
    ComPtr<ID3D12Resource> mInstanceInfoBuffer;  // StructuredBuffer<GpuInstanceInfo>   t5
    ComPtr<ID3D12Resource> mMaterialRangeBuffer; // StructuredBuffer<GpuMaterialRange>
    void* mMappedVertices     = nullptr;
    void* mMappedIndices      = nullptr;
    void* mMappedInstanceInfo = nullptr;
    void* mMappedMaterialRanges = nullptr;

    // ---- Acceleration structures ----
    struct BlasEntry
    {
        ComPtr<ID3D12Resource> Result;
        ComPtr<ID3D12Resource> Scratch;
        uint32_t vertexOffset = 0;  // base index into the global vertex buffer
        uint32_t indexOffset  = 0;  // base index into the global index buffer
        uint32_t vertexCount  = 0;
        uint32_t indexCount   = 0;
    };
    struct BlasKey
    {
        const void* Mesh = nullptr;
        bool        NonOpaque = false;

        bool operator==(const BlasKey& other) const noexcept
        {
            return Mesh == other.Mesh && NonOpaque == other.NonOpaque;
        }
    };

    struct BlasKeyHasher
    {
        size_t operator()(const BlasKey& key) const noexcept
        {
            return (reinterpret_cast<size_t>(key.Mesh) >> 4) ^ (key.NonOpaque ? 0x9e3779b9u : 0u);
        }
    };

    std::unordered_map<BlasKey, BlasEntry, BlasKeyHasher> mBlasCache;
    std::unordered_map<std::string, std::vector<RtMaterialSlotInfo>> mMaterialSlotCache;
    std::unordered_map<std::string, std::array<float, 3>> mTextureAverageColorCache;
    TextureManager mTextureManager;

    D3D12_CPU_DESCRIPTOR_HANDLE mBaseTextureTableCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mBaseTextureTableGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOpacityTextureTableCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOpacityTextureTableGpu = {};
    bool                        mMaterialTextureTableAllocated = false;
    uint32_t                    mMaterialTextureCount = 0;
    std::unordered_map<std::string, uint32_t> mMaterialTextureIndices;

    ComPtr<ID3D12Resource>  mTlas;
    ComPtr<ID3D12Resource>  mTlasScratch;
    ComPtr<ID3D12Resource>  mInstanceDescBuffer;
    UINT                    mTlasMaxInstances = 0;

    // Whether a valid TLAS has been built at least once.
    // RayGen is skipped (outputs zeros) until this is true.
    bool mTlasReady = false;

    // Upload buffers that must stay alive until the GPU finishes the frame.
    // Two-frame ring: index 0 is being built this frame, index 1 was the previous frame.
    // We only release index 1 at the START of each BuildTlas (safe because the GPU
    // will have finished the frame-N-1 commands by the time frame N+1 starts).
    // Upload buffers kept alive until the GPU has finished consuming them.
    //
    // Depth must match the engine's frames in flight (DX12Context's FrameCount),
    // not two: with three frames queued the CPU can be three frames ahead of the
    // GPU, and a two-slot ring would free a BLAS's vertex or index upload while
    // an earlier frame's acceleration-structure build was still reading it.
    static constexpr uint32_t kFramesInFlight = 3;
    std::vector<ComPtr<ID3D12Resource>> mPendingUploads[kFramesInFlight];
    uint32_t mUploadRingIdx = 0;
};
