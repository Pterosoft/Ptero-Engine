#pragma once

#include "System/Cinematics/CinematicSequence.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class CinematicSystem;

// Everything a playing sequence does to the world goes through this interface.
//
// The runtime never includes an engine type: the host (DX12SceneRenderer in a game, the
// editor's preview while authoring, a fake in the tests) turns a binding into an object
// pointer of the binding's PropertyRegistry domain, and decides what "the cut camera" means.
class CinematicHost
{
public:
    virtual ~CinematicHost() = default;

    // The object `binding` animates, as the owner its domain's property accessors expect
    // (Entity* for "Entity"). Null when the world does not contain it, in which case the
    // binding's tracks are skipped. Resolved on every evaluation and never stored, so it
    // only has to stay valid until this evaluation returns.
    virtual void* ResolveBinding(const SequenceBinding& binding) = 0;

    // The camera the sequence currently looks through. `camera` and `owner` are null when
    // no cut covers the current time, or when the player releases the camera (stop,
    // finish). `sequenceTime` lets the host drive time-based effects such as shake
    // deterministically, so a render of frame 120 always shakes the same way.
    virtual void SetCutCamera(const SequenceBinding* camera, void* owner, double sequenceTime) = 0;
};

enum class SequencePlaybackState
{
    Stopped,
    Playing,
    Paused
};

// Plays one sequence: the clock, looping, event firing and evaluation.
//
// Independent of any editor. The Sequencer's preview drives one exactly as a game does,
// through Play/Pause/SetTime, which is what makes "it plays the same in the game" true by
// construction rather than by testing.
class SequencePlayer
{
public:
    SequencePlayer(std::shared_ptr<const CinematicSequence> sequence, CinematicHost* host);
    ~SequencePlayer();

    SequencePlayer(const SequencePlayer&) = delete;
    SequencePlayer& operator=(const SequencePlayer&) = delete;

    const CinematicSequence& Sequence() const { return *mSequence; }
    std::shared_ptr<const CinematicSequence> SharedSequence() const { return mSequence; }
    // Swaps in an edited copy of the sequence without disturbing the clock or the
    // recorded pre-animation state. The editor calls this after every edit.
    void SetSequence(std::shared_ptr<const CinematicSequence> sequence);

    // Unique within the process, for scripts that refer to a player by number.
    std::uint32_t Handle() const { return mHandle; }

    // Starts from the current time. A non-looping player sitting at the end starts over
    // from the beginning, which is what pressing Play on a finished cinematic means.
    void Play();
    void PlayFromStart();
    void Pause();
    void Resume();
    // Stops, returns to the start and releases the camera. Restores the world when
    // RestoreStateOnStop is set.
    void Stop();

    // Jumps to `seconds` (clamped to the playback range) and evaluates. Events between the
    // old and new time fire only with `fireEvents` - scrubbing should not detonate every
    // explosion it passes over.
    void SetTime(double seconds, bool fireEvents = false);
    double GetTime() const { return mTime; }

    SequencePlaybackState GetState() const { return mState; }
    bool IsPlaying() const { return mState == SequencePlaybackState::Playing; }

    void SetLooping(bool looping) { mLooping = looping; }
    bool IsLooping() const { return mLooping; }
    // 1 is real time; negative plays backwards. Zero is refused (use Pause).
    void SetPlayRate(double rate);
    double GetPlayRate() const { return mPlayRate; }

    // Advances the clock while playing, fires the events it crossed, then evaluates.
    void Tick(double deltaSeconds);

    // Applies the current time again - after an edit, or after something else moved the
    // animated objects.
    void Evaluate();

    // Puts back every property the sequence has changed, as it was before the first change,
    // and forgets the record. Also releases the camera.
    void RestoreState();
    // Drops the record without restoring, making the animated state the new baseline.
    void ForgetRestoreState() { mRestoreState.clear(); }
    // Records `value` as what `propertyPath` should be restored to, unless the player
    // already holds a value for it. For an editor that is about to start animating a
    // property it has just watched the user change: the authored value is the one from
    // before that change, not the one the player would read now.
    void SeedRestoreState(const SequenceBinding& binding, const std::string& propertyPath, const PropertyValue& value);

    // What Stop does about the world. Off by default: a cutscene that ends leaves the door
    // it opened open. The editor's preview turns it on so closing the Sequencer returns the
    // level to how it was authored.
    bool RestoreStateOnStop = false;

