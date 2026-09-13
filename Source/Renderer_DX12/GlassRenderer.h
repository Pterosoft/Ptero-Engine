#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

class GlassRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    bool EnsureSize(UINT width, UINT height);
    void Shutdown();

    void Dispatch(
        ID3D12GraphicsCommandList4* commandList,
        ID3D12Resource* sceneColorResource,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSrv,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRtvCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferMaterialSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE tlasSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE vertexSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE indexSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE instanceInfoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE materialRangeSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE baseTextureTableSrv,
        const float invViewProj[16],
        const float viewProj[16],
        const float cameraPos[3],
        uint32_t frameIndex,
        bool tlasReady);

    bool IsInitialized() const { return mIsInitialized; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    struct alignas(256) GlassConstants
    {
        uint32_t FrameWidth = 0;
        uint32_t FrameHeight = 0;
        uint32_t FrameIndex = 0;
        uint32_t TlasReady = 0;
        float InvViewProj[16]{};
        float ViewProj[16]{};
        float CameraPos[3]{};
        float MaxRayDistance = 10000.0f;
        float ScreenTraceStride = 0.12f;
        float ScreenTraceThickness = 0.01f;
        uint32_t ScreenTraceSteps = 28;
        float _Pad0 = 0.0f;
    };
    static_assert(sizeof(GlassConstants) == 256);

    bool CreatePipeline();
    bool CreateResolutionResources(UINT width, UINT height);
    bool CreateDescriptors();

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    DX12Shader mComputeShader;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    GlassConstants* mMappedConstants = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu{};
    bool mDescriptorsAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
