#pragma once

// RadianceProbeRenderer.h
// Manages nested world-space grids (cascades) of L2-SH irradiance probes,
// updated via inline DXR ray queries (cs_6_5): the finest cascade every frame,
// the coarser ones on a rotation. The probes feed indirect diffuse irradiance
// and can be visualised as lit spheres in the debug overlay.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "RadianceProbeSettings.h"
#include "DeferredLightingPass.h"
#include "Components.h"
#include "GpuSkyModel.h"
#include "VirtualShadowMapConstants.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstddef>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;

// ── GPU constant buffer for both probe compute passes (256-byte aligned) ────
struct alignas(256) RadianceProbeConstants
{
    uint32_t ProbeGridX;
    uint32_t ProbeGridY;
    uint32_t ProbeGridZ;
    float    ProbeSpacing;

    float    ProbeOriginX, ProbeOriginY, ProbeOriginZ;
    float    UpdateBlend;

    uint32_t RaysPerProbe;
    uint32_t FrameIndex;
    uint32_t TotalProbes;
    float    DebugSphereRadius;

    float    SunDirX,   SunDirY,   SunDirZ;
    float    _pad0;

    float    SunColorR, SunColorG, SunColorB;
    float    _pad1;

    float    SkyColorR, SkyColorG, SkyColorB;
    float    _pad2;

    float    CameraX,   CameraY,   CameraZ;
    float    ColorLeakIntensity;

    int32_t  MaxBounces;
    int32_t  NumPointLights;
    // Whole cells the follow-camera grid moved since the last update.
    int32_t  HistoryShiftX;
    int32_t  HistoryShiftY;
    DeferredLightingPass::PointLightGpu PointLights[DeferredLightingPass::kMaxPointLights]{};

    float    ViewProj[16];      // row-major view-projection for debug sphere rendering
    float    ViewProjInv[16];

    int32_t  DebugView;
    int32_t  DebugLightingMode;
    int32_t  HistoryShiftZ;
    float    _pad5;

    // Per cascade: origin (xyz) and spacing (w). The single-grid fields above
    // still carry cascade 0.
    float    CascadeOrigin[kMaxRadianceProbeCascades][4];
    // Per cascade: whole cells the grid moved since the last update (xyz), and
    // how many frames one probe waits between traces (w).
    int32_t  CascadeHistory[kMaxRadianceProbeCascades][4];

    uint32_t CascadeCount;
    uint32_t ProbesPerCascade;
    uint32_t _pad6[2];

    // The sky escaped rays see (SkyModel.hlsli), pre-exposed like the colours above.
    GpuSkyModel Sky;

    // Next-event estimation at bounce hits, as RTGI does it (RtGISettings).
    float    VsmMaxTexelSize;
    int32_t  VsmVisibility;     // 1 = ask the shadow map first, trace only where it cannot answer
    int32_t  LightSamples;      // > 0: lights a hit tests, picked stochastically
    float    _pad7;
    VsmGpuConstants Vsm{};      // Active = 0 when the map is not bound this frame
};
// Pinned against the offsets cbuffer RadianceProbeConstants has in
// RadianceProbes_Common.hlsli.
static_assert(offsetof(RadianceProbeConstants, DebugView) == 1792, "see RadianceProbes_Common.hlsli");
static_assert(offsetof(RadianceProbeConstants, CascadeOrigin) == 1808, "see RadianceProbes_Common.hlsli");
static_assert(offsetof(RadianceProbeConstants, CascadeHistory) == 1872, "see RadianceProbes_Common.hlsli");
static_assert(offsetof(RadianceProbeConstants, CascadeCount) == 1936, "see RadianceProbes_Common.hlsli");
static_assert(offsetof(RadianceProbeConstants, Sky) == 1952, "see RadianceProbes_Common.hlsli");
static_assert(offsetof(RadianceProbeConstants, VsmMaxTexelSize) == offsetof(RadianceProbeConstants, Sky) + sizeof(GpuSkyModel),
    "RadianceProbeConstants::VsmMaxTexelSize must follow g_Sky directly, as g_VsmMaxTexelSize does");
