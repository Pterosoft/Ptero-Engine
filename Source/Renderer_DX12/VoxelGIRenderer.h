#pragma once

// VoxelGIRenderer.h
// Voxel cone traced global illumination - GI with no ray tracing hardware.
//
// The scene is rasterized into a clipmap of nested voxel cubes centred on the
// camera (VoxelGI_Voxelize.hlsl), each level twice the voxel size of the one
// inside it. Each level is then lit - sun and local lights with shadows marched
// through the voxels, plus the previous bounce - into premultiplied radiance,
// and a cone trace from every lit voxel gathers the light it receives for the
// next update, which converges to multiple bounces (VoxelGI_Update.hlsl).
// Levels are refreshed round robin, the finest every other frame.
//
// Per frame, every pixel then cone traces the clipmap for diffuse irradiance
// and one reflection cone (VoxelGI_Trace.hlsl), at half resolution by default,
// and a depth- and normal-aware resolve brings both to full resolution in the
// same units the RTGI pass produces, so the deferred resolve consumes either.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DeferredLightingPass.h"
#include "LightClusterRenderer.h"
#include "VoxelGISettings.h"
#include "GpuSkyModel.h"
#include "VirtualShadowMapConstants.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class EntityMeshRenderer;
class VirtualGeometryRenderer;

// Mirrors the VoxelGIConstants cbuffer in VoxelGI_Common.hlsli.
struct alignas(256) VoxelGIConstants
{
    static constexpr int kMaxLevels = 6;
    static constexpr int kMaxPointLights = 16;

    float LevelOrigin[kMaxLevels][4];      // xyz voxel (0,0,0) corner, w voxel size
    float IndirectOrigin[kMaxLevels][4];   // w = 1 when the level's indirect texture is valid

    float ViewProjInv[16];

    float CameraPos[3];     float MaxTraceDistance;
    float SunDir[3];        float SurfaceOffset;
    float SunColor[3];      float MultiBounce;
    float SkyColor[3];      float SpecularRoughnessThreshold;

    uint32_t Resolution;
    uint32_t LevelCount;
    uint32_t UpdateLevel;
    uint32_t DiffuseCones;

    uint32_t FullWidth;
    uint32_t FullHeight;
    uint32_t TraceWidth;
    uint32_t TraceHeight;

    float    InvResolution;
    uint32_t SunShadows;
    uint32_t LocalLightShadows;
    int32_t  DebugView;

    int32_t  NumPointLights;
    uint32_t HalfResolution;
    uint32_t SpecularEnabled;
    float    SpecularIntensity;

    DeferredLightingPass::PointLightGpu PointLights[kMaxPointLights];

    // The sky escaped cones see (SkyModel.hlsli), pre-exposed like the colours above.
    GpuSkyModel Sky;

    float PrevLevelOrigin[4];   // the updated level's previous origin; w = 1 when it may be blended with
    float TemporalBlend;        // share of the new lighting a voxel takes this update
    uint32_t UseLightClusters;  // 1 = local lights from the clustered list (FrameInputs::LightClusters)
    float TemporalPad[2];

    // Lighting the voxels as RTGI lights its bounce hits (VoxelGISettings).
    float    VsmMaxTexelSize;
    int32_t  VsmVisibility;     // 1 = ask the shadow map before marching
    int32_t  LightSamples;      // > 0: local lights a voxel tests, picked stochastically
    uint32_t LightSampleSeed;
    VsmGpuConstants Vsm;        // Active = 0 when the map is not bound
};
static_assert(offsetof(VoxelGIConstants, Vsm) % 16 == 0, "VoxelGIConstants::Vsm must be 16-byte aligned to match g_Vsm");
static_assert(offsetof(VoxelGIConstants, Sky) % 16 == 0, "VoxelGIConstants::Sky must be 16-byte aligned to match gSky");
static_assert(DeferredLightingPass::kMaxPointLights == VoxelGIConstants::kMaxPointLights,
              "VoxelGIConstants carries the deferred pass's light array as is");

