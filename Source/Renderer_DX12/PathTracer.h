#pragma once

// PathTracer.h
// Path tracer for Ptero-Engine, in two modes (PathTracerSettings::Mode), both over
// the acceleration structure, geometry pools and material tables
// RtGlobalIllumination builds each frame, and both writing over the lit scene
// colour - after the deferred resolve, before anything is layered over it - so
// bloom, exposure and tonemapping still apply. Shaders: PathTracer_Common.hlsli.
//
// Reference (PathTracer.hlsl): full paths from the camera, accumulated in an fp32
// target for as long as the camera, lights, sky and scene stay put.
//   copy  scene colour -> background   (the raster image, kept where camera rays escape)
//   trace background, TLAS, pools      -> accumulation (rgb sum, a = camera-ray hits)
//                                      -> output       (accumulation / samples)
//   copy  output -> scene colour
//
// Real time (PathTracer_RealTime.hlsl, PathTracer_Composite.hlsl): from the
// G-Buffer surface, one diffuse and one specular path per pixel per frame, split
// and demodulated for NVIDIA NRD RELAX_DIFFUSE_SPECULAR.
//   copy    scene colour -> background
//   trace   G-Buffer, TLAS, pools -> NRD inputs (diffuse, specular, normal/roughness, viewZ, motion)
//   NRD     -> denoised diffuse and specular lighting
//   compose G-Buffer reflectances x denoised lighting -> output
//   copy    output -> scene colour

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DeferredLightingPass.h"
#include "GpuSkyModel.h"
#include "NrdDenoiser.h"
#include "PathTracerSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// Constant buffer (PathTracerConstants in PathTracer_Common.hlsli).
// -----------------------------------------------------------------------
struct alignas(256) PathTracerConstants
{
    uint32_t Width;
    uint32_t Height;
    uint32_t FrameSeed;
    uint32_t SampleIndex;

    uint32_t SamplesPerFrame;
    uint32_t MaxBounces;
    uint32_t Flags;
    uint32_t NumLights;

    float    IndirectClamp;
    float    EmissiveScale;
    float    SunCosAngularRadius;
    float    InvPreExposure;

    float    ViewProjInv[16];   // row-major (transposed) inverse non-jittered view-projection

    float    CameraPos[3];
    int32_t  DebugView;

    float    SunDir[3];         // from the sun toward the scene
    float    GroundAlbedo;

    float    SunColor[3];       // pre-exposed
    float    ScreenCacheScale;  // real time: this frame's pre-exposure over last frame's

    float    SkyAmbient[3];     // pre-exposed
    float    SssTransmission;   // subsurface transmission intensity; 0 = off

    DeferredLightingPass::PointLightGpu Lights[DeferredLightingPass::kMaxPointLights]{};

    GpuSkyModel Sky{};

    // Real time only, all transposed for the shaders' row-vector multiplies.
    float    ViewProjInvJittered[16];   // reconstructs the G-Buffer, which was drawn jittered
    float    CurrViewProj[16];          // non-jittered, for motion
    float    PrevViewProj[16];          // non-jittered, previous frame
    float    WorldToView[16];           // non-jittered, for NRD's view depth

    // Volumetric fog, laid over the traced pixels as the deferred resolve lays it
    // over the raster ones (PT_FLAG_VOLUMETRIC_FOG).
    float    FogStartDistance;
    float    FogMaxDistance;
    float    _FogPad[2];
};
static_assert(offsetof(PathTracerConstants, ViewProjInv) == 48, "PathTracerConstants must match PathTracer_Common.hlsli");
static_assert(offsetof(PathTracerConstants, Lights) == 176, "PathTracerConstants must match PathTracer_Common.hlsli");
static_assert(offsetof(PathTracerConstants, Sky) % 16 == 0, "PathTracerConstants::Sky must be 16-byte aligned to match g_Sky");
static_assert(offsetof(PathTracerConstants, ViewProjInvJittered) == offsetof(PathTracerConstants, Sky) + sizeof(GpuSkyModel),
              "PathTracerConstants must match PathTracer_Common.hlsli");

class PathTracer
{
public:
    // What one frame traces. The scene handles come from RtGlobalIllumination,
    // whose TLAS must have been built this frame.
    struct FrameInputs
    {
        const PathTracerSettings* Settings = nullptr;

