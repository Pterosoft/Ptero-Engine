#include "AudioManager.h"

#include <windows.h>
#include "fmod_studio.hpp"
#include "fmod.hpp"
#include "fmod_errors.h"

#include <algorithm>
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

    FMOD::DSP* listenerDsp = nullptr;
    result = coreSystem->createDSPByPlugin(listenerPluginHandle, &listenerDsp);
    if (!FmodOk(result, outError, "FMOD::System::createDSPByPlugin(Resonance Audio Listener)"))
        return false;

    FMOD::ChannelGroup* masterChannelGroup = nullptr;
    result = coreSystem->getMasterChannelGroup(&masterChannelGroup);
    if (result != FMOD_OK || masterChannelGroup == nullptr)
    {
        listenerDsp->release();
        outError = result == FMOD_OK
            ? "FMOD::System::getMasterChannelGroup returned a null channel group."
            : std::string("FMOD::System::getMasterChannelGroup: ") + FMOD_ErrorString(result);
        return false;
    }

    result = masterChannelGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, listenerDsp);
    if (result != FMOD_OK)
    {
        listenerDsp->release();
        return FmodOk(result, outError, "FMOD::ChannelGroup::addDSP(Resonance Audio Listener)");
    }

    listenerDsp->setBypass(!m_resonanceAudioEnabled);
    m_resonanceListenerDsp = listenerDsp;
    m_resonanceAudioAvailable = true;
    m_resonanceAudioStatus = "Resonance Audio Listener and Source DSPs loaded from " + pluginPath.string();
    return true;
}

FMOD::DSP* AudioManager::AttachResonanceSource(FMOD::Studio::EventInstance* instance)
{
    if (!m_resonanceAudioAvailable || m_coreSystem == nullptr || instance == nullptr)
        return nullptr;

    // Studio creates the event channel group asynchronously. Flush once after
    // start so the native Resonance source can be inserted before its panner.
    m_studioSystem->flushCommands();

    FMOD::ChannelGroup* eventChannelGroup = nullptr;
    if (instance->getChannelGroup(&eventChannelGroup) != FMOD_OK || eventChannelGroup == nullptr)
        return nullptr;

    FMOD::DSP* sourceDsp = nullptr;
    if (m_coreSystem->createDSPByPlugin(m_resonanceSourcePluginHandle, &sourceDsp) != FMOD_OK || sourceDsp == nullptr)
        return nullptr;

    if (eventChannelGroup->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, sourceDsp) != FMOD_OK)
    {
        sourceDsp->release();
        return nullptr;
    }

    sourceDsp->setBypass(!m_resonanceAudioEnabled);
    return sourceDsp;
}

