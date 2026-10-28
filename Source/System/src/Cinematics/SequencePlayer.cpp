#include "System/Cinematics/SequencePlayer.h"

#include "System/DataFiles.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>

namespace
{
    constexpr const char* kCategory = "Cinematics";

    std::uint32_t NextPlayerHandle()
    {
        static std::atomic<std::uint32_t> next{ 1 };
        return next.fetch_add(1);
    }

    // Bumps a counter for the lifetime of a callback.
    struct CallbackScope
    {
        explicit CallbackScope(int& depth) : Depth(depth) { ++Depth; }
        ~CallbackScope() { --Depth; }
        int& Depth;
    };
}

// The player's view of a track's output: records the pre-animation value the first time
// each property is written, then writes it.
class SequencePlayer::Sink final : public SequenceEvaluationSink
{
public:
    explicit Sink(SequencePlayer& player) : mPlayer(player) {}

    void ApplyProperty(const SequenceBinding& binding, void* owner, const PropertyDescriptor& property,
                       const PropertyValue& value) override
    {
        auto key = std::make_pair(binding.Id, property.Path);
        if (mPlayer.mRestoreState.find(key) == mPlayer.mRestoreState.end())
        {
            RestoreEntry entry;
            entry.Binding.Id = binding.Id;
            entry.Binding.Name = binding.Name;
            entry.Binding.Domain = binding.Domain;
            entry.Binding.EntityId = binding.EntityId;
            entry.Binding.EntityName = binding.EntityName;
            entry.PropertyPath = property.Path;
            if (property.Get && property.Get(owner, entry.Value))
                mPlayer.mRestoreState.emplace(std::move(key), std::move(entry));
        }

        property.Set(owner, value);
    }

    void ApplyCameraCut(const Guid& cameraBinding) override
    {
        CinematicHost* host = mPlayer.mHost;
        if (host == nullptr)
            return;

        const SequenceBinding* binding = cameraBinding.IsValid() ? mPlayer.mSequence->FindBinding(cameraBinding) : nullptr;
        void* owner = binding != nullptr ? host->ResolveBinding(*binding) : nullptr;
        host->SetCutCamera(owner != nullptr ? binding : nullptr, owner, mTime);
        mPlayer.mCameraApplied = true;
    }

    double mTime = 0.0;

private:
    SequencePlayer& mPlayer;
};

SequencePlayer::SequencePlayer(std::shared_ptr<const CinematicSequence> sequence, CinematicHost* host)
    : mSequence(sequence ? std::move(sequence) : std::make_shared<const CinematicSequence>()),
      mHost(host),
      mSink(std::make_unique<Sink>(*this)),
      mHandle(NextPlayerHandle())
{
    mTime = mSequence->PlaybackStart;
}

SequencePlayer::~SequencePlayer() = default;

void SequencePlayer::SetSequence(std::shared_ptr<const CinematicSequence> sequence)
{
    if (!sequence)
        return;
    mSequence = std::move(sequence);
    mTime = std::clamp(mTime, mSequence->PlaybackStart, mSequence->PlaybackEnd);
}

void SequencePlayer::SetPlayRate(double rate)
{
    if (rate != 0.0 && std::isfinite(rate))
        mPlayRate = rate;
}

void SequencePlayer::Play()
{
    if (mState == SequencePlaybackState::Playing)
        return;
    if (mState == SequencePlaybackState::Paused)
    {
        Resume();
        return;
    }

    const CinematicSequence& sequence = *mSequence;
    // A finished player starts over; one stopped part-way (by SetTime while stopped, as
    // the editor does) starts from where its playhead is.
    if (mPlayRate > 0.0 && mTime >= sequence.PlaybackEnd)
        mTime = sequence.PlaybackStart;
    else if (mPlayRate < 0.0 && mTime <= sequence.PlaybackStart)
        mTime = sequence.PlaybackEnd;

    mState = SequencePlaybackState::Playing;
    mFireStartEvents = true;
    ++mTimeGeneration;
    EvaluateAt(mTime);
    NotifyStarted();
}

void SequencePlayer::PlayFromStart()
{
    mState = SequencePlaybackState::Stopped;
    mTime = mPlayRate >= 0.0 ? mSequence->PlaybackStart : mSequence->PlaybackEnd;
    Play();
}