        D3D12_GPU_DESCRIPTOR_HANDLE Tlas{};
        D3D12_GPU_DESCRIPTOR_HANDLE Vertices{};
        D3D12_GPU_DESCRIPTOR_HANDLE Indices{};
        D3D12_GPU_DESCRIPTOR_HANDLE InstanceInfo{};
        D3D12_GPU_DESCRIPTOR_HANDLE MaterialRanges{};
        D3D12_GPU_DESCRIPTOR_HANDLE MaterialSurfaces{};
        D3D12_GPU_DESCRIPTOR_HANDLE MaterialTextures{};   // the surface texture table, kMaxRtSurfaceTextures SRVs
        D3D12_GPU_DESCRIPTOR_HANDLE TerrainTrace{};       // StructuredBuffer<GpuTerrainTrace>
        std::uint64_t SceneSignature = 0;
        bool          SceneHasAlphaCutout = false;
        // Shade with the Disney principled BRDF (ShadingSettings::DisneyBrdf).
        bool          DisneyBrdf = true;
        // Materials with useSubsurfaceScattering scatter their diffuse light
        // (SubsurfaceSettings::Enabled), and transmit through thin parts at this
        // intensity (SubsurfaceSettings::Transmission x TransmissionIntensity; 0 = off).
        bool          Subsurface = false;
        float         SubsurfaceTransmission = 0.0f;
        // SubsurfaceSettings::DebugView 2 ("Profile Mask"): the path tracer draws the mask
        // from its own traced materials, since it replaces the raster pass's.
        bool          SubsurfaceMask = false;

        // Non-jittered: the reference accumulation does its own sub-pixel jitter.
        DirectX::XMFLOAT4X4 ViewProjection{};
        DirectX::XMFLOAT3   CameraPosition{};

        // Scene-linear raster units, not yet pre-exposed.
        DirectX::XMFLOAT3 SunDirection{};   // from the sun toward the scene
        DirectX::XMFLOAT3 SunColor{};
        DirectX::XMFLOAT3 SkyAmbient{};
        const DeferredLightingPass::PointLightGpu* Lights = nullptr;
        int               NumLights = 0;

        // Already pre-exposed, as for the GI passes.
        GpuSkyModel Sky{};
        float       PreExposure = 1.0f;

        // ---- Real time --------------------------------------------------
        // The G-Buffer, readable from compute (ALL or NON_PIXEL_SHADER_RESOURCE).
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferAlbedo{};
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferNormal{};     // xy oct normal, z device depth
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferMaterial{};
        // The two Disney principled targets, one table (GBufferSrvs::Principled0).
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferPrincipled{};
        DirectX::XMFLOAT4X4 JitteredViewProjection{};
        DirectX::XMFLOAT4X4 PrevViewProjection{};        // non-jittered
        // NRD's camera, non-jittered, in DirectXMath's own layout (as RTGI passes it).
        DirectX::XMFLOAT4X4 WorldToView{};
        DirectX::XMFLOAT4X4 PrevWorldToView{};
        DirectX::XMFLOAT4X4 ViewToClip{};
        DirectX::XMFLOAT4X4 PrevViewToClip{};
        float CameraJitter[2]{};
        float PrevCameraJitter[2]{};
        float FrameTimeMs = 0.0f;

        // ---- Both modes -------------------------------------------------
        // VolumetricFogRenderer's integrated volume (ALL_SHADER_RESOURCE), or null
        // when the fog is off. The raster image had it applied by the deferred
        // resolve; the traced pixels replace that image, so they take it again here.
        D3D12_GPU_DESCRIPTOR_HANDLE VolumetricFog{};
        float FogStartDistance = 0.1f;
        float FogMaxDistance = 100.0f;
    };

    PathTracer() = default;
    ~PathTracer() { Shutdown(); }

    bool Initialize(UINT width, UINT height);
    bool EnsureSize(UINT width, UINT height);
    void Shutdown();

    // Traces this frame and overwrites `sceneColor`, which must be in
    // RENDER_TARGET state and is left there.
    void Render(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& inputs);

    // Throws the accumulated image (reference) or the denoiser's history (real
    // time) away.
    void ResetAccumulation() { mSampleCount = 0; mDenoiserHistoryReset = true; mScreenCacheValid = false; }

    uint32_t GetSampleCount() const { return mSampleCount; }
    // Reference: what has restarted the accumulation on every recent frame, or
    // null while it is accumulating normally.
    const char* GetPersistentRestartReason() const { return mConsecutiveRestarts >= 3 ? mRestartReason : nullptr; }
    bool     IsConverged()    const { return mConverged; }

    bool        IsInitialized() const { return mIsInitialized; }
    bool        HasInitFailed() const { return mInitFailed; }
    const char* GetLastError()  const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateResolutionResources();
    // The real-time targets and NRD, created on first use: reference-only users
    // never pay for them.
    bool EnsureRealTimeResources();

    void RenderReference(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& inputs);
    void RenderRealTime(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& inputs);

    // Fills the constants both modes share; returns the GPU address of this
    // frame's block after the caller has finished it with Commit.
    PathTracerConstants BuildConstants(const FrameInputs& inputs) const;
    D3D12_GPU_VIRTUAL_ADDRESS CommitConstants(const PathTracerConstants& constants);

    // Copies the scene colour into the background and readies the output for
    // writing; then, after the passes, copies the output back.
    void BeginOutput(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor);
    void EndOutput(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor);
    void BindScene(ID3D12GraphicsCommandList4* cmdList, D3D12_GPU_VIRTUAL_ADDRESS constants, const FrameInputs& inputs);

