#pragma once

#include "Components.h"
#include "DX12Helper.h"
#include "TerrainRenderer.h"
#include "TaaSettings.h"
#include "SMAASettings.h"
#include "SharpenSettings.h"
#include "DlssSettings.h"
#include "FsrSettings.h"
#include "TimeOfDaySettings.h"
#include "RtGISettings.h"
#include "RadianceCascadesSettings.h"
#include "RtAOSettings.h"
#include "GtaoSettings.h"
#include "SsrSettings.h"
#include "ChromaticAberrationSettings.h"
#include "AgxTonemapSettings.h"
#include "VolumetricFogSettings.h"
#include "VolumetricCloudSettings.h"
#include "BloomSettings.h"
#include "RendererTimingSnapshot.h"
#include "AudioManager.h"
#include "..\Ptero-Engine\EditorCamera.h"
#include "System/NodeGraphEditor.h"

#include "../QtUi/QtUi.h"

#include <DirectXMath.h>

#include <atomic>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class SceneSerializer;

struct EngineResourceUsageEntry
{
    std::string Name;
    float CpuUsagePercent = 0.0f;
    float GpuUsagePercent = 0.0f;
    float RamUsagePercent = 0.0f;
};

struct EngineResourceUsageSnapshot
{
    float TotalCpuUsagePercent = 0.0f;
    float TotalGpuUsagePercent = 0.0f;
    float TotalRamUsagePercent = 0.0f;
    std::vector<EngineResourceUsageEntry> Entries;
};

class Editor
{
public:
    using ProgressCallback = void(__stdcall*)(const wchar_t* message);

    // Roughly how long a gizmo axis should appear on screen, in pixels. One
    // number for the whole gizmo: ComputeGizmoWorldScale turns it into a world
    // length at the pivot's depth, and every axis and ring uses that.
    static constexpr float kGizmoScreenLength = 72.0f;

    enum class ManualGizmoHandle
    {
        None,
        TranslateXAxis,
        TranslateYAxis,
        TranslateZAxis,
        TranslateXYPlane,
        TranslateXZPlane,
        TranslateYZPlane,
        Rotate,
        ScaleXAxis,
        ScaleYAxis,
        ScaleZAxis,
        Scale
    };

    enum class GizmoType
    {
        None,
        Translate,
        Rotate,
        Scale
    };

    Editor();

    bool Initialize(ID3D12GraphicsCommandList* commandList);
    void Shutdown();
    void SetProgressCallback(ProgressCallback callback)
    {
        mProgressCallback = callback;
    }

    // The editor drives the terrain renderer for heightmap painting and
    // height-picking; the renderer DLL hands the pointer in once it has
    // finished creating the scene renderer.
    void SetTerrainRenderer(TerrainRenderer* terrainRenderer)
    {
        mTerrainRenderer = terrainRenderer;
    }

    // The editor can also frame the camera on a new entity so the artist
    // doesn't have to hunt for it in the viewport.  Set by the DLL
    // alongside SetTerrainRenderer.
    void SetSceneRenderer(class DX12SceneRenderer* sceneRenderer)
    {
        mSceneRenderer = sceneRenderer;
    }

    // Brush-mode flag: when true, a left click inside the viewport applies
    // the active terrain brush at the picked world XY ground-plane position.
    void SetTerrainBrushModeActive(bool active) { mTerrainBrushModeActive = active; }
    bool IsTerrainBrushModeActive() const { return mTerrainBrushModeActive; }
    bool* GetShowTerrainToolWindowPointer() { return &mShowTerrainToolWindow; }
    const std::string& GetLastTerrainBrushMessage() const { return mLastTerrainBrushMessage; }

    void SetShowViewportPlacementIcons(bool shouldShow)
    {
        mShowViewportPlacementIcons = shouldShow;
    }

    bool GetShowViewportPlacementIcons() const
    {
        return mShowViewportPlacementIcons;
    }

    void SetShowViewportGrid(bool shouldShow)
    {
        mShowViewportGrid = shouldShow;
    }

