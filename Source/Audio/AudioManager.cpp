#include "AudioManager.h"

#include <windows.h>
#include "fmod_studio.hpp"
#include "fmod.hpp"
#include "fmod_errors.h"

#include "System/DataFiles.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace
{
    FMOD_VECTOR ToFmodVector(const float x, const float y, const float z)
    {
        FMOD_VECTOR vector{};
        vector.x = x;
        vector.y = y;
        vector.z = z;
        return vector;
    }

    bool FmodOk(FMOD_RESULT result, std::string& outError, const char* context)
    {
        if (result != FMOD_OK)
        {
            outError = std::string(context) + ": " + FMOD_ErrorString(result);
            return false;
        }
        return true;
    }

    FMOD_3D_ATTRIBUTES Make3DAttributes(
        const float positionX,
        const float positionY,
        const float positionZ,
        const float forwardX,
        const float forwardY,
        const float forwardZ,
        const float upX,
        const float upY,
        const float upZ)
    {
        FMOD_3D_ATTRIBUTES attributes{};
        attributes.position = ToFmodVector(positionX, positionY, positionZ);
        attributes.forward = ToFmodVector(forwardX, forwardY, forwardZ);
        attributes.up = ToFmodVector(upX, upY, upZ);
        return attributes;
    }

    std::filesystem::path ResolveResonancePluginPath()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
            return {};

        std::filesystem::path current = std::filesystem::path(exePath).parent_path();
        const std::filesystem::path deployedPlugin = current / L"resonanceaudio.dll";
        std::error_code ec;
        if (std::filesystem::is_regular_file(deployedPlugin, ec))
            return deployedPlugin;

        while (!current.empty())
        {
            const std::filesystem::path sourceBuild =
                current / L"Source" / L"SDKs" / L"resonance-audio" / L"build-ptero" /
                L"platforms" / L"fmod" / L"Release" / L"resonanceaudio.dll";
            if (std::filesystem::is_regular_file(sourceBuild, ec))
                return std::filesystem::weakly_canonical(sourceBuild);

            const std::filesystem::path packagedPlugin =
                current / L"Source" / L"SDKs" / L"fmod" / L"plugins" /
                L"resonance_audio" / L"lib" / L"x64" / L"resonanceaudio.dll";
            if (std::filesystem::is_regular_file(packagedPlugin, ec))
                return std::filesystem::weakly_canonical(packagedPlugin);

            const std::filesystem::path parent = current.parent_path();
            if (parent == current)
                break;
            current = parent;
        }

        return {};
    }
}

bool AudioManager::InitializeResonanceAudio(FMOD::System* coreSystem, std::string& outError)
{
    const std::filesystem::path pluginPath = ResolveResonancePluginPath();
    if (pluginPath.empty())
    {
        outError = "Resonance Audio FMOD plugin was not found.";
        return false;
    }

    FMOD_RESULT result = coreSystem->loadPlugin(pluginPath.string().c_str(), &m_resonancePluginHandle, 0);
    if (!FmodOk(result, outError, "FMOD::System::loadPlugin(resonanceaudio.dll)"))
        return false;

    unsigned int listenerPluginHandle = 0;
    int nestedPluginCount = 0;
    result = coreSystem->getNumNestedPlugins(m_resonancePluginHandle, &nestedPluginCount);
    if (!FmodOk(result, outError, "FMOD::System::getNumNestedPlugins(Resonance Audio)"))
        return false;

    for (int pluginIndex = 0; pluginIndex < nestedPluginCount; ++pluginIndex)
    {
        unsigned int nestedHandle = 0;
        if (coreSystem->getNestedPlugin(m_resonancePluginHandle, pluginIndex, &nestedHandle) != FMOD_OK)
            continue;

        FMOD_PLUGINTYPE pluginType = FMOD_PLUGINTYPE_DSP;
        char pluginName[128] = {};
        unsigned int pluginVersion = 0;
        if (coreSystem->getPluginInfo(
                nestedHandle,
                &pluginType,
                pluginName,
                static_cast<int>(std::size(pluginName)),
                &pluginVersion) != FMOD_OK)
        {
            continue;
        }

        if (std::string(pluginName) == "Resonance Audio Listener")
            listenerPluginHandle = nestedHandle;
        else if (std::string(pluginName) == "Resonance Audio Source")
            m_resonanceSourcePluginHandle = nestedHandle;
    }

    if (listenerPluginHandle == 0 || m_resonanceSourcePluginHandle == 0)
    {
        outError = "resonanceaudio.dll did not expose the required Listener and Source DSPs.";
        return false;
    }

    // Loading the plugin is the whole job. The engine deliberately installs no Resonance
    // DSP of its own, because there is no safe place to put one: a Listener on the master
    // channel group does not mix with what passes through it, it replaces it. Measured on
    // the current banks, adding one dropped an unspatialised music event from 13.7% to
    // 3.2% of full scale - it only reproduces what Sources fed into the soundfield, and
    // music and UI are deliberately not routed through a Source.
    //
    // So the Resonance chain lives entirely in the FMOD project, per event, which is also
    // where its gain staging has to be. The spatialised events each carry a matched
    // Source and Listener; everything else carries neither.
    (void)listenerPluginHandle;
    m_resonanceAudioAvailable = true;
    m_resonanceAudioStatus =
        "Resonance Audio plugin loaded from " + pluginPath.string() +
        ". Spatialisation is authored per event in the FMOD banks; the engine installs no "
        "Resonance DSPs of its own.";
    return true;
}