    // Everything the accumulated image depends on, one value per group so a
    // restart can say what caused it.
    enum SignatureGroup : int { kSigScene, kSigCamera, kSigSunSky, kSigLights, kSigSettings, kSigGroupCount };
    struct InputSignature { std::uint64_t Group[kSigGroupCount]{}; };
    InputSignature ComputeInputSignature(const FrameInputs& inputs) const;

    bool        mIsInitialized = false;
    bool        mInitFailed    = false;
    UINT        mWidth         = 0;
    UINT        mHeight        = 0;
    std::string mLastError;

    ComPtr<ID3D12RootSignature> mRootSignature;
    ComPtr<ID3D12PipelineState> mPipeline;            // reference
    ComPtr<ID3D12PipelineState> mPipelineRealTime;    // real-time trace
    ComPtr<ID3D12PipelineState> mPipelineComposite;   // real-time composite

    // One constant block per frame in flight; see three-frames-in-flight.
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr UINT64   kConstantStride = (sizeof(PathTracerConstants) + 255ull) & ~255ull;
    ComPtr<ID3D12Resource> mConstantBuffer;
    uint8_t*               mMappedConstants = nullptr;
    uint32_t               mConstantSlot = 0;

    ComPtr<ID3D12Resource> mAccumulation;   // RGBA32F, always UNORDERED_ACCESS
    ComPtr<ID3D12Resource> mBackground;     // scene colour copy, NON_PIXEL_SHADER_RESOURCE between frames
    ComPtr<ID3D12Resource> mOutput;         // RGBA16F, COPY_SOURCE between frames

    // Real time: NRD's inputs and outputs, UNORDERED_ACCESS between frames.
    ComPtr<ID3D12Resource> mDiffuseIn;        // IN_DIFF_RADIANCE_HITDIST  RGBA16F
    ComPtr<ID3D12Resource> mSpecularIn;       // IN_SPEC_RADIANCE_HITDIST  RGBA16F
    ComPtr<ID3D12Resource> mNormalRoughness;  // IN_NORMAL_ROUGHNESS       R10G10B10A2_TYPELESS
    ComPtr<ID3D12Resource> mViewZ;            // IN_VIEWZ                  R32F
    ComPtr<ID3D12Resource> mMotion;           // IN_MV                     RG16F
    ComPtr<ID3D12Resource> mDiffuseOut;       // OUT_DIFF_RADIANCE_HITDIST RGBA16F
    ComPtr<ID3D12Resource> mSpecularOut;      // OUT_SPEC_RADIANCE_HITDIST RGBA16F
    // The screen radiance cache: last frame's view depth (its lighting is mDiffuseOut,
    // read before NRD overwrites it). NON_PIXEL_SHADER_RESOURCE between frames.
    ComPtr<ID3D12Resource> mPrevViewZ;
    bool                   mScreenCacheValid = false;   // mDiffuseOut and mPrevViewZ hold last frame
    float                  mPrevPreExposure = 1.0f;
    NrdDenoiser            mDenoiser;
    bool                   mRealTimeReady = false;
    bool                   mRealTimeFailed = false;   // creation failed; not retried every frame
    bool                   mDenoiserHistoryReset = true;
    uint32_t               mDenoiserFrameIndex = 0;
    int                    mDenoiserHistoryFrames = -1;   // what mDenoiser was last tuned for

    // Textures replaced by a resize, held until no frame in flight can name them.
    std::vector<ComPtr<ID3D12Resource>> mRetired[kFramesInFlight];
    uint32_t                            mRetireSlot = 0;

    // Shader-visible slots, allocated once (the shared heap never frees) and
    // rewritten in place on resize.
    bool                        mDescriptorsAllocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE mAccumulationUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mAccumulationUavGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mBackgroundSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mBackgroundSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu{};

    bool mRealTimeDescriptorsAllocated = false;
    struct ViewSlot { D3D12_CPU_DESCRIPTOR_HANDLE Cpu{}; D3D12_GPU_DESCRIPTOR_HANDLE Gpu{}; };
    ViewSlot mDiffuseInUav, mSpecularInUav, mNormalRoughnessUav, mViewZUav, mMotionUav;
    ViewSlot mDiffuseOutSrv, mSpecularOutSrv, mPrevViewZSrv;

    uint32_t      mSampleCount = 0;
    uint32_t      mFrameSeed = 0;
    bool          mConverged = false;
    InputSignature mInputSignature{};
    bool           mHasInputSignature = false;
    // Frames in a row the accumulation was thrown away, and by what: an input
    // that changes every frame (a flickering light, an animated sun) keeps the
    // image at one sample, and the status line names it.
    uint32_t       mConsecutiveRestarts = 0;
    const char*    mRestartReason = nullptr;
    PathTracerMode mLastMode = PathTracerMode::Reference;
};