    bool GetShowViewportGrid() const
    {
        return mShowViewportGrid;
    }

    void SetWireframeEnabled(bool shouldShow)
    {
        mWireframeEnabled = shouldShow;
    }

    bool GetWireframeEnabled() const
    {
        return mWireframeEnabled;
    }

    bool* GetShowComponentsPanelPointer()
    {
        return &mShowComponentsPanel;
    }

    bool* GetShowLevelExplorerPanelPointer()
    {
        return &mShowLevelExplorerPanel;
    }

    bool* GetShowPropertiesPanelPointer()
    {
        return &mShowPropertiesPanel;
    }

    bool* GetShowAudioManagerPanelPointer()
    {
        return &mShowAudioManagerPanel;
    }

    bool* GetShowResourceDebugPanelPointer()
    {
        return &mShowResourceDebugPanel;
    }

    bool* GetShowConsolePanelPointer()
    {
        return &mShowConsolePanel;
    }

    bool* GetShowUiEditorPanelPointer()
    {
        return &mShowUiEditorPanel;
    }

    bool SaveSceneToFile(const std::string& filepath);
    bool LoadSceneFromFile(const std::string& filepath);
    bool BeginLoadSceneFromFile(const std::string& filepath);
    bool SaveScene(HWND ownerWindowHandle);
    bool SaveSceneAs(HWND ownerWindowHandle);
    bool OpenScene(HWND ownerWindowHandle);
    bool NewScene(HWND ownerWindowHandle);
    // Shows the unsaved-changes prompt if there is anything to lose. Returns
    // false only when the user cancels, meaning the caller must not proceed.
    // Public because closing the editor has to ask the same question New and
    // Open do, and that call arrives from the host's WM_CLOSE.
    bool ConfirmDiscardUnsavedScene(HWND ownerWindowHandle);
    void UpdateSceneLoading();
    // Requests from widgets are consumed before recording the next GPU frame.
    void UpdatePlaySession();
    void StopPlaySession();
    // Asks for a play session to begin at the next frame boundary, in whichever mode
    // SetPlayInNewWindow last selected. Starting is deferred because it swaps the entity
    // vector the renderer is reading from.
    void RequestPlaySession() { mPlayStartRequested = true; }
    bool IsPlaySessionActive() const;
    // Play in a window of its own, the way a shipped build runs, instead of inside the
    // editor viewport. The mode is chosen before Play and read when the session starts,
    // so changing it mid-session does nothing until the next one.
    void SetPlayInNewWindow(bool enabled) { mPlayInNewWindow = enabled; }
    bool IsPlayInNewWindow() const { return mPlayInNewWindow; }
    bool* GetPlayInNewWindowPointer() { return &mPlayInNewWindow; }
    // The two mode entries, so the Play menu and the Play button's context menu offer
    // the same choice written once.
    void DrawPlayModeMenuItems();
    const NodeGraphDocument& GetRuntimeNodeGraph() const;
    bool IsSceneLoading() const;
    float GetSceneLoadProgress() const;
    std::string GetSceneLoadStatusMessage() const;
    // Parsing the level file is only the start of a load: the render loop then has to
    // read every entity's mesh. It does that a time-boxed slice per frame, so the
    // window keeps pumping messages, and reports back here so the loading overlay
    // stays up and counts the meshes instead of closing at 100% on the parse alone.
    bool IsStreamingSceneAssets() const { return mSceneAssetsStreaming; }
    void SetSceneAssetStreamingProgress(size_t resolvedMeshes, size_t totalMeshes);
    // Separate from SetSceneSettings so the long list there does not have to grow
    // at every call site; saved and restored with the level all the same.
    void SetFsrSettings(FsrSettings* fsrSettings) { mFsrSettings = fsrSettings; }
    void SetSceneSettings(
        TimeOfDaySettings* timeOfDaySettings,
        TaaSettings* taaSettings,
        SMAASettings* smaaSettings,
        SharpenSettings* sharpenSettings,
        DlssSettings* dlssSettings,
        GlobalIlluminationMode* globalIlluminationMode,
        RtGISettings* rtgiSettings,
        RadianceCascadesSettings* radianceCascadesSettings,
        RtAOSettings* rtaoSettings,
        GtaoSettings* gtaoSettings,
        SsrSettings* ssrSettings,
        ChromaticAberrationSettings* chromaticAberrationSettings,
        AgxTonemapSettings* agxSettings,
        VolumetricFogSettings* volumetricFogSettings,
        VolumetricCloudSettings* volumetricCloudSettings,
        BloomSettings* bloomSettings)
    {
        mTimeOfDaySettings = timeOfDaySettings;
        mTaaSettings = taaSettings;
        mSmaaSettings = smaaSettings;
        mSharpenSettings = sharpenSettings;
        mDlssSettings = dlssSettings;
        mGlobalIlluminationMode = globalIlluminationMode;
        mRtgiSettings = rtgiSettings;
        mRadianceCascadesSettings = radianceCascadesSettings;
        mRtaoSettings = rtaoSettings;
        mGtaoSettings = gtaoSettings;
        mSsrSettings = ssrSettings;
        mChromaticAberrationSettings = chromaticAberrationSettings;
        mAgxSettings = agxSettings;
        mVolumetricFogSettings = volumetricFogSettings;
        mVolumetricCloudSettings = volumetricCloudSettings;
        mBloomSettings = bloomSettings;
    }
    bool CanCopySelectedEntity() const;
    bool CanPasteEntity() const;
    bool CanDeleteSelectedEntity() const;
    bool CopySelectedEntity();
    bool PasteCopiedEntity();
    bool DeleteSelectedEntity();
    bool HasUnsavedChanges() const
    {
        // The node graph is part of the level even though the Node Graph window owns the
        // editing session, so an edit there has to count as an unsaved level change.
        return mSceneDirty || NodeGraphEditor::Revision() != mNodeGraphRevisionAtSave;
    }

