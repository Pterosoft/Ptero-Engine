#pragma once

#include "System/Cinematics/CinematicCurve.h"
#include "System/Guid.h"
#include "System/PropertyRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CinematicSequence;
struct SequenceBinding;

// Where a track's evaluation goes. The SequencePlayer implements this; a track never
// writes to the world itself, so the player can record what every property was before
// the sequence touched it and put it back afterwards.
class SequenceEvaluationSink
{
public:
    virtual ~SequenceEvaluationSink() = default;

    virtual void ApplyProperty(const SequenceBinding& binding, void* owner, const PropertyDescriptor& property,
                               const PropertyValue& value) = 0;

    // The camera binding a cut track selects at the evaluated time; the null Guid when no
    // cut covers it.
    virtual void ApplyCameraCut(const Guid& cameraBinding) = 0;
};

struct TrackEvaluationContext
{
    double Time = 0.0;
    const CinematicSequence* Sequence = nullptr;
    // The binding a track sits under, and the object it resolved to. Both null for a
    // master track (camera cuts, events), which belongs to the sequence itself.
    const SequenceBinding* Binding = nullptr;
    void* Owner = nullptr;
    SequenceEvaluationSink* Sink = nullptr;
};

// A clip on a section-based track: a camera cut, and later an animation, an audio clip or
// a subsequence. Keyframe tracks have no sections - their channels span the whole
// sequence - which keeps the common case free of a layer the animator never sees.
struct SequenceSection
{
    Guid Id = Guid::New();
    double Start = 0.0;
    double End = 1.0;
};

// One row of animation. Track types are registered with SequenceTrackFactory by type id,
// which is what a saved sequence names; a module can add a track type without the
// sequence, the player or the serializer knowing about it.
class SequenceTrack
{
public:
    virtual ~SequenceTrack() = default;

    // The factory id, e.g. "Property". Saved with the track.
    virtual const char* TypeId() const = 0;
    virtual std::unique_ptr<SequenceTrack> Clone() const = 0;

    virtual void Evaluate(const TrackEvaluationContext& context) const = 0;

    // Every key or section boundary time, for next/previous key navigation and for
    // fitting the view. Appended, not cleared.
    virtual void CollectKeyTimes(std::vector<double>& times) const = 0;

    // Track-specific fields. The base fields (type, id, name, muted) are handled by
    // SaveTrack/LoadTrack around these.
    virtual void SaveContents(nlohmann::json& json) const = 0;
    virtual bool LoadContents(const nlohmann::json& json, std::string& error) = 0;

    Guid Id = Guid::New();
    std::string Name;
    // A muted track is kept but not evaluated.
    bool Muted = false;
};

namespace SequenceTrackFactory
{
    using CreateFunction = std::function<std::unique_ptr<SequenceTrack>()>;

    // Replaces an earlier registration of the same id. The built-in types are registered
    // on first use of the factory, so they need no explicit call.
    void Register(const std::string& typeId, CreateFunction create);
    std::unique_ptr<SequenceTrack> Create(const std::string& typeId);

    void SaveTrack(const SequenceTrack& track, nlohmann::json& json);
    // Null with `error` filled for an unknown type or malformed contents; the caller
    // decides whether that fails the whole load.
    std::unique_ptr<SequenceTrack> LoadTrack(const nlohmann::json& json, std::string& error);
}

// Animates one registered property of the bound object: a float, a bool, an int, a
// vector, a colour or a whole transform. The property is named by its registry path, so
// this one class is the float, boolean, integer, vector, colour, light, camera and
// environment track all at once - what differs between them is the registry entry.
class PropertyTrack final : public SequenceTrack
{
public:
    static constexpr const char* kTypeId = "Property";

    PropertyTrack() = default;
    PropertyTrack(std::string propertyPath, PropertyType valueType);

    const char* TypeId() const override { return kTypeId; }
    std::unique_ptr<SequenceTrack> Clone() const override;
    void Evaluate(const TrackEvaluationContext& context) const override;
    void CollectKeyTimes(std::vector<double>& times) const override;
    void SaveContents(nlohmann::json& json) const override;
    bool LoadContents(const nlohmann::json& json, std::string& error) override;

