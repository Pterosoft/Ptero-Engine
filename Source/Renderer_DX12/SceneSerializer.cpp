#include "pch.h"

#include "SceneSerializer.h"
#include "EntityIds.h"
#include "ParticleEffects.h"

#include "System/DataFiles.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace
{
    // 1 (or a missing key): entity Euler angles were written for
    //    XMMatrixRotationRollPitchYaw, whose order suits a Y-up world.
    // 2: entity Euler angles are composed X then Y then Z, matching Ptero's Z-up world.
    //    See PteroTransform in Components.h.
    // Only entity rotations changed meaning. CameraRotation is untouched: the editor
    // camera consumes it as an explicit pitch/yaw pair, never through a Euler matrix.
    constexpr int kSceneFormatVersion = 2;

    json SerializeVector3(const DirectX::XMFLOAT3& value)
    {
        return json{
            { "X", value.x },
            { "Y", value.y },
            { "Z", value.z }
        };
    }

    void DeserializeVector3(const json& valueJson, DirectX::XMFLOAT3& value)
    {
        value.x = valueJson.value("X", value.x);
        value.y = valueJson.value("Y", value.y);
        value.z = valueJson.value("Z", value.z);
    }

    bool ReportDeserializeProgress(
        SceneSerializer::ProgressCallback progressCallback,
        void* userData,
        const float progress,
        const char* statusMessage)
    {
        if (progressCallback != nullptr)
        {
            return progressCallback(progress, statusMessage, userData);
        }

        return true;
    }

    json SerializeAgxGradeControl(const AgxColorGradeControl& control)
    {
        return json{
            { "Total", control.Total },
            { "Red", control.Red },
            { "Green", control.Green },
            { "Blue", control.Blue },
            { "Yellow", control.Yellow }
        };
    }

    void DeserializeAgxGradeControl(const json& controlJson, AgxColorGradeControl& control)
    {
        control.Total = controlJson.value("Total", control.Total);
        control.Red = controlJson.value("Red", control.Red);
        control.Green = controlJson.value("Green", control.Green);
        control.Blue = controlJson.value("Blue", control.Blue);
        control.Yellow = controlJson.value("Yellow", control.Yellow);
    }

    json SerializeAgxGradeRegion(const AgxColorGradeRegion& region)
    {
        return json{
            { "Contrast", SerializeAgxGradeControl(region.Contrast) },
            { "Gamma", SerializeAgxGradeControl(region.Gamma) },
            { "Gain", SerializeAgxGradeControl(region.Gain) },
            { "Saturation", SerializeAgxGradeControl(region.Saturation) },
            { "Vibrance", SerializeAgxGradeControl(region.Vibrance) }
        };
    }

    void DeserializeAgxGradeRegion(const json& regionJson, AgxColorGradeRegion& region)
    {
        if (regionJson.contains("Contrast")) DeserializeAgxGradeControl(regionJson["Contrast"], region.Contrast);
        if (regionJson.contains("Gamma")) DeserializeAgxGradeControl(regionJson["Gamma"], region.Gamma);
        if (regionJson.contains("Gain")) DeserializeAgxGradeControl(regionJson["Gain"], region.Gain);
        if (regionJson.contains("Saturation")) DeserializeAgxGradeControl(regionJson["Saturation"], region.Saturation);
        if (regionJson.contains("Vibrance")) DeserializeAgxGradeControl(regionJson["Vibrance"], region.Vibrance);
    }

    json SerializeSkyGlareSettings(const SkyGlareSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Intensity", settings.Intensity },
            { "HaloCore", settings.HaloCore },
            { "HaloRadius", settings.HaloRadius },
            { "HaloFalloff", settings.HaloFalloff },
            { "StreakStrength", settings.StreakStrength },
            { "StreakLength", settings.StreakLength },
            { "ApertureBlades", settings.ApertureBlades },
            { "ApertureRotation", settings.ApertureRotation },
            { "ChromaticSpread", settings.ChromaticSpread }
        };
    }

    // Missing keys keep what settings already holds (the body's defaults).
    void DeserializeSkyGlareSettings(const json& settingsJson, SkyGlareSettings& settings)
    {
        if (!settingsJson.is_object())
            return;
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.HaloCore = settingsJson.value("HaloCore", settings.HaloCore);
        settings.HaloRadius = settingsJson.value("HaloRadius", settings.HaloRadius);
        settings.HaloFalloff = settingsJson.value("HaloFalloff", settings.HaloFalloff);
        settings.StreakStrength = settingsJson.value("StreakStrength", settings.StreakStrength);
        settings.StreakLength = settingsJson.value("StreakLength", settings.StreakLength);
        settings.ApertureBlades = settingsJson.value("ApertureBlades", settings.ApertureBlades);
        settings.ApertureRotation = settingsJson.value("ApertureRotation", settings.ApertureRotation);
        settings.ChromaticSpread = settingsJson.value("ChromaticSpread", settings.ChromaticSpread);
    }

    json SerializeTimeOfDaySettings(const TimeOfDaySettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "TimeOfDay", settings.TimeOfDay },
            { "Turbidity", settings.Turbidity },
            { "GroundAlbedo", settings.GroundAlbedo },
            { "SunIntensityLux", settings.SunIntensityLux },
            { "SkyIntensityLux", settings.SkyIntensityLux },
            { "OverrideSunColor", settings.OverrideSunColor },
            { "OverrideSkyColor", settings.OverrideSkyColor },
            { "SunColorR", settings.SunColorR },
            { "SunColorG", settings.SunColorG },
            { "SunColorB", settings.SunColorB },
            { "SkyColorR", settings.SkyColorR },
            { "SkyColorG", settings.SkyColorG },
            { "SkyColorB", settings.SkyColorB },
            { "Latitude", settings.Latitude },
            { "NorthOffset", settings.NorthOffset },
            { "NightSkyIntensityLux", settings.NightSkyIntensityLux },
            { "NightSkyColorR", settings.NightSkyColorR },
            { "NightSkyColorG", settings.NightSkyColorG },
            { "NightSkyColorB", settings.NightSkyColorB },
            { "NightDesaturation", settings.NightDesaturation },
            { "MoonEnabled", settings.MoonEnabled },
            { "MoonPhase", settings.MoonPhase },
            { "MoonIntensityLux", settings.MoonIntensityLux },
            { "MoonColorR", settings.MoonColorR },
            { "MoonColorG", settings.MoonColorG },
            { "MoonColorB", settings.MoonColorB },
            { "MoonSize", settings.MoonSize },
            { "MoonDiscBrightness", settings.MoonDiscBrightness },
            { "MoonlitSky", settings.MoonlitSky },
            { "StarsEnabled", settings.StarsEnabled },
            { "StarIntensity", settings.StarIntensity },
            { "StarField", settings.StarField },
            { "ControlExposure", settings.ControlExposure },
            { "DayEv100", settings.DayEv100 },
            { "SunsetEv100", settings.SunsetEv100 },
            { "NightEv100", settings.NightEv100 },
            { "EyeAdaptation", settings.EyeAdaptation },
            { "AdaptationStrength", settings.AdaptationStrength },
            { "AdaptationEv100Min", settings.AdaptationEv100Min },
            { "AdaptationEv100Max", settings.AdaptationEv100Max },
            { "SunGlare", SerializeSkyGlareSettings(settings.SunGlare) },
            { "MoonGlare", SerializeSkyGlareSettings(settings.MoonGlare) }
        };
    }

    void DeserializeTimeOfDaySettings(const json& settingsJson, TimeOfDaySettings& settings)
    {
        // Absent in scenes saved before the toggle existed, which were all time-of-day-on.
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.TimeOfDay = settingsJson.value("TimeOfDay", settings.TimeOfDay);
        settings.Turbidity = settingsJson.value("Turbidity", settings.Turbidity);
        settings.GroundAlbedo = settingsJson.value("GroundAlbedo", settings.GroundAlbedo);
        settings.SunIntensityLux = settingsJson.value("SunIntensityLux", settings.SunIntensityLux);
        settings.SkyIntensityLux = settingsJson.value("SkyIntensityLux", settings.SkyIntensityLux);
        settings.OverrideSunColor = settingsJson.value("OverrideSunColor", settings.OverrideSunColor);
        settings.OverrideSkyColor = settingsJson.value("OverrideSkyColor", settings.OverrideSkyColor);
        settings.SunColorR = settingsJson.value("SunColorR", settings.SunColorR);
        settings.SunColorG = settingsJson.value("SunColorG", settings.SunColorG);
        settings.SunColorB = settingsJson.value("SunColorB", settings.SunColorB);
        settings.SkyColorR = settingsJson.value("SkyColorR", settings.SkyColorR);
        settings.SkyColorG = settingsJson.value("SkyColorG", settings.SkyColorG);
        settings.SkyColorB = settingsJson.value("SkyColorB", settings.SkyColorB);

        // Sun path, night sky, moon and stars. Scenes saved before these existed get
        // the defaults, which reproduce the old noon sun.
        const TimeOfDaySettings defaults{};
        settings.Latitude = settingsJson.value("Latitude", defaults.Latitude);
        settings.NorthOffset = settingsJson.value("NorthOffset", defaults.NorthOffset);
        settings.NightSkyIntensityLux = settingsJson.value("NightSkyIntensityLux", defaults.NightSkyIntensityLux);
        settings.NightSkyColorR = settingsJson.value("NightSkyColorR", defaults.NightSkyColorR);
        settings.NightSkyColorG = settingsJson.value("NightSkyColorG", defaults.NightSkyColorG);
        settings.NightSkyColorB = settingsJson.value("NightSkyColorB", defaults.NightSkyColorB);
        settings.NightDesaturation = settingsJson.value("NightDesaturation", defaults.NightDesaturation);
        settings.MoonEnabled = settingsJson.value("MoonEnabled", defaults.MoonEnabled);
        settings.MoonPhase = settingsJson.value("MoonPhase", defaults.MoonPhase);
        settings.MoonIntensityLux = settingsJson.value("MoonIntensityLux", defaults.MoonIntensityLux);
        settings.MoonColorR = settingsJson.value("MoonColorR", defaults.MoonColorR);
        settings.MoonColorG = settingsJson.value("MoonColorG", defaults.MoonColorG);
        settings.MoonColorB = settingsJson.value("MoonColorB", defaults.MoonColorB);
        settings.MoonSize = settingsJson.value("MoonSize", defaults.MoonSize);
        settings.MoonDiscBrightness = settingsJson.value("MoonDiscBrightness", defaults.MoonDiscBrightness);
        settings.MoonlitSky = settingsJson.value("MoonlitSky", defaults.MoonlitSky);
        settings.StarsEnabled = settingsJson.value("StarsEnabled", defaults.StarsEnabled);
        settings.StarIntensity = settingsJson.value("StarIntensity", defaults.StarIntensity);
        settings.StarField = settingsJson.value("StarField", defaults.StarField);

        settings.ControlExposure = settingsJson.value("ControlExposure", defaults.ControlExposure);
        settings.DayEv100 = settingsJson.value("DayEv100", defaults.DayEv100);
        settings.SunsetEv100 = settingsJson.value("SunsetEv100", defaults.SunsetEv100);
        settings.NightEv100 = settingsJson.value("NightEv100", defaults.NightEv100);
        settings.EyeAdaptation = settingsJson.value("EyeAdaptation", defaults.EyeAdaptation);
        settings.AdaptationStrength = settingsJson.value("AdaptationStrength", defaults.AdaptationStrength);
        settings.AdaptationEv100Min = settingsJson.value("AdaptationEv100Min", defaults.AdaptationEv100Min);
        settings.AdaptationEv100Max = settingsJson.value("AdaptationEv100Max", defaults.AdaptationEv100Max);

        // Scenes saved before the sun and moon glare existed get their defaults.
        settings.SunGlare = defaults.SunGlare;
        settings.MoonGlare = defaults.MoonGlare;
        if (settingsJson.contains("SunGlare"))
            DeserializeSkyGlareSettings(settingsJson["SunGlare"], settings.SunGlare);
        if (settingsJson.contains("MoonGlare"))
            DeserializeSkyGlareSettings(settingsJson["MoonGlare"], settings.MoonGlare);
    }

    json SerializeSsrSettings(const SsrSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Intensity", settings.Intensity },
            { "MaxSteps", settings.MaxSteps },
            { "StepSize", settings.StepSize },
            { "StepGrowth", settings.StepGrowth },
            { "Thickness", settings.Thickness },
            { "MaxRoughness", settings.MaxRoughness },
            { "RefineSteps", settings.RefineSteps },
            { "MaxDistance", settings.MaxDistance },
            { "EdgeFadeStart", settings.EdgeFadeStart },
            { "DebugView", settings.DebugView },
            { "Technique", settings.Technique },
            { "SssrDepthThickness", settings.SssrDepthThickness },
            { "SssrMaxTraversalIntersections", settings.SssrMaxTraversalIntersections },
            { "SssrMinTraversalOccupancy", settings.SssrMinTraversalOccupancy },
            { "SssrMostDetailedMip", settings.SssrMostDetailedMip },
            { "SssrSamplesPerQuad", settings.SssrSamplesPerQuad },
            { "SssrTemporalVarianceGuidedTracing", settings.SssrTemporalVarianceGuidedTracing },
            { "SssrTemporalVarianceThreshold", settings.SssrTemporalVarianceThreshold },
            { "SssrTemporalStability", settings.SssrTemporalStability }
        };
    }

    void DeserializeSsrSettings(const json& settingsJson, SsrSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.MaxSteps = settingsJson.value("MaxSteps", settings.MaxSteps);
        settings.StepSize = settingsJson.value("StepSize", settings.StepSize);
        settings.StepGrowth = settingsJson.value("StepGrowth", settings.StepGrowth);
        settings.Thickness = settingsJson.value("Thickness", settings.Thickness);
        settings.MaxRoughness = settingsJson.value("MaxRoughness", settings.MaxRoughness);
        settings.RefineSteps = settingsJson.value("RefineSteps", settings.RefineSteps);
        settings.MaxDistance = settingsJson.value("MaxDistance", settings.MaxDistance);
        settings.EdgeFadeStart = settingsJson.value("EdgeFadeStart", settings.EdgeFadeStart);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
        settings.Technique = settingsJson.value("Technique", settings.Technique);
        settings.SssrDepthThickness = settingsJson.value("SssrDepthThickness", settings.SssrDepthThickness);
        settings.SssrMaxTraversalIntersections = settingsJson.value("SssrMaxTraversalIntersections", settings.SssrMaxTraversalIntersections);
        settings.SssrMinTraversalOccupancy = settingsJson.value("SssrMinTraversalOccupancy", settings.SssrMinTraversalOccupancy);
        settings.SssrMostDetailedMip = settingsJson.value("SssrMostDetailedMip", settings.SssrMostDetailedMip);
        settings.SssrSamplesPerQuad = settingsJson.value("SssrSamplesPerQuad", settings.SssrSamplesPerQuad);
        settings.SssrTemporalVarianceGuidedTracing = settingsJson.value("SssrTemporalVarianceGuidedTracing", settings.SssrTemporalVarianceGuidedTracing);
        settings.SssrTemporalVarianceThreshold = settingsJson.value("SssrTemporalVarianceThreshold", settings.SssrTemporalVarianceThreshold);
        settings.SssrTemporalStability = settingsJson.value("SssrTemporalStability", settings.SssrTemporalStability);
    }

    json SerializeRadianceProbeSettings(const RadianceProbeSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "GridX", settings.GridX },
            { "GridY", settings.GridY },
            { "GridZ", settings.GridZ },
            { "Spacing", settings.Spacing },
            { "CascadeCount", settings.CascadeCount },
            { "OriginX", settings.OriginX },
            { "OriginY", settings.OriginY },
            { "OriginZ", settings.OriginZ },
            { "FollowCamera", settings.FollowCamera },
            { "RaysPerProbe", settings.RaysPerProbe },
            { "TraceResolution", settings.TraceResolution },
            { "UpdateBlend", settings.UpdateBlend },
            { "VsmVisibility", settings.VsmVisibility },
            { "VsmMaxTexelSize", settings.VsmMaxTexelSize },
            { "StochasticLights", settings.StochasticLights },
            { "LightSamples", settings.LightSamples },
            { "GiIntensity", settings.GiIntensity },
            { "SpecularEnabled", settings.SpecularEnabled },
            { "SpecularIntensity", settings.SpecularIntensity }
        };
    }

    // The debug spheres are left out on purpose, like the other passes' overlays.
    // Ranges match what the renderer and the editor accept.
    void DeserializeRadianceProbeSettings(const json& settingsJson, RadianceProbeSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.GridX = std::clamp(settingsJson.value("GridX", settings.GridX), 1, 64);
        settings.GridY = std::clamp(settingsJson.value("GridY", settings.GridY), 1, 64);
        settings.GridZ = std::clamp(settingsJson.value("GridZ", settings.GridZ), 1, 64);
        settings.Spacing = std::clamp(settingsJson.value("Spacing", settings.Spacing), 0.1f, 100.0f);
        settings.CascadeCount = std::clamp(settingsJson.value("CascadeCount", settings.CascadeCount), 1, kMaxRadianceProbeCascades);
        settings.OriginX = settingsJson.value("OriginX", settings.OriginX);
        settings.OriginY = settingsJson.value("OriginY", settings.OriginY);
        settings.OriginZ = settingsJson.value("OriginZ", settings.OriginZ);
        settings.FollowCamera = settingsJson.value("FollowCamera", settings.FollowCamera);
        settings.RaysPerProbe = std::clamp(settingsJson.value("RaysPerProbe", settings.RaysPerProbe), 1, 1024);
        settings.TraceResolution = std::clamp(settingsJson.value("TraceResolution", settings.TraceResolution), 1, 4);
        settings.UpdateBlend = std::clamp(settingsJson.value("UpdateBlend", settings.UpdateBlend), 0.0f, 1.0f);
        settings.VsmVisibility = settingsJson.value("VsmVisibility", settings.VsmVisibility);
        settings.VsmMaxTexelSize = std::clamp(settingsJson.value("VsmMaxTexelSize", settings.VsmMaxTexelSize), 0.005f, 0.25f);
        settings.StochasticLights = settingsJson.value("StochasticLights", settings.StochasticLights);
        settings.LightSamples = std::clamp(settingsJson.value("LightSamples", settings.LightSamples), 1, 4);
        settings.GiIntensity = std::clamp(settingsJson.value("GiIntensity", settings.GiIntensity), 0.0f, 16.0f);
        settings.SpecularEnabled = settingsJson.value("SpecularEnabled", settings.SpecularEnabled);
        settings.SpecularIntensity = std::clamp(settingsJson.value("SpecularIntensity", settings.SpecularIntensity), 0.0f, 16.0f);
    }

    json SerializeVoxelGiSettings(const VoxelGISettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "FallbackWithoutRayTracing", settings.FallbackWithoutRayTracing },
            { "Resolution", settings.Resolution },
            { "LevelCount", settings.LevelCount },
            { "VoxelSize", settings.VoxelSize },
            { "LodBias", settings.LodBias },
            { "SunShadows", settings.SunShadows },
            { "LocalLightShadows", settings.LocalLightShadows },
            { "VsmVisibility", settings.VsmVisibility },
            { "VsmMaxTexelSize", settings.VsmMaxTexelSize },
            { "StochasticLights", settings.StochasticLights },
            { "LightSamples", settings.LightSamples },
            { "MultiBounce", settings.MultiBounce },
            { "TemporalBlend", settings.TemporalBlend },
            { "DiffuseCones", settings.DiffuseCones },
            { "HalfResolution", settings.HalfResolution },
            { "MaxTraceDistance", settings.MaxTraceDistance },
            { "SurfaceOffset", settings.SurfaceOffset },
            { "GiIntensity", settings.GiIntensity },
            { "SpecularEnabled", settings.SpecularEnabled },
            { "SpecularRoughnessThreshold", settings.SpecularRoughnessThreshold },
            { "SpecularIntensity", settings.SpecularIntensity }
        };
    }

    // The debug view is left out on purpose, like the other passes' overlays.
    // Ranges match what the renderer and the editor accept.
    void DeserializeVoxelGiSettings(const json& settingsJson, VoxelGISettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.FallbackWithoutRayTracing = settingsJson.value("FallbackWithoutRayTracing", settings.FallbackWithoutRayTracing);
        settings.Resolution = std::clamp(settingsJson.value("Resolution", settings.Resolution), 32, 128);
        settings.LevelCount = std::clamp(settingsJson.value("LevelCount", settings.LevelCount), 2, 6);
        settings.VoxelSize = std::clamp(settingsJson.value("VoxelSize", settings.VoxelSize), 0.05f, 2.0f);
        settings.LodBias = std::clamp(settingsJson.value("LodBias", settings.LodBias), 0, 8);
        settings.SunShadows = settingsJson.value("SunShadows", settings.SunShadows);
        settings.LocalLightShadows = settingsJson.value("LocalLightShadows", settings.LocalLightShadows);
        settings.VsmVisibility = settingsJson.value("VsmVisibility", settings.VsmVisibility);
        settings.VsmMaxTexelSize = std::clamp(settingsJson.value("VsmMaxTexelSize", settings.VsmMaxTexelSize), 0.005f, 0.5f);
        settings.StochasticLights = settingsJson.value("StochasticLights", settings.StochasticLights);
        settings.LightSamples = std::clamp(settingsJson.value("LightSamples", settings.LightSamples), 1, 4);
        settings.MultiBounce = std::clamp(settingsJson.value("MultiBounce", settings.MultiBounce), 0.0f, 1.0f);
        settings.TemporalBlend = std::clamp(settingsJson.value("TemporalBlend", settings.TemporalBlend), 0.01f, 1.0f);
        settings.DiffuseCones = std::clamp(settingsJson.value("DiffuseCones", settings.DiffuseCones), 1, 9);
        settings.HalfResolution = settingsJson.value("HalfResolution", settings.HalfResolution);
        settings.MaxTraceDistance = std::clamp(settingsJson.value("MaxTraceDistance", settings.MaxTraceDistance), 1.0f, 1000.0f);
        settings.SurfaceOffset = std::clamp(settingsJson.value("SurfaceOffset", settings.SurfaceOffset), 0.0f, 4.0f);
        settings.GiIntensity = std::clamp(settingsJson.value("GiIntensity", settings.GiIntensity), 0.0f, 16.0f);
        settings.SpecularEnabled = settingsJson.value("SpecularEnabled", settings.SpecularEnabled);
        settings.SpecularRoughnessThreshold = std::clamp(settingsJson.value("SpecularRoughnessThreshold", settings.SpecularRoughnessThreshold), 0.0f, 1.0f);
        settings.SpecularIntensity = std::clamp(settingsJson.value("SpecularIntensity", settings.SpecularIntensity), 0.0f, 16.0f);
    }

    json SerializeSubsurfaceSettings(const SubsurfaceSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Mode", settings.Mode },
            { "Quality", settings.Quality },
            { "FollowSurface", settings.FollowSurface },
            { "Transmission", settings.Transmission },
            { "TransmissionIntensity", settings.TransmissionIntensity },
            { "RtSamples", settings.RtSamples },
            { "RtRestir", settings.RtRestir },
            { "RtHistoryLength", settings.RtHistoryLength }
        };
    }

    // The debug view is left out on purpose, like the other passes' overlays.
    void DeserializeSubsurfaceSettings(const json& settingsJson, SubsurfaceSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Mode = std::clamp(settingsJson.value("Mode", settings.Mode), 0, 1);
        settings.Quality = std::clamp(settingsJson.value("Quality", settings.Quality), 0, 2);
        settings.FollowSurface = settingsJson.value("FollowSurface", settings.FollowSurface);
        settings.Transmission = settingsJson.value("Transmission", settings.Transmission);
        settings.TransmissionIntensity = std::clamp(settingsJson.value("TransmissionIntensity", settings.TransmissionIntensity), 0.0f, 10.0f);
        settings.RtSamples = std::clamp(settingsJson.value("RtSamples", settings.RtSamples), 1, 64);
        settings.RtRestir = settingsJson.value("RtRestir", settings.RtRestir);
        settings.RtHistoryLength = std::clamp(settingsJson.value("RtHistoryLength", settings.RtHistoryLength), 1, 64);
    }

    json SerializeShadingSettings(const ShadingSettings& settings)
    {
        return json{
            { "DisneyBrdf", settings.DisneyBrdf }
        };
    }

    void DeserializeShadingSettings(const json& settingsJson, ShadingSettings& settings)
    {
        settings.DisneyBrdf = settingsJson.value("DisneyBrdf", settings.DisneyBrdf);
    }

    json SerializeDpleSettings(const DpleSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "WorkingResolutionDivisor", settings.WorkingResolutionDivisor },
            { "EnableAmbientOcclusion", settings.EnableAmbientOcclusion },
            { "MicroAORadius", settings.MicroAORadius },
            { "MicroAOIntensity", settings.MicroAOIntensity },
            { "ContactAORadius", settings.ContactAORadius },
            { "ContactAOIntensity", settings.ContactAOIntensity },
            { "BroadAORadius", settings.BroadAORadius },
            { "BroadAOIntensity", settings.BroadAOIntensity },
            { "MaxCombinedAO", settings.MaxCombinedAO },
            { "AOPower", settings.AOPower },
            { "AOBias", settings.AOBias },
            { "EnableContactShadows", settings.EnableContactShadows },
            { "ContactShadowLength", settings.ContactShadowLength },
            { "ContactShadowSteps", settings.ContactShadowSteps },
            { "ContactShadowThickness", settings.ContactShadowThickness },
            { "ContactShadowIntensity", settings.ContactShadowIntensity },
            { "EnableMaterialResponse", settings.EnableMaterialResponse },
            { "SkinAOScale", settings.SkinAOScale },
            { "SkinWarmth", settings.SkinWarmth },
            { "FoliageAOScale", settings.FoliageAOScale },
            { "FoliageSaturation", settings.FoliageSaturation },
            { "WetRoughnessThreshold", settings.WetRoughnessThreshold },
            { "WetResponseStrength", settings.WetResponseStrength },
            { "EnableMicroSpecular", settings.EnableMicroSpecular },
            { "SpecularOcclusionStrength", settings.SpecularOcclusionStrength },
            { "MicroSpecularStrength", settings.MicroSpecularStrength },
            { "MicroSpecularDetailScale", settings.MicroSpecularDetailScale },
            { "MicroSpecularRoughnessMax", settings.MicroSpecularRoughnessMax },
            { "IndirectFractionMin", settings.IndirectFractionMin },
            { "IndirectFractionMax", settings.IndirectFractionMax },
            { "DirectLightWeight", settings.DirectLightWeight },
            { "EnableTemporalReconstruction", settings.EnableTemporalReconstruction },
            { "HistoryWeightStable", settings.HistoryWeightStable },
            { "HistoryWeightMoving", settings.HistoryWeightMoving },
            { "DisocclusionDepthTolerance", settings.DisocclusionDepthTolerance },
            { "NeighborhoodClampScale", settings.NeighborhoodClampScale },
            { "EnableSpatialDenoise", settings.EnableSpatialDenoise },
            { "SpatialDenoiseRadius", settings.SpatialDenoiseRadius },
            { "EnableDetailEnhancement", settings.EnableDetailEnhancement },
            { "FineDetailStrength", settings.FineDetailStrength },
            { "StructureStrength", settings.StructureStrength },
            { "MaxLuminanceChange", settings.MaxLuminanceChange },
            { "DetailMotionSuppression", settings.DetailMotionSuppression }
        };
    }

    // The debug view is left out on purpose, like the other passes' overlays. Ranges match
    // the sliders in the DPLE window.
    void DeserializeDpleSettings(const json& settingsJson, DpleSettings& settings)
    {
        auto readFloat = [&](const char* key, float& value, float minValue, float maxValue)
        {
            value = std::clamp(settingsJson.value(key, value), minValue, maxValue);
        };
        auto readInt = [&](const char* key, int& value, int minValue, int maxValue)
        {
            value = std::clamp(settingsJson.value(key, value), minValue, maxValue);
        };
        auto readBool = [&](const char* key, bool& value)
        {
            value = settingsJson.value(key, value);
        };

        readBool("Enabled", settings.Enabled);
        readInt("WorkingResolutionDivisor", settings.WorkingResolutionDivisor, 1, 4);

        readBool("EnableAmbientOcclusion", settings.EnableAmbientOcclusion);
        readFloat("MicroAORadius", settings.MicroAORadius, 0.005f, 0.3f);
        readFloat("MicroAOIntensity", settings.MicroAOIntensity, 0.0f, 1.0f);
        readFloat("ContactAORadius", settings.ContactAORadius, 0.01f, 1.0f);
        readFloat("ContactAOIntensity", settings.ContactAOIntensity, 0.0f, 1.0f);
        readFloat("BroadAORadius", settings.BroadAORadius, 0.1f, 5.0f);
        readFloat("BroadAOIntensity", settings.BroadAOIntensity, 0.0f, 1.0f);
        readFloat("MaxCombinedAO", settings.MaxCombinedAO, 0.0f, 1.0f);
        readFloat("AOPower", settings.AOPower, 0.1f, 3.0f);
        readFloat("AOBias", settings.AOBias, 0.0f, 0.2f);

        readBool("EnableContactShadows", settings.EnableContactShadows);
        readFloat("ContactShadowLength", settings.ContactShadowLength, 0.01f, 2.0f);
        readInt("ContactShadowSteps", settings.ContactShadowSteps, 4, 32);
        readFloat("ContactShadowThickness", settings.ContactShadowThickness, 0.001f, 0.2f);
        readFloat("ContactShadowIntensity", settings.ContactShadowIntensity, 0.0f, 1.0f);

        readBool("EnableMaterialResponse", settings.EnableMaterialResponse);
        readFloat("SkinAOScale", settings.SkinAOScale, 0.0f, 2.0f);
        readFloat("SkinWarmth", settings.SkinWarmth, 0.0f, 1.0f);
        readFloat("FoliageAOScale", settings.FoliageAOScale, 0.0f, 2.0f);
        readFloat("FoliageSaturation", settings.FoliageSaturation, 0.0f, 1.0f);
        readFloat("WetRoughnessThreshold", settings.WetRoughnessThreshold, 0.0f, 0.5f);
        readFloat("WetResponseStrength", settings.WetResponseStrength, 0.0f, 1.0f);

        readBool("EnableMicroSpecular", settings.EnableMicroSpecular);
        readFloat("SpecularOcclusionStrength", settings.SpecularOcclusionStrength, 0.0f, 1.0f);
        readFloat("MicroSpecularStrength", settings.MicroSpecularStrength, 0.0f, 4.0f);
        readFloat("MicroSpecularDetailScale", settings.MicroSpecularDetailScale, 0.5f, 4.0f);
        readFloat("MicroSpecularRoughnessMax", settings.MicroSpecularRoughnessMax, 0.05f, 1.0f);

        readFloat("IndirectFractionMin", settings.IndirectFractionMin, 0.0f, 1.0f);
        readFloat("IndirectFractionMax", settings.IndirectFractionMax, 0.0f, 1.0f);
        readFloat("DirectLightWeight", settings.DirectLightWeight, 0.0f, 1.0f);

        readBool("EnableTemporalReconstruction", settings.EnableTemporalReconstruction);
        readFloat("HistoryWeightStable", settings.HistoryWeightStable, 0.0f, 0.98f);
        readFloat("HistoryWeightMoving", settings.HistoryWeightMoving, 0.0f, 0.98f);
        readFloat("DisocclusionDepthTolerance", settings.DisocclusionDepthTolerance, 0.005f, 0.5f);
        readFloat("NeighborhoodClampScale", settings.NeighborhoodClampScale, 0.5f, 4.0f);
        readBool("EnableSpatialDenoise", settings.EnableSpatialDenoise);
        readInt("SpatialDenoiseRadius", settings.SpatialDenoiseRadius, 0, 4);

        readBool("EnableDetailEnhancement", settings.EnableDetailEnhancement);
        readFloat("FineDetailStrength", settings.FineDetailStrength, 0.0f, 1.0f);
        readFloat("StructureStrength", settings.StructureStrength, 0.0f, 1.0f);
        readFloat("MaxLuminanceChange", settings.MaxLuminanceChange, 0.0f, 1.0f);
        readFloat("DetailMotionSuppression", settings.DetailMotionSuppression, 0.0f, 1.0f);
    }

    json SerializeChromaticAberrationSettings(const ChromaticAberrationSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Strength", settings.Strength },
            { "Falloff", settings.Falloff },
            { "SampleCount", settings.SampleCount },
            { "CenterX", settings.CenterX },
            { "CenterY", settings.CenterY }
        };
    }

    void DeserializeChromaticAberrationSettings(const json& settingsJson, ChromaticAberrationSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Strength = settingsJson.value("Strength", settings.Strength);
        settings.Falloff = settingsJson.value("Falloff", settings.Falloff);
        settings.SampleCount = settingsJson.value("SampleCount", settings.SampleCount);
        settings.CenterX = settingsJson.value("CenterX", settings.CenterX);
        settings.CenterY = settingsJson.value("CenterY", settings.CenterY);
    }

    json SerializeTaaSettings(const TaaSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Quality", settings.Quality },
            { "CurrentFrameWeight", settings.CurrentFrameWeight },
            { "Samples", settings.Samples },
            { "FilterSize", settings.FilterSize },
            { "CatmullRomFilter", settings.CatmullRomFilter },
            { "JitterScale", settings.JitterScale },
            { "Upscaling", settings.Upscaling },
            { "ScreenPercentage", settings.ScreenPercentage },
            { "BlendFactor", settings.BlendFactor }
        };
    }

    void DeserializeTaaSettings(const json& settingsJson, TaaSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        // Levels saved before the Unreal TAA keep their old BlendFactor, which
        // meant something else; they get Unreal's defaults for the new controls.
        settings.Quality = settingsJson.value("Quality", settings.Quality);
        settings.CurrentFrameWeight = settingsJson.value("CurrentFrameWeight", settings.CurrentFrameWeight);
        settings.Samples = settingsJson.value("Samples", settings.Samples);
        settings.FilterSize = settingsJson.value("FilterSize", settings.FilterSize);
        settings.CatmullRomFilter = settingsJson.value("CatmullRomFilter", settings.CatmullRomFilter);
        settings.Upscaling = settingsJson.value("Upscaling", settings.Upscaling);
        settings.ScreenPercentage = settingsJson.value("ScreenPercentage", settings.ScreenPercentage);
        settings.BlendFactor = settingsJson.value("BlendFactor", settings.BlendFactor);
        settings.JitterScale = settingsJson.value("JitterScale", settings.JitterScale);
        settings.UseSharpening = settingsJson.value("UseSharpening", settings.UseSharpening);
        settings.SharpeningStrength = settingsJson.value("SharpeningStrength", settings.SharpeningStrength);
        settings.Validate();
        settings.ResetHistory = true;
    }

    json SerializeSharpenSettings(const SharpenSettings& settings)
    {
        return json{
            { "ImageSharpeningEnabled", settings.ImageSharpeningEnabled },
            { "ImageSharpeningStrength", settings.ImageSharpeningStrength },
            { "ImageSharpeningRadius", settings.ImageSharpeningRadius },
            { "TextureSharpeningEnabled", settings.TextureSharpeningEnabled },
            { "TextureMipLODBias", settings.TextureMipLODBias }
        };
    }

    void DeserializeSharpenSettings(const json& settingsJson, SharpenSettings& settings)
    {
        settings.ImageSharpeningEnabled = settingsJson.value("ImageSharpeningEnabled", settings.ImageSharpeningEnabled);
        settings.ImageSharpeningStrength = settingsJson.value("ImageSharpeningStrength", settings.ImageSharpeningStrength);
        settings.ImageSharpeningRadius = settingsJson.value("ImageSharpeningRadius", settings.ImageSharpeningRadius);
        settings.TextureSharpeningEnabled = settingsJson.value("TextureSharpeningEnabled", settings.TextureSharpeningEnabled);
        settings.TextureMipLODBias = settingsJson.value("TextureMipLODBias", settings.TextureMipLODBias);
        settings.Validate();
    }

    json SerializeSmaaSettings(const SMAASettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "EdgeThreshold", settings.EdgeThreshold },
            { "MaxSearchSteps", settings.MaxSearchSteps },
            { "MaxSearchStepsDiag", settings.MaxSearchStepsDiag },
            { "CornerRounding", settings.CornerRounding },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeSmaaSettings(const json& settingsJson, SMAASettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.EdgeThreshold = settingsJson.value("EdgeThreshold", settings.EdgeThreshold);
        settings.MaxSearchSteps = settingsJson.value("MaxSearchSteps", settings.MaxSearchSteps);
        settings.MaxSearchStepsDiag = settingsJson.value("MaxSearchStepsDiag", settings.MaxSearchStepsDiag);
        settings.CornerRounding = settingsJson.value("CornerRounding", settings.CornerRounding);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    json SerializeAnalyticEdgeAaSettings(const AnalyticEdgeAASettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "DepthThreshold", settings.DepthThreshold },
            { "CreaseAngleDegrees", settings.CreaseAngleDegrees },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeAnalyticEdgeAaSettings(const json& settingsJson, AnalyticEdgeAASettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.DepthThreshold = settingsJson.value("DepthThreshold", settings.DepthThreshold);
        settings.CreaseAngleDegrees = settingsJson.value("CreaseAngleDegrees", settings.CreaseAngleDegrees);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
        settings.Validate();
    }

    json SerializeAarSettings(const AarSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Mode", settings.Mode },
            { "DepthThreshold", settings.DepthThreshold },
            { "CreaseAngleDegrees", settings.CreaseAngleDegrees },
            { "DepthSigma", settings.DepthSigma },
            { "NormalPower", settings.NormalPower },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeAarSettings(const json& settingsJson, AarSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Mode = settingsJson.value("Mode", settings.Mode);
        settings.DepthThreshold = settingsJson.value("DepthThreshold", settings.DepthThreshold);
        settings.CreaseAngleDegrees = settingsJson.value("CreaseAngleDegrees", settings.CreaseAngleDegrees);
        settings.DepthSigma = settingsJson.value("DepthSigma", settings.DepthSigma);
        settings.NormalPower = settingsJson.value("NormalPower", settings.NormalPower);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
        settings.Validate();
    }

    json SerializeDlssSettings(const DlssSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Mode", settings.Mode }
        };
    }

    void DeserializeDlssSettings(const json& settingsJson, DlssSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Mode = settingsJson.value("Mode", settings.Mode);
        settings.ResetHistory = true;
    }

    // Debug overlays are left out on purpose: they are for a session, not a level.
    json SerializeFsrSettings(const FsrSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Mode", settings.Mode },
            { "Sharpening", settings.Sharpening },
            { "Sharpness", settings.Sharpness },
            { "FrameGeneration", settings.FrameGeneration }
        };
    }

    void DeserializeFsrSettings(const json& settingsJson, FsrSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Mode = std::clamp(settingsJson.value("Mode", settings.Mode), 0, 4);
        settings.Sharpening = settingsJson.value("Sharpening", settings.Sharpening);
        settings.Sharpness = std::clamp(settingsJson.value("Sharpness", settings.Sharpness), 0.0f, 1.0f);
        settings.FrameGeneration = settingsJson.value("FrameGeneration", settings.FrameGeneration);
        settings.ResetHistory = true;
    }

    json SerializeXessSettings(const XessSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Mode", settings.Mode },
            { "FrameGeneration", settings.FrameGeneration },
            { "InterpolatedFrames", settings.InterpolatedFrames },
            { "UiComposition", settings.UiComposition },
            { "LowLatency", settings.LowLatency }
        };
    }

    void DeserializeXessSettings(const json& settingsJson, XessSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Mode = std::clamp(settingsJson.value("Mode", settings.Mode), 0, 6);
        settings.FrameGeneration = settingsJson.value("FrameGeneration", settings.FrameGeneration);
        settings.InterpolatedFrames = std::clamp(settingsJson.value("InterpolatedFrames", settings.InterpolatedFrames), 1, 8);
        settings.UiComposition = settingsJson.value("UiComposition", settings.UiComposition);
        settings.LowLatency = settingsJson.value("LowLatency", settings.LowLatency);
        settings.ResetHistory = true;
    }

    json SerializeGlobalIlluminationMode(GlobalIlluminationMode mode)
    {
        return json(static_cast<int>(mode));
    }

    void DeserializeGlobalIlluminationMode(const json& modeJson, GlobalIlluminationMode& mode)
    {
        const int serializedMode = modeJson.get<int>();
        switch (serializedMode)
        {
        case static_cast<int>(GlobalIlluminationMode::Disabled):
            mode = GlobalIlluminationMode::Disabled;
            break;
        case static_cast<int>(GlobalIlluminationMode::RadianceCascades):
            mode = GlobalIlluminationMode::RadianceCascades;
            break;
        case static_cast<int>(GlobalIlluminationMode::Rtgi):
        default:
            mode = GlobalIlluminationMode::Rtgi;
            break;
        }
    }

    json SerializeRtgiSettings(const RtGISettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "UseNrdDenoiser", settings.UseNrdDenoiser },
            { "RaysPerPixel", settings.RaysPerPixel },
            { "DiffuseResolution", settings.DiffuseResolution },
            { "SpecularResolution", settings.SpecularResolution },
            { "MaxBounces", settings.MaxBounces },
            { "NextEventEstimation", settings.NextEventEstimation },
            { "VsmVisibility", settings.VsmVisibility },
            { "VsmMaxTexelSize", settings.VsmMaxTexelSize },
            { "StochasticLights", settings.StochasticLights },
            { "LightSamples", settings.LightSamples },
            { "VirtualGeometryBlas", settings.VirtualGeometryBlas },
            { "BlasLodError", settings.BlasLodError },
            { "BlasTierDistance", settings.BlasTierDistance },
            { "BlasTierCount", settings.BlasTierCount },
            { "TemporalReuseEnabled", settings.TemporalReuseEnabled },
            { "MaxHistoryLength", settings.MaxHistoryLength },
            { "DepthThreshold", settings.DepthThreshold },
            { "NormalThreshold", settings.NormalThreshold },
            { "SpatialReuseEnabled", settings.SpatialReuseEnabled },
            { "SpatialSamples", settings.SpatialSamples },
            { "SpatialRadius", settings.SpatialRadius },
            { "RadianceClamp", settings.RadianceClamp },
            { "AccumulationBlend", settings.AccumulationBlend },
            { "NrdMaxAccumulationTime", settings.NrdMaxAccumulationTime },
            { "NrdDisocclusionThreshold", settings.NrdDisocclusionThreshold },
            { "NrdAtrousIterations", settings.NrdAtrousIterations },
            { "NrdSharpenAmount", settings.NrdSharpenAmount },
            { "GiIntensity", settings.GiIntensity },
            { "ColorLeakIntensity", settings.ColorLeakIntensity },
            { "SpecularEnabled", settings.SpecularEnabled },
            { "SpecularRoughnessThreshold", settings.SpecularRoughnessThreshold },
            { "SpecularIntensity", settings.SpecularIntensity },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeRtgiSettings(const json& settingsJson, RtGISettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.UseNrdDenoiser = settingsJson.value("UseNrdDenoiser", settings.UseNrdDenoiser);
        settings.RaysPerPixel = settingsJson.value("RaysPerPixel", settings.RaysPerPixel);
        settings.DiffuseResolution = settingsJson.value("DiffuseResolution", settings.DiffuseResolution);
        settings.SpecularResolution = settingsJson.value("SpecularResolution", settings.SpecularResolution);
        settings.MaxBounces = settingsJson.value("MaxBounces", settings.MaxBounces);
        settings.NextEventEstimation = settingsJson.value("NextEventEstimation", settings.NextEventEstimation);
        settings.VsmVisibility = settingsJson.value("VsmVisibility", settings.VsmVisibility);
        settings.VsmMaxTexelSize = settingsJson.value("VsmMaxTexelSize", settings.VsmMaxTexelSize);
        settings.StochasticLights = settingsJson.value("StochasticLights", settings.StochasticLights);
        settings.LightSamples = settingsJson.value("LightSamples", settings.LightSamples);
        settings.VirtualGeometryBlas = settingsJson.value("VirtualGeometryBlas", settings.VirtualGeometryBlas);
        settings.BlasLodError = settingsJson.value("BlasLodError", settings.BlasLodError);
        settings.BlasTierDistance = settingsJson.value("BlasTierDistance", settings.BlasTierDistance);
        settings.BlasTierCount = settingsJson.value("BlasTierCount", settings.BlasTierCount);
        settings.TemporalReuseEnabled = settingsJson.value("TemporalReuseEnabled", settings.TemporalReuseEnabled);
        settings.MaxHistoryLength = settingsJson.value("MaxHistoryLength", settings.MaxHistoryLength);
        settings.DepthThreshold = settingsJson.value("DepthThreshold", settings.DepthThreshold);
        settings.NormalThreshold = settingsJson.value("NormalThreshold", settings.NormalThreshold);
        settings.SpatialReuseEnabled = settingsJson.value("SpatialReuseEnabled", settings.SpatialReuseEnabled);
        settings.SpatialSamples = settingsJson.value("SpatialSamples", settings.SpatialSamples);
        settings.SpatialRadius = settingsJson.value("SpatialRadius", settings.SpatialRadius);
        settings.RadianceClamp = settingsJson.value("RadianceClamp", settings.RadianceClamp);
        settings.AccumulationBlend = settingsJson.value("AccumulationBlend", settings.AccumulationBlend);
        settings.NrdMaxAccumulationTime = settingsJson.value("NrdMaxAccumulationTime", settings.NrdMaxAccumulationTime);
        settings.NrdDisocclusionThreshold = settingsJson.value("NrdDisocclusionThreshold", settings.NrdDisocclusionThreshold);
        settings.NrdAtrousIterations = settingsJson.value("NrdAtrousIterations", settings.NrdAtrousIterations);
        settings.NrdSharpenAmount = settingsJson.value("NrdSharpenAmount", settings.NrdSharpenAmount);
        settings.GiIntensity = settingsJson.value("GiIntensity", settings.GiIntensity);
        settings.ColorLeakIntensity = settingsJson.value("ColorLeakIntensity", settings.ColorLeakIntensity);
        settings.SpecularEnabled = settingsJson.value("SpecularEnabled", settings.SpecularEnabled);
        settings.SpecularRoughnessThreshold = settingsJson.value("SpecularRoughnessThreshold", settings.SpecularRoughnessThreshold);
        settings.SpecularIntensity = settingsJson.value("SpecularIntensity", settings.SpecularIntensity);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    json SerializeRadianceCascadesSettings(const RadianceCascadesSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "CascadeCount", settings.CascadeCount },
            { "ProbeSpacingBase", settings.ProbeSpacingBase },
            { "RaysPerProbe", settings.RaysPerProbe },
            { "RayLengthBase", settings.RayLengthBase },
            { "RayLengthScale", settings.RayLengthScale },
            { "IntervalLengthScale", settings.IntervalLengthScale },
            { "Hysteresis", settings.Hysteresis },
            { "GiIntensity", settings.GiIntensity },
            { "ColorBleedingStrength", settings.ColorBleedingStrength },
            { "SparseProbeTableCapacity", settings.SparseProbeTableCapacity },
            { "SparseProbeCellSize", settings.SparseProbeCellSize },
            { "SparseProbeSearchSteps", settings.SparseProbeSearchSteps },
            { "SparseProbeReuseStrength", settings.SparseProbeReuseStrength },
            { "RayBias", settings.RayBias },
            { "SpatialFilterStrength", settings.SpatialFilterStrength },
            { "HistoryClampScale", settings.HistoryClampScale },
            { "HistoryDepthSensitivity", settings.HistoryDepthSensitivity },
            { "HistoryNormalThreshold", settings.HistoryNormalThreshold },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeRadianceCascadesSettings(const json& settingsJson, RadianceCascadesSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.CascadeCount = settingsJson.value("CascadeCount", settings.CascadeCount);
        settings.ProbeSpacingBase = settingsJson.value("ProbeSpacingBase", settings.ProbeSpacingBase);
        settings.RaysPerProbe = settingsJson.value("RaysPerProbe", settings.RaysPerProbe);
        settings.RayLengthBase = settingsJson.value("RayLengthBase", settings.RayLengthBase);
        settings.RayLengthScale = settingsJson.value("RayLengthScale", settings.RayLengthScale);
        settings.IntervalLengthScale = settingsJson.value("IntervalLengthScale", settings.IntervalLengthScale);
        settings.Hysteresis = settingsJson.value("Hysteresis", settings.Hysteresis);
        settings.GiIntensity = settingsJson.value("GiIntensity", settings.GiIntensity);
        settings.ColorBleedingStrength = settingsJson.value("ColorBleedingStrength", settings.ColorBleedingStrength);
        settings.SparseProbeTableCapacity = settingsJson.value("SparseProbeTableCapacity", settings.SparseProbeTableCapacity);
        settings.SparseProbeCellSize = settingsJson.value("SparseProbeCellSize", settings.SparseProbeCellSize);
        settings.SparseProbeSearchSteps = settingsJson.value("SparseProbeSearchSteps", settings.SparseProbeSearchSteps);
        settings.SparseProbeReuseStrength = settingsJson.value("SparseProbeReuseStrength", settings.SparseProbeReuseStrength);
        settings.RayBias = settingsJson.value("RayBias", settings.RayBias);
        settings.SpatialFilterStrength = settingsJson.value("SpatialFilterStrength", settings.SpatialFilterStrength);
        settings.HistoryClampScale = settingsJson.value("HistoryClampScale", settings.HistoryClampScale);
        settings.HistoryDepthSensitivity = settingsJson.value("HistoryDepthSensitivity", settings.HistoryDepthSensitivity);
        settings.HistoryNormalThreshold = settingsJson.value("HistoryNormalThreshold", settings.HistoryNormalThreshold);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    json SerializeRtaoSettings(const RtAOSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "RaysPerPixel", settings.RaysPerPixel },
            { "MaxRayLength", settings.MaxRayLength },
            { "RayBias", settings.RayBias },
            { "AOPower", settings.AOPower },
            { "Intensity", settings.Intensity },
            { "LocalLightOcclusion", settings.LocalLightOcclusion },
            { "DebugView", settings.DebugView }
        };
    }

    json SerializeGtaoSettings(const GtaoSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "QualityLevel", settings.QualityLevel },
            { "Radius", settings.Radius },
            { "DenoisePasses", settings.DenoisePasses },
            { "FinalValuePower", settings.FinalValuePower },
            { "FalloffRange", settings.FalloffRange },
            { "SampleDistributionPower", settings.SampleDistributionPower },
            { "ThinOccluderCompensation", settings.ThinOccluderCompensation },
            { "RadiusMultiplier", settings.RadiusMultiplier },
            { "DepthMIPSamplingOffset", settings.DepthMIPSamplingOffset },
            { "Intensity", settings.Intensity },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeRtaoSettings(const json& settingsJson, RtAOSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.RaysPerPixel = settingsJson.value("RaysPerPixel", settings.RaysPerPixel);
        settings.MaxRayLength = settingsJson.value("MaxRayLength", settings.MaxRayLength);
        settings.RayBias = settingsJson.value("RayBias", settings.RayBias);
        settings.AOPower = settingsJson.value("AOPower", settings.AOPower);
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.LocalLightOcclusion = settingsJson.value("LocalLightOcclusion", settings.LocalLightOcclusion);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    void DeserializeGtaoSettings(const json& settingsJson, GtaoSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.QualityLevel = settingsJson.value("QualityLevel", settings.QualityLevel);
        settings.Radius = settingsJson.value("Radius", settings.Radius);
        settings.DenoisePasses = settingsJson.value("DenoisePasses", settings.DenoisePasses);
        settings.FinalValuePower = settingsJson.value("FinalValuePower", settings.FinalValuePower);
        settings.FalloffRange = settingsJson.value("FalloffRange", settings.FalloffRange);
        settings.SampleDistributionPower = settingsJson.value("SampleDistributionPower", settings.SampleDistributionPower);
        settings.ThinOccluderCompensation = settingsJson.value("ThinOccluderCompensation", settings.ThinOccluderCompensation);
        settings.RadiusMultiplier = settingsJson.value("RadiusMultiplier", settings.RadiusMultiplier);
        settings.DepthMIPSamplingOffset = settingsJson.value("DepthMIPSamplingOffset", settings.DepthMIPSamplingOffset);
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    json SerializeAgxSettings(const AgxTonemapSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "Exposure", settings.Exposure },
            { "Ev100", settings.Ev100 },
            { "Ev100Min", settings.Ev100Min },
            { "Ev100Max", settings.Ev100Max },
            { "ExposureMode", static_cast<int>(settings.ExposureMode) },
            { "AutoExposureLowPercent", settings.AutoExposureLowPercent },
            { "AutoExposureHighPercent", settings.AutoExposureHighPercent },
            { "AutoExposureSpeedUp", settings.AutoExposureSpeedUp },
            { "AutoExposureSpeedDown", settings.AutoExposureSpeedDown },
            { "AutoExposureHistogramLogMin", settings.AutoExposureHistogramLogMin },
            { "AutoExposureHistogramLogMax", settings.AutoExposureHistogramLogMax },
            { "AutoExposureGreyPoint", settings.AutoExposureGreyPoint },
            { "AutoExposureMeteringMask", settings.AutoExposureMeteringMask },
            { "ToeStrength", settings.ToeStrength },
            { "ShoulderStrength", settings.ShoulderStrength },
            { "Global", SerializeAgxGradeRegion(settings.Global) },
            { "Shadows", SerializeAgxGradeRegion(settings.Shadows) },
            { "Midtones", SerializeAgxGradeRegion(settings.Midtones) },
            { "Highlights", SerializeAgxGradeRegion(settings.Highlights) }
        };
    }

    json SerializeVolumetricFogSettings(const VolumetricFogSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "FroxelTileSize", settings.FroxelTileSize },
            { "DepthSlices", settings.DepthSlices },
            { "StartDistance", settings.StartDistance },
            { "MaxDistance", settings.MaxDistance },
            { "Density", settings.Density },
            { "Anisotropy", settings.Anisotropy },
            { "ScatteringAlbedo", settings.ScatteringAlbedo },
            { "GiIntensity", settings.GiIntensity },
            { "GiVolume", settings.GiVolume },
            { "GiVolumeCellSize", settings.GiVolumeCellSize },
            { "BaseHeight", settings.BaseHeight },
            { "HeightFalloff", settings.HeightFalloff },
            { "ColorR", settings.ColorR },
            { "ColorG", settings.ColorG },
            { "ColorB", settings.ColorB },
            { "EmissiveColorR", settings.EmissiveColorR },
            { "EmissiveColorG", settings.EmissiveColorG },
            { "EmissiveColorB", settings.EmissiveColorB },
            { "EmissiveIntensity", settings.EmissiveIntensity },
            { "DebugView", settings.DebugView }
        };
    }

    json SerializeVolumetricCloudSettings(const VolumetricCloudSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "PlanetRadiusKm", settings.PlanetRadiusKm },
            { "LayerBottomMeters", settings.LayerBottomMeters },
            { "LayerThicknessMeters", settings.LayerThicknessMeters },
            { "Coverage", settings.Coverage },
            { "CloudType", settings.CloudType },
            { "Density", settings.Density },
            { "BaseNoiseScaleMeters", settings.BaseNoiseScaleMeters },
            { "DetailNoiseScaleMeters", settings.DetailNoiseScaleMeters },
            { "DetailStrength", settings.DetailStrength },
            { "CurlStrength", settings.CurlStrength },
            { "AnvilBias", settings.AnvilBias },
            { "WeatherScaleMeters", settings.WeatherScaleMeters },
            { "CloudTopOffsetMeters", settings.CloudTopOffsetMeters },
            { "WeatherSeed", settings.WeatherSeed },
            { "WeatherCellSize", settings.WeatherCellSize },
            { "WeatherCoverageBias", settings.WeatherCoverageBias },
            { "WeatherTypeBias", settings.WeatherTypeBias },
            { "WindDirectionDegrees", settings.WindDirectionDegrees },
            { "WindSpeed", settings.WindSpeed },
            { "WindSkew", settings.WindSkew },
            { "DetailWindSpeedScale", settings.DetailWindSpeedScale },
            { "ScatteringColorR", settings.ScatteringColorR },
            { "ScatteringColorG", settings.ScatteringColorG },
            { "ScatteringColorB", settings.ScatteringColorB },
            { "ExtinctionScale", settings.ExtinctionScale },
            { "PhaseG0", settings.PhaseG0 },
            { "PhaseG1", settings.PhaseG1 },
            { "PhaseBlend", settings.PhaseBlend },
            { "PowderStrength", settings.PowderStrength },
            { "MultiScatterOctaves", settings.MultiScatterOctaves },
            { "MsScatterFalloff", settings.MsScatterFalloff },
            { "MsExtinctionFalloff", settings.MsExtinctionFalloff },
            { "MsPhaseFalloff", settings.MsPhaseFalloff },
            { "SunIntensityScale", settings.SunIntensityScale },
            { "AmbientIntensityScale", settings.AmbientIntensityScale },
            { "GroundBounceScale", settings.GroundBounceScale },
            { "MaxSteps", settings.MaxSteps },
            { "LightSteps", settings.LightSteps },
            { "LightMarchDistanceMeters", settings.LightMarchDistanceMeters },
            { "MaxTraceDistanceMeters", settings.MaxTraceDistanceMeters },
            { "DistanceFadeStartMeters", settings.DistanceFadeStartMeters },
            { "DetailFadeDistanceMeters", settings.DetailFadeDistanceMeters },
            { "ShadowConeSpread", settings.ShadowConeSpread },
            { "ShadowStepGrowth", settings.ShadowStepGrowth },
            { "ResolutionDivisor", settings.ResolutionDivisor },
            { "TemporalUpsampling", settings.TemporalUpsampling },
            { "TemporalBlend", settings.TemporalBlend },
            { "DebugView", settings.DebugView }
        };
    }

    void DeserializeVolumetricCloudSettings(const json& settingsJson, VolumetricCloudSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.PlanetRadiusKm = settingsJson.value("PlanetRadiusKm", settings.PlanetRadiusKm);
        settings.LayerBottomMeters = settingsJson.value("LayerBottomMeters", settings.LayerBottomMeters);
        settings.LayerThicknessMeters = settingsJson.value("LayerThicknessMeters", settings.LayerThicknessMeters);
        settings.Coverage = settingsJson.value("Coverage", settings.Coverage);
        settings.CloudType = settingsJson.value("CloudType", settings.CloudType);
        settings.Density = settingsJson.value("Density", settings.Density);
        settings.BaseNoiseScaleMeters = settingsJson.value("BaseNoiseScaleMeters", settings.BaseNoiseScaleMeters);
        settings.DetailNoiseScaleMeters = settingsJson.value("DetailNoiseScaleMeters", settings.DetailNoiseScaleMeters);
        settings.DetailStrength = settingsJson.value("DetailStrength", settings.DetailStrength);
        settings.CurlStrength = settingsJson.value("CurlStrength", settings.CurlStrength);
        settings.AnvilBias = settingsJson.value("AnvilBias", settings.AnvilBias);
        settings.WeatherScaleMeters = settingsJson.value("WeatherScaleMeters", settings.WeatherScaleMeters);
        settings.CloudTopOffsetMeters = settingsJson.value("CloudTopOffsetMeters", settings.CloudTopOffsetMeters);
        settings.WeatherSeed = settingsJson.value("WeatherSeed", settings.WeatherSeed);
        settings.WeatherCellSize = settingsJson.value("WeatherCellSize", settings.WeatherCellSize);
        settings.WeatherCoverageBias = settingsJson.value("WeatherCoverageBias", settings.WeatherCoverageBias);
        settings.WeatherTypeBias = settingsJson.value("WeatherTypeBias", settings.WeatherTypeBias);
        settings.WindDirectionDegrees = settingsJson.value("WindDirectionDegrees", settings.WindDirectionDegrees);
        settings.WindSpeed = settingsJson.value("WindSpeed", settings.WindSpeed);
        settings.WindSkew = settingsJson.value("WindSkew", settings.WindSkew);
        settings.DetailWindSpeedScale = settingsJson.value("DetailWindSpeedScale", settings.DetailWindSpeedScale);
        settings.ScatteringColorR = settingsJson.value("ScatteringColorR", settings.ScatteringColorR);
        settings.ScatteringColorG = settingsJson.value("ScatteringColorG", settings.ScatteringColorG);
        settings.ScatteringColorB = settingsJson.value("ScatteringColorB", settings.ScatteringColorB);
        settings.ExtinctionScale = settingsJson.value("ExtinctionScale", settings.ExtinctionScale);
        settings.PhaseG0 = settingsJson.value("PhaseG0", settings.PhaseG0);
        settings.PhaseG1 = settingsJson.value("PhaseG1", settings.PhaseG1);
        settings.PhaseBlend = settingsJson.value("PhaseBlend", settings.PhaseBlend);
        settings.PowderStrength = settingsJson.value("PowderStrength", settings.PowderStrength);
        settings.MultiScatterOctaves = settingsJson.value("MultiScatterOctaves", settings.MultiScatterOctaves);
        settings.MsScatterFalloff = settingsJson.value("MsScatterFalloff", settings.MsScatterFalloff);
        settings.MsExtinctionFalloff = settingsJson.value("MsExtinctionFalloff", settings.MsExtinctionFalloff);
        settings.MsPhaseFalloff = settingsJson.value("MsPhaseFalloff", settings.MsPhaseFalloff);
        settings.SunIntensityScale = settingsJson.value("SunIntensityScale", settings.SunIntensityScale);
        settings.AmbientIntensityScale = settingsJson.value("AmbientIntensityScale", settings.AmbientIntensityScale);
        settings.GroundBounceScale = settingsJson.value("GroundBounceScale", settings.GroundBounceScale);
        settings.MaxSteps = settingsJson.value("MaxSteps", settings.MaxSteps);
        settings.LightSteps = settingsJson.value("LightSteps", settings.LightSteps);
        settings.LightMarchDistanceMeters = settingsJson.value("LightMarchDistanceMeters", settings.LightMarchDistanceMeters);
        settings.MaxTraceDistanceMeters = settingsJson.value("MaxTraceDistanceMeters", settings.MaxTraceDistanceMeters);
        settings.DistanceFadeStartMeters = settingsJson.value("DistanceFadeStartMeters", settings.DistanceFadeStartMeters);
        settings.DetailFadeDistanceMeters = settingsJson.value("DetailFadeDistanceMeters", settings.DetailFadeDistanceMeters);
        settings.ShadowConeSpread = settingsJson.value("ShadowConeSpread", settings.ShadowConeSpread);
        settings.ShadowStepGrowth = settingsJson.value("ShadowStepGrowth", settings.ShadowStepGrowth);
        settings.ResolutionDivisor = settingsJson.value("ResolutionDivisor", settings.ResolutionDivisor);
        settings.TemporalUpsampling = settingsJson.value("TemporalUpsampling", settings.TemporalUpsampling);
        settings.TemporalBlend = settingsJson.value("TemporalBlend", settings.TemporalBlend);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    void DeserializeAgxSettings(const json& settingsJson, AgxTonemapSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Exposure = settingsJson.value("Exposure", settings.Exposure);

        // Levels saved before AgX got a real exposure control stored Ev100Min /
        // Ev100Max as the log-encoding window, where a default of [-1, 16] was
        // normal. Read as the exposure clamp they are now, that pair would drag
        // the exposure to -1 EV and open the level several stops too dark, so a
        // scene with no "Ev100" key keeps the defaults for all three.
        if (settingsJson.contains("Ev100"))
        {
            settings.Ev100 = settingsJson.value("Ev100", settings.Ev100);
            settings.Ev100Min = settingsJson.value("Ev100Min", settings.Ev100Min);
            settings.Ev100Max = settingsJson.value("Ev100Max", settings.Ev100Max);
        }

        settings.ExposureMode = static_cast<AgxExposureMode>(
            settingsJson.value("ExposureMode", static_cast<int>(settings.ExposureMode)));
        settings.AutoExposureLowPercent = settingsJson.value("AutoExposureLowPercent", settings.AutoExposureLowPercent);
        settings.AutoExposureHighPercent = settingsJson.value("AutoExposureHighPercent", settings.AutoExposureHighPercent);
        settings.AutoExposureSpeedUp = settingsJson.value("AutoExposureSpeedUp", settings.AutoExposureSpeedUp);
        settings.AutoExposureSpeedDown = settingsJson.value("AutoExposureSpeedDown", settings.AutoExposureSpeedDown);
        settings.AutoExposureHistogramLogMin = settingsJson.value("AutoExposureHistogramLogMin", settings.AutoExposureHistogramLogMin);
        settings.AutoExposureHistogramLogMax = settingsJson.value("AutoExposureHistogramLogMax", settings.AutoExposureHistogramLogMax);
        settings.AutoExposureGreyPoint = settingsJson.value("AutoExposureGreyPoint", settings.AutoExposureGreyPoint);
        settings.AutoExposureMeteringMask = settingsJson.value("AutoExposureMeteringMask", settings.AutoExposureMeteringMask);
        settings.ToeStrength = settingsJson.value("ToeStrength", settings.ToeStrength);
        settings.ShoulderStrength = settingsJson.value("ShoulderStrength", settings.ShoulderStrength);
        if (settingsJson.contains("Global")) DeserializeAgxGradeRegion(settingsJson["Global"], settings.Global);
        if (settingsJson.contains("Shadows")) DeserializeAgxGradeRegion(settingsJson["Shadows"], settings.Shadows);
        if (settingsJson.contains("Midtones")) DeserializeAgxGradeRegion(settingsJson["Midtones"], settings.Midtones);
        if (settingsJson.contains("Highlights")) DeserializeAgxGradeRegion(settingsJson["Highlights"], settings.Highlights);
    }

    void DeserializeVolumetricFogSettings(const json& settingsJson, VolumetricFogSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.FroxelTileSize = settingsJson.value("FroxelTileSize", settings.FroxelTileSize);
        settings.DepthSlices = settingsJson.value("DepthSlices", settings.DepthSlices);
        settings.StartDistance = settingsJson.value("StartDistance", settings.StartDistance);
        settings.MaxDistance = settingsJson.value("MaxDistance", settings.MaxDistance);
        settings.Density = settingsJson.value("Density", settings.Density);
        settings.Anisotropy = settingsJson.value("Anisotropy", settings.Anisotropy);
        settings.ScatteringAlbedo = settingsJson.value("ScatteringAlbedo", settings.ScatteringAlbedo);
        settings.GiIntensity = settingsJson.value("GiIntensity", settings.GiIntensity);
        settings.GiVolume = settingsJson.value("GiVolume", settings.GiVolume);
        settings.GiVolumeCellSize = std::clamp(settingsJson.value("GiVolumeCellSize", settings.GiVolumeCellSize), 0.1f, 8.0f);
        settings.BaseHeight = settingsJson.value("BaseHeight", settings.BaseHeight);
        settings.HeightFalloff = settingsJson.value("HeightFalloff", settings.HeightFalloff);
        settings.ColorR = settingsJson.value("ColorR", settings.ColorR);
        settings.ColorG = settingsJson.value("ColorG", settings.ColorG);
        settings.ColorB = settingsJson.value("ColorB", settings.ColorB);
        settings.EmissiveColorR = settingsJson.value("EmissiveColorR", settings.EmissiveColorR);
        settings.EmissiveColorG = settingsJson.value("EmissiveColorG", settings.EmissiveColorG);
        settings.EmissiveColorB = settingsJson.value("EmissiveColorB", settings.EmissiveColorB);
        settings.EmissiveIntensity = settingsJson.value("EmissiveIntensity", settings.EmissiveIntensity);
        settings.DebugView = settingsJson.value("DebugView", settings.DebugView);
    }

    json SerializeBloomSettings(const BloomSettings& settings)
    {
        return json{
            { "Enabled",    settings.Enabled },
            { "Method",     settings.Method == BloomMethod::FftConvolution ? "FFT" : "MipChain" },
            { "Intensity",  settings.Intensity },
            { "Threshold",  settings.Threshold },
            { "Knee",       settings.Knee },
            { "Radius",     settings.Radius },
            { "MipLevels",  settings.MipLevels },
            { "FftResolution",       settings.FftResolution },
            { "FftKernelSize",       settings.FftKernelSize },
            { "FftHaloFalloff",      settings.FftHaloFalloff },
            { "FftHaloStrength",     settings.FftHaloStrength },
            { "FftStreakStrength",   settings.FftStreakStrength },
            { "FftApertureBlades",   settings.FftApertureBlades },
            { "FftApertureRotation", settings.FftApertureRotation },
            { "FftChromaticSpread",  settings.FftChromaticSpread }
        };
    }

    void DeserializeBloomSettings(const json& settingsJson, BloomSettings& settings)
    {
        settings.Enabled   = settingsJson.value("Enabled",   settings.Enabled);
        settings.Method    = settingsJson.value("Method", std::string("MipChain")) == "FFT"
            ? BloomMethod::FftConvolution : BloomMethod::MipChain;
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.Threshold = settingsJson.value("Threshold", settings.Threshold);
        settings.Knee      = settingsJson.value("Knee",      settings.Knee);
        settings.Radius    = settingsJson.value("Radius",    settings.Radius);
        settings.MipLevels = settingsJson.value("MipLevels", settings.MipLevels);
        settings.FftResolution       = settingsJson.value("FftResolution",       settings.FftResolution);
        settings.FftKernelSize       = settingsJson.value("FftKernelSize",       settings.FftKernelSize);
        settings.FftHaloFalloff      = settingsJson.value("FftHaloFalloff",      settings.FftHaloFalloff);
        settings.FftHaloStrength     = settingsJson.value("FftHaloStrength",     settings.FftHaloStrength);
        settings.FftStreakStrength   = settingsJson.value("FftStreakStrength",   settings.FftStreakStrength);
        settings.FftApertureBlades   = settingsJson.value("FftApertureBlades",   settings.FftApertureBlades);
        settings.FftApertureRotation = settingsJson.value("FftApertureRotation", settings.FftApertureRotation);
        settings.FftChromaticSpread  = settingsJson.value("FftChromaticSpread",  settings.FftChromaticSpread);
    }

    json SerializeRtShadowSettings(const RtShadowSettings& settings)
    {
        return json{
            { "Enabled",            settings.Enabled },
            { "LocalLights",        settings.LocalLights },
            { "LocalLightSoftness", settings.LocalLightSoftness },
            { "RaysPerPixel",       settings.RaysPerPixel },
            { "SunAngularDiameter", settings.SunAngularDiameter },
            { "MaxRayLength",       settings.MaxRayLength },
            { "RayBias",            settings.RayBias },
            { "TemporalBlend",      settings.TemporalBlend },
            { "MaxFilterRadius",    settings.MaxFilterRadius }
        };
    }

    void DeserializeRtShadowSettings(const json& settingsJson, RtShadowSettings& settings)
    {
        settings.Enabled            = settingsJson.value("Enabled",            settings.Enabled);
        settings.LocalLights        = settingsJson.value("LocalLights",        settings.LocalLights);
        settings.LocalLightSoftness = settingsJson.value("LocalLightSoftness", settings.LocalLightSoftness);
        settings.RaysPerPixel       = settingsJson.value("RaysPerPixel",       settings.RaysPerPixel);
        settings.SunAngularDiameter = settingsJson.value("SunAngularDiameter", settings.SunAngularDiameter);
        settings.MaxRayLength       = settingsJson.value("MaxRayLength",       settings.MaxRayLength);
        settings.RayBias            = settingsJson.value("RayBias",            settings.RayBias);
        settings.TemporalBlend      = settingsJson.value("TemporalBlend",      settings.TemporalBlend);
        settings.MaxFilterRadius    = settingsJson.value("MaxFilterRadius",    settings.MaxFilterRadius);
    }

    json SerializeLensFlareSettings(const LensFlareSettings& settings)
    {
        return json{
            { "Enabled",                 settings.Enabled },
            { "Lens",                    settings.Lens },
            { "Intensity",               settings.Intensity },
            { "GhostIntensity",          settings.GhostIntensity },
            { "FNumber",                 settings.FNumber },
            { "ApertureBlades",          settings.ApertureBlades },
            { "ApertureRotation",        settings.ApertureRotation },
            { "ApertureRoundness",       settings.ApertureRoundness },
            { "MaxGhosts",               settings.MaxGhosts },
            { "RayGridSize",             settings.RayGridSize },
            { "Wavelengths",             settings.Wavelengths },
            { "StarburstIntensity",      settings.StarburstIntensity },
            { "StarburstSize",           settings.StarburstSize },
            { "SunFlares",               settings.SunFlares },
            { "LocalLightFlares",        settings.LocalLightFlares },
            { "MaxLights",               settings.MaxLights },
            { "LocalLightThreshold",     settings.LocalLightThreshold },
            { "OcclusionDepthTolerance", settings.OcclusionDepthTolerance },
            { "SunCloudOcclusion",       settings.SunCloudOcclusion }
        };
    }

    void DeserializeLensFlareSettings(const json& settingsJson, LensFlareSettings& settings)
    {
        settings.Enabled                 = settingsJson.value("Enabled",                 settings.Enabled);
        settings.Lens                    = settingsJson.value("Lens",                    settings.Lens);
        settings.Intensity               = settingsJson.value("Intensity",               settings.Intensity);
        settings.GhostIntensity          = settingsJson.value("GhostIntensity",          settings.GhostIntensity);
        settings.FNumber                 = settingsJson.value("FNumber",                 settings.FNumber);
        settings.ApertureBlades          = settingsJson.value("ApertureBlades",          settings.ApertureBlades);
        settings.ApertureRotation        = settingsJson.value("ApertureRotation",        settings.ApertureRotation);
        settings.ApertureRoundness       = settingsJson.value("ApertureRoundness",       settings.ApertureRoundness);
        settings.MaxGhosts               = settingsJson.value("MaxGhosts",               settings.MaxGhosts);
        settings.RayGridSize             = settingsJson.value("RayGridSize",             settings.RayGridSize);
        settings.Wavelengths             = settingsJson.value("Wavelengths",             settings.Wavelengths);
        settings.StarburstIntensity      = settingsJson.value("StarburstIntensity",      settings.StarburstIntensity);
        settings.StarburstSize           = settingsJson.value("StarburstSize",           settings.StarburstSize);
        settings.SunFlares               = settingsJson.value("SunFlares",               settings.SunFlares);
        settings.LocalLightFlares        = settingsJson.value("LocalLightFlares",        settings.LocalLightFlares);
        settings.MaxLights               = settingsJson.value("MaxLights",               settings.MaxLights);
        settings.LocalLightThreshold     = settingsJson.value("LocalLightThreshold",     settings.LocalLightThreshold);
        settings.OcclusionDepthTolerance = settingsJson.value("OcclusionDepthTolerance", settings.OcclusionDepthTolerance);
        settings.SunCloudOcclusion       = settingsJson.value("SunCloudOcclusion",       settings.SunCloudOcclusion);
    }
}

