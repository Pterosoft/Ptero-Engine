#include "pch.h"
#include "EditorMainMenu.h"

#include "Editor.h"
#include "DebugViewNames.h"
#include "TimeOfDaySettings.h"
#include "HosekWilkieSky.h"
#include "..\System\MaterialEditor.h"
#include "..\System\include\System\SystemAssetApi.h"

#include "../QtUi/QtUi.h"
#include "VideoImport.h"
#include "VideoPlayerWindow.h"
#include "ReleaseGame.h"
#include "GameProjectSettings.h"
#include "DataFolderWatcher.h"
#include "System/NodeGraphTemplates.h"
#include "System/CVar.h"
#include "LensFlareOptics.h"
#include "LensFlareRenderer.h"
#include "AnalyticEdgeAARenderer.h"

#include <commdlg.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#include <shobjidl.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    bool SliderFloatWithInput(const char* label, float* value, float minValue, float maxValue, const char* format, float inputStep = 0.1f, float inputStepFast = 1.0f)
    {
        bool changed = false;
        QtUi::PushID(label);
        QtUi::BeginGroup();
        changed |= QtUi::SliderFloat("##Slider", value, minValue, maxValue, format);
        QtUi::SameLine();
        QtUi::SetNextItemWidth(90.0f);
        changed |= QtUi::InputFloat(label, value, inputStep, inputStepFast, format);
        *value = std::clamp(*value, minValue, maxValue);
        QtUi::EndGroup();
        QtUi::PopID();
        return changed;
    }

    bool SliderIntWithInput(const char* label, int* value, int minValue, int maxValue, const char* format = "%d")
    {
        bool changed = false;
        QtUi::PushID(label);
        QtUi::BeginGroup();
        changed |= QtUi::SliderInt("##Slider", value, minValue, maxValue, format);
        QtUi::SameLine();
        QtUi::SetNextItemWidth(90.0f);
        changed |= QtUi::InputInt(label, value);
        *value = std::clamp(*value, minValue, maxValue);
        QtUi::EndGroup();
        QtUi::PopID();
        return changed;
    }

    // ---- DPLE window --------------------------------------------------------------------
    // The Deterministic Photoreal Lighting Enhancer gets a window of its own rather than a
    // section in Graphics Settings: it is tuned by eye against a live viewport, so it wants
    // something that can stay open beside the viewport while tuning.
    bool gShowDpleWindow = false;
    int gDplePresetSelection = static_cast<int>(DplePreset::Balanced);

    // One "Reset" per section: tuning happens one section at a time, so undoing it should.
    bool DpleSectionResetButton(const char* id)
    {
        const bool clicked = QtUi::Button(id);
        QtUi::SetItemTooltip("Restores this section's factory values and leaves the other sections alone.");
        return clicked;
    }

    void DrawDpleWindow(DpleSettings& settings, const char* errorMessage)
    {
        QtUi::SetNextWindowSize(UiVec2(460.0f, 0.0f), QtUiCond_FirstUseEver);
        if (!QtUi::Begin("DPLE", &gShowDpleWindow, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::End();
            return;
        }

        QtUi::Checkbox("Enable DPLE##dple", &settings.Enabled);
        QtUi::SetItemTooltip(
            "Deterministic Photoreal Lighting Enhancer. Re-presents lighting the renderer has already\n"
            "resolved: multiscale AO on the indirect share only, sun contact shadows, a per-material\n"
            "response and a micro-specular lobe. It is not a GI solver - it adds no bounce light.");

        if (errorMessage != nullptr)
        {
            QtUi::TextColored(UiVec4(0.90f, 0.45f, 0.35f, 1.0f), "DPLE could not start: %s", errorMessage);
        }

        // ---- presets --------------------------------------------------------------
        // Selecting a preset does nothing on its own; Apply commits it. That way a preset can
        // be a starting point that is then edited without being silently reverted.
        QtUi::SeparatorText("Presets");
        const char* presetNames[] = { "Subtle", "Balanced", "Strong", "Performance" };
        QtUi::SetNextItemWidth(160.0f);
        QtUi::Combo("##dplepreset", &gDplePresetSelection, presetNames, static_cast<int>(std::size(presetNames)));
        QtUi::SetItemTooltip(
            "Subtle: ~half strength, contact grounding only.\n"
            "Balanced: the documented starting values (= factory defaults).\n"
            "Strong: deeper occlusion, detail enhancement on, full-resolution working buffers.\n"
            "Performance: the Balanced look at quarter-resolution working buffers.");
        QtUi::SameLine();
        if (QtUi::Button("Apply Preset##dple"))
        {
            const int debugView = settings.DebugView;
            settings = DpleSettings::MakePreset(static_cast<DplePreset>(
                std::clamp(gDplePresetSelection, 0, static_cast<int>(DplePreset::Count) - 1)));
            settings.DebugView = debugView;
        }
        QtUi::SameLine();
        if (QtUi::Button("Reset To Factory Defaults##dple"))
        {
            const bool enabled = settings.Enabled;
            const int debugView = settings.DebugView;
            settings = DpleSettings{};
            settings.Enabled = enabled;
            settings.DebugView = debugView;
        }
        QtUi::SetItemTooltip("Every value back to the one compiled into the engine. Keeps DPLE on or off as it is.");

        // ---- general -------------------------------------------------------------
        QtUi::SeparatorText("General");
        const char* resolutionNames[] = { "Full", "Half", "Third", "Quarter" };
        int resolutionIndex = std::clamp(settings.WorkingResolutionDivisor, 1, 4) - 1;
        if (QtUi::Combo("Working Resolution##dple", &resolutionIndex, resolutionNames, static_cast<int>(std::size(resolutionNames))))
            settings.WorkingResolutionDivisor = resolutionIndex + 1;
        QtUi::SetItemTooltip(
            "Resolution of the AO, contact shadow, temporal and denoise passes, relative to the\n"
            "output. The main performance lever. With an upscaler at 50%%, Half already equals the\n"
            "render resolution. Full is what resolves pore- and weave-scale micro AO.");

        const auto& debugViewNames = DebugViewNames::Dple;
        static_assert(std::size(debugViewNames) == static_cast<size_t>(DpleDebugView::Count), "debug view list out of date");
        QtUi::Combo("Debug View##dple", &settings.DebugView, debugViewNames, static_cast<int>(std::size(debugViewNames)));
        QtUi::SetItemTooltip(
            "3: red = treated as indirect, green = direct.\n"
            "4: grey default, red skin (subsurface profile), green foliage, blue = inferred wet,\n"
            "   black = passed through (sky, glass, water and other forward surfaces).\n"
            "6: red must ramp left to right and green top to bottom, reaching the edge.\n"
            "8: green brightened, red darkened, flat grey = the term is doing nothing -\n"
            "   then check 7: a near-black specular share means the material limits it.");

        // Everything below only matters while DPLE runs.
        QtUi::BeginDisabled(!settings.Enabled);

        // ---- ambient occlusion ---------------------------------------------------------
        if (QtUi::CollapsingHeader("Multiscale Ambient Occlusion", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dpleao"))
                settings.ResetAmbientOcclusion();
            QtUi::Checkbox("Enable##dpleao", &settings.EnableAmbientOcclusion);
            QtUi::BeginDisabled(!settings.EnableAmbientOcclusion);
            SliderFloatWithInput("Micro Radius##dpleao", &settings.MicroAORadius, 0.005f, 0.3f, "%.3f m", 0.005f, 0.02f);
            QtUi::SetItemTooltip("Pore and weave scale. Needs Full working resolution to have anything to resolve.");
            SliderFloatWithInput("Micro Intensity##dpleao", &settings.MicroAOIntensity, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            SliderFloatWithInput("Contact Radius##dpleao", &settings.ContactAORadius, 0.01f, 1.0f, "%.2f m", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Where objects meet each other and the ground.");
            SliderFloatWithInput("Contact Intensity##dpleao", &settings.ContactAOIntensity, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            SliderFloatWithInput("Broad Radius##dpleao", &settings.BroadAORadius, 0.1f, 5.0f, "%.2f m", 0.05f, 0.5f);
            QtUi::SetItemTooltip("Room and alcove scale.");
            SliderFloatWithInput("Broad Intensity##dpleao", &settings.BroadAOIntensity, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            SliderFloatWithInput("Max Combined##dpleao", &settings.MaxCombinedAO, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Hard ceiling on how much the three radii together may darken the indirect term.\n"
                                 "Without it three reasonable radii stack into crushed black creases.");
            SliderFloatWithInput("Power##dpleao", &settings.AOPower, 0.1f, 3.0f, "%.2f", 0.05f, 0.25f);
            QtUi::SetItemTooltip("Below 1 softens contact darkening, above 1 deepens it.");
            SliderFloatWithInput("Bias##dpleao", &settings.AOBias, 0.0f, 0.2f, "%.3f", 0.005f, 0.02f);
            QtUi::SetItemTooltip("Keeps flat and gently curved surfaces from occluding themselves.");
            QtUi::EndDisabled();
        }

        // ---- contact shadows -----------------------------------------------------------
        if (QtUi::CollapsingHeader("Contact Shadows", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dplecs"))
                settings.ResetContactShadows();
            QtUi::Checkbox("Enable##dplecs", &settings.EnableContactShadows);
            QtUi::SetItemTooltip("Screen-space trace toward the sun. Needs Time of Day on with the sun above the horizon.");
            QtUi::BeginDisabled(!settings.EnableContactShadows);
            SliderFloatWithInput("Length##dplecs", &settings.ContactShadowLength, 0.01f, 2.0f, "%.2f m", 0.01f, 0.1f);
            SliderIntWithInput("Steps##dplecs", &settings.ContactShadowSteps, 4, 32);
            SliderFloatWithInput("Thickness##dplecs", &settings.ContactShadowThickness, 0.001f, 0.2f, "%.3f m", 0.001f, 0.01f);
            QtUi::SetItemTooltip("How thick a depth-buffer occluder is assumed to be. Too small and thin geometry\n"
                                 "stops casting; too large and everything behind a silhouette gets a false shadow.");
            SliderFloatWithInput("Intensity##dplecs", &settings.ContactShadowIntensity, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::EndDisabled();
        }

        // ---- material response ---------------------------------------------------------
        if (QtUi::CollapsingHeader("Material Response", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dplemat"))
                settings.ResetMaterialResponse();
            QtUi::Checkbox("Enable##dplemat", &settings.EnableMaterialResponse);
            QtUi::BeginDisabled(!settings.EnableMaterialResponse);
            SliderFloatWithInput("Skin AO Scale##dplemat", &settings.SkinAOScale, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Surfaces with a subsurface-scattering profile. Skin does not take crunchy AO.");
            SliderFloatWithInput("Skin Warmth##dplemat", &settings.SkinWarmth, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("How far occluded skin shifts toward red, standing in for shallow subsurface transport.");
            SliderFloatWithInput("Foliage AO Scale##dplemat", &settings.FoliageAOScale, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Contact and broad AO on vegetation layers with Translucency above zero.");
            SliderFloatWithInput("Foliage Saturation##dplemat", &settings.FoliageSaturation, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Saturation lift in occluded foliage, standing in for light transmitted through leaves.");
            SliderFloatWithInput("Wet Roughness Threshold##dplemat", &settings.WetRoughnessThreshold, 0.0f, 0.5f, "%.2f", 0.01f, 0.05f);
            QtUi::SetItemTooltip("Smooth non-metals below this roughness are treated as wet. A heuristic: polished\n"
                                 "marble reads as wet to it. 0 turns the inference off.");
            SliderFloatWithInput("Wet Response##dplemat", &settings.WetResponseStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::EndDisabled();
        }

        // ---- micro specular ------------------------------------------------------------
        if (QtUi::CollapsingHeader("Micro Specular", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dplespec"))
                settings.ResetMicroSpecular();
            QtUi::Checkbox("Enable##dplespec", &settings.EnableMicroSpecular);
            QtUi::BeginDisabled(!settings.EnableMicroSpecular);
            SliderFloatWithInput("Specular Occlusion##dplespec", &settings.SpecularOcclusionStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Occludes the specular share by the AO, narrowed by roughness and view angle.");
            SliderFloatWithInput("Strength##dplespec", &settings.MicroSpecularStrength, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
            QtUi::SetItemTooltip("How far the albedo gradient tilts the normal before the sun's GGX lobe is re-evaluated.\n"
                                 "Grain tilted toward the sun flares, grain tilted away goes dark. Needs the sun.");
            SliderFloatWithInput("Detail Scale##dplespec", &settings.MicroSpecularDetailScale, 0.5f, 4.0f, "%.2f texels", 0.05f, 0.25f);
            QtUi::SetItemTooltip("Gradient tap radius in G-Buffer texels. At 1 the signal mostly reads as speckle.");
            SliderFloatWithInput("Roughness Max##dplespec", &settings.MicroSpecularRoughnessMax, 0.05f, 1.0f, "%.2f", 0.01f, 0.05f);
            QtUi::SetItemTooltip("Grain fades out from here to fully rough. Weathered timber sits at 0.6-0.85.");
            QtUi::EndDisabled();
        }

        // ---- indirect estimation -------------------------------------------------------
        if (QtUi::CollapsingHeader("Indirect Estimation", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dpleind"))
                settings.ResetIndirectEstimation();
            QtUi::TextWrapped("AO darkens only the share of each pixel estimated to be indirect light, never direct sun.");
            SliderFloatWithInput("Indirect Fraction Min##dpleind", &settings.IndirectFractionMin, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Share treated as indirect where the sun lights the pixel fully.");
            SliderFloatWithInput("Indirect Fraction Max##dpleind", &settings.IndirectFractionMax, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Share treated as indirect where the pixel gets no direct sun.");
            SliderFloatWithInput("Direct Light Weight##dpleind", &settings.DirectLightWeight, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("How strongly the sun's N.L and the contact shadows push the estimate toward direct.");
        }

        // ---- temporal ------------------------------------------------------------------
        if (QtUi::CollapsingHeader("Temporal & Denoise", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dpletemp"))
                settings.ResetTemporal();
            QtUi::Checkbox("Temporal Reconstruction##dpletemp", &settings.EnableTemporalReconstruction);
            QtUi::SetItemTooltip("Accumulates DPLE's own AO and contact shadows over frames. TAA/upscalers own the image.");
            QtUi::BeginDisabled(!settings.EnableTemporalReconstruction);
            SliderFloatWithInput("History Weight (Still)##dpletemp", &settings.HistoryWeightStable, 0.0f, 0.98f, "%.2f", 0.01f, 0.05f);
            SliderFloatWithInput("History Weight (Moving)##dpletemp", &settings.HistoryWeightMoving, 0.0f, 0.98f, "%.2f", 0.01f, 0.05f);
            QtUi::SetItemTooltip("Never above the still weight - history would be stickiest where reprojection is worst.");
            SliderFloatWithInput("Disocclusion Tolerance##dpletemp", &settings.DisocclusionDepthTolerance, 0.005f, 0.5f, "%.3f", 0.005f, 0.02f);
            QtUi::SetItemTooltip("Relative depth mismatch above which history is dropped outright.");
            SliderFloatWithInput("Neighborhood Clamp##dpletemp", &settings.NeighborhoodClampScale, 0.5f, 4.0f, "%.2f", 0.05f, 0.25f);
            QtUi::EndDisabled();
            QtUi::Checkbox("Spatial Denoise##dpletemp", &settings.EnableSpatialDenoise);
            QtUi::BeginDisabled(!settings.EnableSpatialDenoise);
            SliderIntWithInput("Denoise Radius##dpletemp", &settings.SpatialDenoiseRadius, 0, 4);
            QtUi::SetItemTooltip("Taps per side. Raise it if foliage still sparkles.");
            QtUi::EndDisabled();
        }

        // ---- detail --------------------------------------------------------------------
        if (QtUi::CollapsingHeader("Detail Enhancement", QtUiTreeNodeFlags_DefaultOpen))
        {
            if (DpleSectionResetButton("Reset##dpledet"))
                settings.ResetDetailEnhancement();
            QtUi::Checkbox("Enable##dpledet", &settings.EnableDetailEnhancement);
            QtUi::SetItemTooltip("Re-amplifies the band TAA and the upscalers suppress. Judge it on a slow and a fast\n"
                                 "pan, never on a still frame.");
            QtUi::BeginDisabled(!settings.EnableDetailEnhancement);
            SliderFloatWithInput("Fine Strength##dpledet", &settings.FineDetailStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
            QtUi::SetItemTooltip("0.10 is the tested value; useful range roughly 0.05-0.25. Near 1 it is grain.");
            SliderFloatWithInput("Structure Strength##dpledet", &settings.StructureStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
            SliderFloatWithInput("Max Luminance Change##dpledet", &settings.MaxLuminanceChange, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
            QtUi::SetItemTooltip("Hard clamp on how far a pixel's luminance may move.");
            SliderFloatWithInput("Motion Suppression##dpledet", &settings.DetailMotionSuppression, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Fades detail out while the camera turns (full at 60 deg/s). Camera-wide, not per pixel.");
            QtUi::EndDisabled();
        }

        QtUi::EndDisabled();
        QtUi::End();
    }

    // Keep the menu-specific UI state in this file so the renderer API stays focused on frame orchestration.
    bool gShowAboutWindow = false;
    bool gShowEditorSettingsWindow = false;
    char gNewStyleName[64] = "";
    bool gShowSceneSettingsWindow = false;
    bool gShowGraphicsSettingsWindow = false;
    bool gShowAssetBrowserWindow = false;
    bool gShowBuildGameWindow = false;
    bool gShowExtractPackageWindow = false;
    bool gShowMaterialEditorWindow = false;
    bool gShowTimeOfDayWindow = false;
    bool gShowGBufferDebugWindow = false;
    bool gShowShaderCompileWindow = false;
    bool gLastAssetActionSucceeded = false;
    bool gHasAssetActionResult = false;
    bool gOpenRenamePopup = false;
    bool gOpenNewFolderPopup = false;
    bool gClipboardHasValue = false;
    std::string gClipboardRelativePath;
    std::string gAssetStatusMessage = "Use Import to bring geometry (FBX), textures and videos into Data.";
    std::string gDataDirectoryDisplay = "Data folder not found yet.";
    std::string gSelectedFolderRelativePath;
    std::string gSelectedAssetRelativePath;
    MaterialEditor gMaterialEditor;
    char gRenameBuffer[MAX_PATH] = {};
    char gNewFolderBuffer[MAX_PATH] = {};
    // Height of the scrolling property region in both Material Editor tabs.
    //
    // Fixed rather than derived from the available content area: this UI layer
    // is immediate-mode over retained Qt widgets, so content drives the window's
    // minimum size, and sizing a region from the window it lives in makes the
    // window grow a little every frame. Chosen to leave room for the pinned
    // header and the status line inside the default 640x780 dialog.
    constexpr float kMaterialPropertiesHeight = 520.0f;

    char gMaterialNameBuffer[256] = {};
    char gMultiMaterialNameBuffer[256] = {};
    char gSceneFileBuffer[MAX_PATH] = {};
    // Index of the sub-material currently selected in the multi-material editor (-1 = none).
    int gSelectedSubMaterialIndex = -1;

    // -----------------------------------------------------------------------
    // Asset batch import progress state (geometry, textures and videos).
    // Import runs on a background thread so the UI stays responsive.
    // -----------------------------------------------------------------------
    struct AssetImportProgress
    {
        std::atomic<int>  Total{ 0 };      // total files queued
        std::atomic<int>  Done{ 0 };       // files completed (success or fail)
        std::atomic<bool> Running{ false };// background thread is active
        std::atomic<bool> Cancel{ false }; // stops a video conversion and skips the rest
        std::atomic<float> CurrentFraction{ 0.0f }; // progress within the current file, when known
        std::atomic<bool> CacheDirty{ false };      // an import added files; the UI thread refreshes
        std::mutex        LogMutex;
        std::vector<std::string> Log;      // per-file result messages (guarded by LogMutex)
        std::string CurrentFile;           // file currently being imported (guarded by LogMutex)
    };
    AssetImportProgress gAssetImport;

    struct ShaderCompileMenuState
    {
        std::atomic<bool> Running{ false };
        std::mutex Mutex;
        std::string Status = "Shader compile has not been run yet.";
        bool HasResult = false;
        bool LastSucceeded = true;
    };
    ShaderCompileMenuState gShaderCompileMenu;

    // Defined further down; forward-declared so the Release menu helpers below (which
    // resolve the project root from it) don't have to move to after that definition.
    const std::filesystem::path& GetProjectDataDirectoryCached();

    // -----------------------------------------------------------------------
    // Release menu: Build Game and Extract Package. Both run on a background
    // thread (packaging and copying DLLs can take a while) behind a simple
    // status + scrolling log, the same shape as gShaderCompileMenu above.
    // -----------------------------------------------------------------------
    struct BuildGameMenuState
    {
        std::atomic<bool> Running{ false };
        std::mutex Mutex;
        std::vector<std::string> Log;
        bool HasResult = false;
        bool LastSucceeded = true;
    };
    BuildGameMenuState gBuildGameState;

    char gBuildGameNameBuffer[128] = "My Game";
    std::string gBuildGameIconPath;
    std::string gBuildGameSplashPath;
    std::string gBuildGameOutputFolder;
    bool gBuildGameAutoBuildRelease = true;

    struct BuildGameFolderOption
    {
        std::wstring Name;
        bool Included;
    };
    std::vector<BuildGameFolderOption> gBuildGameFolderOptions;

    struct ExtractPackageMenuState
    {
        std::atomic<bool> Running{ false };
        std::mutex Mutex;
        std::string Status;
    };
    ExtractPackageMenuState gExtractPackageState;
    std::string gExtractPackagePath;
    std::string gExtractPackageOutputFolder;

    void EnsureBuildGameFolderOptionsInitialized()
    {
        if (!gBuildGameFolderOptions.empty())
            return;
        for (const std::wstring& folder : ReleaseGame::DefaultIncludedFolders())
            gBuildGameFolderOptions.push_back({ folder, true });
    }

    // A narrow-string OPENFILENAMEA prompt for a single existing file, the same underlying
    // dialog PromptForAndImportAssets uses below, minus multi-select.
    bool PromptForOpenFile(HWND owner, const char* title, const char* filter, std::string& outPath)
    {
        char buffer[MAX_PATH] = {};
        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = owner;
        openFileName.lpstrFilter = filter;
        openFileName.lpstrFile = buffer;
        openFileName.nMaxFile = static_cast<DWORD>(std::size(buffer));
        openFileName.lpstrTitle = title;
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!QtUi::OpenFileName(&openFileName))
            return false;
        outPath = buffer;
        return true;
    }

    // No existing folder-picker wrapper exists in QtUi (only file open/save), so this talks
    // to the modern IFileOpenDialog directly - the same COM API a folder picker anywhere
    // else in Windows uses.
    bool PromptForFolder(HWND owner, const wchar_t* title, std::wstring& outFolder)
    {
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool ownsComInit = SUCCEEDED(comInit);

        bool result = false;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(title);

            if (SUCCEEDED(dialog->Show(owner)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        outFolder = path;
                        CoTaskMemFree(path);
                        result = true;
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (ownsComInit)
            CoUninitialize();
        return result;
    }

    void StartBuildGameCommand(ReleaseGame::BuildOptions options)
    {
        bool expectedRunning = false;
        if (!gBuildGameState.Running.compare_exchange_strong(expectedRunning, true))
            return;

        {
            std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
            gBuildGameState.Log.clear();
            gBuildGameState.HasResult = false;
        }

        std::thread([options = std::move(options)]() mutable
        {
            bool succeeded;
            try
            {
                succeeded = ReleaseGame::Build(options, [](const std::string& line)
                {
                    std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
                    gBuildGameState.Log.push_back(line);
                });
            }
            catch (const std::exception& exception)
            {
                std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
                gBuildGameState.Log.push_back(std::string("Error: ") + exception.what());
                succeeded = false;
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
                gBuildGameState.Log.push_back("Error: unknown exception.");
                succeeded = false;
            }

            std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
            gBuildGameState.HasResult = true;
            gBuildGameState.LastSucceeded = succeeded;
            gBuildGameState.Running.store(false);
        }).detach();
    }

    void DrawBuildGameWindow()
    {
        if (!gShowBuildGameWindow)
            return;

        EnsureBuildGameFolderOptionsInitialized();

        QtUi::SetNextWindowSize(UiVec2(520.0f, 620.0f), QtUiCond_FirstUseEver);
        if (!QtUi::Begin("Build Game", &gShowBuildGameWindow))
        {
            QtUi::End();
            return;
        }

        const bool running = gBuildGameState.Running.load();
        QtUi::BeginDisabled(running);

        QtUi::InputText("Game Name", gBuildGameNameBuffer, sizeof(gBuildGameNameBuffer));
        QtUi::TextWrapped("Sets both the shipped .exe's file name and its window title.");

        QtUi::Separator();
        QtUi::TextWrapped("Icon: %s", gBuildGameIconPath.empty() ? "(default)" : gBuildGameIconPath.c_str());
        if (QtUi::Button("Choose Icon Image..."))
        {
            std::string path;
            if (PromptForOpenFile(GetActiveWindow(), "Select Icon Image",
                "Images\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0", path))
            {
                gBuildGameIconPath = path;
            }
        }

        QtUi::Separator();
        QtUi::TextWrapped("Splash Screen: %s", gBuildGameSplashPath.empty() ? "(none)" : gBuildGameSplashPath.c_str());
        if (QtUi::Button("Choose Splash Image..."))
        {
            std::string path;
            if (PromptForOpenFile(GetActiveWindow(), "Select Splash Image",
                "Images\0*.png;*.jpg;*.jpeg;*.bmp\0All Files\0*.*\0", path))
            {
                gBuildGameSplashPath = path;
            }
        }
        if (!gBuildGameSplashPath.empty())
        {
            QtUi::SameLine();
            if (QtUi::Button("Clear##splash"))
                gBuildGameSplashPath.clear();
        }
        QtUi::TextWrapped(
            "Shown centred on screen from the moment the game is launched until its first frame "
            "is up. Transparent areas of a PNG stay see-through. Larger images are scaled down "
            "to fit 1600 x 1000.");

        QtUi::Separator();
        QtUi::TextWrapped("Output Folder: %s", gBuildGameOutputFolder.empty() ? "(not set)" : gBuildGameOutputFolder.c_str());
        if (QtUi::Button("Choose Output Folder..."))
        {
            std::wstring folder;
            if (PromptForFolder(GetActiveWindow(), L"Select Output Folder", folder))
            {
                gBuildGameOutputFolder = std::filesystem::path(folder).string();
            }
        }

        QtUi::Separator();
        QtUi::TextUnformatted("Data Folders to Package:");
        for (BuildGameFolderOption& option : gBuildGameFolderOptions)
        {
            const std::string label = std::filesystem::path(option.Name).string();
            QtUi::Checkbox(label.c_str(), &option.Included);
        }

        QtUi::Separator();
        const std::filesystem::path projectRoot = GetProjectDataDirectoryCached().parent_path();
        QtUi::Checkbox("Compile Release DLLs automatically if missing or out of date", &gBuildGameAutoBuildRelease);
        QtUi::TextWrapped(
            "Invokes MSBuild for Video/Audio/Game/Renderer_DX12/GameLauncher's Release|x64 "
            "configuration before packaging, whenever their Release build is missing or older "
            "than the Debug one. This can take several minutes the first time.");

        const std::filesystem::path releaseRendererDll = projectRoot / "Source" / "Renderer_DX12" / "x64" / "Release" / "Renderer_DX12.dll";
        if (!gBuildGameAutoBuildRelease && !std::filesystem::exists(releaseRendererDll))
        {
            QtUi::TextColored(UiVec4(0.90f, 0.45f, 0.35f, 1.0f),
                "Warning: Release|x64 has not been built yet, and automatic building is off. "
                "Build it in Visual Studio first, or turn the checkbox above back on.");
        }

        QtUi::Separator();
        const bool canBuild = gBuildGameNameBuffer[0] != '\0' && !gBuildGameOutputFolder.empty();
        QtUi::BeginDisabled(!canBuild);

        const auto makeOptions = [&](bool buildExe, bool buildPackages)
        {
            ReleaseGame::BuildOptions options;
            options.ProjectRoot = projectRoot.wstring();
            options.GameName = std::filesystem::path(gBuildGameNameBuffer).wstring();
            if (!gBuildGameIconPath.empty())
                options.IconSourcePath = std::filesystem::path(gBuildGameIconPath).wstring();
            if (!gBuildGameSplashPath.empty())
                options.SplashSourcePath = std::filesystem::path(gBuildGameSplashPath).wstring();
            options.OutputDirectory = std::filesystem::path(gBuildGameOutputFolder).wstring();
            for (const BuildGameFolderOption& folderOption : gBuildGameFolderOptions)
                if (folderOption.Included)
                    options.IncludedFolders.push_back(folderOption.Name);
            options.BuildExe = buildExe;
            options.BuildPackages = buildPackages;
            options.AutoBuildRelease = gBuildGameAutoBuildRelease;
            return options;
        };

        if (QtUi::Button("Full Build"))
            StartBuildGameCommand(makeOptions(true, true));
        QtUi::SameLine();
        if (QtUi::Button("EXE Only"))
            StartBuildGameCommand(makeOptions(true, false));
        QtUi::SameLine();
        if (QtUi::Button("Package Only"))
            StartBuildGameCommand(makeOptions(false, true));

        QtUi::EndDisabled(); // canBuild
        QtUi::EndDisabled(); // running

        QtUi::Separator();
        {
            std::lock_guard<std::mutex> lock(gBuildGameState.Mutex);
            if (running)
            {
                QtUi::TextUnformatted("Building...");
            }
            else if (gBuildGameState.HasResult)
            {
                QtUi::TextColored(
                    gBuildGameState.LastSucceeded ? UiVec4(0.35f, 0.85f, 0.45f, 1.0f) : UiVec4(0.90f, 0.45f, 0.35f, 1.0f),
                    "%s", gBuildGameState.LastSucceeded ? "Build succeeded." : "Build failed.");
            }

            if (QtUi::BeginChild("BuildGameLog", UiVec2(0.0f, 200.0f), true))
            {
                for (const std::string& line : gBuildGameState.Log)
                    QtUi::TextWrapped("%s", line.c_str());
                if (QtUi::GetScrollY() >= QtUi::GetScrollMaxY())
                    QtUi::SetScrollHereY(1.0f);
            }
            QtUi::EndChild();
        }

        QtUi::End();
    }

    void DrawExtractPackageWindow()
    {
        if (!gShowExtractPackageWindow)
            return;

        QtUi::SetNextWindowSize(UiVec2(520.0f, 280.0f), QtUiCond_FirstUseEver);
        if (!QtUi::Begin("Extract Package", &gShowExtractPackageWindow))
        {
            QtUi::End();
            return;
        }

        QtUi::TextWrapped("Package: %s", gExtractPackagePath.empty() ? "(not set)" : gExtractPackagePath.c_str());
        if (QtUi::Button("Choose .ppak File..."))
        {
            std::string path;
            if (PromptForOpenFile(GetActiveWindow(), "Select Package",
                "Ptero Package\0*.ppak\0All Files\0*.*\0", path))
            {
                gExtractPackagePath = path;
            }
        }

        QtUi::TextWrapped("Destination Folder: %s", gExtractPackageOutputFolder.empty() ? "(not set)" : gExtractPackageOutputFolder.c_str());
        if (QtUi::Button("Choose Destination Folder..."))
        {
            std::wstring folder;
            if (PromptForFolder(GetActiveWindow(), L"Select Destination Folder", folder))
            {
                gExtractPackageOutputFolder = std::filesystem::path(folder).string();
            }
        }

        QtUi::Separator();
        const bool running = gExtractPackageState.Running.load();
        const bool canExtract = !running && !gExtractPackagePath.empty() && !gExtractPackageOutputFolder.empty();
        QtUi::BeginDisabled(!canExtract);
        if (QtUi::Button("Extract"))
        {
            gExtractPackageState.Running.store(true);
            {
                std::lock_guard<std::mutex> lock(gExtractPackageState.Mutex);
                gExtractPackageState.Status = "Extracting...";
            }

            const std::wstring projectRoot = GetProjectDataDirectoryCached().parent_path().wstring();
            const std::wstring pakPath = std::filesystem::path(gExtractPackagePath).wstring();
            const std::wstring destination = std::filesystem::path(gExtractPackageOutputFolder).wstring();
            std::thread([projectRoot, pakPath, destination]()
            {
                std::string error;
                const bool ok = ReleaseGame::ExtractOnePackage(projectRoot, pakPath, destination, error);
                std::lock_guard<std::mutex> lock(gExtractPackageState.Mutex);
                gExtractPackageState.Status = ok ? "Extraction complete." : ("Error: " + error);
                gExtractPackageState.Running.store(false);
            }).detach();
        }
        QtUi::EndDisabled();

        {
            std::lock_guard<std::mutex> lock(gExtractPackageState.Mutex);
            if (!gExtractPackageState.Status.empty())
                QtUi::TextWrapped("%s", gExtractPackageState.Status.c_str());
        }

        QtUi::End();
    }

    // ---- Game Settings -----------------------------------------------------------------
    // The project's player setup: which controller runs the player - FirstPersonCharacter
    // in Game.dll or a .nodegraph - and which graph. The character's values live in the
    // controller itself. Edited on a copy and written to Data/Game/GameSettings.json by
    // Save; a play session reads the file when it starts.
    bool gShowGameSettingsWindow = false;
    bool gGameSettingsLoaded = false;
    bool gGameSettingsDirty = false;
    GameProjectSettings gGameSettings;
    std::string gGameSettingsStatus;

    bool PromptForSaveFile(HWND owner, const char* title, const char* filter, const char* defaultExtension,
                           const std::string& initialPath, std::string& outPath)
    {
        char buffer[MAX_PATH] = {};
        strncpy_s(buffer, initialPath.c_str(), _TRUNCATE);
        OPENFILENAMEA saveFileName{};
        saveFileName.lStructSize = sizeof(saveFileName);
        saveFileName.hwndOwner = owner;
        saveFileName.lpstrFilter = filter;
        saveFileName.lpstrFile = buffer;
        saveFileName.nMaxFile = static_cast<DWORD>(std::size(buffer));
        saveFileName.lpstrTitle = title;
        saveFileName.lpstrDefExt = defaultExtension;
        saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!QtUi::SaveFileName(&saveFileName))
            return false;
        outPath = buffer;
        return true;
    }

    // Data-relative with forward slashes, or empty when `path` is outside Data - where a
    // packaged game could never read it.
    std::string DataRelativePath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path data = std::filesystem::weakly_canonical(GetProjectDataDirectoryCached(), error);
        const std::filesystem::path file = std::filesystem::weakly_canonical(path, error);
        const std::filesystem::path relative = file.lexically_relative(data);
        if (relative.empty() || *relative.begin() == "..")
            return std::string();
        return relative.generic_string();
    }

    void LoadGameSettingsForEditing()
    {
        std::string error;
        gGameSettingsStatus.clear();
        if (!gGameSettings.Load(GetProjectDataDirectoryCached(), &error))
            gGameSettingsStatus = error;
        gGameSettingsLoaded = true;
        gGameSettingsDirty = false;
    }

    void DrawGameSettingsWindow(const std::string& currentLevelPath)
    {
        if (!gShowGameSettingsWindow)
            return;

        if (!gGameSettingsLoaded)
            LoadGameSettingsForEditing();

        QtUi::SetNextWindowSize(UiVec2(560.0f, 480.0f), QtUiCond_FirstUseEver);
        if (!QtUi::Begin("Game Settings", &gShowGameSettingsWindow))
        {
            QtUi::End();
            return;
        }

        QtUi::SeparatorText("Player Controller");
        QtUi::TextWrapped(
            "Who drives the first-person character. A Node Graph controller also holds all of the "
            "character's movement, camera and control values, on its Set Movement / Camera / Control "
            "Settings nodes. The C++ controller sets them in FirstPersonCharacter::ConfigureCharacter.");

        const bool native = gGameSettings.PlayerController == PlayerControllerKind::Native;
        if (QtUi::RadioButton("C++ (Game.dll, FirstPersonCharacter)", native) && !native)
        {
            gGameSettings.PlayerController = PlayerControllerKind::Native;
            gGameSettingsDirty = true;
        }
        if (QtUi::RadioButton("Node Graph (.nodegraph)", !native) && native)
        {
            gGameSettings.PlayerController = PlayerControllerKind::NodeGraph;
            gGameSettingsDirty = true;
        }

        QtUi::TextWrapped("Controller graph: %s", gGameSettings.ControllerGraph.empty()
            ? "(none - the built-in First Person Controller is used)"
            : gGameSettings.ControllerGraph.c_str());

        const std::filesystem::path controllerDirectory =
            GameProjectSettings::DefaultControllerDirectory(GetProjectDataDirectoryCached());

        if (QtUi::Button("Choose .nodegraph..."))
        {
            std::string path;
            if (PromptForOpenFile(GetActiveWindow(), "Select Player Controller Graph",
                "Node Graph\0*.nodegraph\0All Files\0*.*\0", path))
            {
                const std::string relative = DataRelativePath(path);
                if (relative.empty())
                {
                    gGameSettingsStatus = "The controller graph must be inside the Data folder, or the packaged "
                                          "game cannot load it. Copy it into Data/Game/Controllers first.";
                }
                else
                {
                    gGameSettings.ControllerGraph = relative;
                    gGameSettingsDirty = true;
                    gGameSettingsStatus.clear();
                }
            }
        }
        QtUi::SameLine();
        if (QtUi::Button("Export Node Graph Controller..."))
        {
            std::error_code ignored;
            std::filesystem::create_directories(controllerDirectory, ignored);
            std::string path;
            if (PromptForSaveFile(GetActiveWindow(), "Export Player Controller Graph",
                "Node Graph\0*.nodegraph\0", "nodegraph",
                (controllerDirectory / "FirstPersonController.nodegraph").string(), path))
            {
                std::string error;
                if (!NodeGraphTemplates::FirstPersonController().SaveToFile(path, &error))
                {
                    gGameSettingsStatus = error;
                }
                else
                {
                    const std::string relative = DataRelativePath(path);
                    if (!relative.empty())
                    {
                        gGameSettings.ControllerGraph = relative;
                        gGameSettingsDirty = true;
                    }
                    gGameSettingsStatus = "Exported the First Person Controller to " + path +
                        (relative.empty() ? ". It is outside Data, so it was not selected."
                                          : ". Select Node Graph above and Save to play with it.");
                }
            }
        }
        QtUi::SetItemTooltip(
            "Writes the node version of the C++ controller - its settings nodes, movement, look, jump, "
            "crouch, sprint, and a right-mouse zoom - as a starting point for your own.");
        if (!gGameSettings.ControllerGraph.empty())
        {
            QtUi::SameLine();
            if (QtUi::Button("Clear"))
            {
                gGameSettings.ControllerGraph.clear();
                gGameSettingsDirty = true;
            }
        }
        QtUi::TextDisabledWrapped(
            "Edit a controller in Windows > Node Graph: File > Load Graph, then File > Export Graph back to "
            "the same file. Loading replaces the level's graph in that window, so export that first if the "
            "level has one.");

        QtUi::SeparatorText("Startup Level");
        QtUi::TextWrapped("Level the packaged game opens with: %s", gGameSettings.StartupLevel.empty()
            ? "(not set - Build Game needs one)"
            : gGameSettings.StartupLevel.c_str());
        const std::string openLevel = currentLevelPath.empty() ? std::string() : DataRelativePath(currentLevelPath);
        QtUi::BeginDisabled(openLevel.empty());
        if (QtUi::Button("Use Open Level"))
        {
            gGameSettings.StartupLevel = openLevel;
            gGameSettingsDirty = true;
        }
        QtUi::EndDisabled();
        QtUi::SameLine();
        if (QtUi::Button("Choose Level..."))
        {
            std::string path;
            if (PromptForOpenFile(GetActiveWindow(), "Select Startup Level",
                "Levels\0*.level;*.json\0All Files\0*.*\0", path))
            {
                const std::string relative = DataRelativePath(path);
                if (relative.empty())
                {
                    gGameSettingsStatus = "The startup level must be inside the Data folder.";
                }
                else
                {
                    gGameSettings.StartupLevel = relative;
                    gGameSettingsDirty = true;
                    gGameSettingsStatus.clear();
                }
            }
        }
        QtUi::TextDisabled("Playing in the editor always runs the level that is open.");

        QtUi::TextDisabledWrapped(
            "The player spawns at an entity named \"PlayerStart\" (feet at its position, facing its local +Y), "
            "or at the editor camera when the level has none.");

        QtUi::SeparatorText("Steam");
        int steamAppId = static_cast<int>(gGameSettings.SteamAppId);
        if (QtUi::InputInt("Steam App ID", &steamAppId, 0, 0))
        {
            gGameSettings.SteamAppId = static_cast<std::uint32_t>(steamAppId < 0 ? 0 : steamAppId);
            gGameSettingsDirty = true;
        }
        QtUi::TextDisabledWrapped(
            "0 = not a Steam build. With an App ID, Build Game ships steam_api64.dll and the packaged game "
            "only runs through the Steam client: started any other way it asks Steam to relaunch it, and it "
            "refuses to run when Steam is not running or the account does not own the game. Put a "
            "steam_appid.txt holding the ID next to the exe to test a build outside Steam (never ship it). "
            "The Steam DRM wrapper itself is applied after Build Game; see Documentation/Steam.md.");

        QtUi::SeparatorText("Pterosoft Launcher");
        int pterosoftProductId = static_cast<int>(gGameSettings.PterosoftProductId);
        if (QtUi::InputInt("Pterosoft Product ID", &pterosoftProductId, 0, 0))
        {
            gGameSettings.PterosoftProductId = static_cast<std::uint32_t>(pterosoftProductId < 0 ? 0 : pterosoftProductId);
            gGameSettingsDirty = true;
        }
        QtUi::TextDisabledWrapped(
            "0 = not sold through Pterosoft Launcher. With the store's product ID, the packaged game only runs "
            "when Pterosoft Launcher starts it: at every start it verifies the launcher's one-shot ticket online "
            "and refuses to run without a ticket, without a connection, or when the account does not own the "
            "game. Publish the build with minimum launcher version 0.8.1. See Documentation/PterosoftLauncher.md.");

        QtUi::Separator();
        if (QtUi::Button(gGameSettingsDirty ? "Save *" : "Save"))
        {
            std::string error;
            if (gGameSettings.Save(GetProjectDataDirectoryCached(), &error))
            {
                gGameSettingsDirty = false;
                gGameSettingsStatus = "Saved " + GameProjectSettings::FilePath(GetProjectDataDirectoryCached()).string() +
                    ". Applies from the next Play.";
            }
            else
            {
                gGameSettingsStatus = error;
            }
        }
        QtUi::SameLine();
        if (QtUi::Button("Revert"))
            LoadGameSettingsForEditing();

        if (!gGameSettingsStatus.empty())
            QtUi::TextWrapped("%s", gGameSettingsStatus.c_str());

        QtUi::End();
    }

    void StartShaderCompileCommand(const CompileShadersCommandFn compileShadersCommand)
    {
        if (compileShadersCommand == nullptr)
        {
            std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
            gShaderCompileMenu.Status = "Shader compile command is not available.";
            gShaderCompileMenu.HasResult = true;
            gShaderCompileMenu.LastSucceeded = false;
            gShowShaderCompileWindow = true;
            return;
        }

        bool expectedRunning = false;
        if (!gShaderCompileMenu.Running.compare_exchange_strong(expectedRunning, true))
        {
            gShowShaderCompileWindow = true;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
            gShaderCompileMenu.Status = "Compiling shaders into Cache\\Shaders...";
            gShaderCompileMenu.HasResult = false;
            gShaderCompileMenu.LastSucceeded = true;
        }
        gShowShaderCompileWindow = true;

        std::thread([compileShadersCommand]()
        {
            std::string statusMessage;
            bool succeeded = true;
            try
            {
                statusMessage = compileShadersCommand();
                succeeded = statusMessage.find(" failed") == std::string::npos;
            }
            catch (const std::exception& exception)
            {
                statusMessage = std::string("Shader compile command failed: ") + exception.what();
                succeeded = false;
            }
            catch (...)
            {
                statusMessage = "Shader compile command failed with an unknown error.";
                succeeded = false;
            }

            {
                std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
                gShaderCompileMenu.Status = statusMessage;
                gShaderCompileMenu.HasResult = true;
                gShaderCompileMenu.LastSucceeded = succeeded;
            }

            gShaderCompileMenu.Running.store(false);
        }).detach();
    }

    void RefreshMaterialNameBuffer()
    {
        strcpy_s(gMaterialNameBuffer, gMaterialEditor.GetCurrentMaterial().Name.c_str());
    }

    void RefreshMultiMaterialNameBuffer()
    {
        strcpy_s(gMultiMaterialNameBuffer, gMaterialEditor.GetCurrentMultiMaterial().Name.c_str());
    }

    // UV transform + parallax occlusion controls. Shared by the single-material tab and
    // by each sub-material of the multi-material tab so both stay in step.
    void DrawMaterialUvAndParallaxControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("UV Transform");

        QtUi::DragFloat("Tiling U", &materialDefinition.UvTiling[0], 0.05f, -256.0f, 256.0f, "%.3f");
        QtUi::SetItemTooltip("How many times the textures repeat across the mesh's U axis.");
        QtUi::DragFloat("Tiling V", &materialDefinition.UvTiling[1], 0.05f, -256.0f, 256.0f, "%.3f");
        QtUi::SetItemTooltip("How many times the textures repeat across the mesh's V axis.");

        if (QtUi::Button("Link V to U"))
        {
            materialDefinition.UvTiling[1] = materialDefinition.UvTiling[0];
        }
        QtUi::SameLine();
        if (QtUi::Button("Reset UV"))
        {
            materialDefinition.UvTiling = { 1.0f, 1.0f };
            materialDefinition.UvOffset = { 0.0f, 0.0f };
            materialDefinition.UvRotationDegrees = 0.0f;
        }

        QtUi::DragFloat("Offset U", &materialDefinition.UvOffset[0], 0.005f, -64.0f, 64.0f, "%.3f");
        QtUi::DragFloat("Offset V", &materialDefinition.UvOffset[1], 0.005f, -64.0f, 64.0f, "%.3f");
        QtUi::SliderFloat("Rotation", &materialDefinition.UvRotationDegrees, -180.0f, 180.0f, "%.1f deg");
        QtUi::SetItemTooltip("Rotates the source UVs about (0.5, 0.5) before tiling is applied.");

        QtUi::SeparatorText("Parallax Occlusion");

        const bool hasHeightTexture = !materialDefinition.Textures.HeightTexturePath.empty();
        QtUi::Checkbox("Parallax Occlusion Mapping", &materialDefinition.UseParallaxOcclusion);
        QtUi::SetItemTooltip("Ray marches the Height texture so the surface gains real depth "
                             "and self-occlusion without extra geometry.");
        if (materialDefinition.UseParallaxOcclusion && !hasHeightTexture)
        {
            QtUi::TextDisabled("Assign a Height texture below to see any effect.");
        }

        QtUi::BeginDisabled(!materialDefinition.UseParallaxOcclusion);
        QtUi::SliderFloat("Height Scale", &materialDefinition.HeightScale, 0.0f, 0.5f, "%.3f");
        QtUi::SetItemTooltip("Depth of the height volume in tiled UV units. Large values "
                             "exaggerate the effect and expose stretching at grazing angles.");
        QtUi::SliderFloat("Reference Plane", &materialDefinition.HeightReference, 0.05f, 1.0f, "%.3f");
        QtUi::SetItemTooltip("Height value that sits at the polygon surface. Leave at 1 for a "
                             "0-1 height map. For a Substance-style map that is signed around "
                             "0.5, set this to the map's brightest value, or the whole surface "
                             "sits half a volume deep and slides about instead of showing relief.");
        QtUi::SliderInt("Min Steps", &materialDefinition.ParallaxMinSteps, 1, 64);
        QtUi::SetItemTooltip("Ray-march steps used when looking straight at the surface.");
        QtUi::SliderInt("Max Steps", &materialDefinition.ParallaxMaxSteps, 1, 256);
        QtUi::SetItemTooltip("Ray-march steps used at grazing angles, where the ray travels furthest.");
        if (materialDefinition.ParallaxMaxSteps < materialDefinition.ParallaxMinSteps)
        {
            materialDefinition.ParallaxMaxSteps = materialDefinition.ParallaxMinSteps;
        }
        QtUi::SliderFloat("Fade Distance", &materialDefinition.ParallaxFadeDistance, 0.0f, 300.0f, "%.1f m");
        QtUi::SetItemTooltip("Parallax fades out past this distance so far surfaces skip the march. 0 disables the fade.");
        QtUi::EndDisabled();

        QtUi::SeparatorText("Tessellation & Displacement");

        QtUi::Checkbox("Tessellation", &materialDefinition.UseTessellation);
        QtUi::SetItemTooltip("Subdivides the surface on the GPU and moves the new vertices along the "
                             "normal by the Height / Displacement texture - real geometry with a real "
                             "silhouette. Works on meshes and terrain.");
        if (materialDefinition.UseTessellation && !hasHeightTexture)
        {
            QtUi::TextDisabled("Assign a Height / Displacement texture below to see any effect.");
        }

        QtUi::BeginDisabled(!materialDefinition.UseTessellation);
        QtUi::SliderFloat("Displacement (m)", &materialDefinition.DisplacementScale, 0.0f, 2.0f, "%.3f m");
        QtUi::SetItemTooltip("World distance between the texture's black and white.");
        QtUi::SliderFloat("Mid Level", &materialDefinition.DisplacementMidLevel, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Height value that stays on the original surface. 0.5 keeps the average "
                             "surface in place; 0 only pushes outward.");
        QtUi::SliderFloat("Max Subdivision", &materialDefinition.TessellationMaxFactor, 1.0f, 64.0f, "%.0f");
        QtUi::SetItemTooltip("Upper limit on how many pieces one edge is split into.");
        QtUi::SliderFloat("Target Edge (px)", &materialDefinition.TessellationTargetPixels, 2.0f, 64.0f, "%.0f px");
        QtUi::SetItemTooltip("Edges are split until each piece is about this long on screen. Smaller is "
                             "finer and more expensive.");
        QtUi::SliderFloat("Tess Fade Distance", &materialDefinition.TessellationFadeDistance, 0.0f, 500.0f, "%.0f m");
        QtUi::SetItemTooltip("Subdivision tapers off to none at this distance. 0 disables the fade.");
        QtUi::EndDisabled();
    }

    // Particle material controls. Shared by the single-material tab and by each
    // sub-material of the multi-material tab so both stay in step.
    void DrawMaterialParticleControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("Particle");

        QtUi::Checkbox("Particle Material", &materialDefinition.IsParticleMaterial);
        QtUi::SetItemTooltip("Drawn as a camera-facing sprite by the particle renderer instead of "
                             "into the G-Buffer. Base Color is the sprite texture, Emissive Color is the glow.");

        QtUi::BeginDisabled(!materialDefinition.IsParticleMaterial);

        const char* blendModes[] = { "Additive", "Alpha Blend", "Premultiplied" };
        int blendMode = static_cast<int>(materialDefinition.ParticleBlend);
        if (QtUi::Combo("Blend Mode", &blendMode, blendModes, static_cast<int>(std::size(blendModes))))
        {
            materialDefinition.ParticleBlend = static_cast<ParticleBlendMode>(blendMode);
        }
        QtUi::SetItemTooltip("Additive for fire and sparks, Alpha Blend for smoke and steam, "
                             "Premultiplied for atlases exported from a fluid sim.");

        QtUi::DragFloat("Emissive Intensity", &materialDefinition.ParticleEmissiveIntensity, 0.05f, 0.0f, 200.0f, "%.2f");
        QtUi::SetItemTooltip("Multiplies Emissive Color. This is what pushes a flame into HDR and "
                             "what the emitter turns into light for the GI.");

        QtUi::SliderFloat("Scene Lighting", &materialDefinition.ParticleLightingInfluence, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("How much the sprite is lit by the scene. 0 for fire (it makes its own "
                             "light), around 1 for smoke so it darkens away from the flame.");

        QtUi::BeginDisabled(materialDefinition.ParticleLightingInfluence <= 0.0f);
        QtUi::SliderFloat("Spherical Normal", &materialDefinition.ParticleSphericalNormal, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("0 shades the sprite as a flat card, 1 as a sphere. Thick smoke wants "
                             "the sphere; thin wisps want the card.");
        QtUi::EndDisabled();

        QtUi::DragFloat("Camera Fade", &materialDefinition.ParticleCameraFadeDistance, 0.01f, 0.0f, 10.0f, "%.2f m");
        QtUi::SetItemTooltip("Fades the sprite out as the camera gets this close, so flying through "
                             "a fire does not end in a full-screen flash.");

        QtUi::Checkbox("Alpha From Luminance", &materialDefinition.ParticleAlphaFromLuminance);
        QtUi::SetItemTooltip("Take opacity from the brightness of the RGB instead of the alpha channel.\n"
                             "Turn this on for additive sheets authored as colour on black - flames,\n"
                             "sparks, explosions - whose alpha channel is empty or junk.");

        QtUi::SliderInt("Flipbook Columns", &materialDefinition.ParticleFlipbookColumns, 1, 16);
        QtUi::SliderInt("Flipbook Rows", &materialDefinition.ParticleFlipbookRows, 1, 16);
        QtUi::SetItemTooltip("Atlas layout of the Base Color texture, read left to right, top to "
                             "bottom. 1x1 uses the texture whole. An emitter left at 1x1 inherits this.");

        if (materialDefinition.ParticleFlipbookColumns < 1) materialDefinition.ParticleFlipbookColumns = 1;
        if (materialDefinition.ParticleFlipbookRows < 1)    materialDefinition.ParticleFlipbookRows = 1;

        QtUi::EndDisabled();
    }

    // Disney principled BRDF lobes (PrincipledBrdf.hlsli). Shared by the single-material
    // tab and by each sub-material of the multi-material tab. They only take effect while
    // the Disney BRDF render setting is on; every one of them is off at 0.
    void DrawMaterialPrincipledControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("Disney Principled BRDF");

        // Labels are kept to words short enough for the property column: a single long word
        // does not wrap there and is cut off ("Subsurfac", "Anisotrop").
        QtUi::SliderFloat("Flatten", &materialDefinition.SubsurfaceFlatten, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Disney's \"subsurface\": flattens the diffuse response towards a thin-scattering "
                             "look, softening the terminator. A shape change of the diffuse lobe only; for "
                             "light that actually travels under the surface use Subsurface Scattering below.");

        QtUi::SliderFloat("Specular Tint", &materialDefinition.SpecularTint, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Tints the non-metallic reflection towards the base colour's hue. An artistic "
                             "control; real dielectrics reflect untinted.");

        QtUi::SliderFloat("Aniso", &materialDefinition.Anisotropic, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Anisotropic. Stretches the highlight along the texture's U direction: brushed metal, hair, "
                             "satin. Rotate the UVs to turn the streaks.");

        QtUi::SliderFloat("Sheen", &materialDefinition.Sheen, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Extra reflection at grazing angles, for cloth and velvet.");

        QtUi::BeginDisabled(materialDefinition.Sheen <= 0.0f);
        QtUi::SliderFloat("Sheen Tint", &materialDefinition.SheenTint, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("Tints the sheen towards the base colour's hue.");
        QtUi::EndDisabled();

        QtUi::SliderFloat("Clearcoat", &materialDefinition.Clearcoat, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("A second, colourless specular layer on top: car paint, lacquered wood, "
                             "varnish.");

        QtUi::BeginDisabled(materialDefinition.Clearcoat <= 0.0f);
        QtUi::SliderFloat("Clearcoat Gloss", &materialDefinition.ClearcoatGloss, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("0 is a satin coat, 1 a high-gloss one.");
        QtUi::EndDisabled();
    }

    // Subsurface scattering controls. Shared by the single-material tab and by each
    // sub-material of the multi-material tab. How the profile is evaluated (screen-space
    // blur or ray traced) is a scene setting, not a material one; the material only
    // describes the medium.
    void DrawMaterialSubsurfaceControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("Subsurface Scattering");

        QtUi::Checkbox("Subsurface Scattering", &materialDefinition.UseSubsurfaceScattering);
        QtUi::SetItemTooltip("Light entering the surface scatters inside it and leaves somewhere else, "
                             "softening the lighting and bleeding colour into shadow edges. For skin, "
                             "wax, marble, jade and leaves. Opaque materials only.");

        QtUi::BeginDisabled(!materialDefinition.UseSubsurfaceScattering);

        // Starting points, not ground truth: each is the SDK's skin profile re-weighted
        // toward how the medium tints and how far light travels in it.
        struct SubsurfacePreset
        {
            const char* Name;
            std::array<float, 3> Color;
            std::array<float, 3> Falloff;
            float RadiusMm;
            float Translucency;
        };
        static const SubsurfacePreset kPresets[] = {
            { "Skin",    { 0.48f, 0.41f, 0.28f }, { 1.0f, 0.37f, 0.30f },  3.0f, 0.80f },
            { "Wax",     { 0.85f, 0.75f, 0.55f }, { 1.0f, 0.75f, 0.45f }, 12.0f, 0.85f },
            { "Marble",  { 0.60f, 0.60f, 0.58f }, { 1.0f, 0.95f, 0.90f },  8.0f, 0.60f },
            { "Jade",    { 0.45f, 0.75f, 0.50f }, { 0.55f, 1.0f, 0.60f }, 10.0f, 0.75f },
            { "Foliage", { 0.35f, 0.70f, 0.20f }, { 0.60f, 1.0f, 0.35f },  4.0f, 0.95f },
            // Sclera and iris: a whiter, shorter-reaching medium than skin, with the red
            // of the blood vessels still carried furthest. Translucency is kept low - an
            // eyeball is too thick to glow through, but its rim should still soften.
            { "Eye",     { 0.62f, 0.55f, 0.50f }, { 1.0f, 0.55f, 0.45f },  1.5f, 0.35f },
        };
        QtUi::TextUnformatted("Preset");
        for (const SubsurfacePreset& preset : kPresets)
        {
            QtUi::SameLine();
            if (QtUi::Button(preset.Name))
            {
                materialDefinition.SubsurfaceColor = preset.Color;
                materialDefinition.SubsurfaceFalloff = preset.Falloff;
                materialDefinition.SubsurfaceRadiusMm = preset.RadiusMm;
                materialDefinition.SubsurfaceTranslucency = preset.Translucency;
            }
        }

        QtUi::ColorEdit3("Scatter Strength", materialDefinition.SubsurfaceColor.data());
        QtUi::SetItemTooltip("How much of each colour channel's diffuse light is scattered. "
                             "Black keeps the surface looking opaque; white scatters everything.");

        QtUi::ColorEdit3("Scatter Falloff", materialDefinition.SubsurfaceFalloff.data());
        QtUi::SetItemTooltip("How far each channel travels, relative to Scatter Radius. Skin carries red "
                             "furthest, which is what gives it its warm shadow edges.");

        QtUi::SliderFloat("Scatter Radius", &materialDefinition.SubsurfaceRadiusMm, 0.1f, 100.0f, "%.1f mm");
        QtUi::SetItemTooltip("World-space reach of the scattering. About 3 mm is human skin at real scale; "
                             "wax and marble want 8-15 mm. Scales with the object, not the screen.");

        QtUi::SliderFloat("Translucency", &materialDefinition.SubsurfaceTranslucency, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("How easily light passes all the way through thin parts - ears, fingers, "
                             "leaves, candle rims - and glows on the far side. 0 disables transmission.");

        QtUi::EndDisabled();

        if (materialDefinition.SubsurfaceRadiusMm < 0.01f)
            materialDefinition.SubsurfaceRadiusMm = 0.01f;
    }

    // Iris adjust controls for eye materials (GBuffer.hlsl ApplyIrisAdjust). Shared by the
    // single-material tab and by each sub-material of the multi-material tab.
    void DrawMaterialEyeControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("Eye (Iris)");

        QtUi::Checkbox("Iris Adjust", &materialDefinition.UseIrisAdjust);
        QtUi::SetItemTooltip("Move, resize and dilate the iris in the texture, so the eye can look "
                             "somewhere without rotating the eyeball mesh. Set Iris Center and Iris "
                             "Radius to match the texture first, then use Offset and the scales.");

        QtUi::BeginDisabled(!materialDefinition.UseIrisAdjust);

        QtUi::SliderFloat("Look U", &materialDefinition.IrisOffset[0], -0.3f, 0.3f, "%.3f");
        QtUi::SetItemTooltip("Moves the iris along the texture's U axis. Which way that turns the eye "
                             "depends on how the eyeball is unwrapped; a mirrored eye moves the other way.");
        QtUi::SliderFloat("Look V", &materialDefinition.IrisOffset[1], -0.3f, 0.3f, "%.3f");
        QtUi::SetItemTooltip("Moves the iris along the texture's V axis.");
        if (QtUi::Button("Center Look"))
        {
            materialDefinition.IrisOffset = { 0.0f, 0.0f };
        }

        QtUi::SliderFloat("Iris Scale", &materialDefinition.IrisScale, 0.5f, 2.0f, "%.2f");
        QtUi::SetItemTooltip("Size of the iris relative to the texture's. The sclera around it "
                             "stretches or squeezes to make room.");
        QtUi::SliderFloat("Pupil Scale", &materialDefinition.PupilScale, 0.3f, 3.0f, "%.2f");
        QtUi::SetItemTooltip("Dilates (above 1) or constricts (below 1) the pupil. The iris's outer "
                             "edge stays where it is.");

        QtUi::SeparatorText("Iris Layout (texture)");
        QtUi::SliderFloat("Iris Center U", &materialDefinition.IrisCenter[0], 0.0f, 1.0f, "%.3f");
        QtUi::SliderFloat("Iris Center V", &materialDefinition.IrisCenter[1], 0.0f, 1.0f, "%.3f");
        QtUi::SetItemTooltip("Where the iris sits in the texture. 0.5, 0.5 for most eyeball maps.");
        QtUi::SliderFloat("Iris Radius", &materialDefinition.IrisRadius, 0.01f, 0.5f, "%.3f");
        QtUi::SetItemTooltip("Radius of the iris in the texture, in UV. Too small and the iris's rim "
                             "smears into the sclera when moved; too large and sclera moves with it.");
        QtUi::SliderFloat("Pupil Radius", &materialDefinition.PupilRadius, 0.0f, 0.4f, "%.3f");
        QtUi::SetItemTooltip("Radius of the pupil in the texture, in UV. Only Pupil Scale uses it.");
        QtUi::SliderFloat("Warp Radius", &materialDefinition.IrisWarpRadius, 0.05f, 0.7f, "%.3f");
        QtUi::SetItemTooltip("Edge of the region the warp may stretch. Keep it inside the white of the "
                             "eye's texture; a larger one spreads the stretching more thinly and lets "
                             "the iris move further.");

        QtUi::EndDisabled();

        if (materialDefinition.PupilRadius > materialDefinition.IrisRadius * 0.95f)
            materialDefinition.PupilRadius = materialDefinition.IrisRadius * 0.95f;
    }

    bool PromptForSceneOpenPath(HWND ownerWindowHandle, char* sceneFileBuffer, const DWORD sceneFileBufferSize)
    {
        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = ownerWindowHandle;
        openFileName.lpstrTitle = "Open Scene";
        openFileName.lpstrFilter = "Level\0*.level;*.json\0Level (.level)\0*.level\0Legacy JSON Level (.json)\0*.json\0All Files\0*.*\0";
        openFileName.lpstrFile = sceneFileBuffer;
        openFileName.nMaxFile = sceneFileBufferSize;
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        return QtUi::OpenFileName(&openFileName) == TRUE;
    }

    bool PromptForSceneSavePath(HWND ownerWindowHandle, char* sceneFileBuffer, const DWORD sceneFileBufferSize)
    {
        OPENFILENAMEA saveFileName{};
        saveFileName.lStructSize = sizeof(saveFileName);
        saveFileName.hwndOwner = ownerWindowHandle;
        saveFileName.lpstrTitle = "Save Scene";
        saveFileName.lpstrFilter = "Level (.level)\0*.level\0Legacy JSON Level (.json)\0*.json\0All Files\0*.*\0";
        saveFileName.lpstrFile = sceneFileBuffer;
        saveFileName.nMaxFile = sceneFileBufferSize;
        saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        saveFileName.lpstrDefExt = "level";
        return QtUi::SaveFileName(&saveFileName) == TRUE;
    }

    struct AssetBrowserItem
    {
        std::string Name;
        std::string RelativePath;
        bool IsDirectory = false;
    };

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath);
    std::filesystem::path FindProjectDataDirectory();

    std::filesystem::path gCachedDataDirectory;
    bool gHasCachedDataDirectory = false;
    std::unordered_map<std::string, std::vector<std::string>> gCachedChildFolders;
    std::unordered_map<std::string, std::vector<AssetBrowserItem>> gCachedFolderItems;

    void InvalidateAssetBrowserDirectoryCache()
    {
        gCachedChildFolders.clear();
        gCachedFolderItems.clear();
    }

    const std::filesystem::path& GetProjectDataDirectoryCached()
    {
        if (!gHasCachedDataDirectory)
        {
            gCachedDataDirectory = FindProjectDataDirectory();
            gHasCachedDataDirectory = true;
        }

        return gCachedDataDirectory;
    }

    std::filesystem::path ResolveDataAssetPath(const std::string& assetPath)
    {
        if (assetPath.empty())
        {
            return {};
        }

        const std::filesystem::path candidatePath(assetPath);
        if (candidatePath.is_absolute())
        {
            return candidatePath;
        }

        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        return dataDirectory.empty() ? candidatePath : (dataDirectory / candidatePath);
    }

    std::optional<std::string> BuildDataRelativeAssetPath(const std::filesystem::path& assetFilePath)
    {
        if (assetFilePath.empty())
        {
            return std::nullopt;
        }

        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        if (dataDirectory.empty())
        {
            return std::nullopt;
        }

        std::error_code canonicalError;
        const std::filesystem::path canonicalDataDirectory = std::filesystem::weakly_canonical(dataDirectory, canonicalError);
        const std::filesystem::path dataRoot = canonicalError ? dataDirectory.lexically_normal() : canonicalDataDirectory;

        canonicalError.clear();
        std::filesystem::path absoluteAssetPath = assetFilePath.is_absolute()
            ? std::filesystem::weakly_canonical(assetFilePath, canonicalError)
            : std::filesystem::weakly_canonical(dataDirectory / assetFilePath, canonicalError);
        if (canonicalError)
        {
            absoluteAssetPath = assetFilePath.is_absolute()
                ? assetFilePath.lexically_normal()
                : (dataDirectory / assetFilePath).lexically_normal();
        }

        std::error_code relativeError;
        const std::filesystem::path relativePath = std::filesystem::relative(absoluteAssetPath, dataRoot, relativeError);
        if (relativeError || relativePath.empty())
        {
            return std::nullopt;
        }

        auto firstPathPart = relativePath.begin();
        if (firstPathPart != relativePath.end() && firstPathPart->string() == "..")
        {
            return std::nullopt;
        }

        const std::string normalizedPath = NormalizeRelativeDataPath(relativePath);
        if (normalizedPath.empty())
        {
            return std::nullopt;
        }

        return normalizedPath;
    }

    void DrawSelectedMeshMaterialBinding(Editor* editorInstance, const char* id, const std::filesystem::path& currentMaterialPath, const bool multiMaterialMode)
    {
        QtUi::PushID(id);
        QtUi::SeparatorText("Selected Mesh");

        Entity* selectedEntity = editorInstance != nullptr ? editorInstance->GetSelectedEntity() : nullptr;
        if (selectedEntity == nullptr)
        {
            QtUi::TextDisabled("No selected mesh.");
            QtUi::PopID();
            return;
        }

        if (!selectedEntity->Mesh.has_value())
        {
            QtUi::TextDisabled("Selected entity has no mesh component.");
            QtUi::PopID();
            return;
        }

        MeshComponent& meshComponent = *selectedEntity->Mesh;
        const std::size_t selectedCount = editorInstance->GetSelectedEntities().size();
        if (selectedCount > 1)
            QtUi::TextWrapped("Selected Entity: %s (+%zu more - Assign applies to every selected mesh)",
                              selectedEntity->Name.c_str(), selectedCount - 1);
        else
            QtUi::TextWrapped("Selected Entity: %s", selectedEntity->Name.c_str());
        QtUi::TextWrapped("Assigned Material: %s", meshComponent.MaterialPath.empty() ? "(none)" : meshComponent.MaterialPath.c_str());

        const std::optional<std::string> currentAssignmentPath = BuildDataRelativeAssetPath(currentMaterialPath);
        const std::string assignedPath = NormalizeRelativeDataPath(std::filesystem::path(meshComponent.MaterialPath));
        if (currentAssignmentPath.has_value()
            && !assignedPath.empty()
            && assignedPath != *currentAssignmentPath)
        {
            QtUi::TextColored(UiVec4(1.0f, 0.78f, 0.25f, 1.0f), "Current editor file is not assigned to the selected mesh.");
        }

        const bool hasAssignedMaterial = !meshComponent.MaterialPath.empty();
        QtUi::BeginDisabled(!hasAssignedMaterial);
        if (QtUi::Button("Open Assigned"))
        {
            const std::filesystem::path assignedMaterialPath = ResolveDataAssetPath(meshComponent.MaterialPath);
            if (multiMaterialMode)
            {
                if (gMaterialEditor.LoadMultiMaterialFromFile(assignedMaterialPath))
                {
                    gSelectedSubMaterialIndex = -1;
                    RefreshMultiMaterialNameBuffer();
                }
            }
            else if (gMaterialEditor.LoadMaterialFromFile(assignedMaterialPath))
            {
                RefreshMaterialNameBuffer();
            }
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        const bool canAssignCurrentMaterial = currentAssignmentPath.has_value();
        QtUi::BeginDisabled(!canAssignCurrentMaterial);
        if (QtUi::Button("Save + Assign Current"))
        {
            bool savedCurrentMaterial = false;
            if (multiMaterialMode)
            {
                gMaterialEditor.GetCurrentMultiMaterial().Name = gMultiMaterialNameBuffer;
                savedCurrentMaterial = gMaterialEditor.SaveCurrentMultiMaterial();
            }
            else
            {
                gMaterialEditor.GetCurrentMaterial().Name = gMaterialNameBuffer;
                savedCurrentMaterial = gMaterialEditor.SaveCurrentMaterial();
            }

            if (savedCurrentMaterial && editorInstance != nullptr)
            {
                // Every selected mesh takes the material, not only the one shown above.
                for (Entity* entity : editorInstance->GetSelectedEntities())
                {
                    if (entity->Mesh.has_value())
                        entity->Mesh->MaterialPath = *currentAssignmentPath;
                }
                editorInstance->MarkSceneDirty();
            }
        }
        QtUi::EndDisabled();

        if (!canAssignCurrentMaterial)
        {
            QtUi::TextDisabled("Save the current material before assigning it.");
        }

        QtUi::PopID();
    }

    std::string BuildChildRelativePath(const std::string& parentFolderRelativePath, const std::filesystem::path& childName)
    {
        return NormalizeRelativeDataPath(
            parentFolderRelativePath.empty()
            ? childName
            : (std::filesystem::path(parentFolderRelativePath) / childName));
    }

    using SystemImportFbxToDataFn = decltype(&System_ImportFbxToData);
    using SystemImportTextureToDataFn = decltype(&System_ImportTextureToData);
    using SystemGenerateMeshLodsFn = decltype(&System_GenerateMeshLods);
    using SystemGenerateCollisionsFn = decltype(&System_GenerateCollisions);
    using SystemImportUnrealAssetToDataFn = decltype(&System_ImportUnrealAssetToData);
    using SystemGetUnrealAssetInfoFn = decltype(&System_GetUnrealAssetInfo);
    using SystemIsUnrealImportAvailableFn = decltype(&System_IsUnrealImportAvailable);

    void InvalidateEditorMeshAssets(void* editor, const std::string& relativeGeometryPath);

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path normalizedPath = relativePath.lexically_normal();
        return (normalizedPath.empty() || normalizedPath == ".") ? std::string{} : normalizedPath.generic_string();
    }

    bool IsPathInsideRoot(const std::filesystem::path& rootPath, const std::filesystem::path& candidatePath)
    {
        const std::filesystem::path normalizedRootPath = rootPath.lexically_normal();
        const std::filesystem::path normalizedCandidatePath = candidatePath.lexically_normal();

        auto rootIterator = normalizedRootPath.begin();
        auto candidateIterator = normalizedCandidatePath.begin();
        for (; rootIterator != normalizedRootPath.end(); ++rootIterator, ++candidateIterator)
        {
            if (candidateIterator == normalizedCandidatePath.end() || *rootIterator != *candidateIterator)
            {
                return false;
            }
        }

        return true;
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
        {
            return {};
        }

        // Walk upward from the editor executable until the repository Data folder is found.
        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
            {
                return dataDirectory;
            }

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
            {
                break;
            }

            currentPath = parentPath;
        }

        return {};
    }

    std::filesystem::path GetAbsoluteDataPath(const std::string& relativePath)
    {
        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        return relativePath.empty()
            ? dataDirectory
            : (dataDirectory / std::filesystem::path(relativePath)).lexically_normal();
    }

    void SetAssetBrowserStatus(const bool succeeded, const std::string& statusMessage)
    {
        gLastAssetActionSucceeded = succeeded;
        gHasAssetActionResult = true;
        gAssetStatusMessage = statusMessage;
    }

    void RefreshAssetBrowserState()
    {
        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        if (dataDirectory.empty())
        {
            gDataDirectoryDisplay = "Failed to locate the repository Data folder.";
            gSelectedFolderRelativePath.clear();
            gSelectedAssetRelativePath.clear();
            return;
        }

        gDataDirectoryDisplay = dataDirectory.string();

        // Files added by the background importer since the last frame.
        if (gAssetImport.CacheDirty.exchange(false))
        {
            InvalidateAssetBrowserDirectoryCache();
        }

        const std::filesystem::path selectedFolderPath = GetAbsoluteDataPath(gSelectedFolderRelativePath);
        if (!gSelectedFolderRelativePath.empty() && !std::filesystem::exists(selectedFolderPath))
        {
            gSelectedFolderRelativePath.clear();
        }

        const std::filesystem::path selectedAssetPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        if (!gSelectedAssetRelativePath.empty() && !std::filesystem::exists(selectedAssetPath))
        {
            gSelectedAssetRelativePath.clear();
        }
    }

    const std::vector<std::string>& GetChildFolderRelativePaths(const std::string& parentFolderRelativePath)
    {
        static const std::vector<std::string> emptyChildFolders;

        const auto cachedFolders = gCachedChildFolders.find(parentFolderRelativePath);
        if (cachedFolders != gCachedChildFolders.end())
        {
            return cachedFolders->second;
        }

        std::vector<std::string> childFolders;

        const std::filesystem::path absoluteFolderPath = GetAbsoluteDataPath(parentFolderRelativePath);
        if (absoluteFolderPath.empty())
        {
            return emptyChildFolders;
        }

        std::error_code iteratorError;
        for (std::filesystem::directory_iterator it(absoluteFolderPath, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            std::error_code statusError;
            if (!it->is_directory(statusError) || statusError)
            {
                continue;
            }

            childFolders.push_back(BuildChildRelativePath(parentFolderRelativePath, it->path().filename()));
        }

        std::sort(childFolders.begin(), childFolders.end());
        return gCachedChildFolders.emplace(parentFolderRelativePath, std::move(childFolders)).first->second;
    }

    const std::vector<AssetBrowserItem>& CollectFolderItems(const std::string& folderRelativePath)
    {
        static const std::vector<AssetBrowserItem> emptyItems;

        const auto cachedItems = gCachedFolderItems.find(folderRelativePath);
        if (cachedItems != gCachedFolderItems.end())
        {
            return cachedItems->second;
        }

        std::vector<AssetBrowserItem> items;

        const std::filesystem::path absoluteFolderPath = GetAbsoluteDataPath(folderRelativePath);
        if (absoluteFolderPath.empty())
        {
            return emptyItems;
        }

        std::error_code iteratorError;
        for (std::filesystem::directory_iterator it(absoluteFolderPath, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            std::error_code statusError;
            const bool isDirectory = it->is_directory(statusError);
            const bool isRegularFile = it->is_regular_file(statusError);
            if (statusError || (!isDirectory && !isRegularFile))
            {
                continue;
            }

            AssetBrowserItem item;
            item.Name = it->path().filename().string();
            item.RelativePath = BuildChildRelativePath(folderRelativePath, it->path().filename());
            item.IsDirectory = isDirectory;
            items.push_back(item);
        }

        std::sort(
            items.begin(),
            items.end(),
            [](const AssetBrowserItem& leftItem, const AssetBrowserItem& rightItem)
            {
                if (leftItem.IsDirectory != rightItem.IsDirectory)
                {
                    return leftItem.IsDirectory > rightItem.IsDirectory;
                }

                return _stricmp(leftItem.Name.c_str(), rightItem.Name.c_str()) < 0;
            });

        return gCachedFolderItems.emplace(folderRelativePath, std::move(items)).first->second;
    }

    std::filesystem::path BuildUniqueDuplicatePath(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationDirectory)
    {
        const std::string baseName = sourcePath.stem().string().empty()
            ? sourcePath.filename().string()
            : sourcePath.stem().string();
        const std::string extension = sourcePath.has_extension() ? sourcePath.extension().string() : std::string{};

        std::filesystem::path candidatePath = destinationDirectory / (baseName + "_Copy" + extension);
        int copyIndex = 2;
        while (std::filesystem::exists(candidatePath))
        {
            candidatePath = destinationDirectory / (baseName + "_Copy" + std::to_string(copyIndex) + extension);
            ++copyIndex;
        }

        return candidatePath;
    }

    HMODULE EnsureSystemModuleLoaded(char* statusMessage, const int statusMessageCapacity)
    {
        // The renderer resolves the System DLL on demand so the editor UI can stay responsive even if the DLL is missing.
        HMODULE systemModule = GetModuleHandleW(L"System.dll");
        if (systemModule == nullptr)
        {
            systemModule = LoadLibraryW(L"System.dll");
        }

        if (systemModule == nullptr && statusMessage != nullptr && statusMessageCapacity > 0)
        {
            _snprintf_s(
                statusMessage,
                static_cast<size_t>(statusMessageCapacity),
                _TRUNCATE,
                "Failed to load System.dll. Win32 error: %lu",
                GetLastError());
        }

        return systemModule;
    }

    bool ImportFbxIntoDataFromSystem(
        const char* sourceFbxPath,
        const char* targetDirectoryRelativeToData,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto importFbx = reinterpret_cast<SystemImportFbxToDataFn>(GetProcAddress(systemModule, "System_ImportFbxToData"));
        if (importFbx == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_ImportFbxToData export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return importFbx(sourceFbxPath, targetDirectoryRelativeToData, statusMessage, statusMessageCapacity);
    }

    bool ImportTextureIntoDataFromSystem(
        const char* sourceTexturePath,
        const char* targetDirectoryRelativeToData,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto importTexture = reinterpret_cast<SystemImportTextureToDataFn>(GetProcAddress(systemModule, "System_ImportTextureToData"));
        if (importTexture == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_ImportTextureToData export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return importTexture(sourceTexturePath, targetDirectoryRelativeToData, statusMessage, statusMessageCapacity);
    }

    bool GenerateMeshLodsFromSystem(
        const char* geometryPath,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto generateMeshLods = reinterpret_cast<SystemGenerateMeshLodsFn>(GetProcAddress(systemModule, "System_GenerateMeshLods"));
        if (generateMeshLods == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_GenerateMeshLods export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return generateMeshLods(geometryPath, statusMessage, statusMessageCapacity);
    }

    bool GenerateCollisionsFromSystem(
        const char* fbxOrPteroPath,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto generateCollisions = reinterpret_cast<SystemGenerateCollisionsFn>(GetProcAddress(systemModule, "System_GenerateCollisions"));
        if (generateCollisions == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_GenerateCollisions export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return generateCollisions(fbxOrPteroPath, statusMessage, statusMessageCapacity);
    }

    bool ImportUnrealAssetIntoDataFromSystem(
        const char* sourceUassetPath,
        const char* targetDirectoryRelativeToData,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto importUnreal = reinterpret_cast<SystemImportUnrealAssetToDataFn>(GetProcAddress(systemModule, "System_ImportUnrealAssetToData"));
        if (importUnreal == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_ImportUnrealAssetToData export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return importUnreal(sourceUassetPath, targetDirectoryRelativeToData, statusMessage, statusMessageCapacity);
    }

    // Header-only look at a .uasset: its class, and whether it is importable and still
    // lacks an up-to-date converted file. Thread-safe.
    bool GetUnrealAssetInfoFromSystem(const std::filesystem::path& uassetPath, std::string& assetClass, bool& importable, bool& alreadyImported)
    {
        HMODULE systemModule = EnsureSystemModuleLoaded(nullptr, 0);
        const auto getInfo = systemModule != nullptr
            ? reinterpret_cast<SystemGetUnrealAssetInfoFn>(GetProcAddress(systemModule, "System_GetUnrealAssetInfo"))
            : nullptr;
        if (getInfo == nullptr)
        {
            return false;
        }

        char className[128] = {};
        importable = false;
        alreadyImported = false;
        const bool read = getInfo(uassetPath.string().c_str(), className, static_cast<int>(std::size(className)), &importable, &alreadyImported);
        assetClass = className;
        return read;
    }

    // Empty when Unreal assets can be imported on this machine, otherwise why not.
    const std::string& UnrealImportUnavailableReason()
    {
        static const std::string reason = []
        {
            HMODULE systemModule = EnsureSystemModuleLoaded(nullptr, 0);
            const auto isAvailable = systemModule != nullptr
                ? reinterpret_cast<SystemIsUnrealImportAvailableFn>(GetProcAddress(systemModule, "System_IsUnrealImportAvailable"))
                : nullptr;
            char message[512] = {};
            if (isAvailable == nullptr)
            {
                return std::string("System.dll does not support Unreal import.");
            }
            return isAvailable(message, static_cast<int>(std::size(message))) ? std::string{} : std::string(message);
        }();
        return reason;
    }

    bool IsGeometryAssetPath(const std::string& relativePath)
    {
        const std::string extension = std::filesystem::path(relativePath).extension().string();
        return _stricmp(extension.c_str(), ".ptero") == 0 || _stricmp(extension.c_str(), ".fbx") == 0;
    }

    bool RegenerateSelectedGeometryLods(void* editor)
    {
        if (gSelectedAssetRelativePath.empty() || !IsGeometryAssetPath(gSelectedAssetRelativePath))
        {
            SetAssetBrowserStatus(false, "Select an FBX or .ptero geometry asset before generating LODs.");
            return false;
        }

        const std::filesystem::path geometryPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        char statusMessage[512] = {};
        const bool succeeded = GenerateMeshLodsFromSystem(
            geometryPath.string().c_str(),
            statusMessage,
            static_cast<int>(std::size(statusMessage)));
        SetAssetBrowserStatus(succeeded, statusMessage);
        if (succeeded)
        {
            InvalidateEditorMeshAssets(editor, gSelectedAssetRelativePath);
        }
        return succeeded;
    }

    bool GenerateSelectedGeometryCollisions()
    {
        if (gSelectedAssetRelativePath.empty() || !IsGeometryAssetPath(gSelectedAssetRelativePath))
        {
            SetAssetBrowserStatus(false, "Select an FBX or .ptero geometry asset before generating collisions.");
            return false;
        }

        const std::filesystem::path geometryPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        char statusMessage[512] = {};
        const bool succeeded = GenerateCollisionsFromSystem(
            geometryPath.string().c_str(),
            statusMessage,
            static_cast<int>(std::size(statusMessage)));
        SetAssetBrowserStatus(succeeded, statusMessage);
        return succeeded;
    }

    void InvalidateEditorMeshAssets(void* editor, const std::string& relativeGeometryPath)
    {
        if (editor == nullptr || relativeGeometryPath.empty())
        {
            return;
        }

        Editor* editorInstance = static_cast<Editor*>(editor);
        const std::filesystem::path requestedPath(relativeGeometryPath);
        const std::string requestedPathString = requestedPath.generic_string();
        const std::string requestedStem = requestedPath.stem().generic_string();
        const bool requestedIsPtero = _stricmp(requestedPath.extension().string().c_str(), ".ptero") == 0;

        for (Entity& entity : editorInstance->GetEntities())
        {
            if (!entity.Mesh.has_value())
            {
                continue;
            }

            MeshComponent& meshComponent = *entity.Mesh;
            if (meshComponent.MeshPath.empty())
            {
                continue;
            }

            const std::filesystem::path entityMeshPath(meshComponent.MeshPath);
            const std::string entityPathString = entityMeshPath.generic_string();
            const std::string entityStem = entityMeshPath.stem().generic_string();
            const bool matchesExactPath = _stricmp(entityPathString.c_str(), requestedPathString.c_str()) == 0;
            const bool matchesCookedPair = requestedIsPtero && _stricmp(entityStem.c_str(), requestedStem.c_str()) == 0;
            const bool matchesFbxPair = !requestedIsPtero && _stricmp(entityPathString.c_str(), requestedPathString.c_str()) == 0;
            if (matchesExactPath || matchesCookedPair || matchesFbxPair)
            {
                meshComponent.MeshAsset.reset();
            }
        }
    }

    enum class ImportKind
    {
        Geometry,
        Texture,
        Video,
        Unreal,
        Unsupported
    };

    constexpr const char* kTextureImportExtensions[] = { ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".hdr" };

    ImportKind ClassifyImportPath(const std::string& sourcePath)
    {
        const std::string extension = std::filesystem::path(sourcePath).extension().string();
        if (_stricmp(extension.c_str(), ".fbx") == 0)
        {
            return ImportKind::Geometry;
        }

        if (_stricmp(extension.c_str(), ".uasset") == 0)
        {
            return ImportKind::Unreal;
        }

        for (const char* textureExtension : kTextureImportExtensions)
        {
            if (_stricmp(extension.c_str(), textureExtension) == 0)
            {
                return ImportKind::Texture;
            }
        }

        if (VideoImport::IsVideoFile(std::filesystem::path(sourcePath).wstring()))
        {
            return ImportKind::Video;
        }

        return ImportKind::Unsupported;
    }

    bool IsVideoAssetPath(const std::string& relativePath)
    {
        return _stricmp(std::filesystem::path(relativePath).extension().string().c_str(), ".webm") == 0;
    }

    // The Asset Browser icon for an entry, from Data/Icons/Editor. Levels and materials
    // are mostly .json, so for those the top-level folder decides.
    const char* GetAssetBrowserIcon(const AssetBrowserItem& item)
    {
        if (item.IsDirectory)
            return "asset-folder";

        const std::filesystem::path path(item.RelativePath);
        const std::string extension = path.extension().string();
        const auto is = [&extension](std::initializer_list<const char*> extensions)
        {
            for (const char* candidate : extensions)
                if (_stricmp(extension.c_str(), candidate) == 0)
                    return true;
            return false;
        };

        if (IsGeometryAssetPath(item.RelativePath) || is({ ".obj", ".assbin" }))
            return "geometry";
        if (is({ ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr", ".bmp", ".exr", ".tif", ".tiff", ".raw" }))
            return "asset-texture";
        if (is({ ".webm", ".mp4", ".mov", ".avi", ".mkv" }))
            return "asset-video";
        if (is({ ".wav", ".mp3", ".ogg", ".flac", ".bank" }))
            return "asset-audio";
        if (is({ ".level" }))
            return "asset-level";
        if (is({ ".material" }))
            return "material-editor";
        if (is({ ".particle" }))
            return "particles";
        if (is({ ".nodegraph" }))
            return "node-graph";
        if (is({ ".sequence" }))
            return "sequencer";
        if (is({ ".ttf", ".otf" }))
            return "asset-font";
        if (is({ ".rml" }))
            return "ui-editor";
        if (is({ ".style", ".rcss" }))
            return "asset-style";
        if (is({ ".hlsl", ".hlsli", ".h", ".cpp", ".py", ".cmd" }))
            return "asset-code";
        if (is({ ".json" }))
        {
            const std::string topFolder = path.begin() != path.end() ? path.begin()->string() : std::string{};
            if (_stricmp(topFolder.c_str(), "Levels") == 0)
                return "asset-level";
            if (_stricmp(topFolder.c_str(), "Materials") == 0 || _stricmp(topFolder.c_str(), "MultiMaterials") == 0)
                return "material-editor";
        }
        if (is({ ".json", ".xml", ".ini", ".txt", ".md", ".mtl" }))
            return "asset-text";
        return "asset-file";
    }

    void OpenVideoInPlayer(const std::string& relativePath)
    {
        const std::filesystem::path videoPath = GetAbsoluteDataPath(relativePath);
        const bool opened = VideoPlayerWindow::Open(videoPath.wstring());
        SetAssetBrowserStatus(
            opened,
            opened ? "Opened " + videoPath.filename().string() + " in the Video Player."
                   : std::string("The Video Player window could not be started."));
    }

    struct AssetImportJob
    {
        std::string SourcePath;
        // Data-relative folder the converted files go to. Sources already inside Data are
        // converted next to themselves whatever it says.
        std::string TargetFolder;
    };

    // Runs a batch of imports on a background thread behind the progress modal; each file
    // is routed to its importer by extension.
    bool StartAssetImport(std::vector<AssetImportJob> jobs)
    {
        if (gAssetImport.Running.load())
        {
            SetAssetBrowserStatus(false, "An import is already in progress.");
            return false;
        }
        if (jobs.empty())
        {
            return false;
        }

        // Videos are written by path rather than through System.dll, so they need the
        // absolute target, resolved here on the UI thread where the Data cache lives.
        std::vector<std::filesystem::path> targetDirectories;
        for (const AssetImportJob& job : jobs)
        {
            targetDirectories.push_back(GetAbsoluteDataPath(job.TargetFolder));
        }

        // Reset progress state and launch the background import thread.
        {
            std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
            gAssetImport.Log.clear();
            gAssetImport.CurrentFile.clear();
        }
        gAssetImport.Total.store(static_cast<int>(jobs.size()));
        gAssetImport.Done.store(0);
        gAssetImport.CurrentFraction.store(0.0f);
        gAssetImport.Cancel.store(false);
        gAssetImport.Running.store(true);

        std::thread([jobs = std::move(jobs), targetDirectories = std::move(targetDirectories)]()
        {
            // .uasset imports run side by side (System.dll caps the texture encodes they
            // share); FBX, texture and video imports go through state that is not
            // thread-safe - System's shared AssetManager, the video progress fraction - so
            // those take turns.
            std::atomic<std::size_t> nextJob{ 0 };
            std::mutex serialImports;
            auto worker = [&]()
            {
                for (std::size_t jobIndex = nextJob.fetch_add(1); jobIndex < jobs.size(); jobIndex = nextJob.fetch_add(1))
                {
                    const std::string& sourcePath = jobs[jobIndex].SourcePath;
                    const std::string& targetFolder = jobs[jobIndex].TargetFolder;
                    const std::filesystem::path& targetDirectory = targetDirectories[jobIndex];
                    const std::string fileName = std::filesystem::path(sourcePath).filename().string();
                    {
                        std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                        gAssetImport.CurrentFile = fileName;
                    }
                    gAssetImport.CurrentFraction.store(0.0f);

                    bool ok = false;
                    std::string statusText;
                    std::unique_lock<std::mutex> serial(serialImports, std::defer_lock);
                    if (ClassifyImportPath(sourcePath) != ImportKind::Unreal)
                    {
                        serial.lock();
                    }
                    if (gAssetImport.Cancel.load())
                    {
                        statusText = "skipped (import cancelled)";
                    }
                    else
                    {
                        char statusMsg[1024] = {};
                        switch (ClassifyImportPath(sourcePath))
                        {
                        case ImportKind::Geometry:
                            ok = ImportFbxIntoDataFromSystem(
                                sourcePath.c_str(), targetFolder.c_str(), statusMsg, static_cast<int>(std::size(statusMsg)));
                            statusText = statusMsg;
                            break;
                        case ImportKind::Texture:
                            ok = ImportTextureIntoDataFromSystem(
                                sourcePath.c_str(), targetFolder.c_str(), statusMsg, static_cast<int>(std::size(statusMsg)));
                            statusText = statusMsg;
                            break;
                        case ImportKind::Unreal:
                            ok = ImportUnrealAssetIntoDataFromSystem(
                                sourcePath.c_str(), targetFolder.c_str(), statusMsg, static_cast<int>(std::size(statusMsg)));
                            statusText = statusMsg;
                            break;
                        case ImportKind::Video:
                        {
                            std::wstring outputPath;
                            ok = VideoImport::ImportIntoDirectory(
                                std::filesystem::path(sourcePath).wstring(),
                                targetDirectory.wstring(),
                                outputPath,
                                statusText,
                                [](float fraction) { gAssetImport.CurrentFraction.store(fraction); },
                                &gAssetImport.Cancel);
                            break;
                        }
                        case ImportKind::Unsupported:
                            statusText = "unsupported file type";
                            break;
                        }
                    }

                    std::string logLine = ok
                        ? (std::string("OK  ") + fileName + (statusText.empty() ? "" : ": " + statusText))
                        : (std::string("ERR ") + fileName + ": " + statusText);

                    if (ok)
                    {
                        // Only flagged here: the cache belongs to the UI thread, which may be
                        // iterating it right now. RefreshAssetBrowserState picks the flag up.
                        gAssetImport.CacheDirty.store(true);
                    }

                    {
                        std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                        gAssetImport.Log.push_back(std::move(logLine));
                    }
                    gAssetImport.Done.fetch_add(1);
                }
            };

            const std::size_t workerCount = (std::min<std::size_t>)(jobs.size(), 3);
            std::vector<std::thread> workers;
            for (std::size_t index = 1; index < workerCount; ++index)
            {
                workers.emplace_back(worker);
            }
            worker();
            for (std::thread& thread : workers)
            {
                thread.join();
            }

            {
                std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                gAssetImport.CurrentFile.clear();
            }
            gAssetImport.Running.store(false);
        }).detach();

        return true;
    }

    // The one Import command: geometry (FBX), Unreal .uasset files, textures and videos in a
    // single multi-select, each routed to its importer by extension. Everything runs on a
    // background thread behind the progress modal, since both texture compression and video
    // conversion can take a while.
    bool PromptForAndImportAssets(HWND windowHandle, const std::string& targetFolderRelativePath)
    {
        if (gAssetImport.Running.load())
        {
            SetAssetBrowserStatus(false, "An import is already in progress.");
            return false;
        }

        std::string texturePattern;
        for (const char* textureExtension : kTextureImportExtensions)
        {
            texturePattern += (texturePattern.empty() ? "*" : ";*") + std::string(textureExtension);
        }

        const std::string videoPattern = VideoImport::GetFileDialogPattern();
        std::string filter;
        const auto addFilter = [&filter](const std::string& label, const std::string& pattern)
        {
            filter += label;
            filter.push_back('\0');
            filter += pattern;
            filter.push_back('\0');
        };
        addFilter("All Supported Assets", "*.fbx;*.uasset;" + texturePattern + ";" + videoPattern);
        addFilter("Geometry (*.fbx)", "*.fbx");
        addFilter("Unreal Engine 5 assets (*.uasset)", "*.uasset");
        addFilter("Textures", texturePattern);
        addFilter("Videos (converted to WebM)", videoPattern);
        addFilter("All Files", "*.*");
        filter.push_back('\0');

        // Use a large buffer so Windows can pack multiple selected paths.
        // Format: "dir\0file1\0file2\0...\0\0"  (or just "full\path\0\0" for single selection)
        constexpr DWORD kMultiSelectBufferSize = 65536;
        std::vector<char> fileBuffer(kMultiSelectBufferSize, '\0');

        // Start browsing in the folder being imported into.
        const std::string initialDirectory = GetAbsoluteDataPath(targetFolderRelativePath).string();

        OPENFILENAMEA openFileName{};
        openFileName.lStructSize  = sizeof(openFileName);
        openFileName.hwndOwner    = windowHandle;
        openFileName.lpstrFilter  = filter.c_str();
        openFileName.lpstrFile    = fileBuffer.data();
        openFileName.nMaxFile     = kMultiSelectBufferSize;
        openFileName.lpstrTitle   = "Import Assets";
        openFileName.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
        openFileName.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;

        if (!QtUi::OpenFileName(&openFileName))
        {
            return false;
        }

        // Parse the multi-select result buffer.
        // Single selection: the buffer is just the full path (no embedded nulls after the name).
        // Multi selection:  buffer = "directory\0name1\0name2\0...\0\0"
        std::vector<std::string> sourcePaths;
        const char* p = fileBuffer.data();
        std::string directory = p;
        p += directory.size() + 1;

        if (*p == '\0')
        {
            // Single file – the whole string is the full path.
            sourcePaths.push_back(directory);
        }
        else
        {
            // Multiple files – prepend directory to each name.
            while (*p != '\0')
            {
                std::string name = p;
                sourcePaths.push_back(directory + "\\" + name);
                p += name.size() + 1;
            }
        }

        if (sourcePaths.empty())
        {
            return false;
        }

        std::vector<AssetImportJob> jobs;
        for (const std::string& sourcePath : sourcePaths)
        {
            jobs.push_back({ sourcePath, targetFolderRelativePath });
        }
        return StartAssetImport(std::move(jobs));
    }

    // -----------------------------------------------------------------------
    // "New files detected" - files that appear in Data from outside the editor are offered
    // for import in a bottom-right notification, as Unreal's content browser does.
    // -----------------------------------------------------------------------

    // Folders whose files are used as they are, never converted.
    bool IsInPassThroughFolder(const std::filesystem::path& dataDirectory, const std::filesystem::path& file)
    {
        static const char* const kFolders[] = { "UI", "Icons", "Styles", "Fonts", "Shaders", "Levels", "Audio", "fmod_project", "Game" };
        const std::filesystem::path relative = file.lexically_relative(dataDirectory);
        if (relative.empty() || relative.begin() == relative.end())
        {
            return true;
        }
        const std::string top = relative.begin()->string();
        for (const char* folder : kFolders)
        {
            if (_stricmp(top.c_str(), folder) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // True when a file that appeared in Data still needs converting. Runs on the watcher
    // thread; everything it touches is either immutable or thread-safe.
    bool ClassifyNewDataFile(const std::filesystem::path& dataDirectory, const std::filesystem::path& file, std::string& kind)
    {
        const std::string name = file.filename().string();
        if (name.empty() || name[0] == '~' || name[0] == '.')
        {
            return false;
        }

        std::error_code ec;
        const auto hasFreshOutput = [&](const std::filesystem::path& output)
        {
            return std::filesystem::exists(output, ec) && std::filesystem::last_write_time(output, ec) >= std::filesystem::last_write_time(file, ec);
        };
        std::filesystem::path sibling = file;

        switch (ClassifyImportPath(file.string()))
        {
        case ImportKind::Unreal:
        {
            bool importable = false;
            bool imported = false;
            if (!GetUnrealAssetInfoFromSystem(file, kind, importable, imported))
            {
                return false;
            }
            return importable && !imported;
        }
        case ImportKind::Geometry:
            kind = "FBX";
            return !IsInPassThroughFolder(dataDirectory, file) && !hasFreshOutput(file.string() + ".ptero");
        case ImportKind::Texture:
            kind = "Texture";
            if (_stricmp(file.extension().string().c_str(), ".dds") == 0 || IsInPassThroughFolder(dataDirectory, file))
            {
                return false;
            }
            // Imported beside the source, or into Data/Textures (the default target).
            return !std::filesystem::exists(sibling.replace_extension(".dds"), ec) &&
                !std::filesystem::exists(dataDirectory / "Textures" / (file.stem().string() + ".dds"), ec);
        case ImportKind::Video:
            kind = "Video";
            if (_stricmp(file.extension().string().c_str(), ".webm") == 0 || IsInPassThroughFolder(dataDirectory, file))
            {
                return false;
            }
            return !std::filesystem::exists(sibling.replace_extension(".webm"), ec);
        default:
            return false;
        }
    }

    std::vector<DataFolderWatcher::DetectedFile> gNewDataFiles;
    bool gNewDataFilesNeedRecheck = false;
    bool gImportWasRunning = false;

    std::string DescribeNewDataFiles(const std::filesystem::path& dataDirectory)
    {
        // Where they are: the deepest folder containing all of them.
        std::filesystem::path common = gNewDataFiles.front().Path.parent_path();
        for (const DataFolderWatcher::DetectedFile& file : gNewDataFiles)
        {
            while (!common.empty() && file.Path.parent_path().lexically_relative(common).string().rfind("..", 0) == 0)
            {
                common = common.parent_path();
            }
        }
        const std::string folder = NormalizeRelativeDataPath(common.lexically_relative(dataDirectory));

        std::map<std::string, int> counts;
        for (const DataFolderWatcher::DetectedFile& file : gNewDataFiles)
        {
            const std::string& kind = file.Kind;
            const char* label = kind == "StaticMesh" ? "static mesh"
                : (kind == "Material" || kind == "MaterialInstanceConstant") ? "material"
                : (kind == "Texture2D" || kind == "TextureCube" || kind == "Texture") ? "texture"
                : kind == "FBX" ? "FBX model"
                : kind == "Video" ? "video"
                : "file";
            ++counts[label];
        }
        std::string summary;
        for (const auto& [label, count] : counts)
        {
            summary += (summary.empty() ? "" : ", ") + std::to_string(count) + " " + label +
                (count == 1 ? "" : (std::string(label) == "static mesh" ? "es" : "s"));
        }

        const std::size_t total = gNewDataFiles.size();
        std::string text = std::to_string(total) + (total == 1 ? " new file" : " new files") +
            (folder.empty() ? std::string(" in Data") : " in Data/" + folder) + ": " + summary + ".";

        const bool hasUnreal = std::any_of(gNewDataFiles.begin(), gNewDataFiles.end(), [](const DataFolderWatcher::DetectedFile& file)
        {
            return ClassifyImportPath(file.Path.string()) == ImportKind::Unreal;
        });
        if (hasUnreal && !UnrealImportUnavailableReason().empty())
        {
            text += "\n\n" + UnrealImportUnavailableReason();
        }
        return text;
    }

    // Called every frame: runs the Data watcher and shows the notification while
    // detected files wait for an answer.
    void UpdateNewDataFilesNotification()
    {
        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        if (dataDirectory.empty())
        {
            return;
        }
        DataFolderWatcher::Start(dataDirectory, [dataDirectory](const std::filesystem::path& file, std::string& kind)
        {
            return ClassifyNewDataFile(dataDirectory, file, kind);
        });

        if (DataFolderWatcher::ConsumeChanged())
        {
            InvalidateAssetBrowserDirectoryCache();
        }

        // An import writes into Data itself; hold the question until it finishes, then
        // drop whatever it converted.
        const bool importRunning = gAssetImport.Running.load();
        if (gImportWasRunning && !importRunning)
        {
            gNewDataFilesNeedRecheck = true;
        }
        gImportWasRunning = importRunning;
        if (importRunning)
        {
            return;
        }

        for (DataFolderWatcher::DetectedFile& detected : DataFolderWatcher::TakeDetectedFiles())
        {
            const bool known = std::any_of(gNewDataFiles.begin(), gNewDataFiles.end(), [&](const DataFolderWatcher::DetectedFile& file)
            {
                return _wcsicmp(file.Path.lexically_normal().c_str(), detected.Path.lexically_normal().c_str()) == 0;
            });
            if (!known)
            {
                gNewDataFiles.push_back(std::move(detected));
            }
        }

        if (gNewDataFilesNeedRecheck)
        {
            gNewDataFilesNeedRecheck = false;
            gNewDataFiles.erase(std::remove_if(gNewDataFiles.begin(), gNewDataFiles.end(), [&](const DataFolderWatcher::DetectedFile& file)
            {
                std::string kind;
                return !ClassifyNewDataFile(dataDirectory, file.Path, kind);
            }), gNewDataFiles.end());
        }

        if (gNewDataFiles.empty())
        {
            return;
        }

        const std::string text = DescribeNewDataFiles(dataDirectory);
        const int choice = QtUi::Notification("NewDataFiles", "New files detected", text.c_str(), "Import", "Don't Import");
        if (choice == 1)
        {
            // Meshes first: each pulls in and converts its materials' textures in parallel,
            // so the textures queued after them are mostly already up to date.
            auto priority = [](const DataFolderWatcher::DetectedFile& file)
            {
                if (file.Kind == "StaticMesh" || file.Kind == "FBX") return 0;
                if (file.Kind == "Material" || file.Kind == "MaterialInstanceConstant") return 1;
                return 2;
            };
            std::stable_sort(gNewDataFiles.begin(), gNewDataFiles.end(), [&](const auto& a, const auto& b) { return priority(a) < priority(b); });

            std::vector<AssetImportJob> jobs;
            for (const DataFolderWatcher::DetectedFile& file : gNewDataFiles)
            {
                // Converted in place, next to the file that arrived.
                jobs.push_back({ file.Path.string(), NormalizeRelativeDataPath(file.Path.parent_path().lexically_relative(dataDirectory)) });
            }
            if (StartAssetImport(std::move(jobs)))
            {
                gShowAssetBrowserWindow = true;   // the progress modal lives there
                gSelectedFolderRelativePath = NormalizeRelativeDataPath(gNewDataFiles.front().Path.parent_path().lexically_relative(dataDirectory));
                gNewDataFiles.clear();
            }
        }
        else if (choice == 2 || choice == 3)
        {
            std::vector<std::filesystem::path> declined;
            for (const DataFolderWatcher::DetectedFile& file : gNewDataFiles)
            {
                declined.push_back(file.Path);
            }
            DataFolderWatcher::Ignore(declined);
            gNewDataFiles.clear();
        }
    }

    void CopySelectedAssetToClipboard()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            return;
        }

        gClipboardRelativePath = gSelectedAssetRelativePath;
        gClipboardHasValue = true;
        SetAssetBrowserStatus(true, "Copied asset selection to the browser clipboard.");
    }

    bool PasteClipboardIntoSelectedFolder()
    {
        if (!gClipboardHasValue || gClipboardRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Copy a file or folder before using Paste.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path sourcePath = GetAbsoluteDataPath(gClipboardRelativePath);
        const std::filesystem::path destinationDirectory = GetAbsoluteDataPath(gSelectedFolderRelativePath);
        if (dataDirectory.empty() || !std::filesystem::exists(sourcePath) || !std::filesystem::exists(destinationDirectory))
        {
            SetAssetBrowserStatus(false, "The source or destination path is no longer available inside Data.");
            return false;
        }

        const std::filesystem::path destinationPath = BuildUniqueDuplicatePath(sourcePath, destinationDirectory);
        if (!IsPathInsideRoot(dataDirectory, destinationPath))
        {
            SetAssetBrowserStatus(false, "Paste must stay inside the Data directory.");
            return false;
        }

        // Prevent recursive folder duplication by blocking paste operations into the copied folder or one of its children.
        if (std::filesystem::is_directory(sourcePath) && IsPathInsideRoot(sourcePath, destinationDirectory))
        {
            SetAssetBrowserStatus(false, "Cannot paste a folder into itself or one of its children.");
            return false;
        }

        std::error_code copyError;
        if (std::filesystem::is_directory(sourcePath))
        {
            std::filesystem::copy(sourcePath, destinationPath, std::filesystem::copy_options::recursive, copyError);
        }
        else
        {
            std::filesystem::copy_file(sourcePath, destinationPath, std::filesystem::copy_options::overwrite_existing, copyError);
        }

        if (copyError)
        {
            SetAssetBrowserStatus(false, std::string("Paste failed: ") + copyError.message());
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(destinationPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Pasted asset into the selected folder.");
        return true;
    }

    bool DeleteSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before removing it.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path selectedPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        if (dataDirectory.empty() || !std::filesystem::exists(selectedPath) || !IsPathInsideRoot(dataDirectory, selectedPath))
        {
            SetAssetBrowserStatus(false, "The selected asset is no longer available inside Data.");
            return false;
        }

        std::error_code removeError;
        if (std::filesystem::is_directory(selectedPath))
        {
            std::filesystem::remove_all(selectedPath, removeError);
        }
        else
        {
            std::filesystem::remove(selectedPath, removeError);
        }

        if (removeError)
        {
            SetAssetBrowserStatus(false, std::string("Remove failed: ") + removeError.message());
            return false;
        }

        gSelectedAssetRelativePath.clear();
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Removed asset from the Data folder.");
        return true;
    }

    bool RenameSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before renaming it.");
            return false;
        }

        const std::string requestedName = gRenameBuffer;
        if (requestedName.empty() || requestedName.find_first_of("\\/") != std::string::npos)
        {
            SetAssetBrowserStatus(false, "Enter a valid file or folder name.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path selectedPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        const std::filesystem::path renamedPath = selectedPath.parent_path() / requestedName;
        if (dataDirectory.empty() || !std::filesystem::exists(selectedPath) || !IsPathInsideRoot(dataDirectory, renamedPath))
        {
            SetAssetBrowserStatus(false, "The selected asset cannot be renamed outside the Data directory.");
            return false;
        }

        if (std::filesystem::exists(renamedPath))
        {
            SetAssetBrowserStatus(false, "An asset with that name already exists in the selected folder.");
            return false;
        }

        std::error_code renameError;
        std::filesystem::rename(selectedPath, renamedPath, renameError);
        if (renameError)
        {
            SetAssetBrowserStatus(false, std::string("Rename failed: ") + renameError.message());
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(renamedPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Renamed asset successfully.");
        return true;
    }

    bool CreateFolderInSelectedFolder()
    {
        const std::string requestedFolderName = gNewFolderBuffer;
        if (requestedFolderName.empty() || requestedFolderName.find_first_of("\\/") != std::string::npos)
        {
            SetAssetBrowserStatus(false, "Enter a valid folder name.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path newFolderPath = GetAbsoluteDataPath(gSelectedFolderRelativePath) / requestedFolderName;
        if (dataDirectory.empty() || !IsPathInsideRoot(dataDirectory, newFolderPath))
        {
            SetAssetBrowserStatus(false, "New folders must stay inside the Data directory.");
            return false;
        }

        std::error_code createError;
        if (!std::filesystem::create_directory(newFolderPath, createError) || createError)
        {
            SetAssetBrowserStatus(false, createError ? std::string("Create folder failed: ") + createError.message() : "A folder with that name already exists.");
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(newFolderPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Created a new folder inside the selected directory.");
        return true;
    }

    void BeginRenameSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before renaming it.");
            return;
        }

        const std::filesystem::path selectedPath(gSelectedAssetRelativePath);
        strcpy_s(gRenameBuffer, selectedPath.filename().string().c_str());
        gOpenRenamePopup = true;
    }

    void BeginCreateFolder()
    {
        gNewFolderBuffer[0] = '\0';
        gOpenNewFolderPopup = true;
    }

    void SelectFolder(const std::string& folderRelativePath)
    {
        gSelectedFolderRelativePath = folderRelativePath;
        gSelectedAssetRelativePath.clear();
    }

    void RenderFolderTreeNode(const std::string& folderRelativePath)
    {
        const std::filesystem::path folderPath = folderRelativePath.empty()
            ? std::filesystem::path("Data")
            : std::filesystem::path(folderRelativePath).filename();
        const std::vector<std::string> childFolders = GetChildFolderRelativePaths(folderRelativePath);
        const QtUiTreeNodeFlags nodeFlags = QtUiTreeNodeFlags_OpenOnArrow
            | QtUiTreeNodeFlags_OpenOnDoubleClick
            | (childFolders.empty() ? QtUiTreeNodeFlags_Leaf : 0)
            | (gSelectedFolderRelativePath == folderRelativePath ? QtUiTreeNodeFlags_Selected : 0);

        QtUi::SetNextItemIcon("asset-folder");
        const bool isOpen = QtUi::TreeNodeEx(folderRelativePath.empty() ? "DataRoot" : folderRelativePath.c_str(), nodeFlags, "%s", folderPath.string().c_str());
        if (QtUi::IsItemClicked())
        {
            SelectFolder(folderRelativePath);
        }

        if (QtUi::BeginPopupContextItem())
        {
            if (QtUi::MenuItem("Import Here..."))
            {
                PromptForAndImportAssets(GetActiveWindow(), folderRelativePath);
                RefreshAssetBrowserState();
            }

            if (QtUi::MenuItem("New Folder"))
            {
                SelectFolder(folderRelativePath);
                BeginCreateFolder();
            }

            QtUi::EndPopup();
        }

        if (isOpen)
        {
            for (const std::string& childFolderRelativePath : childFolders)
            {
                RenderFolderTreeNode(childFolderRelativePath);
            }

            QtUi::TreePop();
        }
    }
}

void RenderEditorMainMenu(
    HWND windowHandle,
    void* editor,
    UiTextureID sceneTextureId,
    const char* sceneStatusMessage,
    const char* statisticsText,
    float* cameraSpeed,
    float* viewDistanceMeters,
    bool* gridEnabled,
    bool* showStatistics,
    bool* showViewportPlacementIcons,
    bool* showComponentsPanel,
    bool* showLevelExplorerPanel,
    bool* showPropertiesPanel,
    bool* showResourceDebugPanel,
    TaaSettings* taaSettings,
    SMAASettings* smaaSettings,
    AnalyticEdgeAASettings* analyticEdgeAaSettings,
    AarSettings* aarSettings,
    const char* aarStatus,
    MsaaSettings* msaaSettings,
    SharpenSettings* sharpenSettings,
    DlssSettings* dlssSettings,
    FsrSettings* fsrSettings,
    const FsrRuntimeStatus* fsrStatus,
    XessSettings* xessSettings,
    const XessRuntimeStatus* xessStatus,
    TimeOfDaySettings* timeOfDaySettings,
    WindSettings* windSettings,
    GlobalIlluminationMode* globalIlluminationMode,
    RtGISettings* rtgiSettings,
    RadianceCascadesSettings* radianceCascadesSettings,
    RadianceProbeSettings* probeSettings,
    VoxelGISettings* voxelGiSettings,
    const VoxelGIStatus* voxelGiStatus,
    RtAOSettings* rtaoSettings,
    PathTracerSettings* pathTracerSettings,
    const char* pathTracerStatus,
    GtaoSettings* gtaoSettings,
    SsrSettings* ssrSettings,
    SubsurfaceSettings* subsurfaceSettings,
    bool subsurfaceRayTracingSupported,
    ShadingSettings* shadingSettings,
    ChromaticAberrationSettings* chromaticAberrationSettings,
    AgxTonemapSettings* agxSettings,
    VolumetricFogSettings* volumetricFogSettings,
    VolumetricCloudSettings* volumetricCloudSettings,
    BloomSettings* bloomSettings,
    LensFlareSettings* lensFlareSettings,
    const LensFlareRenderer* lensFlareRenderer,
    PointShadowSettings* pointShadowSettings,
    VirtualShadowMapSettings* virtualShadowMapSettings,
    const char* virtualShadowMapStatus,
    RtShadowSettings* rtShadowSettings,
    DpleSettings* dpleSettings,
    const char* dpleErrorMessage,
    const GBufferDebugTextureIds* gbufferTextureIds,
    AudioManager* audioManager,
    CompileShadersCommandFn compileShadersCommand,
    float msaaResolveTimeMs)
{
    UNREFERENCED_PARAMETER(sceneTextureId);
    UNREFERENCED_PARAMETER(statisticsText);

    Editor* editorInstance = static_cast<Editor*>(editor);

    UpdateNewDataFilesNotification();

    if (QtUi::BeginMainMenuBar())
    {
        const bool sceneLoading = editorInstance != nullptr && editorInstance->IsSceneLoading();

        if (QtUi::BeginMenu("File"))
        {
            QtUi::SetNextItemIcon("new");
            if (editorInstance != nullptr && QtUi::MenuItem("New", "Ctrl+N", false, !sceneLoading))
            {
                editorInstance->NewScene(windowHandle);
            }

            QtUi::SetNextItemIcon("open");
            if (editorInstance != nullptr && QtUi::MenuItem("Open...", "Ctrl+O", false, !sceneLoading))
            {
                // Through OpenScene, not straight to the loader: that is what
                // asks about unsaved changes first. This item used to call the
                // loader directly and could discard an edited level silently,
                // while Ctrl+O - the same command - asked.
                editorInstance->OpenScene(windowHandle);
            }

            QtUi::SetNextItemIcon("recent");
            if (editorInstance != nullptr && QtUi::BeginMenu("Recent Levels", !sceneLoading))
            {
                // Copied: opening a level reorders the list, and a missing file leaves it.
                const std::vector<std::string> recentLevels = editorInstance->GetRecentLevels();
                if (recentLevels.empty())
                {
                    QtUi::MenuItem("No recent levels", nullptr, false, false);
                }

                const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
                for (const std::string& levelPath : recentLevels)
                {
                    // Data-relative where possible, which is how levels are thought of.
                    std::string shown = levelPath;
                    std::error_code relativeError;
                    const std::filesystem::path relativePath = dataDirectory.empty()
                        ? std::filesystem::path{}
                        : std::filesystem::relative(std::filesystem::path(levelPath), dataDirectory, relativeError);
                    if (!relativeError && !relativePath.empty() && *relativePath.begin() != "..")
                    {
                        shown = relativePath.generic_string();
                    }

                    QtUi::SetNextItemIcon("asset-level");
                    if (QtUi::MenuItem((shown + "##recent:" + levelPath).c_str()))
                    {
                        editorInstance->OpenRecentLevel(windowHandle, levelPath);
                    }
                }

                QtUi::Separator();
                if (QtUi::MenuItem("Clear Recent Levels", nullptr, false, !recentLevels.empty()))
                {
                    editorInstance->ClearRecentLevels();
                }

                QtUi::EndMenu();
            }

            QtUi::SetNextItemIcon("save");
            if (editorInstance != nullptr && QtUi::MenuItem("Save", "Ctrl+S", false, !sceneLoading))
            {
                editorInstance->SaveScene(windowHandle);
            }

            // All four File commands now go through the Editor rather than
            // duplicating its logic here, so the menu and the shortcuts cannot
            // drift apart the way Open had.
            QtUi::SetNextItemIcon("save-as");
            if (editorInstance != nullptr && QtUi::MenuItem("Save As...", nullptr, false, !sceneLoading))
            {
                editorInstance->SaveSceneAs(windowHandle);
            }

            QtUi::Separator();
            const bool shaderCompileRunning = gShaderCompileMenu.Running.load();
            QtUi::SetNextItemIcon("compile-shaders");
            if (QtUi::MenuItem("Compile Shaders", nullptr, false, compileShadersCommand != nullptr && !shaderCompileRunning))
            {
                StartShaderCompileCommand(compileShadersCommand);
            }

            QtUi::Separator();
            QtUi::SetNextItemIcon("exit");
            if (QtUi::MenuItem("Exit"))
            {
                PostMessage(windowHandle, WM_CLOSE, 0, 0);
            }

            QtUi::EndMenu();
        }

        if (editorInstance != nullptr && QtUi::BeginMenu("Edit"))
        {
            QtUi::SetNextItemIcon("undo");
            if (QtUi::MenuItem("Undo", "Ctrl+Z", false, editorInstance->CanUndo()))
            {
                editorInstance->Undo();
            }

            QtUi::SetNextItemIcon("redo");
            if (QtUi::MenuItem("Redo", "Ctrl+Y", false, editorInstance->CanRedo()))
            {
                editorInstance->Redo();
            }

            QtUi::Separator();

            const bool canCopy = editorInstance->CanCopySelectedEntity();
            const bool canPaste = editorInstance->CanPasteEntity();
            const bool canDelete = editorInstance->CanDeleteSelectedEntity();

            QtUi::SetNextItemIcon("copy");
            if (QtUi::MenuItem("Copy", "Ctrl+C", false, canCopy))
            {
                editorInstance->CopySelectedEntity();
            }

            QtUi::SetNextItemIcon("paste");
            if (QtUi::MenuItem("Paste", "Ctrl+V", false, canPaste))
            {
                editorInstance->PasteCopiedEntity();
            }

            QtUi::SetNextItemIcon("delete");
            if (QtUi::MenuItem("Delete", "Delete", false, canDelete))
            {
                editorInstance->DeleteSelectedEntity();
            }

            QtUi::Separator();
            QtUi::SetNextItemIcon("editor-settings");
            if (QtUi::MenuItem("Editor Settings..."))
            {
                gShowEditorSettingsWindow = true;
                QtUi::AvailableStyles(true);
            }

            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("View"))
        {
            QtUi::SetNextItemIcon("gbuffer");
            if (QtUi::MenuItem("G-Buffer / RT Debug..."))
                gShowGBufferDebugWindow = true;

            // Virtualized geometry debug view. Written through the vg.* cvars,
            // which point at the renderer's live settings, so this menu, the
            // Console and the Mesh panel always show the same state.
            const CVar::Var* vgDebugView = CVar::Find("vg.debugview");
            const CVar::Var* vgFreeze = CVar::Find("vg.freeze");
            const CVar::Var* vgAll = CVar::Find("vg.all");
            if (vgDebugView != nullptr && vgDebugView->IntValue != nullptr
                && QtUi::BeginMenu("Virtual Geometry"))
            {
                if (vgAll != nullptr && vgAll->BoolValue != nullptr)
                {
                    if (QtUi::MenuItem("Virtualize All Meshes", nullptr, *vgAll->BoolValue))
                        *vgAll->BoolValue = !*vgAll->BoolValue;
                    QtUi::Separator();
                }

                // The colours replace only what the virtualized path draws.
                QtUi::MenuItem("Debug view (virtualized meshes only):", nullptr, false, false);
                const char* modes[] = { "Off", "Clusters", "Instances", "LOD (DAG Level)" };
                for (int mode = 0; mode < static_cast<int>(std::size(modes)); ++mode)
                {
                    if (QtUi::MenuItem(modes[mode], nullptr, *vgDebugView->IntValue == mode))
                        *vgDebugView->IntValue = mode;
                }

                if (vgFreeze != nullptr && vgFreeze->BoolValue != nullptr)
                {
                    QtUi::Separator();
                    if (QtUi::MenuItem("Freeze Culling", nullptr, *vgFreeze->BoolValue))
                        *vgFreeze->BoolValue = !*vgFreeze->BoolValue;
                }

                QtUi::EndMenu();
            }

            QtUi::EndMenu();
        }

        if (editorInstance != nullptr && QtUi::BeginMenu("Play"))
        {
            // Both entries always, one of them greyed, rather than a single item whose
            // label follows the state: menu nodes are keyed by their label, so a name
            // that changed would add its action at the end of the menu - below the mode
            // entries - the first time the state flipped.
            const bool playing = editorInstance->IsPlaySessionActive();
            QtUi::SetNextItemIcon("play-start");
            if (QtUi::MenuItem("Play", nullptr, false, !playing && !sceneLoading))
                editorInstance->RequestPlaySession();
            QtUi::SetNextItemIcon("stop");
            if (QtUi::MenuItem("Stop", nullptr, false, playing))
                editorInstance->StopPlaySession();
            QtUi::Separator();
            editorInstance->DrawPlayModeMenuItems();
            QtUi::EndMenu();
        }

        if (audioManager != nullptr && audioManager->IsInitialized() && QtUi::BeginMenu("Audio"))
        {
            QtUi::SetNextItemIcon("audio-manager");
            if (QtUi::MenuItem("Audio Manager..."))
                if (editorInstance) *editorInstance->GetShowAudioManagerPanelPointer() = true;
            QtUi::SetNextItemIcon("stop-audio");
            if (QtUi::MenuItem("Stop All"))
                audioManager->StopAll();
            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Windows"))
        {
            QtUi::SetNextItemIcon("settings");
            if (QtUi::MenuItem("Scene Settings..."))
            {
                gShowSceneSettingsWindow = true;
            }

            QtUi::SetNextItemIcon("material-editor");
            if (QtUi::MenuItem("Material Editor..."))
            {
                gShowMaterialEditorWindow = true;
                RefreshMaterialNameBuffer();
            }

            QtUi::SetNextItemIcon("particles");
            if (editorInstance != nullptr && QtUi::MenuItem("Particle Editor..."))
            {
                editorInstance->OpenParticleEditor();
            }

            QtUi::SetNextItemIcon("node-graph");
            if (QtUi::MenuItem("Node Graph..."))
            {
                NodeGraphEditor::Show();
            }

            QtUi::SetNextItemIcon("sequencer");
            if (QtUi::MenuItem("Cinematic Editor..."))
            {
                SequencerEditor::Show();
            }

            if (showComponentsPanel != nullptr)
            {
                QtUi::MenuItem("Components", nullptr, showComponentsPanel);
            }

            if (showLevelExplorerPanel != nullptr)
            {
                QtUi::MenuItem("Level Explorer", nullptr, showLevelExplorerPanel);
            }

            if (showPropertiesPanel != nullptr)
            {
                QtUi::MenuItem("Properties", nullptr, showPropertiesPanel);
            }

            if (showResourceDebugPanel != nullptr)
            {
                QtUi::MenuItem("Resource Debug", nullptr, showResourceDebugPanel);
            }

            if (editorInstance != nullptr)
            {
                QtUi::MenuItem("Console", nullptr, editorInstance->GetShowConsolePanelPointer());
                QtUi::MenuItem("UI Editor", nullptr, editorInstance->GetShowUiEditorPanelPointer());
            }

            if (editorInstance != nullptr)
            {
                bool* showTerrainToolWindow = editorInstance->GetShowTerrainToolWindowPointer();
                QtUi::MenuItem("Terrain Tool", nullptr, showTerrainToolWindow);
            }

            QtUi::SetNextItemIcon("asset-browser");
            if (QtUi::MenuItem("Asset Browser..."))
            {
                gShowAssetBrowserWindow = true;
                InvalidateAssetBrowserDirectoryCache();
                RefreshAssetBrowserState();
            }

            // A separate native window (Video.dll); with no file it offers Open....
            QtUi::SetNextItemIcon("video-player");
            if (QtUi::MenuItem("Video Player..."))
            {
                VideoPlayerWindow::Open(std::wstring());
            }

            QtUi::Separator();

            QtUi::SetNextItemIcon("time-of-day");
            if (QtUi::MenuItem("Time of Day..."))
            {
                gShowTimeOfDayWindow = true;
            }

            QtUi::SetNextItemIcon("graphics-settings");
            if (QtUi::MenuItem("Graphics Settings..."))
            {
                gShowGraphicsSettingsWindow = true;
            }

            // Its own window rather than a section of Graphics Settings: DPLE is tuned by
            // eye, so it wants a window that can sit beside the viewport while tuning.
            QtUi::SetNextItemIcon("dple");
            if (QtUi::MenuItem("DPLE..."))
            {
                gShowDpleWindow = true;
            }

            QtUi::SetNextItemIcon("settings");
            if (QtUi::MenuItem("Game Settings..."))
            {
                gShowGameSettingsWindow = true;
                // Re-read on every open, so a file edited by hand or by another tool shows up.
                if (!gGameSettingsDirty)
                    gGameSettingsLoaded = false;
            }

            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Release"))
        {
            QtUi::SetNextItemIcon("build-game");
            if (QtUi::MenuItem("Build Game..."))
            {
                gShowBuildGameWindow = true;
            }
            QtUi::SetNextItemIcon("extract-package");
            if (QtUi::MenuItem("Extract Package..."))
            {
                gShowExtractPackageWindow = true;
            }
            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Help"))
        {
            QtUi::SetNextItemIcon("about");
            if (QtUi::MenuItem("About Ptero Editor"))
            {
                gShowAboutWindow = true;
            }

            QtUi::EndMenu();
        }

        QtUi::EndMainMenuBar();
    }


    if (gShowSceneSettingsWindow)
    {
        QtUi::Begin("Scene Settings", &gShowSceneSettingsWindow, QtUiWindowFlags_AlwaysAutoResize);

        if (cameraSpeed != nullptr)
        {
            // Expose the camera speed directly in the editor so movement tuning does not require a rebuild.
            // The upper bound also caps what the right-drag mouse wheel can reach, since the
            // slider writes its own clamped value back every frame.
            QtUi::SliderFloat("Camera Speed", cameraSpeed, 0.0f, 200.0f, "%.2f");
            QtUi::SetItemTooltip("Hold the right mouse button in the viewport and scroll the wheel to change this while flying.");
        }

        if (gridEnabled != nullptr)
        {
            // Let the user hide the reference grid without changing the renderer's geometry setup.
            QtUi::Checkbox("Show Grid", gridEnabled);
        }

        if (showStatistics != nullptr)
        {
            // Keep the statistics toggle in scene settings so the viewport overlay can be disabled without code changes.
            QtUi::Checkbox("Show Renderer Statistics", showStatistics);
        }

        if (showViewportPlacementIcons != nullptr)
        {
            // Let the user hide the placement markers when they want an unobstructed view of the scene.
            QtUi::Checkbox("Show Placement Icons & Light Shapes", showViewportPlacementIcons);
        }

        QtUi::End();
    }

    if (gShowShaderCompileWindow)
    {
        QtUi::SetNextWindowSize(UiVec2(560.0f, 150.0f), QtUiCond_Appearing);
        if (QtUi::Begin("Shader Compile", &gShowShaderCompileWindow, QtUiWindowFlags_AlwaysAutoResize))
        {
            const bool running = gShaderCompileMenu.Running.load();
            std::string statusMessage;
            bool hasResult = false;
            bool succeeded = true;
            {
                std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
                statusMessage = gShaderCompileMenu.Status;
                hasResult = gShaderCompileMenu.HasResult;
                succeeded = gShaderCompileMenu.LastSucceeded;
            }

            if (running)
            {
                QtUi::ProgressBar(-1.0f * static_cast<float>(QtUi::GetTime()), UiVec2(-FLT_MIN, 0.0f), "Compiling");
            }

            const UiVec4 statusColor = (!hasResult || succeeded)
                ? UiVec4(0.35f, 0.85f, 0.45f, 1.0f)
                : UiVec4(0.90f, 0.45f, 0.35f, 1.0f);
            QtUi::TextColored(statusColor, "%s", statusMessage.c_str());

            QtUi::BeginDisabled(running);
            if (QtUi::Button("Close"))
            {
                gShowShaderCompileWindow = false;
            }
            QtUi::EndDisabled();
        }
        QtUi::End();
    }

    if (gShowTimeOfDayWindow && timeOfDaySettings != nullptr)
    {
        const TimeOfDaySettings defaultTimeOfDaySettings{};
        QtUi::SetNextWindowSize(UiVec2(420.0f, 0.0f), QtUiCond_FirstUseEver);
        QtUi::Begin("Time of Day", &gShowTimeOfDayWindow, QtUiWindowFlags_AlwaysAutoResize);
        if (QtUi::Button("Revert All##timeofday"))
        {
            *timeOfDaySettings = defaultTimeOfDaySettings;
        }

        QtUi::Checkbox("Enable Time Of Day##timeofday", &timeOfDaySettings->Enabled);
        QtUi::SetItemTooltip("Turns off the sun, the sky ambient, the procedural sky and the cloud "
                             "layer, leaving the scene lit only by the lights placed in it. Wind is "
                             "independent and keeps running.");

        // Everything down to the Wind section describes the sky, so it all greys out
        // together. Wind stays live: vegetation and rain read it regardless of the sky.
        QtUi::BeginDisabled(!timeOfDaySettings->Enabled);

        // ---- Time of day ------------------------------------------------
        QtUi::SeparatorText("Time of Day");

        // Display as HH:MM for readability.
        int hours   = static_cast<int>(timeOfDaySettings->TimeOfDay);
        int minutes = static_cast<int>((timeOfDaySettings->TimeOfDay - hours) * 60.0f);
        QtUi::Text("  %02d:%02d", hours, minutes);
        SliderFloatWithInput("Time Of Day", &timeOfDaySettings->TimeOfDay, 0.0f, 23.99f, "%.2f h", 0.05f, 1.0f);
        SliderFloatWithInput("Latitude##timeofday", &timeOfDaySettings->Latitude, -89.0f, 89.0f, "%.1f deg", 0.5f, 5.0f);
        QtUi::SetItemTooltip("Degrees north. The sun rises due east at 06:00, sets due west at 18:00 "
                             "and stands at 90 minus this at noon. 30 gives the 60 degree noon sun.");
        SliderFloatWithInput("North Offset##timeofday", &timeOfDaySettings->NorthOffset, -180.0f, 180.0f, "%.0f deg", 1.0f, 15.0f);
        QtUi::SetItemTooltip("Turns the sun path, moon and stars about the vertical. At 0 the sun "
                             "rises toward +X and stands toward -Y at noon.");

        // What the hour currently resolves to, so the exposure curve and the
        // sun/moon handover can be read rather than guessed at.
        const HosekWilkieResult todState = EvaluateHosekWilkie(*timeOfDaySettings);
        QtUi::Text("  Sun %.1f deg, moon %.1f deg - lit by the %s",
            todState.SolarElevationRad * (180.0f / 3.14159265f),
            todState.MoonElevationRad * (180.0f / 3.14159265f),
            todState.KeyLightIsMoon ? "moon" : "sun");

        QtUi::Spacing();
        QtUi::SeparatorText("Atmosphere");
        // Turbidity: 1 = pristine, 10 = heavy haze. Tooltip describes the range.
        SliderFloatWithInput("Turbidity", &timeOfDaySettings->Turbidity, 1.0f, 10.0f, "%.1f");
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("1 = very clear sky, 10 = heavy haze / smog");
        SliderFloatWithInput("Ground Albedo", &timeOfDaySettings->GroundAlbedo, 0.0f, 1.0f, "%.2f");
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Reflectivity of the ground (affects horizon brightness)");

        // ---- Sun --------------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Sun");

        SliderFloatWithInput("Sun Intensity (lux)", &timeOfDaySettings->SunIntensityLux,
            0.0f, 200000.0f, "%.0f lx", 100.0f, 1000.0f);
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Real-world noon sun: ~100 000 lx\nOvercast day: ~1 000 lx");

        QtUi::Checkbox("Override Sun Color##sun", &timeOfDaySettings->OverrideSunColor);
        if (timeOfDaySettings->OverrideSunColor)
        {
            float sunCol[3] = { timeOfDaySettings->SunColorR,
                                timeOfDaySettings->SunColorG,
                                timeOfDaySettings->SunColorB };
            if (QtUi::ColorEdit3("Sun Color", sunCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->SunColorR = sunCol[0];
                timeOfDaySettings->SunColorG = sunCol[1];
                timeOfDaySettings->SunColorB = sunCol[2];
            }
        }

        // ---- Sun glare --------------------------------------------------
        // Its own pass, so the level's bloom keeps whatever it was tuned to. The moon's
        // glow uses the same controls further down.
        const auto skyGlareControls = [](const char* title, const std::string& id, const char* body,
                                         const char* tooltip, SkyGlareSettings& glare, const SkyGlareSettings& defaults)
        {
            const auto label = [&id](const char* name) { return std::string(name) + id; };
            QtUi::Spacing();
            QtUi::SeparatorText(title);
            QtUi::Checkbox(label(title).c_str(), &glare.Enabled);
            QtUi::SetItemTooltip(tooltip);
            QtUi::SameLine();
            if (QtUi::SmallButton(label("Revert").c_str()))
                glare = defaults;
            QtUi::BeginDisabled(!glare.Enabled);
            SliderFloatWithInput(label("Intensity").c_str(), &glare.Intensity, 0.0f, 0.5f, "%.3f", 0.001f, 0.01f);
            QtUi::SetItemTooltip((std::string("Share of the ") + body + "'s light spread into the glare.").c_str());
            SliderFloatWithInput(label("Halo Core").c_str(), &glare.HaloCore, 0.05f, 5.0f, "%.2f deg", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Radius of the bright centre the halo stays flat over before it falls off. "
                                 "Never smaller than the disc.");
            SliderFloatWithInput(label("Halo Radius").c_str(), &glare.HaloRadius, 1.0f, 90.0f, "%.1f deg", 0.5f, 5.0f);
            QtUi::SetItemTooltip("How far the halo reaches; it fades smoothly to nothing there.");
            SliderFloatWithInput(label("Halo Falloff").c_str(), &glare.HaloFalloff, 1.0f, 6.0f, "%.2f", 0.05f, 0.25f);
            QtUi::SetItemTooltip("Exponent of the falloff past the core. Higher = tighter glare.");
            SliderFloatWithInput(label("Spike Strength").c_str(), &glare.StreakStrength, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
            QtUi::SetItemTooltip("Share of the glare in the aperture's diffraction spikes. 0 = none.");
            QtUi::BeginDisabled(glare.StreakStrength <= 0.0f);
            SliderFloatWithInput(label("Spike Length").c_str(), &glare.StreakLength, 0.5f, 60.0f, "%.1f deg", 0.5f, 5.0f);
            QtUi::SliderInt(label("Aperture Blades").c_str(), &glare.ApertureBlades, 3, 16);
            QtUi::SetItemTooltip("An even count gives that many spikes, an odd one twice as many.");
            SliderFloatWithInput(label("Aperture Rotation").c_str(), &glare.ApertureRotation, 0.0f, 180.0f, "%.0f deg", 1.0f, 15.0f);
            QtUi::EndDisabled();
            SliderFloatWithInput(label("Chromatic Spread").c_str(), &glare.ChromaticSpread, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
            QtUi::SetItemTooltip("Red reaches further than blue, tinting the glare's edge warm. 0 = neutral.");
            QtUi::EndDisabled();
        };
        skyGlareControls("Sun Glare", "##sunglare", "sun",
            "The glare the eye and the lens scatter around the sun, from its true brightness. "
            "Separate from the level's bloom, which only sees the sun disc as a white pixel.",
            timeOfDaySettings->SunGlare, defaultTimeOfDaySettings.SunGlare);

        // ---- Sky --------------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Sky");

        SliderFloatWithInput("Sky Intensity (lux)", &timeOfDaySettings->SkyIntensityLux,
            0.0f, 100000.0f, "%.0f lx", 100.0f, 1000.0f);
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Clear blue sky: ~20 000 lx\nOvercast: ~10 000 lx");

        QtUi::Checkbox("Override Sky Color##sky", &timeOfDaySettings->OverrideSkyColor);
        if (timeOfDaySettings->OverrideSkyColor)
        {
            float skyCol[3] = { timeOfDaySettings->SkyColorR,
                                timeOfDaySettings->SkyColorG,
                                timeOfDaySettings->SkyColorB };
            if (QtUi::ColorEdit3("Sky Color", skyCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->SkyColorR = skyCol[0];
                timeOfDaySettings->SkyColorG = skyCol[1];
                timeOfDaySettings->SkyColorB = skyCol[2];
            }
        }

        // ---- Night ------------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Night");

        SliderFloatWithInput("Night Sky Intensity (lux)", &timeOfDaySettings->NightSkyIntensityLux,
            0.0f, 2000.0f, "%.1f lx", 1.0f, 10.0f);
        QtUi::SetItemTooltip("Sky ambient once the sun is 12 degrees down. The daylight sky fades "
                             "into this through twilight. A game value, not starlight's real fraction of a lux.");
        {
            float nightCol[3] = { timeOfDaySettings->NightSkyColorR,
                                  timeOfDaySettings->NightSkyColorG,
                                  timeOfDaySettings->NightSkyColorB };
            if (QtUi::ColorEdit3("Night Sky Color", nightCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->NightSkyColorR = nightCol[0];
                timeOfDaySettings->NightSkyColorG = nightCol[1];
                timeOfDaySettings->NightSkyColorB = nightCol[2];
            }
        }
        SliderFloatWithInput("Night Desaturation##timeofday", &timeOfDaySettings->NightDesaturation, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
        QtUi::SetItemTooltip("Night vision: how much colour the dark parts of the frame lose at night, as the "
                             "eye's rods take over. Lights and the moon keep their colour. Comes in from sunset, "
                             "full once the sun is 8 degrees down. 0 = off.");

        QtUi::Checkbox("Moon##timeofday", &timeOfDaySettings->MoonEnabled);
        QtUi::SetItemTooltip("The moon takes over as the directional light, with shadows, once the sun is down.");
        QtUi::BeginDisabled(!timeOfDaySettings->MoonEnabled);
        SliderFloatWithInput("Moon Phase##timeofday", &timeOfDaySettings->MoonPhase, 0.0f, 1.0f, "%.2f", 0.01f, 0.125f);
        QtUi::SetItemTooltip("0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter.\n"
                             "Also places the moon: a full moon rises at sunset and sets at sunrise.");
        SliderFloatWithInput("Moon Intensity (lux)", &timeOfDaySettings->MoonIntensityLux,
            0.0f, 5000.0f, "%.0f lx", 5.0f, 50.0f);
        QtUi::SetItemTooltip("Full-moon illuminance, scaled down by the phase. A game value; the real "
                             "full moon is about 0.25 lx.");
        {
            float moonCol[3] = { timeOfDaySettings->MoonColorR,
                                 timeOfDaySettings->MoonColorG,
                                 timeOfDaySettings->MoonColorB };
            if (QtUi::ColorEdit3("Moon Color", moonCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->MoonColorR = moonCol[0];
                timeOfDaySettings->MoonColorG = moonCol[1];
                timeOfDaySettings->MoonColorB = moonCol[2];
            }
        }
        SliderFloatWithInput("Moon Size##timeofday", &timeOfDaySettings->MoonSize, 0.1f, 5.0f, "%.2f", 0.05f, 0.5f);
        SliderFloatWithInput("Moon Disc Brightness##timeofday", &timeOfDaySettings->MoonDiscBrightness, 0.0f, 50.0f, "%.1f", 0.1f, 1.0f);
        QtUi::SetItemTooltip("How bright the moon disc looks, as a multiple of its light - separate from how much "
                             "it lights the scene. The moon glare follows it.");
        SliderFloatWithInput("Moonlit Sky##timeofday", &timeOfDaySettings->MoonlitSky, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
        QtUi::SetItemTooltip("The sky the moon lights: the glow of the air around it and the blue it gives the "
                             "night, in proportion to its phase. 1 = as bright relative to the moon as the day "
                             "sky is to the sun; that is brighter than the night ambient lights the scene, hence lower.");
        skyGlareControls("Moon Glare", "##moonglare", "moon",
            "The soft glow a bright moon has in a dark sky, from the light its disc sends - so it "
            "follows the phase. Separate from the level's bloom.",
            timeOfDaySettings->MoonGlare, defaultTimeOfDaySettings.MoonGlare);
        QtUi::EndDisabled();

        QtUi::Checkbox("Stars##timeofday", &timeOfDaySettings->StarsEnabled);
        QtUi::BeginDisabled(!timeOfDaySettings->StarsEnabled);
        SliderFloatWithInput("Star Intensity##timeofday", &timeOfDaySettings->StarIntensity, 0.0f, 10.0f, "%.2f", 0.05f, 0.5f);
        {
            const char* starFields[] = { "Procedural", "Star Map", "Both" };
            int starField = std::clamp(timeOfDaySettings->StarField, 0, 2);
            if (QtUi::Combo("Star Field##timeofday", &starField, starFields, 3))
                timeOfDaySettings->StarField = starField;
            QtUi::SetItemTooltip("Procedural: sharp twinkling stars and a noise Milky Way.\n"
                                 "Star Map: the real sky from Textures/Sky/2k_stars_milky_way.dds, soft up close.\n"
                                 "Both: the map for the Milky Way and faint background, procedural stars on top.");
        }
        QtUi::EndDisabled();

        // ---- Exposure ---------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Exposure");

        QtUi::Checkbox("Time of Day Controls Exposure##timeofday", &timeOfDaySettings->ControlExposure);
        QtUi::SetItemTooltip("Replaces the AgX EV100 with one that follows the sun: Day with the sun "
                             "25 degrees up or more, Sunset with it on the horizon, Night from 12 degrees "
                             "below. The AgX exposure trim and grade still apply.");
        QtUi::BeginDisabled(!timeOfDaySettings->ControlExposure);
        SliderFloatWithInput("Day EV100##timeofday", &timeOfDaySettings->DayEv100, -16.0f, 16.0f, "%.2f", 0.05f, 0.5f);
        QtUi::SetItemTooltip("-2 is the Desert level's noon exposure. Higher is darker.");
        SliderFloatWithInput("Sunset EV100##timeofday", &timeOfDaySettings->SunsetEv100, -16.0f, 16.0f, "%.2f", 0.05f, 0.5f);
        SliderFloatWithInput("Night EV100##timeofday", &timeOfDaySettings->NightEv100, -16.0f, 16.0f, "%.2f", 0.05f, 0.5f);

        QtUi::Checkbox("Eye Adaptation##timeofday", &timeOfDaySettings->EyeAdaptation);
        QtUi::SetItemTooltip("Shifts the exposure by how much brighter or darker the view is than an open "
                             "outdoor one at this hour: a torch-lit interior at night stops down, a dark "
                             "interior by day opens up, and outdoors stays on the curve above. Differences "
                             "under a stop are ignored. Speed is the AgX auto-exposure speed.");
        QtUi::BeginDisabled(!timeOfDaySettings->EyeAdaptation);
        SliderFloatWithInput("Adaptation Strength##timeofday", &timeOfDaySettings->AdaptationStrength, 0.0f, 1.0f, "%.2f", 0.05f, 0.25f);
        SliderFloatWithInput("EV100 Min##timeofday", &timeOfDaySettings->AdaptationEv100Min, -16.0f, 16.0f, "%.2f", 0.05f, 0.5f);
        QtUi::SetItemTooltip("Brightest exposure adaptation may reach (lower EV = brighter): how far a dark interior opens up.");
        SliderFloatWithInput("EV100 Max##timeofday", &timeOfDaySettings->AdaptationEv100Max, -16.0f, 16.0f, "%.2f", 0.05f, 0.5f);
        QtUi::SetItemTooltip("Darkest exposure adaptation may reach (higher EV = darker): how far a bright interior stops down.");
        if (timeOfDaySettings->AdaptationEv100Min > (std::min)((std::min)(timeOfDaySettings->DayEv100, timeOfDaySettings->SunsetEv100), timeOfDaySettings->NightEv100)
            || timeOfDaySettings->AdaptationEv100Max < (std::max)((std::max)(timeOfDaySettings->DayEv100, timeOfDaySettings->SunsetEv100), timeOfDaySettings->NightEv100))
        {
            QtUi::TextWrapped("The range does not cover the Day/Sunset/Night EV100s, so the curve itself is clamped outdoors.");
        }
        QtUi::EndDisabled();
        QtUi::EndDisabled();
        QtUi::Text("  Now: EV100 %.2f, GI pre-exposure x%.1f", todState.Ev100, todState.PreExposure);
        QtUi::SetItemTooltip("RTGI and the radiance probes trace at daylight brightness scaled by the "
                             "pre-exposure and divide it back out, so night GI keeps its precision.");

        QtUi::EndDisabled();

        // ---- Wind -------------------------------------------------------
        // Scene-wide, and deliberately not per-component: vegetation bending
        // and the rain simulation both read these values, so there is no way
        // for the two to end up disagreeing about the weather.
        if (windSettings != nullptr)
        {
            QtUi::Spacing();
            QtUi::SeparatorText("Wind");

            QtUi::Checkbox("Enable Wind##wind", &windSettings->Enabled);

            float directionDegrees = windSettings->DirectionRadians * (180.0f / 3.14159265358979323846f);
            if (SliderFloatWithInput("Direction##wind", &directionDegrees, 0.0f, 360.0f, "%.0f deg", 1.0f, 15.0f))
            {
                windSettings->DirectionRadians = directionDegrees * (3.14159265358979323846f / 180.0f);
            }
            if (QtUi::IsItemHovered())
                QtUi::SetTooltip("Heading, counter-clockwise from world +X.");

            SliderFloatWithInput("Speed##wind", &windSettings->Strength, 0.0f, 30.0f, "%.1f m/s", 0.1f, 1.0f);
            if (QtUi::IsItemHovered())
            {
                QtUi::SetTooltip(
                    "2 m/s: leaves barely move\n"
                    "8 m/s: branches in motion\n"
                    "15 m/s: whole trees swaying");
            }

            SliderFloatWithInput("Gust Amplitude##wind", &windSettings->GustAmplitude, 0.0f, 20.0f, "%.1f m/s", 0.1f, 1.0f);
            SliderFloatWithInput("Gust Frequency##wind", &windSettings->GustFrequency, 0.0f, 3.0f, "%.2f Hz", 0.01f, 0.1f);
            SliderFloatWithInput("Gust Wavelength##wind", &windSettings->GustWavelength, 1.0f, 400.0f, "%.0f m", 1.0f, 10.0f);
            if (QtUi::IsItemHovered())
            {
                QtUi::SetTooltip(
                    "Size of one gust cell.  Larger than the visible area reads as\n"
                    "a breeze crossing a meadow; smaller makes the whole field\n"
                    "twitch at once.");
            }

            SliderFloatWithInput("Flutter Frequency##wind", &windSettings->FlutterFrequency, 0.0f, 8.0f, "%.2f Hz", 0.05f, 0.5f);
            SliderFloatWithInput("Vegetation Bend Scale##wind", &windSettings->VegetationBendScale, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
            if (QtUi::IsItemHovered())
                QtUi::SetTooltip("Global multiplier on all vegetation bending.");
        }

        QtUi::End();
    }

    if (gShowDpleWindow && dpleSettings != nullptr)
    {
        DrawDpleWindow(*dpleSettings, dpleErrorMessage);
    }

    if (gShowGraphicsSettingsWindow)
    {
        const TaaSettings defaultTaaSettings{};
        const SMAASettings defaultSmaaSettings{};
        const AnalyticEdgeAASettings defaultAnalyticEdgeAaSettings{};
        const AarSettings defaultAarSettings{};
        const MsaaSettings defaultMsaaSettings{};
        const SharpenSettings defaultSharpenSettings{};
        const DlssSettings defaultDlssSettings{};
        const RtGISettings defaultRtgiSettings{};
        const RadianceCascadesSettings defaultRadianceCascadesSettings{};
        const RtAOSettings defaultRtaoSettings{};
        const AgxTonemapSettings defaultAgxSettings{};
        const VolumetricFogSettings defaultVolumetricFogSettings{};
        const PointShadowSettings defaultPointShadowSettings{};
        const VirtualShadowMapSettings defaultVirtualShadowMapSettings{};
        QtUi::Begin("Graphics Settings", &gShowGraphicsSettingsWindow, QtUiWindowFlags_AlwaysAutoResize);

        if (viewDistanceMeters != nullptr)
        {
            if (QtUi::CollapsingHeader("Rendering", QtUiTreeNodeFlags_DefaultOpen))
            {
                float viewDistanceKilometers = *viewDistanceMeters / 1000.0f;
                if (SliderFloatWithInput(
                    "View Distance",
                    &viewDistanceKilometers,
                    0.5f,
                    100.0f,
                    "%.1f km",
                    0.5f,
                    2.0f))
                {
                    *viewDistanceMeters = viewDistanceKilometers * 1000.0f;
                }
                QtUi::SetItemTooltip("Maximum camera and terrain rendering distance. Higher values may reduce depth precision and performance.");
            }
        }

        if (taaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Temporal Anti-Aliasing (TAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##taa"))
                {
                    *taaSettings = defaultTaaSettings;
                }
                if (QtUi::Checkbox("Enable TAA", &taaSettings->Enabled))
                    taaSettings->ResetHistory = true;
                QtUi::SetItemTooltip("Unreal Engine 5's temporal anti-aliasing (TemporalAA.usf).");

                if (taaSettings->Enabled)
                {
                    const char* taaQualities[] = { "Low", "Medium", "High", "Medium High" };
                    if (QtUi::Combo("Quality##taa", &taaSettings->Quality, taaQualities, static_cast<int>(std::size(taaQualities))))
                        taaSettings->ResetHistory = true;
                    QtUi::SetItemTooltip("r.TemporalAA.Quality. Low skips the reconstruction filter; High adds dynamic\n"
                                         "anti-ghosting (and, when upsampling, a 3x3 filter with distance-based clamping).");

                    // Unreal's default is 0.04: lower accumulates longer and is smoother but ghosts more.
                    SliderFloatWithInput("Current Frame Weight##taa", &taaSettings->CurrentFrameWeight, 0.01f, 0.5f, "%.3f", 0.005f, 0.02f);
                    QtUi::SetItemTooltip("r.TemporalAACurrentFrameWeight. Weight of the new frame once history has converged.");

                    float samples = static_cast<float>(taaSettings->Samples);
                    if (SliderFloatWithInput("Samples##taa", &samples, 2.0f, 64.0f, "%.0f", 1.0f, 4.0f))
                        taaSettings->Samples = static_cast<int>(samples + 0.5f);
                    QtUi::SetItemTooltip("r.TemporalAASamples. Length of the Halton jitter sequence at 100% screen percentage.");

                    SliderFloatWithInput("Filter Size##taa", &taaSettings->FilterSize, 0.5f, 2.0f, "%.2f", 0.05f, 0.1f);
                    QtUi::SetItemTooltip("r.TemporalAAFilterSize. Width of the current-frame reconstruction filter, in pixels.");
                    QtUi::Checkbox("Catmull-Rom Filter##taa", &taaSettings->CatmullRomFilter);
                    QtUi::SetItemTooltip("r.TemporalAACatmullRom. Sharper reconstruction weights instead of the Gaussian.");

                    SliderFloatWithInput("Jitter Scale##taa", &taaSettings->JitterScale, 0.0f, 2.0f, "%.2f");

                    QtUi::Separator();
                    if (QtUi::Checkbox("Temporal Upsampling (TAAU)##taa", &taaSettings->Upscaling))
                        taaSettings->ResetHistory = true;
                    QtUi::SetItemTooltip("r.TemporalAA.Upsampling. Renders at the screen percentage below and lets TAA\n"
                                         "reconstruct the full resolution. DLSS, FSR and XeSS take precedence when they are on.");
                    if (taaSettings->Upscaling)
                    {
                        SliderFloatWithInput("Screen Percentage##taa", &taaSettings->ScreenPercentage, 25.0f, 100.0f, "%.0f%%", 1.0f, 5.0f);
                        QtUi::SetItemTooltip("r.ScreenPercentage. Render resolution per axis, relative to the output.");

                        const bool otherUpscaler = (dlssSettings != nullptr && dlssSettings->Enabled)
                            || (fsrSettings != nullptr && fsrSettings->Enabled)
                            || (xessSettings != nullptr && xessSettings->Enabled);
                        if (otherUpscaler)
                        {
                            QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                            QtUi::TextWrapped("Another upscaler is enabled and takes precedence; TAAU is not running.");
                            QtUi::PopStyleColor();
                        }
                    }
                    taaSettings->Validate();
                }
            }
        }

        if (smaaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Subpixel Morphological Anti-Aliasing (SMAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##smaa"))
                {
                    *smaaSettings = defaultSmaaSettings;
                }
                QtUi::Checkbox("Enable SMAA", &smaaSettings->Enabled);
                if (smaaSettings->Enabled)
                {
                    SliderFloatWithInput("Edge Threshold##smaa", &smaaSettings->EdgeThreshold, 0.01f, 0.30f, "%.2f", 0.01f, 0.05f);
                    SliderFloatWithInput("Search Steps##smaa", &smaaSettings->MaxSearchSteps, 1.0f, 32.0f, "%.0f", 1.0f, 2.0f);
                    SliderFloatWithInput("Diagonal Search Steps##smaa", &smaaSettings->MaxSearchStepsDiag, 0.0f, 16.0f, "%.0f", 1.0f, 2.0f);
                    SliderFloatWithInput("Corner Rounding##smaa", &smaaSettings->CornerRounding, 0.0f, 100.0f, "%.0f", 1.0f, 5.0f);
                    const auto& smaaDebugModes = DebugViewNames::Smaa;
                    QtUi::Combo("Debug View##smaa", &smaaSettings->DebugView, smaaDebugModes, std::size(smaaDebugModes));
                }
            }
        }

        if (analyticEdgeAaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Analytic Edge Anti-Aliasing", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##edgeaa"))
                {
                    *analyticEdgeAaSettings = defaultAnalyticEdgeAaSettings;
                }
                QtUi::Checkbox("Enable Analytic Edge AA", &analyticEdgeAaSettings->Enabled);
                QtUi::SetItemTooltip("Frequency-aware AA for geometric edges. Pixels where the geometry breaks get the exact\n"
                                     "area each triangle covers, clipped from the triangle's own edges, instead of one\n"
                                     "point sample. Everything else is left to TAA / SMAA, which it combines with.");

                if (!AnalyticEdge::IsSupported())
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                    QtUi::TextWrapped("This GPU does not support Shader Model 6.1 barycentrics, so the G-Buffer carries no edges.");
                    QtUi::PopStyleColor();
                }
                else if (analyticEdgeAaSettings->Enabled)
                {
                    SliderFloatWithInput("Depth Threshold##edgeaa", &analyticEdgeAaSettings->DepthThreshold, 0.001f, 0.1f, "%.3f", 0.001f, 0.005f);
                    QtUi::SetItemTooltip("Relative depth step, beyond what the neighbouring plane predicts, that counts as an edge.\n"
                                         "Lower catches shallower silhouettes.");
                    SliderFloatWithInput("Crease Angle##edgeaa", &analyticEdgeAaSettings->CreaseAngleDegrees, 10.0f, 180.0f, "%.0f deg", 1.0f, 5.0f);
                    QtUi::SetItemTooltip("Normals further apart than this meet at a crease edge. 180 disables the normal test.");
                    const auto& edgeAaDebugModes = DebugViewNames::EdgeAa;
                    QtUi::Combo("Debug View##edgeaa", &analyticEdgeAaSettings->DebugView, edgeAaDebugModes, std::size(edgeAaDebugModes));
                    QtUi::SetItemTooltip("Classification: red = own triangle clipped, green = covered by a nearer neighbour's\n"
                                         "triangle, blue = a geometric break with no triangle edge to resolve (alpha cut-outs).\n"
                                         "Coverage: the exact share of each edge pixel its own surface keeps.");
                    analyticEdgeAaSettings->Validate();

                    if (msaaSettings && msaaSettings->Enabled)
                    {
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("MSAA is enabled and already resolves geometric edges; Analytic Edge AA is not running.");
                        QtUi::PopStyleColor();
                    }
                }
            }
        }

        if (aarSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Analytic-Aware Reconstruction (AAR)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##aar"))
                {
                    *aarSettings = defaultAarSettings;
                }
                QtUi::Checkbox("Enable AAR Upscaling##aar", &aarSettings->Enabled);
                QtUi::SetItemTooltip("Spatial upscaler built on the G-Buffer's triangle edges. Output pixels crossed by a\n"
                                     "geometric edge get the exact share each surface covers at output resolution, with each\n"
                                     "surface's colour rebuilt only from its own samples; elsewhere a depth- and normal-aware\n"
                                     "Catmull-Rom. No history or jitter. Replaces TAA, SMAA and Analytic Edge AA while on.");

                if (aarStatus != nullptr)
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                    QtUi::TextWrapped("%s", aarStatus);
                    QtUi::PopStyleColor();
                }

                if (aarSettings->Enabled)
                {
                    const char* aarModes[] = { "Native (100%)", "Ultra Quality (80%)", "Quality (75%)", "Balanced (66.7%)" };
                    QtUi::Combo("Mode##aar", &aarSettings->Mode, aarModes, static_cast<int>(std::size(aarModes)));
                    QtUi::SetItemTooltip("Render resolution per axis, relative to the output.");
                    SliderFloatWithInput("Depth Threshold##aar", &aarSettings->DepthThreshold, 0.001f, 0.1f, "%.3f", 0.001f, 0.005f);
                    QtUi::SetItemTooltip("Relative depth step, beyond what the neighbouring plane predicts, that makes an edge.");
                    SliderFloatWithInput("Crease Angle##aar", &aarSettings->CreaseAngleDegrees, 10.0f, 180.0f, "%.0f deg", 1.0f, 5.0f);
                    QtUi::SetItemTooltip("Normals further apart than this meet at a crease edge. 180 disables the normal test.");
                    SliderFloatWithInput("Depth Sigma##aar", &aarSettings->DepthSigma, 0.001f, 0.2f, "%.3f", 0.001f, 0.005f);
                    QtUi::SetItemTooltip("How far a sample may sit off the target surface's plane and still count towards it.");
                    SliderFloatWithInput("Normal Power##aar", &aarSettings->NormalPower, 0.0f, 16.0f, "%.1f", 0.5f, 1.0f);
                    QtUi::SetItemTooltip("How strictly samples must face the same way as the target surface. 0 ignores normals.");
                    const auto& aarDebugModes = DebugViewNames::Aar;
                    QtUi::Combo("Debug View##aar", &aarSettings->DebugView, aarDebugModes, static_cast<int>(std::size(aarDebugModes)));
                    QtUi::SetItemTooltip("Reconstruction Path: red = analytic edge reconstruction, green = interior Catmull-Rom,\n"
                                         "blue = interior on a nearer surface found through its triangles.\n"
                                         "Coverage: surface A's exact share of each edge pixel at output resolution.");
                    aarSettings->Validate();
                }
            }
        }
        if (msaaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Multi-Sample Anti-Aliasing (MSAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##msaa"))
                {
                    *msaaSettings = defaultMsaaSettings;
                }
                
                QtUi::Checkbox("Enable MSAA##msaa", &msaaSettings->Enabled);
                QtUi::SetItemTooltip("Hardware MSAA for geometry edges. Requires scene restart to take effect.\nNote: MSAA significantly increases memory usage (2-8x G-Buffer size).");
                
                if (msaaSettings->Enabled)
                {
                    // Sample count selector
                    const char* sampleCounts[] = { "2x MSAA", "4x MSAA", "8x MSAA" };
                    int sampleIndex = 1; // Default to 4x
                    if (msaaSettings->SampleCount == 2) sampleIndex = 0;
                    else if (msaaSettings->SampleCount == 4) sampleIndex = 1;
                    else if (msaaSettings->SampleCount == 8) sampleIndex = 2;
                    
                    if (QtUi::Combo("Sample Count##msaa", &sampleIndex, sampleCounts, std::size(sampleCounts)))
                    {
                        if (sampleIndex == 0) msaaSettings->SampleCount = 2;
                        else if (sampleIndex == 1) msaaSettings->SampleCount = 4;
                        else if (sampleIndex == 2) msaaSettings->SampleCount = 8;
                    }
                    QtUi::SetItemTooltip("Higher sample counts provide better quality but use more memory and bandwidth.");
                    
                    // Display memory impact
                    const float memoryMultiplier = msaaSettings->GetMemoryMultiplier();
                    QtUi::Text("Memory Impact: %.1fx G-Buffer size", memoryMultiplier);
                    
                    // Display resolve timing if available
                    if (msaaResolveTimeMs > 0.0f)
                    {
                        QtUi::Text("Resolve Time: %.2f ms", msaaResolveTimeMs);
                    }
                    
                    QtUi::Separator();
                    QtUi::TextWrapped("MSAA provides excellent geometric edge anti-aliasing. "
                                      "It works best for static scenes and complements TAA for temporal stability.");
                    
                    // Warning about combining with other AA
                    if (taaSettings && taaSettings->Enabled)
                    {
                        QtUi::Spacing();
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("Note: MSAA + TAA can be combined for best quality, but may impact performance.");
                        QtUi::PopStyleColor();
                    }
                    if (smaaSettings && smaaSettings->Enabled)
                    {
                        QtUi::Spacing();
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("Note: MSAA + SMAA is redundant. Consider disabling one for better performance.");
                        QtUi::PopStyleColor();
                    }
                }
            }
        }

        if (sharpenSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Sharpening", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##sharpen"))
                {
                    *sharpenSettings = defaultSharpenSettings;
                }

                QtUi::Checkbox("Image Sharpening##sharpen", &sharpenSettings->ImageSharpeningEnabled);
                if (sharpenSettings->ImageSharpeningEnabled)
                {
                    SliderFloatWithInput("Image Strength##sharpen", &sharpenSettings->ImageSharpeningStrength, 0.0f, 3.0f, "%.2f", 0.01f, 0.05f);
                    SliderFloatWithInput("Image Radius##sharpen", &sharpenSettings->ImageSharpeningRadius, 1.0f, 8.0f, "%.1f px", 0.1f, 0.5f);
                    QtUi::SetItemTooltip("Size of the unsharp mask. 1 sharpens only pixel-level edges;\n"
                                         "larger radii also bring out broader detail.");
                }

                QtUi::Checkbox("Texture Sharpening##sharpen", &sharpenSettings->TextureSharpeningEnabled);
                if (sharpenSettings->TextureSharpeningEnabled)
                {
                    SliderFloatWithInput("Texture Mip Bias##sharpen", &sharpenSettings->TextureMipLODBias, -3.0f, 0.0f, "%.2f", 0.05f, 0.25f);
                }

                sharpenSettings->Validate();
            }
        }

        if (dlssSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("DLSS Super Resolution", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##dlss"))
                {
                    *dlssSettings = defaultDlssSettings;
                }

                // One upscaler at a time: turning DLSS on here turns FSR and XeSS upscaling off.
                if (QtUi::Checkbox("Enable DLSS Super Resolution", &dlssSettings->Enabled)
                    && dlssSettings->Enabled)
                {
                    if (fsrSettings != nullptr)
                        fsrSettings->Enabled = false;
                    if (xessSettings != nullptr)
                        xessSettings->Enabled = false;
                }
                if (dlssSettings->Enabled)
                {
                    const char* modes[] =
                    {
                        "Off",
                        "Max Performance",
                        "Balanced",
                        "Max Quality",
                        "Ultra Performance",
                        "Ultra Quality",
                        "DLAA"
                    };
                    QtUi::Combo("Mode##dlss", &dlssSettings->Mode, modes, std::size(modes));
                }
            }
        }

        if (fsrSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("AMD FSR (Upscaling & Frame Generation)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##fsr"))
                {
                    *fsrSettings = FsrSettings{};
                    fsrSettings->ResetHistory = true;
                }

                const bool apiAvailable = fsrStatus == nullptr || fsrStatus->ApiAvailable;
                if (!apiAvailable)
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("AMD FSR is unavailable: %s",
                        (fsrStatus != nullptr && !fsrStatus->LastError.empty())
                            ? fsrStatus->LastError.c_str()
                            : "the FidelityFX DLLs were not found.");
                    QtUi::PopStyleColor();
                }

                QtUi::BeginDisabled(!apiAvailable);

                // ---- Upscaling ----
                QtUi::TextDisabled("Upscaling");
                if (QtUi::Checkbox("Enable FSR Upscaling##fsr", &fsrSettings->Enabled))
                {
                    fsrSettings->ResetHistory = true;
                    if (fsrSettings->Enabled && dlssSettings != nullptr)
                        dlssSettings->Enabled = false;
                    if (fsrSettings->Enabled && xessSettings != nullptr)
                        xessSettings->Enabled = false;
                }
                QtUi::SetItemTooltip("Renders at a lower resolution and reconstructs the full-resolution image.\n"
                                     "Replaces TAA and SMAA while it is on. Uses FSR 4 on RDNA 4 GPUs, FSR 3.1 elsewhere.");

                if (fsrSettings->Enabled)
                {
                    const char* fsrModes[] =
                    {
                        "Native AA (1.0x)",
                        "Quality (1.5x)",
                        "Balanced (1.7x)",
                        "Performance (2.0x)",
                        "Ultra Performance (3.0x)"
                    };
                    if (QtUi::Combo("Quality Mode##fsr", &fsrSettings->Mode, fsrModes, static_cast<int>(std::size(fsrModes))))
                        fsrSettings->ResetHistory = true;
                    QtUi::SetItemTooltip("Per-axis ratio between the output and the resolution the scene is rendered at.");

                    QtUi::Checkbox("Sharpening (RCAS)##fsr", &fsrSettings->Sharpening);
                    if (fsrSettings->Sharpening)
                        SliderFloatWithInput("Sharpness##fsr", &fsrSettings->Sharpness, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);

                    if (fsrStatus != nullptr)
                    {
                        if (fsrStatus->OverriddenByDlss)
                        {
                            QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                            QtUi::TextWrapped("DLSS is enabled and takes precedence; FSR upscaling is not running.");
                            QtUi::PopStyleColor();
                        }
                        else if (fsrStatus->UpscalerActive)
                        {
                            QtUi::Text("Render %u x %u  ->  Output %u x %u",
                                fsrStatus->RenderWidth, fsrStatus->RenderHeight,
                                fsrStatus->OutputWidth, fsrStatus->OutputHeight);
                            if (!fsrStatus->UpscalerVersion.empty())
                                QtUi::TextDisabled("Provider: %s", fsrStatus->UpscalerVersion.c_str());
                        }
                    }
                }

                // ---- Frame generation ----
                QtUi::Separator();
                QtUi::TextDisabled("Frame Generation");
                if (QtUi::Checkbox("Enable Frame Generation##fsr", &fsrSettings->FrameGeneration)
                    && fsrSettings->FrameGeneration && xessSettings != nullptr)
                {
                    // One frame generator owns the swap chain at a time.
                    xessSettings->FrameGeneration = false;
                }
                QtUi::SetItemTooltip("Presents an interpolated frame between every two rendered frames.\n"
                                     "Works with or without upscaling. Best above ~60 rendered fps; adds some latency.\n"
                                     "Switching it replaces the viewport's swap chain, which takes a moment.");

                if (fsrSettings->FrameGeneration)
                {
                    if (fsrStatus != nullptr && fsrStatus->FrameGenerationActive)
                    {
                        if (!fsrStatus->FrameGenerationVersion.empty())
                            QtUi::TextDisabled("Provider: %s", fsrStatus->FrameGenerationVersion.c_str());
                    }
                    else
                    {
                        QtUi::TextDisabled("Starting...");
                    }

                    QtUi::Checkbox("Debug: Tear Lines##fsr", &fsrSettings->FrameGenerationDebugTearLines);
                    QtUi::SetItemTooltip("Draws a bar that moves every presented frame, to confirm generated frames are shown.");
                    QtUi::Checkbox("Debug: Reset Indicators##fsr", &fsrSettings->FrameGenerationDebugResetIndicators);
                    QtUi::Checkbox("Debug: View##fsr", &fsrSettings->FrameGenerationDebugView);
                }

                QtUi::EndDisabled();

                if (apiAvailable && fsrStatus != nullptr && !fsrStatus->LastError.empty())
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("%s", fsrStatus->LastError.c_str());
                    QtUi::PopStyleColor();
                }
            }
        }

        if (xessSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Intel XeSS (Super Resolution & Frame Generation)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##xess"))
                {
                    *xessSettings = XessSettings{};
                    xessSettings->ResetHistory = true;
                }

                const bool srAvailable = xessStatus == nullptr || xessStatus->SuperResolutionAvailable;
                const bool fgAvailable = xessStatus == nullptr || xessStatus->FrameGenerationAvailable;

                // ---- Super resolution ----
                QtUi::TextDisabled("Super Resolution");
                if (!srAvailable)
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("XeSS Super Resolution is unavailable: %s",
                        (xessStatus != nullptr && !xessStatus->LastError.empty())
                            ? xessStatus->LastError.c_str()
                            : "libxess.dll was not found or the GPU lacks Shader Model 6.4.");
                    QtUi::PopStyleColor();
                }
                QtUi::BeginDisabled(!srAvailable);
                if (QtUi::Checkbox("Enable XeSS Super Resolution##xess", &xessSettings->Enabled))
                {
                    xessSettings->ResetHistory = true;
                    if (xessSettings->Enabled)
                    {
                        if (dlssSettings != nullptr)
                            dlssSettings->Enabled = false;
                        if (fsrSettings != nullptr)
                            fsrSettings->Enabled = false;
                    }
                }
                QtUi::SetItemTooltip("Renders at a lower resolution and reconstructs the full-resolution image with XeSS.\n"
                                     "Replaces TAA and SMAA while it is on. Uses XMX on Intel Arc GPUs, DP4a elsewhere.");

                if (xessSettings->Enabled)
                {
                    const char* xessModes[] =
                    {
                        "Native AA (1.0x)",
                        "Ultra Quality Plus (1.3x)",
                        "Ultra Quality (1.5x)",
                        "Quality (1.7x)",
                        "Balanced (2.0x)",
                        "Performance (2.3x)",
                        "Ultra Performance (3.0x)"
                    };
                    if (QtUi::Combo("Quality Mode##xess", &xessSettings->Mode, xessModes, static_cast<int>(std::size(xessModes))))
                        xessSettings->ResetHistory = true;
                    QtUi::SetItemTooltip("Per-axis ratio between the output and the resolution the scene is rendered at.");

                    if (xessStatus != nullptr)
                    {
                        if (xessStatus->OverriddenByOtherUpscaler)
                        {
                            QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                            QtUi::TextWrapped("DLSS or FSR is enabled and takes precedence; XeSS upscaling is not running.");
                            QtUi::PopStyleColor();
                        }
                        else if (xessStatus->UpscalerActive)
                        {
                            QtUi::Text("Render %u x %u  ->  Output %u x %u",
                                xessStatus->RenderWidth, xessStatus->RenderHeight,
                                xessStatus->OutputWidth, xessStatus->OutputHeight);
                            if (!xessStatus->SuperResolutionVersion.empty())
                                QtUi::TextDisabled("Provider: %s", xessStatus->SuperResolutionVersion.c_str());
                        }
                    }
                }
                QtUi::EndDisabled();

                // ---- Frame generation ----
                QtUi::Separator();
                QtUi::TextDisabled("Frame Generation");
                if (!fgAvailable)
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("XeSS Frame Generation is unavailable: libxess_fg.dll or libxell.dll was not found.");
                    QtUi::PopStyleColor();
                }
                QtUi::BeginDisabled(!fgAvailable);
                if (QtUi::Checkbox("Enable Frame Generation##xess", &xessSettings->FrameGeneration)
                    && xessSettings->FrameGeneration && fsrSettings != nullptr)
                {
                    // One frame generator owns the swap chain at a time.
                    fsrSettings->FrameGeneration = false;
                }
                QtUi::SetItemTooltip("Presents generated frames between rendered ones (XeSS-FG). Works with or without\n"
                                     "upscaling and switches Xe Low Latency on. Switching it replaces the viewport's\n"
                                     "swap chain, which takes a moment.");

                if (xessSettings->FrameGeneration)
                {
                    const unsigned maxFrames = (xessStatus != nullptr && xessStatus->MaxInterpolatedFrames > 0)
                        ? xessStatus->MaxInterpolatedFrames : 1u;
                    if (maxFrames > 1)
                    {
                        float frames = static_cast<float>(xessSettings->InterpolatedFrames);
                        if (SliderFloatWithInput("Generated Frames##xess", &frames, 1.0f, static_cast<float>(maxFrames), "%.0f", 1.0f, 1.0f))
                            xessSettings->InterpolatedFrames = static_cast<int>(frames + 0.5f);
                        QtUi::SetItemTooltip("Frames generated per rendered frame. More than one needs an Intel Arc GPU.");
                    }
                    QtUi::Checkbox("Compose UI Over Generated Frames##xess", &xessSettings->UiComposition);
                    QtUi::SetItemTooltip("Composite the game UI over generated frames instead of interpolating it.\n"
                                         "Try it if UI elements smear or wobble.");

                    if (xessStatus != nullptr && xessStatus->FrameGenerationOverriddenByFsr)
                    {
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("FSR frame generation is enabled and takes precedence.");
                        QtUi::PopStyleColor();
                    }
                    else if (xessStatus != nullptr && xessStatus->FrameGenerationActive)
                    {
                        if (!xessStatus->FrameGenerationVersion.empty())
                            QtUi::TextDisabled("Provider: %s", xessStatus->FrameGenerationVersion.c_str());
                    }
                    else
                    {
                        QtUi::TextDisabled("Starting...");
                    }
                }

                // ---- Xe Low Latency ----
                QtUi::Separator();
                QtUi::TextDisabled("Xe Low Latency");
                QtUi::BeginDisabled(xessSettings->FrameGeneration);
                bool lowLatency = xessSettings->LowLatency || xessSettings->FrameGeneration;
                if (QtUi::Checkbox("Enable Xe Low Latency##xess", &lowLatency))
                    xessSettings->LowLatency = lowLatency;
                QtUi::EndDisabled();
                QtUi::SetItemTooltip("Paces the CPU so frames do not queue up in front of the GPU. Always on with XeSS\n"
                                     "frame generation. On non-Intel GPUs it only takes effect together with it.");
                if (xessStatus != nullptr)
                    QtUi::TextDisabled("Latency reduction: %s", xessStatus->LowLatencyActive ? "active" : "inactive");
                QtUi::EndDisabled();

                if (xessStatus != nullptr && !xessStatus->LastError.empty() && (srAvailable || fgAvailable))
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("%s", xessStatus->LastError.c_str());
                    QtUi::PopStyleColor();
                }
            }
        }

        if (globalIlluminationMode != nullptr)
        {
            const char* giModes[] = { "Disabled", "RTGI", "Radiance Cascades" };
            int giMode = static_cast<int>(*globalIlluminationMode);
            if (QtUi::Combo("Global Illumination Solution", &giMode, giModes, std::size(giModes)))
                *globalIlluminationMode = static_cast<GlobalIlluminationMode>(giMode);
        }

        // -------------------------------------------------------------------
        // RTGI – ReSTIR Global Illumination
        // -------------------------------------------------------------------
        if (rtgiSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("RTGI", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##rtgi"))
                {
                    *rtgiSettings = defaultRtgiSettings;
                }
                QtUi::Checkbox("Enable RTGI##rtgi", &rtgiSettings->Enabled);
                QtUi::SetItemTooltip("Enable custom ReSTIR GI ray-traced global illumination.");

                // Show any runtime error (e.g. shader compile failure) in red.
                if (sceneStatusMessage && sceneStatusMessage[0] != '\0')
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("Error: %s", sceneStatusMessage);
                    QtUi::PopStyleColor();
                }

                if (rtgiSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Denoiser");

                    QtUi::Checkbox("Use NVIDIA NRD##rtgi", &rtgiSettings->UseNrdDenoiser);
                    QtUi::SetItemTooltip("Use NVIDIA NRD RELAX diffuse denoising for the RTGI result.");

                    // --- Ray generation ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Ray Generation");

                    SliderIntWithInput("Rays Per Pixel##rtgi", &rtgiSettings->RaysPerPixel, 1, 8);
                    QtUi::SetItemTooltip("Number of GI rays fired per pixel each frame. Higher = better quality, lower performance.");
                    {
                        // Stored as the divisor of the frame's resolution.
                        const char* diffuseResolutions[] = { "Full", "Half", "Quarter" };
                        int diffuseIndex = rtgiSettings->DiffuseResolution >= 4 ? 2 : rtgiSettings->DiffuseResolution >= 2 ? 1 : 0;
                        if (QtUi::Combo("Diffuse Resolution##rtgi", &diffuseIndex, diffuseResolutions, std::size(diffuseResolutions)))
                            rtgiSettings->DiffuseResolution = 1 << diffuseIndex;
                        QtUi::SetItemTooltip(
                            "Resolution the diffuse GI is traced and denoised at. Half traces a quarter of the rays,\n"
                            "Quarter a sixteenth; a depth- and normal-aware upsample brings it back to full resolution.");
                        const char* specularResolutions[] = { "Full", "Half" };
                        int specularIndex = rtgiSettings->SpecularResolution >= 2 ? 1 : 0;
                        if (QtUi::Combo("Reflection Resolution##rtgi", &specularIndex, specularResolutions, std::size(specularResolutions)))
                            rtgiSettings->SpecularResolution = 1 << specularIndex;
                        QtUi::SetItemTooltip(
                            "Resolution the specular reflection rays are traced at. Half traces a quarter of the rays.\n"
                            "Also applies to the reflections that run alongside probe GI.");
                    }
                    SliderIntWithInput("Max Bounces##rtgi", &rtgiSettings->MaxBounces, 1, 4);
                    QtUi::SetItemTooltip("Maximum indirect bounce depth for GI rays.");
                    QtUi::Checkbox("Next Event Estimation##rtgi", &rtgiSettings->NextEventEstimation);
                    QtUi::SetItemTooltip("Trace a shadow ray from the secondary hit toward the sun to explicitly estimate direct lighting in the GI path.");
                    if (rtgiSettings->NextEventEstimation)
                    {
                        QtUi::Checkbox("Shadow Map Visibility##rtgi", &rtgiSettings->VsmVisibility);
                        QtUi::SetItemTooltip(
                            "Answer next-event visibility from the virtual shadow map where it holds a fine enough page,\n"
                            "and trace a shadow ray only where it does not. Cheaper per light, and matches the raster\n"
                            "shadows (alpha-tested foliage included). Needs the virtual shadow map to be on.");
                        if (rtgiSettings->VsmVisibility)
                        {
                            SliderFloatWithInput("Max Shadow Texel##rtgi", &rtgiSettings->VsmMaxTexelSize, 0.005f, 0.25f, "%.3f m", 0.001f, 0.01f);
                            QtUi::SetItemTooltip(
                                "Coarsest shadow-map texel trusted at a bounce hit. Coarse pages leak light through walls\n"
                                "about as thick as a texel, so hits only coarse pages cover trace a ray instead.\n"
                                "Lower = fewer leaks, more rays.");
                        }
                        QtUi::Checkbox("Stochastic Light Sampling##rtgi", &rtgiSettings->StochasticLights);
                        QtUi::SetItemTooltip(
                            "Test the shadows of only a few lights at each bounce hit, picked by how much each would add,\n"
                            "instead of every light in reach. GI cost stops growing with the number of lights.\n"
                            "Unbiased, but adds a little noise where several similar lights overlap. Off = test every light.");
                        if (rtgiSettings->StochasticLights)
                        {
                            SliderIntWithInput("Light Samples##rtgi", &rtgiSettings->LightSamples, 1, 4);
                            QtUi::SetItemTooltip("Lights whose shadow each bounce hit tests. More = less noise, more cost.");
                        }
                    }

                    // --- Acceleration structure ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Acceleration Structure");

                    QtUi::Checkbox("Virtual Geometry BLAS##rtgi", &rtgiSettings->VirtualGeometryBlas);
                    QtUi::SetItemTooltip(
                        "Ray trace virtualized meshes against a simplified cut through their cluster hierarchy\n"
                        "instead of every source triangle. Smaller BLASes, faster builds, less VRAM.");
                    if (rtgiSettings->VirtualGeometryBlas)
                    {
                        SliderFloatWithInput("Near Error##rtgi_blas", &rtgiSettings->BlasLodError, 0.0f, 0.02f, "%.4f m", 0.0005f, 0.002f);
                        QtUi::SetItemTooltip(
                            "Simplification error of the nearest tier. Keep it under 1 cm: GI rays re-find their\n"
                            "origin on the traced surface within that tolerance near the camera.");
                        SliderFloatWithInput("Tier Distance##rtgi_blas", &rtgiSettings->BlasTierDistance, 1.0f, 200.0f, "%.1f m");
                        QtUi::SetItemTooltip("Where the second tier starts. Each tier starts 4x further out and allows 4x the error.");
                        SliderIntWithInput("Tiers##rtgi_blas", &rtgiSettings->BlasTierCount, 1, 3);
                        QtUi::SetItemTooltip("Detail tiers by distance from the camera. 1 = one cut at every distance.");
                    }

                    // --- Firefly suppression ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Firefly Suppression");

                    SliderFloatWithInput("Radiance Clamp##rtgi", &rtgiSettings->RadianceClamp, 0.0f, 50.0f, "%.1f");
                    QtUi::SetItemTooltip("Clamp incoming radiance to this value to suppress fireflies. Set to 0 to disable.");

                    // --- Temporal accumulation denoiser ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Temporal Accumulation");

                    SliderFloatWithInput("Accumulation Blend##rtgi", &rtgiSettings->AccumulationBlend, 0.01f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend factor between current frame (1.0) and accumulated history (0.0). Lower = smoother but ghosts on motion.");

                    if (rtgiSettings->UseNrdDenoiser)
                    {
                        QtUi::Separator();
                        QtUi::TextDisabled("NRD RELAX");

                        SliderFloatWithInput("Max Accumulation Time##rtgi_nrd", &rtgiSettings->NrdMaxAccumulationTime, 0.01f, 5.0f, "%.2f s");
                        QtUi::SetItemTooltip("How long NRD is allowed to accumulate diffuse history, at full resolution. Higher values can reduce\n"
                                             "motion artifacts but respond more slowly to lighting changes. At a reduced Diffuse Resolution\n"
                                             "it is multiplied by the divisor (x2 at Half, x4 at Quarter), which is what keeps the coarser\n"
                                             "grid's noise from sparkling.");
                        if (rtgiSettings->DiffuseResolution > 1)
                            QtUi::TextDisabled("Effective at this resolution: %.2f s",
                                rtgiSettings->NrdMaxAccumulationTime * static_cast<float>(rtgiSettings->DiffuseResolution >= 4 ? 4 : 2));
                        SliderFloatWithInput("Disocclusion Threshold##rtgi_nrd", &rtgiSettings->NrdDisocclusionThreshold, 0.001f, 0.5f, "%.3f", 0.001f, 0.01f);
                        QtUi::SetItemTooltip("Depth-based history rejection threshold used by NRD. Higher values can stabilize camera motion but may preserve more history across edges.");
                        SliderIntWithInput("A-Trous Iterations##rtgi_nrd", &rtgiSettings->NrdAtrousIterations, 2, 8);
                        QtUi::SetItemTooltip("Number of RELAX wavelet filter iterations.");
                        SliderFloatWithInput("Sharpen##rtgi_nrd", &rtgiSettings->NrdSharpenAmount, 0.0f, 1.5f, "%.2f");
                        QtUi::SetItemTooltip("Applies a lightweight post-denoise sharpen to recover lost detail. Higher values can help with softness but may re-introduce noise halos.");
                    }

                    // --- Output ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("GI Intensity##rtgi", &rtgiSettings->GiIntensity, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the RTGI output before compositing. 1.0 = physically correct.");
                    SliderFloatWithInput("Color Leak Intensity##rtgi", &rtgiSettings->ColorLeakIntensity, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Boosts bounced material tinting for stronger color bleed. 1.0 = physically based transport.");

                    // --- Specular Reflections ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Specular Reflections");

                    QtUi::Checkbox("Enable Specular##rtgi", &rtgiSettings->SpecularEnabled);
                    QtUi::SetItemTooltip("Ray-traced GGX specular reflections composited on top of the deferred lighting pass.");
                    if (rtgiSettings->SpecularEnabled)
                    {
                        SliderFloatWithInput("Roughness Threshold##rtgi", &rtgiSettings->SpecularRoughnessThreshold, 0.0f, 1.0f, "%.2f");
                        QtUi::SetItemTooltip("Surfaces with perceptual roughness above this value skip the specular ray.");
                        SliderFloatWithInput("Specular Intensity##rtgi", &rtgiSettings->SpecularIntensity, 0.0f, 16.0f, "%.2f");
                        QtUi::SetItemTooltip("Intensity multiplier for ray-traced specular reflections.");
                    }

                    // --- Debug ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    // The last four replace the GI signal with one of its own
                    // inputs, so an instability can be attributed to a stage
                    // rather than guessed at. Their ids are not contiguous with
                    // the display modes, hence the explicit value map.
                    const auto& debugModes = DebugViewNames::Rtgi;
                    const auto& debugModeValues = DebugViewNames::RtgiValues;

                    int debugModeIndex = 0;
                    for (int i = 0; i < static_cast<int>(std::size(debugModeValues)); ++i)
                    {
                        if (debugModeValues[i] == rtgiSettings->DebugView)
                        {
                            debugModeIndex = i;
                            break;
                        }
                    }

                    if (QtUi::Combo("Debug View##rtgi", &debugModeIndex, debugModes, static_cast<int>(std::size(debugModes))))
                    {
                        rtgiSettings->DebugView = debugModeValues[debugModeIndex];
                    }
                    QtUi::SetItemTooltip(
                        "Overlay mode for inspecting intermediate RTGI data.\n\n"
                        "The Diag modes replace the GI signal with one of its inputs, to\n"
                        "localise an instability. Read them with the denoiser OFF, or it\n"
                        "will filter out the very thing being looked for.\n\n"
                        "World Position / Normal / Depth flickering means the G-Buffer or\n"
                        "the camera matrices disagree between frames. Flat White flickering\n"
                        "means the fault is not in ray generation at all.\n\n"
                        "Deterministic Probe fires one ray along the surface normal from a\n"
                        "fixed seed, so its result depends only on where the surface is -\n"
                        "never on the camera or the frame. It will look flat and wrong as an\n"
                        "image; that is fine. What matters is whether it is STABLE. Any\n"
                        "flicker there is real instability in traversal or shading, not the\n"
                        "sampling variance that normal GI legitimately has under motion.\n\n"
                        "NEE Sun Visibility compares the NEE sun shadow ray with the sun\n"
                        "shadow map at each visible surface: green both lit, RED shadow\n"
                        "map lit but the ray is blocked (a ray-tracing-only blocker), blue\n"
                        "ray reaches the sun but the shadow map is dark, grey both\n"
                        "shadowed, black faces away from the sun.\n\n"
                        "NEE Shadow Map Coverage fires one ray along the normal and shows\n"
                        "who answers sun visibility at its hit: green / red the shadow map\n"
                        "(lit / shadowed), BLUE no page fine enough, so a ray is traced.");

                    // --- Ray visualisation ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Ray Visualisation");

                    QtUi::Checkbox("Show GI Rays##rtgi_rays", &rtgiSettings->RayDebugEnabled);
                    QtUi::SetItemTooltip(
                        "Draw the GI paths of a sparse set of pixels over the image: every bounce,\n"
                        "the sun shadow ray from each hit, and optionally the camera ray that found\n"
                        "the surface. Rays that escape to the sky and shadow rays fade out.\n\n"
                        "Freeze, then move the camera to see the captured rays from the side.");
                    if (rtgiSettings->RayDebugEnabled)
                    {
                        QtUi::Checkbox("Freeze##rtgi_rays", &rtgiSettings->RayDebugFreeze);
                        QtUi::SetItemTooltip("Stop capturing and keep drawing the current rays wherever the camera goes.");

                        const char* patterns[] = { "Pixel Grid", "Centre Pixel" };
                        QtUi::Combo("Pattern##rtgi_rays", &rtgiSettings->RayDebugPattern, patterns,
                            static_cast<int>(std::size(patterns)));
                        QtUi::SetItemTooltip(
                            "Pixel Grid traces from pixels spread over the screen.\n"
                            "Centre Pixel traces from the pixel under the crosshair only; with\n"
                            "History raised, its hemisphere of rays fills in over time.");
                        if (rtgiSettings->RayDebugPattern == 0)
                        {
                            SliderIntWithInput("Grid Spacing##rtgi_rays", &rtgiSettings->RayDebugSpacing, 8, 512);
                            QtUi::SetItemTooltip("Pixels between traced pixels. Widened automatically if the grid is too dense to capture.");
                        }
                        SliderIntWithInput("History Frames##rtgi_rays", &rtgiSettings->RayDebugHistoryFrames, 1, 256);
                        QtUi::SetItemTooltip("Frames of rays kept on screen; each frame draws new random directions.");

                        const char* colorModes[] = { "By Bounce", "Carried Radiance" };
                        QtUi::Combo("Colour##rtgi_rays", &rtgiSettings->RayDebugColorMode, colorModes,
                            static_cast<int>(std::size(colorModes)));
                        QtUi::SetItemTooltip(
                            "By Bounce: first bounce cyan, second amber, third magenta, fourth green.\n"
                            "Carried Radiance: each ray shows the light travelling back along it.\n"
                            "Shadow rays are yellow where they reach the sun and red where blocked.");

                        QtUi::Checkbox("Shadow Rays##rtgi_rays", &rtgiSettings->RayDebugShowShadowRays);
                        QtUi::SetItemTooltip("Show the next-event sun shadow ray from each bounce hit (needs Next Event Estimation).");
                        QtUi::Checkbox("Camera Rays##rtgi_rays", &rtgiSettings->RayDebugShowCameraRays);
                        QtUi::SetItemTooltip("Show the ray from the camera to each traced pixel's surface.");

                        SliderFloatWithInput("Brightness##rtgi_rays", &rtgiSettings->RayDebugBrightness, 0.0f, 20.0f, "%.2f");
                        QtUi::SetItemTooltip("1 is about as bright as a sunlit white surface; higher values bloom.");
                        SliderFloatWithInput("Line Width##rtgi_rays", &rtgiSettings->RayDebugLineWidth, 0.5f, 8.0f, "%.1f px");
                        SliderFloatWithInput("Sky Ray Length##rtgi_rays", &rtgiSettings->RayDebugMissLength, 0.1f, 50.0f, "%.1f m");
                        QtUi::SetItemTooltip("How far rays that escape to the sky are drawn before they fade out.");
                        SliderFloatWithInput("Shadow Ray Length##rtgi_rays", &rtgiSettings->RayDebugShadowLength, 0.1f, 20.0f, "%.1f m");
                        QtUi::SetItemTooltip("How far sun shadow rays are drawn before they fade out.");
                    }
                }
            }
        }

        if (radianceCascadesSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Radiance Cascades GI", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##radiancecascades"))
                    *radianceCascadesSettings = defaultRadianceCascadesSettings;

                QtUi::Checkbox("Enable Radiance Cascades##radiancecascades", &radianceCascadesSettings->Enabled);
                QtUi::SetItemTooltip("Enable radiance cascades as an alternate GI solution.");

                if (radianceCascadesSettings->Enabled)
                {
                    int cascadeCount = static_cast<int>(radianceCascadesSettings->CascadeCount);
                    int probeSpacingBase = static_cast<int>(radianceCascadesSettings->ProbeSpacingBase);
                    int raysPerProbe = static_cast<int>(radianceCascadesSettings->RaysPerProbe);
                    int sparseProbeTableCapacity = static_cast<int>(radianceCascadesSettings->SparseProbeTableCapacity);
                    int sparseProbeCellSize = static_cast<int>(radianceCascadesSettings->SparseProbeCellSize);
                    int sparseProbeSearchSteps = static_cast<int>(radianceCascadesSettings->SparseProbeSearchSteps);

                    QtUi::Separator();
                    QtUi::TextDisabled("Radiance Field");
                    if (SliderIntWithInput("Cascade Count##radiancecascades", &cascadeCount, 1, 8))
                        radianceCascadesSettings->CascadeCount = static_cast<unsigned int>(cascadeCount);
                    if (SliderIntWithInput("Probe Spacing Base##radiancecascades", &probeSpacingBase, 1, 64))
                        radianceCascadesSettings->ProbeSpacingBase = static_cast<unsigned int>(probeSpacingBase);
                    SliderFloatWithInput("Ray Length Base##radiancecascades", &radianceCascadesSettings->RayLengthBase, 0.1f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Base world-space query distance for the nearest cascade.");
                    SliderFloatWithInput("Ray Length Scale##radiancecascades", &radianceCascadesSettings->RayLengthScale, 1.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Distance multiplier between cascades.");
                    SliderFloatWithInput("Interval Scale##radiancecascades", &radianceCascadesSettings->IntervalLengthScale, 1.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Additional interval growth per cascade for wider far-field GI coverage.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Hardware RT Queries");
                    if (SliderIntWithInput("Rays Per Probe##radiancecascades", &raysPerProbe, 1, 16))
                        radianceCascadesSettings->RaysPerProbe = static_cast<unsigned int>(raysPerProbe);
                    QtUi::SetItemTooltip("Number of inline ray queries fired by each active sparse radiance probe.");
                    SliderFloatWithInput("Ray Bias##radiancecascades", &radianceCascadesSettings->RayBias, 0.001f, 0.20f, "%.3f", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("World-space ray origin offset used for GI rays and shadow visibility rays.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Sparse Structure");
                    if (SliderIntWithInput("Sparse Probe Table Capacity##radiancecascades", &sparseProbeTableCapacity, 1024, 262144))
                        radianceCascadesSettings->SparseProbeTableCapacity = static_cast<unsigned int>(sparseProbeTableCapacity);
                    QtUi::SetItemTooltip("Maximum hash-table entries for visible sparse probes. Higher values reduce collisions without allocating a dense 3D volume.");
                    if (SliderIntWithInput("Sparse Probe Cell Size##radiancecascades", &sparseProbeCellSize, 1, 128))
                        radianceCascadesSettings->SparseProbeCellSize = static_cast<unsigned int>(sparseProbeCellSize);
                    QtUi::SetItemTooltip("World-space cell size for sparse radiance probes.");
                    if (SliderIntWithInput("Sparse Search Steps##radiancecascades", &sparseProbeSearchSteps, 4, 128))
                        radianceCascadesSettings->SparseProbeSearchSteps = static_cast<unsigned int>(sparseProbeSearchSteps);
                    QtUi::SetItemTooltip("Linear-probe attempts used when resolving hash collisions in the sparse probe table.");
                    SliderFloatWithInput("Sparse Reuse Strength##radiancecascades", &radianceCascadesSettings->SparseProbeReuseStrength, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("How strongly adjacent sparse probe cells are reused when reconstructing GI for pixels without an exact cell match.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Reconstruction");
                    SliderFloatWithInput("Hysteresis##radiancecascades", &radianceCascadesSettings->Hysteresis, 0.0f, 0.99f, "%.2f");
                    QtUi::SetItemTooltip("Temporal history weight after depth and normal rejection.");
                    SliderFloatWithInput("Spatial Filter Strength##radiancecascades", &radianceCascadesSettings->SpatialFilterStrength, 0.0f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Neighborhood reconstruction strength for filling sparse probe GI across nearby pixels.");
                    SliderFloatWithInput("History Clamp Scale##radiancecascades", &radianceCascadesSettings->HistoryClampScale, 0.0f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Allowed temporal history range around the current radiance before clamping.");
                    SliderFloatWithInput("History Depth Sensitivity##radiancecascades", &radianceCascadesSettings->HistoryDepthSensitivity, 1.0f, 512.0f, "%.0f", 1.0f, 16.0f);
                    QtUi::SetItemTooltip("Higher values reject temporal and spatial samples across smaller depth differences.");
                    SliderFloatWithInput("History Normal Threshold##radiancecascades", &radianceCascadesSettings->HistoryNormalThreshold, 0.0f, 0.999f, "%.3f", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("Minimum normal similarity used for history and spatial reconstruction.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");
                    SliderFloatWithInput("GI Intensity##radiancecascades", &radianceCascadesSettings->GiIntensity, 0.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the radiance cascades GI contribution during deferred lighting.");
                    SliderFloatWithInput("Color Bleed##radiancecascades", &radianceCascadesSettings->ColorBleedingStrength, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Boosts bounced material tinting for stronger secondary color bleed. 1.0 = physically based transport.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const auto& debugModes = DebugViewNames::RadianceCascades;
                    QtUi::Combo("Debug View##radiancecascades", &radianceCascadesSettings->DebugView, debugModes, std::size(debugModes));
                }
            }
        }

        // -------------------------------------------------------------------
        // Radiance Probes – world-space irradiance SH probe grid
        // -------------------------------------------------------------------
        if (probeSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Radiance Probes"))
            {
                static RadianceProbeSettings defaultProbeSettings{};
                if (QtUi::Button("Revert All##probe"))
                    *probeSettings = defaultProbeSettings;

                QtUi::Checkbox("Enable Radiance Probes##probe", &probeSettings->Enabled);
                QtUi::SetItemTooltip("Maintain a world-space grid of ray-traced SH irradiance probes.");

                if (probeSettings->Enabled && voxelGiSettings != nullptr && voxelGiSettings->Enabled)
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.75f, 0.3f, 1.0f));
                    QtUi::TextWrapped("Voxel GI is on and takes over surface GI, so the probes are not updated. "
                                      "Turn Voxel GI off to use them.");
                    QtUi::PopStyleColor();
                }

                if (probeSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Grid");

                    SliderIntWithInput("Grid X##probe", &probeSettings->GridX, 1, 64);
                    SliderIntWithInput("Grid Y##probe", &probeSettings->GridY, 1, 64);
                    SliderIntWithInput("Grid Z##probe", &probeSettings->GridZ, 1, 64);
                    SliderFloatWithInput("Spacing (m)##probe", &probeSettings->Spacing, 0.5f, 10.0f, "%.2f");
                    QtUi::SetItemTooltip("Distance between probes in the finest cascade. Each further cascade doubles it.");
                    QtUi::Checkbox("Follow Camera##probe", &probeSettings->FollowCamera);
                    QtUi::SetItemTooltip("Snaps the probe grid origin to the camera position each frame.");
                    if (probeSettings->FollowCamera)
                    {
                        SliderIntWithInput("Cascades##probe", &probeSettings->CascadeCount, 1, kMaxRadianceProbeCascades);
                        QtUi::SetItemTooltip("Nested grids around the camera, each with twice the spacing and reach of the one inside it. "
                                             "GI blends from one into the next instead of ending at the edge of a single grid.");
                    }

                    if (!probeSettings->FollowCamera)
                    {
                        SliderFloatWithInput("Origin X##probe", &probeSettings->OriginX, -200.0f, 200.0f, "%.1f");
                        SliderFloatWithInput("Origin Y##probe", &probeSettings->OriginY, -50.0f, 50.0f,   "%.1f");
                        SliderFloatWithInput("Origin Z##probe", &probeSettings->OriginZ, -200.0f, 200.0f, "%.1f");
                    }

                    QtUi::Separator();
                    QtUi::TextDisabled("Update");

                    SliderIntWithInput("Rays Per Probe##probe", &probeSettings->RaysPerProbe, 16, 512);
                    {
                        // Stored as a divisor of the probes traced each frame.
                        const char* probeResolutions[] = { "Full", "Half", "Quarter" };
                        int probeIndex = probeSettings->TraceResolution >= 4 ? 2 : probeSettings->TraceResolution >= 2 ? 1 : 0;
                        if (QtUi::Combo("Trace Resolution##probe", &probeIndex, probeResolutions, std::size(probeResolutions)))
                            probeSettings->TraceResolution = 1 << probeIndex;
                        QtUi::SetItemTooltip(
                            "Share of the probes traced each frame. Half traces every other probe group in turn,\n"
                            "Quarter one in four; the rest keep their last result until their turn.");
                    }
                    SliderFloatWithInput("Update Blend##probe", &probeSettings->UpdateBlend, 0.01f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend factor between old and new probe SH each frame. Lower = more temporal accumulation.");

                    QtUi::Checkbox("Shadow Map Visibility##probe", &probeSettings->VsmVisibility);
                    QtUi::SetItemTooltip(
                        "Answer light visibility at a bounce hit from the virtual shadow map where it holds a fine enough\n"
                        "page, and trace a shadow ray only where it does not. Needs the virtual shadow map to be on.");
                    if (probeSettings->VsmVisibility)
                    {
                        SliderFloatWithInput("Max Shadow Texel##probe", &probeSettings->VsmMaxTexelSize, 0.005f, 0.25f, "%.3f m", 0.001f, 0.01f);
                        QtUi::SetItemTooltip(
                            "Coarsest shadow-map texel trusted at a bounce hit. Coarse pages leak light through walls\n"
                            "about as thick as a texel, so hits only coarse pages cover trace a ray instead.");
                    }
                    QtUi::Checkbox("Stochastic Light Sampling##probe", &probeSettings->StochasticLights);
                    QtUi::SetItemTooltip(
                        "Test the shadows of only a few lights at each bounce hit, picked by how much each would add,\n"
                        "instead of every light in reach. The probes' temporal blend averages the noise it adds.");
                    if (probeSettings->StochasticLights)
                        SliderIntWithInput("Light Samples##probe", &probeSettings->LightSamples, 1, 4);

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("GI Intensity##probe", &probeSettings->GiIntensity, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the probe diffuse GI before compositing. 1.0 = physically correct.");
                    QtUi::Checkbox("Specular Reflections##probe", &probeSettings->SpecularEnabled);
                    QtUi::SetItemTooltip("Ray-traced specular reflections alongside the probe GI. Uses the RTGI specular pass and its roughness threshold.");
                    if (probeSettings->SpecularEnabled)
                        SliderFloatWithInput("Specular Intensity##probe", &probeSettings->SpecularIntensity, 0.0f, 16.0f, "%.2f");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    QtUi::Checkbox("Show Probe Spheres##probe", &probeSettings->DebugShowProbes);
                    QtUi::SetItemTooltip("Overlay coloured spheres at each probe position visualising stored irradiance.");
                    if (probeSettings->DebugShowProbes)
                    {
                        const char* probeDebugModes[] = { "Diffuse", "Specular" };
                        QtUi::Combo("Lighting Mode##probe", &probeSettings->DebugLightingMode, probeDebugModes, std::size(probeDebugModes));
                        QtUi::SetItemTooltip("Choose whether debug spheres visualize diffuse probe irradiance or a specular-style directional response.");
                        SliderFloatWithInput("Sphere Radius##probe", &probeSettings->DebugSphereRadius, 0.05f, 2.0f, "%.2f");
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        // Voxel GI – voxel cone traced, no ray tracing hardware needed
        // -------------------------------------------------------------------
        if (voxelGiSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Voxel GI"))
            {
                static VoxelGISettings defaultVoxelGiSettings{};
                if (QtUi::Button("Revert All##voxelgi"))
                    *voxelGiSettings = defaultVoxelGiSettings;

                QtUi::Checkbox("Enable Voxel GI##voxelgi", &voxelGiSettings->Enabled);
                QtUi::SetItemTooltip("Voxel cone traced diffuse GI and reflections. Runs on any DirectX 12 GPU,\n"
                                     "and takes over surface GI from RTGI and the radiance probes while on.");
                QtUi::Checkbox("Fallback Without Ray Tracing##voxelgi", &voxelGiSettings->FallbackWithoutRayTracing);
                QtUi::SetItemTooltip("Run voxel GI in place of RTGI / radiance probes on GPUs without DXR 1.1,\n"
                                     "so a level authored with ray traced GI still gets indirect light there.");

                if (voxelGiStatus != nullptr)
                {
                    if (voxelGiStatus->Error != nullptr && voxelGiStatus->Error[0] != '\0')
                    {
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        QtUi::TextWrapped("Error: %s", voxelGiStatus->Error);
                        QtUi::PopStyleColor();
                    }
                    else if (voxelGiStatus->Active)
                    {
                        QtUi::TextDisabled(voxelGiStatus->Fallback
                            ? "Active - standing in for ray traced GI (no DXR 1.1 on this GPU)."
                            : "Active.");
                        QtUi::TextDisabled("Clipmap memory: %.1f MB", static_cast<double>(voxelGiStatus->ClipmapBytes) / (1024.0 * 1024.0));
                    }
                    else if (!voxelGiStatus->RayTracingSupported)
                    {
                        QtUi::TextDisabled("This GPU cannot ray trace (no DXR 1.1).");
                    }
                }

                if (voxelGiSettings->Enabled || voxelGiSettings->FallbackWithoutRayTracing)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Clipmap");

                    const char* resolutions[] = { "32", "64", "96", "128" };
                    const int resolutionValues[] = { 32, 64, 96, 128 };
                    int resolutionIndex = 1;
                    for (int i = 0; i < static_cast<int>(std::size(resolutionValues)); ++i)
                    {
                        if (resolutionValues[i] == voxelGiSettings->Resolution)
                            resolutionIndex = i;
                    }
                    if (QtUi::Combo("Resolution##voxelgi", &resolutionIndex, resolutions, std::size(resolutions)))
                        voxelGiSettings->Resolution = resolutionValues[resolutionIndex];
                    QtUi::SetItemTooltip("Voxels along each edge of one clipmap level. Memory grows with its cube.");
                    SliderIntWithInput("Levels##voxelgi", &voxelGiSettings->LevelCount, 2, 6);
                    QtUi::SetItemTooltip("Nested levels around the camera, each twice the extent and voxel size of the one inside it.");
                    SliderFloatWithInput("Voxel Size##voxelgi", &voxelGiSettings->VoxelSize, 0.05f, 2.0f, "%.2f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Voxel edge in the finest level. Walls thinner than about one voxel leak light.");
                    {
                        const float reach = voxelGiSettings->VoxelSize * static_cast<float>(voxelGiSettings->Resolution)
                            * static_cast<float>(1 << ((std::clamp)(voxelGiSettings->LevelCount, 2, 6) - 1)) * 0.5f;
                        QtUi::TextDisabled("Covers %.0f m around the camera.", reach);
                    }
                    SliderIntWithInput("LOD Bias##voxelgi", &voxelGiSettings->LodBias, 0, 4);
                    QtUi::SetItemTooltip("Mesh LOD voxelized into the finest level; each coarser level uses one LOD coarser.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Lighting");

                    QtUi::Checkbox("Sun Shadows##voxelgi", &voxelGiSettings->SunShadows);
                    QtUi::SetItemTooltip("Trace the sun's shadow through the voxels. Off leaks sunlight into interiors.");
                    QtUi::Checkbox("Local Light Shadows##voxelgi", &voxelGiSettings->LocalLightShadows);
                    QtUi::SetItemTooltip("Trace each local light's shadow through the voxels.");
                    if (voxelGiSettings->SunShadows || voxelGiSettings->LocalLightShadows)
                    {
                        QtUi::Checkbox("Shadow Map Visibility##voxelgi", &voxelGiSettings->VsmVisibility);
                        QtUi::SetItemTooltip(
                            "Answer the voxels' light visibility from the virtual shadow map where it holds a fine enough\n"
                            "page, and march through the voxels only where it does not. Far cheaper than the march and\n"
                            "sharper than the voxels. Needs the virtual shadow map to be on.");
                        if (voxelGiSettings->VsmVisibility)
                        {
                            SliderFloatWithInput("Max Shadow Texel##voxelgi", &voxelGiSettings->VsmMaxTexelSize, 0.005f, 0.5f, "%.3f m", 0.001f, 0.01f);
                            QtUi::SetItemTooltip(
                                "Coarsest shadow-map texel trusted at a voxel. Coarse pages leak light through walls about\n"
                                "as thick as a texel, so voxels only coarse pages cover march instead.");
                        }
                    }
                    QtUi::Checkbox("Stochastic Light Sampling##voxelgi", &voxelGiSettings->StochasticLights);
                    QtUi::SetItemTooltip(
                        "Light each voxel from only a few local lights, picked by how much each would add, instead of every\n"
                        "light in reach. The picks change every update and the temporal blend averages them.");
                    if (voxelGiSettings->StochasticLights)
                        SliderIntWithInput("Light Samples##voxelgi", &voxelGiSettings->LightSamples, 1, 4);
                    SliderFloatWithInput("Multi Bounce##voxelgi", &voxelGiSettings->MultiBounce, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("How much of the previous bounce feeds back into the voxels. 0 = a single bounce.");
                    SliderFloatWithInput("Temporal Blend##voxelgi", &voxelGiSettings->TemporalBlend, 0.01f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Share of the voxels' new lighting taken per frame. Lower is steadier but slower\n"
                                         "to follow moving or flickering lights. 1 = no smoothing.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Tracing");

                    const char* coneModes[] = { "1 (fastest)", "5", "9 (smoothest)" };
                    int coneIndex = voxelGiSettings->DiffuseCones <= 1 ? 0 : (voxelGiSettings->DiffuseCones >= 9 ? 2 : 1);
                    if (QtUi::Combo("Diffuse Cones##voxelgi", &coneIndex, coneModes, std::size(coneModes)))
                        voxelGiSettings->DiffuseCones = coneIndex == 0 ? 1 : (coneIndex == 2 ? 9 : 5);
                    QtUi::Checkbox("Half Resolution##voxelgi", &voxelGiSettings->HalfResolution);
                    QtUi::SetItemTooltip("Trace a quarter of the pixels and upsample along depth and normal edges.");
                    SliderFloatWithInput("Max Distance##voxelgi", &voxelGiSettings->MaxTraceDistance, 5.0f, 200.0f, "%.0f m");
                    QtUi::SetItemTooltip("Furthest a cone travels before it takes the sky.");
                    SliderFloatWithInput("Surface Offset##voxelgi", &voxelGiSettings->SurfaceOffset, 0.0f, 4.0f, "%.2f voxels");
                    QtUi::SetItemTooltip("How far cones start off the surface. Too low darkens surfaces with their own voxels.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("GI Intensity##voxelgi", &voxelGiSettings->GiIntensity, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the voxel diffuse GI before compositing. 1.0 = physically correct.");
                    QtUi::Checkbox("Specular Reflections##voxelgi", &voxelGiSettings->SpecularEnabled);
                    QtUi::SetItemTooltip("Cone traced reflections. Screen-space reflections, when on, replace them wherever they hit.");
                    if (voxelGiSettings->SpecularEnabled)
                    {
                        SliderFloatWithInput("Roughness Threshold##voxelgi", &voxelGiSettings->SpecularRoughnessThreshold, 0.0f, 1.0f, "%.2f");
                        SliderFloatWithInput("Specular Intensity##voxelgi", &voxelGiSettings->SpecularIntensity, 0.0f, 16.0f, "%.2f");
                    }

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    const auto& voxelDebugModes = DebugViewNames::VoxelGi;
                    const auto& voxelDebugValues = DebugViewNames::VoxelGiValues;
                    int voxelDebugIndex = 0;
                    for (int i = 0; i < static_cast<int>(std::size(voxelDebugValues)); ++i)
                    {
                        if (voxelDebugValues[i] == voxelGiSettings->DebugView)
                            voxelDebugIndex = i;
                    }
                    if (QtUi::Combo("Debug View##voxelgi", &voxelDebugIndex, voxelDebugModes, std::size(voxelDebugModes)))
                        voxelGiSettings->DebugView = voxelDebugValues[voxelDebugIndex];
                }
            }
        }

        // -------------------------------------------------------------------
        // RTAO – Ray Traced Ambient Occlusion
        // -------------------------------------------------------------------
        if (rtaoSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Ray Traced Ambient Occlusion", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##rtao"))
                {
                    *rtaoSettings = defaultRtaoSettings;
                }
                QtUi::Checkbox("Enable RTAO (Experimental)##rtao", &rtaoSettings->Enabled);
                QtUi::SetItemTooltip("Enable ray-traced ambient occlusion using DXR inline RayQuery.");

                if (rtaoSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Ray Generation");

                    SliderIntWithInput("Rays Per Pixel##rtao", &rtaoSettings->RaysPerPixel, 1, 16);
                    QtUi::SetItemTooltip("Number of occlusion rays per pixel per frame. Higher = less noise, lower performance.");

                    SliderFloatWithInput("Max Ray Length##rtao", &rtaoSettings->MaxRayLength, 0.01f, 10.0f, "%.3f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Maximum length (world units) of each occlusion ray.");

                    SliderFloatWithInput("Ray Bias##rtao", &rtaoSettings->RayBias, 0.0001f, 0.05f, "%.4f", 0.0001f, 0.001f);
                    QtUi::SetItemTooltip("Normal-direction offset applied to ray origins to avoid self-intersection.");

                    SliderFloatWithInput("AO Power##rtao", &rtaoSettings->AOPower, 0.5f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Gamma exponent applied to the raw AO value. >1 darkens, <1 brightens.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("AO Intensity##rtao", &rtaoSettings->Intensity, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend weight of RTAO over the G-Buffer baked AO. 1.0 = full RTAO.");

                    SliderFloatWithInput("Local Light Occlusion##rtao", &rtaoSettings->LocalLightOcclusion, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("How much RTAO also darkens point, spot and rect lights. 0 = sky ambient and GI only, which leaves AO invisible in spaces lit by lamps; 1 = full.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    const auto& aoDebugModes = DebugViewNames::Rtao;
                    QtUi::Combo("Debug View##rtao", &rtaoSettings->DebugView, aoDebugModes, std::size(aoDebugModes));
                    QtUi::SetItemTooltip("Overlay mode for inspecting raw versus NRD-denoised RTAO.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Path tracing – progressive reference render
        // -------------------------------------------------------------------
        if (pathTracerSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Path Tracing"))
            {
                const PathTracerSettings defaultPathTracerSettings{};
                if (QtUi::Button("Revert All##pathtracer"))
                {
                    const bool wasEnabled = pathTracerSettings->Enabled;
                    *pathTracerSettings = defaultPathTracerSettings;
                    pathTracerSettings->Enabled = wasEnabled;
                }
                QtUi::Checkbox("Enable Path Tracing (Experimental)##pathtracer", &pathTracerSettings->Enabled);
                QtUi::SetItemTooltip("Replace the lit scene with a path-traced render. Needs a GPU with DXR 1.1 ray queries.");

                if (pathTracerSettings->Enabled)
                {
                    const char* pathTracerModes[] = { "Reference (accumulate)", "Real Time (NRD denoised)" };
                    int pathTracerMode = static_cast<int>(pathTracerSettings->Mode);
                    if (QtUi::Combo("Mode##pathtracer", &pathTracerMode, pathTracerModes, std::size(pathTracerModes)))
                        pathTracerSettings->Mode = static_cast<PathTracerMode>(pathTracerMode);
                    QtUi::SetItemTooltip("Reference: full paths from the camera, refined while nothing changes - the ground truth.\n"
                                         "Real Time: one diffuse and one specular path per pixel from the G-Buffer every frame,\n"
                                         "denoised by NVIDIA NRD - stable while moving.");

                    if (pathTracerStatus != nullptr)
                        QtUi::TextWrapped("%s", pathTracerStatus);
                    const bool realTime = pathTracerSettings->Mode == PathTracerMode::RealTime;
                    if (QtUi::Button("Restart##pathtracer"))
                        pathTracerSettings->ResetAccumulation = true;
                    QtUi::SetItemTooltip(realTime ? "Throw the denoiser's history away." : "Throw the accumulated image away and start again.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Sampling");

                    if (realTime)
                    {
                        SliderIntWithInput("Bounces##pathtracer_rt", &pathTracerSettings->RealTimeMaxBounces, 0, 8);
                        QtUi::SetItemTooltip("Bounces after the visible surface. Each costs a ray per lobe per pixel per frame.\n"
                                             "0 = direct light only, 1 = one-bounce GI and reflections.");

                        SliderIntWithInput("Samples Per Pixel##pathtracer_rt", &pathTracerSettings->RealTimeSamplesPerPixel, 1, 4);
                        QtUi::SetItemTooltip("Paths per lobe per pixel each frame, averaged before denoising.\n"
                                             "Less noise in dark and contact-lit areas, proportionally more GPU time.");

                        QtUi::Checkbox("Screen Radiance Cache##pathtracer", &pathTracerSettings->ScreenCache);
                        QtUi::SetItemTooltip("Paths that reach their last bounce on a surface that was on screen last frame take\n"
                                             "that frame's denoised lighting there. Converges to unlimited bounces for no extra rays,\n"
                                             "which is what lights interiors that only get bounced light. Needs Denoise.");

                        QtUi::Checkbox("Denoise##pathtracer", &pathTracerSettings->Denoise);
                        QtUi::SetItemTooltip("NVIDIA NRD RELAX. Off shows the raw signal: one diffuse and one specular path per pixel.");

                        if (pathTracerSettings->Denoise)
                        {
                            SliderIntWithInput("History Frames##pathtracer", &pathTracerSettings->DenoiserHistoryFrames, 4, 60);
                            QtUi::SetItemTooltip("Frames the denoiser accumulates over. More is smoother but slower to follow\n"
                                                 "moving lights and objects.");
                        }
                    }
                    else
                    {
                        SliderIntWithInput("Max Bounces##pathtracer", &pathTracerSettings->MaxBounces, 0, 32);
                        QtUi::SetItemTooltip("Bounces after the first hit. 0 = direct light only.");

                        SliderIntWithInput("Samples Per Frame##pathtracer", &pathTracerSettings->SamplesPerFrame, 1, 16);
                        QtUi::SetItemTooltip("Paths per pixel each frame. Converges faster per frame, costs proportionally more.");

                        SliderIntWithInput("Max Samples##pathtracer", &pathTracerSettings->MaxSamples, 0, 65536);
                        QtUi::SetItemTooltip("Stop refining after this many samples per pixel and hold the image. 0 = never stop.");
                    }

                    SliderFloatWithInput("Indirect Clamp##pathtracer", &pathTracerSettings->IndirectClamp, 0.0f, 100.0f, "%.1f", 1.0f, 10.0f);
                    QtUi::SetItemTooltip("Caps the brightness of bounced light per sample to remove fireflies.\n0 = off (unbiased, but slow to clean up).");

                    QtUi::Separator();
                    QtUi::TextDisabled("Scene");

                    QtUi::Checkbox("Soft Shadows##pathtracer", &pathTracerSettings->SoftShadows);
                    QtUi::SetItemTooltip("Sample the sun's disc and the area of sized lights. Off treats every light as a point.");

                    QtUi::Checkbox("Terrain Displacement Shadows##pathtracer", &pathTracerSettings->TerrainDisplacementShadows);
                    QtUi::SetItemTooltip("Shadow rays leaving displaced terrain march its height maps, so sand ripples and small dunes shadow themselves.\nThe traced terrain is the undisplaced mesh; this costs 16 height lookups per shadow ray on terrain.");

                    QtUi::Checkbox("Alpha-Tested Materials##pathtracer", &pathTracerSettings->AlphaTest);
                    QtUi::SetItemTooltip("Let light through the cut-out parts of alpha-tested materials (foliage, fences).\nOff treats them as solid, which is faster.");

                    SliderFloatWithInput("Emissive Intensity##pathtracer", &pathTracerSettings->EmissiveIntensity, 0.0f, 100.0f, "%.2f", 0.1f, 1.0f);
                    QtUi::SetItemTooltip("Multiplier on material emissive colour. Emissive surfaces light the scene only when path tracing.");

                    SliderFloatWithInput("Ground Albedo##pathtracer", &pathTracerSettings->GroundAlbedo, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Terrain is not ray traced. Rays that leave the scene downward see a flat ground\nof this brightness, lit by the sun and sky.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    const auto& pathTracerDebugModes = DebugViewNames::PathTracer;
                    const int debugModeCount = realTime ? static_cast<int>(std::size(pathTracerDebugModes))
                                                        : DebugViewNames::PathTracerReferenceCount;
                    if (pathTracerSettings->DebugView >= debugModeCount)
                        pathTracerSettings->DebugView = 0;
                    QtUi::Combo("Debug View##pathtracer", &pathTracerSettings->DebugView, pathTracerDebugModes, debugModeCount);
                    QtUi::SetItemTooltip("Show what the path tracer sees at the first hit: the materials and normals it was given.\n"
                                         "Real time also shows each lobe's lighting before the surface colour - what NRD filters.");
                }
            }
        }

        // -------------------------------------------------------------------
        // XeGTAO – Screen-Space Ground Truth Ambient Occlusion
        // -------------------------------------------------------------------
        if (gtaoSettings != nullptr)
        {
            const GtaoSettings defaultGtaoSettings{};
            if (QtUi::CollapsingHeader("XeGTAO (Screen-Space AO)"))
            {
                if (QtUi::Button("Revert All##gtao"))
                {
                    *gtaoSettings = defaultGtaoSettings;
                }
                QtUi::Checkbox("Enable XeGTAO##gtao", &gtaoSettings->Enabled);
                QtUi::SetItemTooltip("Intel XeGTAO screen-space ambient occlusion. Runs in parallel with (or instead of) RTAO.");

                if (gtaoSettings->Enabled)
                {
                    const char* qualityNames[] = { "Low", "Medium", "High", "Ultra" };
                    QtUi::Combo("Quality Level##gtao", &gtaoSettings->QualityLevel, qualityNames, std::size(qualityNames));
                    QtUi::SetItemTooltip("Higher quality uses more samples per pixel.");

                    SliderFloatWithInput("Radius##gtao",            &gtaoSettings->Radius,                   0.1f, 5.0f,  "%.2f");
                    QtUi::SetItemTooltip("World-space maximum AO radius.");

                    SliderFloatWithInput("Radius Multiplier##gtao", &gtaoSettings->RadiusMultiplier,         0.5f, 3.0f,  "%.3f");
                    QtUi::SetItemTooltip("Scales the effective radius to counteract screen-space bias.");

                    SliderFloatWithInput("Falloff Range##gtao",     &gtaoSettings->FalloffRange,             0.0f, 1.0f,  "%.3f");
                    QtUi::SetItemTooltip("Fraction of the radius over which distant samples fade out.");

                    SliderIntWithInput("Denoise Passes##gtao",      &gtaoSettings->DenoisePasses,            0,   2);
                    QtUi::SetItemTooltip("0 = no spatial denoise, 1-2 = iterative bilateral blur.");

                    SliderFloatWithInput("Final Value Power##gtao", &gtaoSettings->FinalValuePower,          0.5f, 5.0f,  "%.2f");
                    QtUi::SetItemTooltip("Power curve applied to the final AO value.");

                    SliderFloatWithInput("AO Intensity##gtao",      &gtaoSettings->Intensity,                0.0f, 1.0f,  "%.2f");
                    QtUi::SetItemTooltip("Blend weight of XeGTAO over the G-Buffer baked AO. 1.0 = full GTAO.");

                    SliderFloatWithInput("Sample Distribution Power##gtao", &gtaoSettings->SampleDistributionPower, 1.0f, 3.0f, "%.2f");
                    QtUi::SetItemTooltip("Concentrates more samples toward small crevices.");

                    SliderFloatWithInput("Thin Occluder Compensation##gtao", &gtaoSettings->ThinOccluderCompensation, 0.0f, 0.7f, "%.2f");
                    QtUi::SetItemTooltip("Reduces over-occlusion on thin geometry.");

                    SliderFloatWithInput("Depth MIP Sampling Offset##gtao", &gtaoSettings->DepthMIPSamplingOffset, 2.0f, 6.0f, "%.2f");
                    QtUi::SetItemTooltip("Trade-off between performance (bandwidth) and quality.");

                    const auto& gtaoDebugModes = DebugViewNames::Gtao;
                    QtUi::Combo("Debug View##gtao", &gtaoSettings->DebugView, gtaoDebugModes, std::size(gtaoDebugModes));
                    QtUi::SetItemTooltip("Overlay the raw GTAO output for inspection.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Screen-Space Reflections
        // -------------------------------------------------------------------
        if (ssrSettings != nullptr)
        {
            const SsrSettings defaultSsrSettings{};
            if (QtUi::CollapsingHeader("Screen-Space Reflections", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##ssr"))
                    *ssrSettings = defaultSsrSettings;

                QtUi::Checkbox("Enable SSR##ssr", &ssrSettings->Enabled);
                QtUi::SetItemTooltip("Reflects what is already on screen. Anything off screen, "
                                     "behind the camera, or hidden behind other geometry cannot be "
                                     "reflected - that is the standing limitation of the technique.");

                if (ssrSettings->Enabled)
                {
                    const char* ssrTechniques[] = { "Ray March", "AMD FidelityFX SSSR" };
                    QtUi::Combo("Technique##ssr", &ssrSettings->Technique, ssrTechniques, std::size(ssrTechniques));
                    QtUi::SetItemTooltip("Ray March: one sharp ray per pixel, marched in world space.\n"
                                         "AMD FidelityFX SSSR: stochastic GGX rays traced through a depth "
                                         "pyramid and denoised over time, so glossy surfaces get properly "
                                         "blurred reflections.");
                    const bool fidelityFx = ssrSettings->Technique == 1;

                    QtUi::Separator();
                    SliderFloatWithInput("Intensity##ssr", &ssrSettings->Intensity, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Scales the reflection before it is added. 1.0 is physical strength.");

                    SliderFloatWithInput("Max Roughness##ssr", &ssrSettings->MaxRoughness, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
                    QtUi::SetItemTooltip(fidelityFx
                        ? "Surfaces rougher than this are not traced. Reflections fade out over the "
                          "last quarter below it."
                        : "Surfaces rougher than this get no reflection - a single sharp ray "
                          "cannot stand in for the blurred lobe they need.");

                    if (fidelityFx)
                    {
                        QtUi::SeparatorText("Tracing");
                        SliderIntWithInput("Max Traversal Steps##sssr", &ssrSettings->SssrMaxTraversalIntersections, 8, 512);
                        QtUi::SetItemTooltip("Depth-pyramid steps per ray before it gives up. The main cost control.");

                        SliderFloatWithInput("Depth Thickness##sssr", &ssrSettings->SssrDepthThickness, 0.0f, 0.5f, "%.3f m", 0.001f, 0.01f);
                        QtUi::SetItemTooltip("How far behind the depth buffer a hit may land and still be trusted.");

                        const char* samplesPerQuadModes[] = { "1", "2", "4" };
                        int samplesPerQuadIndex = ssrSettings->SssrSamplesPerQuad >= 4 ? 2 : (ssrSettings->SssrSamplesPerQuad >= 2 ? 1 : 0);
                        if (QtUi::Combo("Rays per Quad##sssr", &samplesPerQuadIndex, samplesPerQuadModes, std::size(samplesPerQuadModes)))
                            ssrSettings->SssrSamplesPerQuad = 1 << samplesPerQuadIndex;
                        QtUi::SetItemTooltip("Glossy rays traced per 2x2 pixel quad; the denoiser fills in the rest. "
                                             "Mirror-smooth surfaces always get one ray per pixel.");

                        SliderIntWithInput("Most Detailed Mip##sssr", &ssrSettings->SssrMostDetailedMip, 0, 4);
                        QtUi::SetItemTooltip("Depth-pyramid level glossy rays start on. Higher is cheaper and coarser.");

                        SliderIntWithInput("Min Occupancy##sssr", &ssrSettings->SssrMinTraversalOccupancy, 0, 32);
                        QtUi::SetItemTooltip("A wave stops tracing once this few rays are still running, so one "
                                             "long ray cannot stall the rest.");

                        QtUi::SeparatorText("Denoiser");
                        SliderFloatWithInput("Temporal Stability##sssr", &ssrSettings->SssrTemporalStability, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
                        QtUi::SetItemTooltip("How loosely history is clipped to the current frame. Higher is steadier; "
                                             "lower reacts faster and ghosts less.");

                        QtUi::Checkbox("Variance-Guided Tracing##sssr", &ssrSettings->SssrTemporalVarianceGuidedTracing);
                        QtUi::SetItemTooltip("Also trace pixels skipped by Rays per Quad wherever last frame's result "
                                             "was still unstable.");

                        if (ssrSettings->SssrTemporalVarianceGuidedTracing)
                        {
                            SliderFloatWithInput("Variance Threshold##sssr", &ssrSettings->SssrTemporalVarianceThreshold, 0.0f, 0.01f, "%.4f", 0.0001f, 0.001f);
                            QtUi::SetItemTooltip("Temporal variance above which a skipped pixel is traced anyway.");
                        }

                        QtUi::Separator();
                        const auto& sssrDebugModes = DebugViewNames::Sssr;
                        QtUi::Combo("Debug View##sssr", &ssrSettings->DebugView, sssrDebugModes, std::size(sssrDebugModes));
                        QtUi::SetItemTooltip("Isolates the denoised reflection, or the traced result before the denoiser.");
                    }
                }

                if (ssrSettings->Enabled && ssrSettings->Technique != 1)
                {
                    SliderFloatWithInput("Max Distance##ssr", &ssrSettings->MaxDistance, 1.0f, 400.0f, "%.1f m", 1.0f, 10.0f);
                    QtUi::SetItemTooltip("Longest ray, and the distance over which reflections fade out.");

                    QtUi::SeparatorText("Ray March");
                    SliderIntWithInput("Max Steps##ssr", &ssrSettings->MaxSteps, 8, 256);
                    QtUi::SetItemTooltip("Ray-march budget. The main cost control.");

                    SliderFloatWithInput("Step Size##ssr", &ssrSettings->StepSize, 0.01f, 2.0f, "%.3f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Length of the first step. Smaller resolves contact reflections better.");

                    SliderFloatWithInput("Step Growth##ssr", &ssrSettings->StepGrowth, 1.0f, 1.5f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Geometric growth per step, so reach costs less than precision. 1.0 marches evenly.");

                    SliderFloatWithInput("Thickness##ssr", &ssrSettings->Thickness, 0.01f, 5.0f, "%.3f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("How far behind the depth buffer a sample still counts as a hit. "
                                         "Too small and rays pass through thin geometry; too large and they "
                                         "latch onto the wrong surface.");

                    SliderIntWithInput("Refine Steps##ssr", &ssrSettings->RefineSteps, 0, 16);
                    QtUi::SetItemTooltip("Binary-search iterations that pin down the hit after the coarse march.");

                    SliderFloatWithInput("Edge Fade##ssr", &ssrSettings->EdgeFadeStart, 0.0f, 0.99f, "%.2f", 0.01f, 0.05f);
                    QtUi::SetItemTooltip("Where reflections start fading toward the screen border, which has no data behind it.");

                    QtUi::Separator();
                    const auto& ssrDebugModes = DebugViewNames::Ssr;
                    QtUi::Combo("Debug View##ssr", &ssrSettings->DebugView, ssrDebugModes, std::size(ssrDebugModes));
                    QtUi::SetItemTooltip("Isolates the reflection term, or shows where SSR found a hit and how much it trusts it.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Shading Model
        // -------------------------------------------------------------------
        if (shadingSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Shading Model", QtUiTreeNodeFlags_DefaultOpen))
            {
                QtUi::Checkbox("Disney Principled BRDF##shading", &shadingSettings->DisneyBrdf);
                QtUi::SetItemTooltip("Burley's Disney BRDF: retro-reflective diffuse, Disney's GGX masking, and "
                                     "the material lobes it adds (subsurface, specular tint, anisotropic, sheen, "
                                     "clearcoat). Off: the original Lambert + Cook-Torrance model, with those "
                                     "material settings ignored. Applies to the lighting pass and the path tracer.");
            }
        }

        // -------------------------------------------------------------------
        // Subsurface Scattering
        // -------------------------------------------------------------------
        if (subsurfaceSettings != nullptr)
        {
            const SubsurfaceSettings defaultSubsurfaceSettings{};
            if (QtUi::CollapsingHeader("Subsurface Scattering", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##sss"))
                    *subsurfaceSettings = defaultSubsurfaceSettings;

                QtUi::Checkbox("Enable Subsurface Scattering##sss", &subsurfaceSettings->Enabled);
                QtUi::SetItemTooltip("Scatters the diffuse light of materials with Subsurface Scattering "
                                     "enabled in the Material Editor. Costs nothing while none are on screen.");

                if (subsurfaceSettings->Enabled)
                {
                    QtUi::Separator();
                    const char* sssModes[] = { "Screen Space (Separable SSS)", "Ray Traced (World Space)" };
                    QtUi::Combo("Mode##sss", &subsurfaceSettings->Mode, sssModes, std::size(sssModes));
                    QtUi::SetItemTooltip("Screen Space: Jimenez's separable blur of the diffuse lighting. Cheap and "
                                         "stable, but it only sees the depth buffer and measures transmission "
                                         "from shadow maps.\n"
                                         "Ray Traced (World Space): scatter samples are found on the real mesh with "
                                         "probe rays and lit there by every light with ray-traced shadows - nothing "
                                         "is read from the screen, so light wraps around ears, noses and fingers "
                                         "and behind silhouettes. Transmission thickness is measured through the "
                                         "mesh. Needs DXR 1.1.");
                    if (subsurfaceSettings->Mode == 1 && !subsurfaceRayTracingSupported)
                    {
                        QtUi::TextDisabled("This GPU has no DXR 1.1 - rendering in screen space.");
                    }

                    if (subsurfaceSettings->Mode == 0)
                    {
                        const char* sssQualities[] = { "Low (11 taps)", "Medium (17 taps)", "High (25 taps)" };
                        QtUi::Combo("Quality##sss", &subsurfaceSettings->Quality, sssQualities, std::size(sssQualities));
                        QtUi::SetItemTooltip("Taps per blur direction. High also widens the kernel to the full "
                                             "profile range.");

                        QtUi::Checkbox("Follow Surface##sss", &subsurfaceSettings->FollowSurface);
                        QtUi::SetItemTooltip("Stops the blur at depth discontinuities so light does not bleed "
                                             "off a silhouette onto whatever is behind it.");
                    }
                    else
                    {
                        SliderIntWithInput("Samples##sss", &subsurfaceSettings->RtSamples, 1, 64);
                        QtUi::SetItemTooltip("Surface probes per pixel. The pattern rotates every frame, so "
                                             "temporal anti-aliasing integrates it; 8-16 is plenty with TAA, "
                                             "DLSS or FSR. With ReSTIR, 4-8 is usually enough.");

                        QtUi::Checkbox("ReSTIR Temporal Reuse##sss", &subsurfaceSettings->RtRestir);
                        QtUi::SetItemTooltip("Resamples each pixel's probes together with its previous frames' "
                                             "and lights the chosen points afresh every frame: about half the "
                                             "noise at the same sample count, with no lag on moving lights. "
                                             "Off: each pixel averages its own probes.");
                        QtUi::BeginDisabled(!subsurfaceSettings->RtRestir);
                        SliderIntWithInput("History Length##sss", &subsurfaceSettings->RtHistoryLength, 1, 64);
                        QtUi::SetItemTooltip("Frames of probes the history may stand for. 5 suits a moving or "
                                             "jittered (TAA, DLSS, FSR) camera; longer only helps a perfectly still "
                                             "view and otherwise leaves slowly-changing blotches.");
                        QtUi::EndDisabled();
                    }

                    QtUi::SeparatorText("Transmission");
                    QtUi::Checkbox("Transmission##sss", &subsurfaceSettings->Transmission);
                    QtUi::SetItemTooltip("Light passing through thin parts - ears, fingers, leaves, candle rims - "
                                         "and lighting the far side. Each material's Translucency sets how much.");
                    QtUi::BeginDisabled(!subsurfaceSettings->Transmission);
                    SliderFloatWithInput("Transmission Intensity##sss", &subsurfaceSettings->TransmissionIntensity,
                                         0.0f, 4.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::EndDisabled();

                    QtUi::Separator();
                    const auto& sssDebugModes = DebugViewNames::Subsurface;
                    QtUi::Combo("Debug View##sss", &subsurfaceSettings->DebugView, sssDebugModes, std::size(sssDebugModes));
                    QtUi::SetItemTooltip("Shows the scattered diffuse light on its own, or which pixels "
                                         "scatter and with which material profile.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Chromatic Aberration
        // -------------------------------------------------------------------
        if (chromaticAberrationSettings != nullptr)
        {
            const ChromaticAberrationSettings defaultChromaticAberrationSettings{};
            if (QtUi::CollapsingHeader("Chromatic Aberration", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##ca"))
                    *chromaticAberrationSettings = defaultChromaticAberrationSettings;

                QtUi::Checkbox("Enable Chromatic Aberration##ca", &chromaticAberrationSettings->Enabled);
                QtUi::SetItemTooltip("Splits the colour channels toward the edges of the frame, the way a "
                                     "lens focuses each wavelength at a slightly different magnification. "
                                     "Applied last, on the tonemapped image.");

                if (chromaticAberrationSettings->Enabled)
                {
                    QtUi::Separator();
                    SliderFloatWithInput("Strength##ca", &chromaticAberrationSettings->Strength, 0.0f, 20.0f, "%.2f px", 0.1f, 1.0f);
                    QtUi::SetItemTooltip("Channel displacement at the corner of the frame, in pixels. The centre "
                                         "is always clean. 1-3 reads as lens character; past 8 it reads as an effect.");

                    SliderFloatWithInput("Falloff##ca", &chromaticAberrationSettings->Falloff, 0.0f, 6.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("How quickly the split grows from the centre outward. 1 is a linear ramp; "
                                         "higher values keep the middle of the frame clean.");

                    SliderIntWithInput("Sample Count##ca", &chromaticAberrationSettings->SampleCount, 1, 16);
                    QtUi::SetItemTooltip("1 does a plain three-channel split - cheapest, and fine at low strength. "
                                         "Higher counts smear spectrally along the same line, which stops the fringe "
                                         "banding into hard red/blue edges when the strength is pushed up.");

                    QtUi::SeparatorText("Optical Centre");
                    SliderFloatWithInput("Center X##ca", &chromaticAberrationSettings->CenterX, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    SliderFloatWithInput("Center Y##ca", &chromaticAberrationSettings->CenterY, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("The point the aberration radiates from. Move it off centre to imitate a decentred lens.");
                }
            }
        }

        // -------------------------------------------------------------------
        // AgX Tonemapper
        // -------------------------------------------------------------------
        if (agxSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("AgX Tonemapper", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##agx"))
                {
                    *agxSettings = defaultAgxSettings;
                }
                QtUi::Checkbox("Enable AgX Tonemapper##agx", &agxSettings->Enabled);
                QtUi::SetItemTooltip("Apply the AgX display transform to the final image.");

                if (agxSettings->Enabled)
                {
                    auto drawGradeControl = [](const char* label, AgxColorGradeControl& control)
                    {
                        QtUi::PushID(label);
                        if (QtUi::TreeNode(label))
                        {
                            SliderFloatWithInput("Total", &control.Total, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Red", &control.Red, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Green", &control.Green, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Blue", &control.Blue, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Yellow", &control.Yellow, -1.0f, 1.0f, "%.2f");
                            QtUi::TreePop();
                        }
                        QtUi::PopID();
                    };

                    auto drawGradeRegion = [&drawGradeControl](const char* label, AgxColorGradeRegion& region)
                    {
                        QtUi::PushID(label);
                        if (QtUi::CollapsingHeader(label, QtUiTreeNodeFlags_DefaultOpen))
                        {
                            drawGradeControl("Contrast", region.Contrast);
                            drawGradeControl("Gamma", region.Gamma);
                            drawGradeControl("Gain", region.Gain);
                            drawGradeControl("Saturation", region.Saturation);
                            drawGradeControl("Vibrance", region.Vibrance);
                        }
                        QtUi::PopID();
                    };

                    QtUi::Separator();
                    QtUi::TextDisabled("Exposure");

                    if (timeOfDaySettings != nullptr && timeOfDaySettings->Enabled && timeOfDaySettings->ControlExposure)
                    {
                        QtUi::TextWrapped("Time of Day is controlling the EV100 (Windows > Time of Day). "
                                          "The mode and EV100 below are ignored while it does; the trim still applies.");
                    }

                    {
                        const char* exposureModes[] = { "Manual", "Auto (Histogram)" };
                        int exposureMode = static_cast<int>(agxSettings->ExposureMode);
                        if (QtUi::Combo("Exposure Mode##agx", &exposureMode, exposureModes, 2))
                        {
                            agxSettings->ExposureMode = static_cast<AgxExposureMode>(exposureMode);
                        }
                        QtUi::SetItemTooltip("Manual uses the EV100 value below. Auto meters the scene with a 64-bin luminance histogram each frame and adapts towards it, clamped to the EV100 Min/Max range.");
                    }

                    const bool autoExposure = agxSettings->ExposureMode == AgxExposureMode::AutoHistogram;

                    QtUi::BeginDisabled(autoExposure);
                    SliderFloatWithInput("Exposure (EV100)##agx", &agxSettings->Ev100, -16.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Photographic exposure. One unit is one stop and higher is darker, like stopping a camera down. AgX itself always encodes over the same fixed window; this is what decides how bright the frame is. Driven by the meter in Auto mode.");
                    QtUi::EndDisabled();

                    SliderFloatWithInput("Exposure Trim (EV)##agx", &agxSettings->Exposure, -5.0f, 5.0f, "%.2f");
                    QtUi::SetItemTooltip("Offset on top of the EV100 exposure, in stops. 0 = no change, positive = brighter.");

                    const bool minEvChanged = SliderFloatWithInput("EV100 Min##agx", &agxSettings->Ev100Min, -16.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Lower clamp on the exposure. In Auto mode this is the darkest the meter is allowed to settle. Set Min and Max to the same value to pin the exposure there.");

                    const bool maxEvChanged = SliderFloatWithInput("EV100 Max##agx", &agxSettings->Ev100Max, -16.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Upper clamp on the exposure. In Auto mode this is the brightest the meter is allowed to settle. Set Min and Max to the same value to pin the exposure there.");

                    // Min == Max is a legal, useful setting now that these only clamp
                    // the exposure, so the bounds are merely kept in order: push the
                    // one the user is not dragging.
                    if (agxSettings->Ev100Max < agxSettings->Ev100Min)
                    {
                        if (minEvChanged && !maxEvChanged)
                        {
                            agxSettings->Ev100Max = agxSettings->Ev100Min;
                        }
                        else
                        {
                            agxSettings->Ev100Min = agxSettings->Ev100Max;
                        }
                    }

                    if (autoExposure)
                    {
                        QtUi::Separator();
                        QtUi::TextDisabled("Auto Exposure");

                        SliderFloatWithInput("Speed Up##agx", &agxSettings->AutoExposureSpeedUp, 0.0f, 10.0f, "%.2f");
                        QtUi::SetItemTooltip("Stops per second when adapting to a brighter scene, such as walking out of a cave. 0 freezes the adaptation.");

                        SliderFloatWithInput("Speed Down##agx", &agxSettings->AutoExposureSpeedDown, 0.0f, 10.0f, "%.2f");
                        QtUi::SetItemTooltip("Stops per second when adapting to a darker scene. Usually slower than Speed Up, the way an eye takes longer to dark-adapt.");

                        SliderFloatWithInput("Low Percent##agx", &agxSettings->AutoExposureLowPercent, 0.0f, 1.0f, "%.2f");
                        QtUi::SetItemTooltip("Fraction of the darkest pixels ignored when metering. Raising it stops large shadowed areas from washing the image out.");

                        SliderFloatWithInput("High Percent##agx", &agxSettings->AutoExposureHighPercent, 0.0f, 1.0f, "%.2f");
                        QtUi::SetItemTooltip("Cumulative point the metering stops at. Lowering it stops a lamp or a bright specular hit from crushing the image.");

                        if (agxSettings->AutoExposureHighPercent < agxSettings->AutoExposureLowPercent)
                            agxSettings->AutoExposureHighPercent = agxSettings->AutoExposureLowPercent;

                        SliderFloatWithInput("Grey Point##agx", &agxSettings->AutoExposureGreyPoint, 0.02f, 0.5f, "%.3f");
                        QtUi::SetItemTooltip("Scene luminance the metered average is exposed onto. 0.18 is photographic middle grey; lower is a darker overall image.");

                        SliderFloatWithInput("Metering Mask##agx", &agxSettings->AutoExposureMeteringMask, 0.0f, 1.0f, "%.2f");
                        QtUi::SetItemTooltip("0 meters the whole frame evenly, 1 weights the centre heavily. Centre bias stops something bright at the edge of the screen from stopping the exposure down.");

                        SliderFloatWithInput("Histogram Log Min##agx", &agxSettings->AutoExposureHistogramLogMin, -20.0f, 0.0f, "%.1f");
                        QtUi::SetItemTooltip("Darkest luminance the histogram resolves, in log2. Only needs to span the scene's real range; widening it costs resolution per bin.");

                        SliderFloatWithInput("Histogram Log Max##agx", &agxSettings->AutoExposureHistogramLogMax, 0.0f, 20.0f, "%.1f");
                        QtUi::SetItemTooltip("Brightest luminance the histogram resolves, in log2.");
                    }

                    QtUi::Separator();
                    QtUi::TextDisabled("Curve");

                    SliderFloatWithInput("Toe Strength##agx", &agxSettings->ToeStrength, 0.1f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Controls how quickly shadows roll off. 1.0 = default.");

                    SliderFloatWithInput("Shoulder Strength##agx", &agxSettings->ShoulderStrength, 0.1f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Controls how quickly highlights compress. 1.0 = default.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Color Grading");

                    drawGradeRegion("Global", agxSettings->Global);
                    drawGradeRegion("Shadows", agxSettings->Shadows);
                    drawGradeRegion("Midtones", agxSettings->Midtones);
                    drawGradeRegion("Highlights", agxSettings->Highlights);
                }
            }
        }

        if (volumetricFogSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Volumetric Fog", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##volfog"))
                {
                    *volumetricFogSettings = defaultVolumetricFogSettings;
                }

                QtUi::Checkbox("Enable Volumetric Fog", &volumetricFogSettings->Enabled);
                QtUi::SetItemTooltip("Enable froxel-based volumetric fog with sun-light injection and deferred compositing.");

                if (volumetricFogSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Froxel Grid");
                    SliderIntWithInput("Froxel Tile Size##volfog", &volumetricFogSettings->FroxelTileSize, 4, 16);
                    QtUi::SetItemTooltip("Screen-space froxel size in pixels. Lower = more detail and much higher cost. Very small values can create extremely large 3D fog volumes.");
                    SliderIntWithInput("Depth Slices##volfog", &volumetricFogSettings->DepthSlices, 8, 128);
                    QtUi::SetItemTooltip("Number of logarithmic depth slices in the froxel volume.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Medium");
                    SliderFloatWithInput("Start Distance##volfog", &volumetricFogSettings->StartDistance, 0.0f, 50.0f, "%.2f m");
                    SliderFloatWithInput("Max Distance##volfog", &volumetricFogSettings->MaxDistance, 1.0f, 500.0f, "%.1f m", 1.0f, 10.0f);
                    if (volumetricFogSettings->MaxDistance <= volumetricFogSettings->StartDistance)
                        volumetricFogSettings->MaxDistance = volumetricFogSettings->StartDistance + 0.001f;
                    SliderFloatWithInput("Density##volfog", &volumetricFogSettings->Density, 0.0f, 1.0f, "%.4f", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("Extinction per metre: a ray of length d keeps exp(-Density * d). Interiors need far more than outdoor haze - a 10 m room at 0.01 is only 10% attenuated.");
                    SliderFloatWithInput("Scattering Albedo##volfog", &volumetricFogSettings->ScatteringAlbedo, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("How much of what the fog blocks it scatters back out rather than absorbing. 1 is smoke-free haze; lower values darken unlit fog toward soot.");
                    SliderFloatWithInput("Anisotropy##volfog", &volumetricFogSettings->Anisotropy, -0.95f, 0.95f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Positive values bias forward scattering for stronger god rays.");
                    SliderFloatWithInput("GI Intensity##volfog", &volumetricFogSettings->GiIntensity, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("Indirect light the fog picks up from the running GI: the probe grid with Probe GI, RTGI's or voxel GI's\n"
                                         "light otherwise. Often the only thing lighting fog in an interior with no sky.");
                    QtUi::Checkbox("World-Space GI##volfog", &volumetricFogSettings->GiVolume);
                    QtUi::SetItemTooltip("With RTGI or voxel GI, gather their light into a world-space volume around the camera and light\n"
                                         "the fog from it: the air between surfaces takes the light of the surfaces around it, and what the\n"
                                         "camera turned away from is remembered. Off reads the GI of the surface behind each fog cell.");
                    if (volumetricFogSettings->GiVolume)
                    {
                        SliderFloatWithInput("GI Cell Size##volfog", &volumetricFogSettings->GiVolumeCellSize, 0.1f, 4.0f, "%.2f m", 0.05f, 0.25f);
                        QtUi::SetItemTooltip("Size of the world-space GI volume's cells. The volume is 128 x 128 x 48 cells, so this also sets\n"
                                             "its reach; beyond it the fog reads the screen-space GI.");
                    }
                    SliderFloatWithInput("Base Height##volfog", &volumetricFogSettings->BaseHeight, -100.0f, 1000.0f, "%.2f m", 0.1f, 1.0f);
                    SliderFloatWithInput("Height Falloff##volfog", &volumetricFogSettings->HeightFalloff, 0.0f, 2.0f, "%.3f", 0.001f, 0.01f);

                    float fogColor[3] =
                    {
                        volumetricFogSettings->ColorR,
                        volumetricFogSettings->ColorG,
                        volumetricFogSettings->ColorB
                    };
                    if (QtUi::ColorEdit3("Fog Color", fogColor, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
                    {
                        volumetricFogSettings->ColorR = fogColor[0];
                        volumetricFogSettings->ColorG = fogColor[1];
                        volumetricFogSettings->ColorB = fogColor[2];
                    }

                    float fogEmissiveColor[3] =
                    {
                        volumetricFogSettings->EmissiveColorR,
                        volumetricFogSettings->EmissiveColorG,
                        volumetricFogSettings->EmissiveColorB
                    };
                    if (QtUi::ColorEdit3("Emissive Color##volfog", fogEmissiveColor, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
                    {
                        volumetricFogSettings->EmissiveColorR = fogEmissiveColor[0];
                        volumetricFogSettings->EmissiveColorG = fogEmissiveColor[1];
                        volumetricFogSettings->EmissiveColorB = fogEmissiveColor[2];
                    }
                    SliderFloatWithInput("Emissive Intensity##volfog", &volumetricFogSettings->EmissiveIntensity, 0.0f, 50.0f, "%.2f", 0.1f, 1.0f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const auto& fogDebugModes = DebugViewNames::VolumetricFog;
                    QtUi::Combo("Debug View##volfog", &volumetricFogSettings->DebugView, fogDebugModes, std::size(fogDebugModes));
                }
            }
        }

        if (volumetricCloudSettings != nullptr)
        {
            const VolumetricCloudSettings defaultVolumetricCloudSettings{};
            if (QtUi::CollapsingHeader("Volumetric Clouds", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##volclouds"))
                {
                    *volumetricCloudSettings = defaultVolumetricCloudSettings;
                }

                QtUi::Checkbox("Enable Volumetric Clouds", &volumetricCloudSettings->Enabled);
                QtUi::SetItemTooltip("Raymarched cloud layer with multiple-scattering, self-shadowing and temporal upsampling.");

                if (volumetricCloudSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Presets");
                    if (QtUi::Button("Clear##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::ClearSkies);
                    QtUi::SameLine();
                    if (QtUi::Button("Scattered##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::ScatteredCumulus);
                    QtUi::SameLine();
                    if (QtUi::Button("Overcast##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::Overcast);
                    QtUi::SameLine();
                    if (QtUi::Button("Stormy##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::Stormy);
                    QtUi::SetItemTooltip("Presets only touch the weather sliders; lighting and performance tuning is preserved.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Layer");
                    SliderFloatWithInput("Base Altitude##volclouds", &volumetricCloudSettings->LayerBottomMeters, 100.0f, 8000.0f, "%.0f m", 10.0f, 100.0f);
                    QtUi::SetItemTooltip("Height of the cloud base above ground level.");
                    SliderFloatWithInput("Layer Thickness##volclouds", &volumetricCloudSettings->LayerThicknessMeters, 200.0f, 12000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Planet Radius##volclouds", &volumetricCloudSettings->PlanetRadiusKm, 100.0f, 20000.0f, "%.0f km", 10.0f, 100.0f);
                    QtUi::SetItemTooltip("Curvature of the layer. Smaller values pull the horizon in and exaggerate the curve.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Weather");
                    SliderFloatWithInput("Coverage##volclouds", &volumetricCloudSettings->Coverage, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Threshold applied to the weather map. 0 clears the sky, 1 fills it completely.");
                    SliderFloatWithInput("Cloud Type##volclouds", &volumetricCloudSettings->CloudType, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("0 = flat stratus, 0.5 = cumulus, 1 = towering cumulonimbus. Blended with the weather map.");
                    SliderFloatWithInput("Density##volclouds", &volumetricCloudSettings->Density, 0.0f, 2.0f, "%.3f", 0.01f, 0.1f);
                    SliderFloatWithInput("Anvil Bias##volclouds", &volumetricCloudSettings->AnvilBias, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Spreads the tops of tall clouds outwards into anvil shapes.");
                    SliderFloatWithInput("Weather Scale##volclouds", &volumetricCloudSettings->WeatherScaleMeters, 5000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    QtUi::SetItemTooltip("World size of one tile of the weather map, i.e. the size of the storm systems.");

                    QtUi::TextDisabled("Weather Map (re-bakes on change)");
                    SliderIntWithInput("Seed##volclouds", &volumetricCloudSettings->WeatherSeed, 0, 9999);
                    SliderFloatWithInput("Cell Size##volclouds", &volumetricCloudSettings->WeatherCellSize, 0.25f, 6.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Coverage Bias##volclouds", &volumetricCloudSettings->WeatherCoverageBias, -1.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Type Bias##volclouds", &volumetricCloudSettings->WeatherTypeBias, -1.0f, 1.0f, "%.2f", 0.01f, 0.1f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Shape Detail");
                    SliderFloatWithInput("Base Noise Scale##volclouds", &volumetricCloudSettings->BaseNoiseScaleMeters, 2000.0f, 120000.0f, "%.0f m", 100.0f, 1000.0f);
                    SliderFloatWithInput("Detail Noise Scale##volclouds", &volumetricCloudSettings->DetailNoiseScaleMeters, 100.0f, 10000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Detail Strength##volclouds", &volumetricCloudSettings->DetailStrength, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("How aggressively high frequency noise erodes the cloud silhouette.");
                    SliderFloatWithInput("Curl Strength##volclouds", &volumetricCloudSettings->CurlStrength, 0.0f, 3.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Swirls the erosion lookup so cloud edges read as turbulence.");
                    SliderFloatWithInput("Top Lean##volclouds", &volumetricCloudSettings->CloudTopOffsetMeters, -2000.0f, 2000.0f, "%.0f m", 10.0f, 100.0f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Wind");
                    SliderFloatWithInput("Direction##volclouds", &volumetricCloudSettings->WindDirectionDegrees, 0.0f, 360.0f, "%.1f deg", 1.0f, 10.0f);
                    SliderFloatWithInput("Speed##volclouds", &volumetricCloudSettings->WindSpeed, 0.0f, 80.0f, "%.1f m/s", 0.5f, 5.0f);
                    SliderFloatWithInput("Shear##volclouds", &volumetricCloudSettings->WindSkew, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Horizontal offset between the base and the top of the layer, as a fraction of its thickness.");
                    SliderFloatWithInput("Detail Speed Scale##volclouds", &volumetricCloudSettings->DetailWindSpeedScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("How much faster the fine detail advects than the base shape. This is what makes cloud interiors churn.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Lighting");
                    float scatteringColor[3] =
                    {
                        volumetricCloudSettings->ScatteringColorR,
                        volumetricCloudSettings->ScatteringColorG,
                        volumetricCloudSettings->ScatteringColorB
                    };
                    if (QtUi::ColorEdit3("Scattering Albedo##volclouds", scatteringColor, QtUiColorEditFlags_Float))
                    {
                        volumetricCloudSettings->ScatteringColorR = scatteringColor[0];
                        volumetricCloudSettings->ScatteringColorG = scatteringColor[1];
                        volumetricCloudSettings->ScatteringColorB = scatteringColor[2];
                    }
                    SliderFloatWithInput("Extinction##volclouds", &volumetricCloudSettings->ExtinctionScale, 0.001f, 0.3f, "%.4f /m", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("Extinction per metre at density 1. Real cumulus sit around 0.04 - 0.1.");
                    SliderFloatWithInput("Sun Intensity##volclouds", &volumetricCloudSettings->SunIntensityScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Ambient Intensity##volclouds", &volumetricCloudSettings->AmbientIntensityScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Ground Bounce##volclouds", &volumetricCloudSettings->GroundBounceScale, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("How much sky light still reaches the shadowed base of the layer.");
                    SliderFloatWithInput("Forward Lobe (g0)##volclouds", &volumetricCloudSettings->PhaseG0, -0.99f, 0.99f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Drives the bright silver lining when looking towards the sun.");
                    SliderFloatWithInput("Back Lobe (g1)##volclouds", &volumetricCloudSettings->PhaseG1, -0.99f, 0.99f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Lobe Blend##volclouds", &volumetricCloudSettings->PhaseBlend, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Powder##volclouds", &volumetricCloudSettings->PowderStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Darkens cloud faces that turn towards the light, which restores the depth single scattering loses.");

                    QtUi::TextDisabled("Multiple Scattering");
                    SliderIntWithInput("Octaves##volclouds", &volumetricCloudSettings->MultiScatterOctaves, 1, 4);
                    QtUi::SetItemTooltip("Wrenninge octave approximation. 1 is single scattering only; each extra octave recovers more of the light real clouds bounce internally.");
                    SliderFloatWithInput("Scatter Falloff##volclouds", &volumetricCloudSettings->MsScatterFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Extinction Falloff##volclouds", &volumetricCloudSettings->MsExtinctionFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Phase Falloff##volclouds", &volumetricCloudSettings->MsPhaseFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Quality And Performance");
                    const char* cloudResolutionModes[] = { "Full", "Half", "Third", "Quarter" };
                    int resolutionIndex = std::clamp(volumetricCloudSettings->ResolutionDivisor, 1, 4) - 1;
                    if (QtUi::Combo("Trace Resolution##volclouds", &resolutionIndex, cloudResolutionModes, std::size(cloudResolutionModes)))
                        volumetricCloudSettings->ResolutionDivisor = resolutionIndex + 1;
                    QtUi::SetItemTooltip("Resolution the raymarch runs at before temporal reconstruction. Half is the usual shipping setting.");

                    QtUi::Checkbox("Temporal Upsampling##volclouds", &volumetricCloudSettings->TemporalUpsampling);
                    QtUi::SetItemTooltip("Blends with the reprojected previous frame. Turn off to see the raw trace.");
                    if (volumetricCloudSettings->TemporalUpsampling)
                    {
                        SliderFloatWithInput("Temporal Blend##volclouds", &volumetricCloudSettings->TemporalBlend, 0.0f, 0.98f, "%.3f", 0.005f, 0.05f);
                        QtUi::SetItemTooltip("Weight given to history. Higher is smoother but slower to react.");
                    }

                    SliderIntWithInput("View Steps##volclouds", &volumetricCloudSettings->MaxSteps, 16, 256);
                    QtUi::SetItemTooltip("Steps for a vertical traversal of the layer. Grazing rays scale up to twice this.");
                    SliderIntWithInput("Light Steps##volclouds", &volumetricCloudSettings->LightSteps, 1, 6);
                    SliderFloatWithInput("Light March Distance##volclouds", &volumetricCloudSettings->LightMarchDistanceMeters, 100.0f, 8000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Shadow Cone Spread##volclouds", &volumetricCloudSettings->ShadowConeSpread, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Widens the light march into a cone so self-shadowing picks up neighbouring cloud mass.");
                    SliderFloatWithInput("Shadow Step Growth##volclouds", &volumetricCloudSettings->ShadowStepGrowth, 1.0f, 3.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Max Trace Distance##volclouds", &volumetricCloudSettings->MaxTraceDistanceMeters, 5000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    SliderFloatWithInput("Distance Fade Start##volclouds", &volumetricCloudSettings->DistanceFadeStartMeters, 1000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    if (volumetricCloudSettings->DistanceFadeStartMeters >= volumetricCloudSettings->MaxTraceDistanceMeters)
                        volumetricCloudSettings->DistanceFadeStartMeters = volumetricCloudSettings->MaxTraceDistanceMeters - 1.0f;
                    SliderFloatWithInput("Detail Fade Distance##volclouds", &volumetricCloudSettings->DetailFadeDistanceMeters, 1000.0f, 200000.0f, "%.0f m", 500.0f, 5000.0f);
                    QtUi::SetItemTooltip("Beyond this, high frequency erosion is skipped. Lower values are much cheaper and reduce distant shimmer.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const auto& cloudDebugModes = DebugViewNames::VolumetricClouds;
                    QtUi::Combo("Debug View##volclouds", &volumetricCloudSettings->DebugView, cloudDebugModes, std::size(cloudDebugModes));
                }
            }
        }

        if (bloomSettings != nullptr)
        {
            const BloomSettings defaultBloomSettings{};
            if (QtUi::CollapsingHeader("Bloom", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##bloom"))
                    *bloomSettings = defaultBloomSettings;

                QtUi::Checkbox("Enable Bloom", &bloomSettings->Enabled);

                if (bloomSettings->Enabled)
                {
                    const char* bloomMethods[] = { "Mip Chain", "FFT Convolution" };
                    int bloomMethod = static_cast<int>(bloomSettings->Method);
                    if (QtUi::Combo("Method##bloom", &bloomMethod, bloomMethods, static_cast<int>(std::size(bloomMethods))))
                        bloomSettings->Method = static_cast<BloomMethod>(bloomMethod);
                    QtUi::SetItemTooltip("Mip Chain: the cheap downsample/upsample glow. FFT Convolution: every bright pixel is convolved with a full-frame glare kernel (sharp core, power-law halo, aperture diffraction spikes) in the frequency domain; costs more, looks like a real lens.");

                    QtUi::Separator();
                    SliderFloatWithInput("Intensity##bloom",  &bloomSettings->Intensity,  0.0f,  1.0f,  "%.3f", 0.005f, 0.05f);
                    SliderFloatWithInput("Threshold##bloom",  &bloomSettings->Threshold,  0.0f,  10.0f, "%.2f");
                    SliderFloatWithInput("Knee##bloom",       &bloomSettings->Knee,       0.0f,  2.0f,  "%.2f");

                    if (bloomSettings->Method == BloomMethod::MipChain)
                    {
                        SliderFloatWithInput("Radius##bloom",     &bloomSettings->Radius,     0.1f,  4.0f,  "%.2f");
                        SliderIntWithInput("Mip Levels##bloom",   &bloomSettings->MipLevels,  2,     8);
                    }
                    else
                    {
                        const char* fftSizes[] = { "256", "512", "1024" };
                        int fftSizeIndex = bloomSettings->FftResolution <= 256 ? 0 : (bloomSettings->FftResolution <= 512 ? 1 : 2);
                        if (QtUi::Combo("FFT Resolution##bloom", &fftSizeIndex, fftSizes, static_cast<int>(std::size(fftSizes))))
                            bloomSettings->FftResolution = 256 << fftSizeIndex;
                        QtUi::SetItemTooltip("Side of the FFT grid. The frame is convolved at half of it (the rest is padding), so 512 blooms a 256-pixel-wide copy. Higher keeps small highlights sharper but costs about 4x per step.");
                        SliderFloatWithInput("Kernel Size##bloom", &bloomSettings->FftKernelSize, 0.05f, 1.0f, "%.2f", 0.01f, 0.1f);
                        QtUi::SetItemTooltip("How far the glare reaches, as a fraction of the frame width.");
                        SliderFloatWithInput("Halo Strength##bloom", &bloomSettings->FftHaloStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                        QtUi::SetItemTooltip("Share of the glare's energy in the wide halo; the rest stays in the sharp core.");
                        SliderFloatWithInput("Halo Falloff##bloom", &bloomSettings->FftHaloFalloff, 0.5f, 6.0f, "%.2f", 0.05f, 0.25f);
                        QtUi::SetItemTooltip("Power-law exponent of the halo. Higher = tighter.");
                        SliderFloatWithInput("Streak Strength##bloom", &bloomSettings->FftStreakStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                        QtUi::SetItemTooltip("Aperture diffraction spikes baked into the kernel.");
                        SliderIntWithInput("Aperture Blades##bloom", &bloomSettings->FftApertureBlades, 3, 16);
                        QtUi::SetItemTooltip("An even blade count gives that many spikes, an odd one twice as many.");
                        SliderFloatWithInput("Aperture Rotation##bloom", &bloomSettings->FftApertureRotation, 0.0f, 180.0f, "%.1f deg", 0.5f, 5.0f);
                        SliderFloatWithInput("Chromatic Spread##bloom", &bloomSettings->FftChromaticSpread, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                        QtUi::SetItemTooltip("Red glare reaches further than blue, as it does through a real lens.");
                    }
                }
            }
        }

        if (lensFlareSettings != nullptr)
        {
            const LensFlareSettings defaultLensFlareSettings{};
            if (QtUi::CollapsingHeader("Lens Flares", QtUiTreeNodeFlags_DefaultOpen))
            {
                LensFlareSettings& flare = *lensFlareSettings;
                if (QtUi::Button("Revert All##lensflare"))
                    flare = defaultLensFlareSettings;

                QtUi::Checkbox("Enable Lens Flares", &flare.Enabled);
                QtUi::SetItemTooltip("Physically based ghosts, ray traced every frame through a real lens prescription (Data/LensFlares/Lenses), plus an aperture-diffraction starburst, for the sun and the brightest visible level lights.");

                if (flare.Enabled)
                {
                    if (QtUi::BeginCombo("Lens##lensflare", flare.Lens.c_str()))
                    {
                        for (const std::string& lensName : LensFlareOptics::ListLenses())
                        {
                            if (QtUi::Selectable(lensName.c_str(), lensName == flare.Lens))
                                flare.Lens = lensName;
                        }
                        QtUi::EndCombo();
                    }
                    if (lensFlareRenderer != nullptr)
                    {
                        if (const char* lensError = lensFlareRenderer->GetLensError())
                            QtUi::TextWrapped("%s", lensError);
                        else if (lensFlareRenderer->IsInitialized())
                            QtUi::TextDisabled("%d ghosts per light, %d light(s) flaring",
                                lensFlareRenderer->GetGhostCount(), lensFlareRenderer->GetActiveLightCount());
                        else
                            QtUi::TextWrapped("Lens flares failed to start: %s",
                                lensFlareRenderer->GetLastErrorMessage() ? lensFlareRenderer->GetLastErrorMessage() : "unknown error");
                    }

                    QtUi::Separator();
                    SliderFloatWithInput("Intensity##lensflare", &flare.Intensity, 0.0f, 10.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Scales ghosts and starbursts together.");
                    SliderFloatWithInput("Ghost Intensity##lensflare", &flare.GhostIntensity, 0.0f, 2000.0f, "%.0f", 1.0f, 25.0f);
                    QtUi::SetItemTooltip("1 is the energy a real multi-coated lens reflects into its ghosts, which is very faint; games usually exaggerate it.");
                    SliderFloatWithInput("F-Number##lensflare", &flare.FNumber, 0.0f, 32.0f, "%.1f", 0.1f, 1.0f);
                    QtUi::SetItemTooltip("0 uses the lens file's own f-number. Stopping down makes the ghosts smaller and more clearly iris-shaped. The nikon-zoom lenses pass almost no light off axis at their stock f/22: open them up to f/8 or wider.");
                    SliderIntWithInput("Aperture Blades##lensflare", &flare.ApertureBlades, 3, 16);
                    SliderFloatWithInput("Aperture Rotation##lensflare", &flare.ApertureRotation, 0.0f, 180.0f, "%.1f deg", 0.5f, 5.0f);
                    SliderFloatWithInput("Aperture Roundness##lensflare", &flare.ApertureRoundness, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderIntWithInput("Max Ghosts##lensflare", &flare.MaxGhosts, 0, 256);
                    QtUi::SetItemTooltip("The brightest ghosts of the lens are drawn, ranked once when the lens loads. Zoom lenses have hundreds.");
                    SliderIntWithInput("Ray Grid##lensflare", &flare.RayGridSize, 8, 64);
                    QtUi::SetItemTooltip("Rays per side of each ghost's grid. Higher resolves sharper caustics and costs quadratically.");
                    SliderIntWithInput("Wavelengths##lensflare", &flare.Wavelengths, 1, 6);
                    QtUi::SetItemTooltip("Wavelengths traced per ghost. 1 is monochrome; 3 and up give the coloured fringes from dispersion.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Starburst");
                    SliderFloatWithInput("Starburst Intensity##lensflare", &flare.StarburstIntensity, 0.0f, 10.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Starburst Size##lensflare", &flare.StarburstSize, 0.02f, 1.5f, "%.2f", 0.01f, 0.05f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Light Sources");
                    QtUi::Checkbox("Sun##lensflare", &flare.SunFlares);
                    QtUi::Checkbox("Level Lights##lensflare", &flare.LocalLightFlares);
                    QtUi::SetItemTooltip("Point, spot and rect lights placed in the level, and particle lights.");
                    SliderIntWithInput("Max Lights##lensflare", &flare.MaxLights, 1, 8);
                    QtUi::SetItemTooltip("Most lights flaring at once, the sun included. The brightest visible ones win.");
                    SliderFloatWithInput("Light Threshold##lensflare", &flare.LocalLightThreshold, 0.0f, 1.0f, "%.4f", 0.0005f, 0.01f);
                    QtUi::SetItemTooltip("A level light flares only when the light it throws on the camera is at least this bright (the noon sun is about 1).");
                    SliderFloatWithInput("Occlusion Tolerance##lensflare", &flare.OcclusionDepthTolerance, 0.0f, 2.0f, "%.2f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("A light this far behind the surface in front of it still counts as visible, so a bulb inside its own lamp mesh still flares.");
                    QtUi::Checkbox("Clouds Dim Sun Flare##lensflare", &flare.SunCloudOcclusion);
                    QtUi::SetItemTooltip("Dims the sun's flare when its disc is darker than expected - behind clouds or in fog.");
                }
            }
        }

        if (virtualShadowMapSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Virtual Shadow Maps", QtUiTreeNodeFlags_DefaultOpen))
            {
                VirtualShadowMapSettings& vsm = *virtualShadowMapSettings;
                if (QtUi::Button("Revert All##vsm"))
                    vsm = defaultVirtualShadowMapSettings;

                QtUi::Checkbox("Enabled##vsm", &vsm.Enabled);
                QtUi::SetItemTooltip("Shadow the sun with a virtual shadow map: a camera-centred clipmap of 16K pages, rendered only where the screen needs them and cached between frames. Off falls back to the single 2K sun shadow map and the point-light cubemaps.");
                QtUi::Checkbox("Local Lights##vsm", &vsm.LocalLights);
                QtUi::SetItemTooltip("Shadow point, spot and rect lights through the map as well: a paged, cached 4096x4096 cube per light (up to 16), instead of the cubemap atlas (4 lights).");
                if (virtualShadowMapStatus != nullptr && virtualShadowMapStatus[0] != '\0')
                    QtUi::TextWrapped("%s", virtualShadowMapStatus);

                QtUi::SeparatorText("Soft Shadows");
                QtUi::Checkbox("Soft Shadows##vsm", &vsm.SoftShadows);
                QtUi::SetItemTooltip(
                    "Penumbrae that widen with distance from the caster, from the size of the light (PCSS):\n"
                    "sharp where an object touches the ground, softer further out. For the sun and for local lights.\n"
                    "Off = the fixed 4x4 filter.");
                if (vsm.SoftShadows)
                {
                    SliderFloatWithInput("Sun Angular Size (deg)##vsm", &vsm.SunAngularDiameter, 0.0f, 10.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Apparent diameter of the sun. The real sun is about 0.53 degrees; larger is a softer, hazier sun. 0 = hard.");
                    SliderFloatWithInput("Light Softness##vsm", &vsm.LocalLightSoftness, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("Multiplies every local light's emitter size (its Source Radius, or a rect light's extents) for its shadow.");
                    SliderFloatWithInput("Min Light Size (m)##vsm", &vsm.MinLocalLightSize, 0.0f, 0.5f, "%.3f", 0.005f, 0.02f);
                    QtUi::SetItemTooltip("Emitter radius assumed for lights smaller than this, so plain point lights soften with distance too. 0 keeps them hard.");
                    SliderFloatWithInput("Max Penumbra (m)##vsm", &vsm.MaxPenumbra, 0.05f, 4.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Widest penumbra kept. Bounds the search for blockers; very wide penumbrae read coarser shadow data.");
                }
                QtUi::Separator();

                SliderFloatWithInput("Resolution Bias##vsm", &vsm.ResolutionLodBias, -2.0f, 3.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Shifts the sun clipmap level each pixel reads. +1 halves the shadow resolution and quarters the pages it needs; -1 doubles it.");

                SliderFloatWithInput("Light Resolution Bias##vsm", &vsm.LocalResolutionBias, -2.0f, 3.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("The same for the local lights' cube mips.");

                QtUi::SeparatorText("Light Shadow Budget");
                QtUi::Checkbox("Shadow Budget##vsm", &vsm.ShadowBudget);
                QtUi::SetItemTooltip(
                    "Only the lights that matter most to the view (brightness over distance, nothing off screen) cast\n"
                    "shadows; the rest light unshadowed. Lights entering or leaving the set fade over a moment.\n"
                    "Off = every shadow-casting light casts (up to 16).");
                if (vsm.ShadowBudget)
                {
                    SliderIntWithInput("Max Shadowed Lights##vsm", &vsm.MaxShadowedLights, 1, 16);
                    QtUi::SetItemTooltip("How many lights cast shadows at once.");
                    SliderFloatWithInput("Fade Time##vsm", &vsm.ShadowFadeSeconds, 0.0f, 2.0f, "%.2f s", 0.05f, 0.1f);
                    QtUi::SetItemTooltip("How long a shadow takes to fade in or out when its light enters or leaves the budget.");
                }
                QtUi::Checkbox("Resolution by Importance##vsm", &vsm.ImportanceResolution);
                QtUi::SetItemTooltip(
                    "Lights past the most important few read a coarser shadow cube: softer shadows, fewer pages.\n"
                    "Off = every light at full resolution.");
                if (vsm.ImportanceResolution)
                {
                    SliderIntWithInput("Full Resolution Lights##vsm", &vsm.FullResolutionLights, 0, 16);
                    QtUi::SetItemTooltip("How many of the most important lights keep full shadow resolution.");
                    SliderFloatWithInput("Reduced Light Bias##vsm", &vsm.ReducedResolutionBias, 0.0f, 3.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("Mip bias for the other lights; +1 halves their shadow resolution.");
                }
                QtUi::Checkbox("Cheap Distant Filtering##vsm", &vsm.CheapDistantFiltering);
                QtUi::SetItemTooltip(
                    "A 2x2 shadow filter instead of 4x4 for receivers far from the camera, and everywhere for the\n"
                    "reduced-resolution lights. A quarter of the taps; slightly harder edges far away.\n"
                    "Off = 4x4 everywhere.");
                if (vsm.CheapDistantFiltering)
                {
                    SliderFloatWithInput("Cheap Filter Distance##vsm", &vsm.CheapFilterDistance, 0.0f, 200.0f, "%.1f m", 0.5f, 5.0f);
                    QtUi::SetItemTooltip("Camera distance beyond which the 2x2 filter is used.");
                }
                QtUi::Separator();

                SliderFloatWithInput("First Level Texel (m)##vsm", &vsm.FirstLevelTexelSize, 0.0005f, 0.05f, "%.4f", 0.0005f, 0.001f);
                QtUi::SetItemTooltip("Texel size of the finest clipmap level, in metres. Each further level doubles it. Changing it drops the cache.");

                SliderIntWithInput("Levels##vsm", &vsm.LevelCount, 1, 16);
                QtUi::SetItemTooltip("Clipmap levels in use. Beyond the last one, surfaces receive no sun shadow.");

                SliderIntWithInput("Pool Pages##vsm", &vsm.PhysicalPages, 256, 4096);
                QtUi::SetItemTooltip("Physical 128x128 pages backing the map (2048 = 128 MB). Changing it rebuilds the pool.");

                SliderIntWithInput("Pages Per Frame##vsm", &vsm.MaxPagesPerFrame, 8, 240);
                QtUi::SetItemTooltip("Most pages rendered in one frame. Pages over the budget wait a frame, with coarser levels standing in.");

                SliderFloatWithInput("Normal Offset##vsm", &vsm.NormalOffset, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Receiver offset along the normal, in texels of the level being read. Fights acne on curved surfaces.");

                SliderFloatWithInput("Depth Bias##vsm", &vsm.ConstantBias, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Receiver depth bias, in texels of the level being read.");

                SliderFloatWithInput("Slope Scaled Bias##vsm", &vsm.SlopeScaledDepthBias, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Rasterizer slope bias for the page render. Changing it drops the cache.");

                SliderFloatWithInput("Rotation Threshold (deg)##vsm", &vsm.LightRotationThreshold, 0.0f, 2.0f, "%.3f", 0.01f, 0.05f);
                QtUi::SetItemTooltip("A sun rotation smaller than this keeps the cached pages. Every step past it re-renders every page, spread over frames by the page budget.");

                QtUi::Checkbox("Terrain Shadows##vsm", &vsm.TerrainShadows);
                QtUi::SetItemTooltip("Terrain casts into the map: dunes and hills shadow the scene and themselves.");
                if (!vsm.TerrainShadows)
                    QtUi::BeginDisabled();
                QtUi::Checkbox("Displaced Terrain Shadows##vsm", &vsm.TerrainDisplacementShadows);
                QtUi::SetItemTooltip("Cast the terrain tessellated and displaced as it is drawn, so ripples and small dunes shadow too.\nExpensive: every shadow page tessellates the terrain under it. Off casts the plain mesh.");
                if (!vsm.TerrainShadows)
                    QtUi::EndDisabled();

                QtUi::Checkbox("Disable Caching##vsm", &vsm.DisableCaching);
                QtUi::SetItemTooltip("Re-render every page in use every frame, for comparison with the cache.");

                const auto& vsmDebugModes = DebugViewNames::VirtualShadowMap;
                QtUi::Combo("Debug View##vsm", &vsm.DebugView, vsmDebugModes, std::size(vsmDebugModes));
                QtUi::SetItemTooltip("Clipmap Level tints each pixel by the level that shadows it (magenta = no page resident yet).");
            }
        }

        if (rtShadowSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Ray-Traced Shadows"))
            {
                RtShadowSettings& rts = *rtShadowSettings;
                const RtShadowSettings defaultRtShadowSettings{};
                if (QtUi::Button("Revert All##rtshadow"))
                    rts = defaultRtShadowSettings;

                QtUi::Checkbox("Enabled##rtshadow", &rts.Enabled);
                QtUi::SetItemTooltip("Shadow the sun with DXR rays against the scene instead of the shadow map: exact contact shadows and penumbrae from the sun's real size. Needs a ray-tracing GPU; the shadow maps still render for fog, GI and subsurface.");
                QtUi::Checkbox("Local Lights##rtshadow", &rts.LocalLights);
                QtUi::SetItemTooltip("Ray trace point, spot and rect lights that cast shadows too - up to 8 per pixel; any further lights on a pixel keep their shadow maps.");

                if (rts.Enabled)
                {
                    SliderIntWithInput("Rays Per Pixel##rtshadow", &rts.RaysPerPixel, 1, 8);
                    QtUi::SetItemTooltip("Rays per pixel per light each frame. The sun is denoised, so 1 is usually enough; local light penumbrae are only smoothed by TAA, so they gain the most from more.");
                    SliderFloatWithInput("Sun Angular Size (deg)##rtshadow", &rts.SunAngularDiameter, 0.0f, 10.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Apparent diameter of the sun. The real sun is about 0.53 degrees; larger is softer. 0 = hard.");
                    SliderFloatWithInput("Light Softness##rtshadow", &rts.LocalLightSoftness, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("Multiplies every local light's emitter size (its Source Radius, or a rect light's panel, at most 1) for its shadow. 0 = hard.");
                    SliderFloatWithInput("Max Ray Length (m)##rtshadow", &rts.MaxRayLength, 1.0f, 2000.0f, "%.0f", 1.0f, 10.0f);
                    QtUi::SetItemTooltip("Sun casters further away than this cast nothing.");
                    SliderFloatWithInput("Ray Bias (m)##rtshadow", &rts.RayBias, 0.0001f, 0.1f, "%.4f", 0.0005f, 0.005f);
                    QtUi::SetItemTooltip("Offset of the ray origin off the surface. Raise it if flat surfaces show dark speckles; lower it if shadows detach from their casters.");
                    SliderFloatWithInput("Temporal Blend##rtshadow", &rts.TemporalBlend, 0.02f, 1.0f, "%.2f", 0.01f, 0.05f);
                    QtUi::SetItemTooltip("Weight of the current frame in the sun shadow history. Lower = smoother penumbrae, more lag behind moving casters.");
                    SliderIntWithInput("Max Filter Radius (px)##rtshadow", &rts.MaxFilterRadius, 0, 16);
                    QtUi::SetItemTooltip("Widest blur across a sun penumbra, in pixels.");
                    const char* rtShadowDebugModes[] = { "Lighting", "Sun Visibility", "Raw Sun Rays", "Local Light Visibility", "Raw Local Light Rays" };
                    QtUi::Combo("Debug View##rtshadow", &rts.DebugView, rtShadowDebugModes, std::size(rtShadowDebugModes));
                }
            }
        }

        if (pointShadowSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Point Light Shadows", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##pointshadows"))
                    *pointShadowSettings = defaultPointShadowSettings;

                SliderIntWithInput("Map Size##pointshadows", &pointShadowSettings->MapSize, 256, 4096);
                QtUi::SetItemTooltip("Resolution of each point-light shadow face. Higher values improve detail but cost more memory and rendering time.");

                SliderFloatWithInput("Shadow Bias##pointshadows", &pointShadowSettings->Bias, 0.0001f, 0.1f, "%.4f", 0.0001f, 0.001f);
                QtUi::SetItemTooltip("Depth bias applied when sampling omni shadow maps to reduce self-shadow acne.");

                SliderFloatWithInput("Slope Scaled Bias##pointshadows", &pointShadowSettings->SlopeScaledDepthBias, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Rasterizer slope-scaled depth bias for point-shadow depth rendering. Increases bias on steep polygons to reduce acne on curved/angled surfaces.");

                SliderFloatWithInput("Normal Offset##pointshadows", &pointShadowSettings->NormalOffset, 0.0f, 4.0f, "%.2f", 0.01f, 0.1f);
                QtUi::SetItemTooltip("Offsets shadow test position along the geometric normal before sampling point shadows to reduce acne without large peter-panning.");

                SliderIntWithInput("Filter Radius##pointshadows", &pointShadowSettings->FilterRadius, 0, 4);
                QtUi::SetItemTooltip("PCF filter radius for point-light shadows. Higher values reduce banding but cost more.");

                SliderFloatWithInput("Seam Blend##pointshadows", &pointShadowSettings->SeamBlendDistance, 0.0f, 0.25f, "%.3f", 0.001f, 0.01f);
                QtUi::SetItemTooltip("Blends point-shadow visibility across cubemap face edges to reduce seam artifacts.");

                const auto& pointShadowDebugModes = DebugViewNames::PointShadows;
                QtUi::Combo("Debug View##pointshadows", &pointShadowSettings->DebugView, pointShadowDebugModes, std::size(pointShadowDebugModes));
                QtUi::SetItemTooltip("Shows raw point-shadow visibility to diagnose sampling, seams, and filtering.");
            }
        }

        QtUi::End();
    }

    if (gShowMaterialEditorWindow)
    {
        QtUi::SetNextWindowSize(UiVec2(640.0f, 780.0f), QtUiCond_FirstUseEver);
        if (QtUi::Begin("Material Editor", &gShowMaterialEditorWindow))
        {
            // Tab bar splits the editor into single-material mode and the CryEngine-style multi-material mode.
            if (QtUi::BeginTabBar("MaterialEditorTabs"))
            {
                // ----------------------------------------------------------------
                // Tab 1: Single Material (original behaviour, unchanged)
                // ----------------------------------------------------------------
                if (QtUi::BeginTabItem("Single Material"))
                {
                    MaterialDefinition& materialDefinition = gMaterialEditor.GetCurrentMaterial();
                    if (gMaterialNameBuffer[0] == '\0')
                    {
                        RefreshMaterialNameBuffer();
                    }

                    if (QtUi::Button("New"))
                    {
                        gMaterialEditor.NewMaterial();
                        RefreshMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Open..."))
                    {
                        if (gMaterialEditor.OpenMaterialWithDialog(windowHandle))
                        {
                            RefreshMaterialNameBuffer();
                        }
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save"))
                    {
                        materialDefinition.Name = gMaterialNameBuffer;
                        gMaterialEditor.SaveCurrentMaterial();
                        RefreshMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save As..."))
                    {
                        materialDefinition.Name = gMaterialNameBuffer;
                        if (gMaterialEditor.SaveMaterialWithDialog(windowHandle))
                        {
                            RefreshMaterialNameBuffer();
                        }
                    }

                    QtUi::Separator();
                    QtUi::InputText("Material Name", gMaterialNameBuffer, std::size(gMaterialNameBuffer));
                    materialDefinition.Name = gMaterialNameBuffer;

                    // Everything below scrolls inside its own region so the file
                    // buttons and the name stay put. The window body is
                    // scroll-wrapped as a whole, so without this the Save button
                    // scrolls off the top as soon as the property list is long -
                    // and it is long.
                    QtUi::BeginChild("SingleMaterialProperties", UiVec2(0.0f, kMaterialPropertiesHeight));

                    QtUi::ColorEdit4("Base Color Tint", materialDefinition.BaseColorTint.data());
                    QtUi::ColorEdit3("Emissive Color", materialDefinition.EmissiveColor.data());
                    QtUi::SliderFloat("Metallic Factor", &materialDefinition.MetallicFactor, 0.0f, 1.0f);
                    QtUi::SliderFloat("Roughness Factor", &materialDefinition.RoughnessFactor, 0.0f, 1.0f);
                    // 0.5 is neutral: it reproduces the 0.04 dielectric reflectance every
                    // material used before this slider existed. Raising it is how a metal
                    // gets a visible reflection in a scene with nothing to mirror.
                    QtUi::SliderFloat("Specular", &materialDefinition.SpecularFactor, 0.0f, 1.0f);
                    QtUi::SliderFloat("Normal Scale", &materialDefinition.NormalScale, 0.0f, 4.0f);
                    QtUi::Checkbox("Flip Normal Green (OpenGL)", &materialDefinition.FlipNormalGreen);
                    QtUi::SetItemTooltip("Tick for OpenGL-style normal maps (green points up). The engine expects "
                                         "DirectX-style maps; an unflipped OpenGL map lights its relief from the "
                                         "wrong vertical direction.");
                    QtUi::SliderFloat("Ambient Occlusion Strength", &materialDefinition.AmbientOcclusionStrength, 0.0f, 4.0f);
                    QtUi::SliderFloat("Opacity", &materialDefinition.Opacity, 0.0f, 1.0f);
                    QtUi::SliderFloat("Alpha Cutoff", &materialDefinition.AlphaCutoff, 0.0f, 1.0f);
                    QtUi::Checkbox("Double Sided", &materialDefinition.IsDoubleSided);
                    QtUi::Checkbox("Use Alpha Cutout", &materialDefinition.UseAlphaCutout);
                    QtUi::Checkbox("Use Transparent Blend", &materialDefinition.UseTransparentBlend);
                    QtUi::Checkbox("Decal Material", &materialDefinition.IsDecalMaterial);
                    QtUi::Separator();
                    QtUi::Checkbox("Glass Rendering", &materialDefinition.UseGlassRendering);
                    QtUi::BeginDisabled(!materialDefinition.UseGlassRendering);
                    QtUi::Checkbox("Thin Glass", &materialDefinition.UseThinGlass);
                    QtUi::SliderFloat("Glass IOR", &materialDefinition.GlassIor, 1.0f, 2.5f, "%.2f");
                    QtUi::SliderFloat("Glass Thickness", &materialDefinition.GlassThickness, 0.0f, 1.0f, "%.3f");
                    QtUi::SliderFloat("Glass Dispersion", &materialDefinition.GlassDispersion, 0.0f, 2.0f, "%.2f");
                    QtUi::SliderFloat("Fake Caustics", &materialDefinition.GlassCausticStrength, 0.0f, 1.0f, "%.2f");
                    QtUi::EndDisabled();

                    DrawMaterialPrincipledControls(materialDefinition);

                    DrawMaterialSubsurfaceControls(materialDefinition);

                    DrawMaterialEyeControls(materialDefinition);

                    DrawMaterialParticleControls(materialDefinition);

                    DrawMaterialUvAndParallaxControls(materialDefinition);

                    const auto drawTextureField = [&](const char* label, const MaterialTextureSlot textureSlot)
                    {
                        std::string& texturePath = materialDefinition.Textures.GetTexturePath(textureSlot);
                        QtUi::PushID(label);
                        QtUi::TextUnformatted(label);
                        QtUi::SameLine(180.0f);
                        QtUi::TextWrapped("%s", texturePath.empty() ? "<none>" : texturePath.c_str());
                        if (QtUi::Button("Browse..."))
                        {
                            gMaterialEditor.BrowseForTexture(windowHandle, textureSlot);
                        }
                        QtUi::SameLine();
                        if (QtUi::Button("Clear"))
                        {
                            texturePath.clear();
                        }
                        QtUi::PopID();
                    };

                    QtUi::Separator();
                    QtUi::TextUnformatted("Textures");
                    drawTextureField("Base Color", MaterialTextureSlot::BaseColor);
                    drawTextureField("Normal", MaterialTextureSlot::Normal);
                    drawTextureField("Metallic", MaterialTextureSlot::Metallic);
                    drawTextureField("Roughness", MaterialTextureSlot::Roughness);
                    drawTextureField("Metallic Roughness", MaterialTextureSlot::MetallicRoughness);
                    drawTextureField("ORM (Occlusion R, Roughness G, Metallic B)", MaterialTextureSlot::OcclusionRoughnessMetallic);
                    drawTextureField("Ambient Occlusion", MaterialTextureSlot::AmbientOcclusion);
                    drawTextureField("Emissive", MaterialTextureSlot::Emissive);
                    drawTextureField("Height / Displacement", MaterialTextureSlot::Height);
                    drawTextureField("Opacity", MaterialTextureSlot::Opacity);
                    drawTextureField("Disney Mask (Clearcoat R, Sheen G, Aniso B, Flatten A)", MaterialTextureSlot::PrincipledMask);
                    drawTextureField("Anisotropy Direction (RG)", MaterialTextureSlot::AnisotropyDirection);

                    QtUi::Separator();
                    const std::vector<std::filesystem::path> availableMaterials = gMaterialEditor.FindAvailableMaterials();
                    QtUi::TextUnformatted("Material Library");
                    if (availableMaterials.empty())
                    {
                        QtUi::TextDisabled("No saved materials found under Data/Materials.");
                    }
                    else
                    {
                        if (QtUi::BeginChild("MaterialLibrary", UiVec2(0.0f, 150.0f), true))
                        {
                            for (const std::filesystem::path& materialPath : availableMaterials)
                            {
                                const std::string itemLabel = materialPath.filename().string();
                                if (QtUi::Selectable(itemLabel.c_str(), gMaterialEditor.GetCurrentMaterialPath() == materialPath))
                                {
                                    if (gMaterialEditor.LoadMaterialFromFile(materialPath))
                                    {
                                        RefreshMaterialNameBuffer();
                                    }
                                }
                            }
                        }
                        QtUi::EndChild();
                    }

                    if (!gMaterialEditor.GetCurrentMaterialPath().empty())
                    {
                        QtUi::TextWrapped("Current File: %s", gMaterialEditor.GetCurrentMaterialPath().string().c_str());
                    }

                    DrawSelectedMeshMaterialBinding(editorInstance, "SingleMaterialBinding", gMaterialEditor.GetCurrentMaterialPath(), false);

                    QtUi::EndChild();

                    // Outside the scroll region: a save failure is worth seeing
                    // without having to hunt for it.
                    if (!gMaterialEditor.GetLastErrorMessage().empty())
                    {
                        QtUi::TextWrapped("Status: %s", gMaterialEditor.GetLastErrorMessage().c_str());
                    }

                    QtUi::EndTabItem();
                }

                // ----------------------------------------------------------------
                // Tab 2: Multi-Material  (CryEngine-style parent + sub-materials)
                // Each sub-material index == FBX material ID / mesh sub-mesh index.
                // ----------------------------------------------------------------
                if (QtUi::BeginTabItem("Multi-Material"))
                {
                    MultiMaterialDefinition& multiMat = gMaterialEditor.GetCurrentMultiMaterial();

                    if (gMultiMaterialNameBuffer[0] == '\0')
                    {
                        RefreshMultiMaterialNameBuffer();
                    }

                    // --- Toolbar ---
                    if (QtUi::Button("New##Multi"))
                    {
                        gMaterialEditor.NewMultiMaterial();
                        gSelectedSubMaterialIndex = -1;
                        RefreshMultiMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Open...##Multi"))
                    {
                        if (gMaterialEditor.OpenMultiMaterialWithDialog(windowHandle))
                        {
                            gSelectedSubMaterialIndex = -1;
                            RefreshMultiMaterialNameBuffer();
                        }
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save##Multi"))
                    {
                        multiMat.Name = gMultiMaterialNameBuffer;
                        gMaterialEditor.SaveCurrentMultiMaterial();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save As...##Multi"))
                    {
                        multiMat.Name = gMultiMaterialNameBuffer;
                        gMaterialEditor.SaveMultiMaterialWithDialog(windowHandle);
                    }

                    QtUi::Separator();

                    // Parent name field
                    QtUi::InputText("Multi-Material Name", gMultiMaterialNameBuffer, std::size(gMultiMaterialNameBuffer));
                    multiMat.Name = gMultiMaterialNameBuffer;

                    QtUi::Separator();

                    // Same as the single-material tab: scroll the sub-material
                    // editing below, keep the file buttons and name pinned.
                    QtUi::BeginChild("MultiMaterialProperties", UiVec2(0.0f, kMaterialPropertiesHeight));

                    // Two-column layout: sub-material list on the left, properties on the right
                    const float subMatListWidth = 200.0f;
                    QtUi::BeginGroup();

                    QtUi::TextUnformatted("Sub-Materials");
                    QtUi::SameLine();

                    // Add button inserts a new sub-material at the next slot
                    if (QtUi::SmallButton("+  Add"))
                    {
                        gMaterialEditor.AddSubMaterial();
                        gSelectedSubMaterialIndex = static_cast<int>(multiMat.SubMaterials.size()) - 1;
                    }

                    QtUi::SameLine();
                    QtUi::BeginDisabled(gSelectedSubMaterialIndex < 0 ||
                        gSelectedSubMaterialIndex >= static_cast<int>(multiMat.SubMaterials.size()));
                    if (QtUi::SmallButton("-  Remove"))
                    {
                        gMaterialEditor.RemoveSubMaterial(gSelectedSubMaterialIndex);
                        // Clamp selection to valid range after removal
                        const int newCount = static_cast<int>(multiMat.SubMaterials.size());
                        if (gSelectedSubMaterialIndex >= newCount)
                        {
                            gSelectedSubMaterialIndex = newCount - 1;
                        }
                    }
                    QtUi::EndDisabled();

                    // Sub-material list box – each row shows "Mat ID X – Name"
                    if (QtUi::BeginChild("SubMatList", UiVec2(subMatListWidth, 0.0f), true))
                    {
                        for (int i = 0; i < static_cast<int>(multiMat.SubMaterials.size()); ++i)
                        {
                            char label[128];
                            std::snprintf(label, sizeof(label), "ID %d – %s", i, multiMat.SubMaterials[i].Name.c_str());
                            if (QtUi::Selectable(label, gSelectedSubMaterialIndex == i))
                            {
                                gSelectedSubMaterialIndex = i;
                            }
                        }

                        if (multiMat.SubMaterials.empty())
                        {
                            QtUi::TextDisabled("No sub-materials.\nClick '+ Add'.");
                        }
                    }
                    QtUi::EndChild();

                    QtUi::EndGroup();

                    QtUi::SameLine();

                    // Right panel: properties of the selected sub-material
                    QtUi::BeginGroup();
                    if (gSelectedSubMaterialIndex >= 0 &&
                        gSelectedSubMaterialIndex < static_cast<int>(multiMat.SubMaterials.size()))
                    {
                        MaterialDefinition& subMat = multiMat.SubMaterials[gSelectedSubMaterialIndex];

                        QtUi::TextDisabled("Material ID: %d  (matches FBX material slot)", gSelectedSubMaterialIndex);

                        // Sub-material name – use a per-index buffer via InputText with a PushID
                        QtUi::PushID(gSelectedSubMaterialIndex);
                        char subNameBuf[256];
                        strcpy_s(subNameBuf, subMat.Name.c_str());
                        if (QtUi::InputText("Sub-Material Name", subNameBuf, std::size(subNameBuf)))
                        {
                            subMat.Name = subNameBuf;
                        }

                        QtUi::ColorEdit4("Base Color Tint", subMat.BaseColorTint.data());
                        QtUi::ColorEdit3("Emissive Color", subMat.EmissiveColor.data());
                        QtUi::SliderFloat("Metallic Factor", &subMat.MetallicFactor, 0.0f, 1.0f);
                        QtUi::SliderFloat("Roughness Factor", &subMat.RoughnessFactor, 0.0f, 1.0f);
                        QtUi::SliderFloat("Specular", &subMat.SpecularFactor, 0.0f, 1.0f);
                        QtUi::SliderFloat("Normal Scale", &subMat.NormalScale, 0.0f, 4.0f);
                        QtUi::Checkbox("Flip Normal Green (OpenGL)", &subMat.FlipNormalGreen);
                        QtUi::SetItemTooltip("Tick for OpenGL-style normal maps (green points up). The engine expects "
                                             "DirectX-style maps; an unflipped OpenGL map lights its relief from the "
                                             "wrong vertical direction.");
                        QtUi::SliderFloat("AO Strength", &subMat.AmbientOcclusionStrength, 0.0f, 4.0f);
                        QtUi::SliderFloat("Opacity", &subMat.Opacity, 0.0f, 1.0f);
                        QtUi::SliderFloat("Alpha Cutoff", &subMat.AlphaCutoff, 0.0f, 1.0f);
                        QtUi::Checkbox("Double Sided", &subMat.IsDoubleSided);
                        QtUi::Checkbox("Alpha Cutout", &subMat.UseAlphaCutout);
                        QtUi::Checkbox("Transparent Blend", &subMat.UseTransparentBlend);
                        QtUi::Checkbox("Decal Material", &subMat.IsDecalMaterial);
                        QtUi::Separator();
                        QtUi::Checkbox("Glass Rendering", &subMat.UseGlassRendering);
                        QtUi::BeginDisabled(!subMat.UseGlassRendering);
                        QtUi::Checkbox("Thin Glass", &subMat.UseThinGlass);
                        QtUi::SliderFloat("Glass IOR", &subMat.GlassIor, 1.0f, 2.5f, "%.2f");
                        QtUi::SliderFloat("Glass Thickness", &subMat.GlassThickness, 0.0f, 1.0f, "%.3f");
                        QtUi::SliderFloat("Glass Dispersion", &subMat.GlassDispersion, 0.0f, 2.0f, "%.2f");
                        QtUi::SliderFloat("Fake Caustics", &subMat.GlassCausticStrength, 0.0f, 1.0f, "%.2f");
                        QtUi::EndDisabled();

                        DrawMaterialPrincipledControls(subMat);

                        DrawMaterialSubsurfaceControls(subMat);

                        DrawMaterialEyeControls(subMat);

                        DrawMaterialParticleControls(subMat);

                        DrawMaterialUvAndParallaxControls(subMat);

                        const auto drawSubTexField = [&](const char* label, const MaterialTextureSlot slot)
                        {
                            std::string& path = subMat.Textures.GetTexturePath(slot);
                            QtUi::PushID(label);
                            QtUi::TextUnformatted(label);
                            QtUi::SameLine(160.0f);
                            QtUi::TextWrapped("%s", path.empty() ? "<none>" : path.c_str());
                            if (QtUi::Button("Browse..."))
                            {
                                gMaterialEditor.BrowseForSubMaterialTexture(windowHandle, gSelectedSubMaterialIndex, slot);
                            }
                            QtUi::SameLine();
                            if (QtUi::Button("Clear"))
                            {
                                path.clear();
                            }
                            QtUi::PopID();
                        };

                        QtUi::Separator();
                        QtUi::TextUnformatted("Textures");
                        drawSubTexField("Base Color", MaterialTextureSlot::BaseColor);
                        drawSubTexField("Normal", MaterialTextureSlot::Normal);
                        drawSubTexField("Metallic", MaterialTextureSlot::Metallic);
                        drawSubTexField("Roughness", MaterialTextureSlot::Roughness);
                        drawSubTexField("Metallic Roughness", MaterialTextureSlot::MetallicRoughness);
                        drawSubTexField("ORM (Occlusion R, Roughness G, Metallic B)", MaterialTextureSlot::OcclusionRoughnessMetallic);
                        drawSubTexField("Ambient Occlusion", MaterialTextureSlot::AmbientOcclusion);
                        drawSubTexField("Emissive", MaterialTextureSlot::Emissive);
                        drawSubTexField("Height / Displacement", MaterialTextureSlot::Height);
                        drawSubTexField("Opacity", MaterialTextureSlot::Opacity);
                        drawSubTexField("Disney Mask (Clearcoat R, Sheen G, Aniso B, Flatten A)", MaterialTextureSlot::PrincipledMask);
                        drawSubTexField("Anisotropy Direction (RG)", MaterialTextureSlot::AnisotropyDirection);

                        QtUi::PopID();
                    }
                    else
                    {
                        QtUi::TextDisabled("Select a sub-material from the list on the left,\nor click '+ Add' to create one.");
                    }
                    QtUi::EndGroup();

                    QtUi::Separator();

                    // Multi-material library list (Data/MultiMaterials/)
                    const std::vector<std::filesystem::path> availableMultiMats = gMaterialEditor.FindAvailableMultiMaterials();
                    QtUi::TextUnformatted("Multi-Material Library");
                    if (availableMultiMats.empty())
                    {
                        QtUi::TextDisabled("No saved multi-materials found under Data/MultiMaterials.");
                    }
                    else
                    {
                        if (QtUi::BeginChild("MultiMatLibrary", UiVec2(0.0f, 100.0f), true))
                        {
                            for (const std::filesystem::path& mmPath : availableMultiMats)
                            {
                                const std::string itemLabel = mmPath.filename().string();
                                if (QtUi::Selectable(itemLabel.c_str(), gMaterialEditor.GetCurrentMultiMaterialPath() == mmPath))
                                {
                                    if (gMaterialEditor.LoadMultiMaterialFromFile(mmPath))
                                    {
                                        gSelectedSubMaterialIndex = -1;
                                        RefreshMultiMaterialNameBuffer();
                                    }
                                }
                            }
                        }
                        QtUi::EndChild();
                    }

                    if (!gMaterialEditor.GetCurrentMultiMaterialPath().empty())
                    {
                        QtUi::TextWrapped("Current File: %s", gMaterialEditor.GetCurrentMultiMaterialPath().string().c_str());
                    }

                    DrawSelectedMeshMaterialBinding(editorInstance, "MultiMaterialBinding", gMaterialEditor.GetCurrentMultiMaterialPath(), true);

                    QtUi::EndChild();

                    if (!gMaterialEditor.GetLastErrorMessage().empty())
                    {
                        QtUi::TextWrapped("Status: %s", gMaterialEditor.GetLastErrorMessage().c_str());
                    }

                    QtUi::EndTabItem();
                }

                QtUi::EndTabBar();
            }
        }
        QtUi::End();
    }

    if (gShowAssetBrowserWindow)
    {
        RefreshAssetBrowserState();
        QtUi::Begin("Asset Browser", &gShowAssetBrowserWindow, QtUiWindowFlags_NoScrollbar | QtUiWindowFlags_NoScrollWithMouse);

        if (gOpenRenamePopup)
        {
            QtUi::OpenPopup("Rename Asset");
            gOpenRenamePopup = false;
        }

        if (gOpenNewFolderPopup)
        {
            QtUi::OpenPopup("Create Folder");
            gOpenNewFolderPopup = false;
        }

        const bool hasSelectedAsset = !gSelectedAssetRelativePath.empty();
        const bool selectedAssetIsGeometry = hasSelectedAsset && IsGeometryAssetPath(gSelectedAssetRelativePath);
        const bool canPaste = gClipboardHasValue;

        // The toolbar keeps common content-browser actions visible, similar to the asset views used by large game editors.
        QtUi::BeginDisabled(gAssetImport.Running.load());
        if (QtUi::Button("Import..."))
        {
            if (PromptForAndImportAssets(windowHandle, gSelectedFolderRelativePath))
            {
                RefreshAssetBrowserState();
            }
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        if (QtUi::Button("New Folder"))
        {
            BeginCreateFolder();
        }

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Rename"))
        {
            BeginRenameSelectedAsset();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Copy"))
        {
            CopySelectedAssetToClipboard();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!canPaste);
        if (QtUi::Button("Paste"))
        {
            PasteClipboardIntoSelectedFolder();
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Remove"))
        {
            DeleteSelectedAsset();
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!selectedAssetIsGeometry);
        if (QtUi::Button("Generate LODs"))
        {
            RegenerateSelectedGeometryLods(editor);
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!selectedAssetIsGeometry);
        if (QtUi::Button("Generate Collisions"))
        {
            GenerateSelectedGeometryCollisions();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        if (QtUi::Button("Refresh"))
        {
            InvalidateAssetBrowserDirectoryCache();
            RefreshAssetBrowserState();
        }

        QtUi::Separator();
        QtUi::TextWrapped("Data Folder: %s", gDataDirectoryDisplay.c_str());
        QtUi::TextWrapped(
            "Current Folder: %s",
            gSelectedFolderRelativePath.empty() ? "Data" : gSelectedFolderRelativePath.c_str());
        QtUi::Separator();

        if (gHasAssetActionResult)
        {
            const UiVec4 statusColor = gLastAssetActionSucceeded
                ? UiVec4(0.35f, 0.85f, 0.45f, 1.0f)
                : UiVec4(0.90f, 0.45f, 0.35f, 1.0f);
            QtUi::TextColored(statusColor, "%s", gAssetStatusMessage.c_str());
        }
        else
        {
            QtUi::TextWrapped("%s", gAssetStatusMessage.c_str());
        }

        if (QtUi::BeginPopupModal("Rename Asset", nullptr, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::InputText("New Name", gRenameBuffer, std::size(gRenameBuffer));

            if (QtUi::Button("Apply"))
            {
                if (RenameSelectedAsset())
                {
                    RefreshAssetBrowserState();
                    QtUi::CloseCurrentPopup();
                }
            }

            QtUi::SameLine();
            if (QtUi::Button("Cancel"))
            {
                QtUi::CloseCurrentPopup();
            }

            QtUi::EndPopup();
        }

        if (QtUi::BeginPopupModal("Create Folder", nullptr, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::InputText("Folder Name", gNewFolderBuffer, std::size(gNewFolderBuffer));

            if (QtUi::Button("Create"))
            {
                if (CreateFolderInSelectedFolder())
                {
                    RefreshAssetBrowserState();
                    QtUi::CloseCurrentPopup();
                }
            }

            QtUi::SameLine();
            if (QtUi::Button("Cancel"))
            {
                QtUi::CloseCurrentPopup();
            }

            QtUi::EndPopup();
        }

        // Open the import progress modal whenever a batch import starts.
        if (gAssetImport.Running.load() || gAssetImport.Done.load() > 0)
        {
            QtUi::OpenPopup("Importing Assets...");
        }

        QtUi::SetNextWindowSize(UiVec2(760.0f, 420.0f), QtUiCond_Appearing);
        QtUi::SetNextWindowSizeConstraints(UiVec2(560.0f, 260.0f), UiVec2(FLT_MAX, FLT_MAX));
        if (QtUi::BeginPopupModal("Importing Assets...", nullptr, QtUiWindowFlags_None))
        {
            const int total = gAssetImport.Total.load();
            const int done  = gAssetImport.Done.load();
            const bool running = gAssetImport.Running.load();
            const float currentFraction = running ? gAssetImport.CurrentFraction.load() : 0.0f;
            std::string currentFile;
            {
                std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                currentFile = gAssetImport.CurrentFile;
            }

            // Whole files done, plus how far the current one has got when its importer
            // reports it (video conversion does; texture and FBX imports do not).
            float fraction = (total > 0)
                ? ((static_cast<float>(done) + (std::min)(currentFraction, 1.0f)) / static_cast<float>(total))
                : 0.0f;
            if (running && total > 0)
            {
                fraction = (std::max)(fraction, 0.02f);
            }
            char progressLabel[64];
            _snprintf_s(progressLabel, _TRUNCATE, "%d / %d", done, total);
            QtUi::ProgressBar(fraction, UiVec2(-FLT_MIN, 0.0f), progressLabel);

            if (running)
            {
                if (gAssetImport.Cancel.load())
                {
                    QtUi::TextUnformatted("Cancelling...");
                }
                else if (!currentFile.empty())
                {
                    if (currentFraction > 0.0f)
                    {
                        QtUi::TextWrapped("Processing: %s (%d%%)", currentFile.c_str(), static_cast<int>(currentFraction * 100.0f));
                    }
                    else
                    {
                        QtUi::TextWrapped("Processing: %s", currentFile.c_str());
                    }
                }
                else
                {
                    QtUi::TextUnformatted("Preparing import...");
                }

                if (done == 0)
                {
                    QtUi::TextWrapped("Large 4K textures and long videos can take a while before the first file completes. Videos are converted to WebM (VP9).");
                }
            }
            else
            {
                QtUi::TextUnformatted("Import complete.");
            }

            // Scrollable log of per-file results.
            QtUi::Separator();
            const float footerHeight = QtUi::GetFrameHeightWithSpacing() + QtUi::GetStyle().ItemSpacing.y;
            QtUi::BeginChild("ImportLog", UiVec2(0.0f, -footerHeight), true);
            {
                std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                for (const std::string& line : gAssetImport.Log)
                {
                    const bool isError = line.size() >= 3 && line.substr(0, 3) == "ERR";
                    if (isError)
                    {
                        QtUi::TextColored(UiVec4(0.9f, 0.4f, 0.3f, 1.0f), "%s", line.c_str());
                    }
                    else
                    {
                        QtUi::TextColored(UiVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", line.c_str());
                    }
                }
                // Auto-scroll to the bottom.
                if (QtUi::GetScrollY() >= QtUi::GetScrollMaxY())
                {
                    QtUi::SetScrollHereY(1.0f);
                }
            }
            QtUi::EndChild();

            // Cancel stops a video conversion mid-way and skips whatever is still queued;
            // a texture or FBX already being processed finishes first.
            QtUi::BeginDisabled(!running || gAssetImport.Cancel.load());
            if (QtUi::Button("Cancel"))
            {
                gAssetImport.Cancel.store(true);
            }
            QtUi::EndDisabled();

            QtUi::SameLine();

            // Only allow closing once the thread is done.
            QtUi::BeginDisabled(running);
            if (QtUi::Button("Close"))
            {
                // Reset so the modal won't reopen next frame.
                gAssetImport.Total.store(0);
                gAssetImport.Done.store(0);
                {
                    std::lock_guard<std::mutex> lock(gAssetImport.LogMutex);
                    gAssetImport.Log.clear();
                    gAssetImport.CurrentFile.clear();
                }
                RefreshAssetBrowserState();
                QtUi::CloseCurrentPopup();
            }
            QtUi::EndDisabled();

            QtUi::EndPopup();
        }

        QtUi::Separator();

        const float folderPaneWidth = 240.0f;
        if (QtUi::BeginChild("AssetFolders", UiVec2(folderPaneWidth, 0.0f), true))
        {
            RenderFolderTreeNode({});
        }
        QtUi::EndChild();

        QtUi::SameLine();

        if (QtUi::BeginChild("AssetContents", UiVec2(0.0f, 0.0f), true))
        {
            const std::vector<AssetBrowserItem>& items = CollectFolderItems(gSelectedFolderRelativePath);
            if (items.empty())
            {
                QtUi::TextUnformatted("This folder is empty.");
            }
            else
            {
                for (const AssetBrowserItem& item : items)
                {
                    const bool itemIsVideo = !item.IsDirectory && IsVideoAssetPath(item.RelativePath);
                    const std::string itemExtension = item.IsDirectory ? std::string{}
                        : std::filesystem::path(item.RelativePath).extension().string();
                    const bool itemIsParticleEffect = _stricmp(itemExtension.c_str(), ".particle") == 0;
                    const bool itemIsSequence = _stricmp(itemExtension.c_str(), ".sequence") == 0;
                    // The icon tells the asset type; the ## suffix keeps each row's
                    // identity even if two entries would ever show the same name.
                    const std::string itemLabel = item.Name + "##" + item.RelativePath;
                    QtUi::SetNextItemIcon(GetAssetBrowserIcon(item));
                    if (QtUi::Selectable(itemLabel.c_str(), gSelectedAssetRelativePath == item.RelativePath))
                    {
                        gSelectedAssetRelativePath = item.RelativePath;
                    }

                    if (QtUi::IsItemHovered() && QtUi::IsMouseDoubleClicked(QtUiMouseButton_Left))
                    {
                        if (item.IsDirectory)
                        {
                            SelectFolder(item.RelativePath);
                        }
                        else if (itemIsVideo)
                        {
                            OpenVideoInPlayer(item.RelativePath);
                        }
                        else if (itemIsParticleEffect && editor != nullptr)
                        {
                            static_cast<Editor*>(editor)->OpenParticleEditor(item.RelativePath);
                        }
                        else if (itemIsSequence)
                        {
                            SequencerEditor::OpenSequence(GetAbsoluteDataPath(item.RelativePath).string());
                        }
                    }

                    if (QtUi::BeginPopupContextItem(item.RelativePath.c_str()))
                    {
                        gSelectedAssetRelativePath = item.RelativePath;

                        if (item.IsDirectory && QtUi::MenuItem("Open"))
                        {
                            SelectFolder(item.RelativePath);
                        }

                        if (itemIsVideo && QtUi::MenuItem("Play Video"))
                        {
                            OpenVideoInPlayer(item.RelativePath);
                        }

                        if (itemIsParticleEffect && editor != nullptr && QtUi::MenuItem("Edit Particle Effect"))
                        {
                            static_cast<Editor*>(editor)->OpenParticleEditor(item.RelativePath);
                        }

                        if (itemIsSequence && QtUi::MenuItem("Open in Sequencer"))
                        {
                            SequencerEditor::OpenSequence(GetAbsoluteDataPath(item.RelativePath).string());
                        }

                        if (item.IsDirectory && QtUi::MenuItem("Import Here..."))
                        {
                            SelectFolder(item.RelativePath);
                            PromptForAndImportAssets(windowHandle, item.RelativePath);
                            RefreshAssetBrowserState();
                        }

                        // Unreal assets dropped into Data convert in place (also offered by
                        // the "New files detected" notification when they arrive).
                        if (!item.IsDirectory && _stricmp(itemExtension.c_str(), ".uasset") == 0 &&
                            QtUi::MenuItem(gAssetImport.Running.load() ? "Import (busy)" : "Import Unreal Asset"))
                        {
                            const std::filesystem::path source = GetAbsoluteDataPath(item.RelativePath);
                            StartAssetImport({ { source.string(), NormalizeRelativeDataPath(std::filesystem::path(item.RelativePath).parent_path()) } });
                        }

                        if (QtUi::MenuItem("Rename"))
                        {
                            BeginRenameSelectedAsset();
                        }

                        if (QtUi::MenuItem("Copy"))
                        {
                            CopySelectedAssetToClipboard();
                        }

                        if (!item.IsDirectory && IsGeometryAssetPath(item.RelativePath) && QtUi::MenuItem("Generate LODs"))
                        {
                            RegenerateSelectedGeometryLods(editor);
                            RefreshAssetBrowserState();
                        }

                        if (!item.IsDirectory && IsGeometryAssetPath(item.RelativePath) && QtUi::MenuItem("Generate Collisions"))
                        {
                            GenerateSelectedGeometryCollisions();
                        }

                        if (QtUi::MenuItem("Remove"))
                        {
                            DeleteSelectedAsset();
                            RefreshAssetBrowserState();
                        }

                        QtUi::EndPopup();
                    }
                }
            }

            if (QtUi::BeginPopupContextWindow("AssetBrowserBackgroundContext", QtUiPopupFlags_NoOpenOverItems | QtUiPopupFlags_MouseButtonRight))
            {
                if (QtUi::MenuItem("Import Here..."))
                {
                    PromptForAndImportAssets(windowHandle, gSelectedFolderRelativePath);
                    RefreshAssetBrowserState();
                }

                if (QtUi::MenuItem("New Folder"))
                {
                    BeginCreateFolder();
                }

                QtUi::BeginDisabled(!canPaste);
                if (QtUi::MenuItem("Paste"))
                {
                    PasteClipboardIntoSelectedFolder();
                    RefreshAssetBrowserState();
                }
                QtUi::EndDisabled();

                QtUi::EndPopup();
            }
        }
        QtUi::EndChild();
        QtUi::End();
    }

    if (gShowAboutWindow)
    {
        QtUi::Begin("About Ptero Editor", &gShowAboutWindow, QtUiWindowFlags_AlwaysAutoResize);
        QtUi::TextUnformatted("Ptero Editor");
        QtUi::TextDisabled("Interface: Qt Widgets | Rendering: DirectX 12");
        QtUi::TextUnformatted("Copyright \xC2\xA9 2026 Pterosoft Studio | All rights reserved");

        // Everything under Source/SDKs that the build or the shaders actually use, plus the
        // code ported or vendored from elsewhere. Folders nothing references any more
        // (crest, ImGui, ImGuizmo, ISPCTextureCompressor, RTXGI, UnrealClouds) are left out.
        // Keep in step with SDKs.md.
        struct ThirdPartyEntry
        {
            const char* Name;
            const char* Version;
            const char* Author;
            const char* UsedFor;
        };
        static const ThirdPartyEntry kThirdParty[] = {
            { "Qt",                       "6.11.2",       "The Qt Company",        "Editor interface" },
            { "QtNodes (nodeeditor)",     "",             "Dmitry Pinaev",         "Node Graph editor" },
            { "RmlUi",                    "",             "The RmlUi Team",        "In-game UI" },
            { "Lucide icons",             "0.469.0",      "Lucide Contributors",   "Editor icons" },
            { "DirectX Shader Compiler",  "",             "Microsoft",             "Runtime HLSL compilation" },
            { "DirectXTex",               "211",          "Microsoft",             "Texture import and DDS loading" },
            { "NVIDIA NRD / NRI",         "4.17.3 / 179", "NVIDIA",                "Ray-traced GI and AO denoising" },
            { "NVIDIA Streamline",        "2.11.1",       "NVIDIA",                "DLSS Super Resolution" },
            { "NVIDIA RTXDI",             "",             "NVIDIA",                "Many-light sampling (shaders)" },
            { "AMD FidelityFX SDK",       "2.3.0",        "AMD",                   "FSR upscaling and frame generation" },
            { "AMD FidelityFX SSSR",      "1.3",          "AMD",                   "Screen-space reflections" },
            { "Intel XeSS",               "3.0.2",        "Intel",                 "XeSS super resolution, frame generation, Xe Low Latency" },
            { "Intel XeGTAO",             "1.02",         "Intel",                 "Ambient occlusion" },
            { "SMAA",                     "",             "Jimenez et al.",        "Anti-aliasing" },
            { "Separable SSS",            "1.0",          "Jorge Jimenez",         "Subsurface scattering" },
            { "Hosek-Wilkie sky model",   "",             "Hosek and Wilkie",      "Analytic sky" },
            { "water-shader",             "",             "tuxalin",               "Water shading" },
            { "FMOD Studio API",          "2.03.13",      "Firelight Technologies", "Audio" },
            { "Resonance Audio",          "",             "Google",                "Spatial audio (FMOD plugin)" },
            { "libvpx",                   "1.17.0",       "The WebM Project",      "VP8/VP9 video decoding" },
            { "libwebm",                  "1.0.0.32",     "The WebM Project",      "WebM container parsing" },
            { "Opus",                     "1.5.2",        "Xiph.Org Foundation",   "Video soundtrack decoding" },
            { "Autodesk FBX SDK",         "2020.3.9",     "Autodesk",              "FBX import" },
            { "meshoptimizer",            "1.1",          "Arseny Kapoulkine",     "LOD generation and mesh optimisation" },
            { "CoACD",                    "",             "Wei, Liu et al.",       "Collision generation" },
            { "LZ4",                      "1.10.0",       "Yann Collet",           "Game package compression" },
            { "libsodium",                "1.0.22",       "Frank Denis",           "Game package encryption" },
            { "JSON for Modern C++",      "",             "Niels Lohmann",         "Levels and asset files" },
        };

        QtUi::SeparatorText("Third-party software");
        QtUi::TextWrapped("Ptero Editor is built with the following software. Each remains the property of "
            "its authors and is used under its own licence, which ships with it in Source/SDKs.");
        if (QtUi::BeginTable("AboutThirdParty", 4, QtUiTableFlags_Borders | QtUiTableFlags_RowBg | QtUiTableFlags_SizingStretchProp))
        {
            QtUi::TableSetupColumn("Component");
            QtUi::TableSetupColumn("Version");
            QtUi::TableSetupColumn("By");
            QtUi::TableSetupColumn("Used for");
            QtUi::TableHeadersRow();
            for (const ThirdPartyEntry& entry : kThirdParty)
            {
                QtUi::TableNextRow();
                QtUi::TableSetColumnIndex(0);
                QtUi::TextUnformatted(entry.Name);
                QtUi::TableSetColumnIndex(1);
                QtUi::TextDisabled("%s", entry.Version[0] != '\0' ? entry.Version : "-");
                QtUi::TableSetColumnIndex(2);
                QtUi::TextDisabled("%s", entry.Author);
                QtUi::TableSetColumnIndex(3);
                QtUi::TextUnformatted(entry.UsedFor);
            }
            QtUi::EndTable();
        }
        QtUi::End();
    }

    // -----------------------------------------------------------------------
    // Editor Settings: picks the editor style, one of the .style files in
    // Data/Styles. QtUi applies it and remembers the choice for next time.
    // -----------------------------------------------------------------------
    if (gShowEditorSettingsWindow)
    {
        if (QtUi::Begin("Editor Settings", &gShowEditorSettingsWindow, QtUiWindowFlags_AlwaysAutoResize))
        {
            const std::vector<QtUi::StyleEntry>& styles = QtUi::AvailableStyles();
            const std::string currentFile = QtUi::CurrentStyleFile();

            std::vector<const char*> names;
            names.reserve(styles.size());
            int currentIndex = -1;
            for (std::size_t i = 0; i < styles.size(); ++i)
            {
                names.push_back(styles[i].Name.c_str());
                if (_stricmp(styles[i].File.c_str(), currentFile.c_str()) == 0)
                    currentIndex = static_cast<int>(i);
            }

            QtUi::SeparatorText("Style");
            if (styles.empty())
            {
                QtUi::TextWrapped("No .style files found in %s. The editor is using its built-in style.",
                    QtUi::StylesDirectory());
            }
            else
            {
                int selected = currentIndex;
                if (QtUi::Combo("Editor style", &selected, names.data(), static_cast<int>(names.size()))
                    && selected >= 0 && selected < static_cast<int>(styles.size()))
                {
                    QtUi::LoadStyle(styles[selected].File.c_str());
                }
                if (currentIndex >= 0 && !styles[currentIndex].Description.empty())
                    QtUi::TextDisabled("%s", styles[currentIndex].Description.c_str());
            }

            if (QtUi::Button("Reload"))
                QtUi::ReloadStyle();
            QtUi::SetItemTooltip("Read the style file again. Saving the file does this on its own.");
            QtUi::SameLine();
            if (QtUi::Button("Rescan Folder"))
                QtUi::AvailableStyles(true);
            QtUi::SetItemTooltip("Look for .style files added since the list was read.");
            QtUi::SameLine();
            if (QtUi::Button("Open Styles Folder"))
            {
                // QtUi hands out UTF-8; widen it as such rather than through the ANSI page.
                const char* folderUtf8 = QtUi::StylesDirectory();
                std::wstring folder(MultiByteToWideChar(CP_UTF8, 0, folderUtf8, -1, nullptr, 0), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, folderUtf8, -1, folder.data(), static_cast<int>(folder.size()));
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }

            QtUi::SeparatorText("New style");
            QtUi::TextWrapped("Copies the current style to a new .style file and switches to it. "
                "Edit it in any text editor; the editor reapplies it each time you save. "
                "Data/Styles/README.md lists every setting.");
            QtUi::InputText("Name", gNewStyleName, sizeof(gNewStyleName));
            QtUi::SameLine();
            QtUi::BeginDisabled(gNewStyleName[0] == '\0');
            if (QtUi::Button("Duplicate") && QtUi::DuplicateStyle(gNewStyleName))
                gNewStyleName[0] = '\0';
            QtUi::EndDisabled();

            const char* styleError = QtUi::StyleError();
            if (styleError != nullptr && styleError[0] != '\0')
                QtUi::TextColored(UiVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", styleError);
        }
        QtUi::End();
    }

    // -----------------------------------------------------------------------
    // G-Buffer / RT Debug window
    // Displays each G-Buffer layer and the RT GI accumulation texture as a
    // tiled image grid so rendering issues can be diagnosed visually.
    // -----------------------------------------------------------------------
    if (gShowGBufferDebugWindow && gbufferTextureIds != nullptr)
    {
        // Force the window large enough for all 5 tiles every time it is opened
        // so that stale imgui.ini sizes from earlier sessions don't cut off tiles.
        QtUi::SetNextWindowSize(UiVec2(900.0f, 980.0f), QtUiCond_Appearing);
        if (QtUi::Begin("G-Buffer / RT Debug", &gShowGBufferDebugWindow))
        {
            // Each tile shows a label, then the texture at a fixed size.
            //
            // The tile size is deliberately NOT derived from the available
            // content region. This layer is immediate-mode over retained Qt
            // widgets, so the content determines the window's minimum size -
            // sizing the tiles from the window therefore feeds back on itself
            // and the window grows a little every frame until it fills the
            // screen. Two fixed columns at this width fit the 900px default.
            const float padding    = 8.0f;
            const float tileWidth  = 420.0f;
            const float tileHeight = tileWidth * (9.0f / 16.0f); // 16:9 aspect

            // Order tiles so the most diagnostic ones appear first (top-left).
            struct Tile { const char* label; UiTextureID id; };
            Tile tiles[] =
            {
                { "RT GI Accumulation (RGB)",              gbufferTextureIds->GiAccum  },
                { "Albedo (RGB)",                          gbufferTextureIds->Albedo   },
                { "World Normal (RGB)",                    gbufferTextureIds->Normal   },
                { "Material (R=Roughness, G=Metallic, B=AO)", gbufferTextureIds->Material },
                { "Depth (reversed preview of scene depth)",  gbufferTextureIds->Depth    },
                { "Point Shadow Array (slice 0 view)",       gbufferTextureIds->PointShadowArray },
            };

            for (int i = 0; i < static_cast<int>(std::size(tiles)); ++i)
            {
                // Two columns: even indices go left, odd indices go right (SameLine).
                if (i > 0 && (i % 2) == 1)
                    QtUi::SameLine(tileWidth + padding);

                QtUi::BeginGroup();
                QtUi::TextUnformatted(tiles[i].label);
                if (tiles[i].id != UiTextureID_Invalid)
                    QtUi::Image(tiles[i].id, UiVec2(tileWidth, tileHeight));
                else
                    QtUi::TextDisabled("(not available)");
                QtUi::EndGroup();
            }
        }
        QtUi::End();
    }

    DrawBuildGameWindow();
    DrawExtractPackageWindow();
    DrawGameSettingsWindow(editorInstance != nullptr ? editorInstance->GetCurrentSceneFilePath() : std::string());
}
