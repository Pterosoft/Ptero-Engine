#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TimeOfDaySettings.h"
#include "HosekWilkieSky.h"

#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>

// SkyRenderer
// Renders a fullscreen sky background (sky gradient + sun disc) using
// colours derived from the Hosek-Wilkie atmospheric model.
// Must be drawn BEFORE the scene geometry so the depth test hides it.
class SkyRenderer
{
public:
    bool Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    void Shutdown();

    // Draw the sky.  Call this while the scene RTV+DSV are bound, BEFORE
    // opaque geometry, so the sky only shows through untouched pixels.
    void Render(
        ID3D12GraphicsCommandList* commandList,
        const HosekWilkieResult&   hosekResult,
        const TimeOfDaySettings&   settings,
        const DirectX::XMMATRIX&   projectionMatrix,
        const DirectX::XMMATRIX&   viewMatrix,
        UINT                       viewportWidth,
        UINT                       viewportHeight);

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // Constant buffer layout — must match SkyPass.hlsl.
    struct alignas(256) SkyConstants
    {
        DirectX::XMFLOAT3 SkyZenithColor;   float Pad0;
        DirectX::XMFLOAT3 SkyHorizonColor;  float Pad1;
        DirectX::XMFLOAT3 SunDirectionVS;   float Pad2;
        DirectX::XMFLOAT3 SunColor;
        float              SunDiscHalfAngleCos;
        DirectX::XMFLOAT4X4 InvProj;
    };

    DX12Shader  mVertexShader;
    DX12Shader  mPixelShader;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    void* mMappedCb = nullptr;

    bool        mIsInitialized = false;
    std::string mLastError;
};