bool AudioManager::PlayEventByPath(const std::string& eventPath)
{
    if (!m_initialized || eventPath.empty())
        return false;

    FMOD::Studio::EventDescription* desc = nullptr;
    FMOD_RESULT result = m_studioSystem->getEvent(eventPath.c_str(), &desc);
    if (result != FMOD_OK || !desc)
        return false;

    FMOD::Studio::EventInstance* inst = nullptr;
    result = desc->createInstance(&inst);
    if (result != FMOD_OK || !inst)
        return false;


    bool is3D = false;
    if (desc->is3D(&is3D) == FMOD_OK && is3D)
    {
        const float previewDistance = 1.0f;
        const FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
            m_listenerPosX + (m_listenerFwdX * previewDistance),
            m_listenerPosY + (m_listenerFwdY * previewDistance),
            m_listenerPosZ + (m_listenerFwdZ * previewDistance),
            0.0f,
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f);
        inst->set3DAttributes(&attributes);
    }

    inst->setVolume(m_soundVolume);
    result = inst->start();
    inst->release();
    return result == FMOD_OK;
}

std::string AudioManager::ResolveEventPath(const std::string& nameOrPath) const
{
    if (nameOrPath.empty())
        return {};

    // Exact match first, so a caller that already knows the full path always wins.
    for (const AudioEvent& candidate : m_events)
        if (candidate.Path == nameOrPath)
            return candidate.Path;

    // Otherwise treat the argument as a leaf name and match the last path component.
    // Case-insensitive, because FMOD Studio event names are authored by hand.
    const auto equalsIgnoringCase = [](const std::string& left, const std::string& right)
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t i = 0; i < left.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i])))
                return false;
        return true;
    };

    for (const AudioEvent& candidate : m_events)
    {
        const std::size_t slash = candidate.Path.find_last_of('/');
        const std::string leaf = (slash == std::string::npos) ? candidate.Path : candidate.Path.substr(slash + 1);
        if (equalsIgnoringCase(leaf, nameOrPath))
            return candidate.Path;
    }

    return {};
}

void AudioManager::PlaceAtListener(FMOD::Studio::EventInstance* instance) const
{
    if (instance == nullptr)
        return;

    const FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
        m_listenerPosX, m_listenerPosY, m_listenerPosZ,
        m_listenerFwdX, m_listenerFwdY, m_listenerFwdZ,
        m_listenerUpX, m_listenerUpY, m_listenerUpZ);
    instance->set3DAttributes(&attributes);
}

