#pragma once

#include <windows.h>

#include <string>

#include "../QtUi/QtUi.h"
#include "TaaSettings.h"
#include "SMAASettings.h"
#include "AnalyticEdgeAASettings.h"
#include "AarSettings.h"
#include "MsaaSettings.h"
#include "SharpenSettings.h"
#include "DlssSettings.h"
#include "FsrSettings.h"
#include "XessSettings.h"
#include "TimeOfDaySettings.h"
#include "WindSettings.h"
#include "RadianceCascadesSettings.h"
#include "RtGISettings.h"
#include "RtAOSettings.h"
#include "PathTracerSettings.h"
#include "RadianceProbeSettings.h"
#include "VoxelGISettings.h"
#include "AgxTonemapSettings.h"
#include "VolumetricFogSettings.h"
#include "VolumetricCloudSettings.h"
#include "GtaoSettings.h"
#include "SsrSettings.h"
#include "SubsurfaceSettings.h"
#include "ShadingSettings.h"
#include "ChromaticAberrationSettings.h"
#include "BloomSettings.h"
#include "LensFlareSettings.h"

class LensFlareRenderer;
#include "PointShadowSettings.h"
#include "VirtualShadowMapSettings.h"
#include "RtShadowSettings.h"
#include "DpleSettings.h"
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
    AnalyticEdgeAASettings* analyticEdgeAaSettings,
    AarSettings* aarSettings,
    const char* aarStatus,
    MsaaSettings* msaaSettings,
    SharpenSettings* sharpenSettings,
    DlssSettings* dlssSettings,
    FsrSettings* fsrSettings,
    const FsrRuntimeStatus* fsrStatus,
    XessSettings* xessSettings,
    const XessRuntimeStatus* xessStatus,
    TimeOfDaySettings* timeOfDaySettings,
    WindSettings* windSettings,
    GlobalIlluminationMode* globalIlluminationMode,
    RtGISettings* rtgiSettings,
    RadianceCascadesSettings* radianceCascadesSettings,
    RadianceProbeSettings* probeSettings,
    VoxelGISettings* voxelGiSettings,
    const VoxelGIStatus* voxelGiStatus,
    RtAOSettings* rtaoSettings,
    PathTracerSettings* pathTracerSettings,
    const char* pathTracerStatus,
    GtaoSettings* gtaoSettings,
    SsrSettings* ssrSettings,
    SubsurfaceSettings* subsurfaceSettings,
    bool subsurfaceRayTracingSupported,
    ShadingSettings* shadingSettings,
    ChromaticAberrationSettings* chromaticAberrationSettings,
    AgxTonemapSettings* agxSettings,
    VolumetricFogSettings* volumetricFogSettings,
    VolumetricCloudSettings* volumetricCloudSettings,
    BloomSettings* bloomSettings,
    LensFlareSettings* lensFlareSettings,
    const LensFlareRenderer* lensFlareRenderer,
    PointShadowSettings* pointShadowSettings,
    VirtualShadowMapSettings* virtualShadowMapSettings,
    const char* virtualShadowMapStatus,
    RtShadowSettings* rtShadowSettings,
    DpleSettings* dpleSettings,
    const char* dpleErrorMessage,
    const GBufferDebugTextureIds* gbufferTextureIds,
    AudioManager* audioManager,
    CompileShadersCommandFn compileShadersCommand,
    float msaaResolveTimeMs = 0.0f);
