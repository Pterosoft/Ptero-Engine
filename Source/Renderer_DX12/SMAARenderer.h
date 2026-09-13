#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "SMAASettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

class SMAARenderer
{
public:
    bool Initialize(UINT width, UINT height, ID3D12GraphicsCommandList* uploadCommandList = nullptr);
    void Shutdown();

    void Apply(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* sourceResource,
        D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuSrv,
        const SMAASettings& settings);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv() const { return mOutputUiSrvCpu; }
    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID GetOutputTextureId() const { return mOutputTextureId; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetEdgesGpuSrv() const { return mEdgesDebugSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetBlendGpuSrv() const { return mBlendDebugSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetDebugGpuSrv(int debugView) const
    {
        if (debugView == 1)
            return mEdgesDebugSrvGpu;
        if (debugView == 2)
            return mBlendDebugSrvGpu;
        return mOutputUiSrvGpu;
    }
    UiTextureID GetDebugTextureId(int debugView) const
    {
        const D3D12_GPU_DESCRIPTOR_HANDLE handle = GetDebugGpuSrv(debugView);
        return static_cast<UiTextureID>(handle.ptr);
    }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);
    bool CreateLookupTextures(ID3D12GraphicsCommandList* uploadCommandList);

    struct alignas(256) SMAACbData
    {
        float PixelSizeX;
        float PixelSizeY;
        float EdgeThreshold;
        float MaxSearchSteps;
        float MaxSearchStepsDiag;
        float CornerRounding;
        UINT FrameWidth;
        UINT FrameHeight;
        UINT DebugView;
    };

    enum HeapSlot : UINT
    {
        SlotSourceSrv = 0,
        SlotEdgesSrv,
        SlotBlendSrv,
        SlotAreaSrv,
        SlotSearchSrv,
        SlotEdgesUav,
        SlotBlendUav,
        SlotOutputUav,
        SlotCount
    };

    DX12Shader mEdgesShader;
    DX12Shader mWeightsShader;
    DX12Shader mResolveShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEdgesPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWeightsPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mResolvePso;
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    void* mMappedCb = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mEdgesTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBlendTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAreaTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSearchTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAreaTextureUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSearchTextureUpload;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT mComputeHeapStride = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mEdgesDebugSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mEdgesDebugSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mBlendDebugSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mBlendDebugSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mUiSlotAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