// Deliberately not routed through PlayEventByPath: that one drops the instance a metre
// in front of the listener for the editor's preview button, which is a fabricated
// position no game sound should inherit.
bool AudioManager::PlayOneShotByName(const std::string& nameOrPath)
{
    if (!m_initialized)
        return false;

    const std::string path = ResolveEventPath(nameOrPath);
    if (path.empty())
        return false;

    FMOD::Studio::EventDescription* desc = nullptr;
    if (m_studioSystem->getEvent(path.c_str(), &desc) != FMOD_OK || !desc)
        return false;

    FMOD::Studio::EventInstance* inst = nullptr;
    if (desc->createInstance(&inst) != FMOD_OK || !inst)
        return false;

    PlaceAtListener(inst);

    inst->setVolume(m_soundVolume);
    const FMOD_RESULT result = inst->start();
    inst->release();
    return result == FMOD_OK;
}

bool AudioManager::PlayMusicByName(const std::string& nameOrPath)
{
    if (!m_initialized)
        return false;

    const std::string path = ResolveEventPath(nameOrPath);
    if (path.empty())
        return false;

    FMOD::Studio::EventDescription* desc = nullptr;
    if (m_studioSystem->getEvent(path.c_str(), &desc) != FMOD_OK || !desc)
        return false;

    FMOD::Studio::EventInstance* inst = nullptr;
    if (desc->createInstance(&inst) != FMOD_OK || !inst)
        return false;

    PlaceAtListener(inst);

    // Only swap the channel once the replacement is actually running, so a failed
    // start leaves the current track playing instead of silence.
    inst->setVolume(m_musicVolume);
    if (inst->start() != FMOD_OK)
    {
        inst->release();
        return false;
    }

    StopMusic();
    m_musicInstance = inst;
    return true;
}

bool AudioManager::IsMusicPlaying()
{
    if (!m_musicInstance)
        return false;

    FMOD_STUDIO_PLAYBACK_STATE state = FMOD_STUDIO_PLAYBACK_STOPPED;
    if (m_musicInstance->getPlaybackState(&state) != FMOD_OK || state == FMOD_STUDIO_PLAYBACK_STOPPED)
    {
        // The track ran out. Release it here so the caller's next poll is a plain
        // null check and the instance does not leak until shutdown.
        StopMusic();
        return false;
    }
    return true;
}

void AudioManager::StopMusic()
{
    if (!m_musicInstance)
        return;
    m_musicInstance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
    m_musicInstance->release();
    m_musicInstance = nullptr;
}

