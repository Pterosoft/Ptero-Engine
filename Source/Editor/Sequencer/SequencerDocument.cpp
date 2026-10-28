#include "SequencerDocument.h"

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace
{
    constexpr std::size_t kMaxUndoSteps = 200;

    // Values travel double -> float -> double through a component, so equality has to
    // allow float rounding at any magnitude: 5000 lumens round-trips with an error near
    // 3e-4, far above any fixed epsilon that would still notice a real edit to a small
    // value.
    bool Close(double a, double b)
    {
        return std::fabs(a - b) <= 1e-6 + 1e-5 * (std::max)(std::fabs(a), std::fabs(b));
    }

    // Copied keys, kept outside any one document so they paste into whichever sequence is
    // open next. Times are relative to the earliest copied key.
    struct ClipboardKey
    {
        Guid Track;
        int Channel = 0;
        Keyframe Key;
    };
    struct ClipboardEvent
    {
        Guid Track;
        SequenceEventKey Key;
    };
    std::vector<ClipboardKey> gClipboardKeys;
    std::vector<ClipboardEvent> gClipboardEvents;

    PropertyTrack* AsProperty(SequenceTrack* track) { return dynamic_cast<PropertyTrack*>(track); }
    EventTrack* AsEvents(SequenceTrack* track) { return dynamic_cast<EventTrack*>(track); }

    SequenceEventKey* FindEventKey(EventTrack& track, std::uint32_t id)
    {
        for (SequenceEventKey& key : track.MutableKeys())
        {
            if (key.Id == id)
                return &key;
        }
        return nullptr;
    }
}

SequencerDocument::SequencerDocument(CinematicHost* host)
    : mHost(host), mSequence(std::make_shared<CinematicSequence>())
{
    mPlayer = std::make_unique<SequencePlayer>(std::shared_ptr<const CinematicSequence>(mSequence), host);
    mPlayer->RestoreStateOnStop = true;
}

SequencerDocument::~SequencerDocument()
{
    EndPreview();
}

// ---------------------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------------------

void SequencerDocument::AfterDataReplaced()
{
    mSelectedKeys.clear();
    SelectedCut = Guid{};
    mMoving = false;
    mMoveOrigins.clear();
    mEditCut = Guid{};
    mTransactionDepth = 0;
    ++mRevision;
    ++mStructureRevision;
    mEvaluationDirty = true;
    mObserved.clear();
}

void SequencerDocument::ResetPlayer()
{
    // A fresh player: the playhead returns to the start without evaluating, and nothing
    // recorded for the previous sequence survives into this one.
    const bool looping = mPlayer && mPlayer->IsLooping();
    mPlayer = std::make_unique<SequencePlayer>(std::shared_ptr<const CinematicSequence>(mSequence), mHost);
    mPlayer->RestoreStateOnStop = true;
    mPlayer->SetLooping(looping);
    if (OnPlayerCreated)
        OnPlayerCreated(*mPlayer);
}

void SequencerDocument::New()
{
    EndPreview();
    *mSequence = CinematicSequence{};
    mPath.clear();
    mDirty = false;
    ResetHistory();
    AfterDataReplaced();
    ResetPlayer();
}

bool SequencerDocument::Load(const std::string& path, std::string& error)
{
    CinematicSequence loaded;
    if (!loaded.LoadFromFile(path, error))
        return false;

    EndPreview();
    *mSequence = std::move(loaded);
    mPath = path;
    mDirty = false;
    ResetHistory();
    AfterDataReplaced();
    // Evaluated by the next Tick, once the window is ready to show the result.
    ResetPlayer();
    return true;
}

bool SequencerDocument::Save(const std::string& path, std::string& error)
{
    if (!mSequence->SaveToFile(path, error))
        return false;
    mPath = path;
    mDirty = false;
    ++mRevision;
    return true;
}

void SequencerDocument::MarkChanged(bool structural)
{
    ++mRevision;
    if (structural)
        ++mStructureRevision;
    mDirty = true;
    mEvaluationDirty = true;
}

// ---------------------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------------------

SequencerDocument::ScopeState SequencerDocument::Capture(unsigned flags, const std::set<Guid>& bindings) const
{
    ScopeState state;
    state.Flags = flags;
    state.Order.reserve(mSequence->Bindings.size());
    for (const SequenceBinding& binding : mSequence->Bindings)
        state.Order.push_back(binding.Id);

    if ((flags & Scope_AllBindings) != 0)
        state.Bindings.insert(state.Order.begin(), state.Order.end());
    else
        state.Bindings = bindings;

    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        if (state.Bindings.count(binding.Id) != 0)
            state.BindingJson[binding.Id] = binding.ToJson();
    }

    if ((flags & Scope_Header) != 0)
    {
        const CinematicSequence& s = *mSequence;
        state.Header = { { "Name", s.Name },
                         { "Rate", { s.DisplayRate.Numerator, s.DisplayRate.Denominator } },
                         { "Playback", { s.PlaybackStart, s.PlaybackEnd } },
                         { "Work", { s.WorkStart, s.WorkEnd } },
                         { "Metadata", s.Metadata } };
    }

    if ((flags & Scope_MasterTracks) != 0)
    {
        state.MasterTracks = json::array();
        for (const std::unique_ptr<SequenceTrack>& track : mSequence->MasterTracks)
        {
            json j;
            SequenceTrackFactory::SaveTrack(*track, j);
            state.MasterTracks.push_back(std::move(j));
        }
    }
    return state;
}

