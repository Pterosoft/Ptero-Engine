#include "System/Cinematics/CinematicSequence.h"

#include "System/DataFiles.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

namespace
{
    // Bumped only for a change an older reader cannot cope with. Unknown track types are
    // reported and skipped, so adding a track type does not need a new version.
    constexpr int kSequenceVersion = 1;
    constexpr const char* kVersionKey = "PteroSequence";

    constexpr FrameRate kStandardRates[] = {
        { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 48, 1 }, { 50, 1 }, { 60, 1 }, { 120, 1 },
    };

    std::vector<std::unique_ptr<SequenceTrack>> CloneTracks(const std::vector<std::unique_ptr<SequenceTrack>>& tracks)
    {
        std::vector<std::unique_ptr<SequenceTrack>> copy;
        copy.reserve(tracks.size());
        for (const std::unique_ptr<SequenceTrack>& track : tracks)
            copy.push_back(track->Clone());
        return copy;
    }

    json SaveTracks(const std::vector<std::unique_ptr<SequenceTrack>>& tracks)
    {
        json array = json::array();
        for (const std::unique_ptr<SequenceTrack>& track : tracks)
        {
            json j;
            SequenceTrackFactory::SaveTrack(*track, j);
            array.push_back(std::move(j));
        }
        return array;
    }

    // A track that cannot be read is dropped with a warning appended to `warnings` rather
    // than failing the sequence: one track from a newer build should not make the whole
    // cinematic unopenable.
    void LoadTracks(const json& array, std::vector<std::unique_ptr<SequenceTrack>>& tracks, std::string& warnings)
    {
        tracks.clear();
        if (!array.is_array())
            return;
        for (const json& j : array)
        {
            std::string error;
            std::unique_ptr<SequenceTrack> track = SequenceTrackFactory::LoadTrack(j, error);
            if (track)
                tracks.push_back(std::move(track));
            else
                warnings += (warnings.empty() ? "" : "; ") + error;
        }
    }
}

std::int64_t FrameRate::SecondsToNearestFrame(double seconds) const
{
    return static_cast<std::int64_t>(std::llround(seconds * FramesPerSecond()));
}

std::int64_t FrameRate::SecondsToFloorFrame(double seconds) const
{
    // A hair of tolerance, so 1.0 s at 30 fps lands on frame 30 rather than 29 when the
    // seconds value carries rounding from an earlier conversion.
    return static_cast<std::int64_t>(std::floor(seconds * FramesPerSecond() + 1e-6));
}

const FrameRate* StandardFrameRates(int& count)
{
    count = static_cast<int>(sizeof(kStandardRates) / sizeof(kStandardRates[0]));
    return kStandardRates;
}

std::string FrameRateLabel(const FrameRate& rate)
{
    char buffer[32];
    if (rate.Denominator == 1)
        std::snprintf(buffer, sizeof(buffer), "%d fps", rate.Numerator);
    else
        std::snprintf(buffer, sizeof(buffer), "%.3g fps", rate.FramesPerSecond());
    return buffer;
}

// ---------------------------------------------------------------------------------------
// SequenceBinding
// ---------------------------------------------------------------------------------------

SequenceBinding::SequenceBinding(const SequenceBinding& other)
    : Id(other.Id), Name(other.Name), Domain(other.Domain), EntityId(other.EntityId), EntityName(other.EntityName),
      Tracks(CloneTracks(other.Tracks))
{
}

SequenceBinding& SequenceBinding::operator=(const SequenceBinding& other)
{
    if (this != &other)
    {
        Id = other.Id;
        Name = other.Name;
        Domain = other.Domain;
        EntityId = other.EntityId;
        EntityName = other.EntityName;
        Tracks = CloneTracks(other.Tracks);
    }
    return *this;
}

SequenceTrack* SequenceBinding::FindTrack(const Guid& trackId) const
{
    for (const std::unique_ptr<SequenceTrack>& track : Tracks)
    {
        if (track->Id == trackId)
            return track.get();
    }
    return nullptr;
}

PropertyTrack* SequenceBinding::FindPropertyTrack(const std::string& propertyPath) const
{
    for (const std::unique_ptr<SequenceTrack>& track : Tracks)
    {
        if (auto* property = dynamic_cast<PropertyTrack*>(track.get()); property && property->PropertyPath() == propertyPath)
            return property;
    }
    return nullptr;
}

json SequenceBinding::ToJson() const
{
    json j;
    j["Id"] = Id.ToString();
    j["Name"] = Name;
    j["Domain"] = Domain;
    if (EntityId != 0)
        j["EntityId"] = EntityId;
    if (!EntityName.empty())
        j["EntityName"] = EntityName;
    j["Tracks"] = SaveTracks(Tracks);
    return j;
}

