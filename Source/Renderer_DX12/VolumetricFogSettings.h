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

    // Extinction coefficient sigma_t, per world unit: the fraction of light the
    // medium removes over one metre. A ray of length d keeps exp(-Density * d).
    // The injection shader used to scale this by a private 0.2 before use, so a
    // value here means five times what it used to.
    float Density = 0.02f;

    float Anisotropy = 0.5f;

    // sigma_s / sigma_t - how much of what the medium removes it scatters back
    // out rather than absorbing. 1 is a non-absorbing medium; anything below it
    // leaves the fog grey where no light reaches instead of pure black.
    float ScatteringAlbedo = 0.9f;

    // Weight on indirect light sampled from the radiance probe grid. This is
    // the only light an interior with no sky and no time-of-day gets, so it is
    // on by default - but it makes the fog pass ask for the probe grid, which
    // costs a probe update each frame if nothing else already needed one.
    // Set to 0 to drop both the contribution and the cost.
    float GiIntensity = 1.0f;

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