static_assert(offsetof(RadianceProbeConstants, Vsm) % 16 == 0, "RadianceProbeConstants::Vsm must be 16-byte aligned to match g_Vsm");

// ── ProbeSH struct mirroring the HLSL layout (7 × float4 = 28 floats) ───────
struct ProbeSHGpu
{
    float c[28]; // 7 × float4
};

class RadianceProbeRenderer
{
public:
    RadianceProbeRenderer()  = default;
    ~RadianceProbeRenderer() { Shutdown(); }

    // Initialise GPU resources for the given probe count.
    // Call whenever settings change (probe count or render resolution changes).
    bool Initialize(const RadianceProbeSettings& settings);

    // Release all GPU resources.
    void Shutdown();

    bool IsInitialized() const { return mIsInitialized; }
    bool HasInitFailed() const { return mInitFailed; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    // Update probe SH by firing rays against the provided TLAS.
    // Must be called after RTGI has built its TLAS.
    void Update(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  tlasSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  vertexSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  indexSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  instanceInfoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  materialRangeSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  baseTextureTableSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  materialSurfaceSrv,   // GpuMaterialSurface per range
        const RadianceProbeSettings& settings,
        float                        colorLeakIntensity,
        int                          maxBounces,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t                     numPointLights,
        const float                  cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        uint32_t frameIndex);

    // The sky rays that escape the scene see, pre-exposed like the sun and sky
    // colours passed to Update. Call before it.
    void SetSkyModel(const GpuSkyModel& sky) { mSkyModel = sky; }

    // Next-event estimation at bounce hits, as RTGI does it: visibility from the
    // virtual shadow map where it holds a page no coarser than vsmMaxTexelSize (null
    // constants when the map is not active this frame), and lightSamples
    // stochastically picked lights per hit (0 = every light). Call before Update.
    void SetVirtualShadowMap(const VsmGpuConstants* constants, D3D12_GPU_VIRTUAL_ADDRESS pageTable,
                             D3D12_GPU_DESCRIPTOR_HANDLE poolSrv)
    {
        mVsmBound = constants != nullptr && pageTable != 0 && poolSrv.ptr != 0;
        mVsmConstants = mVsmBound ? *constants : VsmGpuConstants{};
        mVsmPageTable = mVsmBound ? pageTable : 0;
        mVsmPoolSrv = mVsmBound ? poolSrv : D3D12_GPU_DESCRIPTOR_HANDLE{};
    }
    void SetNextEventOptions(bool vsmVisibility, float vsmMaxTexelSize, int lightSamples)
    {
        mVsmVisibility = vsmVisibility;
        mVsmMaxTexelSize = vsmMaxTexelSize;
        mLightSamples = lightSamples;
    }

    // Draw debug probe spheres onto the scene output RTV using real hardware depth testing.
    // sceneRtvHandle is the CPU RTV handle for the bound render target.
    // sceneDsvHandle is the CPU DSV handle for the scene depth buffer.
    void DrawDebug(
        ID3D12GraphicsCommandList*   cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE  sceneRtvHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE  sceneDsvHandle,
        DXGI_FORMAT                  sceneColorFormat,
        UINT                         width,
        UINT                         height,
        const float                  viewProj[16],
        const float                  viewProjInv[16]);

    // SRV to the current probe SH buffer (for use in the deferred lighting pass).
    D3D12_GPU_DESCRIPTOR_HANDLE GetProbeSHSrv() const { return mProbeSHSrvGpu[mReadIdx]; }

    uint32_t GetTotalProbes() const { return mTotalProbes; }

private:
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateDebugPipeline(DXGI_FORMAT rtvFormat);
    bool CreateDebugMesh(ID3D12GraphicsCommandList* commandList = nullptr);
    bool CreateBuffers(uint32_t totalProbes);
    bool CreateDescriptors(uint32_t totalProbes);
    void UploadConstants(
        const RadianceProbeSettings& settings,
        float colorLeakIntensity,
        int maxBounces,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t numPointLights,
        const float cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        uint32_t frameIndex,
        const float viewProj[16]    = nullptr,
        const float viewProjInv[16] = nullptr);

    bool     mIsInitialized = false;
    bool     mInitFailed    = false;
    std::string mLastError;

    GpuSkyModel mSkyModel{};           // SetSkyModel

    // SetVirtualShadowMap / SetNextEventOptions.
    bool            mVsmBound = false;
    VsmGpuConstants mVsmConstants{};
    D3D12_GPU_VIRTUAL_ADDRESS   mVsmPageTable = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE mVsmPoolSrv{};
    bool            mVsmVisibility = false;
    float           mVsmMaxTexelSize = 0.04f;
    int             mLightSamples = 0;

    uint32_t mTotalProbes   = 0;
    uint32_t mWriteIdx      = 0;       // ping-pong SH buffer index
    uint32_t mReadIdx       = 0;       // most recently produced readable SH buffer index

    ComPtr<ID3D12RootSignature>  mRootSignatureUpdate;
    ComPtr<ID3D12RootSignature>  mRootSignatureDebug;    // graphics root sig
    ComPtr<ID3D12PipelineState>  mPSO_Update;
    ComPtr<ID3D12PipelineState>  mPSO_Debug;             // graphics PSO

    // Format used to create the debug pipeline (cached to detect RT format changes).
    DXGI_FORMAT  mDebugRtvFormat = DXGI_FORMAT_UNKNOWN;
    bool         mDebugPipelineReady = false;

    // Ringed one slot per frame in flight: the engine queues three frames, and
    // a single mapped copy was overwritten while earlier frames still read it -
    // the grid origin and history shift then tore whenever the camera moved.
    static constexpr uint32_t kFramesInFlight = 3;
    ComPtr<ID3D12Resource>  mConstantBuffer;
    void*                   mMappedCb = nullptr;
    uint32_t                mCbSlot = 0;
    RadianceProbeConstants* CurrentCb() const
    {
        return mMappedCb ? reinterpret_cast<RadianceProbeConstants*>(mMappedCb) + mCbSlot : nullptr;
    }
    D3D12_GPU_VIRTUAL_ADDRESS CurrentCbAddress() const
    {
        return mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(mCbSlot) * sizeof(RadianceProbeConstants);
    }

    // Solid sphere mesh used by the 3D probe debug view.
    ComPtr<ID3D12Resource>  mDebugVertexBuffer;
    ComPtr<ID3D12Resource>  mDebugVertexUpload;
    ComPtr<ID3D12Resource>  mDebugIndexBuffer;
    ComPtr<ID3D12Resource>  mDebugIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW mDebugVBView{};
    D3D12_INDEX_BUFFER_VIEW  mDebugIBView{};
    UINT                     mDebugIndexCount = 0;

    // Ping-pong probe SH structured buffers.
    ComPtr<ID3D12Resource>  mProbeSHBuffer[2];

    // SRV/UAV descriptors for ping-pong SH buffers.
    D3D12_CPU_DESCRIPTOR_HANDLE mProbeSHSrvCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mProbeSHSrvGpu[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mProbeSHUavCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mProbeSHUavGpu[2] = {};
    D3D12_RESOURCE_STATES       mProbeSHStates[2] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };

    bool mDescriptorsAllocated = false;

    // Grid origin each cascade's SH history was written at, so a follow-camera
    // grid that snapped to a new cell reads each probe's history from where it
    // was.
    bool  mHasHistoryOrigin = false;
    float mHistoryOrigin[kMaxRadianceProbeCascades][3] = {};
    float mHistorySpacing = 0.0f;
    int   mHistoryGrid[3] = {};
    int   mHistoryCascadeCount = 0;
};
