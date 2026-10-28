#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "AnalyticEdgeAASettings.h"

#include <string>

// The pieces of Analytic Edge AA every G-Buffer writer shares.
namespace AnalyticEdge
{
    // Format of the G-Buffer edge target (SV_Target3): the pixel's triangle as three
    // packed edges, one per channel (AnalyticEdge.hlsli). Every geometry-pass pipeline
    // declares it, whether or not its shader writes real edges.
    constexpr DXGI_FORMAT kGBufferFormat = DXGI_FORMAT_R16G16B16A16_UINT;

    // Whether the GPU supports SM 6.1 barycentrics, which the edges are derived from.
    bool IsSupported();

    // A G-Buffer pixel shader request: compiled to write edges (ps_6_1,
    // PTERO_ANALYTIC_EDGES=1) when IsSupported(), else as the plain ps_5_0 shader,
    // which writes "no edge".
    ShaderCompileRequest MakeGBufferPixelShaderRequest(const wchar_t* filePath, const wchar_t* entryPoint);
}

// The resolve: rewrites the edge pixels of the lit scene colour with the exact area
// each surface covers (Data/Shaders/AnalyticEdgeAA.hlsl).
class AnalyticEdgeAARenderer
{
public:
    bool Initialize(UINT width, UINT height, DXGI_FORMAT sceneColorFormat);
    // Re-creates the scene copy for a new render size. Waits for the GPU.
    bool Resize(UINT width, UINT height);
    void Shutdown();

    // sceneColor must be in RENDER_TARGET state and is left in it, with sceneRtv bound
    // and a full viewport, as the deferred resolve leaves things. normalDepthSrv and
    // edgeSrv are the G-Buffer normal and edge targets, in PIXEL_SHADER_RESOURCE.
    // projectionZ33 is the projection matrix's _33.
    void Apply(
        ID3D12GraphicsCommandList*  commandList,
        ID3D12Resource*             sceneColor,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
        D3D12_GPU_DESCRIPTOR_HANDLE normalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE edgeSrv,
        float                       projectionZ33,
        const AnalyticEdgeAASettings& settings);

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateSceneCopy(UINT width, UINT height);

    // Root constants, b0. Must match AnalyticEdgeConstants in AnalyticEdgeAA.hlsl.
    struct Constants
    {
        UINT  Width;
        UINT  Height;
        float DepthThreshold;
        float NormalThreshold;
        float ProjectionZ33;
        UINT  DebugView;
        float Pad[2];
    };
    static_assert(sizeof(Constants) == 32, "AnalyticEdgeConstants is eight 32-bit values.");

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    // Copy of the scene colour the resolve reads, so neighbours stay unfiltered while
    // the pass writes the scene colour itself. Rests in PIXEL_SHADER_RESOURCE.
    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneCopy;
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneCopySrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mSceneCopySrvGpu{};

    DXGI_FORMAT mSceneColorFormat = DXGI_FORMAT_UNKNOWN;
    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
