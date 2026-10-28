#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TaaSettings.h"

#include "../QtUi/UiTypes.h"

#include <DirectXMath.h>
#include <string>

// TAARenderer owns the GPU resources of the temporal AA resolve, a port of
// Unreal Engine 5's TemporalAA.usf (see TAAResolve.hlsl).
//
// It runs in one of two configurations:
//   - TAA:  input, output and history are all the render size.
//   - TAAU: input at render size, output and history at the display size, so
//           the pass is also the upscaler (r.TemporalAA.Upsampling).
//
// Design notes:
//   - The history is two textures used in turn: the one written this frame is
//     read as history next frame, so there is no copy.
//   - Each texture has its own SRV slot in the shared heap, so whichever is the
//     current output can be displayed without rewriting a descriptor the GPU may
//     still be reading for an earlier frame.
//   - The shader-visible descriptors and the constant buffer are rings with one
//     entry per frame in flight, for the same reason.
class TAARenderer
{
public:
    struct FrameInputs
    {
        // Scene colour at the render size, in PIXEL_SHADER_RESOURCE state.
        ID3D12Resource*             Color = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE ColorSrv{};
        // Scene depth (R32_FLOAT view) and motion vectors at the render size,
        // already readable from compute shaders.
        D3D12_CPU_DESCRIPTOR_HANDLE DepthSrv{};
        D3D12_CPU_DESCRIPTOR_HANDLE VelocitySrv{};
        bool                        VelocityAvailable = false;

        // Jitter of this frame's projection, in render pixels with +Y down.
        float JitterX = 0.0f;
        float JitterY = 0.0f;

        // Inverse(current view-projection) * previous view-projection, both
        // without jitter, in the engine's row-vector convention.
        DirectX::XMFLOAT4X4 ClipToPrevClip{};

        // The tonemapper's exposure for this frame (AgxTonemap.hlsl), so HDR
        // weighting sees the image as the player will.
        float Ev100 = 0.0f;
        float Ev100Min = 0.0f;
        float Ev100Max = 0.0f;
        float ExposureTrim = 0.0f;
        // Raw buffer holding the metered EV100 in its first float; null for manual.
        D3D12_CPU_DESCRIPTOR_HANDLE AutoExposureSrv{};
    };

    // Create or resize TAA resources. Output size equals input size for plain TAA.
    bool Initialize(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight);
    bool Initialize(UINT width, UINT height) { return Initialize(width, height, width, height); }

    // Release all GPU resources.
    void Shutdown();

    // Record the resolve. Leaves the colour input in PIXEL_SHADER_RESOURCE and
    // the output in PIXEL_SHADER_RESOURCE. Binds its own descriptor heap and
    // restores the shared one before returning.
    void Resolve(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs, TaaSettings& settings);

    // Halton(2,3) jitter for the frame, Unreal's sequence: r.TemporalAASamples
    // phases, lengthened by 1 / screen percentage^2 when upsampling. In render
    // pixels with +Y down, before JitterScale.
    static void GetJitterOffset(UINT frameIndex, int samples, float screenPercentage, float& outX, float& outY);

    // Ui-displayable handles of the current output (shared heap slots).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputUiSrvGpu[mCurrent]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()    const { return mOutputUiSrvCpu[mCurrent]; }
    ID3D12Resource*             GetOutputResource()  const { return mHistory[mCurrent].Get(); }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId[mCurrent]; }

    bool IsInitialized() const { return mIsInitialized; }
    bool IsUpsampling() const { return mOutputWidth != mInputWidth || mOutputHeight != mInputHeight; }
    UINT GetInputWidth() const { return mInputWidth; }
    UINT GetInputHeight() const { return mInputHeight; }
    UINT GetOutputWidth() const { return mOutputWidth; }
    UINT GetOutputHeight() const { return mOutputHeight; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    static constexpr UINT kFramesInFlight = 3;
    // t0 colour, t1 depth, t2 velocity, t3 history, t4 exposure, u0 output.
    static constexpr UINT kDescriptorsPerSet = 6;

    bool CreateRootSignature();
    bool EnsurePipeline(bool upsample, int quality);
    bool CreateTextures(UINT width, UINT height);

    // ---- constant buffer layout (must match TAAResolve.hlsl) --------
    struct TaaCbData
    {
        DirectX::XMFLOAT4X4 ClipToPrevClip;

        float InputSceneColorSize[4];
        int   InputMinPixelCoord[2];
        int   InputMaxPixelCoord[2];

        float OutputViewportSize[4];
        float HistoryBufferSize[4];
        float HistoryBufferUVMinMax[4];
        float ScreenPosToHistoryBufferUV[4];

        float ScreenPosAbsMax[2];
        float TemporalJitterPixels[2];

        float InputViewSize[4];
        float InputViewMin[2];
        float ScreenPercentage;
        float UpscaleFactor;

        float SampleWeights[12];
        float PlusWeights[8];

        float CurrentFrameWeight;
        int   bCameraCut;
        UINT  bVelocityAvailable;
        float HistoryPreExposureCorrection;

        float Ev100;
        float Ev100Min;
        float Ev100Max;
        float ExposureTrim;
        UINT  UseAutoExposure;
        float DynamicThresholdPixels;
        float Pad0[2];
    };
    static constexpr UINT64 kCbStride = (sizeof(TaaCbData) + 255ull) & ~255ull;

    // ---- pipeline ---------------------------------------------------
    // [upsample][quality]
    DX12Shader                                     mComputeShaders[2][4];
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    mPipelineStates[2][4];
    Microsoft::WRL::ComPtr<ID3D12RootSignature>    mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12Resource>         mConstantBuffer;
    std::uint8_t*                                  mMappedCb = nullptr;

    // ---- history textures (recreated on resize) ---------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistory[2];
    UINT mCurrent = 0;       // index of the texture holding the latest output
    bool mHistoryValid = false;

    // Shader-visible ring: kFramesInFlight sets of kDescriptorsPerSet.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT mComputeHeapStride = 0;
    UINT mFrameSlot = 0;

    // CPU-only heap: [0..1] history SRVs, [2..3] history UAVs, [4] null texture
    // SRV, [5] null raw-buffer SRV. Copied into the ring each frame.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mStagingHeap;

    // Shared-context-heap slots for the Ui display SRVs (allocated once, never freed).
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu[2]{};
    UiTextureID mOutputTextureId[2] = { UiTextureID_Invalid, UiTextureID_Invalid };
    bool mUiSlotsAllocated = false;

    UINT mInputWidth = 0;
    UINT mInputHeight = 0;
    UINT mOutputWidth = 0;
    UINT mOutputHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