void SequencerDocument::Apply(const ScopeState& state)
{
    CinematicSequence& s = *mSequence;

    // Rebuild the binding list in the captured order: scoped bindings come from the
    // capture (absent ones are dropped), unscoped ones are carried over as they are.
    std::vector<SequenceBinding> rebuilt;
    rebuilt.reserve(state.Order.size());
    for (const Guid& id : state.Order)
    {
        if (state.Bindings.count(id) != 0)
        {
            const auto it = state.BindingJson.find(id);
            if (it == state.BindingJson.end())
                continue;
            SequenceBinding binding;
            std::string error;
            binding.FromJson(it->second, error);
            rebuilt.push_back(std::move(binding));
            continue;
        }

        if (SequenceBinding* existing = s.FindBinding(id))
            rebuilt.push_back(std::move(*existing));
    }
    s.Bindings = std::move(rebuilt);

    if ((state.Flags & Scope_Header) != 0 && state.Header.is_object())
    {
        s.Name = state.Header.value("Name", s.Name);
        s.DisplayRate.Numerator = state.Header["Rate"][0].get<int>();
        s.DisplayRate.Denominator = state.Header["Rate"][1].get<int>();
        s.PlaybackStart = state.Header["Playback"][0].get<double>();
        s.PlaybackEnd = state.Header["Playback"][1].get<double>();
        s.WorkStart = state.Header["Work"][0].get<double>();
        s.WorkEnd = state.Header["Work"][1].get<double>();
        s.Metadata = state.Header["Metadata"].get<std::map<std::string, std::string>>();
    }

    if ((state.Flags & Scope_MasterTracks) != 0 && state.MasterTracks.is_array())
    {
        s.MasterTracks.clear();
        for (const json& j : state.MasterTracks)
        {
            std::string error;
            if (std::unique_ptr<SequenceTrack> track = SequenceTrackFactory::LoadTrack(j, error))
                s.MasterTracks.push_back(std::move(track));
        }
    }
}

bool SequencerDocument::SameState(const ScopeState& a, const ScopeState& b) const
{
    if (a.Order != b.Order || a.BindingJson.size() != b.BindingJson.size())
        return false;
    for (const auto& [id, j] : a.BindingJson)
    {
        const auto it = b.BindingJson.find(id);
        if (it == b.BindingJson.end() || it->second != j)
            return false;
    }
    return a.Header == b.Header && a.MasterTracks == b.MasterTracks;
}

void SequencerDocument::Begin(const std::string& label, unsigned flags, const std::vector<Guid>& bindings)
{
    if (mTransactionDepth++ == 0)
    {
        mOpen = Transaction{};
        mOpen.Label = label;
        mOpen.Before = Capture(flags, std::set<Guid>(bindings.begin(), bindings.end()));
        return;
    }

    // Joining an open transaction: whatever it did not cover yet is captured now, which is
    // still "before" for those parts - the transaction could not have touched them.
    ScopeState& before = mOpen.Before;
    std::set<Guid> added;
    if ((flags & Scope_AllBindings) != 0)
    {
        for (const SequenceBinding& binding : mSequence->Bindings)
        {
            if (before.Bindings.count(binding.Id) == 0)
                added.insert(binding.Id);
        }
    }
    for (const Guid& id : bindings)
    {
        if (before.Bindings.count(id) == 0)
            added.insert(id);
    }
    const unsigned newFlags = flags & ~before.Flags & (Scope_Header | Scope_MasterTracks);
    if (added.empty() && newFlags == 0)
        return;

    const ScopeState extra = Capture(newFlags, added);
    before.Flags |= newFlags | (flags & Scope_AllBindings);
    for (const Guid& id : added)
    {
        before.Bindings.insert(id);
        if (const auto it = extra.BindingJson.find(id); it != extra.BindingJson.end())
            before.BindingJson[id] = it->second;
    }
    if ((newFlags & Scope_Header) != 0)
        before.Header = extra.Header;
    if ((newFlags & Scope_MasterTracks) != 0)
        before.MasterTracks = extra.MasterTracks;
}

void SequencerDocument::Commit()
{
    if (mTransactionDepth == 0 || --mTransactionDepth > 0)
        return;

    // Bindings that exist now but did not before were added by this transaction: they
    // join the scope, absent in "before", so undo removes them and redo restores them.
    std::set<Guid> scope = mOpen.Before.Bindings;
    const std::set<Guid> previous(mOpen.Before.Order.begin(), mOpen.Before.Order.end());
    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        if (previous.count(binding.Id) == 0)
            scope.insert(binding.Id);
    }
    mOpen.Before.Bindings = scope;
    mOpen.After = Capture(mOpen.Before.Flags & ~Scope_AllBindings, scope);
    mOpen.After.Flags = mOpen.Before.Flags;

    if (SameState(mOpen.Before, mOpen.After))
        return;

    mUndo.push_back(std::move(mOpen));
    if (mUndo.size() > kMaxUndoSteps)
        mUndo.erase(mUndo.begin());
    mRedo.clear();
    mDirty = true;
}

void SequencerDocument::Cancel()
{
    if (mTransactionDepth == 0 || --mTransactionDepth > 0)
        return;
    Apply(mOpen.Before);
    mSelectedKeys.clear();
    SelectedCut = Guid{};
    MarkChanged(true);
}

void SequencerDocument::Discard()
{
    if (mTransactionDepth > 0)
        --mTransactionDepth;
}

void SequencerDocument::Undo()
{
    if (mUndo.empty() || mTransactionDepth > 0)
        return;
    Transaction transaction = std::move(mUndo.back());
    mUndo.pop_back();
    Apply(transaction.Before);
    mRedo.push_back(std::move(transaction));
    // Key ids are reissued when a binding is rebuilt from its saved form.
    ClearSelection();
    MarkChanged(true);
}

void SequencerDocument::Redo()
{
    if (mRedo.empty() || mTransactionDepth > 0)
        return;
    Transaction transaction = std::move(mRedo.back());
    mRedo.pop_back();
    Apply(transaction.After);
    mUndo.push_back(std::move(transaction));
    ClearSelection();
    MarkChanged(true);
}

void SequencerDocument::ResetHistory()
{
    mUndo.clear();
    mRedo.clear();
    mTransactionDepth = 0;
}

// ---------------------------------------------------------------------------------------
// Lookup and selection
// ---------------------------------------------------------------------------------------

