// Headless tests for the cinematic runtime (System/Cinematics) and the property registry.
//
// Built straight from the runtime sources with no engine, no renderer and no Qt - the
// point of the runtime's CinematicHost seam. Run Build.cmd in this folder; it compiles
// and runs the tests, and exits non-zero when any check fails.

#include "System/Cinematics/CinematicCurve.h"
#include "System/Cinematics/CinematicSequence.h"
#include "System/Cinematics/SequencePlayer.h"
#include "System/Guid.h"
#include "System/PropertyRegistry.h"

#include "SequencerDocument.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
    int gChecks = 0;
    int gFailures = 0;

#define CHECK(condition)                                                                                  \
    do                                                                                                    \
    {                                                                                                     \
        ++gChecks;                                                                                        \
        if (!(condition))                                                                                 \
        {                                                                                                 \
            ++gFailures;                                                                                  \
            std::printf("  FAILED %s:%d  %s\n", __FILE__, __LINE__, #condition);                          \
        }                                                                                                 \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                                           \
    do                                                                                                    \
    {                                                                                                     \
        ++gChecks;                                                                                        \
        const double a_ = (actual), e_ = (expected);                                                      \
        if (!(std::fabs(a_ - e_) <= (tolerance)))                                                         \
        {                                                                                                 \
            ++gFailures;                                                                                  \
            std::printf("  FAILED %s:%d  %s = %.6f, expected %.6f\n", __FILE__, __LINE__, #actual, a_, e_); \
        }                                                                                                 \
    } while (0)

    // -----------------------------------------------------------------------------------
    // A stand-in world: the runtime only ever sees it through the registry and the host.
    // -----------------------------------------------------------------------------------

    struct FakeObject
    {
        std::uint64_t Id = 0;
        std::string Name;
        double Location[3]{};
        double RotationDegrees[3]{};
        double Scale[3]{ 1, 1, 1 };
        bool HasLight = false;
        double Intensity = 0.0;
        double Color[4]{ 1, 1, 1, 1 };
        bool Visible = true;
        int Count = 0;
        bool IsCamera = false;
    };

    void RegisterFakeProperties()
    {
        PropertyDescriptor transform;
        transform.Domain = "Entity";
        transform.Path = "Transform";
        transform.Group = "Transform";
        transform.DisplayName = "Transform";
        transform.Type = PropertyType::Transform;
        transform.Get = [](const void* owner, PropertyValue& value) {
            const auto* o = static_cast<const FakeObject*>(owner);
            value = PropertyValue::MakeTransform();
            for (int i = 0; i < 3; ++i)
            {
                value.Components[i] = o->Location[i];
                value.Components[3 + i] = o->RotationDegrees[i];
                value.Components[6 + i] = o->Scale[i];
            }
            return true;
        };
        transform.Set = [](void* owner, const PropertyValue& value) {
            auto* o = static_cast<FakeObject*>(owner);
            for (int i = 0; i < 3; ++i)
            {
                o->Location[i] = value.Components[i];
                o->RotationDegrees[i] = value.Components[3 + i];
                o->Scale[i] = value.Components[6 + i];
            }
            return true;
        };
        PropertyRegistry::Register(transform);

        PropertyDescriptor intensity;
        intensity.Domain = "Entity";
        intensity.Path = "Light.Intensity";
        intensity.Group = "Light";
        intensity.DisplayName = "Intensity";
        intensity.Aliases = { "Light Intensity" };
        intensity.Type = PropertyType::Float;
        intensity.Has = [](const void* owner) { return static_cast<const FakeObject*>(owner)->HasLight; };
        intensity.Get = [](const void* owner, PropertyValue& value) {
            value = PropertyValue::MakeFloat(static_cast<const FakeObject*>(owner)->Intensity);
            return true;
        };
        intensity.Set = [](void* owner, const PropertyValue& value) {
            static_cast<FakeObject*>(owner)->Intensity = value.AsFloat();
            return true;
        };
        PropertyRegistry::Register(intensity);

        PropertyDescriptor color;
        color.Domain = "Entity";
        color.Path = "Light.Color";
        color.Group = "Light";
        color.DisplayName = "Color";
        color.Type = PropertyType::Color;
        color.Has = intensity.Has;
        color.Get = [](const void* owner, PropertyValue& value) {
            const auto* o = static_cast<const FakeObject*>(owner);
            value = PropertyValue::MakeColor(o->Color[0], o->Color[1], o->Color[2], o->Color[3]);
            return true;
        };
        color.Set = [](void* owner, const PropertyValue& value) {
            auto* o = static_cast<FakeObject*>(owner);
            for (int i = 0; i < 4; ++i)
                o->Color[i] = value.Components[i];
            return true;
        };
        PropertyRegistry::Register(color);

        PropertyDescriptor visible;
        visible.Domain = "Entity";
        visible.Path = "Visible";
        visible.DisplayName = "Visible";
        visible.Type = PropertyType::Bool;
        visible.Get = [](const void* owner, PropertyValue& value) {
            value = PropertyValue::MakeBool(static_cast<const FakeObject*>(owner)->Visible);
            return true;
        };
        visible.Set = [](void* owner, const PropertyValue& value) {
            static_cast<FakeObject*>(owner)->Visible = value.AsBool();
            return true;
        };
        PropertyRegistry::Register(visible);

        PropertyDescriptor count;
        count.Domain = "Entity";
        count.Path = "Count";
        count.DisplayName = "Count";
        count.Type = PropertyType::Int;
        count.Get = [](const void* owner, PropertyValue& value) {
            value = PropertyValue::MakeInt(static_cast<const FakeObject*>(owner)->Count);
            return true;
        };
        count.Set = [](void* owner, const PropertyValue& value) {
            static_cast<FakeObject*>(owner)->Count = value.AsInt();
            return true;
        };
        PropertyRegistry::Register(count);
    }

    class FakeHost final : public CinematicHost
    {
    public:
        std::vector<FakeObject> Objects;
        // What SetCutCamera last received.
        std::string CutCameraName;
        int CutCameraCalls = 0;
        int ResolveCalls = 0;

        FakeObject& Add(std::uint64_t id, const std::string& name)
        {
            FakeObject object;
            object.Id = id;
            object.Name = name;
            Objects.push_back(object);
            return Objects.back();
        }

        FakeObject* Find(const std::string& name)
        {
            for (FakeObject& object : Objects)
            {
                if (object.Name == name)
                    return &object;
            }
            return nullptr;
        }

        void* ResolveBinding(const SequenceBinding& binding) override
        {
            ++ResolveCalls;
            for (FakeObject& object : Objects)
            {
                if (binding.EntityId != 0 && object.Id == binding.EntityId)
                    return &object;
            }
            // The name fallback a real host uses for a sequence reused in another level.
            for (FakeObject& object : Objects)
            {
                if (!binding.EntityName.empty() && object.Name == binding.EntityName)
                    return &object;
            }
            return nullptr;
        }

        void SetCutCamera(const SequenceBinding*, void* owner, double) override
        {
            ++CutCameraCalls;
            CutCameraName = owner != nullptr ? static_cast<FakeObject*>(owner)->Name : std::string();
        }
    };

    SequenceBinding& Bind(CinematicSequence& sequence, const FakeObject& object)
    {
        SequenceBinding binding;
        binding.Name = object.Name;
        binding.EntityId = object.Id;
        binding.EntityName = object.Name;
        sequence.Bindings.push_back(std::move(binding));
        return sequence.Bindings.back();
    }

    template <class Track>
    Track& AddTrack(SequenceBinding& binding, std::unique_ptr<Track> track)
    {
        Track& result = *track;
        binding.Tracks.push_back(std::move(track));
        return result;
    }

    // -----------------------------------------------------------------------------------

    void TestGuid()
    {
        std::printf("Guid\n");
        std::set<std::string> seen;
        for (int i = 0; i < 1000; ++i)
            seen.insert(Guid::New().ToString());
        CHECK(seen.size() == 1000);

        const Guid guid = Guid::New();
        bool ok = false;
        const Guid parsed = Guid::Parse(guid.ToString(), &ok);
        CHECK(ok);
        CHECK(parsed == guid);
        CHECK(Guid::Parse("{" + guid.ToString() + "}") == guid);
        Guid::Parse("not-a-guid", &ok);
        CHECK(!ok);
        CHECK(guid.ToString()[14] == '4'); // version 4
    }

    void TestBasicInterpolation()
    {
        std::printf("Interpolation modes\n");
        FloatChannel channel;
        channel.SetKey(0.0, 0.0, KeyInterpolation::Linear);
        channel.SetKey(2.0, 10.0, KeyInterpolation::Linear);
        CHECK_NEAR(channel.Evaluate(1.0), 5.0, 1e-9);
        CHECK_NEAR(channel.Evaluate(-5.0), 0.0, 1e-9); // holds before
        CHECK_NEAR(channel.Evaluate(9.0), 10.0, 1e-9); // holds after

        FloatChannel constant;
        constant.SetKey(0.0, 1.0, KeyInterpolation::Constant);
        constant.SetKey(1.0, 2.0, KeyInterpolation::Constant);
        CHECK_NEAR(constant.Evaluate(0.999), 1.0, 1e-9);
        CHECK_NEAR(constant.Evaluate(1.0), 2.0, 1e-9);

        const KeyInterpolation eases[] = { KeyInterpolation::EaseIn, KeyInterpolation::EaseOut, KeyInterpolation::EaseInOut };
        for (KeyInterpolation ease : eases)
        {
            FloatChannel eased;
            eased.SetKey(0.0, 0.0, ease);
            eased.SetKey(1.0, 1.0, ease);
            CHECK_NEAR(eased.Evaluate(0.0), 0.0, 1e-9);
            CHECK_NEAR(eased.Evaluate(1.0), 1.0, 1e-9);
            const double quarter = eased.Evaluate(0.25);
            if (ease == KeyInterpolation::EaseIn)
                CHECK(quarter < 0.25);
            if (ease == KeyInterpolation::EaseOut)
                CHECK(quarter > 0.25);
            if (ease == KeyInterpolation::EaseInOut)
                CHECK_NEAR(eased.Evaluate(0.5), 0.5, 1e-9);
        }

        // Cubic with user tangents is a Hermite spline: slope 0 at both ends of 0 -> 1 over
        // one second gives smoothstep, 0.5 at the middle and 0.15625 at a quarter.
        FloatChannel cubic;
        cubic.SetKey(0.0, 0.0, KeyInterpolation::Cubic);
        cubic.SetKey(1.0, 1.0, KeyInterpolation::Cubic);
        for (const Keyframe& key : cubic.Keys())
        {
            Keyframe* k = cubic.MutableKeyById(key.Id);
            k->Tangents = TangentMode::Unified;
            k->ArriveTangent = 0.0;
            k->LeaveTangent = 0.0;
        }
        cubic.OnKeysChanged();
        CHECK_NEAR(cubic.Evaluate(0.5), 0.5, 1e-9);
        CHECK_NEAR(cubic.Evaluate(0.25), 0.15625, 1e-9);

        // A Bezier segment with default (third) weights is the same curve as a cubic one.
        FloatChannel bezier;
        bezier.SetKey(0.0, 0.0, KeyInterpolation::Bezier);
        bezier.SetKey(1.0, 1.0, KeyInterpolation::Bezier);
        Keyframe* first = bezier.MutableKeyById(bezier.Keys()[0].Id);
        first->Tangents = TangentMode::Broken;
        first->LeaveTangent = 2.0;
        Keyframe* second = bezier.MutableKeyById(bezier.Keys()[1].Id);
        second->Tangents = TangentMode::Broken;
        second->ArriveTangent = -1.0;
        bezier.OnKeysChanged();

        FloatChannel hermite;
        hermite.SetKey(0.0, 0.0, KeyInterpolation::Cubic);
        hermite.SetKey(1.0, 1.0, KeyInterpolation::Cubic);
        Keyframe* h0 = hermite.MutableKeyById(hermite.Keys()[0].Id);
        h0->Tangents = TangentMode::Broken;
        h0->LeaveTangent = 2.0;
        Keyframe* h1 = hermite.MutableKeyById(hermite.Keys()[1].Id);
        h1->Tangents = TangentMode::Broken;
        h1->ArriveTangent = -1.0;
        hermite.OnKeysChanged();
        for (double t = 0.0; t <= 1.0; t += 0.05)
            CHECK_NEAR(bezier.Evaluate(t), hermite.Evaluate(t), 1e-6);

        // Long weights pull the curve along the handle: further along than a plain cubic.
        first = bezier.MutableKeyById(bezier.Keys()[0].Id);
        first->LeaveWeight = 0.9;
        bezier.OnKeysChanged();
        CHECK(bezier.Evaluate(0.3) > hermite.Evaluate(0.3));
        CHECK_NEAR(bezier.Evaluate(1.0), 1.0, 1e-9);
    }

    void TestAutoTangents()
    {
        std::printf("Auto tangents\n");
        // Monotonic keys: an auto curve must not overshoot between them.
        FloatChannel channel;
        channel.SetKey(0.0, 0.0);
        channel.SetKey(1.0, 1.0);
        channel.SetKey(1.2, 5.0);
        channel.SetKey(3.0, 5.5);
        double previous = -1.0;
        bool monotonic = true;
        for (double t = 0.0; t <= 3.0; t += 0.001)
        {
            const double value = channel.Evaluate(t);
            monotonic = monotonic && value >= previous - 1e-9;
            previous = value;
        }
        CHECK(monotonic);

        // A local extremum is flat, so the curve never exceeds the keyed peak.
        FloatChannel peak;
        peak.SetKey(0.0, 0.0);
        peak.SetKey(1.0, 1.0);
        peak.SetKey(2.0, 0.0);
        double maximum = 0.0;
        for (double t = 0.0; t <= 2.0; t += 0.001)
            maximum = (std::max)(maximum, peak.Evaluate(t));
        CHECK_NEAR(maximum, 1.0, 1e-9);
        CHECK_NEAR(peak.Keys()[1].LeaveTangent, 0.0, 1e-12);
    }

    void TestValueKinds()
    {
        std::printf("Bool and Int channels\n");
        FloatChannel flag;
        flag.SetValueKind(ChannelValueKind::Bool);
        flag.SetKey(0.0, 0.0, KeyInterpolation::Linear); // forced to Constant
        flag.SetKey(1.0, 1.0);
        CHECK(flag.Keys()[0].Interpolation == KeyInterpolation::Constant);
        CHECK_NEAR(flag.Evaluate(0.9), 0.0, 0.0);
        CHECK_NEAR(flag.Evaluate(1.0), 1.0, 0.0);

        FloatChannel integer;
        integer.SetValueKind(ChannelValueKind::Int);
        integer.SetKey(0.0, 0.0, KeyInterpolation::Linear);
        integer.SetKey(1.0, 10.0, KeyInterpolation::Linear);
        CHECK_NEAR(integer.Evaluate(0.44), 4.0, 0.0);
        CHECK_NEAR(integer.EvaluateRaw(0.44), 4.4, 1e-9);
    }

    // Reference evaluation by linear scan, to check the cached binary search against.
    double ReferenceSegmentStart(const FloatChannel& channel, double time)
    {
        const auto& keys = channel.Keys();
        int segment = 0;
        for (int i = 0; i + 1 < static_cast<int>(keys.size()); ++i)
        {
            if (keys[i].Time <= time)
                segment = i;
        }
        return keys[segment].Time;
    }

    void TestSegmentSearch()
    {
        std::printf("Key lookup (hinted binary search)\n");
        std::mt19937 random(1234);
        std::uniform_real_distribution<double> value(-10.0, 10.0);

        FloatChannel channel;
        for (int i = 0; i < 5000; ++i)
            channel.SetKey(i * 0.01 + (i % 7) * 0.001, value(random), KeyInterpolation::Linear);

        // Compare against a copy evaluated in a fresh, randomly ordered way so the hints
        // of the two diverge: sequential playback on one, random jumps on the other.
        FloatChannel jumps = channel;
        std::uniform_real_distribution<double> anyTime(-1.0, 51.0);
        bool sequentialMatches = true;
        bool randomMatches = true;
        for (double t = -0.5; t < 51.0; t += 0.0037)
        {
            const double a = channel.Evaluate(t);
            const double b = jumps.Evaluate(anyTime(random));
            (void)b;
            const double c = jumps.Evaluate(t);
            sequentialMatches = sequentialMatches && std::fabs(a - c) < 1e-12;
        }
        for (int i = 0; i < 20000; ++i)
        {
            const double t = anyTime(random);
            // Linear interpolation between the reference segment's keys.
            const auto& keys = channel.Keys();
            double expected = 0.0;
            if (t <= keys.front().Time)
                expected = keys.front().Value;
            else if (t >= keys.back().Time)
                expected = keys.back().Value;
            else
            {
                const double start = ReferenceSegmentStart(channel, t);
                int index = channel.FindKeyIndexAt(start, 1e-12);
                const Keyframe& a = keys[index];
                const Keyframe& b = keys[index + 1];
                expected = a.Value + (b.Value - a.Value) * (t - a.Time) / (b.Time - a.Time);
            }
            randomMatches = randomMatches && std::fabs(channel.Evaluate(t) - expected) < 1e-9;
            if (i > 300)
                break; // the reference is O(n); a few hundred random probes are plenty
        }
        CHECK(sequentialMatches);
        CHECK(randomMatches);

        // Throughput: 1M sequential evaluations over 5 000 keys.
        const auto begin = std::chrono::steady_clock::now();
        double sink = 0.0;
        for (int i = 0; i < 1000000; ++i)
            sink += channel.Evaluate(i * 0.00005);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::printf("    1M evaluations over 5000 keys: %.1f ms (checksum %.3f)\n", ms, sink);
        CHECK(ms < 2000.0);
    }

    void TestRegistry()
    {
        std::printf("Property registry\n");
        CHECK(PropertyRegistry::Find("Entity", "Light.Intensity") != nullptr);
        CHECK(PropertyRegistry::Find("Entity", "Light Intensity") == PropertyRegistry::Find("Entity", "Light.Intensity"));
        CHECK(PropertyRegistry::Find("Environment", "Light.Intensity") == nullptr);
        CHECK(PropertyRegistry::Find("Entity", "Nope") == nullptr);

        // Re-registering replaces in place: the old pointer still answers, with the new data.
        const PropertyDescriptor* before = PropertyRegistry::Find("Entity", "Count");
        const std::size_t revision = PropertyRegistry::Revision();
        PropertyDescriptor copy = *before;
        copy.DisplayName = "Counter";
        PropertyRegistry::Register(copy);
        CHECK(PropertyRegistry::Find("Entity", "Count") == before);
        CHECK(before->DisplayName == "Counter");
        CHECK(PropertyRegistry::Revision() > revision);

        CHECK(PropertyChannelCount(PropertyType::Transform) == 9);
        CHECK(std::string(PropertyChannelName(PropertyType::Transform, 4)) == "Rotation.Y");
        CHECK(std::string(PropertyChannelName(PropertyType::Color, 3)) == "A");
    }

    // Builds the "first working demo" sequence from the brief against fake objects.
    CinematicSequence BuildIntro(FakeHost& host)
    {
        FakeObject& cameraA = host.Add(1001, "Camera_A");
        cameraA.IsCamera = true;
        FakeObject& cameraB = host.Add(1002, "Camera_B");
        cameraB.IsCamera = true;
        cameraB.Location[0] = 6.0;
        FakeObject& light = host.Add(1003, "PointLight");
        light.HasLight = true;
        light.Intensity = 0.0;
        host.Add(1004, "Cube");

        CinematicSequence sequence;
        sequence.Name = "Intro";
        sequence.PlaybackStart = 0.0;
        sequence.PlaybackEnd = 5.0;

        SequenceBinding& a = Bind(sequence, *host.Find("Camera_A"));
        auto& aTransform = AddTrack(a, std::make_unique<PropertyTrack>("Transform", PropertyType::Transform));
        aTransform.Name = "Transform";
        // 0-3 s: Camera_A moves forward 4 m along +Y.
        aTransform.Channel(1).SetKey(0.0, 0.0, KeyInterpolation::Linear);
        aTransform.Channel(1).SetKey(3.0, 4.0, KeyInterpolation::Linear);

        SequenceBinding& b = Bind(sequence, *host.Find("Camera_B"));
        auto& bTransform = AddTrack(b, std::make_unique<PropertyTrack>("Transform", PropertyType::Transform));
        // 3-5 s: Camera_B turns toward the cube (yaw, Rotation.Z) from 0 to 90 degrees.
        bTransform.Channel(5).SetKey(3.0, 0.0);
        bTransform.Channel(5).SetKey(5.0, 90.0);

        SequenceBinding& cube = Bind(sequence, *host.Find("Cube"));
        auto& cubeTransform = AddTrack(cube, std::make_unique<PropertyTrack>("Transform", PropertyType::Transform));
        // 1-4 s: the cube rises 2 m.
        cubeTransform.Channel(2).SetKey(1.0, 0.0, KeyInterpolation::Linear);
        cubeTransform.Channel(2).SetKey(4.0, 2.0, KeyInterpolation::Linear);

        SequenceBinding& lightBinding = Bind(sequence, *host.Find("PointLight"));
        auto& intensity = AddTrack(lightBinding, std::make_unique<PropertyTrack>("Light.Intensity", PropertyType::Float));
        // 4-5 s: 0 -> 5000.
        intensity.Channel(0).SetKey(4.0, 0.0, KeyInterpolation::Linear);
        intensity.Channel(0).SetKey(5.0, 5000.0, KeyInterpolation::Linear);

        CameraCutTrack& cuts = sequence.GetOrAddCameraCutTrack();
        CameraCutSection first;
        first.Start = 0.0;
        first.End = 3.0;
        first.CameraBinding = sequence.Bindings[0].Id;
        CameraCutSection second;
        second.Start = 3.0;
        second.End = 5.0;
        second.CameraBinding = sequence.Bindings[1].Id;
        cuts.Sections = { first, second };

        EventTrack& events = sequence.GetOrAddEventTrack();
        events.AddKey(2.0, "LightOn");
        events.AddKey(5.0, "IntroFinished");
        return sequence;
    }

    void TestSerialization()
    {
        std::printf("Serialization\n");
        FakeHost host;
        const CinematicSequence sequence = BuildIntro(host);
        const std::string text = sequence.ToJsonString();

        CinematicSequence loaded;
        std::string error;
        CHECK(loaded.FromJsonString(text, error));
        CHECK(error.empty());
        const std::string reloaded = loaded.ToJsonString();
        CHECK(reloaded == text);
        if (reloaded != text)
        {
            std::size_t at = 0;
            while (at < text.size() && at < reloaded.size() && text[at] == reloaded[at])
                ++at;
            const std::size_t from = at > 120 ? at - 120 : 0;
            std::printf("    first difference at %zu:\n    saved:    ...%s\n    reloaded: ...%s\n", at,
                        text.substr(from, 200).c_str(), reloaded.substr(from, 200).c_str());
        }
        CHECK(loaded.Id == sequence.Id);
        CHECK(loaded.Bindings.size() == 4);
        CHECK(loaded.FindCameraCutTrack() != nullptr && loaded.FindCameraCutTrack()->Sections.size() == 2);
        CHECK(loaded.FindEventTrack() != nullptr && loaded.FindEventTrack()->Keys().size() == 2);

        // Nothing pointer-like is written: bindings name entities by id and name.
        const nlohmann::json json = nlohmann::json::parse(text);
        CHECK(json["Bindings"][0]["EntityId"].get<std::uint64_t>() == 1001);
        CHECK(json["Bindings"][0]["EntityName"].get<std::string>() == "Camera_A");

        // Unknown track types are dropped with a warning, not a failed load.
        nlohmann::json future = json;
        future["Tracks"].push_back({ { "Type", "HologramTrack" }, { "Id", Guid::New().ToString() } });
        CinematicSequence partial;
        error.clear();
        CHECK(partial.FromJson(future, error));
        CHECK(error.find("HologramTrack") != std::string::npos);
        CHECK(partial.MasterTracks.size() == 2);

        CinematicSequence rejected;
        CHECK(!rejected.FromJsonString("{\"PteroSequence\": 99}", error));
        CHECK(!rejected.FromJsonString("{}", error));

        // Copies are deep.
        CinematicSequence copy = sequence;
        static_cast<PropertyTrack&>(*copy.Bindings[0].Tracks[0]).Channel(1).SetKey(1.0, 100.0);
        CHECK(static_cast<PropertyTrack&>(*sequence.Bindings[0].Tracks[0]).Channel(1).KeyCount() == 2);
    }

    void TestIntroPlayback()
    {
        std::printf("Intro demo playback\n");
        FakeHost host;
        auto sequence = std::make_shared<CinematicSequence>(BuildIntro(host));
        CinematicSystem system;
        system.SetHost(&host);

        std::vector<std::pair<std::string, double>> events;
        int started = 0;
        int finished = 0;
        system.AddEventListener([&](SequencePlayer& player, const SequenceEventKey& key) {
            events.emplace_back(key.Name, player.GetTime());
        });
        system.AddStartedListener([&](SequencePlayer&) { ++started; });
        system.AddFinishedListener([&](SequencePlayer&) { ++finished; });

        SequencePlayer* player = system.Play(sequence);
        CHECK(player != nullptr);
        CHECK(started == 1);
        CHECK(host.CutCameraName == "Camera_A");

        FakeObject& cameraA = *host.Find("Camera_A");
        FakeObject& cameraB = *host.Find("Camera_B");
        FakeObject& cube = *host.Find("Cube");
        FakeObject& light = *host.Find("PointLight");

        const double frame = 1.0 / 30.0;
        bool cutSeenAtThree = false;
        while (player->IsPlaying())
        {
            system.Tick(frame);
            const double t = player->GetTime();
            if (std::fabs(t - 1.5) < frame * 0.5)
            {
                CHECK_NEAR(cameraA.Location[1], 2.0, 0.05);
                CHECK_NEAR(cube.Location[2], 2.0 / 3.0 * 0.5, 0.05);
                CHECK(host.CutCameraName == "Camera_A");
            }
            if (t > 3.0 + frame && t < 3.0 + 2 * frame)
                cutSeenAtThree = host.CutCameraName == "Camera_B";
            if (std::fabs(t - 4.5) < frame * 0.5)
            {
                CHECK_NEAR(light.Intensity, 2500.0, 200.0);
                CHECK(cameraB.RotationDegrees[2] > 0.0 && cameraB.RotationDegrees[2] < 90.0);
            }
        }

        CHECK(cutSeenAtThree);
        CHECK(finished == 1);
        CHECK_NEAR(player->GetTime(), 5.0, 1e-9);
        CHECK_NEAR(light.Intensity, 5000.0, 1e-6);
        CHECK_NEAR(cube.Location[2], 2.0, 1e-6);
        CHECK_NEAR(cameraA.Location[1], 4.0, 1e-6);
        CHECK_NEAR(cameraB.RotationDegrees[2], 90.0, 1e-6);
        // Camera_B's location was never keyed, so the sequence left it alone.
        CHECK_NEAR(cameraB.Location[0], 6.0, 0.0);
        // The camera is handed back when the sequence finishes.
        CHECK(host.CutCameraName.empty());

        CHECK(events.size() == 2);
        if (events.size() == 2)
        {
            CHECK(events[0].first == "LightOn");
            CHECK(events[1].first == "IntroFinished");
            CHECK(events[0].second >= 2.0 && events[0].second < 2.0 + frame + 1e-9);
        }

        // Releasing from inside a callback is deferred, never a use-after-free.
        player->OnSequenceStarted = [&](SequencePlayer& p) { system.Release(&p); };
        player->PlayFromStart();
        CHECK(system.Players().size() == 1); // still alive inside the call that released it
        system.Tick(frame);
        CHECK(system.Players().empty());
    }

    void TestLoopingAndScrubbing()
    {
        std::printf("Looping, scrubbing, reverse, pause\n");
        FakeHost host;
        auto sequence = std::make_shared<CinematicSequence>(BuildIntro(host));
        SequencePlayer player(sequence, &host);

        std::vector<std::string> events;
        player.OnSequenceEvent = [&](SequencePlayer&, const SequenceEventKey& key) { events.push_back(key.Name); };

        // Scrubbing does not fire events unless asked to.
        player.SetTime(4.0);
        CHECK(events.empty());
        CHECK_NEAR(host.Find("Cube")->Location[2], 2.0, 1e-9);
        // Asked to, a scrub fires what it crosses in either direction - backwards too, by
        // the mirrored rule - so a marker is passed once per crossing.
        player.SetTime(1.0, true);
        CHECK(events.size() == 1);
        player.SetTime(3.0, true);
        CHECK(events.size() == 2 && events[1] == "LightOn");
        player.SetTime(3.0, true);
        CHECK(events.size() == 2);
        events.clear();

        // Looping: three full passes fire each marker three times, in order.
        player.SetLooping(true);
        player.PlayFromStart();
        for (int i = 0; i < 455; ++i) // just past 15 s at 30 fps, clear of float drift at the boundary
            player.Tick(1.0 / 30.0);
        int lightOn = 0;
        int finishedMarker = 0;
        for (const std::string& name : events)
        {
            lightOn += name == "LightOn";
            finishedMarker += name == "IntroFinished";
        }
        CHECK(lightOn == 3);
        CHECK(finishedMarker == 3);
        CHECK(player.IsPlaying());

        // Pause freezes the clock.
        player.Pause();
        const double paused = player.GetTime();
        player.Tick(1.0);
        CHECK_NEAR(player.GetTime(), paused, 0.0);
        player.Resume();
        player.Tick(0.1);
        CHECK(player.GetTime() != paused);

        // Reverse playback crosses LightOn once going down.
        player.SetLooping(false);
        player.SetTime(4.0);
        events.clear();
        player.SetPlayRate(-1.0);
        for (int i = 0; i < 300 && player.IsPlaying(); ++i)
            player.Tick(1.0 / 30.0);
        CHECK(events.size() == 1 && !events.empty() && events[0] == "LightOn");
        CHECK_NEAR(player.GetTime(), 0.0, 1e-9);
        CHECK(!player.IsPlaying());

        // A listener that stops the player ends the batch: no later event fires.
        player.SetPlayRate(1.0);
        events.clear();
        player.OnSequenceEvent = [&](SequencePlayer& p, const SequenceEventKey& key) {
            events.push_back(key.Name);
            p.Stop();
        };
        player.PlayFromStart();
        player.Tick(10.0); // one huge step across both markers
        CHECK(events.size() == 1);
        CHECK(!player.IsPlaying());
    }

    void TestRestoreState()
    {
        std::printf("Restore state\n");
        FakeHost host;
        auto sequence = std::make_shared<CinematicSequence>(BuildIntro(host));
        FakeObject& cube = *host.Find("Cube");
        cube.Location[0] = 7.0;
        cube.Location[2] = -3.0;

        SequencePlayer player(sequence, &host);
        player.RestoreStateOnStop = true;
        player.SetTime(4.0);
        CHECK_NEAR(cube.Location[2], 2.0, 1e-9);
        CHECK_NEAR(cube.Location[0], 7.0, 0.0); // unkeyed component untouched
        player.Play();
        player.Tick(0.5);
        player.Stop();
        CHECK_NEAR(cube.Location[2], -3.0, 0.0);
        CHECK_NEAR(host.Find("PointLight")->Intensity, 0.0, 0.0);
        CHECK(host.CutCameraName.empty());

        // Restore still finds an object whose binding was deleted from the sequence.
        auto edited = std::make_shared<CinematicSequence>(*sequence);
        player.SetSequence(edited);
        player.SetTime(4.0);
        edited->Bindings.erase(edited->Bindings.begin() + 2); // the cube
        player.RestoreState();
        CHECK_NEAR(cube.Location[2], -3.0, 0.0);
    }

    void TestTrackTypes()
    {
        std::printf("Bool, int, colour tracks and missing objects\n");
        FakeHost host;
        host.Objects.reserve(8); // the references below must survive the second Add
        FakeObject& lamp = host.Add(1, "Lamp");
        lamp.HasLight = true;
        FakeObject& plain = host.Add(2, "Plain"); // no light: light tracks must skip it

        CinematicSequence sequence;
        sequence.PlaybackEnd = 2.0;
        SequenceBinding& lampBinding = Bind(sequence, lamp);
        auto& visible = AddTrack(lampBinding, std::make_unique<PropertyTrack>("Visible", PropertyType::Bool));
        visible.SetKey(0.0, PropertyValue::MakeBool(true));
        visible.SetKey(1.0, PropertyValue::MakeBool(false));
        auto& count = AddTrack(lampBinding, std::make_unique<PropertyTrack>("Count", PropertyType::Int));
        count.SetKey(0.0, PropertyValue::MakeInt(0), KeyInterpolation::Linear);
        count.SetKey(2.0, PropertyValue::MakeInt(10), KeyInterpolation::Linear);
        auto& color = AddTrack(lampBinding, std::make_unique<PropertyTrack>("Light.Color", PropertyType::Color));
        color.SetKey(0.0, PropertyValue::MakeColor(1, 0, 0, 1), KeyInterpolation::Linear);
        color.SetKey(2.0, PropertyValue::MakeColor(0, 0, 1, 1), KeyInterpolation::Linear);

        SequenceBinding& plainBinding = Bind(sequence, plain);
        auto& plainIntensity = AddTrack(plainBinding, std::make_unique<PropertyTrack>("Light.Intensity", PropertyType::Float));
        plainIntensity.SetKey(0.0, PropertyValue::MakeFloat(100.0));

        SequenceBinding missing;
        missing.Name = "Ghost";
        missing.EntityId = 999;
        sequence.Bindings.push_back(std::move(missing));
        auto& ghost = AddTrack(sequence.Bindings.back(), std::make_unique<PropertyTrack>("Transform", PropertyType::Transform));
        ghost.Channel(0).SetKey(0.0, 1.0);

        SequencePlayer player(std::make_shared<CinematicSequence>(sequence), &host);
        player.SetTime(0.5);
        CHECK(lamp.Visible);
        CHECK(lamp.Count == 3); // 2.5 rounds to 3 (half away from zero)
        CHECK_NEAR(lamp.Color[0], 0.75, 1e-9);
        CHECK_NEAR(lamp.Color[2], 0.25, 1e-9);
        player.SetTime(1.5);
        CHECK(!lamp.Visible);
        CHECK(lamp.Count == 8);
        CHECK_NEAR(plain.Intensity, 0.0, 0.0);

        // Muted tracks are skipped.
        auto mutedSequence = std::make_shared<CinematicSequence>(sequence);
        mutedSequence->Bindings[0].Tracks[0]->Muted = true;
        SequencePlayer mutedPlayer(mutedSequence, &host);
        lamp.Visible = true;
        mutedPlayer.SetTime(1.5);
        CHECK(lamp.Visible);

        // Name fallback: a binding whose id is not in this world still finds its object.
        CinematicSequence renamed = sequence;
        renamed.Bindings[0].EntityId = 424242;
        SequencePlayer fallback(std::make_shared<CinematicSequence>(renamed), &host);
        lamp.Count = 0;
        fallback.SetTime(2.0);
        CHECK(lamp.Count == 10);
    }

    // -----------------------------------------------------------------------------------
    // The Sequencer's editing model (Editor/Sequencer/SequencerDocument), still Qt-free.
    // -----------------------------------------------------------------------------------

    std::uint32_t FirstKeyId(const PropertyTrack& track, int channel, double time)
    {
        for (const Keyframe& key : track.Channel(channel).Keys())
        {
            if (std::fabs(key.Time - time) < 1e-9)
                return key.Id;
        }
        return 0;
    }

    void TestDocumentUndo()
    {
        std::printf("Sequencer document: transactions and undo\n");
        FakeHost host;
        host.Objects.reserve(8);
        FakeObject& lamp = host.Add(1, "Lamp");
        lamp.HasLight = true;
        lamp.Intensity = 100.0;

        SequencerDocument document(&host);
        const PropertyDescriptor* intensity = PropertyRegistry::Find("Entity", "Light.Intensity");

        const Guid binding = document.AddEntityBinding(lamp.Id, lamp.Name);
        CHECK(document.Sequence().Bindings.size() == 1);
        CHECK(document.AddEntityBinding(lamp.Id, lamp.Name) == binding); // no duplicate

        PropertyTrack* track = document.AddPropertyTrack(binding, *intensity, true, 0.0);
        CHECK(track != nullptr && track->Channel(0).KeyCount() == 1);
        const Guid trackId = track->Id;

        document.SetTime(1.0);
        lamp.Intensity = 900.0;
        document.AddKey(trackId, 1.0);
        CHECK(track->Channel(0).KeyCount() == 2);

        // Undo walks back key, track, binding; redo brings them all back.
        document.Undo();
        CHECK(static_cast<PropertyTrack*>(document.FindTrack(trackId))->Channel(0).KeyCount() == 1);
        document.Undo();
        CHECK(document.FindTrack(trackId) == nullptr);
        document.Undo();
        CHECK(document.Sequence().Bindings.empty());
        CHECK(!document.CanUndo());
        document.Redo();
        document.Redo();
        document.Redo();
        CHECK(document.Sequence().Bindings.size() == 1);
        track = static_cast<PropertyTrack*>(document.FindTrack(trackId));
        CHECK(track != nullptr && track->Channel(0).KeyCount() == 2);

        // A transaction that changes nothing is not recorded.
        const bool couldUndo = document.CanUndo();
        document.Begin("Nothing", SequencerDocument::Scope_AllBindings);
        document.Commit();
        CHECK(document.CanUndo() == couldUndo);
        CHECK(document.UndoLabel() == "Add Key");

        // Deleting a binding and undoing puts it back in the same place.
        FakeObject& other = host.Add(2, "Other");
        const Guid second = document.AddEntityBinding(other.Id, other.Name);
        document.RemoveBinding(binding);
        CHECK(document.Sequence().Bindings.size() == 1);
        document.Undo();
        CHECK(document.Sequence().Bindings.size() == 2);
        CHECK(document.Sequence().Bindings[0].Id == binding);
        CHECK(document.Sequence().Bindings[1].Id == second);
    }

    void TestDocumentKeyEditing()
    {
        std::printf("Sequencer document: moving, scaling, copying keys\n");
        FakeHost host;
        host.Objects.reserve(8);
        FakeObject& cube = host.Add(1, "Cube");
        SequencerDocument document(&host);
        const PropertyDescriptor* transform = PropertyRegistry::Find("Entity", "Transform");
        const Guid binding = document.AddEntityBinding(cube.Id, cube.Name);
        PropertyTrack* track = document.AddPropertyTrack(binding, *transform, false, 0.0);
        const Guid trackId = track->Id;

        document.Begin("Setup", SequencerDocument::Scope_None, { binding });
        track->Channel(2).SetKey(1.0, 0.0, KeyInterpolation::Linear);
        track->Channel(2).SetKey(2.0, 5.0, KeyInterpolation::Linear);
        track->Channel(0).SetKey(1.5, 3.0, KeyInterpolation::Linear);
        document.Commit();

        // Drag two keys with snapping at 30 fps: offsets stay exact.
        const std::uint32_t a = FirstKeyId(*track, 2, 1.0);
        const std::uint32_t b = FirstKeyId(*track, 0, 1.5);
        document.SelectedKeys() = { { trackId, 2, a }, { trackId, 0, b } };
        document.BeginMoveKeys({ trackId, 2, a });
        document.UpdateMoveKeys(0.51); // anchor 1.51 -> snaps to 1.5 (frame 45)
        document.EndMoveKeys();
        double ta = 0.0;
        double tb = 0.0;
        CHECK(document.KeyTime({ trackId, 2, a }, ta) && document.KeyTime({ trackId, 0, b }, tb));
        CHECK_NEAR(ta, 1.5, 1e-9);
        CHECK_NEAR(tb - ta, 0.5, 1e-9);
        CHECK(document.UndoLabel() == "Move Keys");

        // Dropping a key onto another replaces the one underneath.
        document.SelectedKeys() = { { trackId, 2, a } };
        document.BeginMoveKeys({ trackId, 2, a });
        document.UpdateMoveKeys(0.5); // onto the key at 2.0
        document.EndMoveKeys();
        CHECK(track->Channel(2).KeyCount() == 1);
        document.Undo();
        track = static_cast<PropertyTrack*>(document.FindTrack(trackId));
        CHECK(track->Channel(2).KeyCount() == 2);

        // A cancelled drag leaves no history and every key where it was.
        const std::size_t before = track->Channel(2).Keys().size();
        const std::uint32_t c = track->Channel(2).Keys()[0].Id;
        const double cTime = track->Channel(2).Keys()[0].Time;
        const std::string undoBefore = document.UndoLabel();
        document.SelectedKeys() = { { trackId, 2, c } };
        document.BeginMoveKeys({ trackId, 2, c });
        document.UpdateMoveKeys(0.3);
        document.CancelMoveKeys();
        double cAfter = 0.0;
        CHECK(document.KeyTime({ trackId, 2, c }, cAfter));
        CHECK_NEAR(cAfter, cTime, 0.0);
        CHECK(track->Channel(2).Keys().size() == before);
        CHECK(document.UndoLabel() == undoBefore);

        // Scale about a pivot.
        const std::uint32_t z0 = track->Channel(2).Keys()[0].Id;
        const std::uint32_t z1 = track->Channel(2).Keys()[1].Id;
        document.SelectedKeys() = { { trackId, 2, z0 }, { trackId, 2, z1 } };
        double t0 = 0.0;
        double t1 = 0.0;
        document.KeyTime({ trackId, 2, z0 }, t0);
        document.KeyTime({ trackId, 2, z1 }, t1);
        document.ScaleSelectedKeys(2.0, t0);
        double s1 = 0.0;
        document.KeyTime({ trackId, 2, z1 }, s1);
        CHECK_NEAR(s1 - t0, (t1 - t0) * 2.0, 1e-9);

        // Copy and paste at a new time keeps the spacing, and selects the copies.
        document.CopySelectedKeys();
        const std::size_t keysBefore = track->Channel(2).Keys().size();
        document.Paste(10.0);
        CHECK(track->Channel(2).Keys().size() == keysBefore + 2);
        CHECK(document.SelectedKeys().size() == 2);
        double p0 = 1e9;
        for (const SequencerKeyHandle& handle : document.SelectedKeys())
        {
            double t = 0.0;
            document.KeyTime(handle, t);
            p0 = (std::min)(p0, t);
        }
        CHECK_NEAR(p0, 10.0, 1e-9);

        // Delete selection.
        document.DeleteSelectedKeys();
        CHECK(track->Channel(2).Keys().size() == keysBefore);
    }

    void TestDocumentCameraCuts()
    {
        std::printf("Sequencer document: camera cuts\n");
        FakeHost host;
        host.Objects.reserve(8);
        host.Add(1, "CamA");
        host.Add(2, "CamB");
        SequencerDocument document(&host);
        document.Sequence().PlaybackEnd = 6.0;
        const Guid a = document.AddEntityBinding(1, "CamA");
        const Guid b = document.AddEntityBinding(2, "CamB");

        document.AddCameraCut(0.0, a);
        document.AddCameraCut(3.0, b); // splits A's cut
        const CameraCutTrack* cuts = document.Sequence().FindCameraCutTrack();
        CHECK(cuts != nullptr && cuts->Sections.size() == 2);
        CHECK_NEAR(cuts->Sections[0].End, 3.0, 1e-9);
        CHECK_NEAR(cuts->Sections[1].End, 6.0, 1e-9);

        // Dragging the shared edge resizes both cuts.
        const Guid second = cuts->Sections[1].Id;
        document.BeginEditCut(second);
        document.UpdateCutEdge(0, 2.0);
        document.EndEditCut();
        cuts = document.Sequence().FindCameraCutTrack();
        CHECK_NEAR(cuts->Sections[0].End, 2.0, 1e-9);
        CHECK_NEAR(cuts->Sections[1].Start, 2.0, 1e-9);

        document.SetTime(2.5);
        CHECK(host.CutCameraName == "CamB");
        document.SetTime(1.0);
        CHECK(host.CutCameraName == "CamA");

        document.Undo();
        cuts = document.Sequence().FindCameraCutTrack();
        CHECK_NEAR(cuts->Sections[0].End, 3.0, 1e-9);
    }

    void TestDocumentAutoKey()
    {
        std::printf("Sequencer document: Auto Key and preview restore\n");
        FakeHost host;
        host.Objects.reserve(8);
        FakeObject& camera = host.Add(1, "Camera01");
        camera.Location[1] = -5.0;
        FakeObject& bystander = host.Add(2, "Bystander");
        SequencerDocument document(&host);
        document.AddEntityBinding(camera.Id, camera.Name);

        // Without Auto Key an edit is just a level edit.
        document.SetTime(0.0);
        camera.Location[0] = 1.0;
        CHECK(!document.OnWorldEdited());
        CHECK(document.Sequence().Bindings[0].Tracks.empty());

        // With it, moving a bound entity at frame 240 (8 s at 30 fps) starts a transform
        // track with keys on exactly the channels that moved.
        document.Sequence().PlaybackEnd = 10.0;
        document.AutoKey = true;
        document.SetTime(8.0);
        camera.Location[0] = 4.0;
        camera.Location[2] = 2.0;
        bystander.Location[0] = 9.0; // not bound: ignored
        CHECK(document.OnWorldEdited());
        const auto* track = dynamic_cast<const PropertyTrack*>(document.Sequence().Bindings[0].Tracks[0].get());
        CHECK(track != nullptr && track->PropertyPath() == "Transform");
        CHECK(track->Channel(0).KeyCount() == 1 && track->Channel(2).KeyCount() == 1);
        CHECK(track->Channel(1).KeyCount() == 0);
        CHECK_NEAR(track->Channel(0).Keys()[0].Time, 8.0, 1e-9);

        // Editing again at another time updates the curve there; earlier keys stay.
        document.SetTime(4.0);
        camera.Location[0] = -2.0;
        CHECK(document.OnWorldEdited());
        CHECK(track->Channel(0).KeyCount() == 2);
        document.SetTime(8.0);
        CHECK_NEAR(camera.Location[0], 4.0, 1e-6);

        // Ending the preview returns the level to its authored pose - from before the
        // first Auto Key edit, not the edited one.
        document.EndPreview();
        CHECK_NEAR(camera.Location[0], 1.0, 1e-6);
        CHECK_NEAR(camera.Location[2], 0.0, 1e-6);
        CHECK_NEAR(bystander.Location[0], 9.0, 0.0);

        // Suspend/resume (around a level save) restores, then re-applies.
        document.SetTime(8.0);
        CHECK_NEAR(camera.Location[0], 4.0, 1e-6);
        document.SuspendPreview();
        CHECK_NEAR(camera.Location[0], 1.0, 1e-6);
        document.SetTime(4.0); // ignored while suspended
        CHECK_NEAR(camera.Location[0], 1.0, 1e-6);
        document.ResumePreview();
        document.Tick(0.0);
        CHECK_NEAR(camera.Location[0], 4.0, 1e-6);
    }

    // The shipped demo assets: Data/Cinematics/Intro.sequence against the entities of
    // Data/Levels/CinematicDemo.level, read from disk exactly as the engine reads them.
    void TestDemoAssets()
    {
        std::printf("Demo assets (Intro.sequence on CinematicDemo.level)\n");
        const std::string root = std::string(__FILE__).substr(0, std::string(__FILE__).find("Source")) + "Data/";

        std::ifstream levelFile(root + "Levels/CinematicDemo.level");
        CHECK(levelFile.good());
        if (!levelFile.good())
            return;
        const nlohmann::json level = nlohmann::json::parse(levelFile, nullptr, false);
        CHECK(!level.is_discarded());

        FakeHost host;
        host.Objects.reserve(64);
        for (const nlohmann::json& entity : level["Entities"])
        {
            FakeObject& object = host.Add(entity.value("Id", std::uint64_t(0)), entity["NameComponent"].value("Name", ""));
            const nlohmann::json& transform = entity["TransformComponent"];
            object.Location[0] = transform["Position"]["x"].get<double>();
            object.Location[1] = transform["Position"]["y"].get<double>();
            object.Location[2] = transform["Position"]["z"].get<double>();
            object.HasLight = entity.contains("PointLightComponent");
            object.IsCamera = entity.contains("CameraComponent");
            if (object.HasLight)
                object.Intensity = entity["PointLightComponent"].value("IntensityLumens", 0.0);
        }

        auto sequence = std::make_shared<CinematicSequence>();
        std::string error;
        CHECK(sequence->LoadFromFile(root + "Cinematics/Intro.sequence", error));
        CHECK(error.empty());
        if (!error.empty())
            std::printf("    %s\n", error.c_str());

        // Every binding finds its entity by id, and the cameras the cuts use are cameras.
        for (const SequenceBinding& binding : sequence->Bindings)
        {
            const auto* object = static_cast<FakeObject*>(host.ResolveBinding(binding));
            CHECK(object != nullptr && object->Id == binding.EntityId);
        }
        const CameraCutTrack* cuts = sequence->FindCameraCutTrack();
        CHECK(cuts != nullptr && cuts->Sections.size() == 2);
        for (const CameraCutSection& section : cuts->Sections)
        {
            const SequenceBinding* camera = sequence->FindBinding(section.CameraBinding);
            CHECK(camera != nullptr && static_cast<FakeObject*>(host.ResolveBinding(*camera))->IsCamera);
        }

        // Play it at 30 fps as the game would.
        CinematicSystem system;
        system.SetHost(&host);
        std::vector<std::pair<std::string, double>> events;
        std::vector<std::pair<double, std::string>> cutChanges;
        system.AddEventListener([&](SequencePlayer& player, const SequenceEventKey& key) {
            events.emplace_back(key.Name, player.GetTime());
        });
        SequencePlayer* player = system.Play(sequence);
        std::string lastCut = host.CutCameraName;
        cutChanges.emplace_back(0.0, lastCut);
        while (player->IsPlaying())
        {
            system.Tick(1.0 / 30.0);
            if (host.CutCameraName != lastCut)
            {
                lastCut = host.CutCameraName;
                cutChanges.emplace_back(player->GetTime(), lastCut);
            }
        }

        CHECK(events.size() == 2);
        CHECK(events.size() == 2 && events[0].first == "LightOn" && std::fabs(events[0].second - 2.0) < 1e-9);
        CHECK(events.size() == 2 && events[1].first == "IntroFinished" && std::fabs(events[1].second - 5.0) < 1e-9);
        // Camera_A from the start, Camera_B from 3 s, the camera handed back at the end.
        CHECK(cutChanges.size() == 3);
        CHECK(cutChanges.size() == 3 && cutChanges[0].second == "Camera_A");
        CHECK(cutChanges.size() == 3 && cutChanges[1].second == "Camera_B" && std::fabs(cutChanges[1].first - 3.0) < 0.04);
        CHECK(cutChanges.size() == 3 && cutChanges[2].second.empty());
        CHECK_NEAR(host.Find("Camera_A")->Location[1], -3.0, 1e-6);
        CHECK_NEAR(host.Find("Cube")->Location[2], 2.5, 1e-6);
        CHECK_NEAR(host.Find("Camera_B")->RotationDegrees[2], 90.0, 1e-6);
        CHECK_NEAR(host.Find("PointLight")->Intensity, 5000.0, 1e-6);
    }

    void TestPerformance()
    {
        std::printf("Large sequence evaluation\n");
        FakeHost host;
        CinematicSequence sequence;
        sequence.PlaybackEnd = 60.0;
        // 300 bound objects, each with a 9-channel transform keyed every half second:
        // 300 * 9 * 120 = 324 000 keys.
        for (int i = 0; i < 300; ++i)
        {
            FakeObject& object = host.Add(10000 + i, "Object" + std::to_string(i));
            SequenceBinding& binding = Bind(sequence, object);
            auto& transform = AddTrack(binding, std::make_unique<PropertyTrack>("Transform", PropertyType::Transform));
            for (int k = 0; k < 120; ++k)
            {
                PropertyValue value = PropertyValue::MakeTransform();
                for (int c = 0; c < 9; ++c)
                    value.Components[c] = std::sin(k * 0.1 + c + i);
                transform.SetKey(k * 0.5, value);
            }
        }

        SequencePlayer player(std::make_shared<CinematicSequence>(std::move(sequence)), &host);
        player.Play();
        const auto begin = std::chrono::steady_clock::now();
        int frames = 0;
        while (player.IsPlaying())
        {
            player.Tick(1.0 / 60.0);
            ++frames;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::printf("    %d frames, 300 transforms, 324k keys: %.3f ms per frame\n", frames, ms / frames);
        CHECK(ms / frames < 5.0);
    }
}

int main()
{
    // Unbuffered, so a crash still leaves the output of every test before it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RegisterFakeProperties();

    // An exception fails its test and the run carries on.
    auto run = [](void (*test)()) {
        try
        {
            test();
        }
        catch (const std::exception& exception)
        {
            ++gFailures;
            std::printf("  FAILED with exception: %s\n", exception.what());
        }
    };

    run(TestGuid);
    run(TestBasicInterpolation);
    run(TestAutoTangents);
    run(TestValueKinds);
    run(TestSegmentSearch);
    run(TestRegistry);
    run(TestSerialization);
    run(TestIntroPlayback);
    run(TestLoopingAndScrubbing);
    run(TestRestoreState);
    run(TestTrackTypes);
    run(TestDocumentUndo);
    run(TestDocumentKeyEditing);
    run(TestDocumentCameraCuts);
    run(TestDocumentAutoKey);
    run(TestDemoAssets);
    run(TestPerformance);

    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