void SceneSerializer::Serialize(const std::string& filepath)
{
    if (mScene == nullptr || mScene->Entities == nullptr)
    {
        return;
    }

    json sceneJson;
    // Version 2 switched entity Euler angles from XMMatrixRotationRollPitchYaw's Y-up
    // order to the Z-up order in PteroTransform::ComposeRotation. A scene without this
    // key predates that and has its rotations converted on load.
    sceneJson["FormatVersion"] = kSceneFormatVersion;
    sceneJson["Entities"] = json::array();

    if (mScene->CameraPosition != nullptr)
        sceneJson["CameraPosition"] = SerializeVector3(*mScene->CameraPosition);
    if (mScene->CameraRotation != nullptr)
        sceneJson["CameraRotation"] = SerializeVector3(*mScene->CameraRotation);

    if (mScene->TimeOfDay != nullptr)
        sceneJson["TimeOfDaySettings"] = SerializeTimeOfDaySettings(*mScene->TimeOfDay);
    if (mScene->Taa != nullptr)
        sceneJson["TaaSettings"] = SerializeTaaSettings(*mScene->Taa);
    if (mScene->Smaa != nullptr)
        sceneJson["SMAASettings"] = SerializeSmaaSettings(*mScene->Smaa);
    if (mScene->AnalyticEdgeAa != nullptr)
        sceneJson["AnalyticEdgeAASettings"] = SerializeAnalyticEdgeAaSettings(*mScene->AnalyticEdgeAa);
    if (mScene->Aar != nullptr)
        sceneJson["AarSettings"] = SerializeAarSettings(*mScene->Aar);
    if (mScene->Sharpen != nullptr)
        sceneJson["SharpenSettings"] = SerializeSharpenSettings(*mScene->Sharpen);
    if (mScene->Dlss != nullptr)
        sceneJson["DlssSettings"] = SerializeDlssSettings(*mScene->Dlss);
    if (mScene->Fsr != nullptr)
        sceneJson["FsrSettings"] = SerializeFsrSettings(*mScene->Fsr);
    if (mScene->Xess != nullptr)
        sceneJson["XessSettings"] = SerializeXessSettings(*mScene->Xess);
    if (mScene->GlobalIllumination != nullptr)
        sceneJson["GlobalIlluminationMode"] = SerializeGlobalIlluminationMode(*mScene->GlobalIllumination);
    if (mScene->Rtgi != nullptr)
        sceneJson["RtGISettings"] = SerializeRtgiSettings(*mScene->Rtgi);
    if (mScene->RadianceCascades != nullptr)
        sceneJson["RadianceCascadesSettings"] = SerializeRadianceCascadesSettings(*mScene->RadianceCascades);
    if (mScene->Rtao != nullptr)
        sceneJson["RtAOSettings"] = SerializeRtaoSettings(*mScene->Rtao);
    if (mScene->Gtao != nullptr)
        sceneJson["GTAOSettings"] = SerializeGtaoSettings(*mScene->Gtao);
    if (mScene->Ssr != nullptr)
        sceneJson["SsrSettings"] = SerializeSsrSettings(*mScene->Ssr);
    if (mScene->Subsurface != nullptr)
        sceneJson["SubsurfaceSettings"] = SerializeSubsurfaceSettings(*mScene->Subsurface);
    if (mScene->Shading != nullptr)
        sceneJson["ShadingSettings"] = SerializeShadingSettings(*mScene->Shading);
    if (mScene->Dple != nullptr)
        sceneJson["DpleSettings"] = SerializeDpleSettings(*mScene->Dple);
    if (mScene->RadianceProbes != nullptr)
        sceneJson["RadianceProbeSettings"] = SerializeRadianceProbeSettings(*mScene->RadianceProbes);
    if (mScene->VoxelGi != nullptr)
        sceneJson["VoxelGISettings"] = SerializeVoxelGiSettings(*mScene->VoxelGi);
    if (mScene->ChromaticAberration != nullptr)
        sceneJson["ChromaticAberrationSettings"] = SerializeChromaticAberrationSettings(*mScene->ChromaticAberration);
    if (mScene->Agx != nullptr)
        sceneJson["AgxTonemapSettings"] = SerializeAgxSettings(*mScene->Agx);
    if (mScene->VolumetricFog != nullptr)
        sceneJson["VolumetricFogSettings"] = SerializeVolumetricFogSettings(*mScene->VolumetricFog);

    if (mScene->VolumetricCloud != nullptr)
        sceneJson["VolumetricCloudSettings"] = SerializeVolumetricCloudSettings(*mScene->VolumetricCloud);
    if (mScene->Bloom != nullptr)
        sceneJson["BloomSettings"] = SerializeBloomSettings(*mScene->Bloom);
    if (mScene->LensFlare != nullptr)
        sceneJson["LensFlareSettings"] = SerializeLensFlareSettings(*mScene->LensFlare);
    if (mScene->RtShadows != nullptr)
        sceneJson["RtShadowSettings"] = SerializeRtShadowSettings(*mScene->RtShadows);

    // An empty graph writes nothing, so levels made before visual scripting existed keep
    // round-tripping unchanged.
    if (mScene->NodeGraph != nullptr && !mScene->NodeGraph->IsEmpty())
        sceneJson["NodeGraph"] = json::parse(mScene->NodeGraph->ToJsonString());

    // Node graphs reference entities by id, so every entity must have one on disk.
    EnsureEntityIds(*mScene->Entities);

    for (const Entity& entity : *mScene->Entities)
    {
        json entityJson;
        entityJson["Id"] = entity.Id;
        if (entity.HasNameComponent())
        {
            // The non-intrusive JSON macros on the component types handle conversion to JSON automatically.
            entityJson["NameComponent"] = entity.GetNameComponent();
        }

        if (entity.HasTransformComponent())
        {
            entityJson["TransformComponent"] = entity.GetTransformComponent();
        }

        // Persist mesh and material paths so they survive level save/load cycles.
        if (entity.HasMeshComponent())
        {
            entityJson["MeshComponent"] = *entity.Mesh;
        }

        // Persist all point light properties so lights survive level save/load cycles.
        if (entity.HasPointLightComponent())
        {
            entityJson["PointLightComponent"] = *entity.PointLight;
        }

        if (entity.Camera.has_value())
        {
            entityJson["CameraComponent"] = *entity.Camera;
        }

        if (entity.HasAudioEmitterComponent())
        {
            entityJson["AudioEmitterComponent"] = *entity.AudioEmitter;
        }

        if (entity.HasDecalComponent())
        {
            entityJson["DecalComponent"] = *entity.Decal;
        }

        if (entity.RigidBody.has_value())
        {
            entityJson["RigidBodyComponent"] = *entity.RigidBody;
        }

        if (entity.Animator.has_value())
        {
            entityJson["AnimatorComponent"] = *entity.Animator;
        }

        if (entity.HasRainComponent())
        {
            entityJson["RainComponent"] = *entity.Rain;
        }

        if (entity.HasParticleSystemComponent())
        {
            entityJson["ParticleSystemComponent"] = *entity.ParticleSystem;
        }

        if (entity.HasTerrainComponent())
        {
            entityJson["TerrainComponent"] = *entity.Terrain;
        }

        if (entity.HasWaterComponent())
        {
            entityJson["WaterComponent"] = *entity.Water;
        }

        if (entity.HasVegetationAreaComponent())
        {
            entityJson["VegetationAreaComponent"] = *entity.VegetationArea;
        }

        sceneJson["Entities"].push_back(entityJson);
    }

    std::ofstream outputStream(filepath);
    if (!outputStream)
    {
        return;
    }

    outputStream << sceneJson.dump(4);
}