SequenceTrack* SequencerDocument::FindTrack(const Guid& trackId, SequenceBinding** binding)
{
    for (SequenceBinding& candidate : mSequence->Bindings)
    {
        if (SequenceTrack* track = candidate.FindTrack(trackId))
        {
            if (binding != nullptr)
                *binding = &candidate;
            return track;
        }
    }
    for (const std::unique_ptr<SequenceTrack>& track : mSequence->MasterTracks)
    {
        if (track->Id == trackId)
        {
            if (binding != nullptr)
                *binding = nullptr;
            return track.get();
        }
    }
    return nullptr;
}

std::vector<Guid> SequencerDocument::BindingsOfTracks(const std::set<Guid>& tracks)
{
    std::vector<Guid> bindings;
    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        for (const std::unique_ptr<SequenceTrack>& track : binding.Tracks)
        {
            if (tracks.count(track->Id) != 0)
            {
                bindings.push_back(binding.Id);
                break;
            }
        }
    }
    return bindings;
}

void SequencerDocument::ClearSelection()
{
    mSelectedKeys.clear();
    SelectedCut = Guid{};
}

bool SequencerDocument::KeyTime(const SequencerKeyHandle& handle, double& time)
{
    SequenceTrack* track = FindTrack(handle.Track);
    if (track == nullptr)
        return false;

    if (handle.Channel == SequencerKeyHandle::kEventChannel)
    {
        EventTrack* events = AsEvents(track);
        const SequenceEventKey* key = events != nullptr ? FindEventKey(*events, handle.Key) : nullptr;
        if (key == nullptr)
            return false;
        time = key->Time;
        return true;
    }

    PropertyTrack* property = AsProperty(track);
    if (property == nullptr || handle.Channel < 0 || handle.Channel >= property->ChannelCount())
        return false;
    const Keyframe* key = property->Channel(handle.Channel).FindKeyById(handle.Key);
    if (key == nullptr)
        return false;
    time = key->Time;
    return true;
}

void SequencerDocument::PruneSelection()
{
    for (auto it = mSelectedKeys.begin(); it != mSelectedKeys.end();)
    {
        double time = 0.0;
        it = KeyTime(*it, time) ? std::next(it) : mSelectedKeys.erase(it);
    }

    if (SelectedCut.IsValid())
    {
        const CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
        const bool exists = cuts != nullptr &&
            std::any_of(cuts->Sections.begin(), cuts->Sections.end(),
                        [&](const CameraCutSection& section) { return section.Id == SelectedCut; });
        if (!exists)
            SelectedCut = Guid{};
    }
}

std::set<Guid> SequencerDocument::SelectedTracks() const
{
    std::set<Guid> tracks;
    for (const SequencerKeyHandle& key : mSelectedKeys)
        tracks.insert(key.Track);
    return tracks;
}

void SequencerDocument::BeginForSelection(const std::string& label)
{
    const std::set<Guid> tracks = SelectedTracks();
    unsigned flags = SelectedCut.IsValid() ? Scope_MasterTracks : Scope_None;
    for (const std::unique_ptr<SequenceTrack>& track : mSequence->MasterTracks)
    {
        if (tracks.count(track->Id) != 0)
            flags |= Scope_MasterTracks;
    }
    Begin(label, flags, BindingsOfTracks(tracks));
}

// ---------------------------------------------------------------------------------------
// Key editing
// ---------------------------------------------------------------------------------------

double SequencerDocument::Snap(double time) const
{
    return SnapEnabled ? mSequence->DisplayRate.SnapToFrame(time) : time;
}

void SequencerDocument::BeginMoveKeys(const SequencerKeyHandle& anchor)
{
    if (mSelectedKeys.empty())
        return;

    BeginForSelection(mSelectedKeys.size() == 1 ? "Move Key" : "Move Keys");
    mMoveOrigins.clear();
    for (const SequencerKeyHandle& key : mSelectedKeys)
    {
        double time = 0.0;
        if (KeyTime(key, time))
            mMoveOrigins.push_back({ key, time });
    }
    mMoveAnchor = mSelectedKeys.count(anchor) != 0 ? anchor : *mSelectedKeys.begin();
    mMoving = true;
}

void SequencerDocument::UpdateMoveKeys(double rawDelta)
{
    if (!mMoving)
        return;

    double anchorOrigin = 0.0;
    for (const MoveOrigin& origin : mMoveOrigins)
    {
        if (origin.Key == mMoveAnchor)
            anchorOrigin = origin.Time;
    }
    // One offset for every key, picked so the grabbed key lands on a frame. Snapping each
    // key on its own would pull keys that sit between frames apart from their neighbours.
    const double delta = Snap(anchorOrigin + rawDelta) - anchorOrigin;

    std::set<std::pair<Guid, int>> touchedChannels;
    std::set<Guid> touchedEventTracks;
    for (const MoveOrigin& origin : mMoveOrigins)
    {
        SequenceTrack* track = FindTrack(origin.Key.Track);
        if (track == nullptr)
            continue;

        if (origin.Key.Channel == SequencerKeyHandle::kEventChannel)
        {
            if (EventTrack* events = AsEvents(track))
            {
                if (SequenceEventKey* key = FindEventKey(*events, origin.Key.Key))
                {
                    key->Time = origin.Time + delta;
                    touchedEventTracks.insert(track->Id);
                }
            }
            continue;
        }

        if (PropertyTrack* property = AsProperty(track))
        {
            if (origin.Key.Channel < property->ChannelCount())
            {
                if (Keyframe* key = property->Channel(origin.Key.Channel).MutableKeyById(origin.Key.Key))
                {
                    key->Time = origin.Time + delta;
                    touchedChannels.insert({ track->Id, origin.Key.Channel });
                }
            }
        }
    }

    for (const auto& [trackId, channel] : touchedChannels)
        AsProperty(FindTrack(trackId))->Channel(channel).OnKeysChanged();
    for (const Guid& trackId : touchedEventTracks)
        AsEvents(FindTrack(trackId))->OnKeysChanged();
    MarkChanged(false);
}

