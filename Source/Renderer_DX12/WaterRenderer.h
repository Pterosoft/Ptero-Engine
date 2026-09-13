#pragma once

// WaterRenderer
// -------------
// Draws animated, refractive water surfaces (WaterComponent) as a forward pass
// AFTER the deferred lighting resolve, so the fully lit opaque scene is
// available for screen-space refraction and depth-based absorption.
//
// Each WaterComponent owns a flat GPU grid (rebuilt only when the water *area*
// or tessellation changes).  The waves are animated in the vertex shader
// (summed Gerstner waves); the pixel shader composites refraction of the scene
// behind the surface, a Fresnel sky+sun reflection, and foam, writing straight
// into the HDR scene-colour target.  Because it reads a *copy* of the scene
// colour and the opaque depth buffer, the pass owns a scratch refraction
// texture that tracks the scene size.

#include "Components.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "OceanSimulation.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

class WaterRenderer
{
public:
    bool Initialize();
    void Shutdown();

    void SetEntities(std::vector<Entity>* entities);

    // Forward refractive water pass.  Run after the deferred lighting resolve,
    // while `sceneColorResource` (bound via `sceneColorRtv`) holds the lit
    // opaque scene and `sceneDepthSrv` exposes the opaque depth.  The renderer
    // copies the scene colour into an internal texture, then draws the water
    // grids into the scene-colour RT, sampling the copy for refraction.
    //
    //  * viewProjection        : jittered VP (NOT transposed)
    //  * invViewProjTransposed : Transpose(inv(VP)) for depth reconstruction
    //  * sunDirection          : world dir FROM sun TOWARD scene (matches deferred)
    void Render(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneColorRtv,
        ID3D12Resource* sceneColorResource,
        D3D12_RESOURCE_STATES sceneColorStateBefore,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT4X4& invViewProjTransposed,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMFLOAT3& sunDirection,
        const DirectX::XMFLOAT3& sunColor,
        const DirectX::XMFLOAT3& skyColor,
        float timeSeconds,
        UINT width,
        UINT height,
        DXGI_FORMAT sceneColorFormat);

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    struct WaterVertex
    {
        DirectX::XMFLOAT3 Position{};
        DirectX::XMFLOAT3 Normal{ 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT2 TexCoord{};
    };

    struct WaterGpuState
    {
        float BuiltSizeX     = 0.0f;
        float BuiltSizeY     = 0.0f;
        int   BuiltResolution = 0;
        bool  Dirty          = false;

        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUpload;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        std::uint32_t IndexCount = 0;
    };

    // b0 - camera / transform / sun / screen.  Packs to exactly 256 bytes.
    struct alignas(256) WaterFrameConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;
        DirectX::XMFLOAT4X4 Model;
        DirectX::XMFLOAT4X4 InvViewProj;

        DirectX::XMFLOAT3 CameraPos{};    float Time = 0.0f;
        DirectX::XMFLOAT3 SunDirection{}; float ScreenWidth = 0.0f;
        DirectX::XMFLOAT3 SunColor{};     float ScreenHeight = 0.0f;
        DirectX::XMFLOAT3 SkyColor{};     int   DebugMode = 0;
    };
    static_assert(sizeof(WaterFrameConstants) == 256);

    // b1 - water material.  Field order/packing must match Water.hlsl exactly
    // (HLSL 16-byte register packing rules).
    struct alignas(256) WaterMaterialConstants
    {
        // rgb tint on the refracted scene / per-metre Beer-Lambert extinction.
        DirectX::XMFLOAT4 ShallowTint{ 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 Extinction{ 0.46f, 0.09f, 0.06f, 0.0f };
        DirectX::XMFLOAT4 FoamColor{ 0.92f, 0.95f, 0.97f, 1.0f };
        // Scattering *coefficient* (see Water.hlsl) - must stay ~0.005-0.03.
        DirectX::XMFLOAT4 ScatterColor{ 0.005f, 0.018f, 0.028f, 1.0f };
        DirectX::XMFLOAT4 SssColor{ 0.04f, 0.34f, 0.30f, 0.5f };

        float Roughness      = 0.06f;
        float Metallic       = 0.0f;
        float WaveScale      = 1.0f;
        float BaseAmplitude  = 0.6f;

        float BaseWavelength = 28.0f;
        float BaseSpeed      = 1.0f;
        float Steepness      = 0.55f;
        float Choppiness     = 1.0f;

        int   WaveCount      = 8;
        float DetailTiling   = 24.0f;
        float DetailStrength = 0.35f;
        float FoamThreshold  = 0.35f;

        DirectX::XMFLOAT2 WindDir{ 1.0f, 0.35f };
        float FresnelPower       = 5.0f;
        float RefractionStrength = 0.05f;

        float AbsorptionDistance = 8.0f;
        float ShorelineFoam      = 0.4f;
        float SunSpecPower       = 1.0f;
        float F0                 = 0.02f;

        float DirectionalSpread  = 1.0f;
        float SssPower           = 4.0f;
        float SssDistortion      = 0.5f;
        float DetailFadeDistance = 400.0f;

        float SsrStride          = 0.6f;
        int   SsrSteps           = 16;
        float SsrThickness       = 1.5f;
        float ReflectionStrength = 1.0f;

        // Slope-variance filtering (see Water.hlsl).
        float DistantRoughness = 0.10f;
        float DistantFlatten   = 0.55f;
        float VolumeDesaturate = 0.15f;
        // Per-entity, derived from SizeX / (Resolution - 1).
        float GridSpacing      = 1.0f;

        // Cascaded FFT ocean.
        DirectX::XMFLOAT3 OceanPatchSizes{ 512.0f, 96.0f, 18.0f };
        float FftEnabled           = 0.0f;
        float FftNormalStrength    = 1.0f;
        float FftDisplacementScale = 1.0f;
        DirectX::XMFLOAT2 _matPad2{};

        std::byte Padding[16]{};
    };
    static_assert(sizeof(WaterMaterialConstants) == 256);

