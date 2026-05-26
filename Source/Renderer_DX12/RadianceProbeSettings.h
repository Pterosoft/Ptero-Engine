#pragma once

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
