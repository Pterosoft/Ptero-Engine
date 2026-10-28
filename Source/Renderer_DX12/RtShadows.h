#pragma once

// RtShadows.h
// Ray-traced sun shadows for Ptero-Engine.
//
// Three compute passes using DXR inline ray tracing (cs_6_5 RayQuery):
//   Pass 0 - RayGen   : per-pixel shadow rays toward a point on the sun's disc
//                       -> raw visibility + distance to the blocker
//   Pass 1 - Temporal : reprojects the previous frame's result and blends it in,
//                       rejecting history whose surface moved
//   Pass 2 - Spatial  : edge-aware blur whose radius follows the penumbra width
//                       estimated from the blocker distance and the sun's size
//   Pass 3 - Local    : walks the pixel's clustered light list and traces the first
//                       kMaxLocalLightsPerPixel shadow-casting lights that reach it
//   Pass 4 - LocalTemporal : per light (matched by index), blends in the reprojected
//                       history
//   Pass 5 - LocalSpatial  : per light, edge-aware blur across penumbrae
// The sun output (R16F, 1 = lit) replaces the shadow-map sun term in DeferredLighting;
// the local output (RGBA32_UINT, eight 16-bit "light index << 8 | visibility" pairs,
// 0xFFFF = empty) replaces the shadow maps of the lights it lists.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "LightClusterRenderer.h"
#include "RtShadowSettings.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// GPU constant buffer shared across the RT shadow shaders (RtShadow_Common.hlsli).
// -----------------------------------------------------------------------
struct alignas(256) RtShadowConstants
{
    uint32_t FrameWidth;
    uint32_t FrameHeight;
    uint32_t FrameIndex;
    uint32_t RaysPerPixel;      // 16 bytes

    float    SunDirection[3];   // world-space direction TOWARD the sun (normalised)
    float    SunTanHalfAngle;   // 32 bytes

    float    MaxRayLength;
    float    RayBias;
    float    TemporalBlend;
    int      MaxFilterRadius;   // 48 bytes

    float    CameraPos[3];
    uint32_t HistoryValid;      // 64 bytes

    float    PrevCameraPos[3];
    float    PixelAngle;        // 80 bytes  (radians covered by one pixel at the screen centre)

    float    ViewProjInv[16];   // 144 bytes (row-major inverse jittered VP for world-pos reconstruction)
    float    PrevViewProj[16];  // 208 bytes (row-major previous non-jittered VP for reprojection)

    int      DebugView;
    float    LocalLightSoftness;
    uint32_t LocalHistoryValid;
    float    _pad0;             // 224 bytes
};

static_assert(sizeof(RtShadowConstants) == 256, "RtShadowConstants must stay one 256-byte CBV");

class RtShadows
{
public:
    RtShadows() = default;
    ~RtShadows() { Shutdown(); }

    // Initialise (or re-initialise) for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Resize resolution-dependent resources. No-op if the size is unchanged.
    bool EnsureSize(UINT width, UINT height);

    // Discard the temporal history so the next dispatch uses only the current frame.
    void ResetHistory() { mHistoryValid = false; mLocalHistoryValid = false; }

    void Shutdown();

    //   gbufferNormalSrv - G-Buffer normal target (RG = oct normal, B = hardware depth)
    //   tlasSrv          - scene TLAS (RtGlobalIllumination::GetTlasSrv)
    //   sunDirection     - direction FROM the sun toward the scene, as the lighting pass has it
    //   viewProjInv      - row-major inverse of the jittered view-projection
    //   prevViewProj     - row-major previous non-jittered view-projection
    //   lightClusters    - this frame's clustered light lists; local lights are
    //                      skipped while they are not valid
    void Dispatch(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  tlasSrv,
        const LightClusterBindings&  lightClusters,
        const RtShadowSettings&      settings,
        const float                  sunDirection[3],
        const float                  viewProjInv[16],
        const float                  prevViewProj[16],
        const float                  cameraPos[3],
        const float                  prevCameraPos[3],
        float                        verticalFovRadians);

    // Shader-visible SRV of the final sun visibility (R16F, 1 = lit); with debug view 2
    // it holds the raw one-frame visibility instead.
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv() const { return mOutputSrvGpu; }

    // Shader-visible SRV of the local light visibility pairs, and whether the last
    // Dispatch filled it.
    D3D12_GPU_DESCRIPTOR_HANDLE GetLocalSrv() const { return mLocalSrvGpu; }
    bool HasLocalLights() const { return mLocalWritten; }

    bool        IsInitialized() const { return mIsInitialized; }
    bool        HasInitFailed() const { return mInitFailed; }
    const char* GetLastError()  const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateResolutionResources();

    bool        mIsInitialized = false;
    bool        mInitFailed    = false;
    bool        mHistoryValid  = false;
    bool        mOutputInSrvState = true;
    bool        mLocalInSrvState = true;
    bool        mLocalWritten  = false;
    bool        mLocalHistoryValid = false;
    uint32_t    mLocalHistoryIndex = 0;
    bool        mDescriptorsAllocated = false;
    UINT        mWidth         = 0;
    UINT        mHeight        = 0;
    uint32_t    mFrameIndex    = 0;
    uint32_t    mHistoryIndex  = 0;
    std::string mLastError;

    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mPSO_RayGen;
    ComPtr<ID3D12PipelineState> mPSO_Temporal;
    ComPtr<ID3D12PipelineState> mPSO_Spatial;
    ComPtr<ID3D12PipelineState> mPSO_Local;
    ComPtr<ID3D12PipelineState> mPSO_LocalTemporal;
    ComPtr<ID3D12PipelineState> mPSO_LocalSpatial;

    // One slot per recording, more than there are frames in flight: a single slot was
    // rewritten for the next frame while the GPU still read it, so while the camera
    // moved the passes rebuilt positions with the wrong frame's matrices.
    static constexpr UINT  kCbRingSlots = 8;
    ComPtr<ID3D12Resource> mConstantBuffer;
    void*                  mMappedCb = nullptr;
    UINT                   mCbRingSlot = 0;

    // Raw: x = visibility, y = blocker distance. History: x = visibility,
    // y = blocker distance, z = distance from the camera (for disocclusion tests).
    ComPtr<ID3D12Resource> mRawBuffer;
    ComPtr<ID3D12Resource> mHistory[2];
    ComPtr<ID3D12Resource> mOutputBuffer;
    ComPtr<ID3D12Resource> mLocalBuffer;
    // Raw traced pairs, and the per-light history: [ping-pong][entries 0-3 / 4-7].
    ComPtr<ID3D12Resource> mLocalRawBuffer;
    ComPtr<ID3D12Resource> mLocalHistory[2][2];

    D3D12_CPU_DESCRIPTOR_HANDLE mLocalRawUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mLocalRawUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mLocalHistoryUavCpu[2][2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mLocalHistoryUavGpu[2][2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mLocalUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mLocalUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mLocalSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mLocalSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mRawUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRawUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mHistoryUavCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mHistoryUavGpu[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu = {};
};
