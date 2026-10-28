#pragma once
// NrdDenoiser.h
// Pure D3D12 wrapper around the NVIDIA Real-time Denoisers (NRD) library.
// Uses RELAX_DIFFUSE to denoise one-bounce GI radiance produced by RtGI_RayGen,
// or RELAX_DIFFUSE_SPECULAR when the specular textures are given (the path tracer).
//
// Does NOT use the NRDIntegration.hpp helper (which requires NRI).
// Manages its own shader-visible descriptor heap and pipelines built from
// the DXIL bytecode embedded in NRD.lib.

#include "DX12Helper.h"
#include <NRD.h>

#include <cstdint>
#include <vector>
#include <string>

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Resource pointers and their formats, passed to NrdDenoiser::Initialize().
// All textures must stay alive for the lifetime of the NrdDenoiser.
// ----------------------------------------------------------------------------
struct NrdInitTextures
{
    // NRD inputs (written by RayGen shader as UAVs, read by NRD as SRVs)
    ID3D12Resource* diffRadianceHitDist = nullptr; // IN_DIFF_RADIANCE_HITDIST (RGBA16F)
    ID3D12Resource* motionVectors       = nullptr; // IN_MV                    (RG16F)
    ID3D12Resource* normalRoughness     = nullptr; // IN_NORMAL_ROUGHNESS      (format varies)
    ID3D12Resource* viewZ               = nullptr; // IN_VIEWZ                 (R32F)

    // NRD output (written by NRD as UAV, read by Composite as SRV)
    ID3D12Resource* outDiffRadiance = nullptr;     // OUT_DIFF_RADIANCE_HITDIST (R11G11B10F)

    // Optional specular channel. Both set selects RELAX_DIFFUSE_SPECULAR.
    ID3D12Resource* specRadianceHitDist = nullptr; // IN_SPEC_RADIANCE_HITDIST  (RGBA16F)
    ID3D12Resource* outSpecRadiance     = nullptr; // OUT_SPEC_RADIANCE_HITDIST (RGBA16F)

    // Formats for descriptor creation
    DXGI_FORMAT diffRadianceHitDistFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT motionVectorsFmt       = DXGI_FORMAT_R16G16_FLOAT;
    DXGI_FORMAT normalRoughnessFmt     = DXGI_FORMAT_R10G10B10A2_UNORM;
    DXGI_FORMAT viewZFmt               = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT outDiffRadianceFmt     = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT specRadianceHitDistFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT outSpecRadianceFmt     = DXGI_FORMAT_R16G16B16A16_FLOAT;

    bool HasSpecular() const { return specRadianceHitDist != nullptr && outSpecRadiance != nullptr; }
};

// ----------------------------------------------------------------------------
// NrdDenoiser
// Lifetime:  Initialize() once → Denoise() per frame → Shutdown()
// If the render resolution changes, call Shutdown() then Initialize() again.
// ----------------------------------------------------------------------------
class NrdDenoiser
{
public:
    NrdDenoiser()  = default;
    ~NrdDenoiser() { Shutdown(); }

    // Returns true on success.  Must be called after DX12Context is ready.
    bool Initialize(UINT width, UINT height, const NrdInitTextures& textures);

    // Release all GPU and NRD objects.
    void Shutdown();

    bool IsInitialized() const { return mIsInitialized; }

    // The format NRD expects for IN_NORMAL_ROUGHNESS (read from LibraryDesc).
    DXGI_FORMAT GetNormalRoughnessFormat() const { return mNormalRoughnessFormat; }

    // Run all RELAX_DIFFUSE compute dispatches for this frame.
    //
    // All external textures from NrdInitTextures must be in
    // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE on entry (they were
    // written as UAVs by RayGen and transitioned by the caller).
    // (In practice callers leave them as UNORDERED_ACCESS, which is what the
    // wrapper assumes.) Every external texture - inputs and outputs - is left in
    // D3D12_RESOURCE_STATE_UNORDERED_ACCESS on exit.
    //
    // viewToClipMatrix / worldToViewMatrix: column-major, NON-jittered.
    void Denoise(
        ID3D12GraphicsCommandList* cmdList,
        const float viewToClipMatrix[16],
        const float viewToClipMatrixPrev[16],
        const float worldToViewMatrix[16],
        const float worldToViewMatrixPrev[16],
        const float cameraJitter[2],
        const float cameraJitterPrev[2],
        UINT        frameIndex,
        bool        resetHistory,
        float       timeDeltaMs = 0.0f);