void SequencerDocument::EndMoveKeys()
{
    if (!mMoving)
        return;
    mMoving = false;

    // A moved key that lands on an unselected key replaces it, as overwriting a key in
    // place would: two keys at one instant make a step nobody asked for.
    for (const MoveOrigin& origin : mMoveOrigins)
    {
        if (origin.Key.Channel == SequencerKeyHandle::kEventChannel)
            continue;
        PropertyTrack* property = AsProperty(FindTrack(origin.Key.Track));
        if (property == nullptr || origin.Key.Channel >= property->ChannelCount())
            continue;
        FloatChannel& channel = property->Channel(origin.Key.Channel);
        const Keyframe* moved = channel.FindKeyById(origin.Key.Key);
        if (moved == nullptr)
            continue;
        const double time = moved->Time;

        std::vector<std::uint32_t> doomed;
        for (const Keyframe& other : channel.Keys())
        {
            if (other.Id != origin.Key.Key && std::fabs(other.Time - time) < 1e-6 &&
                mSelectedKeys.count({ origin.Key.Track, origin.Key.Channel, other.Id }) == 0)
                doomed.push_back(other.Id);
        }
        for (std::uint32_t id : doomed)
            channel.RemoveKeyById(id);
    }

    mMoveOrigins.clear();
    MarkChanged(false);
    Commit();
}

void SequencerDocument::CancelMoveKeys()
{
    if (!mMoving)
        return;
    UpdateMoveKeys(0.0);
    // Snapping may have nudged the anchor; put every key back exactly.
    for (const MoveOrigin& origin : mMoveOrigins)
    {
        SequenceTrack* track = FindTrack(origin.Key.Track);
        if (PropertyTrack* property = AsProperty(track))
        {
            if (Keyframe* key = property->Channel(origin.Key.Channel).MutableKeyById(origin.Key.Key))
                key->Time = origin.Time;
            property->Channel(origin.Key.Channel).OnKeysChanged();
        }
        else if (EventTrack* events = AsEvents(track))
        {
            if (SequenceEventKey* key = FindEventKey(*events, origin.Key.Key))
                key->Time = origin.Time;
            events->OnKeysChanged();
        }
    }
    mMoving = false;
    mMoveOrigins.clear();
    MarkChanged(false);
    Discard();
}

void SequencerDocument::ScaleSelectedKeys(double factor, double pivot)
{
    if (mSelectedKeys.empty() || !(factor > 0.0))
        return;

    BeginForSelection("Scale Keys");
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        SequenceTrack* track = FindTrack(handle.Track);
        if (PropertyTrack* property = AsProperty(track))
        {
            if (Keyframe* key = property->Channel(handle.Channel).MutableKeyById(handle.Key))
                key->Time = pivot + (key->Time - pivot) * factor;
        }
        else if (EventTrack* events = AsEvents(track))
        {
            if (SequenceEventKey* key = FindEventKey(*events, handle.Key))
                key->Time = pivot + (key->Time - pivot) * factor;
        }
    }
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        SequenceTrack* track = FindTrack(handle.Track);
        if (PropertyTrack* property = AsProperty(track))
            property->Channel(handle.Channel).OnKeysChanged();
        else if (EventTrack* events = AsEvents(track))
            events->OnKeysChanged();
    }
    MarkChanged(false);
    Commit();
}

void SequencerDocument::DeleteSelectedKeys()
{
    if (mSelectedKeys.empty() && !SelectedCut.IsValid())
        return;

    BeginForSelection("Delete");
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        SequenceTrack* track = FindTrack(handle.Track);
        if (PropertyTrack* property = AsProperty(track))
        {
            if (handle.Channel < property->ChannelCount())
                property->Channel(handle.Channel).RemoveKeyById(handle.Key);
        }
        else if (EventTrack* events = AsEvents(track))
        {
            events->RemoveKeyById(handle.Key);
        }
    }
    if (SelectedCut.IsValid())
        RemoveCut(SelectedCut);
    ClearSelection();
    MarkChanged(false);
    Commit();
}

void SequencerDocument::SetSelectedInterpolation(KeyInterpolation interpolation)
{
    BeginForSelection("Set Interpolation");
    std::set<std::pair<Guid, int>> channels;
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        PropertyTrack* property = AsProperty(FindTrack(handle.Track));
        if (property == nullptr)
            continue;
        if (Keyframe* key = property->Channel(handle.Channel).MutableKeyById(handle.Key))
        {
            key->Interpolation = interpolation;
            channels.insert({ handle.Track, handle.Channel });
        }
    }
    for (const auto& [trackId, channel] : channels)
        AsProperty(FindTrack(trackId))->Channel(channel).OnKeysChanged();
    MarkChanged(false);
    Commit();
}

void SequencerDocument::SetSelectedTangentMode(TangentMode mode)
{
    BeginForSelection("Set Tangents");
    std::set<std::pair<Guid, int>> channels;
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        PropertyTrack* property = AsProperty(FindTrack(handle.Track));
        if (property == nullptr)
            continue;
        if (Keyframe* key = property->Channel(handle.Channel).MutableKeyById(handle.Key))
        {
            key->Tangents = mode;
            if (mode == TangentMode::Unified)
                key->ArriveTangent = key->LeaveTangent;
            // A tangent the user can edit needs a segment that reads it.
            if (mode != TangentMode::Auto && key->Interpolation == KeyInterpolation::Auto)
                key->Interpolation = KeyInterpolation::Cubic;
            channels.insert({ handle.Track, handle.Channel });
        }
    }
    for (const auto& [trackId, channel] : channels)
        AsProperty(FindTrack(trackId))->Channel(channel).OnKeysChanged();
    MarkChanged(false);
    Commit();
}

void SequencerDocument::CopySelectedKeys()
{
    double earliest = 0.0;
    bool any = false;
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        double time = 0.0;
        if (KeyTime(handle, time))
        {
            earliest = any ? (std::min)(earliest, time) : time;
            any = true;
        }
    }
    if (!any)
        return;

    gClipboardKeys.clear();
    gClipboardEvents.clear();
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        SequenceTrack* track = FindTrack(handle.Track);
        if (PropertyTrack* property = AsProperty(track))
        {
            if (const Keyframe* key = property->Channel(handle.Channel).FindKeyById(handle.Key))
            {
                ClipboardKey copy{ handle.Track, handle.Channel, *key };
                copy.Key.Time -= earliest;
                gClipboardKeys.push_back(copy);
            }
        }
        else if (EventTrack* events = AsEvents(track))
        {
            if (const SequenceEventKey* key = FindEventKey(*events, handle.Key))
            {
                ClipboardEvent copy{ handle.Track, *key };
                copy.Key.Time -= earliest;
                gClipboardEvents.push_back(copy);
            }
        }
    }
}

