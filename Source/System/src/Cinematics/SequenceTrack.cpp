#include "System/Cinematics/SequenceTrack.h"

#include "System/Cinematics/CinematicSequence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

using json = nlohmann::json;

// ---------------------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------------------

namespace
{
    std::map<std::string, SequenceTrackFactory::CreateFunction>& Factories()
    {
        static std::map<std::string, SequenceTrackFactory::CreateFunction> factories = [] {
            std::map<std::string, SequenceTrackFactory::CreateFunction> builtIn;
            builtIn[PropertyTrack::kTypeId] = [] { return std::make_unique<PropertyTrack>(); };
            builtIn[CameraCutTrack::kTypeId] = [] { return std::make_unique<CameraCutTrack>(); };
            builtIn[EventTrack::kTypeId] = [] { return std::make_unique<EventTrack>(); };
            return builtIn;
        }();
        return factories;
    }

    // Keys are written as small objects with every default left out: a linear key is
    // {"t":1,"v":2,"i":"linear"}, which keeps a long curve readable in a diff.
    json SaveKey(const Keyframe& key)
    {
        json j;
        j["t"] = key.Time;
        j["v"] = key.Value;
        if (key.Interpolation != KeyInterpolation::Auto)
            j["i"] = KeyInterpolationId(key.Interpolation);
        if (key.Tangents != TangentMode::Auto)
        {
            j["tm"] = TangentModeId(key.Tangents);
            j["at"] = key.ArriveTangent;
            j["lt"] = key.LeaveTangent;
        }
        if (key.ArriveWeight > 0.0)
            j["aw"] = key.ArriveWeight;
        if (key.LeaveWeight > 0.0)
            j["lw"] = key.LeaveWeight;
        return j;
    }

    Keyframe LoadKey(const json& j)
    {
        Keyframe key;
        key.Time = j.value("t", 0.0);
        key.Value = j.value("v", 0.0);
        key.Interpolation = KeyInterpolationFromId(j.value("i", std::string("auto")));
        key.Tangents = TangentModeFromId(j.value("tm", std::string("auto")));
        key.ArriveTangent = j.value("at", 0.0);
        key.LeaveTangent = j.value("lt", 0.0);
        key.ArriveWeight = j.value("aw", 0.0);
        key.LeaveWeight = j.value("lw", 0.0);
        return key;
    }

    ChannelValueKind ValueKindFor(PropertyType type)
    {
        switch (type)
        {
        case PropertyType::Bool: return ChannelValueKind::Bool;
        case PropertyType::Int: return ChannelValueKind::Int;
        default: return ChannelValueKind::Float;
        }
    }
}

namespace SequenceTrackFactory
{
    void Register(const std::string& typeId, CreateFunction create)
    {
        Factories()[typeId] = std::move(create);
    }

    std::unique_ptr<SequenceTrack> Create(const std::string& typeId)
    {
        const auto it = Factories().find(typeId);
        return it != Factories().end() ? it->second() : nullptr;
    }

    void SaveTrack(const SequenceTrack& track, json& j)
    {
        j = json::object();
        j["Type"] = track.TypeId();
        j["Id"] = track.Id.ToString();
        j["Name"] = track.Name;
        if (track.Muted)
            j["Muted"] = true;
        track.SaveContents(j);
    }

    std::unique_ptr<SequenceTrack> LoadTrack(const json& j, std::string& error)
    {
        if (!j.is_object())
        {
            error = "a track is not a JSON object";
            return nullptr;
        }

        const std::string type = j.value("Type", std::string());
        std::unique_ptr<SequenceTrack> track = Create(type);
        if (!track)
        {
            error = "unknown track type '" + type + "'";
            return nullptr;
        }

        bool idOk = false;
        const Guid id = Guid::Parse(j.value("Id", std::string()), &idOk);
        if (idOk && id.IsValid())
            track->Id = id;
        track->Name = j.value("Name", track->Name);
        track->Muted = j.value("Muted", false);

        if (!track->LoadContents(j, error))
            return nullptr;
        return track;
    }
}

// ---------------------------------------------------------------------------------------
// PropertyTrack
// ---------------------------------------------------------------------------------------

PropertyTrack::PropertyTrack(std::string propertyPath, PropertyType valueType)
    : mPropertyPath(std::move(propertyPath)), mValueType(valueType)
{
    ResetChannels();
}

void PropertyTrack::ResetChannels()
{
    mChannels.assign(static_cast<std::size_t>(PropertyChannelCount(mValueType)), FloatChannel{});
    for (FloatChannel& channel : mChannels)
        channel.SetValueKind(ValueKindFor(mValueType));

    // A transform's unkeyed scale channels read 1, not 0, if anything ever asks them for
    // their default (a curve editor drawing an empty channel, for instance).
    if (mValueType == PropertyType::Transform)
    {
        for (int i = 6; i < 9; ++i)
            mChannels[i].DefaultValue = 1.0;
    }
}