    // Apply NRD settings (called whenever the user changes denoiser params).
    void SetMaxAccumulationTime(float seconds, float fps) { mMaxAccumTime = seconds; mFps = fps; }
    void SetAtrousIterations(int n) { mAtrousIterations = n; }
    void SetDisocclusionThreshold(float t) { mDisocclusionThreshold = t; }

    // Replaces the built-in RELAX tuning (which is tuned for the RTGI one-bounce
    // signal) with the caller's own, from the next Denoise() on.
    void SetRelaxSettings(const nrd::RelaxSettings& settings) { mCustomRelax = settings; mHasCustomRelax = true; }

    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    // ------------ pipeline helpers ------------------------------------------
    bool CreatePipelinesAndRootSignatures();
    bool CreatePoolTextures();
    bool CreateCpuDescriptorHeap();
    bool CreateShaderVisibleDescriptorHeap();
    bool BuildDispatchDescriptorTables();

    // ------------ per-dispatch execution ------------------------------------
    void ExecuteDispatch(
        ID3D12GraphicsCommandList* cmdList,
        const nrd::DispatchDesc&   dispatch);

    // ------------ resource mapping ------------------------------------------
    // Returns the CPU-heap slot index for a given NRD resource desc.
    UINT GetCpuSlot(const nrd::ResourceDesc& r, bool wantSrv) const;

    // Creates one SRV and one UAV descriptor for 'res' into the CPU heap at the
    // given slot (SRV) and slot+N (UAV, where N = total SRV count).
    void CreateTexDescriptors(
        ID3D12Device*               device,
        ID3D12Resource*             res,
        DXGI_FORMAT                 fmt,
        UINT                        srvSlot,  // index into mCpuHeap
        UINT                        uavSlot); // index into mCpuHeap

    // Overload that allows different formats for SRV and UAV (e.g. R10G10B10A2 SRV + R32_UINT UAV).
    void CreateTexDescriptors(
        ID3D12Device*               device,
        ID3D12Resource*             res,
        DXGI_FORMAT                 srvFmt,
        DXGI_FORMAT                 uavFmt,
        UINT                        srvSlot,
        UINT                        uavSlot);

    // ------------ format helpers --------------------------------------------
    static DXGI_FORMAT NrdToDxgi(nrd::Format f);
    static DXGI_FORMAT NormalEncodingToDxgi(nrd::NormalEncoding enc);

    // ------------ state --------------------------------------------------
    nrd::Instance* mNrdInstance = nullptr;
    static constexpr nrd::Identifier kDenoiserId = 0;

    UINT mWidth  = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;

    // ------------ D3D12 pipelines per NRD pass ------------------------------
    struct NrdPipeline
    {
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;
        UINT srvCount = 0; // number of SRV descriptors this pass needs
        UINT uavCount = 0; // number of UAV descriptors this pass needs
        bool hasConstantData = false;
    };
    std::vector<NrdPipeline> mPipelines;

    // ------------ pool textures ---------------------------------------------
    std::vector<ComPtr<ID3D12Resource>>  mPermanentTextures;
    std::vector<ComPtr<ID3D12Resource>>  mTransientTextures;
    // Current D3D12 resource state for each pool texture (needed for transitions).
    std::vector<D3D12_RESOURCE_STATES>   mPermStates;
    std::vector<D3D12_RESOURCE_STATES>   mTransStates;
    // Current state for external textures (indexed by kExt* constants).
    D3D12_RESOURCE_STATES mExtStates[7] = {};   // kExtCount, declared below

    // ------------ descriptor heaps ------------------------------------------
    // CPU-only heap: stores SRV+UAV for every managed resource (pools + external).
    // Layout:
    //   [0 .. N_perm-1]                     permanent SRVs
    //   [N_perm .. 2*N_perm-1]              permanent UAVs
    //   [2*N_perm .. 2*N_perm+N_trans-1]   transient  SRVs
    //   [2*N_perm+N_trans .. 2*N_perm+2*N_trans-1] transient UAVs
    //   [2*N_perm+2*N_trans+0..4]           external   SRVs (5 textures)
    //   [2*N_perm+2*N_trans+5..9]           external   UAVs (5 textures)
    ComPtr<ID3D12DescriptorHeap> mCpuHeap;
    UINT mCpuHeapSize   = 0;
    UINT mPermCount     = 0;
    UINT mTransCount    = 0;

