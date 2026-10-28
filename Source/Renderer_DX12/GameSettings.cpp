#include "pch.h"
#include "DX12SceneRenderer.h"
#include "../QtUi/QtUi.h"
#include "../../Data/UI/Farkle/FarkleMenuController.h"
#include "System/PteroLog.h"
#include "AudioManager.h"
#include "UiLocalization.h"

#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

// The player-facing Settings pages (Data/UI/Farkle/settings-*.rml) mapped onto the
// renderer, window and audio. Quality tiers are deliberately relative to what the level
// authored: "High" is the level exactly as its artist tuned it, and the other tiers
// scale the cost-bearing knobs from there without touching the look-defining ones
// (fog density and colour, GI intensity, and so on).
namespace
{
    using Values = std::map<std::string, std::string>;

    // The player's settings live in the game's own folder, beside game.cfg: settings.cfg
    // next to the exe (Binaries/ for the editor's play sessions). Deliberately not in the
    // user's AppData, so the game folder carries everything about the install with it.
    std::filesystem::path SettingsPath()
    {
        wchar_t exePath[MAX_PATH * 4] = {};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length >= std::size(exePath))
            return {};
        return std::filesystem::path(exePath).parent_path() / L"settings.cfg";
    }

    // Where earlier builds kept the settings. Read once, when the game folder has none yet,
    // so an existing player does not lose their choices; never written to again.
    std::filesystem::path LegacySettingsPath()
    {
        PWSTR folder = nullptr;
        std::filesystem::path path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
            path = std::filesystem::path(folder) / L"Ptero-Engine" / L"Farkle" / L"Settings.ini";
        CoTaskMemFree(folder);
        return path;
    }

    Values LoadSettingsFile()
    {
        Values values;
        std::filesystem::path path = SettingsPath();
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError))
            path = LegacySettingsPath();
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto equals = line.find('=');
            if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
            values[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return values;
    }

    bool SaveSettingsFile(const Values& values, std::string& error)
    {
        const auto path = SettingsPath();
        if (path.empty()) { error = "the game folder could not be determined"; return false; }
        std::error_code ec;
        // Written aside and swapped in, so a crash mid-write cannot leave half a file.
        auto temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::trunc);
            file << "# Farkle player settings, written by the game's Settings menu.\n";
            for (const auto& entry : values) file << entry.first << '=' << entry.second << '\n';
            if (!file) { error = "could not write " + temporary.string(); return false; }
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec) { error = ec.message(); return false; }
        return true;
    }

    // -1 off, 0 low, 1 medium, 2 high, 3 ultra. Anything unrecognised reads as high,
    // which is the level as authored.
    int Tier(const Values& values, const char* key)
    {
        const auto it = values.find(key);
        if (it == values.end()) return 2;
        const std::string& value = it->second;
        if (value == "off") return -1;
        if (value == "low") return 0;
        if (value == "medium") return 1;
        if (value == "ultra") return 3;
        return 2;
    }

    bool ParseResolution(const std::string& text, unsigned& width, unsigned& height)
    {
        const auto x = text.find('x');
        if (x == std::string::npos) return false;
        try { width = static_cast<unsigned>(std::stoul(text.substr(0, x))); height = static_cast<unsigned>(std::stoul(text.substr(x + 1))); }
        catch (...) { return false; }
        return width > 0 && height > 0;
    }

    // The display modes the game's monitor actually offers, landscape and at least the
    // 1280x720 the menus are laid out for. Refresh rates and bit depths collapse into one
    // entry per size, smallest first.
    FarkleMenuController::Choices MonitorResolutions()
    {
        std::set<std::pair<unsigned, unsigned>> sizes;
        HWND window = QtUi::HostHandle();
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        if (window && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            for (DWORD i = 0; EnumDisplaySettingsW(monitor.szDevice, i, &mode); ++i)
                if (mode.dmPelsWidth >= 1280 && mode.dmPelsHeight >= 720 && mode.dmPelsWidth > mode.dmPelsHeight)
                    sizes.insert({mode.dmPelsWidth, mode.dmPelsHeight});
        }
        std::vector<std::pair<unsigned, unsigned>> ordered(sizes.begin(), sizes.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.first * a.second != b.first * b.second ? a.first * a.second < b.first * b.second : a.first < b.first;
        });
        FarkleMenuController::Choices choices;
        for (const auto& size : ordered)
            choices.push_back({std::to_string(size.first) + "x" + std::to_string(size.second),
                               std::to_string(size.first) + " \xC3\x97 " + std::to_string(size.second)});
        return choices;
    }

    // Moves the standalone game's window to the saved display mode. Remembers what it last
    // did, so re-applying unchanged settings (the menus' Apply, the game start) does not
    // switch the monitor's mode again or make the window flicker.
    std::string ApplyDisplayMode(Values& values)
    {
        static int appliedMode = -1;
        static unsigned appliedWidth = 0, appliedHeight = 0;

        std::string note;
        const auto resolutions = MonitorResolutions();
        std::string& resolution = values["resolution"];
        if (!resolutions.empty() && std::none_of(resolutions.begin(), resolutions.end(), [&](const auto& c) { return c.first == resolution; }))
        {
            note = "That resolution is not offered by this display.";
            resolution = resolutions.back().first;
        }
        unsigned width = 1920, height = 1080;
        ParseResolution(resolution, width, height);
        const std::string& displayMode = values["display-mode"];
        const int mode = displayMode == "windowed" ? 0 : displayMode == "fullscreen" ? 2 : 1;
        // Borderless covers the monitor whatever the resolution says.
        if (mode == appliedMode && (mode == 1 || (width == appliedWidth && height == appliedHeight)))
            return note;

        if (!QtUi::SetGameDisplayMode(mode, width, height) && mode == 2)
        {
            values["display-mode"] = "borderless";
            note += std::string(note.empty() ? "" : " ") + "Fullscreen at that resolution failed; using borderless.";
            appliedMode = 1;
        }
        else
        {
            appliedMode = mode;
        }
        appliedWidth = width;
        appliedHeight = height;
        return note;
    }

    // A linear 0-100 slider is almost all loud; squaring the gain spreads the audible
    // range across the whole slider.
    // A 0-100 slider's value; 50 (the middle) when missing or unreadable.
    int SliderPercent(const Values& values, const char* key, int fallback = 50)
    {
        const auto it = values.find(key);
        int percent = fallback;
        if (it != values.end()) { try { percent = std::stoi(it->second); } catch (...) {} }
        return std::clamp(percent, 0, 100);
    }

    float VolumeGain(const Values& values, const char* key)
    {
        const float linear = SliderPercent(values, key, 100) / 100.0f;
        return linear * linear;
    }
}