void SequencePlayer::Pause()
{
    if (mState == SequencePlaybackState::Playing)
        mState = SequencePlaybackState::Paused;
}

void SequencePlayer::Resume()
{
    if (mState == SequencePlaybackState::Paused)
        mState = SequencePlaybackState::Playing;
}

void SequencePlayer::Stop()
{
    const bool wasActive = mState != SequencePlaybackState::Stopped;
    mState = SequencePlaybackState::Stopped;
    mTime = mSequence->PlaybackStart;
    mFireStartEvents = false;
    ++mTimeGeneration;

    if (RestoreStateOnStop)
        RestoreState();
    else
        ReleaseCamera();

    if (wasActive)
        NotifyStopped();
}

void SequencePlayer::SetTime(double seconds, bool fireEvents)
{
    const CinematicSequence& sequence = *mSequence;
    const double target = std::clamp(std::isfinite(seconds) ? seconds : sequence.PlaybackStart,
                                     sequence.PlaybackStart, sequence.PlaybackEnd);
    const double previous = mTime;
    mTime = target;
    mFireStartEvents = false;
    const std::uint64_t generation = ++mTimeGeneration;

    if (fireEvents && target != previous)
    {
        FireEvents(previous, target, false);
        if (generation != mTimeGeneration)
            return;
        mTime = target;
    }

    EvaluateAt(mTime);
}

void SequencePlayer::Tick(double deltaSeconds)
{
    if (mState != SequencePlaybackState::Playing || !(deltaSeconds > 0.0))
        return;

    const CinematicSequence& sequence = *mSequence;
    const double start = sequence.PlaybackStart;
    const double end = sequence.PlaybackEnd;
    const double span = end - start;
    const double from = mTime;
    const double to = from + deltaSeconds * mPlayRate;
    const bool includeFrom = mFireStartEvents;
    mFireStartEvents = false;
    const std::uint64_t generation = mTimeGeneration;

    const bool forward = mPlayRate > 0.0;
    const bool pastEnd = forward ? to >= end : to <= start;

    if (!pastEnd)
    {
        FireEvents(from, to, includeFrom);
        if (generation != mTimeGeneration)
            return;
        mTime = to;
        EvaluateAt(mTime);
        return;
    }

    const double boundary = forward ? end : start;
    FireEvents(from, boundary, includeFrom);
    if (generation != mTimeGeneration)
        return;

    if (mLooping && span > 1e-9)
    {
        // A long hitch can cover several loops; only the remainder matters, and events of
        // the skipped whole loops are not replayed - a frame that late has bigger problems
        // than a missed footstep.
        const double overshoot = std::fmod(std::fabs(to - boundary), span);
        const double wrapped = forward ? start + overshoot : end - overshoot;
        FireEvents(forward ? start : end, wrapped, true);
        if (generation != mTimeGeneration)
            return;
        mTime = wrapped;
        EvaluateAt(mTime);
        return;
    }

    // Finished. The world is left in the final pose and the camera handed back; Stop is
    // what restores, and only when asked to.
    mTime = boundary;
    EvaluateAt(mTime);
    mState = SequencePlaybackState::Stopped;
    ++mTimeGeneration;
    ReleaseCamera();
    NotifyFinished();
}

void SequencePlayer::Evaluate()
{
    EvaluateAt(mTime);
}

void SequencePlayer::EvaluateAt(double time)
{
    if (mHost == nullptr)
        return;

    const CinematicSequence& sequence = *mSequence;
    mSink->mTime = time;

    TrackEvaluationContext context;
    context.Time = time;
    context.Sequence = &sequence;
    context.Sink = mSink.get();

    for (const SequenceBinding& binding : sequence.Bindings)
    {
        if (binding.Tracks.empty())
            continue;
        void* owner = mHost->ResolveBinding(binding);
        if (owner == nullptr)
            continue;

        context.Binding = &binding;
        context.Owner = owner;
        for (const std::unique_ptr<SequenceTrack>& track : binding.Tracks)
        {
            if (!track->Muted)
                track->Evaluate(context);
        }
    }

    context.Binding = nullptr;
    context.Owner = nullptr;
    for (const std::unique_ptr<SequenceTrack>& track : sequence.MasterTracks)
    {
        if (!track->Muted)
            track->Evaluate(context);
    }
}

