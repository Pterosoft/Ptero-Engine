#pragma once

#include "ChromaticAberrationSettings.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include "../QtUi/UiTypes.h"

#include <string>

// ChromaticAberrationRenderer
// Final stage of the post chain. Takes whatever the previous stage produced - tonemapped
// where AgX is on - and writes a channel-split copy, so it slots into the same
// input-selection chain bloom and the tonemapper already use.
class ChromaticAberrationRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    void Apply(
        ID3D12GraphicsCommandList*           commandList,
        ID3D12Resource*                      inputResource,
        D3D12_CPU_DESCRIPTOR_HANDLE          inputCpuSrv,
        const ChromaticAberrationSettings&   settings);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()    const { return mOutputUiSrvCpu; }
    ID3D12Resource*             GetOutputResource()  const { return mOutputTexture.Get(); }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    // Mirrors ChromaticAberrationConstants in ChromaticAberration.hlsl.
    struct alignas(256) ChromaticAberrationCbData
    {
        UINT  FrameWidth = 0;
        UINT  FrameHeight = 0;
        float Strength = 0.0f;
        float Falloff = 2.0f;

        int   SampleCount = 6;
        float CenterX = 0.5f;
        float CenterY = 0.5f;
        float Pad0 = 0.0f;
    };

    DX12Shader                                   mComputeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>       mConstantBuffer;
    void*                                        mMappedCb = nullptr;

    // Private 2-slot shader-visible heap:
    //   slot 0 - t0 input SRV (copied in each frame, since the stage feeding this one
    //            varies with which effects are enabled)
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