void ApplySavedGameDisplayMode()
{
    // Before the renderer's first frame, so the window is already the saved size and the
    // swap chain is created for it: the intro videos and the level must never be seen in
    // a window that later jumps to fullscreen. Only the Settings.ini keys the menus know
    // count; the rest of the file is applied when the game starts.
    FarkleMenuController::DefaultLanguage = UiLocalization::DetectLanguage();
    FarkleMenuController menus;
    menus.Load(LoadSettingsFile());
    Values values = menus.Saved();
    ApplyDisplayMode(values);
    // The startup screens come before the game's settings are applied, and are translated too.
    UiLocalization::SetLanguage(values.at("ui-language"));
}

void DX12SceneRenderer::BeginGameSettings(bool standalone)
{
    auto& baseline = mGameSettingsBaseline;
    baseline.Taa = mTaaSettings; baseline.Smaa = mSmaaSettings; baseline.Msaa = mMsaaSettings; baseline.AnalyticEdgeAa = mAnalyticEdgeAaSettings; baseline.Aar = mAarSettings;
    baseline.Dlss = mDlssSettings; baseline.Fsr = mFsrSettings; baseline.Xess = mXessSettings;
    baseline.GiMode = mGlobalIlluminationMode; baseline.Rtgi = mRtgiSettings;
    baseline.Cascades = mRadianceCascadesSettings; baseline.Voxel = mVoxelGiSettings; baseline.Probes = mProbeSettings; baseline.Fog = mVolumetricFogSettings;
    baseline.PointShadows = mPointShadowSettings;
    baseline.VirtualShadows = mVirtualShadowMapSettings;
    baseline.RtShadows = mRtShadowSettings;
    baseline.Gtao = mGtaoSettings; baseline.Rtao = mRtaoSettings;
    baseline.ChromaticAberration = mChromaticAberrationSettings;
    baseline.PathTracer = mPathTracerSettings;
    baseline.VirtualGeometry = mVirtualGeometryRenderer.GetSettings();
    baseline.Ssr = mSsrSettings; baseline.Subsurface = mSubsurfaceSettings;
    baseline.Bloom = mBloomSettings; baseline.LensFlare = mLensFlareSettings;
    mGameSettingsActive = true;
    mGameSettingsApplied = false;

    FarkleMenuController& menus = mRmlUiRenderer.GetFarkleMenus();
    // Every session reads the file, so the editor's test sessions use the same winning
    // score and language as the player's; only the standalone game applies the rest.
    FarkleMenuController::DefaultLanguage = UiLocalization::DetectLanguage();
    menus.Load(LoadSettingsFile());
    UiLocalization::SetLanguage(menus.Saved().at("ui-language"));
    menus.Options.clear();
    FarkleMenuController::Choices languages;
    for (const auto& language : UiLocalization::Languages()) languages.push_back({language.Code, language.Name});
    menus.Options["ui-language"] = std::move(languages);
    if (standalone)
    {
        if (auto resolutions = MonitorResolutions(); !resolutions.empty())
            menus.Options["resolution"] = std::move(resolutions);
        FarkleMenuController::Choices upscalers{{"none", "None"}};
        if (mDlssRenderer.IsAvailable()) upscalers.push_back({"dlss", "DLSS"});
        if (mFsrRenderer.IsAvailable()) upscalers.push_back({"fsr", "FSR"});
        if (mXessRenderer.IsAvailable()) upscalers.push_back({"xess", "XeSS"});
        if (mTaaRenderer.IsInitialized()) upscalers.push_back({"taau", "TAA Upsampling"});
        // AAR and Analytic Edge AA go by the engine's names in the menus.
        if (AnalyticEdge::IsSupported()) upscalers.push_back({"aar", "Ptero-SR"});
        menus.Options["upscaler"] = std::move(upscalers);
        FarkleMenuController::Choices antiAliasing{{"taa", "TAA"}, {"msaa", "MSAA"}, {"smaa", "SMAA"}};
        if (AnalyticEdge::IsSupported()) antiAliasing.push_back({"ptero-aa", "Ptero-AA"});
        menus.Options["anti-aliasing"] = std::move(antiAliasing);
        // The key predates XeSS; "on" still means FSR so saved files keep working.
        FarkleMenuController::Choices frameGeneration{{"off", "Off"}};
        if (mFrameGeneration.IsApiAvailable()) frameGeneration.push_back({"on", "FSR"});
        if (mXessFrameGeneration.IsApiAvailable()) frameGeneration.push_back({"xess", "XeSS"});
        menus.Options["fsr-frame-generation"] = std::move(frameGeneration);
        FarkleMenuController::Choices pathTracing{{"off", "Off"}};
        if (IsRayTracingSupported() && !mPathTracer.HasInitFailed())
            pathTracing.push_back({"realtime", "On"});
        menus.Options["path-tracing"] = std::move(pathTracing);
        // RTGI and the probes trace rays; voxel GI runs on any GPU.
        FarkleMenuController::Choices giMethods{{"off", "Off"}};
        if (IsRayTracingSupported())
        {
            giMethods.push_back({"rtgi", "RTGI"});
            if (!mProbeRenderer.HasInitFailed()) giMethods.push_back({"probes", "Probe GI"});
        }
        if (!mVoxelGiRenderer.HasInitFailed()) giMethods.push_back({"voxel", "Voxel GI"});
        menus.Options["gi-method"] = std::move(giMethods);
        // Ray-traced AO and shadows are offered only where the GPU can trace.
        FarkleMenuController::Choices aoMethods{{"gtao", "Screen-Space"}};
        if (IsRayTracingSupported()) aoMethods.push_back({"rtao", "Ray Traced"});
        menus.Options["ao-method"] = std::move(aoMethods);
        FarkleMenuController::Choices rtShadowChoices{{"off", "Off"}};
        if (IsRayTracingSupported())
        {
            rtShadowChoices.push_back({"sun", "Sun"});
            rtShadowChoices.push_back({"all", "Sun + Lights"});
        }
        menus.Options["rt-shadows"] = std::move(rtShadowChoices);
    }
    menus.Apply = [this](Values& values) { return ApplyGameSettings(values, true); };
}

