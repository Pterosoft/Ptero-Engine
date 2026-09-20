#pragma once

#include <cmath>

// RadianceProbeSettings.h
// Settings for the world-space Radiance Probe grid.
// Probes are placed on a uniform 3D grid in the scene and store
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

    // World-space spacing between adjacent probes (metres).
    float Spacing = 2.0f;

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

    // ── Debug ─────────────────────────────────────────────────────────────
    // Show visible debug spheres for each probe in the scene.
    bool DebugShowProbes = false;

    // 0 = diffuse irradiance, 1 = specular-style response.
    int DebugLightingMode = 0;

    // Radius of the debug probe sphere in world-space metres.
    float DebugSphereRadius = 0.18f;
};

// The grid's world-space origin for this frame.
//
// Three passes need it and must agree to the metre: the probe renderer writes
// SH at these positions, deferred shading reads them back, and the volumetric
// fog samples the same grid per froxel. A copy of this that drifted would shift
// every probe lookup by up to one cell and show up as light leaking through
// walls, so the snapping lives here rather than being re-derived at each site.
inline void ResolveProbeGridOrigin(
    const RadianceProbeSettings& settings,
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

    if (!settings.FollowCamera || settings.Spacing <= 0.0f)
        return;

    const auto axisCount = [](int count) { return (count > 1) ? (count - 1) : 0; };
    const float halfX = axisCount(settings.GridX) * settings.Spacing * 0.5f;
    const float halfY = axisCount(settings.GridY) * settings.Spacing * 0.5f;
    const float halfZ = axisCount(settings.GridZ) * settings.Spacing * 0.5f;

    // Snap to the nearest cell so the grid does not shift under the probes
    // every frame and invalidate their accumulated SH.
    const auto snap = [](float value, float spacing)
    {
        return std::floor(value / spacing) * spacing;
    };

    outOriginX = snap(cameraX - halfX, settings.Spacing);
    outOriginY = snap(cameraY - halfY, settings.Spacing);
    outOriginZ = snap(cameraZ - halfZ, settings.Spacing);
}
