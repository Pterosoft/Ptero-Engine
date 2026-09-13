#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "SsrSettings.h"

#include <DirectXMath.h>
#include <string>

// SsrRenderer
// Screen-space reflections as a single compute dispatch.
//
// The pass reads the finished scene colour plus the G-Buffer, writes the composited
// result into its own target, and copies that back over the scene colour. Compositing in
// place is what keeps this insertable: every later pass (TAA, SMAA, bloom, tonemap) goes
// on reading the same scene colour texture it did before, so none of the "which texture
// is current" plumbing downstream has to learn about SSR.
class SsrRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    // sceneColorResource is both an input (sampled) and the destination the result is
    // copied back into. It must be in PIXEL_SHADER_RESOURCE state on entry, and is left
    // that way on exit. The depth SRV must already be readable by a compute shader.
    void Dispatch(
        ID3D12GraphicsCommandList*   commandList,
        ID3D12Resource*              sceneColorResource,
        D3D12_GPU_DESCRIPTOR_HANDLE  sceneColorSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  depthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  normalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  materialSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  albedoSrv,
        const SsrSettings&           settings,
        const DirectX::XMFLOAT4X4&   viewProjection,      // already transposed for HLSL
        const DirectX::XMFLOAT4X4&   inverseViewProjection,
        const DirectX::XMFLOAT3&     cameraPosition);

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    // Mirrors SsrConstants in Ssr.hlsl.
    struct alignas(256) SsrCbData
    {
        DirectX::XMFLOAT4X4 ViewProj{};
        DirectX::XMFLOAT4X4 InvViewProj{};
        DirectX::XMFLOAT3   CameraPos{};   float Intensity = 1.0f;
        UINT  FrameWidth = 0;              UINT  FrameHeight = 0;
        int   MaxSteps = 48;               float StepSize = 0.25f;
        float StepGrowth = 1.05f;          float Thickness = 0.5f;
        float MaxRoughness = 0.6f;         int   RefineSteps = 6;
        float MaxDistance = 60.0f;         float EdgeFadeStart = 0.85f;
        int   DebugView = 0;               float Pad0 = 0.0f;
    };

    DX12Shader                                   mComputeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>       mConstantBuffer;
    void*                                        mMappedCb = nullptr;

    // Output lives in the shared shader-visible heap so the SRV inputs can be bound
    // straight from the handles their owners already hold, with no per-frame copying.
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu{};
    bool                        mUavSlotAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
