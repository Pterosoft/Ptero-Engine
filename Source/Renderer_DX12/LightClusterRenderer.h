#pragma once

// LightClusterRenderer
// --------------------
// Clustered light lists for the passes that shade with every scene light: the deferred
// resolve, the volumetric fog injection and the RTGI next-event estimation.
//
// Each frame, once the lights and their shadow indices are final:
//
//   Build   uploads every scene light (up to kMaxSceneLights) to a structured buffer,
//           with a per-light float4 of extras (x fog scale, y GI scale), and runs
//           LightClusterBuild.hlsl: a light bitmask per view cluster (64-pixel tile x
//           32 exponential depth slices) and per cell of a camera-centred world grid.
//
// The consumers bind GetBindings() through five root parameters in register space 7
// (FillRootParameters / Bind); Data/Shaders/LightClusters.hlsli has the lookups. A
// point no cluster covers falls back to every light, so nothing is lost off the
// edges, only time.

#include "DeferredLightingPass.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

// GPU addresses of everything a consumer binds. All zero until the first Build.
struct LightClusterBindings
{
    D3D12_GPU_VIRTUAL_ADDRESS Constants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS Lights = 0;
    D3D12_GPU_VIRTUAL_ADDRESS Extras = 0;
    D3D12_GPU_VIRTUAL_ADDRESS ViewMasks = 0;
    D3D12_GPU_VIRTUAL_ADDRESS GridMasks = 0;

    bool IsValid() const { return Constants != 0; }
};

// Must match PteroLightClusterConstants in LightClusters.hlsli.
struct LightClusterGpuConstants
{
    DirectX::XMFLOAT4X4 View{};          // world -> view (row vectors)
    float         ProjX = 1.0f;
    float         ProjY = 1.0f;
    float         ProjOffsetX = 0.0f;
    float         ProjOffsetY = 0.0f;
    float         ProjW = 1.0f;
    float         NearDepth = 0.1f;
    float         SliceScale = 1.0f;
    float         FarDepth = 500.0f;
    std::uint32_t TilesX = 1;
    std::uint32_t TilesY = 1;
    std::uint32_t Slices = 1;
    std::uint32_t LightCount = 0;
    std::uint32_t WordCount = 0;
    std::uint32_t Enabled = 1;
    std::uint32_t GridX = 1;
    std::uint32_t GridY = 1;
    DirectX::XMFLOAT3 GridOrigin{};
    float         GridCellSize = 4.0f;
    std::uint32_t GridZ = 1;
    std::uint32_t Pad[3]{};
};
static_assert(sizeof(LightClusterGpuConstants) == 160, "Must match PteroLightClusterConstants");

class LightClusterRenderer
{
public:
    // Must match PTERO_MAX_SCENE_LIGHTS.
    static constexpr int  kMaxSceneLights = DeferredLightingPass::kMaxSceneLights;
    static constexpr UINT kMaskWords = kMaxSceneLights / 32;

    // View clusters: tiles of kTileSize pixels, kSlices depth slices. The mask buffer is
    // sized for kMaxTilesX x kMaxTilesY tiles (8K); a larger target uses bigger tiles.
    static constexpr UINT kTileSize = 64;
    static constexpr UINT kSlices = 32;
    static constexpr UINT kMaxTilesX = 128;
    static constexpr UINT kMaxTilesY = 72;

    // World grid around the camera, for points that need not be on screen.
    static constexpr UINT kGridX = 32;
    static constexpr UINT kGridY = 32;
    static constexpr UINT kGridZ = 16;

    // Consumers' root parameters: b0, t0..t3 in space 7.
    static constexpr UINT kRootParameterCount = 5;
    static constexpr UINT kRegisterSpace = 7;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return mInitialized; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    struct FrameInputs
    {
        // Every scene light, shadow indices final, and one float4 of extras per light.
        const DeferredLightingPass::PointLightGpu* Lights = nullptr;
        const DirectX::XMFLOAT4* Extras = nullptr;
        int Count = 0;

        // The camera's non-jittered view and projection (row vectors), and the size of
        // the target the passes shade.
        DirectX::XMFLOAT4X4 View{};
        DirectX::XMFLOAT4X4 Projection{};
        UINT Width = 1;
        UINT Height = 1;
        DirectX::XMFLOAT3 CameraPosition{};

        // View depths the slices span; the last slice runs on past FarDepth.
        float NearDepth = 0.1f;
        float FarDepth = 500.0f;
        // World grid cell size, in metres.
        float GridCellSize = 4.0f;
        // Off: no masks are built and every lookup answers every light.
        bool Enabled = true;
    };

    void Build(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs);
    const LightClusterBindings& GetBindings() const { return mBindings; }
    const LightClusterGpuConstants& GetConstants() const { return mConstants; }

    // Writes the consumer's kRootParameterCount root parameters into params.
    static void FillRootParameters(D3D12_ROOT_PARAMETER* params, D3D12_SHADER_VISIBILITY visibility);
    static void BindGraphics(ID3D12GraphicsCommandList* commandList, UINT firstRootIndex, const LightClusterBindings& bindings);
    static void BindCompute(ID3D12GraphicsCommandList* commandList, UINT firstRootIndex, const LightClusterBindings& bindings);

private:
    // Well over the frames in flight: a frame can render the scene more than once
    // (the deferred pass keeps 8 slots for the same reason), and a slot must not be
    // rewritten while the GPU still reads it.
    static constexpr UINT kUploadSlots = 12;
    static constexpr UINT64 kLightBytes = UINT64(kMaxSceneLights) * sizeof(DeferredLightingPass::PointLightGpu);
    static constexpr UINT64 kExtrasBytes = UINT64(kMaxSceneLights) * sizeof(DirectX::XMFLOAT4);
    static constexpr UINT64 kConstantStride = (sizeof(LightClusterGpuConstants) + 255ull) & ~255ull;
    static constexpr UINT64 kViewMaskBytes = UINT64(kMaxTilesX) * kMaxTilesY * kSlices * kMaskWords * 4ull;
    static constexpr UINT64 kGridMaskBytes = UINT64(kGridX) * kGridY * kGridZ * kMaskWords * 4ull;

    bool CreateBuffers();
    bool CreatePipelines();

    bool        mInitialized = false;
    std::string mLastError;

    Microsoft::WRL::ComPtr<ID3D12Resource> mUpload;       // kUploadSlots x (constants, lights, extras)
    std::byte*                             mUploadMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mViewMasks;    // DEFAULT, UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> mGridMasks;    // DEFAULT, UAV
    D3D12_RESOURCE_STATES                  mMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    DX12Shader mBuildViewShader;
    DX12Shader mBuildGridShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBuildViewPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBuildGridPipeline;

    UINT                     mSlot = 0;
    LightClusterGpuConstants mConstants{};
    LightClusterBindings     mBindings{};
};