void AudioManager::SetVolumes(float overall, float sound, float music)
{
    m_overallVolume = std::clamp(overall, 0.0f, 1.0f);
    m_soundVolume = std::clamp(sound, 0.0f, 1.0f);
    m_musicVolume = std::clamp(music, 0.0f, 1.0f);
    if (!m_initialized)
        return;

    FMOD::Studio::Bus* masterBus = nullptr;
    if (m_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
        masterBus->setVolume(m_overallVolume);
    if (m_musicInstance)
        m_musicInstance->setVolume(m_musicVolume);
    // Instances started from here on pick the level up as they start; these are the
    // long-lived ones already playing. Fire-and-forget one-shots are too short to matter.
    for (FMOD::Studio::EventInstance* instance : m_instances)
        if (instance && instance->isValid())
            instance->setVolume(m_soundVolume);
    for (const EmitterState& emitter : m_emitters)
        if (auto* instance = static_cast<FMOD::Studio::EventInstance*>(emitter.Instance); instance && instance->isValid())
            instance->setVolume(m_soundVolume);
}

// ---------------------------------------------------------------------------
AudioManager::AudioManager() = default;
AudioManager::~AudioManager()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
std::filesystem::path AudioManager::ResolveAudioDataPath()
{
    // The repository's Data folder, or a packaged game's virtual one (DataFiles.h).
    const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
    if (dataDirectory.empty())
        return {};

    const std::filesystem::path audioDirectory = dataDirectory / L"Audio";
    if (!DataFiles::IsDirectory(audioDirectory))
        return {};
    if (DataFiles::IsPackaged())
        return audioDirectory;

    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(audioDirectory, ec);
    return ec ? audioDirectory : canonical;
}

// ---------------------------------------------------------------------------
bool AudioManager::Initialize(std::string& outError)
{
    if (m_initialized)
        return true;

    m_audioDataPath.clear();
    m_loadedBanks.clear();
    m_bankLoadFailures.clear();
    m_lastStatus.clear();

    FMOD_RESULT result;

    // Create the Studio System.
    result = FMOD::Studio::System::create(&m_studioSystem);
    if (!FmodOk(result, outError, "FMOD::Studio::System::create"))
        return false;

    FMOD::System* coreSystem = nullptr;
    result = m_studioSystem->getCoreSystem(&coreSystem);
    if (!FmodOk(result, outError, "FMOD::Studio::System::getCoreSystem") || !coreSystem)
    {
        m_studioSystem->release();
        m_studioSystem = nullptr;
        return false;
    }

    // Initialise with sensible defaults (512 channels, live update in debug).
    FMOD_STUDIO_INITFLAGS studioFlags = FMOD_STUDIO_INIT_NORMAL | FMOD_STUDIO_INIT_ALLOW_MISSING_PLUGINS;
#ifdef PTERO_DEBUG
    studioFlags |= FMOD_STUDIO_INIT_LIVEUPDATE;
#endif

    result = m_studioSystem->initialize(512, studioFlags, FMOD_INIT_NORMAL, nullptr);
    if (!FmodOk(result, outError, "FMOD::Studio::System::initialize"))
    {
        m_studioSystem->release();
        m_studioSystem = nullptr;
        return false;
    }

    m_coreSystem = coreSystem;
    if (!InitializeResonanceAudio(coreSystem, outError))
    {
        m_studioSystem->release();
        m_studioSystem = nullptr;
        m_coreSystem = nullptr;
        return false;
    }

    FMOD::Studio::Bus* masterBus = nullptr;
    if (m_studioSystem->getBus("bus:/", &masterBus) == FMOD_OK && masterBus)
    {
        masterBus->setMute(false);
        masterBus->setPaused(false);
        masterBus->setVolume(m_overallVolume);
    }

    const FMOD_3D_ATTRIBUTES defaultListenerAttributes = Make3DAttributes(
        m_listenerPosX,
        m_listenerPosY,
        m_listenerPosZ,
        m_listenerFwdX,
        m_listenerFwdY,
        m_listenerFwdZ,
        m_listenerUpX,
        m_listenerUpY,
        m_listenerUpZ);
    m_studioSystem->setListenerAttributes(0, &defaultListenerAttributes);

    // Load banks from Data/Audio.
    m_audioDataPath = ResolveAudioDataPath();
    if (!m_audioDataPath.empty())
    {
        std::vector<std::filesystem::path> bankPaths;
        for (const std::filesystem::path& p : DataFiles::ListFiles(m_audioDataPath, true))
        {
            if (_wcsicmp(p.extension().c_str(), L".bank") != 0) continue;

            bankPaths.push_back(p);
        }

        std::sort(bankPaths.begin(), bankPaths.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
        {
            const std::wstring aName = a.filename().wstring();
            const std::wstring bName = b.filename().wstring();
            const bool aIsStrings = aName.find(L".strings.bank") != std::wstring::npos;
            const bool bIsStrings = bName.find(L".strings.bank") != std::wstring::npos;
            if (aIsStrings != bIsStrings)
                return aIsStrings;
            return _wcsicmp(aName.c_str(), bName.c_str()) < 0;
        });

        for (const auto& p : bankPaths)
        {
            const std::filesystem::path relativePath = p.lexically_relative(m_audioDataPath);
            const std::string displayPath = relativePath.empty() ? p.string() : relativePath.string();
            const std::wstring fileName = p.filename().wstring();
            const bool isStringsBank = fileName.find(L".strings.bank") != std::wstring::npos;

            FMOD::Studio::Bank* bank = nullptr;
            FMOD_RESULT loadResult = FMOD_ERR_FILE_NOTFOUND;
            if (DataFiles::IsPackaged())
            {
                // Packaged banks exist only inside Audio.ppak. LOAD_MEMORY makes FMOD
                // take its own copy, so the decrypted bytes can go as soon as it returns.
                std::vector<std::uint8_t> bankBytes;
                if (DataFiles::ReadBytes(p, bankBytes) && !bankBytes.empty())
                {
                    loadResult = m_studioSystem->loadBankMemory(
                        reinterpret_cast<const char*>(bankBytes.data()), static_cast<int>(bankBytes.size()),
                        FMOD_STUDIO_LOAD_MEMORY, FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);
                }
            }
            else
            {
                const std::string pathUtf8 = p.string();
                loadResult = m_studioSystem->loadBankFile(
                    pathUtf8.c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);
            }
            if (loadResult == FMOD_OK && bank != nullptr)
            {
                m_banks.push_back(bank);
                m_loadedBanks.push_back(displayPath);

                if (!isStringsBank)
                {
                    const FMOD_RESULT sampleLoadResult = bank->loadSampleData();
                    if (sampleLoadResult != FMOD_OK)
                    {
                        m_bankLoadFailures.push_back(displayPath + " sample data: " + FMOD_ErrorString(sampleLoadResult));
                    }
                }
            }
            else
            {
                std::string failure = displayPath + ": " + FMOD_ErrorString(loadResult);
                if (loadResult == FMOD_ERR_PLUGIN_MISSING)
                {
                    failure += " (The bank references an FMOD effect that is not available. Resonance Audio is loaded as resonanceaudio.dll before bank loading.)";
                }
                m_bankLoadFailures.push_back(std::move(failure));
            }
        }

        if (bankPaths.empty())
        {
            m_lastStatus = "No .bank files were found in the resolved Data/Audio folder.";
        }
    }
    else
    {
        m_lastStatus = "Could not resolve a Data/Audio folder relative to the executable.";
    }

    m_studioSystem->flushCommands();
    m_studioSystem->flushSampleLoading();

    m_initialized = true;
    RefreshEventList();

    if (m_lastStatus.empty())
    {
        std::ostringstream statusBuilder;
        statusBuilder << "Loaded " << m_loadedBanks.size() << " bank(s) and discovered " << m_events.size()
                      << " event(s). Resonance Audio is active.";
        if (!m_bankLoadFailures.empty())
        {
            statusBuilder << " " << m_bankLoadFailures.size() << " bank(s) failed to load.";
        }
        m_lastStatus = statusBuilder.str();
    }

    return true;
}

// ---------------------------------------------------------------------------
void AudioManager::Update()
{
    if (!m_initialized || !m_studioSystem)
        return;

    m_studioSystem->update();
}

// ---------------------------------------------------------------------------
void AudioManager::Shutdown()
{
    if (!m_initialized)
        return;

    StopAll();

    for (FMOD::Studio::EventInstance* inst : m_instances)
        if (inst)
            inst->release();
    m_instances.clear();

    for (FMOD::Studio::Bank* bank : m_banks)
        bank->unload();
    m_banks.clear();

    if (m_studioSystem)
    {
        m_studioSystem->release();
        m_studioSystem = nullptr;
    }

    m_coreSystem = nullptr;
    m_resonanceAudioAvailable = false;
    m_resonancePluginHandle = 0;
    m_resonanceSourcePluginHandle = 0;
    m_events.clear();
    m_initialized = false;
}

// ---------------------------------------------------------------------------
void AudioManager::RefreshEventList()
{
    m_events.clear();
    m_instances.clear();

    if (!m_studioSystem)
        return;

    for (FMOD::Studio::Bank* bank : m_banks)
    {
        int count = 0;
        if (bank->getEventCount(&count) != FMOD_OK || count <= 0)
            continue;

        std::vector<FMOD::Studio::EventDescription*> descs(static_cast<size_t>(count));
        int fetched = 0;
        if (bank->getEventList(descs.data(), count, &fetched) != FMOD_OK)
            continue;

        for (int i = 0; i < fetched; ++i)
        {
            if (!descs[i]) continue;
            char path[1024] = {};
            int retrieved = 0;
            if (descs[i]->getPath(path, static_cast<int>(std::size(path)), &retrieved) == FMOD_OK)
            {
                AudioEvent ev;
                ev.Path = path;
                ev.IsPlaying = false;
                const auto duplicateIt = std::find_if(m_events.begin(), m_events.end(), [&ev](const AudioEvent& existing)
                {
                    return existing.Path == ev.Path;
                });

                if (duplicateIt == m_events.end())
                    m_events.push_back(std::move(ev));
            }
        }
    }

    // Resize instance slots to match.
    m_instances.resize(m_events.size(), nullptr);
}

// ---------------------------------------------------------------------------
bool AudioManager::PlayEvent(int index)
{
    if (!m_initialized || index < 0 || index >= static_cast<int>(m_events.size()))
        return false;

    // Stop any previous instance for this event.
    StopEvent(index);

    FMOD::Studio::EventDescription* desc = nullptr;
    FMOD_RESULT result = m_studioSystem->getEvent(m_events[static_cast<size_t>(index)].Path.c_str(), &desc);
    if (result != FMOD_OK || !desc)
        return false;

    FMOD::Studio::EventInstance* inst = nullptr;
    result = desc->createInstance(&inst);
    if (result != FMOD_OK || !inst)
        return false;


    desc->loadSampleData();
    m_studioSystem->flushSampleLoading();

    bool is3D = false;
    if (desc->is3D(&is3D) == FMOD_OK && is3D)
    {
        const float previewDistance = 1.0f;
        const FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
            m_listenerPosX + (m_listenerFwdX * previewDistance),
            m_listenerPosY + (m_listenerFwdY * previewDistance),
            m_listenerPosZ + (m_listenerFwdZ * previewDistance),
            0.0f,
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f);
        inst->set3DAttributes(&attributes);
    }

    inst->setVolume(m_soundVolume);
    result = inst->start();
    if (result != FMOD_OK)
    {
        inst->release();
        return false;
    }

    m_instances[static_cast<size_t>(index)] = inst;
    m_events[static_cast<size_t>(index)].IsPlaying = true;
    return true;
}

