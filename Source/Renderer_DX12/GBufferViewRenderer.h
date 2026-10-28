#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DeferredLightingPass.h"

#include "../QtUi/UiTypes.h"

#include <DirectXMath.h>
#include <iterator>
#include <string>

// What the viewport's G-Buffer view mode shows (View Mode > G-Buffer). The values are
// shared with GBufferView.hlsl.
enum class GBufferView : int
{
    Off = 0,
    BaseColor,
    WorldNormal,
    Roughness,
    Metallic,
    MaterialAo,
    Specular,
    Opacity,
    SceneDepth,
    WorldPosition,
    MotionVectors,
    SubsurfaceProfile,
    Foliage,
    DisneySubsurface,
    SpecularTint,
    Sheen,
    SheenTint,
    Clearcoat,
    ClearcoatGloss,
    Anisotropy,
    Count,

    // Every view above at once, in a grid of kOverviewColumns.
    Overview = 100,
};

namespace GBufferViewLayout
{
    constexpr int kOverviewColumns = 5;
    constexpr int kOverviewTiles   = static_cast<int>(GBufferView::Count) - 1;
    constexpr int kOverviewRows    = (kOverviewTiles + kOverviewColumns - 1) / kOverviewColumns;

    // Menu and tile names, indexed by GBufferView (Off included, so the index is the value).
    inline constexpr const char* kNames[] =
    {
        "Off",
        "Base Color",
        "World Normal",
        "Roughness",
        "Metallic",
        "Material AO",
        "Specular",
        "Opacity",
        "Scene Depth",
        "World Position",
        "Motion Vectors",
        "Subsurface Profile",
        "Foliage",
        "Disney: Subsurface",
        "Disney: Specular Tint",
        "Disney: Sheen",
        "Disney: Sheen Tint",
        "Disney: Clearcoat",
        "Disney: Clearcoat Gloss",
        "Disney: Anisotropy",
    };
    static_assert(std::size(kNames) == static_cast<size_t>(GBufferView::Count), "G-Buffer view names out of date");
}

// Draws one G-Buffer channel - or all of them in a grid - over the whole frame, for the
// viewport's G-Buffer view modes. Runs after the post chain into a target of its own,
// which the viewport then shows in place of the finished frame, so nothing downstream
// (exposure, tonemapping, bloom, TAA) alters the data on screen.
class GBufferViewRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    // The G-Buffer must be readable by a pixel shader (EndGeometryPass has run).
    void Render(
        ID3D12GraphicsCommandList*        commandList,
        GBufferView                       view,
        const GBufferSrvs&                gbuffer,
        D3D12_GPU_DESCRIPTOR_HANDLE       motionVectorSrv,
        const DirectX::XMFLOAT4X4&        invViewProjTransposed,
        const DirectX::XMFLOAT3&          cameraPosition);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputSrvGpu; }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreatePipeline();
    bool CreateTextures(UINT width, UINT height);

    struct alignas(256) ViewConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj{};
        DirectX::XMFLOAT3   CameraPos{};
        int                 View = 0;
        UINT                Width = 0;
        UINT                Height = 0;
        UINT                HasMotion = 0;
        UINT                Pad = 0;
    };

    DX12Shader                                   mVertexShader;
    DX12Shader                                   mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>       mConstantBuffer;
    void*                                        mMappedCb = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       mOutputTexture;

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};
    UiTextureID                 mOutputTextureId = UiTextureID_Invalid;
    bool                        mSrvSlotAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