    static constexpr UINT kExtCount      = 7; // # of external textures
    static constexpr UINT kExtDiffRadiance   = 0; // IN_DIFF_RADIANCE_HITDIST
    static constexpr UINT kExtMotionVec      = 1; // IN_MV
    static constexpr UINT kExtNormalRoughness= 2; // IN_NORMAL_ROUGHNESS
    static constexpr UINT kExtViewZ          = 3; // IN_VIEWZ
    static constexpr UINT kExtOutDiff        = 4; // OUT_DIFF_RADIANCE_HITDIST
    static constexpr UINT kExtSpecRadiance   = 5; // IN_SPEC_RADIANCE_HITDIST  (DIFFUSE_SPECULAR only)
    static constexpr UINT kExtOutSpec        = 6; // OUT_SPEC_RADIANCE_HITDIST (DIFFUSE_SPECULAR only)

    static_assert(kExtCount == sizeof(mExtStates) / sizeof(mExtStates[0]), "mExtStates must hold every external texture");

    // NRD resource type -> kExt* index, or kExtCount for one this wrapper does not bind.
    static UINT ExternalIndex(nrd::ResourceType type);
    ID3D12Resource* ExternalResource(UINT extIdx) const;

    // Shader-visible CBV/SRV/UAV heap for NRD dispatches.
    // Re-populated each frame using CopyDescriptors.
    // Size = totalTexturesNum + totalStorageTexturesNum (from DescriptorPoolDesc).
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    UINT mSrvHeapCapacity = 0;
    UINT mDescriptorStride = 0;

    // Sampler heap: slot 0 = NEAREST_CLAMP, slot 1 = LINEAR_CLAMP (or static samplers).
    // We use static samplers in the root signature — no runtime sampler heap needed.

    // ------------ per-dispatch descriptor tables (pre-computed at init) -----
    // For each NRD dispatch, store the CPU handles to copy into the shader-
    // visible heap, and the offsets within mSrvHeap where they will land.
    struct DispatchTableInfo
    {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvCpuHandles; // to CopyDescriptors from
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> uavCpuHandles;
        UINT srvHeapOffset = 0; // offset in mSrvHeap for this dispatch's SRV block
        UINT uavHeapOffset = 0; // offset in mSrvHeap for this dispatch's UAV block
        UINT pipelineIndex = 0;
    };
    std::vector<DispatchTableInfo> mDispatchTables;

    // ------------ constant buffer ring buffer --------------------------------
    // One 256-byte slot per NRD dispatch, persistently mapped UPLOAD heap.
    ComPtr<ID3D12Resource> mCbRingBuffer;
    uint8_t*               mCbMapped    = nullptr;
    UINT                   mCbRingSize  = 0; // total bytes
    UINT                   mCbSlotSize  = 256; // per-slot size (must be >= NRD constantBufferMaxDataSize and 256-byte aligned)
    UINT                   mCbSlotCount = 0; // # of slots

    // Shader-visible heap offset that advances across dispatches within one frame.
    // Reset to the frame slot's base at the start of each Denoise() call.
    UINT mFrameHeapOffset = 0;
    // Monotonic per-frame dispatch index used for constant-buffer ring allocation.
    UINT mCbDispatchIndex = 0;

    // The descriptor heap and the constant ring hold one region per frame in
    // flight. A single region was rewritten from the start by every Denoise()
    // while the GPU could still be running up to two earlier frames' NRD passes,
    // which read their descriptors and constants from it - see
    // three-frames-in-flight-upload-buffers.
    static constexpr UINT kFramesInFlight = 3;
    UINT mFrameSlot = 0;
    UINT mSrvHeapCapacityPerFrame = 0;

    // ------------ denoiser settings ------------------------------------------
    float mMaxAccumTime          = 0.5f;  // seconds
    float mFps                   = 60.0f;
    float mDisocclusionThreshold = 0.01f;
    int   mAtrousIterations      = 5;

    nrd::RelaxSettings mCustomRelax{};
    bool               mHasCustomRelax = false;
    bool               mSpecular       = false;   // RELAX_DIFFUSE_SPECULAR

    // ------------ cached from LibraryDesc ------------------------------------
    DXGI_FORMAT mNormalRoughnessFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT        mNormalRoughnessNrdEncoding = 0; // NRD_NORMAL_ENCODING_* value

    // Stored init textures so we can create the external descriptors
    NrdInitTextures mInitTextures;
};