    // Every mutation calls this. It lights the unsaved-changes flag and records
    // the pre-edit state for undo; see EditorUndo.cpp for why the two are one
    // call rather than two.
    void MarkSceneChanged();

    // Kept as the older name for callers that only mean "this needs saving".
    void MarkSceneDirty() { MarkSceneChanged(); }

    bool CanUndo() const { return !mUndoStack.empty(); }
    bool CanRedo() const { return !mRedoStack.empty(); }
    bool Undo();
    bool Redo();
    void ResetUndoHistory();

    const std::string& GetCurrentSceneFilePath() const
    {
        return mCurrentSceneFilePath;
    }

    std::string& GetCurrentSceneFilePath()
    {
        return mCurrentSceneFilePath;
    }

    const std::string& GetLastSceneStatusMessage() const
    {
        return mLastSceneStatusMessage;
    }

    void Draw(
        D3D12_GPU_DESCRIPTOR_HANDLE sceneTextureHandle,
        const EditorCamera& camera,
        const char* sceneStatusMessage,
        const char* statisticsText,
        bool showStatistics,
        AudioManager* audioManager);

    void DrawToolbar(
        D3D12_GPU_DESCRIPTOR_HANDLE selectIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE moveIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE rotateIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE scaleIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE wireframeIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE proxyIcon,
        D3D12_GPU_DESCRIPTOR_HANDLE gameIcon);

    void DrawPropertiesPanel(Entity* selectedEntity, AudioManager* audioManager);
    void DrawAudioManagerWindow(AudioManager* audioManager);
    void DrawResourceDebugWindow();
    void DrawViewportResolutionWindow();
    void DrawScreenshotWindow();
    void DrawViewport(
        D3D12_GPU_DESCRIPTOR_HANDLE sceneTextureHandle,
        const EditorCamera& camera,
        Entity* selectedEntity);

    Entity* GetSelectedEntity();
    std::vector<Entity>& GetEntities() { return mEntities; }