// ---------------------------------------------------------------------------
bool AudioManager::StopEvent(int index)
{
    if (!m_initialized || index < 0 || index >= static_cast<int>(m_events.size()))
        return false;

    FMOD::Studio::EventInstance* inst = m_instances[static_cast<size_t>(index)];
    if (inst)
    {
        inst->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
        inst->release();
        m_instances[static_cast<size_t>(index)] = nullptr;
    }
    m_events[static_cast<size_t>(index)].IsPlaying = false;
    return true;
}

// ---------------------------------------------------------------------------
void AudioManager::StopAll()
{
    StopMusic();

    for (int i = 0; i < static_cast<int>(m_events.size()); ++i)
        StopEvent(i);

    for (EmitterState& emitter : m_emitters)
    {
        FMOD::Studio::EventInstance* instance = static_cast<FMOD::Studio::EventInstance*>(emitter.Instance);
        if (instance)
        {
            instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
            instance->release();
            emitter.Instance = nullptr;
        }
        emitter.Playing = false;
    }
}

void AudioManager::SetListenerTransform(
    float positionX,
    float positionY,
    float positionZ,
    float forwardX,
    float forwardY,
    float forwardZ,
    float upX,
    float upY,
    float upZ)
{
    if (!m_initialized || !m_studioSystem)
        return;

    // Cache the transform used by FMOD and the Resonance source DSPs.
    m_listenerPosX = positionX; m_listenerPosY = positionY; m_listenerPosZ = positionZ;
    m_listenerFwdX = forwardX;  m_listenerFwdY = forwardY;  m_listenerFwdZ = forwardZ;
    m_listenerUpX  = upX;       m_listenerUpY  = upY;       m_listenerUpZ  = upZ;

    FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
        positionX,
        positionY,
        positionZ,
        forwardX,
        forwardY,
        forwardZ,
        upX,
        upY,
        upZ);
    m_studioSystem->setListenerAttributes(0, &attributes);
}

