#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "AarSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>

// AAR - Analytic-Aware Reconstruction (AarSettings.h, AnalyticAwareReconstruction.hlsl).
//
// Two fullscreen passes per frame:
//   gate         render size: keeps the G-Buffer triangle edges that bound a real
//                geometric break (R16G16B16A16_UINT, same packing as the G-Buffer).
//   reconstruct  output size: the upscaled HDR image (R16G16B16A16_FLOAT).
//
// Both textures rest in PIXEL_SHADER_RESOURCE, like the other upscalers' outputs.
class AarRenderer
{
public:
    // Creates the pipelines on first use and (re)creates the textures for these sizes.
    // Waits for the GPU when it has to replace textures.
    bool EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight);
    // Frees the textures while AAR is off; the pipelines and descriptor slots stay.
    void ReleaseResources();
    void Shutdown();

    // sceneColorSrv, normalDepthSrv and edgeSrv are render-size SRVs in the shared heap,
    // all in PIXEL_SHADER_RESOURCE. Leaves its own render target and viewport bound.
    void Dispatch(
        ID3D12GraphicsCommandList*  commandList,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE normalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE edgeSrv,
        float                       projectionZ33,
        const AarSettings&          settings);

    bool HasOutput() const { return mOutputTexture != nullptr; }
    UINT GetOutputWidth() const { return mOutputWidth; }
    UINT GetOutputHeight() const { return mOutputHeight; }
    UINT GetRenderWidth() const { return mRenderWidth; }
    UINT GetRenderHeight() const { return mRenderHeight; }

    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv() const { return mOutputSrvCpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputSrvGpu; }
    UiTextureID GetOutputTextureId() const { return static_cast<UiTextureID>(mOutputSrvGpu.ptr); }

    // The render size AAR wants for an output size in the given mode.
    static void QueryRenderSize(UINT outputWidth, UINT outputHeight, int mode, UINT& renderWidth, UINT& renderHeight);

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipelines();
    bool CreateTextures(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight);

    // Root constants, b0. Must match AarConstants in AnalyticAwareReconstruction.hlsl.
    struct Constants
    {
        UINT  RenderWidth;
        UINT  RenderHeight;
        UINT  OutputWidth;
        UINT  OutputHeight;
        float DepthThreshold;
        float NormalThreshold;
        float ProjectionZ33;
        float DepthSigma;
        float NormalPower;
        UINT  DebugView;
        float Pad[2];
    };
    static_assert(sizeof(Constants) == 48, "AarConstants is twelve 32-bit values.");

    DX12Shader mVertexShader;
    DX12Shader mGateShader;
    DX12Shader mReconstructShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGatePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mReconstructPipeline;

    Microsoft::WRL::ComPtr<ID3D12Resource> mGatedEdges;
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;   // 0 gated edges, 1 output
    D3D12_CPU_DESCRIPTOR_HANDLE mGatedEdgesRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputRtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE mGatedEdgesSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mGatedEdgesSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};

    UINT mRenderWidth = 0;
    UINT mRenderHeight = 0;
    UINT mOutputWidth = 0;
    UINT mOutputHeight = 0;
    std::string mLastError;
};
