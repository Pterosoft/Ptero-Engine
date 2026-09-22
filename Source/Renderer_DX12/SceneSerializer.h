#pragma once

#include "Components.h"
#include "TimeOfDaySettings.h"
#include "TaaSettings.h"
#include "SMAASettings.h"
#include "SharpenSettings.h"
#include "DlssSettings.h"
#include "FsrSettings.h"
#include "RadianceCascadesSettings.h"
#include "RtGISettings.h"
#include "RtAOSettings.h"
#include "GtaoSettings.h"
#include "SsrSettings.h"
#include "ChromaticAberrationSettings.h"
#include "AgxTonemapSettings.h"
#include "VolumetricFogSettings.h"
#include "VolumetricCloudSettings.h"
#include "BloomSettings.h"

#include "System/NodeGraphDocument.h"

#include <string>
#include <vector>

struct Scene
{
    std::vector<Entity>* Entities = nullptr;
    DirectX::XMFLOAT3* CameraPosition = nullptr;
    DirectX::XMFLOAT3* CameraRotation = nullptr;
    bool* HasCameraPosition = nullptr;
    bool* HasCameraRotation = nullptr;
    TimeOfDaySettings* TimeOfDay = nullptr;
    TaaSettings* Taa = nullptr;
    SMAASettings* Smaa = nullptr;
    SharpenSettings* Sharpen = nullptr;
    DlssSettings* Dlss = nullptr;
    FsrSettings* Fsr = nullptr;
    GlobalIlluminationMode* GlobalIllumination = nullptr;
    RtGISettings* Rtgi = nullptr;
    RadianceCascadesSettings* RadianceCascades = nullptr;
    RtAOSettings* Rtao = nullptr;
    GtaoSettings* Gtao = nullptr;
    SsrSettings* Ssr = nullptr;
    ChromaticAberrationSettings* ChromaticAberration = nullptr;
    AgxTonemapSettings* Agx = nullptr;
    VolumetricFogSettings* VolumetricFog = nullptr;
    VolumetricCloudSettings* VolumetricCloud = nullptr;
    BloomSettings* Bloom = nullptr;
    // The level's visual script. Saved inline under "NodeGraph"; a level may instead
    // carry a path there, which is how a graph shared between levels is referenced.
    NodeGraphDocument* NodeGraph = nullptr;

    Entity& CreateEntity()
    {
        if (Entities == nullptr)
        {
            throw std::runtime_error("Scene::CreateEntity requires a valid entity container.");
        }

        Entities->emplace_back();
        return Entities->back();
    }
};

class SceneSerializer
{
public:
    using ProgressCallback = bool(*)(float progress, const char* statusMessage, void* userData);

    explicit SceneSerializer(Scene* scene)
        : mScene(scene)
    {
    }

    void Serialize(const std::string& filepath);
    bool Deserialize(const std::string& filepath, ProgressCallback progressCallback = nullptr, void* userData = nullptr);

private:
    Scene* mScene = nullptr;
};
