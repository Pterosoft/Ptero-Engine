// Everything on an entity, and in the engine-wide settings, that Ptero Sequencer can animate
// and the Node Graph can read or set - registered once, here, beside nothing but the data.
//
// A property's path ("Light.Intensity") is written into sequences and graphs, so it is an
// identifier: never rename one without keeping the old path as an alias. The aliases
// below with spaces in them are the Node Graph's original entity property names, kept so
// graphs written before the registry existed still resolve.

#include "pch.h"
#include "EntityProperties.h"

#include "Components.h"
#include "TimeOfDaySettings.h"
#include "VolumetricFogSettings.h"

#include "System/PropertyRegistry.h"

#include <cmath>
#include <initializer_list>

namespace
{
    constexpr double kDegreesPerRadian = 57.29577951308232;

    // Field accessors return null when the entity lacks the owning component, which is
    // also how "Has" is answered.
    using FloatField = float* (*)(Entity&);
    using BoolField = bool* (*)(Entity&);

    Entity& AsEntity(const void* owner)
    {
        // The registry hands back exactly the pointer the host resolved, which for this
        // domain is always a live Entity. Const only because Has/Get take const.
        return *static_cast<Entity*>(const_cast<void*>(owner));
    }

    PropertyDescriptor Describe(const char* domain, const char* path, const char* group, const char* name,
                                PropertyType type, const char* tooltip, std::initializer_list<const char*> aliases,
                                double minimum, double maximum, unsigned flags)
    {
        PropertyDescriptor descriptor;
        descriptor.Domain = domain;
        descriptor.Path = path;
        descriptor.Group = group;
        descriptor.DisplayName = name;
        descriptor.Tooltip = tooltip != nullptr ? tooltip : "";
        for (const char* alias : aliases)
            descriptor.Aliases.emplace_back(alias);
        descriptor.Type = type;
        descriptor.Minimum = minimum;
        descriptor.Maximum = maximum;
        descriptor.Flags = flags;
        return descriptor;
    }

    void EntityFloat(const char* path, const char* group, const char* name, FloatField field, double minimum,
                     double maximum, const char* tooltip = nullptr, std::initializer_list<const char*> aliases = {},
                     unsigned flags = PropertyFlag_Default)
    {
        PropertyDescriptor d = Describe("Entity", path, group, name, PropertyType::Float, tooltip, aliases, minimum,
                                        maximum, flags);
        d.Has = [field](const void* owner) { return field(AsEntity(owner)) != nullptr; };
        d.Get = [field](const void* owner, PropertyValue& value) {
            const float* f = field(AsEntity(owner));
            if (f == nullptr)
                return false;
            value = PropertyValue::MakeFloat(*f);
            return true;
        };
        d.Set = [field, minimum, maximum](void* owner, const PropertyValue& value) {
            float* f = field(AsEntity(owner));
            if (f == nullptr)
                return false;
            double v = value.AsFloat();
            // The range is a hard limit only where it protects the renderer (a negative
            // radius, a zero focal length); a pure slider hint has minimum == maximum.
            if (minimum < maximum)
                v = std::clamp(v, minimum, maximum);
            *f = static_cast<float>(v);
            return true;
        };
        PropertyRegistry::Register(std::move(d));
    }

    void EntityBool(const char* path, const char* group, const char* name, BoolField field,
                    const char* tooltip = nullptr, std::initializer_list<const char*> aliases = {},
                    unsigned flags = PropertyFlag_Default)
    {
        PropertyDescriptor d = Describe("Entity", path, group, name, PropertyType::Bool, tooltip, aliases, 0.0, 0.0, flags);
        d.Has = [field](const void* owner) { return field(AsEntity(owner)) != nullptr; };
        d.Get = [field](const void* owner, PropertyValue& value) {
            const bool* b = field(AsEntity(owner));
            if (b == nullptr)
                return false;
            value = PropertyValue::MakeBool(*b);
            return true;
        };
        d.Set = [field](void* owner, const PropertyValue& value) {
            bool* b = field(AsEntity(owner));
            if (b == nullptr)
                return false;
            *b = value.AsBool();
            return true;
        };
        PropertyRegistry::Register(std::move(d));
    }