void SequencePlayer::FireEvents(double from, double to, bool includeFrom)
{
    std::vector<const SequenceEventKey*> keys;
    const bool forward = to >= from;
    for (const std::unique_ptr<SequenceTrack>& track : mSequence->MasterTracks)
    {
        const auto* events = dynamic_cast<const EventTrack*>(track.get());
        if (events == nullptr || events->Muted)
            continue;

        if (forward)
        {
            events->CollectEvents(from, to, includeFrom, keys);
        }
        else
        {
            // Backwards over [to, from): the forward half-open rule mirrored, so a marker
            // is crossed exactly once whichever way the playhead goes.
            std::vector<const SequenceEventKey*> range;
            events->CollectEvents(to, from, true, range);
            for (const SequenceEventKey* key : range)
            {
                if (key->Time < from || includeFrom)
                    keys.push_back(key);
            }
        }
    }

    if (keys.empty())
        return;

    // Several event tracks interleave by time; backwards playback fires latest first.
    std::stable_sort(keys.begin(), keys.end(), [forward](const SequenceEventKey* a, const SequenceEventKey* b) {
        return forward ? a->Time < b->Time : a->Time > b->Time;
    });

    // The sequence may be swapped by a callback (the editor re-snapshots on edits), so the
    // keys are copied out before any listener runs.
    std::vector<SequenceEventKey> fired;
    fired.reserve(keys.size());
    for (const SequenceEventKey* key : keys)
        fired.push_back(*key);

    const std::uint64_t generation = mTimeGeneration;
    for (const SequenceEventKey& key : fired)
    {
        // A listener asking the player for the time hears the marker's, not the start of
        // the frame that happened to cross it. The caller sets the frame's end time after.
        mTime = key.Time;
        NotifyEvent(key);
        // A listener that stopped or seeked the player ends this batch.
        if (generation != mTimeGeneration)
            return;
    }
}

void SequencePlayer::ReleaseCamera()
{
    if (!mCameraApplied || mHost == nullptr)
        return;
    mCameraApplied = false;
    mHost->SetCutCamera(nullptr, nullptr, mTime);
}

void SequencePlayer::SeedRestoreState(const SequenceBinding& binding, const std::string& propertyPath,
                                      const PropertyValue& value)
{
    auto key = std::make_pair(binding.Id, propertyPath);
    if (mRestoreState.find(key) != mRestoreState.end())
        return;

    RestoreEntry entry;
    entry.Binding.Id = binding.Id;
    entry.Binding.Name = binding.Name;
    entry.Binding.Domain = binding.Domain;
    entry.Binding.EntityId = binding.EntityId;
    entry.Binding.EntityName = binding.EntityName;
    entry.PropertyPath = propertyPath;
    entry.Value = value;
    mRestoreState.emplace(std::move(key), std::move(entry));
}

void SequencePlayer::RestoreState()
{
    if (mHost != nullptr)
    {
        for (const auto& [key, entry] : mRestoreState)
        {
            void* owner = mHost->ResolveBinding(entry.Binding);
            if (owner == nullptr)
                continue;
            const PropertyDescriptor* property = PropertyRegistry::Find(entry.Binding.Domain, entry.PropertyPath);
            if (property != nullptr && property->Set && property->IsAvailableOn(owner))
                property->Set(owner, entry.Value);
        }
    }

    mRestoreState.clear();
    ReleaseCamera();
}

void SequencePlayer::NotifyStarted()
{
    const CallbackScope scope(mCallbackDepth);
    if (OnSequenceStarted)
        OnSequenceStarted(*this);
    if (mSystem != nullptr)
    {
        for (const auto& listener : mSystem->mStartedListeners)
            listener(*this);
    }
}

void SequencePlayer::NotifyFinished()
{
    const CallbackScope scope(mCallbackDepth);
    if (OnSequenceFinished)
        OnSequenceFinished(*this);
    if (mSystem != nullptr)
    {
        for (const auto& listener : mSystem->mFinishedListeners)
            listener(*this);
    }
}

void SequencePlayer::NotifyStopped()
{
    const CallbackScope scope(mCallbackDepth);
    if (OnSequenceStopped)
        OnSequenceStopped(*this);
}