    // Screenshot functionality
    bool GetScreenshotRequested() const { return mRequestScreenshot; }
    void ClearScreenshotRequest() { mRequestScreenshot = false; }
    const std::string& GetScreenshotFolder() const { return mScreenshotOutputFolder; }

    bool GetViewportResolutionChangeRequested() const { return mRequestViewportResolutionChange; }
    void ClearViewportResolutionChangeRequest() { mRequestViewportResolutionChange = false; }
    int GetRequestedViewportResolutionWidth() const { return mViewportResolutionWidth; }
    int GetRequestedViewportResolutionHeight() const { return mViewportResolutionHeight; }
    void SetViewportResolution(int width, int height)
    {
        mViewportResolutionWidth = width;
        mViewportResolutionHeight = height;
    }
    void RequestViewportResolution(int width, int height)
    {
        SetViewportResolution(width, height);
        mRequestViewportResolutionChange = true;
    }
    // True while a resolution chosen in the Resolution dialog is in effect.
    // The viewport auto-fit must leave the scene target alone, otherwise it
    // resizes it straight back to the panel size on the next frame.
    bool IsViewportResolutionFixed() const { return mViewportResolutionFixed; }
    bool GetLastViewportContentResolution(int& width, int& height) const
    {
        if (mLastViewportContentSize.x <= 1.0f || mLastViewportContentSize.y <= 1.0f)
            return false;

        const float framebufferScale = QtUi::FramebufferScale();
        width = static_cast<int>(mLastViewportContentSize.x * framebufferScale + 0.5f);
        height = static_cast<int>(mLastViewportContentSize.y * framebufferScale + 0.5f);
        return width > 0 && height > 0;
    }
    bool HasPendingCameraRestore() const { return mRequestCameraRestore; }
    const DirectX::XMFLOAT3& GetPendingCameraRestorePosition() const { return mSavedCameraPosition; }
    const DirectX::XMFLOAT3& GetPendingCameraRestoreRotation() const { return mSavedCameraRotation; }
    void ConsumePendingCameraRestore() { mRequestCameraRestore = false; }
    void SetResourceUsageSnapshot(const EngineResourceUsageSnapshot& snapshot) { mResourceUsageSnapshot = snapshot; }
    void SetRendererTimingSnapshot(const RendererTimingSnapshot& snapshot) { mRendererTimingSnapshot = snapshot; }

private:
    void ReportProgress(const wchar_t* message) const;
    struct IconTexture
    {
        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle{};
        Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> Upload;
    };

    struct ViewportSelectionState
    {
        bool IsDragging = false;
        UiVec2 Start{};
        UiVec2 Current{};
    };

    struct ManualGizmoState
    {
        bool IsActive = false;
        ManualGizmoHandle ActiveHandle = ManualGizmoHandle::None;
        // What the cursor is over while nothing is being dragged, so the
        // drawing can light it up before the mouse goes down. Set by the same
        // pick that starts the drag, so the two cannot disagree.
        ManualGizmoHandle HoveredHandle = ManualGizmoHandle::None;
        DirectX::XMFLOAT3 HoveredRotationAxis{};
        UiVec2 StartMouse{};
        DirectX::XMFLOAT3 StartPosition{};
        DirectX::XMFLOAT3 StartRotation{};
        DirectX::XMFLOAT3 StartScale{ 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 AxisWorldDirection{};
        DirectX::XMFLOAT3 SecondaryAxisWorldDirection{};
        DirectX::XMFLOAT3 PlaneNormal{};
        DirectX::XMFLOAT3 StartPlaneHit{};
        UiVec2 AxisScreenDirection{};
        UiVec2 SecondaryAxisScreenDirection{};
        float PixelsPerWorldUnit = 1.0f;
        float SecondaryPixelsPerWorldUnit = 1.0f;
        float StartAngle = 0.0f;
    };

