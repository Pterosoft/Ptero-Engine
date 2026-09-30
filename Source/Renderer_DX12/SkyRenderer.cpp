#include "pch.h"
#include "SkyRenderer.h"

#include "d3dx12.h"
#include "System/DataFiles.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstddef>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
}

// Solar angular radius ≈ 0.265° → half-angle cos ≈ cos(0.00463 rad).
// We double it for a slightly more visible disc in the editor viewport.
static constexpr float kSunDiscHalfAngleDeg = 0.8f;
static constexpr float kSunDiscHalfAngleRad = kSunDiscHalfAngleDeg * (3.14159265f / 180.0f);

// The moon is drawn at about the same inflated size as the sun, scaled by
// TimeOfDaySettings::MoonSize.
static constexpr float kMoonDiscHalfAngleDeg = 0.9f;
static constexpr float kMoonDiscHalfAngleRad = kMoonDiscHalfAngleDeg * (3.14159265f / 180.0f);

void SkyRenderer::LoadTextures()
{
    mTexturesRequested = true;

    // NASA's LROC map for the moon (public domain), Solar System Scope's sun and
    // star map (CC BY 4.0). All equirectangular.
    static constexpr const char* kFiles[kSkyTextureCount] =
    {
        "Textures/Sky/lroc_color_2k.dds",
        "Textures/Sky/2k_sun.dds",
        "Textures/Sky/2k_stars_milky_way.dds",
    };

    const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
    for (int i = 0; i < kSkyTextureCount; ++i)
    {
        const std::string path = (dataDirectory / kFiles[i]).lexically_normal().string();
        mTextures[i] = mTextureManager.LoadDDS(path, TextureSemantic::Color);
        if (!mTextures[i] || !mTextures[i]->IsValid())
        {
            mTextures[i].reset();
            PTERO_LOG_WARNING("Renderer", "Sky texture unavailable, using the procedural fallback: %s",
                mTextureManager.LastError().c_str());
        }
    }
}

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

        // ---- root signature ----
        // [0] inline CBV b0
        // [1..3] one SRV each: t0 moon, t1 sun, t2 star map. Separate tables, not
        // one range, because TextureManager places each texture wherever the shared
        // heap has room. Version 1.0 descriptors are volatile, so a table whose
        // texture failed to load may point anywhere as long as the shader does not
        // read it - the constants tell it not to.
        D3D12_DESCRIPTOR_RANGE ranges[kSkyTextureCount]{};
        D3D12_ROOT_PARAMETER params[1 + kSkyTextureCount]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < kSkyTextureCount; ++i)
        {
            ranges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors                    = 1;
            ranges[i].BaseShaderRegister                = i;
            ranges[i].RegisterSpace                     = 0;
            ranges[i].OffsetInDescriptorsFromTableStart = 0;
            params[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges   = &ranges[i];
            params[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        // Trilinear, wrapping in longitude, clamped at the poles.
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister   = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = 1 + kSkyTextureCount;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &sampler;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
        mCbStride = (sizeof(SkyConstants) + 255ull) & ~255ull;
        mFrameSlot = 0;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask  = 1;
        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width            = mCbStride * kFramesInFlight;
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
    for (auto& texture : mTextures)
        texture.reset();
    mTextureManager.Shutdown();
    mTexturesRequested = false;
    mIsInitialized = false;
}

void SkyRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const HosekWilkieResult&   hosekResult,
    const TimeOfDaySettings&   settings,
    const DirectX::XMMATRIX&   projectionMatrix,
    const DirectX::XMMATRIX&   viewMatrix,
    UINT                       viewportWidth,
    UINT                       viewportHeight,
    float                      deltaSeconds)
{
    UNREFERENCED_PARAMETER(viewportWidth);
    if (!mIsInitialized || !commandList) return;

    if (!mTexturesRequested)
        LoadTextures();

    // The ambient is already lux-scaled and already blends day into night, so the
    // zenith is simply that. The horizon keeps the old daylight treatment - a
    // warmer, slightly desaturated zenith - and turns a slightly paler blue as
    // the daylight goes.
    const float skyScale = (std::max)(settings.SkyIntensityLux, 0.0f) / kTimeOfDayRefSkyLux;
    const float daylight = hosekResult.DaylightWeight;
    const XMFLOAT3 zenith(hosekResult.AmbientR, hosekResult.AmbientG, hosekResult.AmbientB);
    const XMFLOAT3 dayHorizon(
        zenith.x * 1.2f + 0.05f * skyScale * daylight,
        zenith.y * 1.1f + 0.03f * skyScale * daylight,
        zenith.z * 0.8f + 0.02f * skyScale * daylight);
    const XMFLOAT3 nightHorizon(zenith.x * 1.15f, zenith.y * 1.15f, zenith.z * 1.05f);
    const XMFLOAT3 horizon(
        nightHorizon.x + (dayHorizon.x - nightHorizon.x) * daylight,
        nightHorizon.y + (dayHorizon.y - nightHorizon.y) * daylight,
        nightHorizon.z + (dayHorizon.z - nightHorizon.z) * daylight);

    // An orange band low on the horizon around the sun while it is within a few
    // degrees of setting, peaking just after it has gone under.
    const float elevDeg = hosekResult.SolarElevationRad * (180.0f / 3.14159265f);
    const auto smooth = [](float e0, float e1, float x)
    {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float glow = smooth(-10.0f, -1.0f, elevDeg) * (1.0f - smooth(2.0f, 12.0f, elevDeg)) * skyScale * 0.12f;

    // Build inverse projection for ray reconstruction in the PS, and the inverse
    // view to take the ray into world space, where the sun, moon and stars live.
    XMFLOAT4X4 invProjF;
    XMStoreFloat4x4(&invProjF, XMMatrixTranspose(XMMatrixInverse(nullptr, projectionMatrix)));
    XMFLOAT4X4 invViewF;
    XMStoreFloat4x4(&invViewF, XMMatrixTranspose(XMMatrixInverse(nullptr, viewMatrix)));

    // One pixel's angular size, so the stars and the disc edges stay a pixel or
    // so wide at any field of view instead of aliasing or smearing.
    XMFLOAT4X4 projF;
    XMStoreFloat4x4(&projF, projectionMatrix);
    const float pixelAngle = 2.0f / ((std::max)(std::fabs(projF._22), 1e-4f) * static_cast<float>((std::max)(viewportHeight, 1u)));

    mTimeSeconds = std::fmod(mTimeSeconds + (std::max)(deltaSeconds, 0.0f), 3600.0f);

    // Fill this frame's slot of the constant buffer ring.
    SkyConstants cb{};
    cb.InvProj              = invProjF;
    cb.InvView              = invViewF;
    cb.SkyZenithColor       = zenith;
    cb.PixelAngle           = pixelAngle;
    cb.SkyHorizonColor      = horizon;
    cb.TimeSeconds          = mTimeSeconds;
    cb.SunDirection         = { -hosekResult.SolarDirX, -hosekResult.SolarDirY, -hosekResult.SolarDirZ };
    cb.SunDiscHalfAngleCos  = std::cos(kSunDiscHalfAngleRad);
    cb.SunColor             = { hosekResult.SunDiscR, hosekResult.SunDiscG, hosekResult.SunDiscB };
    cb.MoonDiscSin          = std::sin(kMoonDiscHalfAngleRad * std::clamp(settings.MoonSize, 0.1f, 10.0f));
    cb.MoonDirection        = { -hosekResult.MoonDirX, -hosekResult.MoonDirY, -hosekResult.MoonDirZ };
    cb.StarVisibility       = hosekResult.StarVisibility;
    cb.MoonColor            = { hosekResult.MoonDiscR, hosekResult.MoonDiscG, hosekResult.MoonDiscB };
    cb.StarRadiance         = hosekResult.StarRadiance;
    cb.TwilightGlowColor    = { 1.0f * glow, 0.42f * glow, 0.15f * glow };
    // A faint glow around the moon, as much as its lit face and the dark allow.
    cb.MoonHalo             = hosekResult.MoonIllumination * (1.0f - daylight) * 0.02f;
    for (int row = 0; row < 3; ++row)
    {
        cb.StarRotation[row] = XMFLOAT4(
            hosekResult.StarRotation[row * 3 + 0],
            hosekResult.StarRotation[row * 3 + 1],
            hosekResult.StarRotation[row * 3 + 2],
            0.0f);
    }

    cb.MoonTextureOn = mTextures[kMoonTexture] ? 1.0f : 0.0f;
    cb.SunTextureOn  = mTextures[kSunTexture] ? 1.0f : 0.0f;

    // Star field mode; without the map everything falls back to procedural.
    const int starField = mTextures[kStarTexture] ? std::clamp(settings.StarField, 0, 2) : 0;
    cb.StarMapGain            = starField == 1 ? 1.0f : (starField == 2 ? 0.6f : 0.0f);
    cb.ProceduralStarGain     = starField == 1 ? 0.0f : 1.0f;
    cb.ProceduralMilkyWayGain = starField == 0 ? 1.0f : 0.0f;

    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    const UINT64 cbOffset = mCbStride * mFrameSlot;
    std::memcpy(static_cast<std::byte*>(mMappedCb) + cbOffset, &cb, sizeof(cb));

    // Draw.
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetGraphicsRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress() + cbOffset);

    // The textures live in the shared heap. A missing one still needs its table
    // set; it points at the heap start and is never read (see the root signature).
    ID3D12DescriptorHeap* sharedHeap = DX12Context_GetSrvDescriptorHeap();
    if (sharedHeap)
    {
        commandList->SetDescriptorHeaps(1, &sharedHeap);
        const D3D12_GPU_DESCRIPTOR_HANDLE placeholder = sharedHeap->GetGPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kSkyTextureCount; ++i)
            commandList->SetGraphicsRootDescriptorTable(1 + i, mTextures[i] ? mTextures[i]->GpuHandle : placeholder);
    }
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0); // fullscreen triangle
}
