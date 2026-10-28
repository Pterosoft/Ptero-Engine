#pragma once

#include "DX12Helper.h"
#include "XessSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

// Intel XeSS super resolution (XeSS-SR 3). Runs on any GPU with Shader Model 6.4;
// on Intel Arc it uses the XMX path.
//
// Shaped like FsrRenderer so the scene renderer can treat the upscalers alike:
// it renders at GetRenderWidth/Height, hands over colour, depth and motion
// vectors, and reads an output-resolution HDR image back.
class XessRenderer
{
public:
    struct FrameData
    {
        // Pixel jitter in XeSS's convention: +X right, +Y down, in [-0.5, 0.5].
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        bool  Reset = false;
    };

    // Loads libxess.dll and creates the context, which is what tells whether the
    // GPU can run XeSS. The output texture is only created by EnsureSize.
    bool Initialize();
    void Shutdown();

    // Initialises XeSS for the output size and quality mode and creates the
    // output texture. Waits for the GPU when something it may still be reading
    // has to be released.
    bool EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight, const XessSettings& settings);
    // Frees the output texture and drops the initialisation while XeSS is off.
    void ReleaseResources();

    bool QueryRenderSize(UINT outputWidth, UINT outputHeight, const XessSettings& settings,
                         UINT& outRenderWidth, UINT& outRenderHeight);

    // Halton(2,3) jitter for the frame in XeSS's convention (see FrameData).
    // The sequence grows with the upscale ratio so every output pixel is covered.
    static void GetJitterOffset(UINT frameIndex, UINT renderWidth, UINT outputWidth, float& outX, float& outY);

    // Inputs must be in D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE. The output is
    // left in PIXEL_SHADER_RESOURCE. XeSS binds its own descriptor heap, so the
    // caller must rebind the shared one afterwards.
    void Evaluate(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* inputColor,
        ID3D12Resource* depth,
        ID3D12Resource* motionVectors,
        const FrameData& frameData,
        XessSettings& settings);

    bool IsAvailable() const { return mContext != nullptr; }
    bool HasOutput() const { return mInitialized && mOutputTexture != nullptr; }

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv() const { return mOutputSrvCpu; }
    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID GetOutputTextureId() const { return mOutputTextureId; }

    UINT GetRenderWidth() const { return mRenderWidth; }
    UINT GetRenderHeight() const { return mRenderHeight; }
    UINT GetOutputWidth() const { return mOutputWidth; }
    UINT GetOutputHeight() const { return mOutputHeight; }
    // The xess_quality_settings_t it is initialised for, or -1.
    int  GetInitializedQuality() const { return mInitialized ? mInitializedQuality : -1; }

    const std::string& GetVersionName() const { return mVersionName; }
    const char* GetLastErrorMessage() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    // The xess_quality_settings_t for a settings Mode index.
    static int QualityForMode(int mode);

private:
    bool InitializeForOutput(UINT outputWidth, UINT outputHeight, int quality);
    bool CreateOutputTexture(UINT outputWidth, UINT outputHeight);

    void* mContext = nullptr; // xess_context_handle_t
    bool  mInitialized = false;
    int   mInitializedQuality = -1;

    UINT mRenderWidth = 0;
    UINT mRenderHeight = 0;
    UINT mOutputWidth = 0;
    UINT mOutputHeight = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    D3D12_RESOURCE_STATES mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mOutputSrvAllocated = false;

    std::string mVersionName;
    std::string mLastError;
};