std::unique_ptr<SequenceTrack> PropertyTrack::Clone() const
{
    auto copy = std::make_unique<PropertyTrack>(*this);
    copy->mCachedProperty = nullptr;
    copy->mCachedRevision = static_cast<std::size_t>(-1);
    return copy;
}

const PropertyDescriptor* PropertyTrack::ResolveProperty(std::string_view domain) const
{
    const std::size_t revision = PropertyRegistry::Revision();
    if (mCachedRevision != revision || mCachedDomain != domain)
    {
        mCachedProperty = PropertyRegistry::Find(domain, mPropertyPath);
        mCachedRevision = revision;
        mCachedDomain = std::string(domain);
    }
    return mCachedProperty;
}

void PropertyTrack::EvaluateInto(double time, PropertyValue& value) const
{
    value.Type = mValueType;
    const int count = (std::min)(static_cast<int>(mChannels.size()), PropertyValue::kMaxComponents);
    for (int i = 0; i < count; ++i)
    {
        if (mChannels[i].HasKeys())
            value.Components[i] = mChannels[i].Evaluate(time);
    }
}

void PropertyTrack::Evaluate(const TrackEvaluationContext& context) const
{
    if (context.Binding == nullptr || context.Owner == nullptr || context.Sink == nullptr)
        return;

    bool anyKeys = false;
    for (const FloatChannel& channel : mChannels)
        anyKeys = anyKeys || channel.HasKeys();
    if (!anyKeys)
        return;

    const PropertyDescriptor* property = ResolveProperty(context.Binding->Domain);
    if (property == nullptr || property->Type != mValueType || !property->IsAvailableOn(context.Owner) ||
        !property->Get || !property->Set)
        return;

    // Start from the current value so unkeyed components pass through untouched.
    PropertyValue value;
    if (!property->Get(context.Owner, value))
        return;

    EvaluateInto(context.Time, value);
    context.Sink->ApplyProperty(*context.Binding, context.Owner, *property, value);
}

void PropertyTrack::SetKey(double time, const PropertyValue& value, KeyInterpolation interpolation,
                           unsigned channelMask)
{
    const int count = (std::min)(static_cast<int>(mChannels.size()), PropertyValue::kMaxComponents);
    for (int i = 0; i < count; ++i)
    {
        if ((channelMask & (1u << i)) != 0)
            mChannels[i].SetKey(time, value.Components[i], interpolation);
    }
}

void PropertyTrack::CollectKeyTimes(std::vector<double>& times) const
{
    for (const FloatChannel& channel : mChannels)
    {
        for (const Keyframe& key : channel.Keys())
            times.push_back(key.Time);
    }
}

void PropertyTrack::SaveContents(json& j) const
{
    j["Property"] = mPropertyPath;
    j["ValueType"] = PropertyTypeId(mValueType);

    json channels = json::array();
    for (const FloatChannel& channel : mChannels)
    {
        json c = json::object();
        if (channel.DefaultValue != 0.0)
            c["Default"] = channel.DefaultValue;
        json keys = json::array();
        for (const Keyframe& key : channel.Keys())
            keys.push_back(SaveKey(key));
        c["Keys"] = std::move(keys);
        channels.push_back(std::move(c));
    }
    j["Channels"] = std::move(channels);
}

bool PropertyTrack::LoadContents(const json& j, std::string& error)
{
    mPropertyPath = j.value("Property", std::string());
    if (mPropertyPath.empty())
    {
        error = "a property track names no property";
        return false;
    }

    bool knownType = false;
    mValueType = PropertyTypeFromId(j.value("ValueType", std::string("float")), &knownType);
    if (!knownType)
    {
        error = "property track '" + mPropertyPath + "' has an unknown value type";
        return false;
    }

    ResetChannels();
    const auto channels = j.find("Channels");
    if (channels != j.end() && channels->is_array())
    {
        const std::size_t count = (std::min)(channels->size(), mChannels.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            const json& c = (*channels)[i];
            FloatChannel& channel = mChannels[i];
            channel.DefaultValue = c.value("Default", channel.DefaultValue);
            const auto keys = c.find("Keys");
            if (keys == c.end() || !keys->is_array())
                continue;
            for (const json& key : *keys)
                channel.InsertKey(LoadKey(key));
        }
    }

    mCachedProperty = nullptr;
    mCachedRevision = static_cast<std::size_t>(-1);
    return true;
}

// ---------------------------------------------------------------------------------------
// CameraCutTrack
// ---------------------------------------------------------------------------------------

std::unique_ptr<SequenceTrack> CameraCutTrack::Clone() const
{
    return std::make_unique<CameraCutTrack>(*this);
}

void CameraCutTrack::SortSections()
{
    std::stable_sort(Sections.begin(), Sections.end(),
                     [](const CameraCutSection& a, const CameraCutSection& b) { return a.Start < b.Start; });
}

const CameraCutSection* CameraCutTrack::FindSectionAt(double time) const
{
    // Sorted by start: the candidate is the last section starting at or before `time`.
    // Overlaps are allowed while editing; the later-starting section wins, which is the
    // one the animator dragged over the other.
    const auto it = std::upper_bound(Sections.begin(), Sections.end(), time,
                                     [](double t, const CameraCutSection& section) { return t < section.Start; });
    for (auto candidate = it; candidate != Sections.begin();)
    {
        --candidate;
        if (time < candidate->End)
            return &*candidate;
    }
    return nullptr;
}

