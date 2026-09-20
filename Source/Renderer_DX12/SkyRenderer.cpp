#include "pch.h"
#include "SkyRenderer.h"

#include "d3dx12.h"

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
}

// Solar angular radius ≈ 0.265° → half-angle cos ≈ cos(0.00463 rad).
// We double it for a slightly more visible disc in the editor viewport.
static constexpr float kSunDiscHalfAngleDeg = 0.8f;
static constexpr float kSunDiscHalfAngleRad = kSunDiscHalfAngleDeg * (3.14159265f / 180.0f);

bool SkyRenderer::Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
{
    // Kept in the signature so the caller still states the scene's depth format, but
    // the sky pass binds no depth target - see the pipeline state below.
    UNREFERENCED_PARAMETER(depthFormat);

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    mLastError.clear();

    try
    {
        // ---- shaders ----
        const ShaderCompileRequest vsReq{ L"Shaders\\SkyPass.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
        const ShaderCompileRequest psReq{ L"Shaders\\SkyPass.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel  };
        if (!mVertexShader.Compile(vsReq))
        {
            mLastError = std::string("SkyRenderer VS: ") + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
            return false;
        }
        if (!mPixelShader.Compile(psReq))
        {
            mLastError = std::string("SkyRenderer PS: ") + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
            return false;
        }

        // ---- root signature: single inline CBV at b0 ----
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.Descriptor.RegisterSpace  = 0;
        param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters   = &param;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> blob, errors;
        DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
        DX12_THROW_IF_FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

        // ---- PSO: fullscreen triangle, no vertex input, depth read-only ----
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = mRootSignature.Get();
        psoDesc.VS                    = mVertexShader.GetBytecode();
        psoDesc.PS                    = mPixelShader.GetBytecode();
        psoDesc.SampleMask            = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets      = 1;
        psoDesc.RTVFormats[0]         = colorFormat;
        // No depth target: the sky pass binds colour only. D3D12 requires UNKNOWN here
        // when no DSV is bound, and GPU-based validation rejects the draw outright -
        // "the depth stencil format does not match that specified by the current
        // pipeline state" - which is undefined behaviour, not a warning.
        psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        psoDesc.SampleDesc.Count      = 1;

        // No vertex input — the VS generates the fullscreen triangle procedurally.
        psoDesc.InputLayout.NumElements        = 0;
        psoDesc.InputLayout.pInputElementDescs = nullptr;

        // No back-face culling; we're rendering a full-screen triangle.
        psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable       = TRUE;

        // Depth off, to match the pass. This used to ask for a LESS_EQUAL test so the
        // sky only reached pixels with no geometry, but the pass binds no depth buffer,
        // so that test has never actually run: the sky covers the whole target and the
        // lighting pass composites geometry over it afterwards. Declaring a test that
        // cannot happen only made the pipeline state disagree with what was bound.
        psoDesc.DepthStencilState.DepthEnable    = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.StencilEnable  = FALSE;

        // Normal opaque blend.
        D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
        rtBlend.BlendEnable           = FALSE;
        rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.BlendState.RenderTarget[0] = rtBlend;

        DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));

        // ---- constant buffer (persistently mapped upload heap) ----
        const UINT64 cbSize = (sizeof(SkyConstants) + 255ull) & ~255ull;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask  = 1;
        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width            = cbSize;
        cbDesc.Height           = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels        = 1;
        cbDesc.Format           = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstantBuffer)));
        DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mMappedCb));

        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SkyRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void SkyRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedCb = nullptr;
    mConstantBuffer.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

void SkyRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const HosekWilkieResult&   hosekResult,
    const TimeOfDaySettings&   settings,
    const DirectX::XMMATRIX&   projectionMatrix,
    const DirectX::XMMATRIX&   viewMatrix,
    UINT                       viewportWidth,
    UINT                       viewportHeight)
{
    if (!mIsInitialized || !commandList) return;

    // Scale Hosek-Wilkie sky/sun colours by the user-specified lux values,
    // mapped to a display-friendly [0,1] HDR scale factor.
    // We normalise so that the reference sky intensity (20 000 lx) maps to ~1.0.
    constexpr float kRefSkyLux = 20000.0f;
    constexpr float kRefSunLux = 100000.0f;

    // Resolve sky colour (Hosek or override).
    float skyR, skyG, skyB;
    if (settings.OverrideSkyColor)
    {
        skyR = settings.SkyColorR;
        skyG = settings.SkyColorG;
        skyB = settings.SkyColorB;
    }
    else
    {
        skyR = hosekResult.SkyR;
        skyG = hosekResult.SkyG;
        skyB = hosekResult.SkyB;
    }
    const float skyScale = settings.SkyIntensityLux / kRefSkyLux;
    skyR *= skyScale; skyG *= skyScale; skyB *= skyScale;

    // Horizon colour is a warmer, slightly desaturated version of the zenith.
    const float hR = skyR * 1.2f + 0.05f * skyScale;
    const float hG = skyG * 1.1f + 0.03f * skyScale;
    const float hB = skyB * 0.8f + 0.02f * skyScale;

    // Resolve sun colour.
    float sunR, sunG, sunB;
    if (settings.OverrideSunColor)
    {
        sunR = settings.SunColorR;
        sunG = settings.SunColorG;
        sunB = settings.SunColorB;
    }
    else
    {
        sunR = hosekResult.SunR;
        sunG = hosekResult.SunG;
        sunB = hosekResult.SunB;
    }
    const float sunScale = settings.SunIntensityLux / kRefSunLux;
    sunR *= sunScale; sunG *= sunScale; sunB *= sunScale;

    // HosekWilkieResult stores the incoming light direction (from sun toward the scene).
    // The sky pass needs the opposite direction so the sun disc appears where the sun is.
    const XMVECTOR sunWorldDir = XMVectorSet(
        -hosekResult.SunDirX, -hosekResult.SunDirY, -hosekResult.SunDirZ, 0.0f);
    const XMVECTOR sunViewDir = XMVector3TransformNormal(sunWorldDir, viewMatrix);
    XMFLOAT3 sunVS;
    XMStoreFloat3(&sunVS, XMVector3Normalize(sunViewDir));

    // Build inverse projection for ray reconstruction in the PS.
    XMFLOAT4X4 invProjF;
    XMStoreFloat4x4(&invProjF, XMMatrixTranspose(XMMatrixInverse(nullptr, projectionMatrix)));

    // Fill the constant buffer.
    SkyConstants cb{};
    cb.SkyZenithColor       = { skyR,  skyG,  skyB  };
    cb.SkyHorizonColor      = { hR, hG, hB };
    cb.SunDirectionVS       = sunVS;
    cb.SunColor             = { sunR, sunG, sunB };
    cb.SunDiscHalfAngleCos  = std::cos(kSunDiscHalfAngleRad);
    cb.InvProj              = invProjF;
    std::memcpy(mMappedCb, &cb, sizeof(cb));

    // Draw.
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetGraphicsRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0); // fullscreen triangle
}
