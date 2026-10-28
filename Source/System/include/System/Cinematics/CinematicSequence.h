#pragma once

#include "System/Cinematics/SequenceTrack.h"
#include "System/Guid.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The frame rate a sequence is authored and displayed in. Keys are stored in seconds, so
// changing the rate never moves an animation; it changes what "the next frame" and the
// snapping grid mean. Kept as a ratio so 29.97 (30000/1001) is exact.
struct FrameRate
{
    int Numerator = 30;
    int Denominator = 1;

    double FramesPerSecond() const { return Denominator > 0 ? double(Numerator) / double(Denominator) : 30.0; }
    double FrameDuration() const { return 1.0 / FramesPerSecond(); }
    double FrameToSeconds(double frame) const { return frame / FramesPerSecond(); }
    // The frame `seconds` falls in, rounded to the nearest frame boundary.
    std::int64_t SecondsToNearestFrame(double seconds) const;
    std::int64_t SecondsToFloorFrame(double seconds) const;
    double SnapToFrame(double seconds) const { return FrameToSeconds(double(SecondsToNearestFrame(seconds))); }

    friend bool operator==(const FrameRate& a, const FrameRate& b)
    {
        return a.Numerator == b.Numerator && a.Denominator == b.Denominator;
    }
};

// The rates the editor offers. Any other ratio still loads and plays.
const FrameRate* StandardFrameRates(int& count);
std::string FrameRateLabel(const FrameRate& rate);

// The object a set of tracks animates.
//
// A binding never holds a pointer. It names its object the way the level does - by the
// entity's persistent id - plus the name the entity had when it was bound, which is what
// finds it again in a level where that id does not exist (a sequence reused in another
// level, or an entity that was deleted and rebuilt). The host resolves it each time the
// sequence is evaluated.
struct SequenceBinding
{
    Guid Id = Guid::New();
    // Shown in the track list; defaults to the entity's name when bound.
    std::string Name;
    // The PropertyRegistry domain of the bound object: "Entity", or "Environment" for the
    // engine-wide settings, which need no id.
    std::string Domain = "Entity";
    std::uint64_t EntityId = 0;
    std::string EntityName;

    std::vector<std::unique_ptr<SequenceTrack>> Tracks;

    SequenceBinding() = default;
    SequenceBinding(const SequenceBinding& other);
    SequenceBinding& operator=(const SequenceBinding& other);
    SequenceBinding(SequenceBinding&&) noexcept = default;
    SequenceBinding& operator=(SequenceBinding&&) noexcept = default;

    SequenceTrack* FindTrack(const Guid& trackId) const;
    // The property track animating `propertyPath`, or null.
    PropertyTrack* FindPropertyTrack(const std::string& propertyPath) const;

    nlohmann::json ToJson() const;
    bool FromJson(const nlohmann::json& json, std::string& error);
};

// A cinematic: bindings with their tracks, plus the master tracks that belong to the
// sequence as a whole (camera cuts, events).
//
// Plain data with value semantics - copying deep-copies every track - so the editor can
// hand the runtime a snapshot, and undo can restore part of one.
class CinematicSequence
{
public:
    static constexpr const char* kFileExtension = ".sequence";

    Guid Id = Guid::New();
    std::string Name = "Sequence";
    FrameRate DisplayRate;
    // What plays: Play starts at PlaybackStart and finishes (or loops) at PlaybackEnd.
    double PlaybackStart = 0.0;
    double PlaybackEnd = 5.0;
    // The span the editor's timeline frames by default. Saved so a sequence reopens where
    // it was being worked on; the runtime ignores it.
    double WorkStart = -0.5;
    double WorkEnd = 5.5;
    // Free-form key/value pairs - author, notes, the level it was made for.
    std::map<std::string, std::string> Metadata;

    std::vector<SequenceBinding> Bindings;
    std::vector<std::unique_ptr<SequenceTrack>> MasterTracks;

    CinematicSequence() = default;
    CinematicSequence(const CinematicSequence& other);
    CinematicSequence& operator=(const CinematicSequence& other);
    CinematicSequence(CinematicSequence&&) noexcept = default;
    CinematicSequence& operator=(CinematicSequence&&) noexcept = default;

    double Duration() const { return PlaybackEnd - PlaybackStart; }

    SequenceBinding* FindBinding(const Guid& bindingId);
    const SequenceBinding* FindBinding(const Guid& bindingId) const;
    SequenceBinding* FindBindingForEntity(std::uint64_t entityId);

    // The first master track of a type, optionally creating it.
    CameraCutTrack* FindCameraCutTrack() const;
    CameraCutTrack& GetOrAddCameraCutTrack();
    EventTrack* FindEventTrack() const;
    EventTrack& GetOrAddEventTrack();

    // Earliest and latest key or section time across every track; false when empty.
    bool GetKeyTimeRange(double& first, double& last) const;

    nlohmann::json ToJson() const;
    // Replaces this sequence. On failure the sequence is left unchanged.
    bool FromJson(const nlohmann::json& json, std::string& error);

    std::string ToJsonString() const;
    bool FromJsonString(const std::string& text, std::string& error);

    // Read through DataFiles, so a packaged game loads sequences out of its archives.
    bool LoadFromFile(const std::string& filePath, std::string& error);
    // Written indented, so a .sequence stays reviewable in a diff.
    bool SaveToFile(const std::string& filePath, std::string& error) const;
};