void SequencePlayer::NotifyEvent(const SequenceEventKey& key)
{
    const CallbackScope scope(mCallbackDepth);
    if (OnSequenceEvent)
        OnSequenceEvent(*this, key);
    if (mSystem != nullptr)
    {
        for (const auto& listener : mSystem->mEventListeners)
            listener(*this, key);
    }
}

// ---------------------------------------------------------------------------------------
// CinematicSystem
// ---------------------------------------------------------------------------------------

std::shared_ptr<const CinematicSequence> CinematicSystem::Load(const std::string& assetPath)
{
    // Case and slash direction do not make two different assets on Windows, and the
    // packaged archives look paths up case-insensitively too.
    std::string key = assetPath;
    std::replace(key.begin(), key.end(), '\\', '/');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    if (const auto it = mCache.find(key); it != mCache.end())
        return it->second;

    const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
    const std::filesystem::path fullPath = std::filesystem::path(assetPath).is_absolute()
        ? std::filesystem::path(assetPath)
        : dataDirectory / assetPath;

    auto sequence = std::make_shared<CinematicSequence>();
    std::string error;
    if (!sequence->LoadFromFile(fullPath.string(), error))
    {
        PTERO_LOG_ERROR(kCategory, "Cannot load sequence '%s': %s", assetPath.c_str(), error.c_str());
        return nullptr;
    }
    if (!error.empty())
        PTERO_LOG_WARNING(kCategory, "Sequence '%s' loaded with problems: %s", assetPath.c_str(), error.c_str());

    mCache[key] = sequence;
    return sequence;
}

SequencePlayer* CinematicSystem::CreatePlayer(std::shared_ptr<const CinematicSequence> sequence)
{
    if (!sequence)
        return nullptr;
    auto player = std::make_unique<SequencePlayer>(std::move(sequence), mHost);
    player->mSystem = this;
    mPlayers.push_back(std::move(player));
    return mPlayers.back().get();
}

SequencePlayer* CinematicSystem::Play(std::shared_ptr<const CinematicSequence> sequence)
{
    SequencePlayer* player = CreatePlayer(std::move(sequence));
    if (player != nullptr)
        player->Play();
    return player;
}

SequencePlayer* CinematicSystem::Play(const std::string& assetPath)
{
    return Play(Load(assetPath));
}

SequencePlayer* CinematicSystem::FindPlayer(std::uint32_t handle) const
{
    for (const std::unique_ptr<SequencePlayer>& player : mPlayers)
    {
        if (player->Handle() == handle)
            return player.get();
    }
    return nullptr;
}

void CinematicSystem::Release(SequencePlayer* player)
{
    if (player == nullptr)
        return;
    if (std::find(mPendingRelease.begin(), mPendingRelease.end(), player) == mPendingRelease.end())
        mPendingRelease.push_back(player);
    if (!mTicking)
        CollectReleased();
}

void CinematicSystem::CollectReleased()
{
    // A player inside one of its own callbacks stays until that call has unwound.
    std::vector<SequencePlayer*> deferred;
    for (SequencePlayer* player : mPendingRelease)
    {
        if (player->mCallbackDepth > 0)
        {
            deferred.push_back(player);
            continue;
        }
        mPlayers.erase(std::remove_if(mPlayers.begin(), mPlayers.end(),
                                      [player](const std::unique_ptr<SequencePlayer>& p) { return p.get() == player; }),
                       mPlayers.end());
    }
    mPendingRelease = std::move(deferred);
}

void CinematicSystem::StopAll()
{
    // Indexed: a stop callback may start another player, which appends.
    for (std::size_t i = 0; i < mPlayers.size(); ++i)
        mPlayers[i]->Stop();
}

void CinematicSystem::Clear()
{
    StopAll();
    mPendingRelease.clear();
    mPlayers.clear();
}

void CinematicSystem::ClearListeners()
{
    mEventListeners.clear();
    mStartedListeners.clear();
    mFinishedListeners.clear();
}

void CinematicSystem::Tick(double deltaSeconds)
{
    mTicking = true;
    // Indexed for the same reason as StopAll: an event handler may start a new player.
    for (std::size_t i = 0; i < mPlayers.size(); ++i)
        mPlayers[i]->Tick(deltaSeconds);
    mTicking = false;
    CollectReleased();
}