void CameraCutTrack::Evaluate(const TrackEvaluationContext& context) const
{
    if (context.Sink == nullptr)
        return;

    const CameraCutSection* section = FindSectionAt(context.Time);
    // The very end of the last cut is still inside it, otherwise a sequence evaluated at
    // its own end time (where it stops) would drop its camera on the last frame.
    if (section == nullptr && !Sections.empty())
    {
        const CameraCutSection* last = nullptr;
        for (const CameraCutSection& candidate : Sections)
        {
            if (last == nullptr || candidate.End >= last->End)
                last = &candidate;
        }
        if (last != nullptr && context.Time == last->End)
            section = last;
    }

    context.Sink->ApplyCameraCut(section != nullptr ? section->CameraBinding : Guid{});
}

void CameraCutTrack::CollectKeyTimes(std::vector<double>& times) const
{
    for (const CameraCutSection& section : Sections)
    {
        times.push_back(section.Start);
        times.push_back(section.End);
    }
}

void CameraCutTrack::SaveContents(json& j) const
{
    json sections = json::array();
    for (const CameraCutSection& section : Sections)
    {
        sections.push_back({ { "Id", section.Id.ToString() },
                             { "Start", section.Start },
                             { "End", section.End },
                             { "Camera", section.CameraBinding.ToString() } });
    }
    j["Sections"] = std::move(sections);
}

bool CameraCutTrack::LoadContents(const json& j, std::string&)
{
    Sections.clear();
    const auto sections = j.find("Sections");
    if (sections != j.end() && sections->is_array())
    {
        for (const json& s : *sections)
        {
            CameraCutSection section;
            bool ok = false;
            const Guid id = Guid::Parse(s.value("Id", std::string()), &ok);
            section.Id = ok && id.IsValid() ? id : Guid::New();
            section.Start = s.value("Start", 0.0);
            section.End = (std::max)(section.Start, s.value("End", section.Start));
            section.CameraBinding = Guid::Parse(s.value("Camera", std::string()));
            Sections.push_back(section);
        }
    }
    SortSections();
    return true;
}

// ---------------------------------------------------------------------------------------
// EventTrack
// ---------------------------------------------------------------------------------------

std::unique_ptr<SequenceTrack> EventTrack::Clone() const
{
    return std::make_unique<EventTrack>(*this);
}

void EventTrack::OnKeysChanged()
{
    std::stable_sort(mKeys.begin(), mKeys.end(),
                     [](const SequenceEventKey& a, const SequenceEventKey& b) { return a.Time < b.Time; });
}

std::uint32_t EventTrack::AddKey(double time, std::string name, std::string payload)
{
    SequenceEventKey key;
    key.Time = time;
    key.Name = std::move(name);
    key.Payload = std::move(payload);
    key.Id = mNextKeyId++;
    const std::uint32_t id = key.Id;
    mKeys.push_back(std::move(key));
    OnKeysChanged();
    return id;
}

bool EventTrack::RemoveKeyById(std::uint32_t id)
{
    const auto it = std::find_if(mKeys.begin(), mKeys.end(), [id](const SequenceEventKey& key) { return key.Id == id; });
    if (it == mKeys.end())
        return false;
    mKeys.erase(it);
    return true;
}

void EventTrack::CollectEvents(double from, double to, bool includeFrom,
                               std::vector<const SequenceEventKey*>& out) const
{
    // Sorted, so the range is found by search rather than by walking every marker.
    const auto begin = includeFrom
        ? std::lower_bound(mKeys.begin(), mKeys.end(), from,
                           [](const SequenceEventKey& key, double t) { return key.Time < t; })
        : std::upper_bound(mKeys.begin(), mKeys.end(), from,
                           [](double t, const SequenceEventKey& key) { return t < key.Time; });
    for (auto it = begin; it != mKeys.end() && it->Time <= to; ++it)
        out.push_back(&*it);
}

void EventTrack::CollectKeyTimes(std::vector<double>& times) const
{
    for (const SequenceEventKey& key : mKeys)
        times.push_back(key.Time);
}

void EventTrack::SaveContents(json& j) const
{
    json keys = json::array();
    for (const SequenceEventKey& key : mKeys)
    {
        json k = { { "Time", key.Time }, { "Name", key.Name } };
        if (!key.Payload.empty())
            k["Payload"] = key.Payload;
        keys.push_back(std::move(k));
    }
    j["Keys"] = std::move(keys);
}

bool EventTrack::LoadContents(const json& j, std::string&)
{
    mKeys.clear();
    const auto keys = j.find("Keys");
    if (keys != j.end() && keys->is_array())
    {
        for (const json& k : *keys)
            AddKey(k.value("Time", 0.0), k.value("Name", std::string()), k.value("Payload", std::string()));
    }
    return true;
}