    const std::string& PropertyPath() const { return mPropertyPath; }
    PropertyType ValueType() const { return mValueType; }

    // One channel per component (PropertyChannelCount).
    int ChannelCount() const { return static_cast<int>(mChannels.size()); }
    FloatChannel& Channel(int index) { return mChannels[index]; }
    const FloatChannel& Channel(int index) const { return mChannels[index]; }

    // Writes the animated components of the value at `time` into `value`, leaving every
    // component whose channel has no keys as it was. A transform track with only
    // location keys therefore never fights whoever else sets the rotation.
    void EvaluateInto(double time, PropertyValue& value) const;

    // Keys the channels selected by `channelMask` (bit i = channel i) to `value`.
    void SetKey(double time, const PropertyValue& value, KeyInterpolation interpolation = KeyInterpolation::Auto,
                unsigned channelMask = ~0u);

    // The registry entry in the binding's domain, cached until the registry changes.
    const PropertyDescriptor* ResolveProperty(std::string_view domain) const;

private:
    void ResetChannels();

    std::string mPropertyPath;
    PropertyType mValueType = PropertyType::Float;
    std::vector<FloatChannel> mChannels;

    mutable const PropertyDescriptor* mCachedProperty = nullptr;
    mutable std::size_t mCachedRevision = static_cast<std::size_t>(-1);
    mutable std::string mCachedDomain;
};

struct CameraCutSection : SequenceSection
{
    // The binding whose camera is active over [Start, End).
    Guid CameraBinding;
};

// Which camera the sequence looks through, as a row of blocks. Cuts are hard; the
// section carries everything a blend would need (its neighbours' overlap), so blends can
// be added without changing the file format.
class CameraCutTrack final : public SequenceTrack
{
public:
    static constexpr const char* kTypeId = "CameraCut";

    CameraCutTrack() { Name = "Camera Cuts"; }

    const char* TypeId() const override { return kTypeId; }
    std::unique_ptr<SequenceTrack> Clone() const override;
    void Evaluate(const TrackEvaluationContext& context) const override;
    void CollectKeyTimes(std::vector<double>& times) const override;
    void SaveContents(nlohmann::json& json) const override;
    bool LoadContents(const nlohmann::json& json, std::string& error) override;

    // Kept sorted by Start; call SortSections after editing times directly.
    std::vector<CameraCutSection> Sections;
    void SortSections();

    // The section covering `time`, or null.
    const CameraCutSection* FindSectionAt(double time) const;
};

struct SequenceEventKey
{
    double Time = 0.0;
    // What a graph listens for: "LightOn", "IntroFinished".
    std::string Name;
    // Free text handed to the listener with the event.
    std::string Payload;
    // Editor identity while keys move, like Keyframe::Id. Not saved.
    std::uint32_t Id = 0;
};

// Discrete markers fired as playback crosses them. Evaluation does nothing - an event is
// an instant, not a state - and the player collects them between one time and the next.
class EventTrack final : public SequenceTrack
{
public:
    static constexpr const char* kTypeId = "Event";

    EventTrack() { Name = "Events"; }

    const char* TypeId() const override { return kTypeId; }
    std::unique_ptr<SequenceTrack> Clone() const override;
    void Evaluate(const TrackEvaluationContext&) const override {}
    void CollectKeyTimes(std::vector<double>& times) const override;
    void SaveContents(nlohmann::json& json) const override;
    bool LoadContents(const nlohmann::json& json, std::string& error) override;

    const std::vector<SequenceEventKey>& Keys() const { return mKeys; }
    std::vector<SequenceEventKey>& MutableKeys() { return mKeys; }
    // Re-sorts after keys were edited through MutableKeys.
    void OnKeysChanged();

    std::uint32_t AddKey(double time, std::string name, std::string payload = {});
    bool RemoveKeyById(std::uint32_t id);
    std::uint32_t AssignId() { return mNextKeyId++; }

    // Keys with from < time <= to (forward) in time order; with `includeFrom`, from <= time.
    void CollectEvents(double from, double to, bool includeFrom, std::vector<const SequenceEventKey*>& out) const;

private:
    std::vector<SequenceEventKey> mKeys;
    std::uint32_t mNextKeyId = 1;
};
