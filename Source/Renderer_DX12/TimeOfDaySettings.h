#pragma once

// Lux values that map to 1.0 in the engine's scene-linear units. The sun, the
// moon and the sky are all authored in lux and divided by these before they
// reach a shader, so every pass that lights with them agrees on the scale.
inline constexpr float kTimeOfDayRefSunLux = 100000.0f;
inline constexpr float kTimeOfDayRefSkyLux = 20000.0f;

// TimeOfDaySettings
// Holds all parameters that drive the Hosek-Wilkie sky model, the sun and moon,
// the night sky and the exposure the time of day asks for. Passed from the
// editor UI through to DX12SceneRenderer every frame.
struct TimeOfDaySettings
{
    // Turns the whole time-of-day system off: no sun light, no sky ambient, no
    // procedural sky behind the scene and no cloud layer. The scene is then lit purely
    // by the lights placed in it, which is what an interior or a fully artificially lit
    // set wants. Everything below is ignored while this is false.
    bool Enabled = true;

    // Time in hours [0, 24).  Noon = 12.0.
    float TimeOfDay = 12.0f;

    // Where on Earth the scene is, in degrees north. The sun is modelled at an
    // equinox, so it always rises due east at 06:00 and sets due west at 18:00,
    // and its noon elevation is 90 - Latitude. 30 gives the 60 degree noon sun
    // levels were authored against before the sun had a path.
    float Latitude = 30.0f;

    // Rotates the whole sky - sun path, moon and stars - about the vertical axis,
    // in degrees. At 0 the sun rises toward +X and stands toward -Y at noon.
    float NorthOffset = 0.0f;

    // Atmospheric turbidity [1, 10].  2 = very clear, 5 = hazy, 10 = heavy haze.
    float Turbidity = 2.5f;

    // Ground albedo [0, 1] — affects sky colour near the horizon.
    float GroundAlbedo = 0.1f;

    // Sun intensity in lux (lm/m²).  Real noon sun ≈ 100 000 lx.
    float SunIntensityLux = 120000.0f;

    // Sky ambient intensity in lux.  Clear blue sky ≈ 20 000 lx; use a lower
    // default so ambient doesn't overwhelm direct lighting on enclosed scenes.
    // This is the full-daylight value; it fades through twilight toward
    // NightSkyIntensityLux as the sun goes down.
    float SkyIntensityLux = 20000.0f;

    // When true, the editor UI colour pickers override the Hosek-Wilkie
    // computed sun / sky colour with the values below.
    bool OverrideSunColor  = false;
    bool OverrideSkyColor  = false;

    // Manual override colours (linear RGB, HDR).
    float SunColorR = 1.00f, SunColorG = 0.95f, SunColorB = 0.80f;
    float SkyColorR = 0.25f, SkyColorG = 0.45f, SkyColorB = 1.00f;

    // ---- Night -----------------------------------------------------------------
    // Sky ambient once the sun is well below the horizon, in lux. Real starlight is
    // a fraction of a lux; this is a game value, sized so that together with the
    // exposure below a night is dark but readable.
    float NightSkyIntensityLux = 30.0f;
    float NightSkyColorR = 0.20f, NightSkyColorG = 0.30f, NightSkyColorB = 0.60f;

    // The moon lights the scene as the directional light while the sun is down.
    // Phase 0 = new moon, 0.5 = full, 0.25 / 0.75 = first / last quarter. The phase
    // also places the moon: a full moon rises at sunset and sets at sunrise.
    bool  MoonEnabled = true;
    float MoonPhase = 0.5f;
    // Full-moon illuminance in lux, scaled down by the phase. A real full moon is
    // about 0.25 lx; like the night sky this is a game value, tuned together with
    // NightEv100 so a moonlit night is dark but readable.
    float MoonIntensityLux = 5000.0f;
    float MoonColorR = 0.70f, MoonColorG = 0.82f, MoonColorB = 1.00f;
    // Apparent size of the moon disc, as a multiple of the default.
    float MoonSize = 1.0f;

    bool  StarsEnabled = true;
    // Brightness multiplier on the star field.
    float StarIntensity = 1.0f;
    // Where the stars come from. 0 = procedural: pixel-sharp, twinkling stars and a
    // noise Milky Way. 1 = the star map (Data/Textures/Sky/2k_stars_milky_way.dds):
    // the real sky, but at 2K it is soft up close. 2 = both: the map for the Milky
    // Way and faint background, procedural stars for the sharp points.
    int   StarField = 1;

    // ---- Exposure --------------------------------------------------------------
    // When on, the time of day decides the tonemapper's EV100 instead of the AgX
    // settings: DayEv100 with the sun high, SunsetEv100 with it on the horizon and
    // NightEv100 once it is 12 degrees below. AgX's exposure trim still applies on
    // top, and so does the rest of the grade.
    //
    // On by default, levels saved before it existed included.
    bool  ControlExposure = true;
    // -2 is what the Desert level pins its exposure to at noon; sunset and night
    // were tuned by eye on Desert.
    float DayEv100 = -2.0f;
    float SunsetEv100 = -3.5f;
    float NightEv100 = -5.25f;

    // Eye adaptation on top of the time-of-day exposure. The auto-exposure meter
    // compares what the camera sees with what the hour's light predicts for an
    // open outdoor view, and shifts the exposure by that difference - so outdoors
    // stays on the curve above, while a torch-lit tomb at night stops down and a
    // dark interior by day opens up. Differences under a stop are ignored, which
    // is what keeps ordinary outdoor views from drifting off the curve. How fast
    // it moves is the AgX auto-exposure speed.
    bool  EyeAdaptation = true;
    // Fraction of the metered difference that is followed; 1 = all of it.
    float AdaptationStrength = 0.8f;
    // The range eye adaptation keeps the exposure in, in EV100 (higher = darker).
    // Min is the brightest a dark interior may open up to, Max the darkest a
    // bright one may stop down to. Set it to cover the whole Day/Sunset/Night
    // curve, or the curve itself is clamped.
    float AdaptationEv100Min = -7.5f;
    float AdaptationEv100Max = 1.0f;
};