// ---------------------------------------------------------------------------
AudioManager::EmitterHandle AudioManager::RegisterEmitter()
{
    for (int i = 0; i < static_cast<int>(m_emitters.size()); ++i)
    {
        if (!m_emitters[static_cast<size_t>(i)].Allocated)
        {
            m_emitters[static_cast<size_t>(i)] = EmitterState{};
            m_emitters[static_cast<size_t>(i)].Allocated = true;
            return EmitterHandle{ i };
        }
    }

    m_emitters.push_back(EmitterState{});
    m_emitters.back().Allocated = true;
    return EmitterHandle{ static_cast<int>(m_emitters.size() - 1) };
}

void AudioManager::UnregisterEmitter(EmitterHandle handle)
{
    if (!handle.IsValid() || handle.Value >= static_cast<int>(m_emitters.size()))
        return;

    StopEmitter(handle);
    m_emitters[static_cast<size_t>(handle.Value)] = EmitterState{};
}

bool AudioManager::SetEmitterEventPath(EmitterHandle handle, const std::string& eventPath)
{
    if (!handle.IsValid() || handle.Value >= static_cast<int>(m_emitters.size()))
        return false;

    EmitterState& emitter = m_emitters[static_cast<size_t>(handle.Value)];
    if (!emitter.Allocated)
        return false;

    if (emitter.Path == eventPath)
        return true;

    StopEmitter(handle);
    emitter.Path = eventPath;
    return true;
}