    void RegisterTransform()
    {
        PropertyDescriptor d = Describe("Entity", "Transform", "Transform", "Transform", PropertyType::Transform,
                                        "Location in metres, rotation in degrees, scale.", {}, 0.0, 0.0,
                                        PropertyFlag_Default);
        d.Get = [](const void* owner, PropertyValue& value) {
            const TransformComponent& t = AsEntity(owner).Transform;
            value = PropertyValue::MakeTransform();
            const DirectX::XMFLOAT3* parts[3] = { &t.Position, &t.Rotation, &t.Scale };
            for (int p = 0; p < 3; ++p)
            {
                const double scale = p == 1 ? kDegreesPerRadian : 1.0;
                value.Components[p * 3 + 0] = parts[p]->x * scale;
                value.Components[p * 3 + 1] = parts[p]->y * scale;
                value.Components[p * 3 + 2] = parts[p]->z * scale;
            }
            return true;
        };
        d.Set = [](void* owner, const PropertyValue& value) {
            TransformComponent& t = AsEntity(owner).Transform;
            DirectX::XMFLOAT3* parts[3] = { &t.Position, &t.Rotation, &t.Scale };
            for (int p = 0; p < 3; ++p)
            {
                const double scale = p == 1 ? 1.0 / kDegreesPerRadian : 1.0;
                parts[p]->x = static_cast<float>(value.Components[p * 3 + 0] * scale);
                parts[p]->y = static_cast<float>(value.Components[p * 3 + 1] * scale);
                parts[p]->z = static_cast<float>(value.Components[p * 3 + 2] * scale);
            }
            return true;
        };
        PropertyRegistry::Register(std::move(d));
    }

    void RegisterCamera()
    {
        // Field of view is derived from the lens, so it gets accessors of its own rather
        // than a field: setting it moves the focal length.
        PropertyDescriptor fov = Describe("Entity", "Camera.FieldOfView", "Camera", "Field of View",
                                          PropertyType::Float, "Vertical field of view in degrees.", {}, 1.0, 170.0,
                                          PropertyFlag_Default);
        fov.Has = [](const void* owner) { return AsEntity(owner).Camera.has_value(); };
        fov.Get = [](const void* owner, PropertyValue& value) {
            const Entity& e = AsEntity(owner);
            if (!e.Camera)
                return false;
            value = PropertyValue::MakeFloat(e.Camera->VerticalFovDegrees());
            return true;
        };
        fov.Set = [](void* owner, const PropertyValue& value) {
            Entity& e = AsEntity(owner);
            if (!e.Camera)
                return false;
            e.Camera->SetVerticalFovDegrees(static_cast<float>(value.AsFloat()));
            return true;
        };
        PropertyRegistry::Register(std::move(fov));

        EntityFloat("Camera.FocalLength", "Camera", "Focal Length",
                    [](Entity& e) { return e.Camera ? &e.Camera->FocalLengthMm : nullptr; }, 1.0, 2000.0,
                    "Lens focal length in millimetres.");
        EntityFloat("Camera.SensorWidth", "Camera", "Sensor Width",
                    [](Entity& e) { return e.Camera ? &e.Camera->SensorWidthMm : nullptr; }, 1.0, 100.0,
                    "Sensor (film back) width in millimetres.");
        EntityFloat("Camera.SensorHeight", "Camera", "Sensor Height",
                    [](Entity& e) { return e.Camera ? &e.Camera->SensorHeightMm : nullptr; }, 1.0, 100.0,
                    "Sensor (film back) height in millimetres. With the focal length, sets the field of view.");
        EntityFloat("Camera.NearClip", "Camera", "Near Clip",
                    [](Entity& e) { return e.Camera ? &e.Camera->NearClip : nullptr; }, 0.001, 100.0,
                    "Near clipping distance in metres.");
        EntityFloat("Camera.ShakeAmplitude", "Camera", "Shake Amplitude",
                    [](Entity& e) { return e.Camera ? &e.Camera->ShakeAmplitudeDegrees : nullptr; }, 0.0, 45.0,
                    "Handheld shake, in degrees of wobble.");
        EntityFloat("Camera.ShakeFrequency", "Camera", "Shake Frequency",
                    [](Entity& e) { return e.Camera ? &e.Camera->ShakeFrequency : nullptr; }, 0.0, 50.0,
                    "Handheld shake speed, in wobbles per second.");
    }