    struct SceneLoadData
    {
        std::vector<Entity> Entities;
        NodeGraphDocument NodeGraph;
        DirectX::XMFLOAT3 CameraPosition{};
        DirectX::XMFLOAT3 CameraRotation{};
        TimeOfDaySettings TimeOfDay{};
        TaaSettings Taa{};
        SMAASettings Smaa{};
        SharpenSettings Sharpen{};
        DlssSettings Dlss{};
        FsrSettings Fsr{};
        GlobalIlluminationMode GlobalIlluminationMode = GlobalIlluminationMode::Rtgi;
        RtGISettings Rtgi{};
        RadianceCascadesSettings RadianceCascades{};
        RtAOSettings Rtao{};
        GtaoSettings Gtao{};
        SsrSettings Ssr{};
        ChromaticAberrationSettings ChromaticAberration{};
        AgxTonemapSettings Agx{};
        VolumetricFogSettings VolumetricFog{};
        VolumetricCloudSettings VolumetricCloud{};
        BloomSettings Bloom{};
        bool HasCameraPosition = false;
        bool HasCameraRotation = false;
        bool HasTimeOfDay = false;
        bool HasTaa = false;
        bool HasSmaa = false;
        bool HasDlss = false;
        bool HasGlobalIlluminationMode = false;
        bool HasRtgi = false;
        bool HasRadianceCascades = false;
        bool HasRtao = false;
        bool HasGtao = false;
        bool HasSsr = false;
        bool HasChromaticAberration = false;
        bool HasAgx = false;
        bool HasVolumetricFog = false;
        bool HasVolumetricCloud = false;
        bool HasBloom = false;
    };

    struct SceneLoadState
    {
        std::atomic<bool> InProgress{ false };
        std::atomic<bool> Completed{ false };
        std::atomic<bool> CancelRequested{ false };
        std::atomic<float> Progress{ 0.0f };
        mutable std::mutex Mutex;
        std::string StatusMessage;
        std::string TargetFilePath;
        std::string ErrorMessage;
        std::optional<SceneLoadData> Result;
    };

    DirectX::XMFLOAT3 mSavedCameraPosition = DirectX::XMFLOAT3(0.0f, -6.0f, 1.5f);
    DirectX::XMFLOAT3 mSavedCameraRotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // Light style editor, shared by the point light inspector and the particle
    // system's proxy light so the two cannot drift apart. Sets mSceneDirty on
    // any change; idSuffix disambiguates the widget ids between the two uses.
    void DrawLightStyleControls(
        const char*   idSuffix,
        LightStyleId& style,
        float&        styleSpeed,
        float&        styleAmplitude,
        float&        stylePhaseOffset,
        std::string&  customStylePattern);

    void DrawComponentsPanel();
    // Defined in EditorConsole.cpp.
    void DrawConsolePanel();
    // Defined in EditorUiViewer.cpp.
    void DrawUiEditorPanel();
    void DrawLevelExplorerPanel();
    void DrawTerrainToolWindow(Entity* selectedEntity);

    // Prompts the user to pick a .raw heightmap, then a width/height dialog,
    // and finally creates a new entity with a TerrainComponent that the
    // terrain renderer can pick up.  Used by the "Create Terrain..." button
    // in the Components panel.
    void CreateTerrainFromRawFile(const DirectX::XMFLOAT3* placementPosition = nullptr);
    void DrawSceneLoadingOverlay();
    bool LoadGeometryIcon(ID3D12GraphicsCommandList* commandList);
    bool LoadIconTexture(const wchar_t* fileName, IconTexture& iconTexture, std::string* statusMessage, ID3D12GraphicsCommandList* commandList);
    void ReleaseIconTexture(IconTexture& iconTexture);
    void DrawViewportPlacementIcons(const UiVec2& viewportOrigin, const UiVec2& viewportSize, const EditorCamera& camera);
    // Composites the RmlUi target over the viewport image and forwards pointer input to it.
    void DrawGameUiOverlay(const UiVec2& viewportOrigin, const UiVec2& viewportSize);
    void DrawTerrainViewportOverlay(const UiVec2& viewportOrigin, const UiVec2& viewportSize, const EditorCamera& camera) const;