bool AudioManager::SetEmitterTransform(EmitterHandle handle, float positionX, float positionY, float positionZ)
{
    if (!handle.IsValid() || handle.Value >= static_cast<int>(m_emitters.size()))
        return false;

    EmitterState& emitter = m_emitters[static_cast<size_t>(handle.Value)];
    if (!emitter.Allocated)
        return false;

    emitter.PositionX = positionX;
    emitter.PositionY = positionY;
    emitter.PositionZ = positionZ;

    FMOD::Studio::EventInstance* instance = static_cast<FMOD::Studio::EventInstance*>(emitter.Instance);
    if (instance)
    {
        FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
            emitter.PositionX,
            emitter.PositionY,
            emitter.PositionZ,
            0.0f,
            1.0f,
            0.0f,
            0.0f,
            0.0f,
            1.0f);
        return instance->set3DAttributes(&attributes) == FMOD_OK;
    }

    return true;
}

bool AudioManager::PlayEmitter(EmitterHandle handle)
{
    if (!m_initialized || !handle.IsValid() || handle.Value >= static_cast<int>(m_emitters.size()))
        return false;

    EmitterState& emitter = m_emitters[static_cast<size_t>(handle.Value)];
    if (!emitter.Allocated || emitter.Path.empty())
        return false;

    if (emitter.Instance)
        return true;

    FMOD::Studio::EventDescription* desc = nullptr;
    if (m_studioSystem->getEvent(emitter.Path.c_str(), &desc) != FMOD_OK || !desc)
        return false;

    FMOD::Studio::EventInstance* instance = nullptr;
    if (desc->createInstance(&instance) != FMOD_OK || !instance)
        return false;


    desc->loadSampleData();
    m_studioSystem->flushSampleLoading();

    FMOD_3D_ATTRIBUTES attributes = Make3DAttributes(
        emitter.PositionX,
        emitter.PositionY,
        emitter.PositionZ,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f);
    instance->set3DAttributes(&attributes);

    instance->setVolume(m_soundVolume);
    const FMOD_RESULT result = instance->start();
    emitter.Instance = instance;
    emitter.Playing = (result == FMOD_OK);
    if (result != FMOD_OK)
    {
        instance->release();
        emitter.Instance = nullptr;
        return false;
    }

    return true;
}

bool AudioManager::StopEmitter(EmitterHandle handle)
{
    if (!handle.IsValid() || handle.Value >= static_cast<int>(m_emitters.size()))
        return false;

    EmitterState& emitter = m_emitters[static_cast<size_t>(handle.Value)];
    FMOD::Studio::EventInstance* instance = static_cast<FMOD::Studio::EventInstance*>(emitter.Instance);
    if (instance)
    {
        instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
        instance->release();
        emitter.Instance = nullptr;
    }
    emitter.Playing = false;
    return true;
}

void AudioManager::SetResonanceAudioEnabled(bool enabled)
{
    // Not a switch any more. Every event in the banks is routed through a Resonance
    // Source, so bypassing the one listener that decodes them would not turn
    // spatialisation off, it would turn the sound off. The flag is kept only so the
    // editor panel has something to report.
    m_resonanceAudioEnabled = enabled;

    m_resonanceAudioStatus = m_resonanceAudioAvailable
        ? "Resonance Audio is configured per event in the FMOD project: the spatialised "
          "events carry a matched Source and Listener, music and UI carry neither. The "
          "engine installs no Resonance DSPs, so there is nothing to switch here."
        : "Resonance Audio is unavailable.";
}
