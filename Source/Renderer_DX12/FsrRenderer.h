#pragma once

#include "DX12Helper.h"
#include "FsrSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

// AMD FSR upscaling through the FidelityFX API. The loader picks the newest
// provider the GPU supports: FSR 4 (ML) on RDNA 4, FSR 3.1 elsewhere.
//
// Shaped like DlssRenderer so the scene renderer can treat the two alike: it
// renders at GetRenderWidth/Height, hands over colour, depth and motion vectors,
// and reads an output-resolution HDR image back.
class FsrRenderer
{
public:
    struct FrameData
    {
        // Pixel jitter in FSR's convention: +X right, +Y down.
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        float FrameTimeDeltaMs = 16.6f;
        float NearPlane = 0.1f;
        float FarPlane = 1000.0f;
        float FovY = 0.785398f;
        bool  Reset = false;
    };

    // Loads the API and checks that an upscaler provider exists. Cheap; the
    // context and output texture are only created by EnsureSize.
    bool Initialize();
    void Shutdown();

    // Creates or resizes the context and output texture. Waits for the GPU when
    // something it may still be reading has to be released.
    bool EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight);
    // Frees the context and output while FSR is switched off.
    void ReleaseResources();

    bool QueryRenderSize(UINT outputWidth, UINT outputHeight, const FsrSettings& settings,
                         UINT& outRenderWidth, UINT& outRenderHeight);

    // Recommended jitter for the frame, in FSR's convention (see FrameData).
    void GetJitterOffset(UINT frameIndex, UINT renderWidth, UINT outputWidth, float& outX, float& outY);

    // Inputs must be in D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE. The output is
    // left in PIXEL_SHADER_RESOURCE.
    void Evaluate(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* inputColor,
        ID3D12Resource* depth,
        ID3D12Resource* motionVectors,
        const FrameData& frameData,
        FsrSettings& settings);

    bool IsAvailable() const { return mAvailable; }
    bool HasOutput() const { return mContext != nullptr && mOutputTexture != nullptr; }

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv() const { return mOutputSrvCpu; }
    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID GetOutputTextureId() const { return mOutputTextureId; }

    UINT GetRenderWidth() const { return mRenderWidth; }
    UINT GetRenderHeight() const { return mRenderHeight; }
    UINT GetOutputWidth() const { return mOutputWidth; }
    UINT GetOutputHeight() const { return mOutputHeight; }

    const std::string& GetVersionName() const { return mVersionName; }
    const char* GetLastErrorMessage() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateContext(UINT outputWidth, UINT outputHeight);
    void DestroyContext();
    bool CreateOutputTexture(UINT outputWidth, UINT outputHeight);

    void* mContext = nullptr; // ffxContext
    bool  mAvailable = false;

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