bool SequencerDocument::CanPaste() const
{
    return !gClipboardKeys.empty() || !gClipboardEvents.empty();
}

void SequencerDocument::Paste(double time)
{
    if (!CanPaste())
        return;

    std::set<Guid> tracks;
    for (const ClipboardKey& key : gClipboardKeys)
        tracks.insert(key.Track);
    unsigned flags = gClipboardEvents.empty() ? Scope_None : Scope_MasterTracks;
    Begin("Paste Keys", flags, BindingsOfTracks(tracks));

    mSelectedKeys.clear();
    const double base = Snap(time);
    for (const ClipboardKey& clip : gClipboardKeys)
    {
        PropertyTrack* property = AsProperty(FindTrack(clip.Track));
        if (property == nullptr || clip.Channel >= property->ChannelCount())
            continue;
        FloatChannel& channel = property->Channel(clip.Channel);
        Keyframe key = clip.Key;
        key.Time = base + clip.Key.Time;
        // Pasting over an existing key replaces it.
        const int existing = channel.FindKeyIndexAt(key.Time, 1e-6);
        if (existing >= 0)
            channel.RemoveKeyById(channel.Keys()[existing].Id);
        const std::uint32_t id = channel.InsertKey(key);
        mSelectedKeys.insert({ clip.Track, clip.Channel, id });
    }
    for (const ClipboardEvent& clip : gClipboardEvents)
    {
        EventTrack* events = AsEvents(FindTrack(clip.Track));
        if (events == nullptr)
            continue;
        const std::uint32_t id = events->AddKey(base + clip.Key.Time, clip.Key.Name, clip.Key.Payload);
        mSelectedKeys.insert({ clip.Track, SequencerKeyHandle::kEventChannel, id });
    }
    MarkChanged(false);
    Commit();
}

void SequencerDocument::DuplicateSelectedKeys()
{
    double last = 0.0;
    bool any = false;
    for (const SequencerKeyHandle& handle : mSelectedKeys)
    {
        double time = 0.0;
        if (KeyTime(handle, time))
        {
            last = any ? (std::max)(last, time) : time;
            any = true;
        }
    }
    if (!any)
        return;
    CopySelectedKeys();
    Paste(last + mSequence->DisplayRate.FrameDuration());
}

void SequencerDocument::AddKey(const Guid& trackId, double time, unsigned channelMask)
{
    SequenceBinding* binding = nullptr;
    PropertyTrack* property = AsProperty(FindTrack(trackId, &binding));
    if (property == nullptr || binding == nullptr)
        return;

    PropertyValue value;
    value.Type = property->ValueType();
    bool fromWorld = false;
    if (mHost != nullptr && !IsPreviewSuspended())
    {
        void* owner = mHost->ResolveBinding(*binding);
        const PropertyDescriptor* descriptor = property->ResolveProperty(binding->Domain);
        fromWorld = owner != nullptr && descriptor != nullptr && descriptor->IsAvailableOn(owner) && descriptor->Get &&
                    descriptor->Get(owner, value);
    }
    if (!fromWorld)
    {
        for (int i = 0; i < property->ChannelCount() && i < PropertyValue::kMaxComponents; ++i)
            value.Components[i] = property->Channel(i).Evaluate(time);
    }

    Begin("Add Key", Scope_None, { binding->Id });
    property->SetKey(Snap(time), value, DefaultInterpolation, channelMask);
    MarkChanged(false);
    Commit();
}

// ---------------------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------------------

Guid SequencerDocument::AddEntityBinding(std::uint64_t entityId, const std::string& name)
{
    if (SequenceBinding* existing = mSequence->FindBindingForEntity(entityId))
        return existing->Id;

    Begin("Add " + name, Scope_None);
    SequenceBinding binding;
    binding.Name = name;
    binding.Domain = "Entity";
    binding.EntityId = entityId;
    binding.EntityName = name;
    const Guid id = binding.Id;
    mSequence->Bindings.push_back(std::move(binding));
    MarkChanged(true);
    Commit();
    return id;
}

Guid SequencerDocument::AddEnvironmentBinding()
{
    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        if (binding.Domain == "Environment")
            return binding.Id;
    }

    Begin("Add Environment", Scope_None);
    SequenceBinding binding;
    binding.Name = "Environment";
    binding.Domain = "Environment";
    const Guid id = binding.Id;
    mSequence->Bindings.push_back(std::move(binding));
    MarkChanged(true);
    Commit();
    return id;
}

void SequencerDocument::RemoveBinding(const Guid& bindingId)
{
    if (mSequence->FindBinding(bindingId) == nullptr)
        return;
    Begin("Delete Binding", Scope_None, { bindingId });
    auto& bindings = mSequence->Bindings;
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                  [&](const SequenceBinding& binding) { return binding.Id == bindingId; }),
                   bindings.end());
    PruneSelection();
    MarkChanged(true);
    Commit();
}

void SequencerDocument::RenameBinding(const Guid& bindingId, const std::string& name)
{
    SequenceBinding* binding = mSequence->FindBinding(bindingId);
    if (binding == nullptr || binding->Name == name)
        return;
    Begin("Rename Binding", Scope_None, { bindingId });
    binding->Name = name;
    MarkChanged(true);
    Commit();
}