    struct WaterMaterialInfo
    {
        DirectX::XMFLOAT4 ShallowTint{ 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 Extinction{ 0.46f, 0.09f, 0.06f, 0.0f };
        DirectX::XMFLOAT4 FoamColor{ 0.92f, 0.95f, 0.97f, 1.0f };
        DirectX::XMFLOAT4 ScatterColor{ 0.005f, 0.018f, 0.028f, 1.0f };
        DirectX::XMFLOAT4 SssColor{ 0.04f, 0.34f, 0.30f, 0.5f };
        float Roughness      = 0.06f;
        float Metallic       = 0.0f;
        float BaseAmplitude  = 0.6f;
        float BaseWavelength = 28.0f;
        float BaseSpeed      = 1.0f;
        float Steepness      = 0.55f;
        float Choppiness     = 1.0f;
        int   WaveCount      = 8;
        float DetailStrength = 0.35f;
        float FoamThreshold  = 0.35f;
        float FresnelPower   = 5.0f;
        float RefractionStrength = 0.05f;
        float AbsorptionDistance = 8.0f;
        float ShorelineFoam      = 0.4f;
        float SunSpecPower       = 1.0f;
        float F0                 = 0.02f;
        float DirectionalSpread  = 1.0f;
        float SssPower           = 4.0f;
        float SssDistortion      = 0.5f;
        float DetailFadeDistance = 400.0f;
        float SsrStride          = 0.6f;
        int   SsrSteps           = 16;
        float SsrThickness       = 1.5f;
        float ReflectionStrength = 1.0f;
        float DistantRoughness   = 0.10f;
        float DistantFlatten     = 0.55f;
        float VolumeDesaturate   = 0.15f;
        DirectX::XMFLOAT2 WindDir{ 1.0f, 0.35f };

        // Cascaded FFT ocean.  When UseFft is false the analytic Gerstner
        // spectrum is used instead and none of these apply.
        bool  UseFft               = false;
        float WindSpeed            = 9.0f;
        float Fetch                = 100.0f;
        float WaterDepth           = 200.0f;
        float Swell                = 0.35f;
        float FftNormalStrength    = 1.0f;
        float FftDisplacementScale = 1.0f;
        DirectX::XMFLOAT3 PatchSizes{ 512.0f, 96.0f, 18.0f };
    };

    void SyncFromEntities();
    bool CreatePipeline(DXGI_FORMAT sceneColorFormat);
    bool EnsureFrameConstantBuffer(std::size_t requiredCount);
    bool EnsureMaterialConstantBuffer(std::size_t requiredCount);
    bool EnsureRefractionTexture(UINT width, UINT height, DXGI_FORMAT format);
    bool EnsureDepthBuffer(UINT width, UINT height);
    bool EnsureGpuMesh(std::size_t entityIndex, const Entity& entity, WaterGpuState& state);
    void RebuildDirtyWater(ID3D12GraphicsCommandList* commandList);
    WaterMaterialInfo ResolveWaterMaterial(const std::string& materialPath) const;

    std::vector<Entity>* mEntities = nullptr;
    std::unordered_map<std::size_t, WaterGpuState> mGpuStates;
    std::vector<std::size_t> mActiveIndices;

    Microsoft::WRL::ComPtr<ID3D12Resource> mFrameCB;
    WaterFrameConstants* mMappedFrameCB = nullptr;
    std::size_t          mFrameCBCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mMaterialCB;
    WaterMaterialConstants* mMappedMatCB = nullptr;
    std::size_t             mMatCBCapacity = 0;

    // Scratch copy of the lit scene colour, sampled for refraction.
    Microsoft::WRL::ComPtr<ID3D12Resource> mRefractionTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mRefractionSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mRefractionSrvGpu{};
    bool        mRefractionSrvAllocated = false;
    UINT        mRefractionWidth  = 0;
    UINT        mRefractionHeight = 0;
    DXGI_FORMAT mRefractionFormat = DXGI_FORMAT_UNKNOWN;
    // Tracks the refraction texture's current resource state across frames so
    // the copy barriers stay valid (it is created in COPY_DEST).
    D3D12_RESOURCE_STATES mRefractionState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Private depth buffer for the water pass.  The scene depth is bound as an
    // SRV here (for refraction and thickness) so it cannot also be a DSV; but
    // without *some* depth buffer the water surface cannot sort against itself
    // and far crests overwrite near ones.  Opaque occlusion is still handled in
    // the pixel shader against the scene depth SRV.
    Microsoft::WRL::ComPtr<ID3D12Resource>       mWaterDepth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mWaterDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE mWaterDsvHandle{};
    UINT mWaterDepthWidth  = 0;
    UINT mWaterDepthHeight = 0;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    // Cascaded FFT ocean simulation, shared by every water surface in the
    // scene (the spectrum is a global sea state, not a per-entity property).
    OceanSimulation mOcean;
    bool mOceanReady = false;

    bool        mPipelineReady    = false;
    DXGI_FORMAT mSceneColorFormat = DXGI_FORMAT_UNKNOWN;
    std::string mLastError;
};