void AudioManager::ReleaseResonanceDsp(void*& dspHandle)
{
    FMOD::DSP* dsp = static_cast<FMOD::DSP*>(dspHandle);
    if (dsp != nullptr)
        dsp->release();
    dspHandle = nullptr;
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

    result = inst->start();
    if (result == FMOD_OK)
    {
        if (is3D)
        {
            if (FMOD::DSP* resonanceDsp = AttachResonanceSource(inst))
                m_oneShotResonanceDsps.push_back(resonanceDsp);
        }
    }
    inst->release();
    return result == FMOD_OK;
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
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
        return {};

    std::filesystem::path current = std::filesystem::path(exePath).parent_path();
    while (!current.empty())
    {
        const std::filesystem::path candidate = current / L"Data" / L"Audio";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec))
            return std::filesystem::weakly_canonical(candidate);

        const std::filesystem::path parent = current.parent_path();
        if (parent == current)
            break;
        current = parent;
    }
    return {};
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
        masterBus->setVolume(1.0f);
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
        std::error_code ec;
        std::vector<std::filesystem::path> bankPaths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_audioDataPath, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
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
            std::error_code relativeError;
            const std::filesystem::path relativePath = std::filesystem::relative(p, m_audioDataPath, relativeError);
            const std::string displayPath = relativeError ? p.string() : relativePath.string();
            const std::wstring fileName = p.filename().wstring();
            const bool isStringsBank = fileName.find(L".strings.bank") != std::wstring::npos;

            FMOD::Studio::Bank* bank = nullptr;
            const std::string pathUtf8 = p.string();
            FMOD_RESULT loadResult = m_studioSystem->loadBankFile(
                pathUtf8.c_str(), FMOD_STUDIO_LOAD_BANK_NORMAL, &bank);
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

    for (void*& dsp : m_oneShotResonanceDsps)
        ReleaseResonanceDsp(dsp);
    m_oneShotResonanceDsps.clear();

    for (std::size_t instanceIndex = 0; instanceIndex < m_instances.size(); ++instanceIndex)
    {
        FMOD::Studio::EventInstance* inst = m_instances[instanceIndex];
        if (inst)
            inst->release();
        if (instanceIndex < m_instanceResonanceDsps.size())
            ReleaseResonanceDsp(m_instanceResonanceDsps[instanceIndex]);
    }
    m_instances.clear();
    m_instanceResonanceDsps.clear();

    for (FMOD::Studio::Bank* bank : m_banks)
        bank->unload();
    m_banks.clear();

    if (m_studioSystem)
    {
        ReleaseResonanceDsp(m_resonanceListenerDsp);
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
    m_instanceResonanceDsps.clear();

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
    m_instanceResonanceDsps.resize(m_events.size(), nullptr);
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

    result = inst->start();
    if (result != FMOD_OK)
    {
        inst->release();
        return false;
    }

    if (is3D)
        m_instanceResonanceDsps[static_cast<size_t>(index)] = AttachResonanceSource(inst);
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
        if (static_cast<size_t>(index) < m_instanceResonanceDsps.size())
            ReleaseResonanceDsp(m_instanceResonanceDsps[static_cast<size_t>(index)]);
        inst->release();
        m_instances[static_cast<size_t>(index)] = nullptr;
    }
    m_events[static_cast<size_t>(index)].IsPlaying = false;
    return true;
}

// ---------------------------------------------------------------------------
void AudioManager::StopAll()
{
    for (int i = 0; i < static_cast<int>(m_events.size()); ++i)
        StopEvent(i);

    for (EmitterState& emitter : m_emitters)
    {
        FMOD::Studio::EventInstance* instance = static_cast<FMOD::Studio::EventInstance*>(emitter.Instance);
        if (instance)
        {
            instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
            ReleaseResonanceDsp(emitter.ResonanceDsp);
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

    const FMOD_RESULT result = instance->start();
    emitter.Instance = instance;
    emitter.Playing = (result == FMOD_OK);
    if (result != FMOD_OK)
    {
        instance->release();
        emitter.Instance = nullptr;
        return false;
    }

    bool is3D = false;
    if (desc->is3D(&is3D) == FMOD_OK && is3D)
        emitter.ResonanceDsp = AttachResonanceSource(instance);

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
        ReleaseResonanceDsp(emitter.ResonanceDsp);
        instance->release();
        emitter.Instance = nullptr;
    }
    emitter.Playing = false;
    return true;
}

void AudioManager::SetResonanceAudioEnabled(bool enabled)
{
    m_resonanceAudioEnabled = enabled;

    if (FMOD::DSP* listenerDsp = static_cast<FMOD::DSP*>(m_resonanceListenerDsp))
        listenerDsp->setBypass(!enabled);

    for (void* dspHandle : m_instanceResonanceDsps)
    {
        if (FMOD::DSP* dsp = static_cast<FMOD::DSP*>(dspHandle))
            dsp->setBypass(!enabled);
    }

    for (void* dspHandle : m_oneShotResonanceDsps)
    {
        if (FMOD::DSP* dsp = static_cast<FMOD::DSP*>(dspHandle))
            dsp->setBypass(!enabled);
    }

    for (EmitterState& emitter : m_emitters)
    {
        if (FMOD::DSP* dsp = static_cast<FMOD::DSP*>(emitter.ResonanceDsp))
            dsp->setBypass(!enabled);
    }

    m_resonanceAudioStatus = m_resonanceAudioAvailable
        ? (enabled ? "Resonance Audio is active for FMOD 3D events."
                   : "Resonance Audio is loaded but bypassed.")
        : "Resonance Audio is unavailable.";
}