    // Wireframe reach and shape for every light in the scene: the sphere of a
    // point, the cone of a spot, the quad of a rect. Defined in
    // EditorLightGizmos.cpp; gated on the placement-icon switch.
    void DrawLightShapeGizmos(const UiVec2& viewportOrigin, const UiVec2& viewportSize, const EditorCamera& camera) const;
    // World length that projects to about kGizmoScreenLength pixels at the
    // pivot. Zero when the pivot is behind the near plane, which means the
    // gizmo cannot be drawn or hit-tested at all this frame.
    float ComputeGizmoWorldScale(const DirectX::XMFLOAT3& pivotWorldPosition, const UiVec2& viewportSize, const EditorCamera& camera) const;

    void DrawManualGizmoPivot(const UiVec2& viewportOrigin, const UiVec2& viewportSize, const EditorCamera& camera, const Entity* selectedEntity) const;
    bool TryProjectWorldToViewport(
        const DirectX::XMFLOAT3& worldPosition,
        const UiVec2& viewportOrigin,
        const UiVec2& viewportSize,
        const EditorCamera& camera,
        UiVec2& outScreenPosition,
        bool clampToViewport = false) const;
    bool TryGetViewportWorldPositionOnGrid(
        const UiVec2& mousePosition,
        const UiVec2& viewportOrigin,
        const UiVec2& viewportSize,
        const EditorCamera& camera,
        DirectX::XMFLOAT3& outWorldPosition) const;
    bool IsEntitySelected(int entityIndex) const;
    void HandleViewportInteraction(
        const UiVec2& viewportOrigin,
        const UiVec2& viewportSize,
        const EditorCamera& camera,
        bool viewportHovered);
    void HandleManualGizmoInteraction(
        const UiVec2& viewportOrigin,
        const UiVec2& viewportSize,
        const EditorCamera& camera,
        bool viewportHovered,
        Entity* selectedEntity);
    void FinishViewportSelection(const UiVec2& viewportOrigin, const UiVec2& viewportSize, const EditorCamera& camera);
    void CreateGeometryInstanceAt(const DirectX::XMFLOAT3& worldPosition);
    void DrawViewportStatisticsOverlay() const;
    void SetViewportStatisticsText(const char* text);
    void HandleKeyboardShortcuts();
    void ResetScene();
    void SetSceneLoadProgress(float progress, const char* statusMessage);
    static bool SceneLoadProgressCallback(float progress, const char* statusMessage, void* userData);

    static UiTextureID TextureIdFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle);

    std::vector<Entity> mEntities;
    int mSelectedEntityIndex = -1;
    std::vector<int> mSelectedEntityIndices;
    GizmoType mActiveGizmo = GizmoType::None;
    mutable bool mGeometryPrototypeSelected = false;
    mutable bool mPointLightPrototypeSelected = false;
    mutable bool mSpotLightPrototypeSelected = false;
    mutable bool mRectLightPrototypeSelected = false;
    mutable bool mAudioEmitterPrototypeSelected = false;
    mutable bool mDecalPrototypeSelected = false;
    mutable bool mRainPrototypeSelected = false;
    mutable bool mParticleSystemPrototypeSelected = false;
    mutable bool mTerrainPrototypeSelected = false;
    mutable bool mWaterPrototypeSelected = false;
    mutable bool mVegetationPrototypeSelected = false;
    // Hides the side panels and tool windows so the viewport fills the window.
    // Toggled by F11 or the toolbar button; the toolbar itself stays visible.
    bool mViewportFullscreen = false;
    bool mShowViewportPlacementIcons = true;
    bool mShowGameUi = true;
    bool mShowComponentsPanel = true;
    bool mShowLevelExplorerPanel = false;
    bool mShowPropertiesPanel = true;
    bool mShowAudioManagerPanel = false;
    bool mShowResourceDebugPanel = false;
    bool mShowConsolePanel = true;
    bool mShowUiEditorPanel = false;