void SequencerDocument::RebindEntity(const Guid& bindingId, std::uint64_t entityId, const std::string& name)
{
    SequenceBinding* binding = mSequence->FindBinding(bindingId);
    if (binding == nullptr || binding->Domain != "Entity")
        return;
    Begin("Rebind", Scope_None, { bindingId });
    // The old object goes back to how it was before being re-pointed away from.
    EndPreview();
    if (binding->Name == binding->EntityName)
        binding->Name = name;
    binding->EntityId = entityId;
    binding->EntityName = name;
    MarkChanged(true);
    Commit();
}

PropertyTrack* SequencerDocument::AddPropertyTrack(const Guid& bindingId, const PropertyDescriptor& property,
                                                   bool keyNow, double time)
{
    SequenceBinding* binding = mSequence->FindBinding(bindingId);
    if (binding == nullptr)
        return nullptr;
    if (PropertyTrack* existing = binding->FindPropertyTrack(property.Path))
        return existing;

    Begin("Add Track", Scope_None, { bindingId });
    auto track = std::make_unique<PropertyTrack>(property.Path, property.Type);
    track->Name = property.DisplayName.empty() ? property.Path : property.DisplayName;
    PropertyTrack* result = track.get();

    if (keyNow && mHost != nullptr && !IsPreviewSuspended())
    {
        void* owner = mHost->ResolveBinding(*binding);
        PropertyValue value;
        if (owner != nullptr && property.IsAvailableOn(owner) && property.Get && property.Get(owner, value))
            track->SetKey(Snap(time), value, DefaultInterpolation);
    }

    binding->Tracks.push_back(std::move(track));
    MarkChanged(true);
    Commit();
    return result;
}

void SequencerDocument::RemoveTrack(const Guid& trackId)
{
    SequenceBinding* binding = nullptr;
    SequenceTrack* track = FindTrack(trackId, &binding);
    if (track == nullptr)
        return;

    if (binding != nullptr)
    {
        Begin("Delete Track", Scope_None, { binding->Id });
        auto& tracks = binding->Tracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const std::unique_ptr<SequenceTrack>& t) { return t->Id == trackId; }),
                     tracks.end());
    }
    else
    {
        Begin("Delete Track", Scope_MasterTracks);
        auto& tracks = mSequence->MasterTracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const std::unique_ptr<SequenceTrack>& t) { return t->Id == trackId; }),
                     tracks.end());
    }
    PruneSelection();
    MarkChanged(true);
    Commit();
}

void SequencerDocument::SetTrackMuted(const Guid& trackId, bool muted)
{
    SequenceBinding* binding = nullptr;
    SequenceTrack* track = FindTrack(trackId, &binding);
    if (track == nullptr || track->Muted == muted)
        return;
    if (binding != nullptr)
        Begin(muted ? "Mute Track" : "Unmute Track", Scope_None, { binding->Id });
    else
        Begin(muted ? "Mute Track" : "Unmute Track", Scope_MasterTracks);
    track->Muted = muted;
    MarkChanged(true);
    Commit();
}

void SequencerDocument::AddCameraCut(double time, const Guid& cameraBinding)
{
    CinematicSequence& s = *mSequence;
    const double t = std::clamp(Snap(time), s.PlaybackStart, s.PlaybackEnd);

    Begin("Add Camera Cut", Scope_MasterTracks);
    CameraCutTrack& cuts = s.GetOrAddCameraCutTrack();
    cuts.SortSections();

    CameraCutSection* containing = nullptr;
    for (CameraCutSection& section : cuts.Sections)
    {
        if (section.Start <= t && t < section.End)
            containing = &section;
    }

    if (containing != nullptr && std::fabs(containing->Start - t) < 1e-6)
    {
        // A cut exactly at an existing cut's start re-targets that cut.
        containing->CameraBinding = cameraBinding;
        SelectedCut = containing->Id;
    }
    else
    {
        CameraCutSection cut;
        cut.Start = t;
        cut.CameraBinding = cameraBinding;
        if (containing != nullptr)
        {
            // Splits the cut it lands in: the earlier part keeps its camera.
            cut.End = containing->End;
            containing->End = t;
        }
        else
        {
            double next = s.PlaybackEnd;
            for (const CameraCutSection& section : cuts.Sections)
            {
                if (section.Start > t)
                    next = (std::min)(next, section.Start);
            }
            cut.End = next > t ? next : t + 1.0;
        }
        SelectedCut = cut.Id;
        cuts.Sections.push_back(cut);
    }

    cuts.SortSections();
    MarkChanged(true);
    Commit();
}

void SequencerDocument::SetCutCamera(const Guid& cutId, const Guid& cameraBinding)
{
    CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
    if (cuts == nullptr)
        return;
    for (CameraCutSection& section : cuts->Sections)
    {
        if (section.Id == cutId && section.CameraBinding != cameraBinding)
        {
            Begin("Set Cut Camera", Scope_MasterTracks);
            section.CameraBinding = cameraBinding;
            MarkChanged(false);
            Commit();
            return;
        }
    }
}

void SequencerDocument::RemoveCut(const Guid& cutId)
{
    CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
    if (cuts == nullptr)
        return;
    Begin("Delete Camera Cut", Scope_MasterTracks);
    auto& sections = cuts->Sections;
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [&](const CameraCutSection& section) { return section.Id == cutId; }),
                   sections.end());
    if (SelectedCut == cutId)
        SelectedCut = Guid{};
    MarkChanged(false);
    Commit();
}

void SequencerDocument::BeginEditCut(const Guid& cutId)
{
    CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
    if (cuts == nullptr)
        return;

    const CameraCutSection* cut = nullptr;
    for (const CameraCutSection& section : cuts->Sections)
    {
        if (section.Id == cutId)
            cut = &section;
    }
    if (cut == nullptr)
        return;

    Begin("Edit Camera Cut", Scope_MasterTracks);
    mEditCut = cutId;
    mEditCutStart = cut->Start;
    mEditCutEnd = cut->End;
    mEditCutBefore = Guid{};
    mEditCutAfter = Guid{};
    for (const CameraCutSection& section : cuts->Sections)
    {
        if (section.Id == cutId)
            continue;
        if (std::fabs(section.End - cut->Start) < 1e-6)
            mEditCutBefore = section.Id;
        if (std::fabs(section.Start - cut->End) < 1e-6)
            mEditCutAfter = section.Id;
    }
}

