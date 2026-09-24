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
#include "DeferredLightingPass.h"
#include "TimeOfDaySettings.h"
#include "RtGlobalIllumination.h"
#include "RtGISettings.h"
#include "SMAARenderer.h"
#include "SMAASettings.h"
#include "MsaaSettings.h"
#include "RadianceCascadesRenderer.h"
#include "RadianceCascadesSettings.h"
#include "RadianceProbeRenderer.h"
#include "RadianceProbeSettings.h"
#include "RtAmbientOcclusion.h"
#include "RtAOSettings.h"
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
#include "SubsurfaceScattering.h"
#include "SubsurfaceSettings.h"
#include "BloomRenderer.h"
#include "BloomSettings.h"
#include "ImageSharpenRenderer.h"
#include "SharpenSettings.h"
#include "TerrainRenderer.h"
#include "VegetationRenderer.h"
#include "WindSettings.h"
#include "WaterRenderer.h"
#include "RendererTimingSnapshot.h"
#include "DlssRenderer.h"
#include "DlssSettings.h"
#include "FsrRenderer.h"
#include "FsrFrameGeneration.h"
#include "FsrSettings.h"
#include "MotionVectorRenderer.h"
#include "GameHost.h"
#include "RmlUiRenderer.h"
#include "System/NodeGraphRuntime.h"
#include "VideoLayer.h"
#include "..\Ptero-Engine\EditorCamera.h"
#include "..\Ptero-Engine\RenderInterfaces.h"

#include "../QtUi/UiTypes.h"

