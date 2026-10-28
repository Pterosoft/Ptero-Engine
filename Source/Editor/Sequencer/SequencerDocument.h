#pragma once

#include "System/Cinematics/CinematicSequence.h"
#include "System/Cinematics/SequencePlayer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// One key the Sequencer can select: a keyframe of a property track's channel, or an event
// marker (Channel == kEventChannel). Keys are named by id, which survives the re-sorting
// a drag causes, never by index.
struct SequencerKeyHandle
{
    static constexpr int kEventChannel = -1;

    Guid Track;
    int Channel = 0;
    std::uint32_t Key = 0;

    friend bool operator<(const SequencerKeyHandle& a, const SequencerKeyHandle& b)
    {
        if (a.Track != b.Track)
            return a.Track < b.Track;
        if (a.Channel != b.Channel)
            return a.Channel < b.Channel;
        return a.Key < b.Key;
    }
    friend bool operator==(const SequencerKeyHandle& a, const SequencerKeyHandle& b)
    {
        return a.Track == b.Track && a.Channel == b.Channel && a.Key == b.Key;
    }
};

// The Sequencer's editing session on one sequence: the data, its undo history, the
// selection, the playhead and the live preview. Qt-free - the window is a view onto this -
// so every edit the window can make is reachable from a test.
//
// Undo is command-based: a transaction records the parts of the sequence it may touch
// (the header, the master tracks, and the bindings it names) before and after, as JSON.
// Moving one key in a sequence of thousands stores the one binding it lives in, twice.
class SequencerDocument
{
public:
    explicit SequencerDocument(CinematicHost* host);
    ~SequencerDocument();

    // ---- Document ---------------------------------------------------------------------
    void New();
    bool Load(const std::string& path, std::string& error);
    // Saves to `path`, which becomes the document's path.
    bool Save(const std::string& path, std::string& error);
    const std::string& Path() const { return mPath; }
    bool IsDirty() const { return mDirty; }

    CinematicSequence& Sequence() { return *mSequence; }
    const CinematicSequence& Sequence() const { return *mSequence; }

    // Bumped on every change to the data; views cache against it.
    std::uint64_t Revision() const { return mRevision; }
    // Bumped when bindings or tracks are added, removed or renamed - the row layout.
    std::uint64_t StructureRevision() const { return mStructureRevision; }
    // For edits made directly to Sequence() inside a transaction.
    void MarkChanged(bool structural);

    // ---- Undo -------------------------------------------------------------------------
    enum ScopeFlags : unsigned
    {
        Scope_None = 0,
        Scope_Header = 1u << 0,      // name, frame rate, ranges, metadata
        Scope_MasterTracks = 1u << 1, // camera cuts, events
        Scope_AllBindings = 1u << 2,
    };

    // Opens a transaction covering `flags` plus `bindings`. Nested Begin calls join the
    // open transaction (widening its scope), so an operation built from smaller ones is
    // still one undo step.
    void Begin(const std::string& label, unsigned flags, const std::vector<Guid>& bindings = {});
    // Closes the outermost transaction. Records it only if something actually changed.
    void Commit();
    // Abandons the outermost transaction, restoring what it captured.
    void Cancel();
    bool InTransaction() const { return mTransactionDepth > 0; }

    bool CanUndo() const { return !mUndo.empty(); }
    bool CanRedo() const { return !mRedo.empty(); }
    std::string UndoLabel() const { return mUndo.empty() ? std::string() : mUndo.back().Label; }
    std::string RedoLabel() const { return mRedo.empty() ? std::string() : mRedo.back().Label; }
    void Undo();
    void Redo();

    // ---- Lookup -----------------------------------------------------------------------
    // A track anywhere in the sequence. `binding` receives its binding, or null for a
    // master track.
    SequenceTrack* FindTrack(const Guid& trackId, SequenceBinding** binding = nullptr);
    // The bindings owning the given tracks (master tracks contribute none).
    std::vector<Guid> BindingsOfTracks(const std::set<Guid>& tracks);

    // ---- Selection --------------------------------------------------------------------
    std::set<SequencerKeyHandle>& SelectedKeys() { return mSelectedKeys; }
    const std::set<SequencerKeyHandle>& SelectedKeys() const { return mSelectedKeys; }
    Guid SelectedCut;
    void ClearSelection();
    // Drops handles whose key no longer exists.
    void PruneSelection();
    bool KeyTime(const SequencerKeyHandle& key, double& time);
    // The tracks the selection touches, and the transaction scope that covers them.
    std::set<Guid> SelectedTracks() const;
    void BeginForSelection(const std::string& label);

    // ---- Key editing ------------------------------------------------------------------
    // Snaps to the display frame rate when snapping is on.
    double Snap(double time) const;
    bool SnapEnabled = true;
    KeyInterpolation DefaultInterpolation = KeyInterpolation::Auto;

    // Dragging keys: Begin records where every selected key started; Update moves them all
    // by the same offset, chosen so `anchor` lands on a frame when snapping, which keeps
    // their relative timing exact; End commits (and folds keys that landed on top of an
    // unselected key into it). Cancel puts them back.
    void BeginMoveKeys(const SequencerKeyHandle& anchor);
    void UpdateMoveKeys(double rawDelta);
    void EndMoveKeys();
    void CancelMoveKeys();
    bool IsMovingKeys() const { return mMoving; }

    // Scales the selected keys' times about `pivot`.
    void ScaleSelectedKeys(double factor, double pivot);
    void DeleteSelectedKeys();
    void SetSelectedInterpolation(KeyInterpolation interpolation);
    void SetSelectedTangentMode(TangentMode mode);

