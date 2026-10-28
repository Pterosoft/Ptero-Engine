#pragma once

// VoxelGISettings.h
// Tunable parameters for the voxel cone traced global illumination pass
// (VoxelGIRenderer). It needs no ray tracing hardware, so besides being a GI
// solution in its own right it is what runs in place of RTGI and the radiance
// probes on GPUs without DXR 1.1.
struct VoxelGISettings
{
    // Runs voxel GI for surface lighting, taking over from RTGI and the radiance
    // probes. Off by default: levels keep the GI they were authored with.
    bool Enabled = false;

    // Runs voxel GI in place of RTGI / radiance probes when the GPU cannot ray
    // trace (no DXR 1.1), so a level authored with ray traced GI still gets
    // indirect light there instead of none.
    bool FallbackWithoutRayTracing = true;

    // ── Clipmap ───────────────────────────────────────────────────────────
    // Voxels along each edge of one clipmap level. 32, 64, 96 or 128; the
    // memory cost grows with the cube of it (64 x 6 levels is about 40 MB).
    int Resolution = 64;

    // Nested levels, each twice the extent and voxel size of the one inside
    // it. Range [2..6].
    int LevelCount = 5;

    // Edge length, in metres, of a voxel in the finest level. The finest level
    // covers Resolution * VoxelSize metres around the camera. Range [0.05..2].
    float VoxelSize = 0.2f;

    // Mesh LOD the voxelizer draws, added to the level index: level 0 draws
    // LOD bias, level 1 LOD bias + 1, and so on, clamped to the mesh's coarsest.
    int LodBias = 1;

    // ── Lighting ──────────────────────────────────────────────────────────
    // Traces the sun's shadow through the voxels when lighting them. Without it
    // every voxel facing the sun is lit, which leaks sunlight into interiors.
    bool SunShadows = true;

    // Traces each local light's shadow through the voxels. Costs a short march
    // per light per lit voxel.
    bool LocalLightShadows = true;

    // Answers the sun's and local lights' shadows from the virtual shadow map where it
    // holds a page no coarser than VsmMaxTexelSize, and marches through the voxels only
    // where it does not - as RTGI's Shadow Map Visibility does for its shadow rays. A
    // lookup is far cheaper than a march and sharper than the voxels. Needs the
    // virtual shadow map to be on.
    bool  VsmVisibility = true;
    // Coarsest shadow-map texel, in metres, trusted at a voxel. Range [0.005..0.5].
    float VsmMaxTexelSize = 0.1f;

    // Shadow-tests only LightSamples local lights per voxel, picked by how much each
    // would add, instead of every light in reach - RTGI's stochastic light sampling.
    // The temporal blend averages the picks. Off = test every light. Range [1..4].
    bool StochasticLights = true;
    int  LightSamples = 2;

    // How much of the previous bounce is fed back into the voxels' lighting,
    // which converges to multiple bounces over a few updates. 0 = one bounce.
    float MultiBounce = 1.0f;

    // Share of a voxel's new lighting it takes per frame; the rest is what it
    // held before. Lower is steadier - flickering lights and the levels
    // refreshing on different frames stop showing as flicker in the GI - but
    // follows a change in the light more slowly. 1 = no smoothing. Range [0.01..1].
    float TemporalBlend = 0.15f;

    // ── Tracing ───────────────────────────────────────────────────────────
    // Cones per pixel for diffuse GI: 1 (normal only), 5 or 9. Range [1..9].
    int DiffuseCones = 5;

    // Trace at half resolution and upsample, which is a quarter of the cost and
    // all but invisible on low-frequency diffuse light.
    bool HalfResolution = true;

    // Furthest a cone travels, in metres, before it stops and takes the sky.
    float MaxTraceDistance = 60.0f;

    // Offset, in voxels of the finest level, a cone starts from the surface so
    // it does not immediately sample the voxels the surface itself filled.
    float SurfaceOffset = 1.5f;

    // Multiplier applied to the GI output before compositing.
    float GiIntensity = 1.0f;

    // ── Specular reflections ──────────────────────────────────────────────
    // Cone traced reflections. Screen-space reflections, when on, take over
    // wherever they find a hit and leave these the misses.
    bool SpecularEnabled = true;

    // Surfaces rougher than this get no reflection cone. Range [0..1].
    float SpecularRoughnessThreshold = 0.8f;

    // Intensity multiplier for the reflection output.
    float SpecularIntensity = 1.0f;

    // ── Debug ─────────────────────────────────────────────────────────────
    // 0 = final lit, 1 = raw GI irradiance, 2 = GI luminance,
    // 3 = reflections only (the shared RTGI debug views), 20 = voxel albedo,
    // 21 = voxel radiance, 22 = clipmap level (the voxel views ray march the
    // clipmap from the camera).
    int DebugView = 0;
};

// What the voxel GI did last frame, for the settings panel.
struct VoxelGIStatus
{
    bool Active = false;                // it lit the frame
    bool Fallback = false;              // ...in place of RTGI / probes this GPU cannot run
    bool RayTracingSupported = true;    // DXR 1.1, which RTGI and the probes need
    unsigned long long ClipmapBytes = 0;
    const char* Error = nullptr;        // why it is not running, when it failed
};
