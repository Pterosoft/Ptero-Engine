#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "SharpenSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

// ImageSharpenRenderer applies a standalone unsharp-mask pass to the current
// post-AA image. It intentionally lives outside TAA so sharpening can be used
// with TAA, SMAA, DLSS, or no anti-aliasing at all.
class ImageSharpenRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    void Apply(
        ID3D12GraphicsCommandList*  commandList,
        ID3D12Resource*             inputResource,
        D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
        const SharpenSettings&      settings);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()   const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()   const { return mOutputUiSrvCpu; }
    ID3D12Resource*             GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    struct alignas(256) SharpenCbData
    {
        float Strength = 0.0f;
        UINT  FrameWidth = 0;
        UINT  FrameHeight = 0;
        float Pad0 = 0.0f;
    };

    DX12Shader                                   mComputeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    void*                                       mMappedCb = nullptr;

    // Private 2-slot shader-visible heap:
    //   slot 0 - t0 input SRV (copied each frame)
    //   slot 1 - u0 output UAV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT                                         mComputeHeapStride = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID                 mOutputTextureId = UiTextureID_Invalid;
    bool                        mUiSlotAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
