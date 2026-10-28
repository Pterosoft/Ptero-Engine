#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "EntityMeshRenderer.h"
#include "PointLightRenderer.h"
#include "DecalRenderer.h"
#include "TAARenderer.h"
#include "TaaSettings.h"
#include "SkyRenderer.h"
#include "HosekWilkieSky.h"
#include "ShadowMapRenderer.h"
#include "PointShadowMapRenderer.h"
#include "PointShadowSettings.h"
#include "VirtualShadowMapRenderer.h"
#include "LightClusterRenderer.h"
#include "DeferredLightingPass.h"
#include "TimeOfDaySettings.h"
#include "RtGlobalIllumination.h"
#include "RtGISettings.h"
#include "SMAARenderer.h"
#include "SMAASettings.h"
#include "AnalyticEdgeAARenderer.h"
#include "AnalyticEdgeAASettings.h"
#include "AarRenderer.h"
#include "AarSettings.h"
#include "MsaaSettings.h"
#include "RadianceCascadesRenderer.h"
#include "RadianceCascadesSettings.h"
#include "RadianceProbeRenderer.h"
#include "VoxelGIRenderer.h"
#include "RadianceProbeSettings.h"
#include "RtAmbientOcclusion.h"
#include "RtShadows.h"
#include "PathTracer.h"
#include "PathTracerSettings.h"
#include "RtAOSettings.h"
#include "RtShadowSettings.h"
#include "AgxTonemapper.h"
#include "AutoExposure.h"
#include "AgxTonemapSettings.h"
#include "ChromaticAberrationRenderer.h"
#include "ChromaticAberrationSettings.h"
#include "VolumetricFogRenderer.h"
#include "VolumetricFogSettings.h"
#include "VolumetricCloudRenderer.h"
#include "VolumetricCloudSettings.h"
#include "ParticleRenderer.h"
#include "RainRenderer.h"
#include "XeGtaoRenderer.h"
#include "GtaoSettings.h"
#include "SsrRenderer.h"
#include "SsrSettings.h"
#include "SssrRenderer.h"
#include "DpleRenderer.h"
#include "SubsurfaceScattering.h"
#include "SubsurfaceSettings.h"
#include "ShadingSettings.h"
#include "BloomRenderer.h"
#include "FftBloomRenderer.h"
#include "LensFlareRenderer.h"
#include "SkyGlareRenderer.h"
#include "LensFlareSettings.h"
#include "BloomSettings.h"
#include "ImageSharpenRenderer.h"
#include "SharpenSettings.h"
#include "GBufferViewRenderer.h"
#include "TerrainRenderer.h"
#include "VegetationRenderer.h"
#include "VirtualGeometryRenderer.h"
#include "WindSettings.h"
#include "WaterRenderer.h"
#include "RendererTimingSnapshot.h"
#include "DlssRenderer.h"
#include "DlssSettings.h"
#include "FsrRenderer.h"
#include "FsrFrameGeneration.h"
#include "FsrSettings.h"
#include "XessRenderer.h"
#include "XessFrameGeneration.h"
#include "XessSettings.h"
#include "MotionVectorRenderer.h"
#include "GameHost.h"
#include "System/SteamAchievements.h"
#include "GameProjectSettings.h"
#include "PlayerCollision.h"
#include "ScenePhysics.h"
#include "RmlUiRenderer.h"
#include "System/NodeGraphRuntime.h"
#include "CinematicWorldHost.h"
#include "VideoLayer.h"
#include "..\Ptero-Engine\EditorCamera.h"
#include "..\Ptero-Engine\RenderInterfaces.h"

#include "../QtUi/UiTypes.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Declares a scoped CPU timer for the enclosing render pass. The entry it adds
// shows up in the editor's Resource Debug panel.
#define PTERO_PASS_TIMER_CONCAT_INNER(a, b) a##b
#define PTERO_PASS_TIMER_CONCAT(a, b) PTERO_PASS_TIMER_CONCAT_INNER(a, b)
#define PTERO_SCOPED_PASS_TIMER(category, name) \
    const DX12SceneRenderer::PassTimer PTERO_PASS_TIMER_CONCAT(pteroPassTimer, __LINE__)(this, category, name)

class DX12Pipeline final : public IPipeline
{
public:
    DX12Pipeline() = default;

    const wchar_t* GetDebugName() const override
    {
        return mDebugName.c_str();
    }

    void SetDebugName(const std::wstring& debugName)
    {
        mDebugName = debugName;
    }

    ID3D12RootSignature* GetRootSignature() const
    {
        return mRootSignature.Get();
    }

    ID3D12PipelineState* GetPipelineState() const
    {
        return mPipelineState.Get();
    }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

private:
    std::wstring mDebugName;
};

class AudioManager;

// Standalone game only (GameSettings.cpp): moves the host window to the display mode in
// the player's settings.cfg. Called before the first frame, so nothing is ever shown in a
// window that later jumps to fullscreen.
void ApplySavedGameDisplayMode();

class DX12SceneRenderer
{
public:
    using ProgressCallback = void(__stdcall*)(const wchar_t* message);

