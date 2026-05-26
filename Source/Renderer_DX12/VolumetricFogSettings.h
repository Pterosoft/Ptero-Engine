#pragma once

struct VolumetricFogSettings
{
    bool  Enabled = true;

    // Screen-space froxel width/height are derived as sceneResolution / FroxelTileSize.
    // 8 gives 160x90 froxels at 1280x720.
    int   FroxelTileSize = 8;
    int   DepthSlices = 64;

    // World-space fog volume controls.
    float StartDistance = 0.1f;
    float MaxDistance = 100.0f;
    float Density = 0.02f;
    float Anisotropy = 0.5f;

    // Optional exponential height falloff. 0 disables height-based thinning.
    float BaseHeight = 0.0f;
    float HeightFalloff = 0.0f;

    // Linear RGB scattering colour.
    float ColorR = 1.0f;
    float ColorG = 1.0f;
    float ColorB = 1.0f;

    // Linear RGB emissive contribution added directly into the fog medium.
    float EmissiveColorR = 0.0f;
    float EmissiveColorG = 0.0f;
    float EmissiveColorB = 0.0f;
    float EmissiveIntensity = 0.0f;

    // 0 = composite over the scene, 1 = scattering debug, 2 = transmittance debug.
    int   DebugView = 0;
};