    // UI Editor state. All buffer sizes are generous rather than tight: these
    // hold RML ids, property names and CSS values, none of which is worth
    // truncating to save bytes in an editor panel.
    int  mUiEditorDocumentIndex = 0;
    std::string mUiEditorSelectedDocument;
    std::string mUiEditorObservedDocument;
    int  mUiEditorPreviewSizeIndex = 1;      // 640 x 360
    bool mUiEditorInteractive = true;
    bool mUiEditorHadPointer = false;
    char mUiEditorElementId[128] = {};
    char mUiEditorElementText[256] = {};
    char mUiEditorPropertyName[64] = {};
    char mUiEditorPropertyValue[128] = {};
    char mUiEditorClassName[64] = {};

    // Console panel state. The rendered document is cached because the widget
    // behind it re-parses the whole thing on every change, so it is rebuilt only
    // when the log or a filter actually moves.
    int          mConsoleMinimumLevel = 1;      // PteroLog::Level::Debug
    int          mConsoleCategoryIndex = 0;     // 0 = all categories
    std::size_t  mConsoleCategoryCount = 0;
    bool         mConsoleAutoScroll = true;
    bool         mConsoleDocumentDirty = true;
    std::uint64_t mConsoleLastTotal = 0;
    std::uint64_t mConsoleLastEvicted = 0;
    std::string  mConsoleDocument;
    std::vector<UiCompletion> mConsoleCompletions;
    char         mConsoleFilter[128] = {};
    char         mConsoleInput[512] = {};
    bool mShowTerrainToolWindow = false;
    bool mBlockViewportSelection = false;
    int mNextGeometryInstanceId = 1;
    ViewportSelectionState mViewportSelection;
    ManualGizmoState mManualGizmo;

    IconTexture mGeometryIcon;
    IconTexture mPointLightIcon;
    IconTexture mAudioEmitterIcon;
    IconTexture mDecalIcon;
    IconTexture mRainIcon;
    IconTexture mParticleSystemIcon;
    IconTexture mSelectIcon;
    IconTexture mMoveIcon;
    IconTexture mRotateIcon;
    IconTexture mScaleIcon;
    IconTexture mWireframeIcon;
    IconTexture mProxyIcon;
    IconTexture mGameIcon;

    // Sticky message from the last failed play attempt, shown on the Play button tooltip.
    std::string mGameStartErrorMessage;
    bool mIsInitialized = false;
    bool mGeometryIconLoadAttempted = false;
    std::string mGeometryIconStatus;
    std::string mCurrentSceneFilePath;
    std::string mLastSceneStatusMessage;
    std::optional<Entity> mCopiedEntity;
    // One recoverable state of the level. Entities carry shared_ptr handles to
    // their mesh assets, so copying the vector copies component values and
    // shares the geometry - a snapshot is strings and POD, not megabytes.
    struct SceneUndoState
    {
        std::vector<Entity> Entities;
        int SelectedEntityIndex = -1;
        std::vector<int> SelectedEntityIndices;
    };

    static constexpr std::size_t kMaxUndoSteps = 64;
    // Edits closer together than this are one step, so a gizmo drag does not
    // fill the history with sixty identical-looking frames of itself.
    static constexpr std::chrono::milliseconds kUndoCoalesceWindow{ 400 };

    std::deque<SceneUndoState> mUndoStack;
    std::deque<SceneUndoState> mRedoStack;
    // The last committed state. MarkSceneChanged runs after the edit, so this
    // is what it pushes: the scene as it was before.
    SceneUndoState mUndoBaseline;
    std::chrono::steady_clock::time_point mLastSceneChangeTime{};
    bool mApplyingUndoState = false;

    // Marks the scope in which Undo and Redo rewrite the scene, so the
    // MarkSceneChanged calls that rewriting provokes do not record themselves.
    struct UndoApplyGuard
    {
        explicit UndoApplyGuard(Editor& editor) : mEditor(editor) { mEditor.mApplyingUndoState = true; }
        ~UndoApplyGuard() { mEditor.mApplyingUndoState = false; }
        UndoApplyGuard(const UndoApplyGuard&) = delete;
        UndoApplyGuard& operator=(const UndoApplyGuard&) = delete;
        Editor& mEditor;
    };

