#pragma once

// XeGtaoRenderer.h
// Screen-space ambient occlusion using Intel's XeGTAO algorithm.
//
// Pass sequence each frame:
//   Pass 0  PrefilterDepths  – build 5-MIP viewspace depth pyramid
//   Pass 1  MainPass         – per-pixel GTAO occlusion term + edges
//   Pass 2+ Denoise          – 1 or 2 spatial denoise passes
//   PassN   ToFloat          – convert packed R8 AO to R8_UNORM float SRV

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "GtaoSettings.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class XeGtaoRenderer
{
public:
    XeGtaoRenderer()  = default;
    ~XeGtaoRenderer() { Shutdown(); }

    bool Initialize(UINT width, UINT height);
    bool EnsureSize(UINT width, UINT height);
    void Shutdown();

    // Dispatch all XeGTAO passes for this frame.
    //   commandList     – open compute-capable command list
    //   gbufferNormalSrv– SRV for GBuffer RT1 (oct-normal RG, depth B)
    //   sceneDepthSrv   – SRV for scene depth R32_FLOAT
    //   projMatrix[16]  – row-major projection matrix
    //   worldToView[16] – row-major world-to-view matrix
    //   frameIndex      – used for TAA noise cycling
    void Dispatch(
        ID3D12GraphicsCommandList*  commandList,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
        const GtaoSettings&         settings,
        const float                 projMatrix[16],
        const float                 worldToView[16],
        UINT                        frameIndex);

    // Final R8_UNORM AO texture SRV (valid after first Dispatch).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv()  const { return mOutputSrvGpu; }

    bool        IsInitialized()  const { return mIsInitialized; }
    bool        HasInitFailed()  const { return mInitFailed; }
    const char* GetLastError()   const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignatures();
    bool CreatePipelines();
    bool CreateResolutionBuffers(UINT width, UINT height);
    bool CreateDescriptors();

    bool mIsInitialized = false;
    bool mInitFailed    = false;
    std::string mLastError;

    UINT mWidth  = 0;
    UINT mHeight = 0;

    // Root signatures
    ComPtr<ID3D12RootSignature> mRsPrefilter;  // prefilter depths
    ComPtr<ID3D12RootSignature> mRsMainPass;   // main GTAO pass
    ComPtr<ID3D12RootSignature> mRsDenoise;    // denoise pass
    ComPtr<ID3D12RootSignature> mRsToFloat;    // packed → float

    // Pipeline state objects
    ComPtr<ID3D12PipelineState> mPsoPrefilter;
    ComPtr<ID3D12PipelineState> mPsoMainPassLow;
    ComPtr<ID3D12PipelineState> mPsoMainPassMedium;
    ComPtr<ID3D12PipelineState> mPsoMainPassHigh;
    ComPtr<ID3D12PipelineState> mPsoMainPassUltra;
    ComPtr<ID3D12PipelineState> mPsoDenoiseFirst;  // finalApply=false
    ComPtr<ID3D12PipelineState> mPsoDenoiseSecond; // finalApply=true (last pass)
    ComPtr<ID3D12PipelineState> mPsoToFloat;

    // GTAOConstants upload buffer (b0)
    ComPtr<ID3D12Resource> mConstantBuffer;
    void*                  mMappedCB = nullptr;

    // Viewspace depth MIP pyramid – single R32_FLOAT texture with 5 hardware mip levels.
    // PrefilterDepths writes each mip via per-mip UAVs; MainPass samples all mips via one SRV.
    ComPtr<ID3D12Resource> mViewspaceDepth;
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthMipUavCpu[5]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthMipUavGpu[5]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthAllMipsSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthAllMipsSrvGpu{};

    // Working AO term (R8_UINT packed)
    ComPtr<ID3D12Resource> mWorkingAO[2]; // ping-pong for denoise
    D3D12_CPU_DESCRIPTOR_HANDLE mWorkingAOUavCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mWorkingAOUavGpu[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mWorkingAOSrvCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mWorkingAOSrvGpu[2]{};

    // Working edges (R8_UNORM)
    ComPtr<ID3D12Resource> mWorkingEdges;
    D3D12_CPU_DESCRIPTOR_HANDLE mEdgesUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mEdgesUavGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mEdgesSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mEdgesSrvGpu{};

    // Final output (R8_UNORM)
    ComPtr<ID3D12Resource> mOutputAO;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};
    // Tracks whether the output texture is currently in ALL_SHADER_RESOURCE (true) or UAV (false).
    bool mOutputInSrvState = false;

    // World-to-view constant buffer (bound as b1 in MainPass)
    ComPtr<ID3D12Resource> mViewCB;
    void*                  mMappedViewCB = nullptr;
};