    void RegisterLight()
    {
        EntityFloat("Light.Intensity", "Light", "Intensity",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->IntensityLumens : nullptr; }, 0.0, 1.0e7,
                    "Luminous flux in lumens.", { "Light Intensity" });
        EntityFloat("Light.Radius", "Light", "Radius",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->Radius : nullptr; }, 0.0, 1.0e4,
                    "Influence radius in metres.", { "Light Radius" });

        PropertyDescriptor color = Describe("Entity", "Light.Color", "Light", "Color", PropertyType::ColorRGB,
                                            "Linear RGB colour (ignored while Use Temperature is on).", {}, 0.0, 0.0,
                                            PropertyFlag_Default);
        color.Has = [](const void* owner) { return AsEntity(owner).PointLight.has_value(); };
        color.Get = [](const void* owner, PropertyValue& value) {
            const Entity& e = AsEntity(owner);
            if (!e.PointLight)
                return false;
            value = PropertyValue::MakeVector(PropertyType::ColorRGB, e.PointLight->ColorR, e.PointLight->ColorG,
                                              e.PointLight->ColorB);
            return true;
        };
        color.Set = [](void* owner, const PropertyValue& value) {
            Entity& e = AsEntity(owner);
            if (!e.PointLight)
                return false;
            e.PointLight->ColorR = static_cast<float>((std::max)(0.0, value.Components[0]));
            e.PointLight->ColorG = static_cast<float>((std::max)(0.0, value.Components[1]));
            e.PointLight->ColorB = static_cast<float>((std::max)(0.0, value.Components[2]));
            return true;
        };
        PropertyRegistry::Register(std::move(color));

        // Per-channel colour for the graph's single-number property nodes; the Sequencer
        // animates the colour as a whole instead, so these are not offered as tracks.
        EntityFloat("Light.ColorR", "Light", "Color R",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->ColorR : nullptr; }, 0.0, 1.0e4, nullptr,
                    { "Light Color R" }, PropertyFlag_Scriptable);
        EntityFloat("Light.ColorG", "Light", "Color G",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->ColorG : nullptr; }, 0.0, 1.0e4, nullptr,
                    { "Light Color G" }, PropertyFlag_Scriptable);
        EntityFloat("Light.ColorB", "Light", "Color B",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->ColorB : nullptr; }, 0.0, 1.0e4, nullptr,
                    { "Light Color B" }, PropertyFlag_Scriptable);

        EntityFloat("Light.Temperature", "Light", "Temperature",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->TemperatureKelvin : nullptr; }, 1000.0,
                    40000.0, "Blackbody colour temperature in Kelvin (used when Use Temperature is on).",
                    { "Light Temperature" });
        EntityBool("Light.UseTemperature", "Light", "Use Temperature",
                   [](Entity& e) { return e.PointLight ? &e.PointLight->UseTemperature : nullptr; }, nullptr,
                   { "Light Uses Temperature" });
        EntityBool("Light.CastShadows", "Light", "Cast Shadows",
                   [](Entity& e) { return e.PointLight ? &e.PointLight->CastShadows : nullptr; }, nullptr,
                   { "Light Casts Shadows" });
        EntityFloat("Light.SourceRadius", "Light", "Source Radius",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->SourceRadius : nullptr; }, 0.0, 100.0,
                    "Physical emitter size; larger gives softer shadows.");
        EntityFloat("Light.VolumetricFogIntensity", "Light", "Fog Intensity",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->VolumetricFogIntensity : nullptr; }, 0.0,
                    100.0, "How strongly this light scatters in volumetric fog.");
        EntityFloat("Light.GiContribution", "Light", "GI Contribution",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->GiContribution : nullptr; }, 0.0, 100.0,
                    "Scale on this light's indirect (bounce) contribution.");
        EntityFloat("Light.StyleAmplitude", "Light", "Style Amplitude",
                    [](Entity& e) { return e.PointLight ? &e.PointLight->StyleAmplitude : nullptr; }, 0.0, 10.0,
                    "How strongly the light style (flicker, pulse) modulates the light.");
        EntityFloat("Light.SpotInnerCone", "Light", "Spot Inner Cone",
                    [](Entity& e) {
                        return e.PointLight && e.PointLight->Type == LightType::Spot ? &e.PointLight->SpotInnerConeDegrees
                                                                                     : nullptr;
                    },
                    0.0, 179.0, "Full angle of the spot's hotspot, in degrees.");
        EntityFloat("Light.SpotOuterCone", "Light", "Spot Outer Cone",
                    [](Entity& e) {
                        return e.PointLight && e.PointLight->Type == LightType::Spot ? &e.PointLight->SpotOuterConeDegrees
                                                                                     : nullptr;
                    },
                    0.1, 179.0, "Full angle at which the spot fades out, in degrees.");
        EntityFloat("Light.RectWidth", "Light", "Rect Width",
                    [](Entity& e) {
                        return e.PointLight && e.PointLight->Type == LightType::Rect ? &e.PointLight->RectWidth : nullptr;
                    },
                    0.001, 1000.0, "Width of the emitting rectangle in metres.");
        EntityFloat("Light.RectHeight", "Light", "Rect Height",
                    [](Entity& e) {
                        return e.PointLight && e.PointLight->Type == LightType::Rect ? &e.PointLight->RectHeight : nullptr;
                    },
                    0.001, 1000.0, "Height of the emitting rectangle in metres.");
    }

    void RegisterEffects()
    {
        EntityBool("Particles.Enabled", "Particle System", "Enabled",
                   [](Entity& e) { return e.ParticleSystem ? &e.ParticleSystem->Enabled : nullptr; }, nullptr,
                   { "Particles Enabled" });
        EntityFloat("Particles.SpawnRate", "Particle System", "Spawn Rate",
                    [](Entity& e) { return e.ParticleSystem ? &e.ParticleSystem->SpawnRate : nullptr; }, 0.0, 1.0e6,
                    "Particles emitted per second.", { "Particle Spawn Rate" });
        EntityBool("Rain.Enabled", "Rain", "Enabled", [](Entity& e) { return e.Rain ? &e.Rain->Enabled : nullptr; },
                   nullptr, { "Rain Enabled" });
        EntityFloat("Rain.Intensity", "Rain", "Intensity",
                    [](Entity& e) { return e.Rain ? &e.Rain->Intensity : nullptr; }, 0.0, 100.0, nullptr,
                    { "Rain Intensity" });

        PropertyDescriptor decalSize = Describe("Entity", "Decal.Size", "Decal", "Size", PropertyType::Vector3,
                                                "Half-extents of the projection box in metres.", {}, 0.0, 0.0,
                                                PropertyFlag_Default);
        decalSize.Has = [](const void* owner) { return AsEntity(owner).Decal.has_value(); };
        decalSize.Get = [](const void* owner, PropertyValue& value) {
            const Entity& e = AsEntity(owner);
            if (!e.Decal)
                return false;
            value = PropertyValue::MakeVector(PropertyType::Vector3, e.Decal->SizeX, e.Decal->SizeY, e.Decal->SizeZ);
            return true;
        };
        decalSize.Set = [](void* owner, const PropertyValue& value) {
            Entity& e = AsEntity(owner);
            if (!e.Decal)
                return false;
            e.Decal->SizeX = static_cast<float>((std::max)(0.001, value.Components[0]));
            e.Decal->SizeY = static_cast<float>((std::max)(0.001, value.Components[1]));
            e.Decal->SizeZ = static_cast<float>((std::max)(0.001, value.Components[2]));
            return true;
        };
        PropertyRegistry::Register(std::move(decalSize));
    }

    // Environment: owner is EngineEnvironment*. Each accessor checks its own pointer, so a
    // host that supplies only some of the settings still works for the rest.
    template <class Settings>
    void EnvironmentFloat(const char* path, const char* group, const char* name, Settings* EngineEnvironment::*settings,
                          float Settings::*field, double minimum, double maximum, const char* tooltip)
    {
        PropertyDescriptor d = Describe("Environment", path, group, name, PropertyType::Float, tooltip, {}, minimum,
                                        maximum, PropertyFlag_Default);
        d.Has = [settings](const void* owner) {
            return static_cast<const EngineEnvironment*>(owner)->*settings != nullptr;
        };
        d.Get = [settings, field](const void* owner, PropertyValue& value) {
            const Settings* s = static_cast<const EngineEnvironment*>(owner)->*settings;
            if (s == nullptr)
                return false;
            value = PropertyValue::MakeFloat(s->*field);
            return true;
        };
        d.Set = [settings, field, minimum, maximum](void* owner, const PropertyValue& value) {
            Settings* s = static_cast<EngineEnvironment*>(owner)->*settings;
            if (s == nullptr)
                return false;
            double v = value.AsFloat();
            if (minimum < maximum)
                v = std::clamp(v, minimum, maximum);
            s->*field = static_cast<float>(v);
            return true;
        };
        PropertyRegistry::Register(std::move(d));
    }

    void RegisterEnvironment()
    {
        EnvironmentFloat("TimeOfDay.Hour", "Time of Day", "Time of Day", &EngineEnvironment::TimeOfDay,
                         &TimeOfDaySettings::TimeOfDay, 0.0, 24.0, "Hour of the day, 0-24. Moves the sun and moon.");
        EnvironmentFloat("TimeOfDay.SunIntensity", "Time of Day", "Sun Intensity", &EngineEnvironment::TimeOfDay,
                         &TimeOfDaySettings::SunIntensityLux, 0.0, 1.0e6, "Sun illuminance in lux.");
        EnvironmentFloat("TimeOfDay.SkyIntensity", "Time of Day", "Sky Intensity", &EngineEnvironment::TimeOfDay,
                         &TimeOfDaySettings::SkyIntensityLux, 0.0, 1.0e6, "Sky illuminance in lux.");
        EnvironmentFloat("TimeOfDay.Turbidity", "Time of Day", "Turbidity", &EngineEnvironment::TimeOfDay,
                         &TimeOfDaySettings::Turbidity, 1.0, 10.0, "Atmospheric haze.");
        EnvironmentFloat("Fog.Density", "Volumetric Fog", "Density", &EngineEnvironment::VolumetricFog,
                         &VolumetricFogSettings::Density, 0.0, 10.0, "Volumetric fog extinction density.");
        EnvironmentFloat("Fog.Anisotropy", "Volumetric Fog", "Anisotropy", &EngineEnvironment::VolumetricFog,
                         &VolumetricFogSettings::Anisotropy, -0.99, 0.99, "Forward (+) or backward (-) scattering.");
    }
}

void RegisterEngineProperties()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    RegisterTransform();
    RegisterCamera();
    RegisterLight();
    RegisterEffects();
    RegisterEnvironment();
}