    struct Vertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT4 Color;
    };

    struct alignas(256) SceneConstants
    {
        DirectX::XMFLOAT4X4 ModelViewProjection;
    };

    struct BufferResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> DefaultBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> UploadBuffer;
    };

    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Render(ID3D12GraphicsCommandList* commandList);
    void Shutdown();
    bool ResizeSceneTarget(UINT width, UINT height);
    bool ApplyPendingMsaaSettings();
    void ClearCustomSceneResolution();
    void SetProgressCallback(ProgressCallback callback)
    {
        mProgressCallback = callback;
    }

    void SetCameraMovementSpeed(float movementSpeed)
    {
        mCamera.SetMovementSpeed(movementSpeed);
    }

    float GetCameraMovementSpeed() const
    {
        return mCamera.GetMovementSpeed();
    }

    void SetCameraTransform(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& rotation)
    {
        mCamera.SetPosition(position);
        mCamera.SetRotation(rotation.x, rotation.y, rotation.z);
        mTaaSettings.ResetHistory = true;
    }

    void SetGridEnabled(bool isEnabled)
    {
        mGridEnabled = isEnabled;
    }

    bool IsGridEnabled() const
    {
        return mGridEnabled;
    }

    void SetWireframeEnabled(bool isEnabled)
    {
        mEntityMeshRenderer.SetWireframeEnabled(isEnabled);
    }

    bool IsWireframeEnabled() const
    {
        return mEntityMeshRenderer.IsWireframeEnabled();
    }

    // Live handles for the cvar registry. A cvar holds a pointer into the very
    // variable the panels edit, so "rtgi.enabled false" and unticking the box in
    // the UI are the same write and can never disagree.
    bool& GetGridEnabledRef() { return mGridEnabled; }
    bool& GetWireframeEnabledRef() { return mEntityMeshRenderer.GetWireframeEnabledRef(); }

    // For the device-removed report: what was resident, and where. The mesh buffers
    // come from the mesh renderer; the render targets are this class's own, and are
    // the things a geometry-pass draw writes through.
    void LogLiveGpuBufferRanges() const;
    // The pass each GPU event ordinal belongs to, so a device-removed report's
    // "inside BeginEvent #N" can be read as a pass name.
    void LogPassOrder() const;
    void  InvalidateEntityPipeline() { mEntityMeshRenderer.InvalidatePipeline(); }
    float& GetViewDistanceMetersRef() { return mViewDistanceMeters; }

    // DLSS, FSR, XeSS, TAA upsampling (TAAU) and AAR fill the same slot in the post
    // chain, between TAA/SMAA and sharpening. Only one runs a frame; when several
    // are switched on the first of DLSS, FSR, XeSS, TAAU, AAR takes it.
    bool IsDlssUpscalerActive() const
    {
        return mDlssSettings.Enabled && mDlssRenderer.IsAvailable();
    }

    bool IsFsrUpscalerActive() const
    {
        return !IsDlssUpscalerActive() && mFsrSettings.Enabled && mFsrRenderer.IsAvailable();
    }

    bool IsXessUpscalerActive() const
    {
        return !IsDlssUpscalerActive() && !IsFsrUpscalerActive()
            && mXessSettings.Enabled && mXessRenderer.IsAvailable();
    }

    // The vendor upscalers: they replace TAA outright and take motion vectors.
    bool IsExternalUpscalerActive() const
    {
        return IsDlssUpscalerActive() || IsFsrUpscalerActive() || IsXessUpscalerActive();
    }

    // TAA itself upscaling from ScreenPercentage, Unreal's TAAU.
    bool IsTaaUpscalerActive() const
    {
        return !IsExternalUpscalerActive() && mTaaSettings.Enabled && mTaaSettings.Upscaling
            && mTaaRenderer.IsInitialized();
    }

    // AAR, Analytic-Aware Reconstruction: the spatial upscaler built on the G-Buffer's
    // triangle edges (AarSettings.h). Needs the edges, so not without SM 6.1 barycentrics
    // and not under MSAA.
    bool IsAarUpscalerActive() const
    {
        return !IsExternalUpscalerActive() && !IsTaaUpscalerActive()
            && mAarSettings.Enabled && !mAarInitFailed && !mMsaaSettings.Enabled
            && AnalyticEdge::IsSupported();
    }

    // While an upscaler owns the slot, plain TAA and SMAA are skipped: it does their job.
    bool UpscalerOwnsPostAaOutput() const
    {
        return IsExternalUpscalerActive() || IsTaaUpscalerActive() || IsAarUpscalerActive();
    }

    bool IsUpscalerOutputAvailable() const
    {
        if (IsDlssUpscalerActive())
            return mDlssRenderer.IsInitialized() && !mDlssRenderer.IsEvaluationBypassed();
        if (IsFsrUpscalerActive())
            return mFsrRenderer.HasOutput();
        if (IsXessUpscalerActive())
            return mXessRenderer.HasOutput();
        if (IsAarUpscalerActive())
            return mAarRenderer.HasOutput();
        return IsTaaUpscalerActive();
    }

    ID3D12Resource* GetUpscalerOutputResource() const
    {
        if (IsDlssUpscalerActive()) return mDlssRenderer.GetOutputResource();
        if (IsFsrUpscalerActive())  return mFsrRenderer.GetOutputResource();
        if (IsXessUpscalerActive()) return mXessRenderer.GetOutputResource();
        if (IsAarUpscalerActive())  return mAarRenderer.GetOutputResource();
        return mTaaRenderer.GetOutputResource();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetUpscalerOutputCpuSrv() const
    {
        if (IsDlssUpscalerActive()) return mDlssRenderer.GetOutputCpuSrv();
        if (IsFsrUpscalerActive())  return mFsrRenderer.GetOutputCpuSrv();
        if (IsXessUpscalerActive()) return mXessRenderer.GetOutputCpuSrv();
        if (IsAarUpscalerActive())  return mAarRenderer.GetOutputCpuSrv();
        return mTaaRenderer.GetOutputCpuSrv();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetUpscalerOutputGpuSrv() const
    {
        if (IsDlssUpscalerActive()) return mDlssRenderer.GetOutputGpuSrv();
        if (IsFsrUpscalerActive())  return mFsrRenderer.GetOutputGpuSrv();
        if (IsXessUpscalerActive()) return mXessRenderer.GetOutputGpuSrv();
        if (IsAarUpscalerActive())  return mAarRenderer.GetOutputGpuSrv();
        return mTaaRenderer.GetOutputGpuSrv();
    }

    UiTextureID GetUpscalerOutputTextureId() const
    {
        if (IsDlssUpscalerActive()) return mDlssRenderer.GetOutputTextureId();
        if (IsFsrUpscalerActive())  return mFsrRenderer.GetOutputTextureId();
        if (IsXessUpscalerActive()) return mXessRenderer.GetOutputTextureId();
        if (IsAarUpscalerActive())  return mAarRenderer.GetOutputTextureId();
        return mTaaRenderer.GetOutputTextureId();
    }

    // Returns the final scene output for display:
    //   AgX tonemapped → Bloom composited → sharpened image → DLSS/FSR/XeSS/TAAU upscaled → SMAA resolved → TAA resolved → raw scene colour (in priority order when enabled).
    UiTextureID GetSceneTextureId() const
    {
        const bool upscalerOwnsPostAaOutput = UpscalerOwnsPostAaOutput();
        const bool upscalerOutputAvailable = IsUpscalerOutputAvailable();
        const bool imageSharpenOutputAvailable = mSharpenSettings.ImageSharpeningEnabled
            && mImageSharpenRenderer.IsInitialized()
            && (!upscalerOwnsPostAaOutput || upscalerOutputAvailable);

        if (IsGBufferViewActive())
            return mGBufferViewRenderer.GetOutputTextureId();
        if (mRtaoSettings.DebugView > 0)
            return mSceneTextureId;
        // Chromatic aberration is the last thing applied, so when it runs its output is
        // the finished frame regardless of what fed it.
        if (mChromaticAberrationSettings.Enabled && mChromaticAberrationRenderer.IsInitialized())
            return mChromaticAberrationRenderer.GetOutputTextureId();
        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
            return mAgxTonemapper.GetOutputTextureId();
        if (IsBloomStageActive())
            return GetBloomStageOutputTextureId();
        if (imageSharpenOutputAvailable)
            return mImageSharpenRenderer.GetOutputTextureId();
        if (upscalerOutputAvailable)
            return GetUpscalerOutputTextureId();
        if (!upscalerOwnsPostAaOutput && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
            return mSmaaRenderer.GetOutputTextureId();
        if (!upscalerOwnsPostAaOutput && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
            return mTaaRenderer.GetOutputTextureId();
        return mSceneTextureId;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetSceneTextureHandle() const
    {
        const bool upscalerOwnsPostAaOutput = UpscalerOwnsPostAaOutput();
        const bool upscalerOutputAvailable = IsUpscalerOutputAvailable();
        const bool imageSharpenOutputAvailable = mSharpenSettings.ImageSharpeningEnabled
            && mImageSharpenRenderer.IsInitialized()
            && (!upscalerOwnsPostAaOutput || upscalerOutputAvailable);

        if (IsGBufferViewActive())
            return mGBufferViewRenderer.GetOutputGpuSrv();
        if (mRtaoSettings.DebugView > 0)
            return mSceneSrvGpuHandle;
        if (mChromaticAberrationSettings.Enabled && mChromaticAberrationRenderer.IsInitialized())
            return mChromaticAberrationRenderer.GetOutputGpuSrv();
        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
            return mAgxTonemapper.GetOutputGpuSrv();
        if (IsBloomStageActive())
            return GetBloomStageOutputGpuSrv();
        if (imageSharpenOutputAvailable)
            return mImageSharpenRenderer.GetOutputGpuSrv();
        if (upscalerOutputAvailable)
            return GetUpscalerOutputGpuSrv();
        if (!upscalerOwnsPostAaOutput && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
            return mSmaaRenderer.GetOutputGpuSrv();
        if (!upscalerOwnsPostAaOutput && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
            return mTaaRenderer.GetOutputGpuSrv();
        return mSceneSrvGpuHandle;
    }

    TaaSettings& GetTaaSettings() { return mTaaSettings; }
    const TaaSettings& GetTaaSettings() const { return mTaaSettings; }

    SMAASettings& GetSmaaSettings() { return mSmaaSettings; }
    const SMAASettings& GetSmaaSettings() const { return mSmaaSettings; }

    AnalyticEdgeAASettings& GetAnalyticEdgeAaSettings() { return mAnalyticEdgeAaSettings; }
    const AnalyticEdgeAASettings& GetAnalyticEdgeAaSettings() const { return mAnalyticEdgeAaSettings; }
    // False when the GPU lacks SM 6.1 barycentrics, so the G-Buffer carries no edges.
    bool IsAnalyticEdgeAaSupported() const { return AnalyticEdge::IsSupported(); }

    AarSettings& GetAarSettings() { return mAarSettings; }
    const AarSettings& GetAarSettings() const { return mAarSettings; }
    // Why AAR is not running despite being enabled, or null.
    const char* GetAarStatus() const;

    MsaaSettings& GetMsaaSettings() { return mMsaaSettings; }
    const MsaaSettings& GetMsaaSettings() const { return mMsaaSettings; }

    SharpenSettings& GetSharpenSettings() { return mSharpenSettings; }
    const SharpenSettings& GetSharpenSettings() const { return mSharpenSettings; }

    TimeOfDaySettings& GetTimeOfDaySettings() { return mTimeOfDaySettings; }
    const TimeOfDaySettings& GetTimeOfDaySettings() const { return mTimeOfDaySettings; }
    // The time of day as last evaluated: light directions, exposure, and so on.
    const HosekWilkieResult& GetTimeOfDayState() const { return mHosekResult; }

    GlobalIlluminationMode& GetGlobalIlluminationMode() { return mGlobalIlluminationMode; }
    const GlobalIlluminationMode& GetGlobalIlluminationMode() const { return mGlobalIlluminationMode; }

    RtGISettings& GetRtgiSettings() { return mRtgiSettings; }
    const RtGISettings& GetRtgiSettings() const { return mRtgiSettings; }

    RadianceCascadesSettings& GetRadianceCascadesSettings() { return mRadianceCascadesSettings; }
    const RadianceCascadesSettings& GetRadianceCascadesSettings() const { return mRadianceCascadesSettings; }

    RadianceProbeSettings& GetProbeSettings() { return mProbeSettings; }
    const RadianceProbeSettings& GetProbeSettings() const { return mProbeSettings; }

    VoxelGISettings& GetVoxelGiSettings() { return mVoxelGiSettings; }
    const VoxelGISettings& GetVoxelGiSettings() const { return mVoxelGiSettings; }
    // Whether the GPU can run the ray traced GI paths (DXR 1.1 inline ray queries).
    bool IsRayTracingSupported() const;
    // What the voxel GI did last frame, for the settings panel.
    VoxelGIStatus GetVoxelGiStatus() const;

    RtAOSettings& GetRtaoSettings() { return mRtaoSettings; }
    const RtAOSettings& GetRtaoSettings() const { return mRtaoSettings; }

    RtShadowSettings& GetRtShadowSettings() { return mRtShadowSettings; }
    const RtShadowSettings& GetRtShadowSettings() const { return mRtShadowSettings; }

    PathTracerSettings& GetPathTracerSettings() { return mPathTracerSettings; }
    const PathTracerSettings& GetPathTracerSettings() const { return mPathTracerSettings; }
    // One line for the settings panel: samples so far, or why it is not running.
    const char* GetPathTracerStatus() const { return mPathTracerStatus.c_str(); }

    AgxTonemapSettings& GetAgxSettings() { return mAgxSettings; }
    const AgxTonemapSettings& GetAgxSettings() const { return mAgxSettings; }

    VolumetricFogSettings& GetVolumetricFogSettings() { return mVolumetricFogSettings; }
    const VolumetricFogSettings& GetVolumetricFogSettings() const { return mVolumetricFogSettings; }

    VolumetricCloudSettings& GetVolumetricCloudSettings() { return mVolumetricCloudSettings; }
    const VolumetricCloudSettings& GetVolumetricCloudSettings() const { return mVolumetricCloudSettings; }

    GtaoSettings& GetGtaoSettings() { return mGtaoSettings; }
    const GtaoSettings& GetGtaoSettings() const { return mGtaoSettings; }

    SsrSettings& GetSsrSettings() { return mSsrSettings; }
    const SsrSettings& GetSsrSettings() const { return mSsrSettings; }
    SubsurfaceSettings& GetSubsurfaceSettings() { return mSubsurfaceSettings; }
    const SubsurfaceSettings& GetSubsurfaceSettings() const { return mSubsurfaceSettings; }
    ShadingSettings& GetShadingSettings() { return mShadingSettings; }
    const ShadingSettings& GetShadingSettings() const { return mShadingSettings; }
    DpleSettings& GetDpleSettings() { return mDpleSettings; }
    const DpleSettings& GetDpleSettings() const { return mDpleSettings; }
    // Why DPLE is not running although it is enabled, or null when it is fine.
    const char* GetDpleErrorMessage() const
    {
        return mDpleRenderer.HasInitFailed() ? mDpleRenderer.GetLastErrorMessage() : nullptr;
    }
    // Whether the ray-traced subsurface path can run on this device (DXR 1.1), once the
    // pass has initialised; before that it reports true so the option stays selectable.
    bool IsSubsurfaceRayTracingSupported() const
    {
        return !mSubsurfaceRenderer.IsInitialized() || mSubsurfaceRenderer.SupportsRayTracing();
    }

    ChromaticAberrationSettings& GetChromaticAberrationSettings() { return mChromaticAberrationSettings; }
    const ChromaticAberrationSettings& GetChromaticAberrationSettings() const { return mChromaticAberrationSettings; }

    BloomSettings& GetBloomSettings() { return mBloomSettings; }
    const BloomSettings& GetBloomSettings() const { return mBloomSettings; }

    LensFlareSettings& GetLensFlareSettings() { return mLensFlareSettings; }
    const LensFlareSettings& GetLensFlareSettings() const { return mLensFlareSettings; }
    const LensFlareRenderer& GetLensFlareRenderer() const { return mLensFlareRenderer; }

    // ---- The bloom stage ---------------------------------------------------
    // Bloom (either method) and then the lens flare on top of it. Everything after the
    // stage - auto exposure, AgX, chromatic aberration, display, screenshots - asks these
    // for its input rather than naming a renderer, so the stage can change shape freely.
    bool IsBloomActive() const
    {
        if (!mBloomSettings.Enabled)
            return false;
        return mBloomSettings.Method == BloomMethod::FftConvolution
            ? mFftBloomRenderer.IsInitialized()
            : mBloomRenderer.IsInitialized();
    }

    bool IsLensFlareActive() const
    {
        return mLensFlareSettings.Enabled && mLensFlareRenderer.IsInitialized();
    }

    // The sun's and the moon's own glare (SkyGlareRenderer), between the bloom and the
    // lens flare.
    bool IsSkyGlareActive() const
    {
        return mTimeOfDaySettings.Enabled
            && (mTimeOfDaySettings.SunGlare.Enabled || mTimeOfDaySettings.MoonGlare.Enabled)
            && mSkyGlareRenderer.IsInitialized();
    }

    bool IsBloomStageActive() const { return IsLensFlareActive() || IsSkyGlareActive() || IsBloomActive(); }

    ID3D12Resource* GetBloomStageOutputResource() const
    {
        if (IsLensFlareActive())
            return mLensFlareRenderer.GetOutputResource();
        if (IsSkyGlareActive())
            return mSkyGlareRenderer.GetOutputResource();
        return mBloomSettings.Method == BloomMethod::FftConvolution
            ? mFftBloomRenderer.GetOutputResource() : mBloomRenderer.GetOutputResource();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetBloomStageOutputCpuSrv() const
    {
        if (IsLensFlareActive())
            return mLensFlareRenderer.GetOutputCpuSrv();
        if (IsSkyGlareActive())
            return mSkyGlareRenderer.GetOutputCpuSrv();
        return mBloomSettings.Method == BloomMethod::FftConvolution
            ? mFftBloomRenderer.GetOutputCpuSrv() : mBloomRenderer.GetOutputCpuSrv();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetBloomStageOutputGpuSrv() const
    {
        if (IsLensFlareActive())
            return mLensFlareRenderer.GetOutputGpuSrv();
        if (IsSkyGlareActive())
            return mSkyGlareRenderer.GetOutputGpuSrv();
        return mBloomSettings.Method == BloomMethod::FftConvolution
            ? mFftBloomRenderer.GetOutputGpuSrv() : mBloomRenderer.GetOutputGpuSrv();
    }

    UiTextureID GetBloomStageOutputTextureId() const
    {
        if (IsLensFlareActive())
            return mLensFlareRenderer.GetOutputTextureId();
        if (IsSkyGlareActive())
            return mSkyGlareRenderer.GetOutputTextureId();
        return mBloomSettings.Method == BloomMethod::FftConvolution
            ? mFftBloomRenderer.GetOutputTextureId() : mBloomRenderer.GetOutputTextureId();
    }

    PointShadowSettings& GetPointShadowSettings() { return mPointShadowSettings; }
    const PointShadowSettings& GetPointShadowSettings() const { return mPointShadowSettings; }

    // Clustered light lists (LightClusterRenderer): whether they are used, the bounce-hit
    // grid's cell size, and what the last frame built.
    bool& GetClusteredLights() { return mClusteredLights; }
    float& GetLightGridCellSize() { return mLightGridCellSize; }
    const LightClusterGpuConstants& GetLightClusterConstants() const { return mLightClusterRenderer.GetConstants(); }
    int GetSceneLightCount() const { return mNumCachedPointLights; }

    VirtualShadowMapSettings& GetVirtualShadowMapSettings() { return mVirtualShadowMapSettings; }
    const VirtualShadowMapSettings& GetVirtualShadowMapSettings() const { return mVirtualShadowMapSettings; }
    const VirtualShadowMapRenderer::Statistics& GetVirtualShadowMapStatistics() const { return mVirtualShadowMap.GetStatistics(); }
    bool IsVirtualShadowMapActive() const { return mVirtualShadowMap.IsActiveThisFrame(); }

    DlssSettings& GetDlssSettings() { return mDlssSettings; }
    const DlssSettings& GetDlssSettings() const { return mDlssSettings; }

    FsrSettings& GetFsrSettings() { return mFsrSettings; }
    const FsrSettings& GetFsrSettings() const { return mFsrSettings; }

    // Driven from the frame loop, which owns the swap chain it presents through.
    FsrFrameGeneration& GetFrameGeneration() { return mFrameGeneration; }

    // True when the settings ask for frame generation and the API can provide it,
    // i.e. when the frame loop should install FSR's proxy swap chain.
    bool WantsFrameGenerationSwapChain()
    {
        return mFsrSettings.FrameGeneration && mFrameGeneration.IsApiAvailable();
    }

    FsrRuntimeStatus GetFsrRuntimeStatus();

    XessSettings& GetXessSettings() { return mXessSettings; }
    const XessSettings& GetXessSettings() const { return mXessSettings; }

    // Driven from the frame loop, like FSR's.
    XessFrameGeneration& GetXessFrameGeneration() { return mXessFrameGeneration; }

    // Whether the frame loop should install XeSS-FG's proxy swap chain. FSR frame
    // generation takes precedence when both are switched on.
    bool WantsXessFrameGenerationSwapChain()
    {
        return !WantsFrameGenerationSwapChain() && mXessSettings.FrameGeneration && mXessFrameGeneration.IsApiAvailable();
    }

    XessRuntimeStatus GetXessRuntimeStatus();

    UiTextureID GetPointShadowDebugTextureId() const
    {
        return static_cast<UiTextureID>(mPointShadowMapRenderer.GetShadowTextureArraySrvGpuHandle().ptr);
    }

    const EditorCamera& GetCamera() const
    {
        return mCamera;
    }

    TerrainRenderer& GetTerrainRenderer() { return mTerrainRenderer; }
    const TerrainRenderer& GetTerrainRenderer() const { return mTerrainRenderer; }
    WaterRenderer& GetWaterRenderer() { return mWaterRenderer; }
    const WaterRenderer& GetWaterRenderer() const { return mWaterRenderer; }
    EntityMeshRenderer& GetEntityMeshRenderer() { return mEntityMeshRenderer; }
    VegetationRenderer& GetVegetationRenderer() { return mVegetationRenderer; }
    const VegetationRenderer& GetVegetationRenderer() const { return mVegetationRenderer; }
    VirtualGeometryRenderer& GetVirtualGeometryRenderer() { return mVirtualGeometryRenderer; }
    const VirtualGeometryRenderer& GetVirtualGeometryRenderer() const { return mVirtualGeometryRenderer; }

    WindSettings& GetWindSettings() { return mWindSettings; }
    const WindSettings& GetWindSettings() const { return mWindSettings; }

    UINT GetSceneWidth() const { return mSceneWidth; }
    UINT GetSceneHeight() const { return mSceneHeight; }
    float GetViewDistanceMeters() const { return mViewDistanceMeters; }
    void SetViewDistanceMeters(float distanceMeters)
    {
        mViewDistanceMeters = (std::clamp)(distanceMeters, 500.0f, 100000.0f);
        mCamera.SetFarPlane(mViewDistanceMeters);
    }
    float GetMsaaResolveTimeMs() const { return mMsaaResolveTimeMs; }
    const RendererTimingSnapshot& GetRendererTimingSnapshot() const { return mRendererTimingSnapshot; }

    // RAII timer for one render pass's CPU command-recording cost. Declare one
    // at the top of a pass's scope (the PTERO_SCOPED_PASS_TIMER macro below
    // does it); it appends an entry to the frame's snapshot on destruction.
    //
    // Scopes are meant to be flat and non-overlapping. Nesting one inside
    // another would double-count the inner pass against the total.
    class PassTimer
    {
    public:
        PassTimer(DX12SceneRenderer* renderer, const char* category, const char* name)
            : mRenderer(renderer)
            , mCategory(category)
            , mName(name)
            , mStart(std::chrono::steady_clock::now())
        {
            // Also a GPU event, so a device-removed report can name the pass a hung
            // or faulting operation belongs to: DRED keeps the event's string as
            // breadcrumb context, where on its own it only knows "Dispatch #134".
            mCommandList = renderer ? renderer->mMarkerCommandList : nullptr;
            if (mCommandList != nullptr)
            {
                // DRED does not always keep the event strings, so the order is kept
                // here too: the Nth BeginEvent in a command list is entry N.
                renderer->mCurrentPassOrder.push_back({ category, name });
                mOrdinal = static_cast<UINT>(renderer->mCurrentPassOrder.size());
                renderer->WriteGpuProgress(1, mOrdinal, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN);
                mGpuQuery = renderer->BeginGpuPassTimestamp(category, name);
                wchar_t label[96] = {};
                const int written = swprintf_s(label, L"%hs: %hs", category, name);
                if (written > 0)
                    mCommandList->BeginEvent(0, label, static_cast<UINT>((written + 1) * sizeof(wchar_t)));
                else
                    mCommandList = nullptr;
            }
        }

        ~PassTimer()
        {
            if (mCommandList != nullptr)
            {
                mCommandList->EndEvent();
                mRenderer->WriteGpuProgress(2, mOrdinal, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT);
            }
            if (mRenderer != nullptr)
                mRenderer->EndGpuPassTimestamp(mGpuQuery);

            if (mRenderer == nullptr)
                return;

            const float milliseconds = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - mStart).count();
            mRenderer->RecordPassTiming(mCategory, mName, milliseconds);
        }

        PassTimer(const PassTimer&) = delete;
        PassTimer& operator=(const PassTimer&) = delete;

    private:
        DX12SceneRenderer* mRenderer;
        ID3D12GraphicsCommandList* mCommandList = nullptr;
        UINT               mOrdinal = 0;
        UINT               mGpuQuery = UINT_MAX;
        const char*        mCategory;
        const char*        mName;
        std::chrono::steady_clock::time_point mStart;
    };

    void FrameAabbInView(const DirectX::XMFLOAT3& aabbMin, const DirectX::XMFLOAT3& aabbMax);

    // ---- Play mode -------------------------------------------------------
    // While a play session runs, Game.dll drives the camera instead of the
    // editor fly-cam; the editor pose is restored when the session stops.
    // separateWindow picks between the two play modes: its own top-level window, or
    // inside the editor viewport with the panels still live.
    bool StartGame(bool separateWindow);
    void StopGame();
    void ToggleGame(bool separateWindow);
    // True for a real running session AND for the logo intro that precedes it, so Play
    // can't be double-pressed and Stop works during either.
    bool IsGameRunning() const { return mGameHost.IsRunning() || IsGameIntroPlaying(); }
    // Only the game proper: false while the intro is still playing.
    bool IsGameSessionRunning() const { return mGameHost.IsRunning(); }
    // Standalone only, right after the level has loaded: applies the player's saved
    // graphics/audio settings before any warm-up frame or intro video, so nothing changes
    // (upscaler, AA, quality) once the game is on screen. Idempotent.
    void PrepareStandaloneGameSettings();
    // Which mode the running session was started in. Meaningless while stopped.
    bool IsGameInSeparateWindow() const { return mGameInSeparateWindow; }
    bool mGameStopRequested = false;
    std::array<bool, 256> mFarkleKeys{};

    // ---- Game intro (Pterosoft logo -> engine logo -> reconstruction logo) ----
    // A standalone game plays it first of all, over black, before the shader cache and the
    // level load (DX12RendererAPI's standalone startup), on the same VideoLayer the Node
    // Graph's video nodes use (see GetVideoLayer()). PlayStartupIntro() plays the first
    // clip and returns immediately - false when there is nothing to play; UpdateGameIntro()
    // (driven every frame from UpdateCamera(), since the game camera/host do not exist yet)
    // advances it until every clip has finished or been skipped. game.skipintro (see
    // EngineCVars.cpp) bypasses it entirely. The editor's Play has no intro.
    enum class GameIntroStage { None, Playing };
    bool PlayStartupIntro();
    bool IsGameIntroPlaying() const { return mGameIntroStage != GameIntroStage::None; }
    void UpdateGameIntro(float deltaTime);
    bool BeginGameAfterIntro(bool separateWindow);
    bool& GetSkipGameIntroRef() { return mSkipGameIntro; }
    GameIntroStage mGameIntroStage = GameIntroStage::None;
    // Index into the intro's clips of the one playing.
    int mGameIntroClip = 0;
    bool mGameIntroSkipKeyWasDown = false;
    bool mSkipGameIntro = false;

    // Registered by the host once FMOD is up. Null until then, and null for good when
    // audio failed to initialise, so every use is guarded: play sessions run silently
    // rather than not at all.
    void SetAudioManager(AudioManager* audioManager) { mAudioManager = audioManager; }

    // Single predicate for "the game UI should be on screen this frame", shared by the
    // UI render pass, the viewport composite and input forwarding so the three can never
    // disagree. The UI is a play-mode feature: game code decides what it shows, so it
    // stays dark while the editor is the thing being used.
    bool IsGameUiActive() const
    {
        return mRmlUiRenderer.IsInitialized() && mRmlUiRenderer.IsVisible() && mGameHost.IsRunning();
    }

    // True while the editor's UI Editor panel is open. It makes the RmlUi pass
    // run so the panel has something to show; compositing over the viewport
    // stays tied to a running play session and is unaffected.
    void SetUiPreviewActive(bool active) { mRmlUiRenderer.SetPreviewActive(active); }
    bool IsUiPreviewActive() const
    {
        return mRmlUiRenderer.IsInitialized() && mRmlUiRenderer.IsPreviewActive();
    }

    // Startup screens of a standalone game (Data/UI/Farkle/shader-preload.rml, then
    // loading.rml), shown by the host before the play session exists - there is no
    // Game.dll yet to drive the UI. Drawn through the same RmlUi target as the game UI and
    // composited even while the scene itself is still held back. StartGame takes them down.
    bool ShowStartupScreen(const std::string& document);
    // fraction 0..1. Detail is the small line under the bar; empty leaves it as it is.
    void SetStartupScreenProgress(float fraction, const std::string& status, const std::string& detail = {});
    void HideStartupScreen();
    bool IsStartupScreenActive() const { return mStartupScreenActive && mRmlUiRenderer.IsInitialized(); }

    RmlUiRenderer& GetRmlUiRenderer() { return mRmlUiRenderer; }
    const RmlUiRenderer& GetRmlUiRenderer() const { return mRmlUiRenderer; }

    // Full-screen video driven by the Node Graph's video nodes. Composited over the scene
    // and under the game UI by the viewport blit.
    VideoLayer& GetVideoLayer() { return mVideoLayer; }

    // The level's visual script, owned by the editor. The renderer only reads it, and
    // only at the moment a play session starts - the runtime works on its own copy from
    // then on, so the graph can keep being edited while it runs.
    void SetNodeGraph(const NodeGraphDocument* document) { mNodeGraphSource = document; }
    const NodeGraphRuntime& GetNodeGraphRuntime() const { return mNodeGraphRuntime; }

    // The play session's cinematic runtime (Ptero Sequencer assets). Players started here
    // live until the session stops.
    CinematicSystem& GetCinematics() { return mCinematics; }

    // Called every editor frame right after the fly camera has taken its input and before
    // the view is built: the editor pilots camera entities and locks the view to a
    // cinematic cut camera through this. Not called during a play session.
    void SetEditorCameraHook(std::function<void(EditorCamera&)> hook) { mEditorCameraHook = std::move(hook); }

    // Offline rendering (Render Sequence): every frame advances the scene clock - light
    // styles, particles, water, exposure adaptation - by exactly this many seconds rather
    // than by wall-clock time, so frame N of a render looks the same however long the GPU
    // took over it. Zero returns to real time.
    void SetFixedFrameDelta(float seconds) { mFixedFrameDeltaSeconds = (std::max)(0.0f, seconds); }

    const std::string& GetGameStartErrorMessage() const { return mGameHost.GetLastErrorMessage(); }
    const std::string& GetGameName() const { return mGameHost.GetGameName(); }

    const char* GetLastErrorMessage() const
    {
        return mLastErrorMessage.empty() ? nullptr : mLastErrorMessage.c_str();
    }

    // Returns UiTextureID handles for each G-Buffer layer and the GI accumulation
    // buffer so the editor can display them in a debug visualizer window.
    UiTextureID GetGBufferAlbedoTextureId()   const { return static_cast<UiTextureID>(mDeferredLightingPass.GetDebugSrvs().Albedo.ptr); }
    UiTextureID GetGBufferNormalTextureId()   const { return static_cast<UiTextureID>(mDeferredLightingPass.GetDebugSrvs().Normal.ptr); }
    UiTextureID GetGBufferMaterialTextureId() const { return static_cast<UiTextureID>(mDeferredLightingPass.GetDebugSrvs().Material.ptr); }
    // Depth SRV lives on the scene renderer (R32_FLOAT view of the depth target).
    UiTextureID GetGBufferDepthTextureId()    const { return static_cast<UiTextureID>(mDepthDebugSrvGpuHandle.ptr != 0 ? mDepthDebugSrvGpuHandle.ptr : mDepthSrvGpuHandle.ptr); }
    UiTextureID GetGiAccumTextureId()         const { return static_cast<UiTextureID>(mRtgiRenderer.GetOutputSrv().ptr); }

    // The viewport's G-Buffer view mode (View Mode > G-Buffer). While one is set the
    // viewport shows that channel in place of the finished frame. Editor-only state,
    // never saved with a level.
    GBufferView GetGBufferView() const { return mGBufferView; }
    void SetGBufferView(GBufferView view) { mGBufferView = view; }
    bool IsGBufferViewActive() const
    {
        return mGBufferView != GBufferView::Off && mGBufferViewRenderer.IsInitialized();
    }

    // Transitions the scene depth buffer to PIXEL_SHADER_RESOURCE so Ui can
    // sample it in the G-Buffer debug window.  Call before Ui::Render() and
    // pair with TransitionDepthAfterRead() to restore DEPTH_WRITE afterwards.
    void TransitionDepthForRead(ID3D12GraphicsCommandList* commandList);
    void TransitionDepthAfterRead(ID3D12GraphicsCommandList* commandList);

    // Provide the scene renderer with the current entity list so it can render
    // each entity's assigned mesh every frame.
    void SetEntities(std::vector<Entity>* entities)
    {
        mEntities = entities;
        mEntityMeshRenderer.SetEntities(entities);
        mTerrainRenderer.SetEntities(entities);
        mWaterRenderer.SetEntities(entities);
        mVegetationRenderer.SetEntities(entities);
    }

    // Get the final scene color target resource for screenshot capture
    ID3D12Resource* GetSceneColorTargetResource() const
    {
        const bool upscalerOwnsPostAaOutput = UpscalerOwnsPostAaOutput();
        const bool upscalerOutputAvailable = IsUpscalerOutputAvailable();
        const bool imageSharpenOutputAvailable = mSharpenSettings.ImageSharpeningEnabled
            && mImageSharpenRenderer.IsInitialized()
            && (!upscalerOwnsPostAaOutput || upscalerOutputAvailable);

        // Return the final output texture based on which post-process is active
        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
            return mAgxTonemapper.GetOutputResource();
        if (IsBloomStageActive())
            return GetBloomStageOutputResource();
        if (imageSharpenOutputAvailable)
            return mImageSharpenRenderer.GetOutputResource();
        if (upscalerOutputAvailable)
            return GetUpscalerOutputResource();
        if (!upscalerOwnsPostAaOutput && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
            return mSmaaRenderer.GetOutputResource();
        if (!upscalerOwnsPostAaOutput && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
            return mTaaRenderer.GetOutputResource();
        return mSceneColorTarget.Get();
    }

private:
    void ReportProgress(const wchar_t* message) const;
    bool CreatePipeline();
    bool CreateSceneTarget();
    bool EnsureSceneTargetMatchesWindowSize();
    bool ResizeSceneTargetsTo(UINT width, UINT height);
    bool RecreateSceneTargetsForMsaaChange();
    bool CreateDepthDebugResources();
    void RenderDepthDebugPreview(ID3D12GraphicsCommandList* commandList);
    bool CreateResolveMsaaDepthResources();
    void ResolveMsaaDepth(ID3D12GraphicsCommandList* commandList);
    bool CreateGeometry(ID3D12GraphicsCommandList* commandList);
    bool CreateConstantBuffer();
    bool CreateBufferWithUpload(
        ID3D12GraphicsCommandList* commandList,
        const void* initialData,
        UINT64 dataSize,
        D3D12_RESOURCE_STATES finalState,
        BufferResource& outBuffer) const;
    void ReleaseSceneTargetResources(bool waitForGpu = true);
    void UpdateCamera();
    void UpdateGameCamera(float deltaTime, const POINT& mousePosition, bool lookActive);
    void UpdateSceneConstants();
    float ComputeSceneBoundRadius() const;

    // The point lights that get a shadow cubemap this frame, in cubemap order:
    // the first kMaxShadowCastingPointLights shadow casters among the scene's
    // entity lights. When outShadowIndexPerLight
    // is given it receives, per light in gather order, its cubemap index or -1.
    // One rule for the point-shadow pass and for the virtualized geometry
    // culled into those cubemaps, which must agree on the order.
    std::vector<PointShadowMapRenderer::ShadowedPointLight> GatherShadowedPointLights(
        std::vector<int>* outShadowIndexPerLight) const;
    float GetMeshLocalRadius(const Mesh* mesh) const;
    bool IsSceneContentDirtyForTemporal();

    bool mIsInitialized = false;
    ProgressCallback mProgressCallback = nullptr;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    DX12Pipeline mPipeline;
    EditorCamera mCamera;

    // Play-mode state. The editor camera pose is stashed on start so stopping the
    // session puts the viewport back exactly where the artist left it, and the mouse
    // position is tracked here because the game is handed deltas, not absolute pixels.
    GameHost mGameHost;
    AudioManager* mAudioManager = nullptr;
    // Steam achievements and stats, shared by Game.dll and the Node Graph.
    SteamAchievements mAchievements;

    // Game-facing UI. It renders to its own target and is composited over the scene
    // image, so it is deliberately not part of the post-processing chain.
    RmlUiRenderer mRmlUiRenderer;
    VideoLayer mVideoLayer;

    // Visual scripting. The host adapter is the only thing the runtime can reach the
    // engine through, which keeps graph execution from growing renderer dependencies.
    class NodeGraphUiHost final : public NodeGraphHost
    {
    public:
        // The renderer owns everything past UI and video - entities, camera, audio - and
        // a nested class may reach its private members, so the host borrows it whole.
        explicit NodeGraphUiHost(DX12SceneRenderer& owner) : mOwner(owner) {}
        void SetUiRenderer(RmlUiRenderer* uiRenderer) { mUiRenderer = uiRenderer; }
        void SetVideoLayer(VideoLayer* videoLayer) { mVideoLayer = videoLayer; }

        bool ShowUiDocument(const std::string& fileName) override;
        void CloseUiDocument() override;
        bool ReloadUiDocument() override;
        void SetUiVisible(bool visible) override;
        void SetUiInputEnabled(bool enabled) override;
        bool SetUiElementText(const std::string& elementId, const std::string& text) override;
        bool SetUiElementProperty(
            const std::string& elementId,
            const std::string& property,
            const std::string& value) override;
        bool SetUiElementClass(
            const std::string& elementId,
            const std::string& className,
            bool enabled) override;
        bool SetUiElementVisible(const std::string& elementId, bool visible) override;

        bool PlayVideo(const std::string& fileName, bool loop, const std::string& fit) override;
        void PauseVideo() override;
        void ResumeVideo() override;
        void StopVideo() override;
        void SeekVideo(double seconds) override;
        void SetVideoLooping(bool loop) override;
        void SetVideoVolume(double volume) override;
        bool IsVideoPlaying() override;
        double GetVideoTime() override;
        double GetVideoDuration() override;
        bool ConsumeVideoFinished() override;

        std::uint64_t FindEntityByName(const std::string& name) override;
        bool GetEntityName(std::uint64_t entityId, std::string& name) override;
        bool GetEntityTransform(std::uint64_t entityId, NodeGraphTransform& transform) override;
        bool SetEntityTransform(std::uint64_t entityId, const NodeGraphTransform& transform) override;
        bool GetEntityProperty(std::uint64_t entityId, const std::string& property, double& value) override;
        bool SetEntityProperty(std::uint64_t entityId, const std::string& property, double value) override;
        bool SetEntityAudioPlaying(std::uint64_t entityId, bool playing) override;

        bool AddEntityImpulse(std::uint64_t entityId, const double impulse[3]) override;
        bool AddEntityForce(std::uint64_t entityId, const double force[3]) override;
        bool SetEntityVelocity(std::uint64_t entityId, const double velocity[3]) override;
        bool GetEntityVelocity(std::uint64_t entityId, double velocity[3]) override;
        bool SetEntityAngularVelocity(std::uint64_t entityId, const double velocity[3]) override;
        bool Raycast(const double origin[3], const double direction[3], double maxDistance,
                     NodeGraphRayHit& hit) override;
        const std::vector<NodeGraphPhysicsHit>& GetPhysicsHits() override;

        NodeGraphCameraPose GetCamera() override;
        void SetCamera(const NodeGraphCameraPose& pose) override;

        void PlayOneShot(const std::string& eventName) override;
        bool PlayMusic(const std::string& eventName) override;
        void StopMusic() override;
        bool IsMusicPlaying() override;

        bool IsKeyDown(int virtualKey) override;
        bool PollUiClick(std::string& elementId) override;
        void ToggleFullscreen() override;
        bool IsStandalone() override;

        CharacterMovement* GetPlayerCharacter() override;
        void GetMouseDelta(double& x, double& y) override;
        void SetMouseCaptured(bool captured) override;
        bool IsMouseCaptured() override;

        bool UnlockAchievement(const std::string& name) override;
        bool IsAchievementUnlocked(const std::string& name) override;
        bool ClearAchievement(const std::string& name) override;
        bool ShowAchievementProgress(const std::string& name, int progress, int maximum) override;
        bool GetStat(const std::string& name, int& value) override;
        bool SetStat(const std::string& name, int value) override;
        bool IsSteamRunning() override;

    private:
        Entity* FindEntity(std::uint64_t entityId);

        DX12SceneRenderer& mOwner;
        RmlUiRenderer* mUiRenderer = nullptr;
        VideoLayer* mVideoLayer = nullptr;
    };

    NodeGraphUiHost mNodeGraphHost{ *this };
    NodeGraphRuntime mNodeGraphRuntime;
    const NodeGraphDocument* mNodeGraphSource = nullptr;

    // ---- Cinematics ---------------------------------------------------------------
    // Sequences played during a session run against the session's entities. Ticked
    // after the graphs (which may start them) and applied over the game camera: a
    // cutscene's cut camera wins over the player's view for as long as it plays.
    CinematicWorldHost mCinematicHost;
    CinematicSystem mCinematics;
    void UpdateCinematics(float deltaSeconds);
    std::function<void(EditorCamera&)> mEditorCameraHook;
    float mFixedFrameDeltaSeconds = 0.0f;

    // ---- Player (Game Settings) -------------------------------------------------
    // Read from Data/Game/GameSettings.json when play starts, so edits in the Game
    // Settings window apply from the next Play.
    GameProjectSettings mGameProjectSettings;
    // Static meshes and terrain, built at the start of each session for the character.
    PlayerCollision mPlayerCollision;
    // The rigid body simulation (Physics.dll), built at the start of each session and
    // stepped every game frame. Moving bodies live here rather than in mPlayerCollision.
    ScenePhysics mScenePhysics;
    // Set by any character trace this frame: a character is walking, so the physics
    // world gets a proxy capsule for it to shove bodies with.
    mutable bool mPlayerTracedThisFrame = false;
    // The latest physics step's collisions in the Node Graph's form, refreshed after each
    // step and read by both graphs' On Rigid Body Hit on the following tick.
    std::vector<NodeGraphPhysicsHit> mNodeGraphPhysicsHits;
    // The character's world: mPlayerCollision plus the simulation's moving bodies.
    bool TracePlayerWorld(const float origin[3], const float direction[3], float maxDistance,
                          CharacterTraceHit& hit) const;
    static bool TracePlayerWorldCallback(void* user, const float origin[3], const float direction[3],
                                         float maxDistance, CharacterTraceHit* hit);
    // Game.dll addresses entities by list index (GameServices::FindEntity); everything
    // else by Entity::Id. 0 / -1 when there is no such entity.
    std::uint64_t GameEntityId(int index) const;
    int GameEntityIndex(std::uint64_t entityId) const;
    // The player when Game Settings picks a Node Graph controller: the engine owns the
    // character, the controller graph drives it through the Character nodes, and it is
    // stepped after the graphs tick. With the C++ controller Game.dll owns the character
    // and this stays inactive.
    CharacterMovement mGraphCharacter;
    bool mGraphCharacterActive = false;
    NodeGraphRuntime mPlayerControllerRuntime;

    // Where the player spawns: a "PlayerStart" entity if the level has one (its position
    // is the feet, its Z rotation the facing), the editor camera otherwise.
    GameCameraState ComputePlayerSpawn() const;
    void StartPlayerController(const GameCameraState& spawn);
    void ApplyGameFieldOfView(float degrees);

    // Mouse look. The game asks for capture (Requested); Escape suspends it until the
    // player clicks back into the game view, and it only takes effect while the game
    // window has focus. While captured the cursor is hidden and re-centred every frame,
    // and the offset from the centre is the frame's look delta.
    void UpdateMouseCapture();
    void ReleaseMouseCapture();
    bool mMouseCaptureRequested = false;
    bool mMouseCaptureSuspended = false;
    bool mMouseCaptured = false;
    bool mMouseCaptureEscapeWasDown = false;
    bool mMouseCaptureClickWasDown = false;
    // The ~ key that stops an editor play session, edge-detected. Starts as held so a key
    // still down from before Play has to be released first.
    bool mEditorStopKeyWasDown = true;
    float mGameLookDeltaX = 0.0f;
    float mGameLookDeltaY = 0.0f;
    // The editor's lens, put back when the session ends.
    float mFovBeforePlay = 0.0f;

    DirectX::XMFLOAT3 mEditorCameraPositionBeforePlay{};
    DirectX::XMFLOAT3 mEditorCameraRotationBeforePlay{};
    float mGameLastMouseX = 0.0f;
    float mGameLastMouseY = 0.0f;
    bool mGameHasLastMousePosition = false;
    bool mGameInSeparateWindow = false;
    float mViewDistanceMeters = 8000.0f;

    // ---- Player settings (GameSettings.cpp) ----------------------------------
    // The game's Settings pages drive the renderer, window and audio through these.
    // Quality presets are relative to what the level authored, captured when play
    // starts; stopping play puts all of it back so the editor never inherits a preset.
    using GameSettingValues = std::map<std::string, std::string>;
    struct GameSettingsBaseline
    {
        TaaSettings Taa; SMAASettings Smaa; MsaaSettings Msaa; AnalyticEdgeAASettings AnalyticEdgeAa; AarSettings Aar;
        DlssSettings Dlss; FsrSettings Fsr; XessSettings Xess;
        GlobalIlluminationMode GiMode = GlobalIlluminationMode::Disabled;
        RtGISettings Rtgi; RadianceCascadesSettings Cascades; VoxelGISettings Voxel; RadianceProbeSettings Probes;
        VolumetricFogSettings Fog; PointShadowSettings PointShadows; VirtualShadowMapSettings VirtualShadows; RtShadowSettings RtShadows;
        GtaoSettings Gtao; RtAOSettings Rtao; ChromaticAberrationSettings ChromaticAberration;
        PathTracerSettings PathTracer;
        VirtualGeometrySettings VirtualGeometry; SsrSettings Ssr; SubsurfaceSettings Subsurface;
        BloomSettings Bloom; LensFlareSettings LensFlare;
    };
    // Captures the baseline, fills the menus' hardware-dependent options and hooks
    // Apply up. Standalone sessions also apply the settings saved on disk.
    void BeginGameSettings(bool standalone);
    void ApplySavedGameSettings();
    void EndGameSettings();
    // Applies every value; corrects in place any the hardware cannot honour and
    // returns a player-facing status line. Persists on success when asked.
    std::string ApplyGameSettings(GameSettingValues& values, bool persist);
    // One applied value by its settings.cfg key, for GameServices::GetSetting. False (and
    // empty) for a key the menus do not know.
    bool GetGameSetting(const std::string& key, std::string& value);
    GameSettingsBaseline mGameSettingsBaseline;
    bool mGameSettingsActive = false;
    // See ShowStartupScreen.
    bool mStartupScreenActive = false;
    int mStartupScreenPercent = -1;
    // Whether anything was applied this session, so stopping a session that applied
    // nothing leaves alone edits made in the editor's panels while it ran.
    bool mGameSettingsApplied = false;
    // Added to the sharpening mip bias: positive for the lower texture qualities.
    float mTextureQualityMipBias = 0.0f;
    // Player's upscaler sharpening, 0 (off) to 1. FSR applies it through its own RCAS;
    // after DLSS it drives the engine's image sharpen pass. Negative: not set by the
    // player, so the level's own sharpening stands (editor sessions).
    float mUpscalerSharpness = -1.0f;
    // Player's Brightness (stops of exposure on top of the level's) and display Gamma
    // (Settings > Image); see GetEffectiveAgxSettings. 0 and 1 leave the level's look.
    float mPlayerBrightnessStops = 0.0f;
    float mPlayerDisplayGamma = 1.0f;

public:
    // Shadows master switch (settings menu, "shadows.enabled"). Off: no point light casts,
    // and the sun's shadow map is cleared without casters, so everything is lit unshadowed.
    bool& GetShadowsEnabledRef() { return mShadowsEnabled; }
    // Frame-rate/latency overlay in the game UI ("stats.overlay", settings menu).
    bool& GetShowStatisticsOverlayRef() { return mShowStatisticsOverlay; }
private:
    bool mShadowsEnabled = true;
    bool mShowStatisticsOverlay = false;
    // Smoothed for display: the overlay updates a few times a second, not every frame.
    float mStatisticsFps = 0.0f;
    float mStatisticsFrameMs = 0.0f;
    float mStatisticsAccumulatedMs = 0.0f;
    int mStatisticsAccumulatedFrames = 0;
    std::string mStatisticsText;
    void UpdateStatisticsOverlay();

    BufferResource mVertexBuffer;
    BufferResource mIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    SceneConstants* mMappedConstants = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneColorTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSceneDepthTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> mMsaaSceneDepthTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthDebugTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSceneRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSceneDsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDepthDebugRtvHeap;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mRetiredSceneResources;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> mRetiredSceneDescriptorHeaps;
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneDsvHandle{};
    // A second view of the same depth resource with depth writes disabled.
    // Binding this instead of mSceneDsvHandle is what lets a transparent pass
    // depth-test against the scene and sample that same depth as a texture in
    // the same draw - which is what the soft-particle fade needs.
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneReadOnlyDsvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthDebugRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mSceneSrvGpuHandle{};
    // SRV for the scene depth buffer, bound to the deferred lighting pass for
    // world-space position reconstruction.  Requires the depth resource to be
    // created as DXGI_FORMAT_R32_TYPELESS.
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthSrvGpuHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mDepthDebugSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDepthDebugSrvGpuHandle{};
    UiTextureID mSceneTextureId = UiTextureID_Invalid;

    D3D12_VERTEX_BUFFER_VIEW mVertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW mIndexBufferView{};
    UINT mVertexCount = 0;
    UINT mSceneWidth = 1280;
    UINT mSceneHeight = 720;
    bool mHasCustomSceneResolution = false;
    UINT mCustomSceneWidth = 1280;
    UINT mCustomSceneHeight = 720;
    bool mGridEnabled = true;
    // Tracks the current D3D12 resource state of the scene depth buffer so Render()
    // can issue the correct barrier transitions each frame.
    D3D12_RESOURCE_STATES mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES mMsaaDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    UINT mSceneTargetMsaaSampleCount = 1;
    UINT mSceneTargetMsaaQuality = 0;
    std::string mLastErrorMessage;
    RendererTimingSnapshot mRendererTimingSnapshot;

    // Jittered view-projection matrix computed each frame in UpdateSceneConstants().
    // Used for both the grid and entity rendering so all geometry shares the same jitter.
    DirectX::XMFLOAT4X4 mJitteredViewProjection{};
    DirectX::XMFLOAT4X4 mNonJitteredViewProjection{}; // VP without TAA jitter, used by RT GI
    DirectX::XMFLOAT4X4 mInvViewProjection{};         // Transpose(inv(jitteredVP)), for world-pos reconstruction

    uint32_t mRenderFrameIndex = 0;
    uint32_t mProbeFrameIndex = 0; // monotonic counter passed to radiance probe update

    // Jittered projection matrix (without view) stored so the sky pass can be jittered
    // consistently with geometry, preventing sky fringing through mesh edges during TAA.
    DirectX::XMFLOAT4X4 mJitteredProjection{};

    // Previous frame's view matrix used to detect camera movement for TAA history reset.
    DirectX::XMFLOAT4X4 mPreviousViewMatrix{};
    bool                mCameraMovedThisFrame = false;

    // Previous frame's non-jittered view-projection, captured at the very end of
    // Render() so the volumetric cloud temporal filter always reprojects against
    // the previous frame rather than a matrix another pass already advanced.
    DirectX::XMFLOAT4X4 mPreviousViewProjectionForClouds{};
    bool                mHasPreviousCloudViewProjection = false;

    // Previous frame view-projection and camera position used by RTGI for temporal reservoir reprojection.
    DirectX::XMFLOAT4X4 mPreviousViewProjectionForRtgi{};
    DirectX::XMFLOAT4X4 mPreviousViewProjectionForRtao{};
    DirectX::XMFLOAT3   mPrevCameraPositionForRtgi{};
    // The same for the ray-traced shadows' temporal pass; false until a frame has run.
    DirectX::XMFLOAT4X4 mPreviousViewProjectionForRtShadows{};
    DirectX::XMFLOAT3   mPrevCameraPositionForRtShadows{};
    bool                mHasPreviousViewProjectionForRtShadows = false;

    // Non-jittered view and projection matrices stored separately so NRD can receive
    // them as worldToView and viewToClip without needing to decompose the combined VP.
    DirectX::XMFLOAT4X4 mNonJitteredViewMatrix{};
    DirectX::XMFLOAT4X4 mNonJitteredProjectionMatrix{};
    DirectX::XMFLOAT4X4 mPrevNonJitteredViewMatrix{};
    DirectX::XMFLOAT4X4 mPrevNonJitteredProjectionMatrix{};

    // Frame delta time in milliseconds, updated by UpdateCamera() and consumed by Dispatch.
    float mFrameDeltaTimeMs = 16.667f;
    // Monotonic scene clock, advanced once per frame. Light styles are pure
    // functions of it, so the same level flickers identically on every run.
    float mSceneTimeSeconds = 0.0f;

    struct alignas(256) DepthDebugConstants
    {
        float DepthScale = 1.0f;
        // The preview needs the projection's near and far planes to turn raw
        // device depth back into a linear distance. Without them the only
        // available mapping is (1 - depth), and a perspective depth buffer puts
        // an entire interior inside the top two percent of its range - so that
        // preview came out uniformly black.
        float NearPlane = 0.1f;
        float FarPlane = 8000.0f;
        float Pad = 0.0f;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthDebugConstantBuffer;
    DepthDebugConstants*                    mMappedDepthDebugConstants = nullptr;
    DX12Shader                              mDepthDebugVertexShader;
    DX12Shader                              mDepthDebugPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mDepthDebugRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDepthDebugPipelineState;

    struct alignas(256) ResolveMsaaDepthConstants
    {
        UINT SampleCount = 1;
        UINT Pad[3]{};
    };
    DX12Shader mResolveMsaaDepthShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mResolveMsaaDepthRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mResolveMsaaDepthPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> mResolveMsaaDepthConstantBuffer;
    ResolveMsaaDepthConstants* mMappedResolveMsaaDepthConstants = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mResolveMsaaDepthHeap;
    UINT mResolveMsaaDepthHeapStride = 0;

    // Handles GPU buffer management and draw calls for all scene entities (geometry pass).
    EntityMeshRenderer mEntityMeshRenderer;
    // Handles GPU heightmap-driven terrain patches in the G-Buffer pass.
    TerrainRenderer mTerrainRenderer;

    // Draws water surfaces (WaterComponent) in a forward pass after lighting.
    WaterRenderer mWaterRenderer;

    // Procedurally scattered, GPU-culled vegetation (VegetationAreaComponent).
    VegetationRenderer mVegetationRenderer;
    VirtualGeometryRenderer mVirtualGeometryRenderer;

    // Scene-wide wind, shared by vegetation bending and the rain simulation so
    // the two cannot disagree about which way the weather is blowing.
    WindSettings mWindSettings;

    // Accumulated seconds used to drive water wave animation.
    float mWaterTimeSeconds = 0.0f;

    // Per-frame sun/sky colour (lux-scaled) cached for the forward water pass,
    // which runs after the locals that compute them have gone out of scope.
    DirectX::XMFLOAT3 mFrameSunColor{ 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 mFrameSkyColor{ 0.3f, 0.5f, 0.8f };
    // Owns G-Buffer render targets and the fullscreen deferred-lighting resolve pass.
    DeferredLightingPass mDeferredLightingPass;
    // Draws a wireframe sphere gizmo for each point light in the scene.
    PointLightRenderer mPointLightRenderer;
    // Draws a wireframe box gizmo + arrow for each decal in the scene.
    DecalRenderer mDecalRenderer;
    std::vector<Entity>* mEntities = nullptr;

    // Temporal Anti-Aliasing renderer and settings.
    TAARenderer mTaaRenderer;
    TaaSettings mTaaSettings;
    SMAARenderer mSmaaRenderer;
    SMAASettings mSmaaSettings;
    // Analytic Edge AA: exact-coverage resolve of geometric edges, before TAA / SMAA.
    AnalyticEdgeAARenderer mAnalyticEdgeAaRenderer;
    AnalyticEdgeAASettings mAnalyticEdgeAaSettings;
    // Set once initialization has failed, so the frame loop does not retry it every frame.
    bool mAnalyticEdgeAaInitFailed = false;
    // AAR, Analytic-Aware Reconstruction: the spatial upscaler on the triangle edges.
    AarRenderer mAarRenderer;
    AarSettings mAarSettings;
    bool mAarInitFailed = false;
    bool mAarWasActive = false;
    // Sizes AAR's textures; on failure switches it off for the session and logs why.
    bool EnsureAarSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight);
    MsaaSettings mMsaaSettings;
    float mMsaaResolveTimeMs = 0.0f;
    ImageSharpenRenderer mImageSharpenRenderer;
    GBufferViewRenderer  mGBufferViewRenderer;
    GBufferView          mGBufferView = GBufferView::Off;
    SharpenSettings mSharpenSettings;
    DlssRenderer mDlssRenderer;
    DlssSettings mDlssSettings;
    FsrRenderer mFsrRenderer;
    FsrSettings mFsrSettings;
    FsrFrameGeneration mFrameGeneration;
    // FSR's context and output are freed while it is off; this notices the edge.
    bool mFsrWasActive = false;
    // Counts frames for FSR's own jitter sequence, which is longer than TAA's.
    UINT mFsrJitterIndex = 0;
    // DLSS jitter phase (Halton index), advanced once per frame while DLSS is on.
    UINT mDlssJitterIndex = 0;
    XessRenderer mXessRenderer;
    XessSettings mXessSettings;
    XessFrameGeneration mXessFrameGeneration;
    // XeSS's output and initialisation are dropped while it is off; this notices the edge.
    bool mXessWasActive = false;
    UINT mXessJitterIndex = 0;
    // TAAU renders below the output size; this notices it being switched on or off.
    bool mTaaUpscalerWasActive = false;
    MotionVectorRenderer mMotionVectorRenderer;

    // Renders this frame's motion vectors once, for whichever of TAA, the
    // upscalers and frame generation need them.
    void RenderMotionVectors(ID3D12GraphicsCommandList* commandList, bool resetHistory);
    // The exposure the tonemapper will apply this frame, as AgX settings, plus the
    // eye adaptation target when the time of day drives it.
    AgxTonemapSettings GetEffectiveAgxSettings(AutoExposure::RelativeTarget& adaptation, bool& eyeAdaptation) const;

    // Sky rendering and time-of-day lighting.
    SkyRenderer          mSkyRenderer;
    TimeOfDaySettings    mTimeOfDaySettings;
    HosekWilkieResult    mHosekResult{};
    // This frame's GI pre-exposure (see HosekWilkieResult::PreExposure), for
    // BuildGiPointLights, which runs after the locals it is derived from.
    float                mGiPreExposure = 1.0f;

    // Shadow map renderer for the sun directional light.
    ShadowMapRenderer    mShadowMapRenderer;
    PointShadowMapRenderer mPointShadowMapRenderer;
    PointShadowSettings    mPointShadowSettings;
    // The sun's virtual shadow map; the single map above is its fallback when it is
    // off or failed to initialise.
    VirtualShadowMapRenderer mVirtualShadowMap;
    VirtualShadowMapSettings mVirtualShadowMapSettings;

    // Custom RTGI with ReSTIR-style temporal/spatial reservoir resampling.
    RtGlobalIllumination  mRtgiRenderer;
    RtGISettings          mRtgiSettings;
    GlobalIlluminationMode mGlobalIlluminationMode = GlobalIlluminationMode::Rtgi;
    RadianceCascadesRenderer mRadianceCascadesRenderer;
    RadianceCascadesSettings mRadianceCascadesSettings;

    // World-space radiance probe grid (irradiance SH).
    RadianceProbeRenderer mProbeRenderer;
    RadianceProbeSettings mProbeSettings;

    // Voxel cone traced GI: needs no ray tracing hardware, and stands in for RTGI
    // and the probes on GPUs without it.
    VoxelGIRenderer mVoxelGiRenderer;
    VoxelGISettings mVoxelGiSettings;
    bool mVoxelGiActive = false;
    bool mVoxelGiFallbackActive = false;
    // The radiance probe grid was traced this frame (it may be enabled but
    // displaced by voxel GI, or waiting on the TLAS).
    bool mProbeGridUpdated = false;
    // DXR 1.1 support, queried once: -1 not yet asked, 0 no, 1 yes.
    mutable int mRayTracingSupport = -1;

    // Ray Traced Ambient Occlusion.
    RtAmbientOcclusion   mRtaoRenderer;
    RtAOSettings         mRtaoSettings;

    // Ray-traced sun and local light shadows.
    RtShadows            mRtShadows;
    RtShadowSettings     mRtShadowSettings;

    // Progressive reference path tracer; replaces the lit scene while enabled.
    PathTracer           mPathTracer;
    PathTracerSettings   mPathTracerSettings;
    std::string          mPathTracerStatus = "Off.";
    bool                 mPathTracerRanLastFrame = false;
    // The non-jittered view-projection the path tracer last ran with, for NRD's motion.
    DirectX::XMFLOAT4X4  mPathTracerPrevViewProjection{};

    // The terrain shadow signature the virtual shadow map was last given, and when.
    std::uint64_t        mVsmTerrainSignature = 0;
    double               mVsmTerrainSignatureTime = -1.0e9;

    // Screen-Space Ambient Occlusion (XeGTAO).
    XeGtaoRenderer       mGtaoRenderer;
    GtaoSettings         mGtaoSettings;
    SsrRenderer          mSsrRenderer;
    SsrSettings          mSsrSettings;
    // FidelityFX SSSR (SsrSettings::Technique 1). Created on first use: its denoiser
    // history runs to ~140 MB at 1080p, which the ray-march path should not pay for.
    SssrRenderer         mSssrRenderer;
    bool                 mSssrInitFailed = false;
    SubsurfaceScatteringRenderer mSubsurfaceRenderer;
    SubsurfaceSettings   mSubsurfaceSettings;
    ShadingSettings      mShadingSettings;
    // Deterministic Photoreal Lighting Enhancer, between AA/upscaling and sharpening.
    // Created on first use; a failed init is reported once and the pass stays off.
    DpleRenderer         mDpleRenderer;
    DpleSettings         mDpleSettings;
    bool                 mDpleFailureReported = false;
    ChromaticAberrationRenderer  mChromaticAberrationRenderer;
    ChromaticAberrationSettings  mChromaticAberrationSettings;

    // Physical mip-chain bloom.
    BloomRenderer        mBloomRenderer;
    FftBloomRenderer     mFftBloomRenderer;
    BloomSettings        mBloomSettings;
    SkyGlareRenderer     mSkyGlareRenderer;
    LensFlareRenderer    mLensFlareRenderer;
    LensFlareSettings    mLensFlareSettings;

    // AgX tonemapper.
    AgxTonemapper        mAgxTonemapper;
    AutoExposure         mAutoExposure;
    AgxTonemapSettings   mAgxSettings;

    // Volumetric fog froxel renderer.
    VolumetricFogRenderer mVolumetricFogRenderer;
    VolumetricFogSettings mVolumetricFogSettings;

    // Raymarched volumetric cloud layer.
    VolumetricCloudRenderer mVolumetricCloudRenderer;
    VolumetricCloudSettings mVolumetricCloudSettings;

    // Rain particle simulation.
    RainRenderer  mRainRenderer;
    RainSettings  mRainSettings;

    // Authored particle systems (fire, smoke, sparks) placed in the level.
    ParticleRenderer                    mParticleRenderer;
    std::vector<ParticleSystemInstance> mParticleSystems;

    // Cached point lights built each frame; shared with the RT GI dispatch.
    // This is the direct-lighting array: the point shadow pass writes shadow
    // indices into it and re-uploads it to the deferred pass.
    DeferredLightingPass::PointLightGpu mCachedPointLights[DeferredLightingPass::kMaxSceneLights]{};
    int                                 mNumCachedPointLights = 0;

    // Which of those lights the fog is allowed to scatter, recorded while the
    // lights are gathered. The fog's own array is compacted from this just
    // before the fog dispatch rather than here, because the point shadow pass
    // has not assigned shadow indices yet and the fog needs them to shadow its
    // shafts.
    bool                                mCachedPointLightAffectsFog[DeferredLightingPass::kMaxSceneLights]{};
    // Per-light multiplier on the fog's copy of each light (VolumetricFogIntensity).
    float                               mCachedPointLightFogScale[DeferredLightingPass::kMaxSceneLights]{};

    // The same lights weighted for indirect lighting only, so a particle
    // system's GI Contribution can tame a fire's bounce without dimming the
    // pool of light it throws on the floor. Rebuilt by BuildGiPointLights()
    // after the shadow indices are assigned. mGiPointLights holds the legacy
    // subset (below) for the radiance probe, cascade and voxel GI passes; RTGI
    // reads every light's scale from the cluster extras instead.
    DeferredLightingPass::PointLightGpu mGiPointLights[DeferredLightingPass::kMaxPointLights]{};
    float                               mPointLightGiScale[DeferredLightingPass::kMaxSceneLights]{};

    // The passes with a fixed 16-light constant-buffer array (probes, cascades, voxel
    // GI, the path tracer, ray-traced subsurface, particles) get the
    // kMaxPointLights most important lights: steady brightness over squared camera
    // distance, the scene's own order kept when there are no more than that.
    // mLegacyLightIndex maps each back to mCachedPointLights.
    DeferredLightingPass::PointLightGpu mLegacyPointLights[DeferredLightingPass::kMaxPointLights]{};
    int                                 mLegacyLightIndex[DeferredLightingPass::kMaxPointLights]{};
    int                                 mNumLegacyPointLights = 0;

    // Per scene light: x fog scale (0 = not in the fog), y GI scale with the GI's
    // pre-exposure folded in. Uploaded with the lights by LightClusterRenderer.
    DirectX::XMFLOAT4                   mLightClusterExtras[DeferredLightingPass::kMaxSceneLights]{};

    // Once the shadow indices are final: builds the legacy subset, the GI copy of it,
    // the cluster extras, uploads them to the deferred pass and the cluster renderer,
    // and builds the clustered lists.
    void BuildGiPointLights(ID3D12GraphicsCommandList* commandList);

    // Every scene light and its clustered light lists, for the deferred resolve, the
    // fog and RTGI (LightClusterRenderer.h).
    LightClusterRenderer mLightClusterRenderer;
    // Clustered light lists on (the default) or off: off, every pixel, froxel and bounce
    // hit walks every scene light, for comparison. Same image either way.
    bool  mClusteredLights = true;
    // World grid cell size for the bounce-hit clusters, in metres.
    float mLightGridCellSize = 4.0f;

    // Each light's colour without its light style, recorded while the lights are
    // gathered, and the copy of the array built from it for the reference path
    // tracer: its accumulation restarts whenever a light changes, so a flickering
    // candle would otherwise throw the image away every frame.
    DirectX::XMFLOAT3                   mCachedPointLightSteadyColor[DeferredLightingPass::kMaxSceneLights]{};
    DeferredLightingPass::PointLightGpu mPathTracerSteadyLights[DeferredLightingPass::kMaxPointLights]{};

    // Each light's identity from frame to frame (entity Id based), recorded while the
    // lights are gathered: the virtual shadow map keeps a light's cached pages under its
    // key, and the shadow budget its fade.
    std::uint64_t                       mCachedPointLightKeys[DeferredLightingPass::kMaxSceneLights]{};

    // The shadow budget (VirtualShadowMapSettings::ShadowBudget): picks which shadow-
    // casting lights the virtual shadow map shadows this frame and how strongly, fills
    // outLights with them and outLightIndices with each one's index in mCachedPointLights.
    void SelectShadowedLocalLights(std::vector<VirtualShadowMapRenderer::LocalLight>& outLights,
                                   std::vector<int>& outLightIndices);
    struct ShadowBudgetState
    {
        float         Fade = 0.0f;       // current shadow strength, 0..1
        bool          Selected = false;  // in the budget's set (fading toward 1)
        std::uint64_t LastFrame = 0;
    };
    std::unordered_map<std::uint64_t, ShadowBudgetState> mShadowBudget;
    std::uint64_t mShadowBudgetFrame = 0;

    // ---- Per-pass CPU timing ----------------------------------------------
    // Entries accumulate into mPendingTimingSnapshot during Render() and are
    // published to mRendererTimingSnapshot at the end of the frame, so the
    // editor always reads a complete snapshot rather than a half-built one.
    friend class PassTimer;
    void RecordPassTiming(const char* category, const char* name, float milliseconds);
    void BeginTimingFrame();
    void EndTimingFrame(float totalMilliseconds);

    RendererTimingSnapshot mPendingTimingSnapshot;

    // ---- Per-pass GPU timing ----------------------------------------------
    // Each PassTimer also brackets its pass with two timestamp queries, and the
    // frame itself with a pair, resolved into a mapped readback buffer at the end
    // of Render(). A slot is read back kGpuTimingSlots frames later, by which time
    // the engine's three-frame queue has long retired it, so nothing waits.
    static constexpr UINT kGpuTimingSlots = 6;
    static constexpr UINT kGpuTimingMaxPasses = 255;
    static constexpr UINT kGpuTimingQueriesPerSlot = 2 * (kGpuTimingMaxPasses + 1);
    struct GpuTimingSlot
    {
        std::vector<std::pair<const char*, const char*>> Passes;
        bool Pending = false;
    };
    UINT BeginGpuPassTimestamp(const char* category, const char* name);
    void EndGpuPassTimestamp(UINT query);
    void BeginGpuTimingFrame();
    void EndGpuTimingFrame();

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> mGpuTimingHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  mGpuTimingReadback;
    const UINT64* mGpuTimingMapped = nullptr;
    UINT64 mGpuTimingFrequency = 0;
    bool mGpuTimingInitFailed = false;
    std::array<GpuTimingSlot, kGpuTimingSlots> mGpuTimingSlots;
    UINT mGpuTimingSlot = 0;
    bool mGpuTimingActive = false;
    // The newest frame read back, merged into each published snapshot.
    std::vector<RendererTimingEntry> mLatestGpuPassTimes;
    float mLatestGpuFrameMilliseconds = 0.0f;

    // The command list Render() is recording, for the GPU event each PassTimer
    // emits. Null outside Render().
    ID3D12GraphicsCommandList* mMarkerCommandList = nullptr;
    // Pass names in the order their GPU events were opened, for this frame and the
    // last complete one. Printed by LogPassOrder() on device removal.
    std::vector<std::pair<const char*, const char*>> mCurrentPassOrder;
    std::vector<std::pair<const char*, const char*>> mLastPassOrder;

    // GPU progress breadcrumbs that survive a hang without device removal. The GPU
    // writes (frame << 16 | pass ordinal) into a readback buffer as each pass starts
    // and ends: slot 0 = frame begun, 1 = last pass started, 2 = last pass finished,
    // 3 = frame finished. A preemptible compute shader stuck in a loop never trips
    // TDR, so DRED never reports; this does.
    Microsoft::WRL::ComPtr<ID3D12Resource> mGpuProgressBuffer;
    const UINT32* mGpuProgressMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> mMarkerCommandList2;
    UINT mGpuProgressFrame = 0;
    void WriteGpuProgress(UINT slot, UINT ordinal, D3D12_WRITEBUFFERIMMEDIATE_MODE mode);
public:
    // Logs which pass the GPU is inside, read from the progress buffer. For a frame
    // that never retires.
    void LogGpuProgress() const;
private:

    // Jitter state for sub-pixel Halton offset (TAA projection jitter).
    UINT mJitterIndex = 0;
    // NRD expects camera jitter in pixel units in range [-0.5, 0.5].
    float mCurrentCameraJitter[2] = {};
    float mPrevCameraJitter[2] = {};
    bool  mRtaoDebugViewWasActive = false;
    // Tracked only to drop the TAA history when an AO pass is toggled.
    bool  mRtaoEnabledLastFrame = false;
    bool  mGtaoEnabledLastFrame = false;
    std::unordered_map<std::size_t, DirectX::XMFLOAT4X4> mPreviousEntityTransforms;

    // Cache a conservative local-space bounding radius per mesh so the shadow frustum
    // can expand to fit large imported scenes without re-scanning every vertex each frame.
    mutable std::unordered_map<const Mesh*, float> mMeshLocalRadiusCache;
};
