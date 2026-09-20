#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "AgxTonemapSettings.h"

// AutoExposure meters the HDR scene and maintains one adapted EV100 on the GPU,
// following Unreal's histogram eye adaptation.
//
// Two dispatches per frame:
//   Histogram - bins log-luminance of the (strided) scene colour into 64 bins,
//               weighted by a centre-biased metering mask.
//   Adapt     - keeps the percentile band between LowPercent and HighPercent,
//               averages it in log space, converts to EV100, clamps it to
//               [Ev100Min, Ev100Max], and eases the previous frame's value
//               towards it at SpeedUp / SpeedDown stops per second.
//
// The result stays in GPU memory. AgxTonemapper binds the exposure buffer as an
// SRV and reads it in the shader, so nothing has to be read back to the CPU and
// the tonemapper's constant buffer stays static from frame to frame. Reading it
// back would cost a multi-frame stall or a stale value, and writing the
// exposure into AgX's constant buffer every frame would turn that single mapped
// upload buffer into a torn read with three frames in flight.
class AutoExposure
{
public:
    bool Initialize();
    void Shutdown();

    // Record both dispatches. `inputResource` is the HDR scene colour that the
    // tonemapper is about to consume, in PIXEL_SHADER_RESOURCE state; it is
    // left in that state. Its dimensions are read from the resource rather than
    // passed in - with DLSS in the chain the metered image is not necessarily
    // the scene's render size. Returns false if the pass is not usable, in
    // which case the tonemapper falls back to the manual exposure.
    bool Apply(
        ID3D12GraphicsCommandList*  commandList,
        ID3D12Resource*             inputResource,
        D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
        const AgxTonemapSettings&   settings,
        float                       deltaTimeSeconds);

    // Snap to the metered exposure on the next frame instead of easing into it.
    // Call when the view changes discontinuously - a level load, a camera cut -
    // so the player does not watch a second of adaptation from the old scene.
    void ResetHistory() { mResetHistory = true; }

    // CPU handle of the exposure buffer's SRV, for the tonemapper to copy into
    // its own descriptor heap.
    D3D12_CPU_DESCRIPTOR_HANDLE GetExposureCpuSrv() const { return mExposureSrvCpu; }
    ID3D12Resource*             GetExposureResource() const { return mExposureBuffer.Get(); }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipelines();
    bool CreateBuffers();

    static constexpr UINT kHistogramBins  = 64;
    static constexpr UINT kThreadsX       = 16;
    static constexpr UINT kThreadsY       = 16;
    static constexpr UINT kFramesInFlight = 3;

    // Must match AutoExposureCb in AutoExposure.hlsli.
    struct alignas(256) AutoExposureCbData
    {
        UINT  InputWidth;
        UINT  InputHeight;
        UINT  MeterStride;
        UINT  ResetHistory;

        float LogMin;
        float LogRange;
        float LowPercent;
        float HighPercent;

        float MinEv100;
        float MaxEv100;
        float SpeedUp;
        float SpeedDown;

        float DeltaTimeSeconds;
        float GreyPoint;
        float MeteringMask;
        float Pad0;
    };

    DX12Shader                                  mHistogramShader;
    DX12Shader                                  mAdaptShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mHistogramRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mAdaptRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mHistogramPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mAdaptPipeline;

    // Constants change every frame (delta time at minimum), so this is ringed
    // one copy per frame in flight. See the note in the class comment.
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    void*                                  mMappedCb = nullptr;
    UINT                                   mFrameSlot = 0;
    UINT64                                 mCbStride = 0;

    // GPU-only state.
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistogramBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mExposureBuffer;

    // Private heap: slot 0 = t0 scene SRV, slot 1 = u0 histogram UAV,
    //               slot 2 = u1 exposure UAV.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT                                         mComputeHeapStride = 0;

    // Shared-heap SRV of the exposure buffer, read by the tonemapper.
    D3D12_CPU_DESCRIPTOR_HANDLE mExposureSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mExposureSrvGpu{};
    bool                        mExposureSrvAllocated = false;

    bool        mResetHistory = true;
    bool        mIsInitialized = false;
    std::string mLastError;
};