class VoxelGIRenderer
{
public:
    // Everything a frame of voxel GI reads from the scene. Light colours are
    // pre-exposed, as for RTGI; the caller undoes it where the output is used.
    struct FrameInputs
    {
        const float* ViewProjInv = nullptr;   // transposed inverse of the jittered view-projection
        DirectX::XMFLOAT3 CameraPos{};
        float SunDir[3]{};                    // from the sun into the scene
        float SunColor[3]{};
        float SkyColor[3]{};
        GpuSkyModel Sky{};                    // the sky escaped cones see, pre-exposed
        const DeferredLightingPass::PointLightGpu* PointLights = nullptr;
        uint32_t NumPointLights = 0;

        D3D12_GPU_DESCRIPTOR_HANDLE GBufferAlbedo{};
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferNormal{};
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferMaterial{};
        UINT Width = 0;
        UINT Height = 0;

        // Re-voxelize every level this frame (level load, play start).
        bool ResetClipmap = false;

        // The virtual shadow map the voxels' light visibility may come from
        // (VoxelGISettings::VsmVisibility), or null when it is not active this
        // frame. The pool must be readable from compute.
        const VsmGpuConstants* Vsm = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS VsmPageTable = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE VsmPoolSrv{};

        // Every scene light, clustered (LightClusterRenderer), as RTGI lights its bounce
        // hits from: a voxel tests only the lights its world grid cell lists, with the
        // shadow budget's shadow indices. Invalid: the voxels fall back to PointLights.
        LightClusterBindings LightClusters{};

        // Virtualized meshes are voxelized from a cut through their cluster DAG at the
        // level's voxel size (EntityMeshRenderer::RenderVoxelization). Null: from their LODs.
        const VirtualGeometryRenderer* VirtualGeometry = nullptr;
    };

    VoxelGIRenderer() = default;
    ~VoxelGIRenderer() { Shutdown(); }

    // Device-level setup: root signatures and pipelines. Resolution-dependent
    // resources are made by Render as the settings and frame size ask.
    bool Initialize();
    void Shutdown();

    // Refreshes the clipmap levels due this frame, then traces and resolves the
    // screen. The G-Buffer must be readable from compute (ALL_SHADER_RESOURCE).
    // Leaves the outputs in ALL_SHADER_RESOURCE. Changes the bound render
    // targets, viewport and root signatures. False when nothing was produced,
    // in which case the outputs must not be bound.
    bool Render(
        ID3D12GraphicsCommandList* cmdList,
        EntityMeshRenderer&        meshes,
        const VoxelGISettings&     settings,
        const FrameInputs&         inputs);

    // Diffuse irradiance / pi, demodulated (RGBA16F) - RTGI's output units.
    D3D12_GPU_DESCRIPTOR_HANDLE GetDiffuseSrv() const { return mDiffuseOut.Srv; }
    // Reflection weighted by the environment BRDF, for the deferred resolve.
    D3D12_GPU_DESCRIPTOR_HANDLE GetSpecularSrv() const { return mSpecularOut.Srv; }
    // Unweighted reflected radiance (a = 1 where traced), for SSR to fall back on.
    D3D12_GPU_DESCRIPTOR_HANDLE GetReflectionSrv() const { return mReflectionOut.Srv; }

    bool IsInitialized() const { return mIsInitialized; }
    bool HasInitFailed() const { return mInitFailed; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    // Approximate GPU memory the clipmap volumes take, for the settings panel.
    uint64_t GetClipmapBytes() const { return mClipmapBytes; }

private:
    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Srv{};
        D3D12_CPU_DESCRIPTOR_HANDLE UavCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Uav{};
    };

    bool CreateRootSignatures();
    bool CreatePipelines();
    bool AllocateDescriptors();
    bool EnsureClipmap(ID3D12GraphicsCommandList* cmdList, uint32_t resolution, uint32_t levelCount);
    bool EnsureScreenTextures(UINT width, UINT height, bool halfResolution);

    void CreateTexture3D(Texture& texture, UINT width, UINT height, UINT depth, DXGI_FORMAT format, const wchar_t* name);
    void CreateTexture2D(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name);
    void WriteViews(Texture& texture, bool volume);