#include <algorithm>
#include <chrono>
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

    // DLSS and FSR fill the same slot in the post chain, between TAA/SMAA and
    // sharpening. DLSS takes it when both are switched on, so the two never run in
    // the same frame.
    bool IsDlssUpscalerActive() const
    {
        return mDlssSettings.Enabled && mDlssRenderer.IsAvailable();
    }

    bool IsFsrUpscalerActive() const
    {
        return !IsDlssUpscalerActive() && mFsrSettings.Enabled && mFsrRenderer.IsAvailable();
    }

    // While an upscaler owns the slot, TAA and SMAA are skipped: it does their job.
    bool UpscalerOwnsPostAaOutput() const
    {
        return IsDlssUpscalerActive() || IsFsrUpscalerActive();
    }

    bool IsUpscalerOutputAvailable() const
    {
        if (IsDlssUpscalerActive())
            return mDlssRenderer.IsInitialized() && !mDlssRenderer.IsEvaluationBypassed();
        return IsFsrUpscalerActive() && mFsrRenderer.HasOutput();
    }

    ID3D12Resource* GetUpscalerOutputResource() const
    {
        return IsDlssUpscalerActive() ? mDlssRenderer.GetOutputResource() : mFsrRenderer.GetOutputResource();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetUpscalerOutputCpuSrv() const
    {
        return IsDlssUpscalerActive() ? mDlssRenderer.GetOutputCpuSrv() : mFsrRenderer.GetOutputCpuSrv();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetUpscalerOutputGpuSrv() const
    {
        return IsDlssUpscalerActive() ? mDlssRenderer.GetOutputGpuSrv() : mFsrRenderer.GetOutputGpuSrv();
    }

    UiTextureID GetUpscalerOutputTextureId() const
    {
        return IsDlssUpscalerActive() ? mDlssRenderer.GetOutputTextureId() : mFsrRenderer.GetOutputTextureId();
    }

    // Returns the final scene output for display:
    //   AgX tonemapped → Bloom composited → sharpened image → DLSS/FSR upscaled → SMAA resolved → TAA resolved → raw scene colour (in priority order when enabled).
    UiTextureID GetSceneTextureId() const
    {
        const bool upscalerOwnsPostAaOutput = UpscalerOwnsPostAaOutput();
        const bool upscalerOutputAvailable = IsUpscalerOutputAvailable();
        const bool imageSharpenOutputAvailable = mSharpenSettings.ImageSharpeningEnabled
            && mImageSharpenRenderer.IsInitialized()
            && (!upscalerOwnsPostAaOutput || upscalerOutputAvailable);

        if (mRtaoSettings.DebugView > 0)
            return mSceneTextureId;
        // Chromatic aberration is the last thing applied, so when it runs its output is
        // the finished frame regardless of what fed it.
        if (mChromaticAberrationSettings.Enabled && mChromaticAberrationRenderer.IsInitialized())
            return mChromaticAberrationRenderer.GetOutputTextureId();
        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
            return mAgxTonemapper.GetOutputTextureId();
        if (mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
            return mBloomRenderer.GetOutputTextureId();
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

        if (mRtaoSettings.DebugView > 0)
            return mSceneSrvGpuHandle;
        if (mChromaticAberrationSettings.Enabled && mChromaticAberrationRenderer.IsInitialized())
            return mChromaticAberrationRenderer.GetOutputGpuSrv();
        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
            return mAgxTonemapper.GetOutputGpuSrv();
        if (mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
            return mBloomRenderer.GetOutputGpuSrv();
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

    MsaaSettings& GetMsaaSettings() { return mMsaaSettings; }
    const MsaaSettings& GetMsaaSettings() const { return mMsaaSettings; }

    SharpenSettings& GetSharpenSettings() { return mSharpenSettings; }
    const SharpenSettings& GetSharpenSettings() const { return mSharpenSettings; }

    TimeOfDaySettings& GetTimeOfDaySettings() { return mTimeOfDaySettings; }
    const TimeOfDaySettings& GetTimeOfDaySettings() const { return mTimeOfDaySettings; }

    GlobalIlluminationMode& GetGlobalIlluminationMode() { return mGlobalIlluminationMode; }
    const GlobalIlluminationMode& GetGlobalIlluminationMode() const { return mGlobalIlluminationMode; }

    RtGISettings& GetRtgiSettings() { return mRtgiSettings; }
    const RtGISettings& GetRtgiSettings() const { return mRtgiSettings; }

    RadianceCascadesSettings& GetRadianceCascadesSettings() { return mRadianceCascadesSettings; }
    const RadianceCascadesSettings& GetRadianceCascadesSettings() const { return mRadianceCascadesSettings; }

    RadianceProbeSettings& GetProbeSettings() { return mProbeSettings; }
    const RadianceProbeSettings& GetProbeSettings() const { return mProbeSettings; }

    RtAOSettings& GetRtaoSettings() { return mRtaoSettings; }
    const RtAOSettings& GetRtaoSettings() const { return mRtaoSettings; }

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

    PointShadowSettings& GetPointShadowSettings() { return mPointShadowSettings; }
    const PointShadowSettings& GetPointShadowSettings() const { return mPointShadowSettings; }

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
    VegetationRenderer& GetVegetationRenderer() { return mVegetationRenderer; }
    const VegetationRenderer& GetVegetationRenderer() const { return mVegetationRenderer; }

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

    // ---- Game intro (Pterosoft logo -> engine logo) ----------------------
    // Runs once before Game_Start, on the same VideoLayer the Node Graph's video nodes use
    // (see GetVideoLayer()). StartGame() plays the first clip and returns immediately;
    // UpdateGameIntro() (driven every frame from UpdateCamera(), since the game camera/host
    // do not exist yet) advances it and calls BeginGameAfterIntro() once both clips have
    // finished or been skipped. game.skipintro (see EngineCVars.cpp) bypasses it entirely,
    // mainly so editor PIE iteration is not stuck replaying both clips every Play.
    enum class GameIntroStage { None, Pterosoft, Engine };
    bool IsGameIntroPlaying() const { return mGameIntroStage != GameIntroStage::None; }
    void UpdateGameIntro(float deltaTime);
    bool BeginGameAfterIntro(bool separateWindow);
    bool& GetSkipGameIntroRef() { return mSkipGameIntro; }
    GameIntroStage mGameIntroStage = GameIntroStage::None;
    bool mGameIntroSeparateWindow = false;
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
        if (mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
            return mBloomRenderer.GetOutputResource();
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

    private:
        Entity* FindEntity(std::uint64_t entityId);

        DX12SceneRenderer& mOwner;
        RmlUiRenderer* mUiRenderer = nullptr;
        VideoLayer* mVideoLayer = nullptr;
    };

    NodeGraphUiHost mNodeGraphHost{ *this };
    NodeGraphRuntime mNodeGraphRuntime;
    const NodeGraphDocument* mNodeGraphSource = nullptr;

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
        TaaSettings Taa; SMAASettings Smaa; MsaaSettings Msaa;
        DlssSettings Dlss; FsrSettings Fsr;
        GlobalIlluminationMode GiMode = GlobalIlluminationMode::Disabled;
        RtGISettings Rtgi; RadianceCascadesSettings Cascades;
        VolumetricFogSettings Fog; PointShadowSettings PointShadows;
        GtaoSettings Gtao; RtAOSettings Rtao; ChromaticAberrationSettings ChromaticAberration;
    };
    // Captures the baseline, fills the menus' hardware-dependent options and hooks
    // Apply up. Standalone sessions also apply the settings saved on disk.
    void BeginGameSettings(bool standalone);
    void ApplySavedGameSettings();
    void EndGameSettings();
    // Applies every value; corrects in place any the hardware cannot honour and
    // returns a player-facing status line. Persists on success when asked.
    std::string ApplyGameSettings(GameSettingValues& values, bool persist);
    GameSettingsBaseline mGameSettingsBaseline;
    bool mGameSettingsActive = false;
    // Whether anything was applied this session, so stopping a session that applied
    // nothing leaves alone edits made in the editor's panels while it ran.
    bool mGameSettingsApplied = false;
    // Added to the sharpening mip bias: positive for the lower texture qualities.
    float mTextureQualityMipBias = 0.0f;
    // Player's upscaler sharpening, 0 (off) to 1. FSR applies it through its own RCAS;
    // after DLSS it drives the engine's image sharpen pass. Negative: not set by the
    // player, so the level's own sharpening stands (editor sessions).
    float mUpscalerSharpness = -1.0f;

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
    MsaaSettings mMsaaSettings;
    float mMsaaResolveTimeMs = 0.0f;
    ImageSharpenRenderer mImageSharpenRenderer;
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
    MotionVectorRenderer mMotionVectorRenderer;

    // Sky rendering and time-of-day lighting.
    SkyRenderer          mSkyRenderer;
    TimeOfDaySettings    mTimeOfDaySettings;
    HosekWilkieResult    mHosekResult{};

    // Shadow map renderer for the sun directional light.
    ShadowMapRenderer    mShadowMapRenderer;
    PointShadowMapRenderer mPointShadowMapRenderer;
    PointShadowSettings    mPointShadowSettings;

    // Custom RTGI with ReSTIR-style temporal/spatial reservoir resampling.
    RtGlobalIllumination  mRtgiRenderer;
    RtGISettings          mRtgiSettings;
    GlobalIlluminationMode mGlobalIlluminationMode = GlobalIlluminationMode::Rtgi;
    RadianceCascadesRenderer mRadianceCascadesRenderer;
    RadianceCascadesSettings mRadianceCascadesSettings;

    // World-space radiance probe grid (irradiance SH).
    RadianceProbeRenderer mProbeRenderer;
    RadianceProbeSettings mProbeSettings;

    // Ray Traced Ambient Occlusion.
    RtAmbientOcclusion   mRtaoRenderer;
    RtAOSettings         mRtaoSettings;

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
    ChromaticAberrationRenderer  mChromaticAberrationRenderer;
    ChromaticAberrationSettings  mChromaticAberrationSettings;

    // Physical mip-chain bloom.
    BloomRenderer        mBloomRenderer;
    BloomSettings        mBloomSettings;

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
    DeferredLightingPass::PointLightGpu mCachedPointLights[DeferredLightingPass::kMaxPointLights]{};
    int                                 mNumCachedPointLights = 0;

    // Which of those lights the fog is allowed to scatter, recorded while the
    // lights are gathered. The fog's own array is compacted from this just
    // before the fog dispatch rather than here, because the point shadow pass
    // has not assigned shadow indices yet and the fog needs them to shadow its
    // shafts.
    bool                                mCachedPointLightAffectsFog[DeferredLightingPass::kMaxPointLights]{};

    // The same lights weighted for indirect lighting only, so a particle
    // system's GI Contribution can tame a fire's bounce without dimming the
    // pool of light it throws on the floor. Rebuilt by BuildGiPointLights()
    // after the shadow indices are assigned, and fed to the RTGI, radiance
    // probe and radiance cascade passes in place of mCachedPointLights.
    DeferredLightingPass::PointLightGpu mGiPointLights[DeferredLightingPass::kMaxPointLights]{};
    float                               mPointLightGiScale[DeferredLightingPass::kMaxPointLights]{};

    // Copies mCachedPointLights into mGiPointLights, applying mPointLightGiScale.
    void BuildGiPointLights();

    // ---- Per-pass CPU timing ----------------------------------------------
    // Entries accumulate into mPendingTimingSnapshot during Render() and are
    // published to mRendererTimingSnapshot at the end of the frame, so the
    // editor always reads a complete snapshot rather than a half-built one.
    friend class PassTimer;
    void RecordPassTiming(const char* category, const char* name, float milliseconds);
    void BeginTimingFrame();
    void EndTimingFrame(float totalMilliseconds);

    RendererTimingSnapshot mPendingTimingSnapshot;

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