namespace
{
    CameraCutSection* FindCut(CameraCutTrack& cuts, const Guid& id)
    {
        if (!id.IsValid())
            return nullptr;
        for (CameraCutSection& section : cuts.Sections)
        {
            if (section.Id == id)
                return &section;
        }
        return nullptr;
    }
}

void SequencerDocument::UpdateCutEdge(int edge, double time)
{
    CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
    if (cuts == nullptr || !mEditCut.IsValid())
        return;
    CameraCutSection* cut = FindCut(*cuts, mEditCut);
    CameraCutSection* before = FindCut(*cuts, mEditCutBefore);
    CameraCutSection* after = FindCut(*cuts, mEditCutAfter);
    if (cut == nullptr)
        return;

    const double frame = mSequence->DisplayRate.FrameDuration();
    const double t = Snap(time);
    if (edge == 0)
    {
        const double low = before != nullptr ? before->Start + frame : -1.0e9;
        cut->Start = std::clamp(t, low, mEditCutEnd - frame);
        if (before != nullptr)
            before->End = cut->Start;
    }
    else
    {
        const double high = after != nullptr ? after->End - frame : 1.0e9;
        cut->End = std::clamp(t, mEditCutStart + frame, high);
        if (after != nullptr)
            after->Start = cut->End;
    }
    cuts->SortSections();
    MarkChanged(false);
}

void SequencerDocument::UpdateCutMove(double rawDelta)
{
    CameraCutTrack* cuts = mSequence->FindCameraCutTrack();
    if (cuts == nullptr || !mEditCut.IsValid())
        return;
    CameraCutSection* cut = FindCut(*cuts, mEditCut);
    CameraCutSection* before = FindCut(*cuts, mEditCutBefore);
    CameraCutSection* after = FindCut(*cuts, mEditCutAfter);
    if (cut == nullptr)
        return;

    const double frame = mSequence->DisplayRate.FrameDuration();
    const double length = mEditCutEnd - mEditCutStart;
    double start = Snap(mEditCutStart + rawDelta);
    // Neighbours that shared an edge give way but never collapse below a frame.
    if (before != nullptr)
        start = (std::max)(start, before->Start + frame);
    if (after != nullptr)
        start = (std::min)(start, after->End - frame - length);
    cut->Start = start;
    cut->End = start + length;
    if (before != nullptr)
        before->End = cut->Start;
    if (after != nullptr)
        after->Start = cut->End;
    cuts->SortSections();
    MarkChanged(false);
}

void SequencerDocument::EndEditCut()
{
    if (!mEditCut.IsValid())
        return;
    mEditCut = Guid{};
    Commit();
}

std::uint32_t SequencerDocument::AddEvent(double time, const std::string& name, const std::string& payload)
{
    Begin("Add Event", Scope_MasterTracks);
    EventTrack& events = mSequence->GetOrAddEventTrack();
    const std::uint32_t id = events.AddKey(Snap(time), name, payload);
    mSelectedKeys = { { events.Id, SequencerKeyHandle::kEventChannel, id } };
    MarkChanged(true);
    Commit();
    return id;
}

void SequencerDocument::EditEvent(std::uint32_t keyId, double time, const std::string& name, const std::string& payload)
{
    EventTrack* events = mSequence->FindEventTrack();
    SequenceEventKey* key = events != nullptr ? FindEventKey(*events, keyId) : nullptr;
    if (key == nullptr)
        return;
    Begin("Edit Event", Scope_MasterTracks);
    key->Time = time;
    key->Name = name;
    key->Payload = payload;
    events->OnKeysChanged();
    MarkChanged(false);
    Commit();
}

void SequencerDocument::SetFrameRate(const FrameRate& rate)
{
    if (mSequence->DisplayRate == rate)
        return;
    Begin("Set Frame Rate", Scope_Header);
    mSequence->DisplayRate = rate;
    MarkChanged(false);
    Commit();
}

void SequencerDocument::SetPlaybackRange(double start, double end)
{
    if (!(end > start))
        return;
    Begin("Set Playback Range", Scope_Header);
    mSequence->PlaybackStart = start;
    mSequence->PlaybackEnd = end;
    MarkChanged(false);
    Commit();
    SetTime(Time());
}

void SequencerDocument::SetWorkRange(double start, double end)
{
    // The view, saved with the sequence for convenience but not an edit: no undo step,
    // and on its own it does not make the sequence need saving.
    if (end > start)
    {
        mSequence->WorkStart = start;
        mSequence->WorkEnd = end;
    }
}

// ---------------------------------------------------------------------------------------
// Playhead and preview
// ---------------------------------------------------------------------------------------

void SequencerDocument::SetTime(double time)
{
    const double clamped = std::clamp(time, mSequence->PlaybackStart, mSequence->PlaybackEnd);
    if (IsPreviewSuspended())
        return;
    mPlayer->SetTime(clamped);
    RecordObservedValues();
    mEvaluationDirty = false;
}

void SequencerDocument::Tick(double deltaSeconds)
{
    if (IsPreviewSuspended())
        return;

    if (mPlayer->IsPlaying())
    {
        mPlayer->Tick(deltaSeconds);
        RecordObservedValues();
        mEvaluationDirty = false;
    }
    else if (mEvaluationDirty)
    {
        EvaluateNow();
    }
}

void SequencerDocument::EvaluateNow()
{
    if (IsPreviewSuspended())
        return;
    mPlayer->Evaluate();
    RecordObservedValues();
    mEvaluationDirty = false;
}

void SequencerDocument::SuspendPreview()
{
    if (mSuspendCount++ == 0)
    {
        mPlayer->Pause();
        mPlayer->RestoreState();
        mObserved.clear();
    }
}

void SequencerDocument::ResumePreview()
{
    if (mSuspendCount > 0 && --mSuspendCount == 0)
        mEvaluationDirty = true;
}

void SequencerDocument::EndPreview()
{
    mPlayer->Pause();
    mPlayer->RestoreState();
    mObserved.clear();
    mEvaluationDirty = true;
}

