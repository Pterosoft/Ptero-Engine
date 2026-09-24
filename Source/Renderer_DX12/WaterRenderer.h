#pragma once

// WaterRenderer
// -------------
// Draws procedural water surfaces (WaterComponent) as a forward pass AFTER the
// deferred lighting resolve, so the fully lit opaque scene is available for
// screen-space refraction and reflection.
//
// The shading is tuxalin/water-shader (Data/Shaders/Water.hlsl, original in
// Source/SDKs/water-shader-master): Gerstner + sine wave displacement with a
// tiling height map, scrolling normal maps, depth-based colour extinction,
// Fresnel reflection/refraction, sun specular and shore foam.
//
// Each WaterComponent owns a flat GPU grid (rebuilt only when the water *area*
// or tessellation changes); the waves are displaced in the vertex shader.
// Because the pixel shader reads a *copy* of the scene colour and the opaque
// depth buffer, the pass owns a scratch refraction texture that tracks the
// scene size.  The shader's textures (normal, height, foam, shore) are PNGs
// named by the water material and loaded on first use.

#include "Components.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
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

    // Forward water pass.  Run after the deferred lighting resolve, while
    // `sceneColorResource` (bound via `sceneColorRtv`) holds the lit opaque
    // scene and `sceneDepthSrv` exposes the opaque depth.  The renderer copies
    // the scene colour into an internal texture, then draws the water grids
    // into the scene-colour RT, sampling the copy for refraction/reflection.
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
    struct WaterFrameConstants
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

    // b1 - water material.  Field order/packing must match the WaterMaterial
    // cbuffer in Water.hlsl exactly (HLSL 16-byte register packing rules).
    struct WaterMaterialConstants
    {
        DirectX::XMFLOAT4 SurfaceColor{};
        DirectX::XMFLOAT4 ShoreColor{};
        DirectX::XMFLOAT4 DepthColor{};
        DirectX::XMFLOAT4 HorizontalExtinction{};
        DirectX::XMFLOAT4 WaveAmplitude{};
        DirectX::XMFLOAT4 WavesIntensity{};
        DirectX::XMFLOAT4 WavesNoise{};
        DirectX::XMFLOAT4 FoamNoise{};
        DirectX::XMFLOAT4 FoamRanges{};
        DirectX::XMFLOAT4 SpecularValues{};

        DirectX::XMFLOAT2 WindDirection{};
        DirectX::XMFLOAT2 RefractionValues{};

        DirectX::XMFLOAT2 FoamTiling{};
        float FoamSpeed     = 0.0f;
        float FoamIntensity = 0.0f;

        float AmbientDensity  = 0.0f;
        float DiffuseDensity  = 0.0f;
        float HeightIntensity = 0.0f;
        float NormalIntensity = 0.0f;

        float TextureTiling       = 0.0f;
        float WaveTiling          = 0.0f;
        float WaveSteepness       = 0.0f;
        float WaveAmplitudeFactor = 0.0f;

        float WaterClarity      = 0.0f;
        float WaterTransparency = 0.0f;
        float Shininess         = 0.0f;
        float RefractionScale   = 0.0f;

        float Distortion = 0.0f;
        float ShoreFade  = 0.0f;
        float WaterLevel = 0.0f;
        std::uint32_t Features = 0;
    };
    static_assert(sizeof(WaterMaterialConstants) == 256);

    // One draw's constants: b0 at offset 0, b1 at offset 256 (CBVs must be
    // 256-byte aligned).
    struct WaterDrawConstants
    {
        WaterFrameConstants    Frame;
        WaterMaterialConstants Material;
    };
    static_assert(sizeof(WaterDrawConstants) == 512);

    // Material "water" block.  Defaults are the original shader's Unity
    // property defaults (colours converted from sRGB to linear).  Key names
    // are the original's uniform names in camelCase, e.g. "waveSteepness".
    struct WaterMaterialInfo
    {
        DirectX::XMFLOAT3 SurfaceColor{ 0.0006f, 0.2307f, 0.4480f };
        DirectX::XMFLOAT3 ShoreColor{ 0.0006f, 0.2307f, 0.4480f };
        DirectX::XMFLOAT3 DepthColor{ 0.0003f, 0.00015f, 0.0185f };
        DirectX::XMFLOAT3 HorizontalExtinction{ 3.0f, 10.0f, 12.0f };
        DirectX::XMFLOAT4 WaveAmplitude{ 0.05f, 0.1f, 0.2f, 0.3f };
        DirectX::XMFLOAT4 WavesIntensity{ 3.0f, 2.0f, 2.0f, 10.0f };
        DirectX::XMFLOAT4 WavesNoise{ 0.05f, 0.15f, 0.03f, 0.05f };
        DirectX::XMFLOAT4 FoamNoise{ 0.1f, 0.3f, 0.1f, 0.3f };
        // z is the crest-foam height above the water level; 100 m = off.
        DirectX::XMFLOAT3 FoamRanges{ 2.0f, 3.0f, 100.0f };
        DirectX::XMFLOAT3 SpecularValues{ 12.0f, 768.0f, 0.15f };
        // World XY; its length is the wind speed.
        DirectX::XMFLOAT2 WindDirection{ 3.0f, 5.0f };
        DirectX::XMFLOAT2 RefractionValues{ 0.3f, 0.01f };
        DirectX::XMFLOAT2 FoamTiling{ 2.0f, 0.5f };

        float AmbientDensity      = 0.15f;
        float DiffuseDensity      = 0.1f;
        float NormalIntensity     = 0.5f;
        float TextureTiling       = 1.0f;
        float HeightIntensity     = 0.5f;
        float WaveTiling          = 1.0f;
        float WaveAmplitudeFactor = 1.0f;
        float WaveSteepness       = 0.5f;
        float WaterClarity        = 0.75f;
        float WaterTransparency   = 10.0f;
        float RefractionScale     = 0.005f;
        float Shininess           = 0.5f;
        float Distortion          = 0.05f;
        float FoamSpeed           = 10.0f;
        float FoamIntensity       = 0.5f;
        float ShoreFade           = 0.3f;

        bool UseDisplacement = true;
        bool UseFoam         = true;
        bool UseFiltering    = true;
        bool BlinnPhong      = false;

        std::string NormalTexture = "Textures/Water/water_normal.png";
        std::string HeightTexture = "Textures/Water/water_height.png";
        std::string FoamTexture   = "Textures/Water/foam.png";
        std::string ShoreTexture  = "Textures/Water/foam_shore.png";
    };

    struct CachedMaterial
    {
        WaterMaterialInfo Info;
        std::filesystem::file_time_type WriteTime{};
    };

    struct WaterTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_GPU_DESCRIPTOR_HANDLE Srv{};
    };

    // A resource the GPU may still be reading, kept alive until the frames in
    // flight that could reference it have retired.
    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        std::uint64_t ReleaseAfterFrame = 0;
    };

    void SyncFromEntities();
    bool CreatePipeline(DXGI_FORMAT sceneColorFormat);
    bool EnsureDrawConstantBuffer(std::size_t requiredCount);
    bool EnsureRefractionTexture(UINT width, UINT height, DXGI_FORMAT format);
    bool EnsureDepthBuffer(UINT width, UINT height);
    bool EnsureGpuMesh(std::size_t entityIndex, const Entity& entity, WaterGpuState& state);
    void RebuildDirtyWater(ID3D12GraphicsCommandList* commandList);
    const WaterMaterialInfo& ResolveWaterMaterial(const std::string& materialPath);
    // Returns the SRV for a data-relative image, loading it on first use (the
    // upload is recorded on `commandList`).  An unreadable file yields a 1x1
    // texture of `fallbackRgba`, so the draw still has a valid descriptor.
    D3D12_GPU_DESCRIPTOR_HANDLE GetTexture(
        ID3D12GraphicsCommandList* commandList,
        const std::string& dataRelativePath,
        std::uint32_t fallbackRgba);
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    void ReleaseRetiredResources();

    std::vector<Entity>* mEntities = nullptr;
    std::unordered_map<std::size_t, WaterGpuState> mGpuStates;
    std::vector<std::size_t> mActiveIndices;

    // Ring of per-draw constants, one region per frame in flight: the engine
    // keeps three frames in flight, so a single mapped block would be
    // overwritten while an earlier frame is still reading it.
    static constexpr std::uint32_t kFramesInFlight = 3;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawCB;
    WaterDrawConstants* mMappedDrawCB = nullptr;
    std::size_t         mDrawCBCapacity = 0;   // draws per frame region

    std::uint64_t mFrameCounter = 0;
    std::vector<RetiredResource> mRetired;

    std::unordered_map<std::string, CachedMaterial> mMaterialCache;
    const WaterMaterialInfo mDefaultMaterial{};
    // Keyed by data-relative path.  Loaded once and kept: the shared SRV heap
    // never frees descriptors, so textures are never reloaded.
    std::unordered_map<std::string, WaterTexture> mTextures;

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
    // SRV here (for refraction and water depth) so it cannot also be a DSV; but
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

    bool        mPipelineReady    = false;
    DXGI_FORMAT mSceneColorFormat = DXGI_FORMAT_UNKNOWN;
    std::string mLastError;
};