bool SceneSerializer::Deserialize(const std::string& filepath, ProgressCallback progressCallback, void* userData)
{
    if (mScene == nullptr || mScene->Entities == nullptr)
    {
        return false;
    }

    if (!ReportDeserializeProgress(progressCallback, userData, 0.02f, "Opening scene file..."))
    {
        return false;
    }

    // Through DataFiles, so a packaged game reads the level out of Levels.ppak.
    std::string sceneText;
    if (!DataFiles::ReadText(filepath, sceneText))
    {
        return false;
    }

    json sceneJson;
    try
    {
        if (!ReportDeserializeProgress(progressCallback, userData, 0.08f, "Parsing scene file..."))
        {
            return false;
        }
        sceneJson = json::parse(sceneText);
    }
    catch (...)
    {
        return false;
    }

    if (!sceneJson.contains("Entities") || !sceneJson["Entities"].is_array())
    {
        return false;
    }

    if (!ReportDeserializeProgress(progressCallback, userData, 0.15f, "Loading scene settings..."))
    {
        return false;
    }

    if (mScene->HasCameraPosition != nullptr)
        *mScene->HasCameraPosition = false;
    if (mScene->HasCameraRotation != nullptr)
        *mScene->HasCameraRotation = false;

    if (mScene->CameraPosition != nullptr && sceneJson.contains("CameraPosition"))
    {
        DeserializeVector3(sceneJson["CameraPosition"], *mScene->CameraPosition);
        if (mScene->HasCameraPosition != nullptr)
            *mScene->HasCameraPosition = true;
    }

    if (mScene->CameraRotation != nullptr && sceneJson.contains("CameraRotation"))
    {
        DeserializeVector3(sceneJson["CameraRotation"], *mScene->CameraRotation);
        if (mScene->HasCameraRotation != nullptr)
            *mScene->HasCameraRotation = true;
    }

    if (mScene->TimeOfDay != nullptr && sceneJson.contains("TimeOfDaySettings"))
        DeserializeTimeOfDaySettings(sceneJson["TimeOfDaySettings"], *mScene->TimeOfDay);
    if (mScene->Taa != nullptr && sceneJson.contains("TaaSettings"))
        DeserializeTaaSettings(sceneJson["TaaSettings"], *mScene->Taa);
    if (mScene->Smaa != nullptr && sceneJson.contains("SMAASettings"))
        DeserializeSmaaSettings(sceneJson["SMAASettings"], *mScene->Smaa);
    if (mScene->AnalyticEdgeAa != nullptr && sceneJson.contains("AnalyticEdgeAASettings"))
        DeserializeAnalyticEdgeAaSettings(sceneJson["AnalyticEdgeAASettings"], *mScene->AnalyticEdgeAa);
    if (mScene->Aar != nullptr && sceneJson.contains("AarSettings"))
        DeserializeAarSettings(sceneJson["AarSettings"], *mScene->Aar);
    if (mScene->Sharpen != nullptr && sceneJson.contains("SharpenSettings"))
    {
        DeserializeSharpenSettings(sceneJson["SharpenSettings"], *mScene->Sharpen);
    }
    else if (mScene->Sharpen != nullptr && sceneJson.contains("TaaSettings"))
    {
        const json& taaSettingsJson = sceneJson["TaaSettings"];
        mScene->Sharpen->ImageSharpeningEnabled = taaSettingsJson.value("UseSharpening", mScene->Sharpen->ImageSharpeningEnabled);
        mScene->Sharpen->ImageSharpeningStrength = taaSettingsJson.value("SharpeningStrength", mScene->Sharpen->ImageSharpeningStrength);
        mScene->Sharpen->Validate();
    }
    if (mScene->Dlss != nullptr && sceneJson.contains("DlssSettings"))
        DeserializeDlssSettings(sceneJson["DlssSettings"], *mScene->Dlss);
    if (mScene->Fsr != nullptr && sceneJson.contains("FsrSettings"))
        DeserializeFsrSettings(sceneJson["FsrSettings"], *mScene->Fsr);
    if (mScene->Xess != nullptr && sceneJson.contains("XessSettings"))
        DeserializeXessSettings(sceneJson["XessSettings"], *mScene->Xess);
    if (mScene->GlobalIllumination != nullptr && sceneJson.contains("GlobalIlluminationMode"))
        DeserializeGlobalIlluminationMode(sceneJson["GlobalIlluminationMode"], *mScene->GlobalIllumination);
    if (mScene->Rtgi != nullptr && sceneJson.contains("RtGISettings"))
        DeserializeRtgiSettings(sceneJson["RtGISettings"], *mScene->Rtgi);
    if (mScene->RadianceCascades != nullptr && sceneJson.contains("RadianceCascadesSettings"))
        DeserializeRadianceCascadesSettings(sceneJson["RadianceCascadesSettings"], *mScene->RadianceCascades);
    if (mScene->Rtao != nullptr && sceneJson.contains("RtAOSettings"))
        DeserializeRtaoSettings(sceneJson["RtAOSettings"], *mScene->Rtao);
    if (mScene->Gtao != nullptr && sceneJson.contains("GTAOSettings"))
        DeserializeGtaoSettings(sceneJson["GTAOSettings"], *mScene->Gtao);
    if (mScene->Ssr != nullptr && sceneJson.contains("SsrSettings"))
        DeserializeSsrSettings(sceneJson["SsrSettings"], *mScene->Ssr);
    if (mScene->Subsurface != nullptr && sceneJson.contains("SubsurfaceSettings"))
        DeserializeSubsurfaceSettings(sceneJson["SubsurfaceSettings"], *mScene->Subsurface);
    // Levels saved before the setting existed carry no key and get the default (Disney on).
    if (mScene->Shading != nullptr && sceneJson.contains("ShadingSettings"))
        DeserializeShadingSettings(sceneJson["ShadingSettings"], *mScene->Shading);
    if (mScene->Dple != nullptr && sceneJson.contains("DpleSettings"))
        DeserializeDpleSettings(sceneJson["DpleSettings"], *mScene->Dple);
    if (mScene->RadianceProbes != nullptr && sceneJson.contains("RadianceProbeSettings"))
        DeserializeRadianceProbeSettings(sceneJson["RadianceProbeSettings"], *mScene->RadianceProbes);
    if (mScene->VoxelGi != nullptr && sceneJson.contains("VoxelGISettings"))
        DeserializeVoxelGiSettings(sceneJson["VoxelGISettings"], *mScene->VoxelGi);
    if (mScene->ChromaticAberration != nullptr && sceneJson.contains("ChromaticAberrationSettings"))
        DeserializeChromaticAberrationSettings(sceneJson["ChromaticAberrationSettings"], *mScene->ChromaticAberration);
    if (mScene->Agx != nullptr && sceneJson.contains("AgxTonemapSettings"))
        DeserializeAgxSettings(sceneJson["AgxTonemapSettings"], *mScene->Agx);
    if (mScene->VolumetricFog != nullptr && sceneJson.contains("VolumetricFogSettings"))
        DeserializeVolumetricFogSettings(sceneJson["VolumetricFogSettings"], *mScene->VolumetricFog);

    if (mScene->VolumetricCloud != nullptr && sceneJson.contains("VolumetricCloudSettings"))
        DeserializeVolumetricCloudSettings(sceneJson["VolumetricCloudSettings"], *mScene->VolumetricCloud);
    if (mScene->Bloom != nullptr && sceneJson.contains("BloomSettings"))
        DeserializeBloomSettings(sceneJson["BloomSettings"], *mScene->Bloom);
    if (mScene->LensFlare != nullptr && sceneJson.contains("LensFlareSettings"))
        DeserializeLensFlareSettings(sceneJson["LensFlareSettings"], *mScene->LensFlare);
    if (mScene->RtShadows != nullptr && sceneJson.contains("RtShadowSettings"))
        DeserializeRtShadowSettings(sceneJson["RtShadowSettings"], *mScene->RtShadows);

    if (mScene->NodeGraph != nullptr)
    {
        mScene->NodeGraph->Clear();

        if (sceneJson.contains("NodeGraph"))
        {
            const json& nodeGraphJson = sceneJson["NodeGraph"];
            if (nodeGraphJson.is_string())
            {
                // A path instead of a graph: the level shares a .nodegraph that lives
                // beside it, so several levels can drive the same script.
                const std::filesystem::path graphPath =
                    std::filesystem::path(filepath).parent_path() / nodeGraphJson.get<std::string>();
                mScene->NodeGraph->LoadFromFile(graphPath.string());
            }
            else if (nodeGraphJson.is_object())
            {
                mScene->NodeGraph->FromJsonString(nodeGraphJson.dump());
            }
        }
    }

    mScene->Entities->clear();

    // Scenes written before the Z-up rotation order stored their Euler angles for
    // XMMatrixRotationRollPitchYaw and have to be converted as they load.
    const int sceneFormatVersion = sceneJson.value("FormatVersion", 1);
    const bool needsLegacyRotationConversion = sceneFormatVersion < 2;

    const json& entitiesJson = sceneJson["Entities"];
    const size_t entityCount = entitiesJson.size();

    if (entityCount == 0)
    {
        ReportDeserializeProgress(progressCallback, userData, 1.0f, "Loaded empty scene.");
        return true;
    }

    size_t entityIndex = 0;
    try
    {
        for (const json& entityJson : entitiesJson)
        {
            Entity& entity = mScene->CreateEntity();
            // Levels saved before entity ids existed have none; EnsureEntityIds below
            // assigns them, and the next save keeps them.
            entity.Id = entityJson.value("Id", std::uint64_t(0));

            if (entityJson.contains("NameComponent"))
            {
                entity.AddNameComponent(entityJson["NameComponent"].get<NameComponent>());
            }

            if (entityJson.contains("TransformComponent"))
            {
                TransformComponent transform = entityJson["TransformComponent"].get<TransformComponent>();
                if (needsLegacyRotationConversion)
                {
                    // Re-express the saved angles in the current order so the entity keeps
                    // the orientation it was authored with.
                    transform.Rotation = PteroTransform::ConvertLegacyRotation(transform.Rotation);
                }
                entity.AddTransformComponent(transform);
            }

            // Restore mesh/material paths; the asset handle will be lazily loaded on the next render frame.
            if (entityJson.contains("MeshComponent"))
            {
                entity.AddMeshComponent() = entityJson["MeshComponent"].get<MeshComponent>();
            }

            // Restore the point light component if this entity had one when it was saved.
            if (entityJson.contains("PointLightComponent"))
            {
                entity.AddPointLightComponent() = entityJson["PointLightComponent"].get<PointLightComponent>();
            }

            if (entityJson.contains("CameraComponent"))
            {
                entity.Camera = entityJson["CameraComponent"].get<CameraComponent>();
            }

            if (entityJson.contains("AudioEmitterComponent"))
            {
                entity.AddAudioEmitterComponent() = entityJson["AudioEmitterComponent"].get<AudioEmitterComponent>();
            }

            if (entityJson.contains("DecalComponent"))
            {
                entity.AddDecalComponent() = entityJson["DecalComponent"].get<DecalComponent>();
            }

            if (entityJson.contains("RigidBodyComponent"))
            {
                entity.RigidBody = entityJson["RigidBodyComponent"].get<RigidBodyComponent>();
            }

            if (entityJson.contains("AnimatorComponent"))
            {
                entity.Animator = entityJson["AnimatorComponent"].get<AnimatorComponent>();
            }

            if (entityJson.contains("RainComponent"))
            {
                entity.AddRainComponent() = entityJson["RainComponent"].get<RainComponent>();
            }

            if (entityJson.contains("ParticleSystemComponent"))
            {
                entity.AddParticleSystemComponent() =
                    entityJson["ParticleSystemComponent"].get<ParticleSystemComponent>();
            }

            if (entityJson.contains("WaterComponent"))
            {
                entity.AddWaterComponent() = entityJson["WaterComponent"].get<WaterComponent>();
            }

            if (entityJson.contains("TerrainComponent"))
            {
                entity.AddTerrainComponent() = entityJson["TerrainComponent"].get<TerrainComponent>();
            }

            if (entityJson.contains("VegetationAreaComponent"))
            {
                entity.AddVegetationAreaComponent() = entityJson["VegetationAreaComponent"].get<VegetationAreaComponent>();
            }

            ++entityIndex;

            const float entityProgress = 0.15f + (0.85f * (static_cast<float>(entityIndex) / static_cast<float>(entityCount)));
            const std::string statusMessage = "Loading entities (" + std::to_string(entityIndex) + "/" + std::to_string(entityCount) + ")...";
            if (!ReportDeserializeProgress(progressCallback, userData, entityProgress, statusMessage.c_str()))
            {
                return false;
            }
        }
    }
    catch (...)
    {
        return false;
    }

    // Particle systems that name a .particle file stored only the path; read the effects now.
    ParticleEffects::LoadAll(*mScene->Entities);
    EnsureEntityIds(*mScene->Entities);
    ReportDeserializeProgress(progressCallback, userData, 1.0f, "Scene loaded.");
    return true;
}