void SequencerDocument::OnWorldReplaced()
{
    mPlayer->Pause();
    mPlayer->ForgetRestoreState();
    mObserved.clear();
    mEvaluationDirty = true;
}

// ---------------------------------------------------------------------------------------
// Auto Key
// ---------------------------------------------------------------------------------------

void SequencerDocument::RecordObservedValues()
{
    mObserved.clear();
    if (mHost == nullptr)
        return;

    const PropertyDescriptor* transform = PropertyRegistry::Find("Entity", "Transform");
    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        void* owner = mHost->ResolveBinding(binding);
        if (owner == nullptr)
            continue;

        for (const std::unique_ptr<SequenceTrack>& track : binding.Tracks)
        {
            const auto* property = dynamic_cast<const PropertyTrack*>(track.get());
            const PropertyDescriptor* descriptor = property != nullptr ? property->ResolveProperty(binding.Domain) : nullptr;
            PropertyValue value;
            if (descriptor != nullptr && descriptor->IsAvailableOn(owner) && descriptor->Get &&
                descriptor->Get(owner, value))
                mObserved[{ binding.Id, descriptor->Path }] = value;
        }

        PropertyValue value;
        if (binding.Domain == "Entity" && transform != nullptr && transform->Get && transform->Get(owner, value))
            mObserved[{ binding.Id, transform->Path }] = value;
    }
}

bool SequencerDocument::OnWorldEdited()
{
    if (mHost == nullptr || IsPreviewSuspended() || mMoving || InTransaction())
        return false;
    if (!AutoKey)
    {
        // Not keying, but the edit is the new baseline for the next comparison.
        RecordObservedValues();
        return false;
    }

    struct Change
    {
        Guid Binding;
        Guid Track; // null: the binding needs a transform track first
        PropertyValue Value;
        PropertyValue Before;
        bool HasBefore = false;
        unsigned Mask = 0;
    };
    std::vector<Change> changes;

    const double time = Time();
    const PropertyDescriptor* transform = PropertyRegistry::Find("Entity", "Transform");

    for (const SequenceBinding& binding : mSequence->Bindings)
    {
        void* owner = mHost->ResolveBinding(binding);
        if (owner == nullptr)
            continue;

        bool hasTransformTrack = false;
        for (const std::unique_ptr<SequenceTrack>& track : binding.Tracks)
        {
            const auto* property = dynamic_cast<const PropertyTrack*>(track.get());
            if (property == nullptr || property->Muted)
                continue;
            const PropertyDescriptor* descriptor = property->ResolveProperty(binding.Domain);
            if (descriptor == nullptr || !descriptor->IsAvailableOn(owner) || !descriptor->Get)
                continue;
            if (descriptor->Path == "Transform")
                hasTransformTrack = true;

            PropertyValue current;
            if (!descriptor->Get(owner, current))
                continue;
            PropertyValue expected = current;
            property->EvaluateInto(time, expected);
            const auto observed = mObserved.find({ binding.Id, descriptor->Path });

            unsigned mask = 0;
            for (int i = 0; i < property->ChannelCount() && i < PropertyValue::kMaxComponents; ++i)
            {
                // A keyed channel changed if it no longer matches its curve; an unkeyed one
                // if it no longer matches what it was after the last evaluation.
                const bool changed = property->Channel(i).HasKeys()
                    ? !Close(current.Components[i], expected.Components[i])
                    : observed != mObserved.end() && !Close(current.Components[i], observed->second.Components[i]);
                if (changed)
                    mask |= 1u << i;
            }
            if (mask != 0)
            {
                Change change{ binding.Id, property->Id, current };
                change.Mask = mask;
                if (observed != mObserved.end())
                {
                    change.Before = observed->second;
                    change.HasBefore = true;
                }
                changes.push_back(change);
            }
        }

        // A bound entity moved without a transform track yet: start one.
        if (!hasTransformTrack && binding.Domain == "Entity" && transform != nullptr && transform->Get)
        {
            PropertyValue current;
            const auto observed = mObserved.find({ binding.Id, transform->Path });
            if (observed != mObserved.end() && transform->Get(owner, current) &&
                !current.NearlyEquals(observed->second, 0.0))
            {
                unsigned mask = 0;
                for (int i = 0; i < 9; ++i)
                {
                    if (!Close(current.Components[i], observed->second.Components[i]))
                        mask |= 1u << i;
                }
                if (mask != 0)
                {
                    Change change{ binding.Id, Guid{}, current };
                    change.Mask = mask;
                    change.Before = observed->second;
                    change.HasBefore = true;
                    changes.push_back(change);
                }
            }
        }
    }

    if (changes.empty())
    {
        RecordObservedValues();
        return false;
    }

    std::vector<Guid> bindings;
    for (const Change& change : changes)
        bindings.push_back(change.Binding);

    Begin("Auto Key", Scope_None, bindings);
    bool structural = false;
    for (const Change& change : changes)
    {
        SequenceBinding* binding = mSequence->FindBinding(change.Binding);
        if (binding == nullptr)
            continue;

        PropertyTrack* property = nullptr;
        if (change.Track.IsValid())
        {
            property = dynamic_cast<PropertyTrack*>(binding->FindTrack(change.Track));
        }
        else if (transform != nullptr)
        {
            auto track = std::make_unique<PropertyTrack>(transform->Path, transform->Type);
            track->Name = transform->DisplayName;
            property = track.get();
            binding->Tracks.push_back(std::move(track));
            structural = true;
        }
        if (property == nullptr)
            continue;

        // The level should return to how it was before this edit when the preview ends,
        // not to the edited value the player would otherwise record on first write.
        if (change.HasBefore)
            mPlayer->SeedRestoreState(*binding, property->PropertyPath(), change.Before);
        property->SetKey(Snap(time), change.Value, DefaultInterpolation, change.Mask);
    }
    MarkChanged(structural);
    Commit();

    // The keys reproduce what the user set, so this re-applies the same values.
    EvaluateNow();
    return true;
}