    // Callbacks. Called from inside Tick/Play/Stop; a callback may call back into the
    // player (stop it, seek it) - that is checked for and handled.
    std::function<void(SequencePlayer&)> OnSequenceStarted;
    std::function<void(SequencePlayer&)> OnSequenceFinished;
    std::function<void(SequencePlayer&)> OnSequenceStopped;
    std::function<void(SequencePlayer&, const SequenceEventKey&)> OnSequenceEvent;

private:
    class Sink;
    friend class Sink;
    friend class CinematicSystem;

    void EvaluateAt(double time);
    void NotifyStarted();
    void NotifyFinished();
    void NotifyStopped();
    void NotifyEvent(const SequenceEventKey& key);
    void FireEvents(double from, double to, bool includeFrom);
    void ReleaseCamera();

    std::shared_ptr<const CinematicSequence> mSequence;
    CinematicHost* mHost = nullptr;
    // Set when a CinematicSystem owns this player; it hears every callback after the
    // player's own, so a script's handler on one player never displaces the system's.
    CinematicSystem* mSystem = nullptr;
    std::unique_ptr<Sink> mSink;
    std::uint32_t mHandle = 0;

    SequencePlaybackState mState = SequencePlaybackState::Stopped;
    double mTime = 0.0;
    double mPlayRate = 1.0;
    bool mLooping = false;
    // The first tick after Play also fires events sitting exactly on the start time.
    bool mFireStartEvents = false;
    // Bumped whenever a callback changes the clock, so a Tick that called out can tell
    // the time it was working towards is stale.
    std::uint64_t mTimeGeneration = 0;

    // Pre-animation values, keyed by binding and property path - never by pointer, since
    // the objects may move in memory between frames.
    struct RestoreEntry
    {
        // A copy of the binding without its tracks: enough for the host to find the object
        // again even if the binding has since been deleted from the sequence.
        SequenceBinding Binding;
        std::string PropertyPath;
        PropertyValue Value;
    };
    std::map<std::pair<Guid, std::string>, RestoreEntry> mRestoreState;

    bool mCameraApplied = false;
    // Non-zero while a callback is running, so the system never destroys a player from
    // inside its own call stack.
    int mCallbackDepth = 0;
};

// The runtime front door: loads sequence assets, owns the players, ticks them.
//
//   SequencePlayer* player = cinematics.Play("Cinematics/Intro.sequence");
//   player->Pause(); player->Resume(); player->SetTime(2.0); player->SetLooping(true);
//
// A player stays alive - and its pointer valid - until Release or Clear, even after it
// finishes, so a script holding it can still ask whether it is playing. Listeners added
// with AddEventListener hear every player's events, which is how the graph's On Sequence
// Event node receives them without binding to each player.
class CinematicSystem
{
public:
    void SetHost(CinematicHost* host) { mHost = host; }
    CinematicHost* GetHost() const { return mHost; }

    // Loads a Data-relative asset path, cached by path. Null, with the reason logged under
    // "Cinematics", when it cannot be loaded.
    std::shared_ptr<const CinematicSequence> Load(const std::string& assetPath);
    void ClearCache() { mCache.clear(); }

    // Creates a player and starts it. Null when the asset does not load.
    SequencePlayer* Play(const std::string& assetPath);
    SequencePlayer* Play(std::shared_ptr<const CinematicSequence> sequence);
    // Creates a stopped player, for a caller that wants to configure it before it starts.
    SequencePlayer* CreatePlayer(std::shared_ptr<const CinematicSequence> sequence);

    SequencePlayer* FindPlayer(std::uint32_t handle) const;
    const std::vector<std::unique_ptr<SequencePlayer>>& Players() const { return mPlayers; }

    // Destroys a player at the end of the current Tick (immediately when not ticking).
    void Release(SequencePlayer* player);
    void StopAll();
    void Clear();

    void Tick(double deltaSeconds);

    // Event fan-out; see the class comment.
    using StartedListener = std::function<void(SequencePlayer&)>;
    using EventListener = std::function<void(SequencePlayer&, const SequenceEventKey&)>;
    void AddEventListener(EventListener listener) { mEventListeners.push_back(std::move(listener)); }
    void AddStartedListener(StartedListener listener) { mStartedListeners.push_back(std::move(listener)); }
    void AddFinishedListener(StartedListener listener) { mFinishedListeners.push_back(std::move(listener)); }
    void ClearListeners();

private:
    friend class SequencePlayer;
    void CollectReleased();

    CinematicHost* mHost = nullptr;
    std::unordered_map<std::string, std::shared_ptr<const CinematicSequence>> mCache;
    std::vector<std::unique_ptr<SequencePlayer>> mPlayers;
    std::vector<SequencePlayer*> mPendingRelease;
    bool mTicking = false;

    std::vector<EventListener> mEventListeners;
    std::vector<StartedListener> mStartedListeners;
    std::vector<StartedListener> mFinishedListeners;
};
