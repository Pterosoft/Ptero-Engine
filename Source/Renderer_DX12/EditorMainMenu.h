#pragma once

#include <windows.h>

#include <string>

#include "../QtUi/QtUi.h"
#include "TaaSettings.h"
#include "SMAASettings.h"
#include "MsaaSettings.h"
#include "SharpenSettings.h"
#include "DlssSettings.h"
#include "TimeOfDaySettings.h"
#include "WindSettings.h"
#include "RadianceCascadesSettings.h"
#include "RtGISettings.h"
#include "RtAOSettings.h"
#include "RadianceProbeSettings.h"
#include "AgxTonemapSettings.h"
#include "VolumetricFogSettings.h"
#include "VolumetricCloudSettings.h"
#include "GtaoSettings.h"
#include "SsrSettings.h"
#include "ChromaticAberrationSettings.h"
#include "BloomSettings.h"
#include "PointShadowSettings.h"
#include "AudioManager.h"

// G-Buffer and GI debug texture handles passed to the View menu visualizer.
struct GBufferDebugTextureIds
{
    UiTextureID Albedo   = UiTextureID_Invalid;
    UiTextureID Normal   = UiTextureID_Invalid;
    UiTextureID Material = UiTextureID_Invalid;
    UiTextureID Depth    = UiTextureID_Invalid;
    UiTextureID GiAccum  = UiTextureID_Invalid;
    UiTextureID PointShadowArray = UiTextureID_Invalid;
};

using CompileShadersCommandFn = std::string(*)();

void RenderEditorMainMenu(
    HWND windowHandle,
    void* editor,
    UiTextureID sceneTextureId,
    const char* sceneStatusMessage,
    const char* statisticsText,
    float* cameraSpeed,
    float* viewDistanceMeters,
    bool* gridEnabled,
    bool* showStatistics,
    bool* showViewportPlacementIcons,
    bool* showComponentsPanel,
    bool* showLevelExplorerPanel,
    bool* showPropertiesPanel,
    bool* showResourceDebugPanel,
    TaaSettings* taaSettings,
    SMAASettings* smaaSettings,
    MsaaSettings* msaaSettings,
    SharpenSettings* sharpenSettings,
    DlssSettings* dlssSettings,
    TimeOfDaySettings* timeOfDaySettings,
    WindSettings* windSettings,
    GlobalIlluminationMode* globalIlluminationMode,
    RtGISettings* rtgiSettings,
    RadianceCascadesSettings* radianceCascadesSettings,
    RadianceProbeSettings* probeSettings,
    RtAOSettings* rtaoSettings,
    GtaoSettings* gtaoSettings,
    SsrSettings* ssrSettings,
    ChromaticAberrationSettings* chromaticAberrationSettings,
    AgxTonemapSettings* agxSettings,
    VolumetricFogSettings* volumetricFogSettings,
    VolumetricCloudSettings* volumetricCloudSettings,
    BloomSettings* bloomSettings,
    PointShadowSettings* pointShadowSettings,
    const GBufferDebugTextureIds* gbufferTextureIds,
    AudioManager* audioManager,
    CompileShadersCommandFn compileShadersCommand,
    float msaaResolveTimeMs = 0.0f);