void DX12SceneRenderer::ApplySavedGameSettings()
{
    FarkleMenuController& menus = mRmlUiRenderer.GetFarkleMenus();
    Values values = menus.Saved();
    // First launch (or first since the file moved out of AppData): write it straight away,
    // so settings.cfg sits in the game folder from the start rather than after an Apply.
    std::error_code existsError;
    const bool fileExists = std::filesystem::exists(SettingsPath(), existsError);
    const std::string status = ApplyGameSettings(values, !fileExists);
    // Corrected values (an upscaler this GPU lacks, say) become what the menus show.
    if (values != menus.Saved()) menus.Load(values);
    PteroLog::Write(PteroLog::Level::Info, "Game", ("Saved player settings: " + status).c_str());
}

void DX12SceneRenderer::EndGameSettings()
{
    if (!mGameSettingsActive) return;
    mGameSettingsActive = false;
    FarkleMenuController& menus = mRmlUiRenderer.GetFarkleMenus();
    menus.Apply = nullptr;
    menus.Options.clear();
    if (!std::exchange(mGameSettingsApplied, false)) return;

    const auto& baseline = mGameSettingsBaseline;
    mTaaSettings = baseline.Taa; mSmaaSettings = baseline.Smaa; mMsaaSettings = baseline.Msaa; mAnalyticEdgeAaSettings = baseline.AnalyticEdgeAa; mAarSettings = baseline.Aar;
    mDlssSettings = baseline.Dlss; mFsrSettings = baseline.Fsr; mXessSettings = baseline.Xess;
    mGlobalIlluminationMode = baseline.GiMode; mRtgiSettings = baseline.Rtgi;
    mRadianceCascadesSettings = baseline.Cascades; mVoxelGiSettings = baseline.Voxel; mProbeSettings = baseline.Probes; mVolumetricFogSettings = baseline.Fog;
    mPointShadowSettings = baseline.PointShadows;
    mVirtualShadowMapSettings = baseline.VirtualShadows;
    mRtShadowSettings = baseline.RtShadows;
    mGtaoSettings = baseline.Gtao; mRtaoSettings = baseline.Rtao;
    mChromaticAberrationSettings = baseline.ChromaticAberration;
    mPathTracerSettings = baseline.PathTracer;
    mPathTracerSettings.ResetAccumulation = true;
    mVirtualGeometryRenderer.GetSettings() = baseline.VirtualGeometry;
    mSsrSettings = baseline.Ssr; mSubsurfaceSettings = baseline.Subsurface;
    mBloomSettings = baseline.Bloom; mLensFlareSettings = baseline.LensFlare;
    mShadowsEnabled = true;
    mUpscalerSharpness = -1.0f;
    mPlayerBrightnessStops = 0.0f;
    mPlayerDisplayGamma = 1.0f;
    mShowStatisticsOverlay = false;
    mTaaSettings.ResetHistory = true; mDlssSettings.ResetHistory = true; mFsrSettings.ResetHistory = true;
    mXessSettings.ResetHistory = true;
    mTextureQualityMipBias = 0.0f;
    mEntityMeshRenderer.SetLodDistanceScale(1.0f);
    if (mAudioManager) mAudioManager->SetVolumes(1.0f, 1.0f, 1.0f);
}

bool DX12SceneRenderer::GetGameSetting(const std::string& key, std::string& value)
{
    // Saved() is what Apply last left in effect, corrections included.
    const Values& saved = mRmlUiRenderer.GetFarkleMenus().Saved();
    const auto it = saved.find(key);
    value = it == saved.end() ? std::string() : it->second;
    return it != saved.end();
}

