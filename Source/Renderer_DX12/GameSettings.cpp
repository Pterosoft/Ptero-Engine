#include "pch.h"
#include "DX12SceneRenderer.h"
#include "../QtUi/QtUi.h"
#include "../../Data/UI/Farkle/FarkleMenuController.h"
#include "System/PteroLog.h"
#include "AudioManager.h"

#include <ShlObj.h>

#include <algorithm>
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
    float VolumeGain(const Values& values, const char* key)
    {
        const auto it = values.find(key);
        int percent = 100;
        if (it != values.end()) { try { percent = std::stoi(it->second); } catch (...) {} }
        const float linear = std::clamp(percent, 0, 100) / 100.0f;
        return linear * linear;
    }
}

void ApplySavedGameDisplayMode()
{
    // Before the renderer's first frame, so the window is already the saved size and the
    // swap chain is created for it: the intro videos and the level must never be seen in
    // a window that later jumps to fullscreen. Only the Settings.ini keys the menus know
    // count; the rest of the file is applied when the game starts.
    FarkleMenuController menus;
    menus.Load(LoadSettingsFile());
    Values values = menus.Saved();
    ApplyDisplayMode(values);
}

void DX12SceneRenderer::BeginGameSettings(bool standalone)
{
    auto& baseline = mGameSettingsBaseline;
    baseline.Taa = mTaaSettings; baseline.Smaa = mSmaaSettings; baseline.Msaa = mMsaaSettings;
    baseline.Dlss = mDlssSettings; baseline.Fsr = mFsrSettings;
    baseline.GiMode = mGlobalIlluminationMode; baseline.Rtgi = mRtgiSettings;
    baseline.Cascades = mRadianceCascadesSettings; baseline.Fog = mVolumetricFogSettings;
    baseline.PointShadows = mPointShadowSettings;
    baseline.Gtao = mGtaoSettings; baseline.Rtao = mRtaoSettings;
    baseline.ChromaticAberration = mChromaticAberrationSettings;
    mGameSettingsActive = true;
    mGameSettingsApplied = false;

    FarkleMenuController& menus = mRmlUiRenderer.GetFarkleMenus();
    // Every session reads the file, so the editor's test sessions use the same winning
    // score as the player's; only the standalone game applies the rest.
    menus.Load(LoadSettingsFile());
    menus.Options.clear();
    if (standalone)
    {
        if (auto resolutions = MonitorResolutions(); !resolutions.empty())
            menus.Options["resolution"] = std::move(resolutions);
        FarkleMenuController::Choices upscalers{{"none", "None"}};
        if (mDlssRenderer.IsAvailable()) upscalers.push_back({"dlss", "DLSS"});
        if (mFsrRenderer.IsAvailable()) upscalers.push_back({"fsr", "FSR"});
        menus.Options["upscaler"] = std::move(upscalers);
        FarkleMenuController::Choices frameGeneration{{"off", "Off"}};
        if (mFrameGeneration.IsApiAvailable()) frameGeneration.push_back({"on", "On"});
        menus.Options["fsr-frame-generation"] = std::move(frameGeneration);
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
    mTaaSettings = baseline.Taa; mSmaaSettings = baseline.Smaa; mMsaaSettings = baseline.Msaa;
    mDlssSettings = baseline.Dlss; mFsrSettings = baseline.Fsr;
    mGlobalIlluminationMode = baseline.GiMode; mRtgiSettings = baseline.Rtgi;
    mRadianceCascadesSettings = baseline.Cascades; mVolumetricFogSettings = baseline.Fog;
    mPointShadowSettings = baseline.PointShadows;
    mGtaoSettings = baseline.Gtao; mRtaoSettings = baseline.Rtao;
    mChromaticAberrationSettings = baseline.ChromaticAberration;
    mShadowsEnabled = true;
    mUpscalerSharpness = -1.0f;
    mShowStatisticsOverlay = false;
    mTaaSettings.ResetHistory = true; mDlssSettings.ResetHistory = true; mFsrSettings.ResetHistory = true;
    mTextureQualityMipBias = 0.0f;
    mEntityMeshRenderer.SetLodDistanceScale(1.0f);
    if (mAudioManager) mAudioManager->SetVolumes(1.0f, 1.0f, 1.0f);
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
    if (upscaler != "dlss" && upscaler != "fsr") upscaler = "none";
    values["upscaler"] = upscaler;

    const std::string antiAliasing = values["anti-aliasing"];
    mTaaSettings = baseline.Taa; mSmaaSettings = baseline.Smaa; mMsaaSettings = baseline.Msaa;
    // DLSS and FSR are temporal themselves; TAA on top re-jitters an already resolved image
    // and makes it shimmer, so it is off whenever an upscaler is.
    mTaaSettings.Enabled = antiAliasing == "taa" && upscaler == "none";
    mSmaaSettings.Enabled = antiAliasing == "smaa";
    // MSAA multisamples the G-Buffer, which an upscaler would only discard.
    mMsaaSettings.Enabled = antiAliasing == "msaa" && upscaler == "none";
    mTaaSettings.ResetHistory = true;

    // Upscaler quality: the same five steps for both, in each one's own mode numbering
    // (DlssSettings.h / FsrSettings.h). Native renders at full size and keeps only the AA.
    std::string& quality = values["upscaler-quality"];
    static const char* const kQualities[] = { "native", "quality", "balanced", "performance", "ultra-performance" };
    static constexpr int kDlssModes[] = { 6, 3, 2, 1, 4 };
    static constexpr int kFsrModes[] = { 0, 1, 2, 3, 4 };
    int qualityIndex = 1;
    for (int i = 0; i < 5; ++i)
        if (quality == kQualities[i]) qualityIndex = i;
    quality = kQualities[qualityIndex];

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

    // Upscaler sharpening. FSR sharpens with its own RCAS; DLSS has none of its own any
    // more, so after DLSS the engine's image sharpen pass carries the same amount (see
    // DX12SceneRenderer's image sharpen pass). No effect without an upscaler.
    const std::string& sharpening = values["upscaler-sharpness"];
    const float sharpness = sharpening == "off" ? 0.0f : sharpening == "low" ? 0.25f : sharpening == "high" ? 0.8f : 0.5f;
    mFsrSettings.Sharpening = sharpness > 0.0f;
    mFsrSettings.Sharpness = sharpness;
    mUpscalerSharpness = sharpness;

    // ---- Global illumination. Off switches it off; the tiers scale whichever technique
    // the level uses. A level authored without GI stays without it.
    const int gi = Tier(values, "gi-quality");
    mGlobalIlluminationMode = gi < 0 ? GlobalIlluminationMode::Disabled : baseline.GiMode;
    mRtgiSettings = baseline.Rtgi;
    mRadianceCascadesSettings = baseline.Cascades;
    auto& rtgi = mRtgiSettings;
    auto& cascades = mRadianceCascadesSettings;
    const unsigned rays = baseline.Cascades.RaysPerProbe, spacing = baseline.Cascades.ProbeSpacingBase;
    switch (gi)
    {
    case 0:
        rtgi.RaysPerPixel = 1; rtgi.MaxBounces = 1; rtgi.SpecularEnabled = false;
        cascades.RaysPerProbe = (std::max)(2u, rays / 4); cascades.ProbeSpacingBase = spacing * 2;
        break;
    case 1:
        rtgi.RaysPerPixel = (std::max)(1, baseline.Rtgi.RaysPerPixel / 2); rtgi.MaxBounces = (std::max)(1, baseline.Rtgi.MaxBounces - 1);
        cascades.RaysPerProbe = (std::max)(2u, rays / 2); cascades.ProbeSpacingBase = spacing * 3 / 2;
        break;
    case 3:
        rtgi.RaysPerPixel = (std::min)(8, baseline.Rtgi.RaysPerPixel * 2); rtgi.MaxBounces = (std::min)(4, baseline.Rtgi.MaxBounces + 1);
        cascades.RaysPerProbe = (std::min)(16u, rays * 2); cascades.ProbeSpacingBase = (std::max)(4u, spacing * 3 / 4);
        break;
    default:
        break;
    }

    // ---- Volumetric fog: froxel resolution only. Whether the level has fog at all,
    // and what it looks like, stays the level's.
    // ---- Ambient occlusion. Off switches off whichever AO the level uses (GTAO or
    // RTAO); the tiers set GTAO's sample quality, with High the level's own.
    const int ao = Tier(values, "ao-quality");
    mGtaoSettings = baseline.Gtao;
    mRtaoSettings = baseline.Rtao;
    if (ao < 0)
    {
        mGtaoSettings.Enabled = false;
        mRtaoSettings.Enabled = false;
    }
    else if (ao != 2)
    {
        mGtaoSettings.QualityLevel = ao == 3 ? 3 : ao;
    }

    // ---- Chromatic aberration: on is the level's own look, off removes it.
    mChromaticAberrationSettings = baseline.ChromaticAberration;
    mChromaticAberrationSettings.Enabled = values["chromatic-aberration"] != "off";

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

    std::string status = "Settings applied.";
    for (const auto& note : notes) status += " " + note;
    return status;
}