    void Transition(ID3D12GraphicsCommandList* cmdList, Texture& texture, D3D12_RESOURCE_STATES state);
    void UavBarrier(ID3D12GraphicsCommandList* cmdList, Texture& texture);

    // Fills this frame's constant block `block` and returns its GPU address.
    D3D12_GPU_VIRTUAL_ADDRESS WriteConstants(uint32_t block, const VoxelGIConstants& constants);

    void UpdateLevel(
        ID3D12GraphicsCommandList* cmdList,
        EntityMeshRenderer&        meshes,
        const VoxelGISettings&     settings,
        VoxelGIConstants&          constants,
        uint32_t                   level,
        uint32_t                   block,
        bool                       keepHistory);

    bool mIsInitialized = false;
    bool mInitFailed = false;
    std::string mLastError;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mVoxelizeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mVoxelizePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mResolveKeysPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mClearKeysPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mClearVolumePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mInjectPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBouncePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mTracePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mResolvePso;

    // One block per level that can update in a frame plus one for the screen
    // passes, times the frames in flight: the CPU may be three frames ahead of
    // the GPU, so a single block would be rewritten while still being read.
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kBlocksPerFrame = VoxelGIConstants::kMaxLevels + 1;
    static constexpr UINT64   kConstantStride = (sizeof(VoxelGIConstants) + 255ull) & ~255ull;
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    uint8_t* mMappedConstants = nullptr;
    uint32_t mFrameSlot = 0;

    // Textures replaced by a resize, kept until the GPU can no longer be
    // reading them: freed when the frame slot they were retired in comes round.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mPendingReleases[kFramesInFlight];

    // Clipmap volumes, levels stacked along Z.
    Texture mAlbedo;            // RGBA8: albedo, coverage
    Texture mNormal;            // RGBA8: normal * 0.5 + 0.5
    Texture mRadiance;          // RGBA16F: premultiplied radiance, opacity
    Texture mIndirect;          // RGBA16F: received irradiance / pi
    Texture mRadianceScratch;   // RGBA16F, one level: InjectCS's output before it is copied in
    Texture mAlbedoKeys;        // R32_UINT, one level: the voxelizer's atomic albedo keys
    Texture mNormalKeys;        // R32_UINT, one level: the voxelizer's atomic normal keys

    // Screen.
    Texture mTraceDiffuse;
    Texture mTraceReflection;
    Texture mDiffuseOut;
    Texture mSpecularOut;
    Texture mReflectionOut;

    bool mDescriptorsAllocated = false;
    uint32_t mResolution = 0;
    uint32_t mLevelCount = 0;
    uint64_t mClipmapBytes = 0;
    UINT mScreenWidth = 0;
    UINT mScreenHeight = 0;
    bool mHalfResolution = true;

    // Where each level's current content sits (w = voxel size, 0 = never filled),
    // and where its indirect texture was computed.
    std::array<DirectX::XMFLOAT4, VoxelGIConstants::kMaxLevels> mLevelOrigin{};
    std::array<DirectX::XMFLOAT4, VoxelGIConstants::kMaxLevels> mIndirectOrigin{};
    // mFrameCounter when each level was last filled, so the temporal blend can
    // follow light changes at the same speed in levels refreshed less often.
    std::array<uint32_t, VoxelGIConstants::kMaxLevels> mLevelUpdateFrame{};
    float mVoxelSize = 0.0f;
    uint32_t mFrameCounter = 0;

    // This frame's virtual shadow map bindings for InjectCS (FrameInputs::Vsm*).
    D3D12_GPU_DESCRIPTOR_HANDLE mVsmPoolSrv{};
    D3D12_GPU_VIRTUAL_ADDRESS   mVsmPageTable = 0;
    // This frame's FrameInputs::LightClusters and ::VirtualGeometry, for UpdateLevel.
    LightClusterBindings        mLightClusters{};
    const VirtualGeometryRenderer* mVirtualGeometry = nullptr;
    uint32_t mCoarseCursor = 0;
};
