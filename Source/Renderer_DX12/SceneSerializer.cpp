#include "pch.h"

#include "SceneSerializer.h"

#include "..\SDKs\nlohmann\json.hpp"

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
            { "SkyColorB", settings.SkyColorB }
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
            { "DebugView", settings.DebugView }
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
            { "BlendFactor", settings.BlendFactor },
            { "JitterScale", settings.JitterScale }
        };
    }

    void DeserializeTaaSettings(const json& settingsJson, TaaSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.BlendFactor = settingsJson.value("BlendFactor", settings.BlendFactor);
        settings.JitterScale = settingsJson.value("JitterScale", settings.JitterScale);
        settings.UseSharpening = settingsJson.value("UseSharpening", settings.UseSharpening);
        settings.SharpeningStrength = settingsJson.value("SharpeningStrength", settings.SharpeningStrength);
        settings.ResetHistory = true;
    }

    json SerializeSharpenSettings(const SharpenSettings& settings)
    {
        return json{
            { "ImageSharpeningEnabled", settings.ImageSharpeningEnabled },
            { "ImageSharpeningStrength", settings.ImageSharpeningStrength },
            { "TextureSharpeningEnabled", settings.TextureSharpeningEnabled },
            { "TextureMipLODBias", settings.TextureMipLODBias }
        };
    }

    void DeserializeSharpenSettings(const json& settingsJson, SharpenSettings& settings)
    {
        settings.ImageSharpeningEnabled = settingsJson.value("ImageSharpeningEnabled", settings.ImageSharpeningEnabled);
        settings.ImageSharpeningStrength = settingsJson.value("ImageSharpeningStrength", settings.ImageSharpeningStrength);
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
            { "MaxBounces", settings.MaxBounces },
            { "NextEventEstimation", settings.NextEventEstimation },
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
        settings.MaxBounces = settingsJson.value("MaxBounces", settings.MaxBounces);
        settings.NextEventEstimation = settingsJson.value("NextEventEstimation", settings.NextEventEstimation);
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
            { "Intensity",  settings.Intensity },
            { "Threshold",  settings.Threshold },
            { "Knee",       settings.Knee },
            { "Radius",     settings.Radius },
            { "MipLevels",  settings.MipLevels }
        };
    }

    void DeserializeBloomSettings(const json& settingsJson, BloomSettings& settings)
    {
        settings.Enabled   = settingsJson.value("Enabled",   settings.Enabled);
        settings.Intensity = settingsJson.value("Intensity", settings.Intensity);
        settings.Threshold = settingsJson.value("Threshold", settings.Threshold);
        settings.Knee      = settingsJson.value("Knee",      settings.Knee);
        settings.Radius    = settingsJson.value("Radius",    settings.Radius);
        settings.MipLevels = settingsJson.value("MipLevels", settings.MipLevels);
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
    if (mScene->Sharpen != nullptr)
        sceneJson["SharpenSettings"] = SerializeSharpenSettings(*mScene->Sharpen);
    if (mScene->Dlss != nullptr)
        sceneJson["DlssSettings"] = SerializeDlssSettings(*mScene->Dlss);
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

    // An empty graph writes nothing, so levels made before visual scripting existed keep
    // round-tripping unchanged.
    if (mScene->NodeGraph != nullptr && !mScene->NodeGraph->IsEmpty())
        sceneJson["NodeGraph"] = json::parse(mScene->NodeGraph->ToJsonString());

    for (const Entity& entity : *mScene->Entities)
    {
        json entityJson;
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

        if (entity.HasAudioEmitterComponent())
        {
            entityJson["AudioEmitterComponent"] = *entity.AudioEmitter;
        }

        if (entity.HasDecalComponent())
        {
            entityJson["DecalComponent"] = *entity.Decal;
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

    std::ifstream inputStream(filepath);
    if (!inputStream)
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
        inputStream >> sceneJson;
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

            if (entityJson.contains("AudioEmitterComponent"))
            {
                entity.AddAudioEmitterComponent() = entityJson["AudioEmitterComponent"].get<AudioEmitterComponent>();
            }

            if (entityJson.contains("DecalComponent"))
            {
                entity.AddDecalComponent() = entityJson["DecalComponent"].get<DecalComponent>();
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

    ReportDeserializeProgress(progressCallback, userData, 1.0f, "Scene loaded.");
    return true;
}