std::string DX12SceneRenderer::ApplyGameSettings(GameSettingValues& values, bool persist)
{
    if (!mGameSettingsActive) return "Settings can only be applied while the game is running.";
    const auto& baseline = mGameSettingsBaseline;
    mGameSettingsApplied = true;
    std::vector<std::string> notes;

    // ---- Anti-aliasing and upscaling. The AA choice is kept while an upscaler owns
    // the slot, so switching the upscaler back off restores it.
    std::string upscaler = values["upscaler"];
    if (upscaler == "dlss" && !mDlssRenderer.IsAvailable()) { upscaler = "none"; notes.push_back("DLSS is not available on this system."); }
    if (upscaler == "fsr" && !mFsrRenderer.IsAvailable()) { upscaler = "none"; notes.push_back("FSR is not available on this system."); }
    if (upscaler == "xess" && !mXessRenderer.IsAvailable()) { upscaler = "none"; notes.push_back("XeSS is not available on this system."); }
    if (upscaler == "taau" && !mTaaRenderer.IsInitialized()) { upscaler = "none"; notes.push_back("TAA upsampling is not available."); }
    if (upscaler == "aar" && !AnalyticEdge::IsSupported()) { upscaler = "none"; notes.push_back("Ptero-SR needs Shader Model 6.1 barycentrics, which this GPU lacks."); }
    if (upscaler != "dlss" && upscaler != "fsr" && upscaler != "xess" && upscaler != "taau" && upscaler != "aar") upscaler = "none";
    values["upscaler"] = upscaler;

    std::string& antiAliasing = values["anti-aliasing"];
    if (antiAliasing == "ptero-aa" && !AnalyticEdge::IsSupported())
    {
        antiAliasing = "taa";
        notes.push_back("Ptero-AA needs Shader Model 6.1 barycentrics, which this GPU lacks; using TAA.");
    }
    if (antiAliasing != "msaa" && antiAliasing != "smaa" && antiAliasing != "ptero-aa") antiAliasing = "taa";
    mTaaSettings = baseline.Taa; mSmaaSettings = baseline.Smaa; mMsaaSettings = baseline.Msaa; mAnalyticEdgeAaSettings = baseline.AnalyticEdgeAa; mAarSettings = baseline.Aar;
    // DLSS, FSR and XeSS are temporal themselves; TAA on top re-jitters an already resolved
    // image and makes it shimmer, so it is off whenever one of them is. TAA upsampling is
    // TAA itself, so it switches TAA on whatever the anti-aliasing choice.
    const bool taau = upscaler == "taau";
    mTaaSettings.Enabled = (antiAliasing == "taa" && upscaler == "none") || taau;
    mTaaSettings.Upscaling = taau;
    mSmaaSettings.Enabled = antiAliasing == "smaa";
    // Ptero-AA: the engine's analytic edge AA on its own, no temporal pass. An upscaler
    // brings its own AA, as for MSAA.
    mAnalyticEdgeAaSettings.Enabled = antiAliasing == "ptero-aa" && upscaler == "none";
    mAnalyticEdgeAaSettings.DebugView = 0;
    // MSAA multisamples the G-Buffer, which an upscaler would only discard.
    mMsaaSettings.Enabled = antiAliasing == "msaa" && upscaler == "none";
    mTaaSettings.ResetHistory = true;

    // Upscaler quality: the same five steps for all, in each one's own mode numbering
    // (DlssSettings.h / FsrSettings.h / XessSettings.h), matched by render scale.
    // Native renders at full size and keeps only the AA.
    std::string& quality = values["upscaler-quality"];
    static const char* const kQualities[] = { "native", "quality", "balanced", "performance", "ultra-performance" };
    static constexpr int kDlssModes[] = { 6, 3, 2, 1, 4 };
    static constexpr int kFsrModes[] = { 0, 1, 2, 3, 4 };
    // XeSS: Native AA 1.0x, Ultra Quality 1.5x, Quality 1.7x, Balanced 2.0x, Ultra Performance 3.0x.
    static constexpr int kXessModes[] = { 0, 2, 3, 4, 6 };
    // TAAU screen percentages at the same render scales.
    static constexpr float kTaauScreenPercentages[] = { 100.0f, 67.0f, 59.0f, 50.0f, 33.0f };
    int qualityIndex = 1;
    for (int i = 0; i < 5; ++i)
        if (quality == kQualities[i]) qualityIndex = i;
    quality = kQualities[qualityIndex];
    if (taau)
        mTaaSettings.ScreenPercentage = kTaauScreenPercentages[qualityIndex];

    // AAR is spatial and stops at Balanced (66.7 %): below that the render samples are too
    // sparse for its per-surface reconstruction, so the lower steps use Balanced too.
    static constexpr int kAarModes[] = { AarSettings::Native, AarSettings::Quality, AarSettings::Balanced,
                                         AarSettings::Balanced, AarSettings::Balanced };
    mAarSettings.Enabled = upscaler == "aar";
    mAarSettings.Mode = kAarModes[qualityIndex];
    mAarSettings.DebugView = 0;
    if (upscaler == "aar" && qualityIndex > 2)
        notes.push_back("Ptero-SR's lowest setting is Balanced (66.7 %); using that.");

    mXessSettings = baseline.Xess;
    mXessSettings.Enabled = upscaler == "xess";
    mXessSettings.Mode = kXessModes[qualityIndex];
    mXessSettings.ResetHistory = true;

    mDlssSettings = baseline.Dlss;
    mDlssSettings.Enabled = upscaler == "dlss";
    mDlssSettings.Mode = kDlssModes[qualityIndex];
    mDlssSettings.ResetHistory = true;

    mFsrSettings = baseline.Fsr;
    mFsrSettings.Enabled = upscaler == "fsr";
    mFsrSettings.Mode = kFsrModes[qualityIndex];
    mFsrSettings.FrameGeneration = values["fsr-frame-generation"] == "on";
    if (mFsrSettings.FrameGeneration && !mFrameGeneration.IsApiAvailable())
    {
        mFsrSettings.FrameGeneration = false;
        values["fsr-frame-generation"] = "off";
        notes.push_back("FSR frame generation is not available on this system.");
    }
    mFsrSettings.ResetHistory = true;

    mXessSettings.FrameGeneration = values["fsr-frame-generation"] == "xess";
    if (mXessSettings.FrameGeneration && !mXessFrameGeneration.IsApiAvailable())
    {
        mXessSettings.FrameGeneration = false;
        values["fsr-frame-generation"] = "off";
        notes.push_back("XeSS frame generation is not available on this system.");
    }
    if (values["fsr-frame-generation"] != "on" && values["fsr-frame-generation"] != "xess")
        values["fsr-frame-generation"] = "off";

    // Upscaler sharpening. FSR sharpens with its own RCAS; DLSS has none of its own any
    // more, so after DLSS the engine's image sharpen pass carries the same amount (see
    // DX12SceneRenderer's image sharpen pass). No effect without an upscaler.
    const std::string& sharpening = values["upscaler-sharpness"];
    const float sharpness = sharpening == "off" ? 0.0f : sharpening == "low" ? 0.25f : sharpening == "high" ? 0.8f : 0.5f;
    mFsrSettings.Sharpening = sharpness > 0.0f;
    mFsrSettings.Sharpness = sharpness;
    mUpscalerSharpness = sharpness;

    // ---- Global illumination. GI Method picks the technique, or Off for none; GI
    // Quality scales the chosen one from the level's own tuning.
    std::string giMethod = values["gi-method"];
    std::string& giQuality = values["gi-quality"];
    // Earlier builds switched GI off through its quality and had a "level" method.
    if (giQuality == "off") { giMethod = "off"; giQuality = "high"; }
    if (giMethod != "off" && giMethod != "rtgi" && giMethod != "probes" && giMethod != "voxel") giMethod = "rtgi";
    if ((giMethod == "rtgi" || giMethod == "probes") && !IsRayTracingSupported())
    {
        const bool voxelAvailable = !mVoxelGiRenderer.HasInitFailed();
        notes.push_back(std::string(giMethod == "rtgi" ? "RTGI" : "Probe GI") + " needs a GPU with DirectX Raytracing; "
            + (voxelAvailable ? "using Voxel GI." : "GI is off."));
        giMethod = voxelAvailable ? "voxel" : "off";
    }
    if (giMethod == "probes" && mProbeRenderer.HasInitFailed())
    {
        notes.push_back("Probe GI failed to start on this system; using RTGI.");
        giMethod = "rtgi";
    }
    if (giMethod == "voxel" && mVoxelGiRenderer.HasInitFailed())
    {
        notes.push_back("Voxel GI failed to start on this system; GI is off.");
        giMethod = "off";
    }
    values["gi-method"] = giMethod;
    const int gi = giMethod == "off" ? -1 : Tier(values, "gi-quality");
    mGlobalIlluminationMode = gi < 0 ? GlobalIlluminationMode::Disabled : GlobalIlluminationMode::Rtgi;
    mRtgiSettings = baseline.Rtgi;
    mRadianceCascadesSettings = baseline.Cascades;
    mVoxelGiSettings = baseline.Voxel;
    auto& rtgi = mRtgiSettings;
    auto& cascades = mRadianceCascadesSettings;
    auto& voxel = mVoxelGiSettings;
    auto& probes = mProbeSettings;
    probes = baseline.Probes;
    probes.DebugShowProbes = false;

    // The renderer runs voxel GI over the probes over RTGI, by their own switches
    // (DX12SceneRenderer's GI selection; GlobalIlluminationMode is only the editor's
    // label), so a method is chosen by switching the other two off.
    rtgi.Enabled = giMethod == "rtgi";
    probes.Enabled = giMethod == "probes";
    voxel.Enabled = giMethod == "voxel";

    // Off has to reach every method, including the voxel fallback that would stand in
    // for RTGI or the probes on a GPU that cannot ray trace.
    if (gi < 0)
    {
        rtgi.Enabled = false;
        probes.Enabled = false;
        voxel.Enabled = false;
        voxel.FallbackWithoutRayTracing = false;
    }
    const unsigned rays = baseline.Cascades.RaysPerProbe, spacing = baseline.Cascades.ProbeSpacingBase;
    // Probe GI: rays per probe, within the probe renderer's 32..256.
    static constexpr float kProbeRayScale[] = {0.5f, 0.75f, 1.0f, 2.0f};
    if (gi >= 0)
        probes.RaysPerProbe = std::clamp(static_cast<int>(baseline.Probes.RaysPerProbe * kProbeRayScale[gi]), 32, 256);
    switch (gi)
    {
    case 0:
        rtgi.RaysPerPixel = 1; rtgi.MaxBounces = 1;
        cascades.RaysPerProbe = (std::max)(2u, rays / 4); cascades.ProbeSpacingBase = spacing * 2;
        voxel.Resolution = (std::max)(32, baseline.Voxel.Resolution / 2); voxel.DiffuseCones = 1;
        voxel.HalfResolution = true; voxel.SpecularEnabled = false;
        break;
    case 1:
        rtgi.RaysPerPixel = (std::max)(1, baseline.Rtgi.RaysPerPixel / 2); rtgi.MaxBounces = (std::max)(1, baseline.Rtgi.MaxBounces - 1);
        cascades.RaysPerProbe = (std::max)(2u, rays / 2); cascades.ProbeSpacingBase = spacing * 3 / 2;
        voxel.Resolution = (std::max)(32, baseline.Voxel.Resolution * 3 / 4); voxel.HalfResolution = true;
        break;
    case 3:
        rtgi.RaysPerPixel = (std::min)(8, baseline.Rtgi.RaysPerPixel * 2); rtgi.MaxBounces = (std::min)(4, baseline.Rtgi.MaxBounces + 1);
        cascades.RaysPerProbe = (std::min)(16u, rays * 2); cascades.ProbeSpacingBase = (std::max)(4u, spacing * 3 / 4);
        voxel.DiffuseCones = 9; voxel.HalfResolution = false;
        break;
    default:
        break;
    }

    // ---- GI and reflection resolution. GI Resolution is the divisor RTGI traces and
    // denoises its diffuse GI at, which is also the share of probes Probe GI traces each
    // frame (Quarter unless the player picks otherwise; the old "level" reads as it).
    // Reflection resolution is the one RTGI's reflections (and the probes') trace at -
    // full or half only, since reflections carry sharp detail (Half unless picked
    // otherwise; the old "level" reads as it).
    std::string giResolution = values["gi-resolution"];
    if (giResolution != "full" && giResolution != "half") giResolution = "quarter";
    values["gi-resolution"] = giResolution;
    {
        const int divisor = giResolution == "quarter" ? 4 : giResolution == "half" ? 2 : 1;
        rtgi.DiffuseResolution = divisor;
        probes.TraceResolution = divisor;
    }
    std::string reflectionResolution = values["reflection-resolution"];
    if (reflectionResolution != "full") reflectionResolution = "half";
    values["reflection-resolution"] = reflectionResolution;
    rtgi.SpecularResolution = reflectionResolution == "half" ? 2 : 1;

    // ---- Specular GI: RTGI and the probes can trace the reflections too, which then
    // replace the screen-space ones (see Reflections below). Off leaves them to SSR.
    std::string& specularGiValue = values["specular-gi"];
    if (specularGiValue != "off") specularGiValue = "on";
    const bool specularGi = specularGiValue == "on" && (giMethod == "rtgi" || giMethod == "probes");
    rtgi.SpecularEnabled = specularGi && giMethod == "rtgi";
    probes.SpecularEnabled = specularGi && giMethod == "probes";

    // ---- Path tracing: real time only, NRD-denoised paths that replace the GI and keep
    // up with motion. Needs DXR 1.1 and a path tracer that initialized. "realtime" stays
    // the saved value (achievements read it); the old "reference" choice becomes it.
    std::string pathTracing = values["path-tracing"];
    if (pathTracing == "reference" || pathTracing == "on") pathTracing = "realtime";
    if (pathTracing != "realtime") pathTracing = "off";
    if (pathTracing != "off" && (!IsRayTracingSupported() || mPathTracer.HasInitFailed()))
    {
        pathTracing = "off";
        notes.push_back(IsRayTracingSupported() ? "The path tracer failed to start on this system."
                                                : "Path tracing needs a GPU with DirectX Raytracing 1.1.");
    }
    values["path-tracing"] = pathTracing;
    mPathTracerSettings = baseline.PathTracer;
    mPathTracerSettings.Enabled = pathTracing != "off";
    mPathTracerSettings.Mode = PathTracerMode::RealTime;
    mPathTracerSettings.DebugView = 0;
    mPathTracerSettings.ResetAccumulation = true;

    // ---- Screen-space reflections, unless Specular GI traces them instead: SSR Method
    // picks the ray march or FidelityFX SSSR, and the tiers set its trace budget. Off
    // removes them.
    const int reflections = Tier(values, "reflection-quality");
    std::string& ssrMethod = values["ssr-method"];
    if (ssrMethod != "raymarch") ssrMethod = "sssr";
    auto& ssr = mSsrSettings;
    ssr = baseline.Ssr;
    ssr.Enabled = reflections >= 0 && !specularGi;
    ssr.Technique = ssrMethod == "sssr" ? 1 : 0;
    ssr.DebugView = 0;
    switch (reflections)
    {
    case 0: ssr.MaxSteps = (std::max)(12, baseline.Ssr.MaxSteps / 2); ssr.RefineSteps = (std::min)(baseline.Ssr.RefineSteps, 3);
            ssr.SssrMaxTraversalIntersections = (std::max)(32, baseline.Ssr.SssrMaxTraversalIntersections / 2);
            ssr.SssrMostDetailedMip = baseline.Ssr.SssrMostDetailedMip + 1; break;
    case 1: ssr.MaxSteps = (std::max)(16, baseline.Ssr.MaxSteps * 3 / 4);
            ssr.SssrMaxTraversalIntersections = (std::max)(48, baseline.Ssr.SssrMaxTraversalIntersections * 3 / 4); break;
    case 3: ssr.MaxSteps = (std::min)(128, baseline.Ssr.MaxSteps * 3 / 2); ssr.RefineSteps = (std::max)(baseline.Ssr.RefineSteps, 8);
            ssr.SssrMaxTraversalIntersections = (std::min)(256, baseline.Ssr.SssrMaxTraversalIntersections * 2);
            ssr.SssrSamplesPerQuad = (std::max)(baseline.Ssr.SssrSamplesPerQuad, 2); break;
    default: break;
    }

    // ---- Subsurface scattering. Off renders skin and wax as plain diffuse. Low and
    // Medium use the screen-space blur (a ray-traced level drops to it); Ultra spends
    // more of whichever the level uses. The mode is never raised to ray tracing, which
    // stays the level author's call.
    const int subsurface = Tier(values, "subsurface-quality");
    auto& sss = mSubsurfaceSettings;
    sss = baseline.Subsurface;
    sss.Enabled = subsurface >= 0 && baseline.Subsurface.Enabled;
    sss.DebugView = 0;
    switch (subsurface)
    {
    case 0: sss.Mode = 0; sss.Quality = 0; break;
    case 1: sss.Mode = 0; sss.Quality = baseline.Subsurface.Mode == 0 ? (std::min)(baseline.Subsurface.Quality, 1) : 2; break;
    case 3: sss.Quality = 2; sss.RtSamples = (std::max)(baseline.Subsurface.RtSamples, 16); break;
    default: break;
    }

    // ---- Virtualized geometry. Off draws the virtualized meshes with their authored
    // LODs (which Model Quality then scales); On keeps the level's settings, with Model
    // Quality setting how much on-screen error the clusters may show. The editor's
    // viewing aids never carry into play.
    auto& vg = mVirtualGeometryRenderer.GetSettings();
    vg = baseline.VirtualGeometry;
    vg.Enabled = values["virtual-geometry"] != "off" && baseline.VirtualGeometry.Enabled;
    vg.VirtualizeAllMeshes = false;
    vg.DebugView = 0;
    vg.FreezeCulling = false;
    static constexpr float kClusterErrorScale[] = {4.0f, 2.0f, 1.0f, 0.5f};
    vg.ErrorThresholdPixels = baseline.VirtualGeometry.ErrorThresholdPixels * kClusterErrorScale[std::clamp(Tier(values, "model-quality"), 0, 3)];

    // ---- Volumetric fog: froxel resolution only. Whether the level has fog at all,
    // and what it looks like, stays the level's.
    // ---- Ambient occlusion. AO Method picks screen-space (GTAO) or ray-traced (RTAO)
    // AO, and the other is switched off; Off switches both off. The tiers set GTAO's
    // sample quality or RTAO's rays per pixel, with High the level's own.
    const int ao = Tier(values, "ao-quality");
    std::string& aoMethod = values["ao-method"];
    if (aoMethod != "gtao" && aoMethod != "rtao") aoMethod = "gtao";
    if (aoMethod == "rtao" && !IsRayTracingSupported())
    {
        notes.push_back("Ray-traced AO needs a GPU with DirectX Raytracing; using screen-space AO.");
        aoMethod = "gtao";
    }
    mGtaoSettings = baseline.Gtao;
    mRtaoSettings = baseline.Rtao;
    mGtaoSettings.DebugView = 0;
    mRtaoSettings.DebugView = 0;
    mGtaoSettings.Enabled = ao >= 0 && aoMethod == "gtao";
    mRtaoSettings.Enabled = ao >= 0 && aoMethod == "rtao";
    if (ao >= 0 && ao != 2)
    {
        mGtaoSettings.QualityLevel = ao == 3 ? 3 : ao;
        static constexpr int kRtaoRays[] = {4, 8, 0, 32};
        mRtaoSettings.RaysPerPixel = ao == 3 ? (std::max)(baseline.Rtao.RaysPerPixel, kRtaoRays[3])
                                             : (std::min)(baseline.Rtao.RaysPerPixel, kRtaoRays[ao]);
    }

    // ---- Chromatic aberration: on is the level's own look, off removes it.
    mChromaticAberrationSettings = baseline.ChromaticAberration;
    mChromaticAberrationSettings.Enabled = values["chromatic-aberration"] != "off";

    // ---- Bloom. Off removes it; the tiers set the cost of whichever method the level
    // uses, with High the level's own. Low swaps the FFT convolution for the mip chain,
    // which keeps the glow but loses the glare shape. A level without bloom stays without.
    const int bloom = Tier(values, "bloom-quality");
    auto& bloomSettings = mBloomSettings;
    bloomSettings = baseline.Bloom;
    bloomSettings.Enabled = bloom >= 0 && baseline.Bloom.Enabled;
    const bool fftBloom = baseline.Bloom.Method == BloomMethod::FftConvolution;
    switch (bloom)
    {
    case 0: bloomSettings.Method = BloomMethod::MipChain; bloomSettings.MipLevels = (std::min)(baseline.Bloom.MipLevels, 4); break;
    case 1: if (fftBloom) bloomSettings.FftResolution = (std::min)(baseline.Bloom.FftResolution, 256); break;
    case 3: if (fftBloom) bloomSettings.FftResolution = 1024; else bloomSettings.MipLevels = (std::min)(baseline.Bloom.MipLevels + 1, 8); break;
    default: break;
    }

    // ---- Lens flares: how many ghosts are traced, how finely and in how many
    // wavelengths. Off removes them; a level authored without flares stays without.
    const int flares = Tier(values, "lens-flare-quality");
    auto& flareSettings = mLensFlareSettings;
    flareSettings = baseline.LensFlare;
    flareSettings.Enabled = flares >= 0 && baseline.LensFlare.Enabled;
    switch (flares)
    {
    case 0: flareSettings.MaxGhosts = (std::max)(8, baseline.LensFlare.MaxGhosts / 4); flareSettings.RayGridSize = (std::min)(baseline.LensFlare.RayGridSize, 16);
            flareSettings.Wavelengths = 1; flareSettings.MaxLights = (std::min)(baseline.LensFlare.MaxLights, 1); break;
    case 1: flareSettings.MaxGhosts = (std::max)(8, baseline.LensFlare.MaxGhosts / 2); flareSettings.RayGridSize = (std::min)(baseline.LensFlare.RayGridSize, 24);
            flareSettings.MaxLights = (std::min)(baseline.LensFlare.MaxLights, 2); break;
    case 3: flareSettings.RayGridSize = (std::min)(64, baseline.LensFlare.RayGridSize * 3 / 2); flareSettings.Wavelengths = (std::max)(baseline.LensFlare.Wavelengths, 6); break;
    default: break;
    }

    // ---- Brightness and gamma, 0-100 sliders with 50 the level's own look: brightness
    // spans -2..+2 stops of exposure, gamma 0.5..2 (see GetEffectiveAgxSettings).
    mPlayerBrightnessStops = (SliderPercent(values, "brightness") - 50) / 25.0f;
    mPlayerDisplayGamma = std::exp2((SliderPercent(values, "gamma") - 50) / 50.0f);

    // ---- Statistics overlay.
    mShowStatisticsOverlay = values["show-statistics"] == "on";

    auto& fog = mVolumetricFogSettings;
    // Off removes the fog; any other tier keeps whether the level has fog at all.
    fog.Enabled = Tier(values, "fog-quality") >= 0 && baseline.Fog.Enabled;
    const int tile = baseline.Fog.FroxelTileSize, slices = baseline.Fog.DepthSlices;
    switch (Tier(values, "fog-quality"))
    {
    case 0: fog.FroxelTileSize = std::clamp(tile * 2, 4, 32); fog.DepthSlices = (std::max)(16, slices / 2); break;
    case 1: fog.FroxelTileSize = std::clamp(tile * 3 / 2, 4, 32); fog.DepthSlices = (std::max)(16, slices * 3 / 4); break;
    case 3: fog.FroxelTileSize = std::clamp(tile * 3 / 4, 4, 32); fog.DepthSlices = (std::min)(128, slices * 2); break;
    default: fog.FroxelTileSize = tile; fog.DepthSlices = slices; break;
    }

    // ---- Shadows: point-light cube resolution and filter width. Four shadowed lights
    // of six faces each, so 2048 is already 384 MB of depth.
    // Off: nothing casts (see DX12SceneRenderer::mShadowsEnabled); the tiers below only
    // size the point-light cube maps.
    mShadowsEnabled = Tier(values, "shadow-quality") >= 0;
    auto& shadows = mPointShadowSettings;
    switch (Tier(values, "shadow-quality"))
    {
    case 0: shadows.MapSize = 512; shadows.FilterRadius = 1; break;
    case 1: shadows.MapSize = 1024; shadows.FilterRadius = 1; break;
    case 3: shadows.MapSize = 2048; shadows.FilterRadius = (std::max)(baseline.PointShadows.FilterRadius, 3); break;
    default: shadows.MapSize = baseline.PointShadows.MapSize; shadows.FilterRadius = baseline.PointShadows.FilterRadius; break;
    }
    // Ray-traced shadows: Off keeps the shadow maps, Sun traces the sun, Sun + Lights
    // also the point, spot and rect lights. Tuning (sun size, softness, rays) stays the
    // level's. They go with Shadows Off, and need a GPU that can ray trace.
    std::string& rtShadows = values["rt-shadows"];
    if (rtShadows != "off" && rtShadows != "sun" && rtShadows != "all") rtShadows = "off";
    if (rtShadows != "off" && !IsRayTracingSupported())
    {
        notes.push_back("Ray-traced shadows need a GPU with DirectX Raytracing; using shadow maps.");
        rtShadows = "off";
    }
    mRtShadowSettings = baseline.RtShadows;
    mRtShadowSettings.DebugView = 0;
    mRtShadowSettings.Enabled = mShadowsEnabled && rtShadows != "off";
    mRtShadowSettings.LocalLights = rtShadows == "all";

    // The sun's virtual shadow map: a coarser level choice needs a quarter of the pages
    // per step, so the pool shrinks with it.
    auto& sunShadows = mVirtualShadowMapSettings;
    sunShadows = baseline.VirtualShadows;
    // Off: the sun falls back to its single shadow map and local lights to the cube
    // maps sized above (VirtualShadowMapSettings::Enabled).
    std::string& virtualShadows = values["virtual-shadows"];
    if (virtualShadows != "off") virtualShadows = "on";
    sunShadows.Enabled = virtualShadows == "on";
    sunShadows.DebugView = 0;
    switch (Tier(values, "shadow-quality"))
    {
    case 0: sunShadows.ResolutionLodBias = baseline.VirtualShadows.ResolutionLodBias + 1.0f;
            sunShadows.PhysicalPages = (std::min)(baseline.VirtualShadows.PhysicalPages, 1024); break;
    case 1: sunShadows.ResolutionLodBias = baseline.VirtualShadows.ResolutionLodBias + 0.5f; break;
    case 3: sunShadows.ResolutionLodBias = baseline.VirtualShadows.ResolutionLodBias - 0.5f;
            sunShadows.PhysicalPages = (std::max)(baseline.VirtualShadows.PhysicalPages, 4096); break;
    default: break;
    }
    // ...and how many lights the shadow budget lets cast at once (only when the level
    // leaves the budget on; a level that turned it off keeps every shadow).
    switch (Tier(values, "shadow-quality"))
    {
    case 0: sunShadows.MaxShadowedLights = (std::min)(baseline.VirtualShadows.MaxShadowedLights, 4); break;
    case 1: sunShadows.MaxShadowedLights = (std::min)(baseline.VirtualShadows.MaxShadowedLights, 6); break;
    case 3: sunShadows.MaxShadowedLights = (std::min)(baseline.VirtualShadows.MaxShadowedLights + 4, 16); break;
    default: break;
    }

    // ---- Textures: a mip bias on top of the sharpening one. Positive samples smaller
    // mips (softer, less bandwidth); Ultra sharpens a little further.
    static constexpr float kTextureBias[] = {2.0f, 1.0f, 0.0f, -0.5f};
    mTextureQualityMipBias = kTextureBias[std::clamp(Tier(values, "texture-quality"), 0, 3)];

    // ---- Models: how soon the coarser generated LODs take over.
    static constexpr float kLodScale[] = {2.0f, 1.5f, 1.0f, 0.6f};
    mEntityMeshRenderer.SetLodDistanceScale(kLodScale[std::clamp(Tier(values, "model-quality"), 0, 3)]);

    // ---- Audio.
    if (mAudioManager)
        mAudioManager->SetVolumes(VolumeGain(values, "overall-volume"), VolumeGain(values, "sound-volume"), VolumeGain(values, "music-volume"));

    // ---- Window. Only the standalone game owns its window; the editor keeps its own.
    if (QtUi::IsStandaloneGame())
    {
        if (std::string note = ApplyDisplayMode(values); !note.empty())
            notes.push_back(std::move(note));
    }

    if (persist)
    {
        std::string error;
        if (!SaveSettingsFile(values, error))
        {
            PteroLog::Write(PteroLog::Level::Warning, "Game", ("Could not save player settings: " + error).c_str());
            notes.push_back("They could not be saved and last only for this session.");
        }
    }

    // Documents built from here on are in the new language; the menus rebuild their page.
    UiLocalization::SetLanguage(values["ui-language"]);
    values["ui-language"] = UiLocalization::GetLanguage();

    std::string status = "Settings applied.";
    for (const auto& note : notes) status += " " + note;
    return status;
}
