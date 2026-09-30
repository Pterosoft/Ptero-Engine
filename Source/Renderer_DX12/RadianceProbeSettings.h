#pragma once

#include <cmath>
#include <cstdint>

// RadianceProbeSettings.h
// Settings for the world-space Radiance Probe grid.
// Probes are placed on nested uniform 3D grids (cascades) and store
// spherical-harmonics irradiance (L1 SH, 9 coefficients × RGB) updated
// each frame via inline DXR ray queries.
struct RadianceProbeSettings
{
    // Master toggle.
    bool Enabled = false;

    // Grid dimensions (number of probes along X/Y/Z).
    // Total probe count = GridX * GridY * GridZ.
    int GridX = 16;
    int GridY = 16;
    int GridZ = 16;

    // World-space spacing between adjacent probes (metres), in the finest
    // cascade. Each further cascade doubles it.
    float Spacing = 2.0f;

    // Nested grids of the same probe count around the camera, each twice the
    // spacing - and so twice the reach - of the one inside it. Surfaces blend
    // from one cascade into the next instead of falling off the edge of a
    // single grid. Range [1..kMaxRadianceProbeCascades]; only meaningful while
    // the grid follows the camera.
    int CascadeCount = 4;

    // World-space centre of the probe grid (usually around the camera).
    float OriginX = 0.0f;
    float OriginY = 0.0f;
    float OriginZ = 2.0f;

    // Follow camera: if true the grid re-centres on the camera each frame.
    bool FollowCamera = true;

    // Number of rays shot per probe per frame for SH update.
    // Higher = more accurate lighting, higher GPU cost. Range [32..256].
    int RaysPerProbe = 64;

    // Exponential blend weight for probe SH updates (per frame).
    // Lower = smoother, higher = faster response. Range [0.01..1.0].
    float UpdateBlend = 0.15f;

    // ── Output ────────────────────────────────────────────────────────────
    // Multiplier on the probe diffuse GI, the counterpart of RTGI's GI
    // Intensity. 1.0 = physically correct.
    float GiIntensity = 1.0f;

    // Ray-traced specular reflections alongside the probe diffuse GI. The
    // probes cannot hold a reflection themselves, so this runs RTGI's
    // specular pass on its own (its roughness threshold is shared with RTGI).
    bool  SpecularEnabled = true;
    float SpecularIntensity = 1.0f;

    // ── Debug ─────────────────────────────────────────────────────────────
    // Show visible debug spheres for each probe in the scene.
    bool DebugShowProbes = false;

    // 0 = diffuse irradiance, 1 = specular-style response.
    int DebugLightingMode = 0;

    // Radius of the debug probe sphere in world-space metres.
    float DebugSphereRadius = 0.18f;
};

// Must match PTERO_PROBE_MAX_CASCADES in RadianceProbeCommon.hlsli and the
// cascade arrays in RadianceProbes_Common.hlsli.
constexpr int kMaxRadianceProbeCascades = 4;

// Cascades in use. A fixed grid does not move with the camera, so nested
// grids around it would all sit in the same place; it keeps one.
inline int ResolveProbeCascadeCount(const RadianceProbeSettings& settings)
{
    if (!settings.FollowCamera)
        return 1;
    return (settings.CascadeCount < 1) ? 1
         : (settings.CascadeCount > kMaxRadianceProbeCascades) ? kMaxRadianceProbeCascades
         : settings.CascadeCount;
}

inline float ResolveProbeCascadeSpacing(const RadianceProbeSettings& settings, int cascade)
{
    return settings.Spacing * static_cast<float>(1 << cascade);
}

// Probes in one cascade; every cascade has the same grid shape.
inline int ResolveProbesPerCascade(const RadianceProbeSettings& settings)
{
    return settings.GridX * settings.GridY * settings.GridZ;
}

inline int ResolveTotalProbeCount(const RadianceProbeSettings& settings)
{
    return ResolveProbesPerCascade(settings) * ResolveProbeCascadeCount(settings);
}

// The grid's world-space origin for this frame.
//
// Three passes need it and must agree to the metre: the probe renderer writes
// SH at these positions, deferred shading reads them back, and the volumetric
// fog samples the same grid per froxel. A copy of this that drifted would shift
// every probe lookup by up to one cell and show up as light leaking through
// walls, so the snapping lives here rather than being re-derived at each site.
inline void ResolveProbeGridOrigin(
    const RadianceProbeSettings& settings,
    int cascade,
    float cameraX,
    float cameraY,
    float cameraZ,
    float& outOriginX,
    float& outOriginY,
    float& outOriginZ)
{
    outOriginX = settings.OriginX;
    outOriginY = settings.OriginY;
    outOriginZ = settings.OriginZ;

    const float spacing = ResolveProbeCascadeSpacing(settings, cascade);
    if (!settings.FollowCamera || spacing <= 0.0f)
        return;

    const auto axisCount = [](int count) { return (count > 1) ? (count - 1) : 0; };
    const float halfX = axisCount(settings.GridX) * spacing * 0.5f;
    const float halfY = axisCount(settings.GridY) * spacing * 0.5f;
    const float halfZ = axisCount(settings.GridZ) * spacing * 0.5f;

    // Snap to the nearest cell so the grid does not shift under the probes
    // every frame and invalidate their accumulated SH.
    const auto snap = [](float value, float cellSize)
    {
        return std::floor(value / cellSize) * cellSize;
    };

    outOriginX = snap(cameraX - halfX, spacing);
    outOriginY = snap(cameraY - halfY, spacing);
    outOriginZ = snap(cameraZ - halfZ, spacing);
}

// The probe field as every reader sees it: grid shape plus, per cascade, the
// origin (xyz) and spacing (w). Mirrors PteroProbeField in
// RadianceProbeCommon.hlsli, which deferred shading and the fog embed in their
// constant buffers; CascadeCount 0 means no field.
struct RadianceProbeFieldGpu
{
    uint32_t GridX = 0;
    uint32_t GridY = 0;
    uint32_t GridZ = 0;
    uint32_t CascadeCount = 0;
    float    Cascades[kMaxRadianceProbeCascades][4]{};
};
static_assert(sizeof(RadianceProbeFieldGpu) == 80, "must match PteroProbeField");

inline RadianceProbeFieldGpu ResolveProbeField(
    const RadianceProbeSettings& settings,
    float cameraX,
    float cameraY,
    float cameraZ)
{
    RadianceProbeFieldGpu field{};
    if (settings.GridX <= 0 || settings.GridY <= 0 || settings.GridZ <= 0 || settings.Spacing <= 0.0f)
        return field;

    field.GridX = static_cast<uint32_t>(settings.GridX);
    field.GridY = static_cast<uint32_t>(settings.GridY);
    field.GridZ = static_cast<uint32_t>(settings.GridZ);
    field.CascadeCount = static_cast<uint32_t>(ResolveProbeCascadeCount(settings));
    for (uint32_t c = 0; c < field.CascadeCount; ++c)
    {
        const int cascade = static_cast<int>(c);
        ResolveProbeGridOrigin(settings, cascade, cameraX, cameraY, cameraZ,
            field.Cascades[c][0], field.Cascades[c][1], field.Cascades[c][2]);
        field.Cascades[c][3] = ResolveProbeCascadeSpacing(settings, cascade);
    }
    return field;
}