bool SequenceBinding::FromJson(const json& j, std::string& error)
{
    if (!j.is_object())
    {
        error = "a binding is not a JSON object";
        return false;
    }

    bool ok = false;
    const Guid id = Guid::Parse(j.value("Id", std::string()), &ok);
    Id = ok && id.IsValid() ? id : Guid::New();
    Name = j.value("Name", std::string());
    Domain = j.value("Domain", std::string("Entity"));
    EntityId = j.value("EntityId", std::uint64_t(0));
    EntityName = j.value("EntityName", std::string());

    std::string warnings;
    const auto tracks = j.find("Tracks");
    if (tracks != j.end())
        LoadTracks(*tracks, Tracks, warnings);
    if (!warnings.empty())
        error = "binding '" + Name + "': " + warnings;
    return true;
}

// ---------------------------------------------------------------------------------------
// CinematicSequence
// ---------------------------------------------------------------------------------------

CinematicSequence::CinematicSequence(const CinematicSequence& other)
    : Id(other.Id), Name(other.Name), DisplayRate(other.DisplayRate), PlaybackStart(other.PlaybackStart),
      PlaybackEnd(other.PlaybackEnd), WorkStart(other.WorkStart), WorkEnd(other.WorkEnd), Metadata(other.Metadata),
      Bindings(other.Bindings), MasterTracks(CloneTracks(other.MasterTracks))
{
}

CinematicSequence& CinematicSequence::operator=(const CinematicSequence& other)
{
    if (this != &other)
    {
        CinematicSequence copy(other);
        *this = std::move(copy);
    }
    return *this;
}

SequenceBinding* CinematicSequence::FindBinding(const Guid& bindingId)
{
    for (SequenceBinding& binding : Bindings)
    {
        if (binding.Id == bindingId)
            return &binding;
    }
    return nullptr;
}

const SequenceBinding* CinematicSequence::FindBinding(const Guid& bindingId) const
{
    return const_cast<CinematicSequence*>(this)->FindBinding(bindingId);
}

SequenceBinding* CinematicSequence::FindBindingForEntity(std::uint64_t entityId)
{
    if (entityId == 0)
        return nullptr;
    for (SequenceBinding& binding : Bindings)
    {
        if (binding.Domain == "Entity" && binding.EntityId == entityId)
            return &binding;
    }
    return nullptr;
}

CameraCutTrack* CinematicSequence::FindCameraCutTrack() const
{
    for (const std::unique_ptr<SequenceTrack>& track : MasterTracks)
    {
        if (auto* cuts = dynamic_cast<CameraCutTrack*>(track.get()))
            return cuts;
    }
    return nullptr;
}

CameraCutTrack& CinematicSequence::GetOrAddCameraCutTrack()
{
    if (CameraCutTrack* cuts = FindCameraCutTrack())
        return *cuts;
    auto track = std::make_unique<CameraCutTrack>();
    CameraCutTrack& result = *track;
    MasterTracks.push_back(std::move(track));
    return result;
}

EventTrack* CinematicSequence::FindEventTrack() const
{
    for (const std::unique_ptr<SequenceTrack>& track : MasterTracks)
    {
        if (auto* events = dynamic_cast<EventTrack*>(track.get()))
            return events;
    }
    return nullptr;
}

EventTrack& CinematicSequence::GetOrAddEventTrack()
{
    if (EventTrack* events = FindEventTrack())
        return *events;
    auto track = std::make_unique<EventTrack>();
    EventTrack& result = *track;
    MasterTracks.push_back(std::move(track));
    return result;
}

bool CinematicSequence::GetKeyTimeRange(double& first, double& last) const
{
    std::vector<double> times;
    for (const SequenceBinding& binding : Bindings)
    {
        for (const std::unique_ptr<SequenceTrack>& track : binding.Tracks)
            track->CollectKeyTimes(times);
    }
    for (const std::unique_ptr<SequenceTrack>& track : MasterTracks)
        track->CollectKeyTimes(times);

    if (times.empty())
        return false;
    const auto [low, high] = std::minmax_element(times.begin(), times.end());
    first = *low;
    last = *high;
    return true;
}