    SceneUndoState CaptureSceneState() const;
    void ApplySceneState(const SceneUndoState& state);

    bool mSceneDirty = false;
    bool mPlayStartRequested = false;
    bool mPlaySceneActive = false;
    // Which of the two play modes the next Play uses. Defaults to the viewport: the
    // level, the panels and the game stay on one screen, which is what you want while
    // still building the level. See SetPlayInNewWindow.
    bool mPlayInNewWindow = false;
    std::vector<Entity> mEntitiesBeforePlay;
    NodeGraphDocument mPlayNodeGraph;
    std::vector<std::function<void()>> mRestorePlaySettings;
    void RestoreEditorAfterPlay();
    // NodeGraphEditor::Revision() as it was when the level was last saved or loaded.
    unsigned mNodeGraphRevisionAtSave = 0;

    TimeOfDaySettings* mTimeOfDaySettings = nullptr;
    TaaSettings* mTaaSettings = nullptr;
    SMAASettings* mSmaaSettings = nullptr;
    SharpenSettings* mSharpenSettings = nullptr;
    DlssSettings* mDlssSettings = nullptr;
    FsrSettings* mFsrSettings = nullptr;
    GlobalIlluminationMode* mGlobalIlluminationMode = nullptr;
    RtGISettings* mRtgiSettings = nullptr;
    RadianceCascadesSettings* mRadianceCascadesSettings = nullptr;
    RtAOSettings* mRtaoSettings = nullptr;
    GtaoSettings* mGtaoSettings = nullptr;
    SsrSettings* mSsrSettings = nullptr;
    ChromaticAberrationSettings* mChromaticAberrationSettings = nullptr;
    AgxTonemapSettings* mAgxSettings = nullptr;
    VolumetricFogSettings* mVolumetricFogSettings = nullptr;
    VolumetricCloudSettings* mVolumetricCloudSettings = nullptr;
    BloomSettings* mBloomSettings = nullptr;
    ProgressCallback mProgressCallback = nullptr;
    TerrainRenderer* mTerrainRenderer = nullptr;
    class DX12SceneRenderer* mSceneRenderer = nullptr;
    bool mTerrainBrushModeActive = false;
    std::string mLastTerrainBrushMessage;

    const char* mSceneStatusMessage = nullptr;
    const char* mViewportStatisticsText = nullptr;
    bool mShowViewportStatistics = true;
    bool mViewportStatisticsInitialized = false;
    bool mShowViewportGrid = true;
    bool mMovementSnapEnabled = true;
    float mMovementSnapStep = 1.0f;
    bool mRotationSnapEnabled = true;
    float mRotationSnapDegrees = 15.0f;
    bool mWireframeEnabled = false;
    bool mProxyEnabled = false;

    // Viewport resolution control
    int mViewportResolutionWidth = 1920;
    int mViewportResolutionHeight = 1080;
    bool mShowViewportResolutionDialog = false;
    bool mRequestViewportResolutionChange = false;
    bool mViewportResolutionFixed = false;
    bool mRequestCameraRestore = false;

    EngineResourceUsageSnapshot mResourceUsageSnapshot;
    RendererTimingSnapshot mRendererTimingSnapshot;

    // Screenshot functionality
    std::string mScreenshotOutputFolder;
    bool mShowScreenshotDialog = false;
    bool mRequestScreenshot = false;

    UiVec2 mLastViewportContentOrigin{};
    UiVec2 mLastViewportContentSize{};

    SceneLoadState mSceneLoadState;
    std::thread mSceneLoadWorker;
    bool mSceneAssetsStreaming = false;
    // Set on the frame every mesh is resolved; the overlay stays one frame longer so
    // the textures those meshes pull in on their first draw also load under it.
    bool mSceneAssetsFinishing = false;
    size_t mSceneAssetsResolved = 0;
    size_t mSceneAssetsTotal = 0;
};