    // Copies the selected keys, relative to the earliest one.
    void CopySelectedKeys();
    bool CanPaste() const;
    // Pastes onto the tracks they came from, the earliest key at `time`. Selects the
    // pasted keys. Tracks that no longer exist are skipped.
    void Paste(double time);
    // Copy, then paste just after the selection.
    void DuplicateSelectedKeys();

    // Keys `track` at `time` with the bound object's current value (or, if the object is
    // missing, the curve's own value there). `channelMask` limits the channels.
    void AddKey(const Guid& track, double time, unsigned channelMask = ~0u);

    // ---- Structure --------------------------------------------------------------------
    // Returns the id of the new binding - or of the existing one for that entity.
    Guid AddEntityBinding(std::uint64_t entityId, const std::string& name);
    Guid AddEnvironmentBinding();
    void RemoveBinding(const Guid& binding);
    void RenameBinding(const Guid& binding, const std::string& name);
    // Points an existing binding at another entity, keeping its tracks.
    void RebindEntity(const Guid& binding, std::uint64_t entityId, const std::string& name);
    // Adds a track for a registered property (no-op returning the existing one if the
    // binding already animates it). Keys it at `time` with the current value when
    // `keyNow` is set and the object is present.
    PropertyTrack* AddPropertyTrack(const Guid& binding, const PropertyDescriptor& property, bool keyNow, double time);
    void RemoveTrack(const Guid& track);
    void SetTrackMuted(const Guid& track, bool muted);

    // Camera cuts. A new cut starts at `time` and runs to the next cut (or the end).
    void AddCameraCut(double time, const Guid& cameraBinding);
    void SetCutCamera(const Guid& cut, const Guid& cameraBinding);
    void RemoveCut(const Guid& cut);
    // Moves a cut boundary. `edge` 0 = start, 1 = end. An adjoining cut follows, so
    // dragging the line between two cuts changes how long each one lasts.
    void BeginEditCut(const Guid& cut);
    void UpdateCutEdge(int edge, double time);
    void UpdateCutMove(double rawDelta);
    void EndEditCut();

    std::uint32_t AddEvent(double time, const std::string& name, const std::string& payload = {});
    void EditEvent(std::uint32_t key, double time, const std::string& name, const std::string& payload);

    void SetFrameRate(const FrameRate& rate);
    void SetPlaybackRange(double start, double end);
    void SetWorkRange(double start, double end);

    // ---- Playhead and preview ---------------------------------------------------------
    double Time() const { return mPlayer->GetTime(); }
    // Moves the playhead (clamped to the playback range) and re-evaluates.
    void SetTime(double time);
    SequencePlayer& Player() { return *mPlayer; }
    // Called whenever the document replaces its player (New, Load), so the window can
    // attach its callbacks to the new one.
    std::function<void(SequencePlayer&)> OnPlayerCreated;

    // Advances playback; evaluates whatever an edit left pending.
    void Tick(double deltaSeconds);
    void MarkEvaluationDirty() { mEvaluationDirty = true; }
    void EvaluateNow();

    // See SequencerEditor::SuspendPreview. While suspended nothing is evaluated.
    void SuspendPreview();
    void ResumePreview();
    bool IsPreviewSuspended() const { return mSuspendCount > 0; }
    // Stops previewing: restores the level and drops the record.
    void EndPreview();
    // The world was replaced: forget the record without restoring it.
    void OnWorldReplaced();

    // ---- Auto Key ---------------------------------------------------------------------
    bool AutoKey = false;
    // The world was edited outside the Sequencer. With Auto Key on, every animated
    // property of a bound object that now differs from what the sequence would give - and
    // the transform of a bound entity that has no transform track yet - is keyed at the
    // playhead. Returns true when keys were made.
    bool OnWorldEdited();

private:
    struct ScopeState
    {
        unsigned Flags = 0;
        std::set<Guid> Bindings;
        // Binding order, so removed bindings come back where they were.
        std::vector<Guid> Order;
        std::map<Guid, nlohmann::json> BindingJson;
        nlohmann::json Header;
        nlohmann::json MasterTracks;
    };

    struct Transaction
    {
        std::string Label;
        ScopeState Before;
        ScopeState After;
    };

    ScopeState Capture(unsigned flags, const std::set<Guid>& bindings) const;
    void Apply(const ScopeState& state);
    // Closes the outermost transaction without recording it, for an edit that put
    // everything back itself.
    void Discard();
    bool SameState(const ScopeState& a, const ScopeState& b) const;
    void ResetHistory();
    void AfterDataReplaced();
    void ResetPlayer();

    // Values of every tracked property (and of each bound entity's transform) as the last
    // evaluation left them; Auto Key compares against these.
    void RecordObservedValues();

    struct MoveOrigin
    {
        SequencerKeyHandle Key;
        double Time = 0.0;
    };

    CinematicHost* mHost = nullptr;
    std::shared_ptr<CinematicSequence> mSequence;
    std::unique_ptr<SequencePlayer> mPlayer;
    std::string mPath;
    bool mDirty = false;
    std::uint64_t mRevision = 1;
    std::uint64_t mStructureRevision = 1;

    int mTransactionDepth = 0;
    Transaction mOpen;
    std::vector<Transaction> mUndo;
    std::vector<Transaction> mRedo;

    std::set<SequencerKeyHandle> mSelectedKeys;

    bool mMoving = false;
    SequencerKeyHandle mMoveAnchor;
    std::vector<MoveOrigin> mMoveOrigins;

    Guid mEditCut;
    double mEditCutStart = 0.0;
    double mEditCutEnd = 0.0;
    // The cuts sharing the edited cut's edges when the edit began.
    Guid mEditCutBefore;
    Guid mEditCutAfter;

    bool mEvaluationDirty = true;
    int mSuspendCount = 0;
    std::map<std::pair<Guid, std::string>, PropertyValue> mObserved;
};