json CinematicSequence::ToJson() const
{
    json j;
    j[kVersionKey] = kSequenceVersion;
    j["Id"] = Id.ToString();
    j["Name"] = Name;
    j["FrameRate"] = { DisplayRate.Numerator, DisplayRate.Denominator };
    j["PlaybackRange"] = { PlaybackStart, PlaybackEnd };
    j["WorkRange"] = { WorkStart, WorkEnd };
    j["Metadata"] = Metadata;

    json bindings = json::array();
    for (const SequenceBinding& binding : Bindings)
        bindings.push_back(binding.ToJson());
    j["Bindings"] = std::move(bindings);
    j["Tracks"] = SaveTracks(MasterTracks);
    return j;
}

bool CinematicSequence::FromJson(const json& j, std::string& error)
{
    if (!j.is_object() || !j.contains(kVersionKey))
    {
        error = "not a Ptero sequence";
        return false;
    }

    const int version = j.value(kVersionKey, 0);
    if (version > kSequenceVersion)
    {
        error = "written by a newer version of the engine (sequence format " + std::to_string(version) + ")";
        return false;
    }

    try
    {
        CinematicSequence loaded;
        bool ok = false;
        const Guid id = Guid::Parse(j.value("Id", std::string()), &ok);
        loaded.Id = ok && id.IsValid() ? id : Guid::New();
        loaded.Name = j.value("Name", std::string("Sequence"));

        if (const auto rate = j.find("FrameRate"); rate != j.end() && rate->is_array() && rate->size() == 2)
        {
            loaded.DisplayRate.Numerator = (std::max)(1, (*rate)[0].get<int>());
            loaded.DisplayRate.Denominator = (std::max)(1, (*rate)[1].get<int>());
        }
        if (const auto range = j.find("PlaybackRange"); range != j.end() && range->is_array() && range->size() == 2)
        {
            loaded.PlaybackStart = (*range)[0].get<double>();
            loaded.PlaybackEnd = (std::max)(loaded.PlaybackStart, (*range)[1].get<double>());
        }
        if (const auto range = j.find("WorkRange"); range != j.end() && range->is_array() && range->size() == 2)
        {
            loaded.WorkStart = (*range)[0].get<double>();
            loaded.WorkEnd = (*range)[1].get<double>();
            if (loaded.WorkEnd <= loaded.WorkStart)
                loaded.WorkEnd = loaded.WorkStart + 1.0;
        }
        if (const auto metadata = j.find("Metadata"); metadata != j.end() && metadata->is_object())
        {
            for (auto it = metadata->begin(); it != metadata->end(); ++it)
            {
                if (it.value().is_string())
                    loaded.Metadata[it.key()] = it.value().get<std::string>();
            }
        }

        std::string warnings;
        if (const auto bindings = j.find("Bindings"); bindings != j.end() && bindings->is_array())
        {
            for (const json& b : *bindings)
            {
                SequenceBinding binding;
                std::string bindingWarning;
                if (binding.FromJson(b, bindingWarning))
                    loaded.Bindings.push_back(std::move(binding));
                if (!bindingWarning.empty())
                    warnings += (warnings.empty() ? "" : "; ") + bindingWarning;
            }
        }
        if (const auto tracks = j.find("Tracks"); tracks != j.end())
            LoadTracks(*tracks, loaded.MasterTracks, warnings);

        *this = std::move(loaded);
        // Partial success: the sequence loaded, but the caller should report what was
        // dropped. Callers treat a true return with a non-empty error as warnings.
        error = warnings;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("malformed sequence: ") + exception.what();
        return false;
    }
}

std::string CinematicSequence::ToJsonString() const
{
    return ToJson().dump(2);
}

bool CinematicSequence::FromJsonString(const std::string& text, std::string& error)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded())
    {
        error = "not valid JSON";
        return false;
    }
    return FromJson(j, error);
}

bool CinematicSequence::LoadFromFile(const std::string& filePath, std::string& error)
{
    std::string text;
    if (!DataFiles::ReadText(filePath, text))
    {
        error = "cannot read '" + filePath + "'";
        return false;
    }
    return FromJsonString(text, error);
}

bool CinematicSequence::SaveToFile(const std::string& filePath, std::string& error) const
{
    // Written beside the target and moved over it, so a crash mid-write cannot leave a
    // half-written sequence where the good one was.
    const std::string temporaryPath = filePath + ".tmp";
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "cannot write '" + filePath + "'";
            return false;
        }
        file << ToJsonString() << '\n';
        if (!file)
        {
            error = "writing '" + filePath + "' failed";
            return false;
        }
    }

    std::error_code code;
    std::filesystem::rename(temporaryPath, filePath, code);
    if (code)
    {
        std::filesystem::remove(temporaryPath, code);
        error = "cannot replace '" + filePath + "'";
        return false;
    }
    return true;
}
