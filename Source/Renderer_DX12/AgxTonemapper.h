#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "AgxTonemapSettings.h"

#include "../QtUi/UiTypes.h"

// AgxTonemapper performs an AgX-based tone-mapping pass as a compute shader.
//
// Input  – scene colour texture (R8G8B8A8_UNORM, PIXEL_SHADER_RESOURCE).
// Output – tonemapped texture (R8G8B8A8_UNORM, PIXEL_SHADER_RESOURCE).
//
// Call Initialize() once (or on resize), then Apply() each frame.
class AgxTonemapper
{
public:
    // Create or resize resources for the given resolution.
    bool Initialize(UINT width, UINT height);

    // Release all GPU resources.
    void Shutdown();

    // Run the AgX tonemap compute dispatch.
    // inputResource  – the scene colour render target (PIXEL_SHADER_RESOURCE state).
    // inputCpuSrv    – CPU descriptor handle for that resource.
    // settings       – current tonemap parameters.
    void Apply(
        ID3D12GraphicsCommandList*  commandList,
        ID3D12Resource*             inputResource,
        D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
        const AgxTonemapSettings&   settings);

    // Ui-displayable GPU handle of the tonemapped output (shared heap slot).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()   const { return mOutputUiSrvGpu; }
    // Needed by any later stage that copies this output's descriptor into its own heap,
    // the way every other post stage already exposes one.
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()   const { return mOutputUiSrvCpu; }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }
    ID3D12Resource*             GetOutputResource()  const { return mOutputTexture.Get(); }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    struct AgxGradeControlCb
    {
        float Total = 0.0f;
        float Red = 0.0f;
        float Green = 0.0f;
        float Blue = 0.0f;
        float Yellow = 0.0f;
        float Pad0 = 0.0f;
        float Pad1 = 0.0f;
        float Pad2 = 0.0f;
    };

    struct AgxGradeRegionCb
    {
        AgxGradeControlCb Contrast;
        AgxGradeControlCb Gamma;
        AgxGradeControlCb Gain;
        AgxGradeControlCb Saturation;
        AgxGradeControlCb Vibrance;
    };

    // Constant buffer layout – must match AgxTonemap.hlsl.
    struct alignas(256) AgxCbData
    {
        float Exposure;
        float Ev100Min;
        float Ev100Max;
        float ToeStrength;
        float ShoulderStrength;
        float Pad0[3]{};

        AgxGradeRegionCb Global;
        AgxGradeRegionCb Shadows;
        AgxGradeRegionCb Midtones;
        AgxGradeRegionCb Highlights;

        UINT  FrameWidth;
        UINT  FrameHeight;
        float Pad1[2]{};
    };

    // Pipeline
    DX12Shader                                  mComputeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    void*                                       mMappedCb = nullptr;

    // Private 2-slot shader-visible descriptor heap.
    //   slot 0 – t0: input SRV (refreshed via CopyDescriptors each frame)
    //   slot 1 – u0: output UAV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT                                         mComputeHeapStride = 0;

    // Textures (recreated on resize)
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;

    // Shared-context heap slot for Ui display
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID                 mOutputTextureId = UiTextureID_Invalid;
    bool                        mUiSlotAllocated = false;

    UINT mWidth  = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
