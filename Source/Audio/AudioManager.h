#pragma once

#include <string>
#include <vector>
#include <filesystem>

// Forward-declare FMOD types so callers do not need to include fmod headers.
namespace FMOD { class DSP; class System; namespace Studio { class System; class EventInstance; class Bank; } }

#ifdef AUDIO_EXPORTS
#define AUDIO_API __declspec(dllexport)
#else
#define AUDIO_API __declspec(dllimport)
#endif

// ----------------------------------------------------------------------------
// AudioEvent
//   Represents a single FMOD Studio event path and its active instance.
// ----------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable: 4251)
struct AUDIO_API AudioEvent
{
    std::string Path;         // e.g. "event:/Music/MainTheme"
    bool        IsPlaying = false;
};
#pragma warning(pop)

// ----------------------------------------------------------------------------
// AudioManager
//   Wraps the FMOD Studio System.  Call Initialize() once at startup,
//   Shutdown() at exit, and Update() every frame.
//
//   Bank files are loaded from the directory returned by GetAudioDataPath().
//   The engine looks for *.bank files inside <exe-dir>\Data\Audio (or up the
//   directory tree until found).
// ----------------------------------------------------------------------------
class AUDIO_API AudioManager
{
public:
    struct EmitterHandle
    {
        int Value = -1;

        bool IsValid() const { return Value >= 0; }
    };

    AudioManager();
    ~AudioManager();

    // Initialise the FMOD Studio system and load all banks found in Data/Audio.
    // Returns false and fills outError on failure.
    bool Initialize(std::string& outError);

    // Must be called once per frame to pump FMOD Studio.
    void Update();

    // Release all FMOD resources.
    void Shutdown();

    // Enumerate all events discovered in the loaded banks.
    // The list is populated after Initialize().
    const std::vector<AudioEvent>& GetEvents() const { return m_events; }
    const std::filesystem::path& GetAudioDataPath() const { return m_audioDataPath; }
    const std::vector<std::string>& GetLoadedBanks() const { return m_loadedBanks; }
    const std::vector<std::string>& GetBankLoadFailures() const { return m_bankLoadFailures; }
    const std::string& GetLastStatus() const { return m_lastStatus; }

    // Play the event at the given index.  Starts a new one-shot instance.
    // Returns false if the index is out of range or FMOD fails.
    bool PlayEvent(int index);
    bool PlayEventByPath(const std::string& eventPath);

    // Resolve a bare event name ("DiceRoll") against the loaded banks and return its
    // full path ("event:/Ambient/DiceRoll"), so callers do not have to know which
    // folder the event was authored in.  A full path is returned unchanged when it
    // exists.  Returns an empty string when nothing matches.
    std::string ResolveEventPath(const std::string& nameOrPath) const;

    // Fire-and-forget one-shot, looked up with ResolveEventPath().
    bool PlayOneShotByName(const std::string& nameOrPath);

    // Music channel.  Unlike the one-shots above it keeps its single instance alive,
    // which is what lets a caller wait for the current track to finish before
    // choosing the next one.  PlayMusicByName replaces whatever is already playing.
    bool PlayMusicByName(const std::string& nameOrPath);
    bool IsMusicPlaying();
    void StopMusic();

    // Stop the currently playing instance for the event at the given index.
    bool StopEvent(int index);

    // Stop every active event instance.
    void StopAll();

    void SetListenerTransform(
        float positionX,
        float positionY,
        float positionZ,
        float forwardX,
        float forwardY,
        float forwardZ,
        float upX,
        float upY,
        float upZ);

    EmitterHandle RegisterEmitter();
    void UnregisterEmitter(EmitterHandle handle);
    bool SetEmitterEventPath(EmitterHandle handle, const std::string& eventPath);
    bool SetEmitterTransform(EmitterHandle handle, float positionX, float positionY, float positionZ);
    bool PlayEmitter(EmitterHandle handle);
    bool StopEmitter(EmitterHandle handle);

    // Player volume settings, each linear gain in [0, 1]. Overall scales the master bus;
    // music scales the music channel; sound scales every other instance, live and
    // future. Sound has to be per instance rather than a bus: the project routes every
    // event straight to the master bus, and the spatialised events' Resonance Listeners
    // each output the whole shared soundfield, so scaling all of them alike is what
    // scales the spatialised mix (see the FMOD project notes).
    void SetVolumes(float overall, float sound, float music);

    // Returns true when the system was successfully initialised.
    bool IsInitialized() const { return m_initialized; }

    // Resonance is configured entirely in the FMOD project: a spatialised event carries
    // a matched Resonance Source and Listener, and everything else carries neither. The
    // engine only loads the plugin, so this setter has nothing to switch - it records
    // intent, and GetResonanceAudioStatus() reports the arrangement.
    void SetResonanceAudioEnabled(bool enabled);
    bool IsResonanceAudioEnabled() const { return m_resonanceAudioEnabled; }
    bool IsResonanceAudioAvailable() const { return m_resonanceAudioAvailable; }
    const std::string& GetResonanceAudioStatus() const { return m_resonanceAudioStatus; }

private:
    struct EmitterState
    {
        std::string Path;
        void* Instance = nullptr;
        float PositionX = 0.0f;
        float PositionY = 0.0f;
        float PositionZ = 0.0f;
        bool Allocated = false;
        bool Playing = false;
    };

    // Resolve the Data/Audio directory relative to the running executable.
    static std::filesystem::path ResolveAudioDataPath();

    // Refresh m_events from all loaded banks.
    void RefreshEventList();
    // Loads resonanceaudio.dll. That is the whole job: without it the banks fail to
    // load with FMOD_ERR_PLUGIN_MISSING, and with it they bring their own DSP chain.
    bool InitializeResonanceAudio(FMOD::System* coreSystem, std::string& outError);

    // Sits the instance on the listener, so a 3D event plays at the level its mix says
    // and stays there. Music and UI have no position in the world; the alternative is
    // the default (0,0,0), which drifts in and out as the camera moves.
    void PlaceAtListener(FMOD::Studio::EventInstance* instance) const;

    #pragma warning(push)
    #pragma warning(disable: 4251)
        FMOD::Studio::System*  m_studioSystem = nullptr;
        FMOD::System*          m_coreSystem = nullptr;
        std::vector<FMOD::Studio::Bank*>          m_banks;
        std::vector<FMOD::Studio::EventInstance*> m_instances;
        std::vector<AudioEvent>                   m_events;
        std::filesystem::path                     m_audioDataPath;
        std::vector<std::string>                  m_loadedBanks;
        std::vector<std::string>                  m_bankLoadFailures;
        std::string                               m_lastStatus;
        std::string                               m_resonanceAudioStatus;
        std::vector<EmitterState>                 m_emitters;
        FMOD::Studio::EventInstance*              m_musicInstance = nullptr;
        bool                                      m_initialized = false;
        float m_listenerPosX = 0.0f, m_listenerPosY = 0.0f, m_listenerPosZ = 0.0f;
        float m_listenerFwdX = 0.0f, m_listenerFwdY = 0.0f, m_listenerFwdZ = -1.0f;
        float m_listenerUpX  = 0.0f, m_listenerUpY  = 1.0f, m_listenerUpZ  = 0.0f;
    #pragma warning(pop)

    unsigned int m_resonancePluginHandle = 0;
    unsigned int m_resonanceSourcePluginHandle = 0;
    bool          m_resonanceAudioEnabled = true;
    bool          m_resonanceAudioAvailable = false;
    float         m_overallVolume = 1.0f;
    float         m_soundVolume = 1.0f;
    float         m_musicVolume = 1.0f;
};
