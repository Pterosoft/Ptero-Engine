#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TaaSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

// TAARenderer owns all GPU resources required for the TAA temporal resolve pass.
//
// Design notes:
//   - A private 3-slot shader-visible descriptor heap is used so the SRV table
//     layout is always predictable and independent of the shared context heap.
//   - The Ui-visible output SRV is kept in the shared context heap so Ui
//     can display it using its normal draw call without switching heaps.
//   - History and output textures are re-created on resize; the pipeline and
//     constant buffer survive resizes.
class TAARenderer
{
public:
    // Create or resize TAA resources for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Release all GPU resources.
    void Shutdown();

    // Run the TAA resolve compute dispatch.
    // currentFrameColorResource – the scene colour render target (already in
    //   PIXEL_SHADER_RESOURCE state when this is called).
    // currentFrameColorCpuSrv   – CPU descriptor handle for that resource.
    void Resolve(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            currentFrameColorResource,
        D3D12_CPU_DESCRIPTOR_HANDLE currentFrameColorCpuSrv,
        TaaSettings&               settings);

    // Ui-displayable GPU handle of the resolved output (shared heap slot).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()    const { return mOutputUiSrvCpu; }
    ID3D12Resource*             GetOutputResource()  const { return mOutputTexture.Get(); }
    UiTextureID                 GetOutputTextureId()  const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    // ---- constant buffer layout (must match TAAResolve.hlsl) --------
    struct alignas(256) TaaCbData
    {
        float BlendFactor;
        UINT  FrameWidth;
        UINT  FrameHeight;
        float Pad0;
    };

    // ---- pipeline ---------------------------------------------------
    DX12Shader                                     mComputeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>    mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>    mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>         mConstantBuffer;
    void*                                          mMappedCb    = nullptr;

    // ---- per-frame textures (recreated on resize) -------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistoryTexture; // previous resolved frame
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;  // current resolved output

    // Private 3-slot shader-visible heap used by the compute dispatch.
    //   slot 0  –  t0: current frame SRV  (refreshed each frame via CopyDescriptors)
    //   slot 1  –  t1: history SRV        (refreshed on texture resize)
    //   slot 2  –  u0: output UAV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT mComputeHeapStride = 0;

    // Non-shader-visible UAV for ClearUnorderedAccessViewFloat (required by DX12).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mNsUavHeap;

    // Shared-context-heap slot for the Ui display SRV (allocated once, never freed).
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mUiSlotAllocated = false;

    UINT mWidth  = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
