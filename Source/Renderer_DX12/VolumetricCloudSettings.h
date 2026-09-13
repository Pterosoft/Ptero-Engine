#pragma once

// VolumetricCloudSettings
// Artist-facing controls for the raymarched volumetric cloud layer.
//
// All distances are in metres and world space is Z-up with the ground at z = 0,
// matching the rest of the renderer.  The layer is modelled as a spherical
// shell around a planet of PlanetRadiusKm so it curves away at the horizon.
struct VolumetricCloudSettings
{
    bool  Enabled = true;

    // ---- Layer geometry -----------------------------------------------------
    float PlanetRadiusKm       = 6360.0f;
    float LayerBottomMeters    = 1200.0f;
    float LayerThicknessMeters = 4000.0f;

    // ---- Shape --------------------------------------------------------------
    // Coverage acts as a threshold on the weather map: 0 clears the sky, 1 fills it.
    float Coverage             = 0.55f;
    // 0 = flat stratus, 0.5 = cumulus, 1 = towering cumulonimbus.  Blended 50/50
    // with the weather map's own cloud-type channel.
    float CloudType            = 0.50f;
    float Density              = 0.60f;
    // World size of one tile of each noise volume.
    float BaseNoiseScaleMeters   = 24000.0f;
    float DetailNoiseScaleMeters = 1600.0f;
    float DetailStrength         = 0.35f;
    float CurlStrength           = 0.60f;
    float AnvilBias              = 0.25f;
    float WeatherScaleMeters     = 90000.0f;
    float CloudTopOffsetMeters   = 350.0f;
    int   WeatherSeed            = 1337;
    // Baked into the weather map; changing any of these triggers a re-bake.
    float WeatherCellSize        = 1.0f;
    float WeatherCoverageBias    = 0.0f;
    float WeatherTypeBias        = 0.0f;

    // ---- Wind ---------------------------------------------------------------
    float WindDirectionDegrees = 45.0f;
    float WindSpeed            = 12.0f;   // m/s
    // Horizontal shear applied across the layer, as a fraction of its thickness.
    float WindSkew             = 0.35f;
    float DetailWindSpeedScale = 2.0f;

    // ---- Lighting -----------------------------------------------------------
    float ScatteringColorR = 1.0f;
    float ScatteringColorG = 1.0f;
    float ScatteringColorB = 1.0f;
    // Extinction per metre at density 1.  Real cumulus sit around 0.04 - 0.1.
    float ExtinctionScale  = 0.05f;
    // Dual-lobe Henyey-Greenstein: forward lobe, backward lobe, and the blend.
    float PhaseG0    = 0.80f;
    float PhaseG1    = -0.25f;
    float PhaseBlend = 0.28f;
    float PowderStrength = 0.35f;
    // Wrenninge multiple-scattering octaves and their per-octave falloffs.
    int   MultiScatterOctaves = 3;
    float MsScatterFalloff    = 0.55f;
    float MsExtinctionFalloff = 0.55f;
    float MsPhaseFalloff      = 0.50f;
    float SunIntensityScale     = 1.0f;
    float AmbientIntensityScale = 1.0f;
    float GroundBounceScale     = 0.30f;

    // ---- Ray marching -------------------------------------------------------
    // Steps for a vertical traversal of the layer; grazing rays scale up to 2x.
    int   MaxSteps   = 96;
    int   LightSteps = 6;
    float LightMarchDistanceMeters = 1600.0f;
    float MaxTraceDistanceMeters   = 120000.0f;
    float DistanceFadeStartMeters  = 80000.0f;
    // Beyond this, high frequency erosion is skipped entirely.
    float DetailFadeDistanceMeters = 25000.0f;
    float ShadowConeSpread = 0.12f;
    float ShadowStepGrowth = 1.35f;

    // 1 = full resolution trace, 2 = half, 4 = quarter.
    int   ResolutionDivisor  = 2;
    bool  TemporalUpsampling = true;
    float TemporalBlend      = 0.92f;

    // 0 composite, 1 luminance, 2 transmittance, 3 coverage, 4 cloud distance.
    int   DebugView = 0;
};

// Named starting points for the weather sliders.  Only the parameters that make
// the preset recognisable are touched, so any lighting or performance tuning the
// user has done survives a preset change.
enum class VolumetricCloudPreset
{
    ClearSkies,
    ScatteredCumulus,
    Overcast,
    Stormy
};

inline void ApplyVolumetricCloudPreset(VolumetricCloudSettings& settings, VolumetricCloudPreset preset)
{
    switch (preset)
    {
    case VolumetricCloudPreset::ClearSkies:
        settings.Coverage = 0.22f;
        settings.CloudType = 0.30f;
        settings.Density = 0.45f;
        settings.AnvilBias = 0.0f;
        settings.LayerBottomMeters = 2000.0f;
        settings.LayerThicknessMeters = 2500.0f;
        settings.WindSpeed = 8.0f;
        settings.WeatherCoverageBias = -0.15f;
        settings.WeatherTypeBias = -0.10f;
        break;

    case VolumetricCloudPreset::ScatteredCumulus:
        settings.Coverage = 0.55f;
        settings.CloudType = 0.50f;
        settings.Density = 0.60f;
        settings.AnvilBias = 0.25f;
        settings.LayerBottomMeters = 1200.0f;
        settings.LayerThicknessMeters = 4000.0f;
        settings.WindSpeed = 12.0f;
        settings.WeatherCoverageBias = 0.0f;
        settings.WeatherTypeBias = 0.0f;
        break;

    case VolumetricCloudPreset::Overcast:
        settings.Coverage = 0.88f;
        settings.CloudType = 0.18f;
        settings.Density = 0.75f;
        settings.AnvilBias = 0.0f;
        settings.LayerBottomMeters = 800.0f;
        settings.LayerThicknessMeters = 2200.0f;
        settings.WindSpeed = 14.0f;
        settings.WeatherCoverageBias = 0.30f;
        settings.WeatherTypeBias = -0.25f;
        break;

    case VolumetricCloudPreset::Stormy:
        settings.Coverage = 0.92f;
        settings.CloudType = 0.95f;
        settings.Density = 0.95f;
        settings.AnvilBias = 0.85f;
        settings.LayerBottomMeters = 700.0f;
        settings.LayerThicknessMeters = 7000.0f;
        settings.WindSpeed = 26.0f;
        settings.WindSkew = 0.55f;
        settings.WeatherCoverageBias = 0.25f;
        settings.WeatherTypeBias = 0.35f;
        break;
    }
}
