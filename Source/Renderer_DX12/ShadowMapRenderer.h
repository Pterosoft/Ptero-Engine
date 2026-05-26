#pragma once

// ShadowMapRenderer
// Owns the 2K shadow depth texture and the depth-only pipeline used for the
// sun shadow pass.  DX12SceneRenderer calls BeginShadowPass() to set up the
// render target and compute the light-space matrix, then calls
// EntityMeshRenderer::RenderDepthOnly() to actually draw the geometry, then
// calls EndShadowPass() to transition the texture back to SRV state.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <string>
#include <wrl/client.h>

// Resolution of the shadow map texture (power-of-two for clean filtering).
static constexpr UINT kShadowMapSize = 2048;
// Format used for the shadow depth buffer.
static constexpr DXGI_FORMAT kShadowDepthFormat    = DXGI_FORMAT_D32_FLOAT;
// R32_FLOAT view for sampling in the main pass.
static constexpr DXGI_FORMAT kShadowResourceFormat = DXGI_FORMAT_R32_FLOAT;
// Typeless base so we can create both a DSV and an SRV.
static constexpr DXGI_FORMAT kShadowTypelessFormat  = DXGI_FORMAT_R32_TYPELESS;

class ShadowMapRenderer
{
public:
    // Call once after the DX12 device is ready.
    bool Initialize();

    // Set up the shadow render target, compute the light-space matrix from the
    // current sun direction, clear the depth buffer, and bind the DSV.
    // After this call, issue geometry draw calls (via EntityMeshRenderer::RenderDepthOnly).
    // sceneBoundRadius controls the size of the orthographic frustum.
    void BeginShadowPass(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMFLOAT3&   sunDir,
        float                       sceneBoundRadius);

    // Transition the shadow texture back to SRV state for use in the main pass.
    void EndShadowPass(ID3D12GraphicsCommandList* commandList);

    // GPU handle of the shadow depth SRV — bind this at t1 in the main pass.
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowSrvGpuHandle() const { return mShadowSrvGpu; }

    // Light-space view-projection computed during the last BeginShadowPass().
    // Pre-transposed (row-major) so the shader receives it in column-major order.
    const DirectX::XMFLOAT4X4& GetLightViewProjection() const { return mLightViewProjection; }

    // Root signature and pipeline state to pass to RenderDepthOnly().
    ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
    ID3D12PipelineState* GetPipelineState()  const { return mPipelineState.Get(); }

    bool IsInitialized() const { return mIsInitialized; }
    void Shutdown();

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreateResources();
    bool CreatePipeline();

    Microsoft::WRL::ComPtr<ID3D12Resource>       mShadowDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE                  mDsvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE                  mShadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE                  mShadowSrvGpu{};

    DX12Shader                                   mVertexShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineState;

    DirectX::XMFLOAT4X4 mLightViewProjection{};

    bool        mIsInitialized = false;
    std::string mLastError;
};
