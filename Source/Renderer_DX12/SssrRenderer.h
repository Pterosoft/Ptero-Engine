#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "SsrSettings.h"

#include <DirectXMath.h>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// SssrRenderer
// AMD FidelityFX Stochastic Screen Space Reflections (SSSR 1.3) with the FidelityFX
// reflection denoiser, selected by SsrSettings::Technique = 1.
//
// Per frame, all compute:
//   1. DepthDownsample   min-depth pyramid of the scene depth (FidelityFX SPD)
//   2. ClassifyTiles     picks the pixels that get a ray, appends them to a ray list, lists
//                        the 8x8 tiles the denoiser must visit, and extracts this frame's
//                        roughness / normal / depth for the denoiser
//      BlueNoise         refreshes the 128x128 sample texture for the GGX lobe
//   3. PrepareArgs       ray and tile counts -> ExecuteIndirect arguments
//   4. Intersect         hierarchical traversal of each ray (indirect)
//   5. Reproject, Prefilter, ResolveTemporal - the denoiser (indirect, per listed tile)
//   6. Apply             adds the result, weighted by the environment BRDF, to the scene
//
// Like SsrRenderer it composites into a private target and copies that back over the scene
// colour, so no pass downstream has to know it ran.
//
// History is ping-ponged: every per-pixel denoiser input and output exists twice, and the
// halves swap roles each frame, which replaces the sample's end-of-frame history copies.
class SssrRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    // Call on every frame the pass does not run, so re-enabling it does not reproject a
    // history that is several frames stale.
    void InvalidateHistory() { mHistoryValid = false; }

    // Same contract as SsrRenderer::Dispatch: sceneColorResource must be in
    // PIXEL_SHADER_RESOURCE on entry and is left that way; the depth and G-Buffer SRVs must
    // already be readable from compute. Matrices are already transposed for HLSL.
    void Dispatch(
        ID3D12GraphicsCommandList*   commandList,
        ID3D12Resource*              sceneColorResource,
        D3D12_GPU_DESCRIPTOR_HANDLE  sceneColorSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  depthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  normalSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  materialSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  albedoSrv,
        const SsrSettings&           settings,
        const DirectX::XMFLOAT4X4&   viewProjection,
        const DirectX::XMFLOAT4X4&   inverseViewProjection,
        const DirectX::XMFLOAT3&     cameraPosition,
        const DirectX::XMFLOAT3&     cameraForward);

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // A texture together with the state it was last left in and its (fixed) descriptor
    // slots. The slots are allocated once and the views rewritten on resize.
    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES       State = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Srv{};
        D3D12_CPU_DESCRIPTOR_HANDLE UavCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Uav{};
    };

    // Buffers are bound as root descriptors, so they need no heap slots.
    struct Buffer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;

        D3D12_GPU_VIRTUAL_ADDRESS Address() const { return Resource->GetGPUVirtualAddress(); }
    };

    // Every pass shares one root layout shape:
    //   [0]                   CBV b0
    //   [1 .. S]              one single-SRV table per texture input, t0..t(S-1)
    //   [S+1 .. S+U]          one UAV table per output, u0.. (a table may span several
    //                         registers - the depth pyramid binds all its levels at once)
    //   then root SRVs        t(S)..       structured buffers
    //   then root UAVs        u(after the tables)..
    // One table per texture is what lets ping-ponged resources swap by handle each frame
    // without copying descriptors into place.
    struct Pass
    {
        DX12Shader Shader;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
        UINT SrvTables = 0;
        UINT UavTables = 0;
        UINT RootSrvs = 0;
        UINT RootUavs = 0;
    };

    // Mirrors SssrConstants in Sssr_Common.hlsli.
    struct SssrCbData
    {
        DirectX::XMFLOAT4X4 ViewProj{};
        DirectX::XMFLOAT4X4 InvViewProj{};
        DirectX::XMFLOAT4X4 PrevViewProj{};
        DirectX::XMFLOAT3   CameraPos{};        float TemporalStabilityFactor = 0.7f;
        DirectX::XMFLOAT3   CameraForward{};    float DepthBufferThickness = 0.015f;
        UINT  BufferWidth = 0;                  UINT  BufferHeight = 0;
        float InvBufferWidth = 0.0f;            float InvBufferHeight = 0.0f;
        float RoughnessThreshold = 0.6f;        float TemporalVarianceThreshold = 0.0f;
        UINT  FrameIndex = 0;                   UINT  MaxTraversalIntersections = 128;
        UINT  MinTraversalOccupancy = 4;        UINT  MostDetailedMip = 0;
        UINT  SamplesPerQuad = 1;               UINT  TemporalVarianceGuidedTracingEnabled = 1;
        UINT  HistoryValid = 0;                 float Intensity = 1.0f;
        UINT  DebugView = 0;                    float Pad0 = 0.0f;
    };

    // The engine keeps up to three frames in flight, so the per-frame constants are ringed.
    static constexpr UINT kFramesInFlight = 3;
    static constexpr UINT kCbStride = (sizeof(SssrCbData) + 255u) & ~255u;
    // Depth pyramid levels bound to the downsample pass; 13 covers 4096x4096.
    static constexpr UINT kMaxDepthMips = 13;

    bool CreatePipelines();
    bool CreatePass(Pass& pass, const wchar_t* shaderFile, UINT srvTables,
                    std::initializer_list<UINT> uavTableSizes, UINT rootSrvs, UINT rootUavs);
    bool CreateBlueNoiseBuffers();
    bool AllocateDescriptors();
    bool CreateSizeDependentResources(UINT width, UINT height);

    void CreateTexture(Texture& texture, UINT width, UINT height, DXGI_FORMAT format,
                       const wchar_t* name, UINT mipLevels = 1);
    void CreateBuffer(Buffer& buffer, UINT64 sizeInBytes, const wchar_t* name);

    void Require(Texture& texture, D3D12_RESOURCE_STATES state);
    void Require(Buffer& buffer, D3D12_RESOURCE_STATES state);
    void FlushBarriers(ID3D12GraphicsCommandList* commandList);

    void Bind(ID3D12GraphicsCommandList* commandList, const Pass& pass, D3D12_GPU_VIRTUAL_ADDRESS constants,
              std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> srvs,
              std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> uavs,
              std::initializer_list<D3D12_GPU_VIRTUAL_ADDRESS> rootSrvs,
              std::initializer_list<D3D12_GPU_VIRTUAL_ADDRESS> rootUavs);

    Pass mDepthDownsamplePass;
    Pass mClassifyTilesPass;
    Pass mBlueNoisePass;
    Pass mPrepareArgsPass;
    Pass mIntersectPass;
    Pass mReprojectPass;
    Pass mPrefilterPass;
    Pass mResolveTemporalPass;
    Pass mApplyPass;

    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDispatchSignature;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedCb = nullptr;
    UINT          mCbSlot = 0;

    // Blue-noise sampler tables. Uploaded once through the command list on first use.
    Buffer mSobolBuffer;
    Buffer mRankingTileBuffer;
    Buffer mScramblingTileBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBlueNoiseUpload;
    bool mBlueNoiseUploaded = false;

    // Size-independent pass resources.
    Texture mBlueNoiseTexture;
    Buffer  mRayCounter;       // [0] ray append, [1] ray count, [2] tile append, [3] tile count
    Buffer  mIndirectArgs;     // [0..2] intersection dispatch, [3..5] denoiser dispatch
    Buffer  mSpdAtomicCounter;

    // Size-dependent pass resources.
    Texture mDepthHierarchy;
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthHierarchyMipUavCpu[kMaxDepthMips]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthHierarchyMipUavTable{};   // first of kMaxDepthMips contiguous slots
    Buffer  mRayList;
    Buffer  mDenoiserTileList;
    Texture mReprojectedRadiance;
    Texture mOutput;

    // Ping-ponged by mBufferIndex; [mBufferIndex] is this frame, the other is history.
    Texture mDepthCopy[2];
    Texture mNormal[2];
    Texture mRoughness[2];
    Texture mRadiance[2];
    Texture mAverageRadiance[2];
    Texture mVariance[2];
    Texture mSampleCount[2];
    UINT    mBufferIndex = 0;

    std::vector<D3D12_RESOURCE_BARRIER> mPendingBarriers;

    DirectX::XMFLOAT4X4 mPrevViewProjection{};
    bool   mHistoryValid = false;
    UINT   mFrameIndex = 0;

    bool mDescriptorsAllocated = false;
    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
