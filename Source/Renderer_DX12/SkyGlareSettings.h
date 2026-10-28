#pragma once

// SkyGlareSettings
// The glare around the sun or the moon, drawn by its own pass (SkyGlareRenderer)
// rather than by the level's bloom, so it can be tuned without touching the bloom.
// The sky draws the sun disc only bright enough to clip to white, and this pass adds
// the light the eye and the lens scatter around the real sun, from its true energy;
// around the moon it is the soft glow a bright moon has in a dark sky. Part of the
// time of day (TimeOfDaySettings::SunGlare / MoonGlare), and saved with it.
struct SkyGlareSettings
{
    bool  Enabled = true;

    // Share of the body's light the glare spreads around it. Real optics scatter a
    // few percent; the sun is so bright that this is plenty.
    float Intensity = 0.03f;

    // ---- Halo ----------------------------------------------------------------
    // Angular radius, in degrees, of the bright centre the halo stays flat over
    // before it falls off. Never smaller than the disc itself.
    float HaloCore = 0.4f;
    // How far the halo reaches, in degrees; it fades smoothly to nothing there.
    float HaloRadius = 25.0f;
    // Exponent of the power-law falloff past the core. Higher = tighter glare.
    float HaloFalloff = 2.4f;

    // ---- Spikes --------------------------------------------------------------
    // Share of the glare's energy in the aperture's diffraction spikes; 0 = none.
    float StreakStrength = 0.06f;
    // Length of the spikes, in degrees.
    float StreakLength = 8.0f;
    // Aperture that makes them: an even blade count gives that many spikes, an odd
    // one twice as many. Rotation in degrees.
    int   ApertureBlades = 6;
    float ApertureRotation = 15.0f;

    // Wavelength spread: red reaches further than blue, which tints the glare's
    // edge warm, as real glare is. 0 = neutral.
    float ChromaticSpread = 0.3f;
};

// The moon's: a wide, soft, neutral glow and no spikes. Its disc is far dimmer
// than the sun's, so it takes a larger share to show.
inline SkyGlareSettings DefaultMoonGlareSettings()
{
    SkyGlareSettings settings;
    settings.Intensity = 0.08f;
    settings.HaloCore = 1.0f;
    settings.HaloRadius = 20.0f;
    settings.HaloFalloff = 2.0f;
    settings.StreakStrength = 0.0f;
    settings.ChromaticSpread = 0.1f;
    return settings;
}
