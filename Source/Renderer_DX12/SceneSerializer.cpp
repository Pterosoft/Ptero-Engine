#include "pch.h"

#include "SceneSerializer.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <fstream>

using json = nlohmann::json;

namespace
{
    void ReportDeserializeProgress(
        SceneSerializer::ProgressCallback progressCallback,
        void* userData,
        const float progress,
        const char* statusMessage)
    {
        if (progressCallback != nullptr)
        {
            progressCallback(progress, statusMessage, userData);
        }
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

    json SerializeTaaSettings(const TaaSettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "BlendFactor", settings.BlendFactor },
            { "JitterScale", settings.JitterScale },
            { "UseSharpening", settings.UseSharpening },
            { "SharpeningStrength", settings.SharpeningStrength }
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

    json SerializeRtgiSettings(const RtGISettings& settings)
    {
        return json{
            { "Enabled", settings.Enabled },
            { "UseNrdDenoiser", settings.UseNrdDenoiser },
            { "RaysPerPixel", settings.RaysPerPixel },
            { "MaxBounces", settings.MaxBounces },
            { "NextEventEstimation", settings.NextEventEstimation },
            { "RadianceClamp", settings.RadianceClamp },
            { "AccumulationBlend", settings.AccumulationBlend },
            { "NrdMaxAccumulationTime", settings.NrdMaxAccumulationTime },
            { "NrdDisocclusionThreshold", settings.NrdDisocclusionThreshold },
            { "NrdAtrousIterations", settings.NrdAtrousIterations },
            { "NrdSharpenAmount", settings.NrdSharpenAmount },
            { "GiIntensity", settings.GiIntensity },
            { "ColorLeakIntensity", settings.ColorLeakIntensity },
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
        settings.RadianceClamp = settingsJson.value("RadianceClamp", settings.RadianceClamp);
        settings.AccumulationBlend = settingsJson.value("AccumulationBlend", settings.AccumulationBlend);
        settings.NrdMaxAccumulationTime = settingsJson.value("NrdMaxAccumulationTime", settings.NrdMaxAccumulationTime);
        settings.NrdDisocclusionThreshold = settingsJson.value("NrdDisocclusionThreshold", settings.NrdDisocclusionThreshold);
        settings.NrdAtrousIterations = settingsJson.value("NrdAtrousIterations", settings.NrdAtrousIterations);
        settings.NrdSharpenAmount = settingsJson.value("NrdSharpenAmount", settings.NrdSharpenAmount);
        settings.GiIntensity = settingsJson.value("GiIntensity", settings.GiIntensity);
        settings.ColorLeakIntensity = settingsJson.value("ColorLeakIntensity", settings.ColorLeakIntensity);
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
            { "Ev100Min", settings.Ev100Min },
            { "Ev100Max", settings.Ev100Max },
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

    void DeserializeAgxSettings(const json& settingsJson, AgxTonemapSettings& settings)
    {
        settings.Enabled = settingsJson.value("Enabled", settings.Enabled);
        settings.Exposure = settingsJson.value("Exposure", settings.Exposure);
        settings.Ev100Min = settingsJson.value("Ev100Min", settings.Ev100Min);
        settings.Ev100Max = settingsJson.value("Ev100Max", settings.Ev100Max);
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
    sceneJson["Entities"] = json::array();

    if (mScene->TimeOfDay != nullptr)
        sceneJson["TimeOfDaySettings"] = SerializeTimeOfDaySettings(*mScene->TimeOfDay);
    if (mScene->Taa != nullptr)
        sceneJson["TaaSettings"] = SerializeTaaSettings(*mScene->Taa);
    if (mScene->Dlss != nullptr)
        sceneJson["DlssSettings"] = SerializeDlssSettings(*mScene->Dlss);
    if (mScene->Rtgi != nullptr)
        sceneJson["RtGISettings"] = SerializeRtgiSettings(*mScene->Rtgi);
    if (mScene->Rtao != nullptr)
        sceneJson["RtAOSettings"] = SerializeRtaoSettings(*mScene->Rtao);
    if (mScene->Gtao != nullptr)
        sceneJson["GTAOSettings"] = SerializeGtaoSettings(*mScene->Gtao);
    if (mScene->Agx != nullptr)
        sceneJson["AgxTonemapSettings"] = SerializeAgxSettings(*mScene->Agx);
    if (mScene->VolumetricFog != nullptr)
        sceneJson["VolumetricFogSettings"] = SerializeVolumetricFogSettings(*mScene->VolumetricFog);
    if (mScene->Bloom != nullptr)
        sceneJson["BloomSettings"] = SerializeBloomSettings(*mScene->Bloom);

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

    ReportDeserializeProgress(progressCallback, userData, 0.02f, "Opening scene file...");

    std::ifstream inputStream(filepath);
    if (!inputStream)
    {
        return false;
    }

    json sceneJson;
    try
    {
        ReportDeserializeProgress(progressCallback, userData, 0.08f, "Parsing scene file...");
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

    ReportDeserializeProgress(progressCallback, userData, 0.15f, "Loading scene settings...");

    if (mScene->TimeOfDay != nullptr && sceneJson.contains("TimeOfDaySettings"))
        DeserializeTimeOfDaySettings(sceneJson["TimeOfDaySettings"], *mScene->TimeOfDay);
    if (mScene->Taa != nullptr && sceneJson.contains("TaaSettings"))
        DeserializeTaaSettings(sceneJson["TaaSettings"], *mScene->Taa);
    if (mScene->Dlss != nullptr && sceneJson.contains("DlssSettings"))
        DeserializeDlssSettings(sceneJson["DlssSettings"], *mScene->Dlss);
    if (mScene->Rtgi != nullptr && sceneJson.contains("RtGISettings"))
        DeserializeRtgiSettings(sceneJson["RtGISettings"], *mScene->Rtgi);
    if (mScene->Rtao != nullptr && sceneJson.contains("RtAOSettings"))
        DeserializeRtaoSettings(sceneJson["RtAOSettings"], *mScene->Rtao);
    if (mScene->Gtao != nullptr && sceneJson.contains("GTAOSettings"))
        DeserializeGtaoSettings(sceneJson["GTAOSettings"], *mScene->Gtao);
    if (mScene->Agx != nullptr && sceneJson.contains("AgxTonemapSettings"))
        DeserializeAgxSettings(sceneJson["AgxTonemapSettings"], *mScene->Agx);
    if (mScene->VolumetricFog != nullptr && sceneJson.contains("VolumetricFogSettings"))
        DeserializeVolumetricFogSettings(sceneJson["VolumetricFogSettings"], *mScene->VolumetricFog);
    if (mScene->Bloom != nullptr && sceneJson.contains("BloomSettings"))
        DeserializeBloomSettings(sceneJson["BloomSettings"], *mScene->Bloom);

    mScene->Entities->clear();

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
                entity.AddTransformComponent(entityJson["TransformComponent"].get<TransformComponent>());
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

            ++entityIndex;

            const float entityProgress = 0.15f + (0.85f * (static_cast<float>(entityIndex) / static_cast<float>(entityCount)));
            const std::string statusMessage = "Loading entities (" + std::to_string(entityIndex) + "/" + std::to_string(entityCount) + ")...";
            ReportDeserializeProgress(progressCallback, userData, entityProgress, statusMessage.c_str());
        }
    }
    catch (...)
    {
        return false;
    }

    ReportDeserializeProgress(progressCallback, userData, 1.0f, "Scene loaded.");
    return true;
}
