#pragma once

// RtAmbientOcclusion.h
// Ray Traced Ambient Occlusion pass for Ptero-Engine.
//
// Three compute passes using DXR inline ray-tracing (cs_6_5 RayQuery):
//   Pass 0 - RayGen   : per-pixel cosine-hemisphere occlusion rays via RayQuery → raw R16F AO
//   Pass 1 - Temporal : exponential moving average accumulation of the raw AO signal
//   Pass 2 - Spatial  : edge-aware 5×5 bilateral blur using G-Buffer normals and depth

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "NrdDenoiser.h"
#include "RtAOSettings.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// GPU constant buffer shared across the RTAO shaders.
// alignas(256) ensures the root CBV address is correctly aligned.
// -----------------------------------------------------------------------
struct alignas(256) RtAOConstants
{
    uint32_t FrameWidth;
    uint32_t FrameHeight;
    uint32_t FrameIndex;
    uint32_t RaysPerPixel;      // 16 bytes

    float    MaxRayLength;
    float    RayBias;
    float    AOPower;
    float    Intensity;         // 32 bytes

    int      DebugView;
    float    _pad0[3];          // 48 bytes

    float    ViewProjInv[16];   // 112 bytes  (row-major inverse VP for world-pos reconstruction)

    float    CurrViewProj[16];  // 176 bytes  (row-major current non-jittered VP for reprojection)

    float    PrevViewProj[16];  // 240 bytes  (row-major previous non-jittered VP for reprojection)

    float    WorldToView[16];   // 304 bytes  (row-major current non-jittered world-to-view)

    float    CameraPos[3];
    float    _pad1;
};

static_assert(sizeof(RtAOConstants) == 512, "RtAOConstants must remain 256-byte aligned");

// -----------------------------------------------------------------------
// RtAmbientOcclusion
// -----------------------------------------------------------------------
class RtAmbientOcclusion
{
public:
    RtAmbientOcclusion()  = default;
    ~RtAmbientOcclusion() { Shutdown(); }

    // Initialise (or re-initialise) for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Resize resolution-dependent resources.  No-op if size is unchanged.
    bool EnsureSize(UINT width, UINT height);

    // Discard temporal AO history so the next dispatch uses only the current frame.
    void ResetHistory();

    // Release all GPU resources.
    void Shutdown();

    // Dispatch the ray-generation pass and NRD denoising path for this frame.
    //
    //   cmdList            – must be a CommandList4; not closed by this function.
    //   gbufferNormalSrv   – SRV GPU handle for the G-Buffer normal target
    //                        (RG = oct-encoded world normal, B = linear depth).
    //   tlasSrv            – SRV GPU handle for the scene TLAS built by RtGlobalIllumination.
    //   viewProjInv[16]    – row-major inverse view-projection for world-pos reconstruction.
    //   cameraPos[3]       – world-space camera position.
    void Dispatch(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  sceneDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  tlasSrv,
        const RtAOSettings&          settings,
        const float                  viewProjInv[16],
        const float                  currViewProj[16],
        const float                  prevViewProj[16],
        const float                  cameraPos[3],
        const float                  worldToViewMatrix[16],
        const float                  viewToClipMatrix[16],
        const float                  prevWorldToViewMatrix[16],
        const float                  prevViewToClipMatrix[16],
        const float                  cameraJitter[2],
        const float                  prevCameraJitter[2],
        float                        timeDeltaMs);

    // Shader-visible SRV GPU handle for the final resolved AO texture (R16F).
    // Valid after the first successful Dispatch().
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv() const { return mOutputSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetRawAOSrv() const { return mRawAOSrvGpu; }

    bool        IsInitialized()  const { return mIsInitialized; }
    bool        HasInitFailed()  const { return mInitFailed; }
    const char* GetLastError()   const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignature();
    bool CreateRootSignatureNrd();
    bool CreatePipelines();
    bool CreateResolutionBuffers();
    bool CreateDescriptors();
    bool InitNrd();

    void UploadConstants(
        const RtAOSettings& s,
        const float viewProjInv[16],
        const float currViewProj[16],
        const float prevViewProj[16],
        const float cameraPos[3],
        const float worldToViewMatrix[16]);

    // ── State ─────────────────────────────────────────────────────────────
    bool        mIsInitialized = false;
    bool        mInitFailed    = false;
    UINT        mWidth         = 0;
    UINT        mHeight        = 0;
    uint32_t    mFrameIndex    = 0;
    bool        mRawAOBufferInSrvState = false;
    bool        mOutputBufferInSrvState = false;
    std::string mLastError;

    // ── Root signatures ────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12RootSignature> mRootSignatureNrd;

    // ── One PSO per compute pass ──────────────────────────────────────────
    ComPtr<ID3D12PipelineState> mPSO_RayGen;
    ComPtr<ID3D12PipelineState> mPSO_NrdPrepare;
    ComPtr<ID3D12PipelineState> mPSO_NrdComposite;

    // ── Persistently-mapped 256-byte upload constant buffer ───────────────
    ComPtr<ID3D12Resource> mConstantBuffer;
    void*                  mMappedCb = nullptr;

    // ── AO and NRD textures ────────────────────────────────────────────────
    ComPtr<ID3D12Resource> mRawAOBuffer;
    ComPtr<ID3D12Resource> mOutputBuffer;
    ComPtr<ID3D12Resource> mNrdDiffRadianceHitDist;
    ComPtr<ID3D12Resource> mNrdMotionVectors;
    ComPtr<ID3D12Resource> mNrdNormalRoughness;
    ComPtr<ID3D12Resource> mNrdViewZ;
    ComPtr<ID3D12Resource> mNrdOutDiff;

    // ── Descriptor handles allocated from the shared SRV/UAV heap ─────────
    D3D12_CPU_DESCRIPTOR_HANDLE mRawAOSrvCpu  = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRawAOSrvGpu  = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mRawAOUavCpu  = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRawAOUavGpu  = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdViewZUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdViewZUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvGpu = {};

    bool mDescriptorsAllocated = false;
    bool mNrdDescriptorsAllocated = false;
    NrdDenoiser mNrdDenoiser;
};
