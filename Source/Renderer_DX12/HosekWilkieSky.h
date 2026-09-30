#pragma once

// HosekWilkieSky
// Evaluates the time of day: where the sun and moon are, what colour the sky
// and the directional light are, how visible the stars are, and what exposure
// the hour calls for. The daylight sky colour comes from the Hosek-Wilkie RGB
// sky model SDK; night and twilight are blended on top of it.

struct TimeOfDaySettings;

struct HosekWilkieResult
{
    // Solar elevation in radians above the horizon (negative = below horizon).
    float SolarElevationRad = 0.0f;

    // The scene's directional light: the sun by day, the moon by night (see
    // KeyLightIsMoon). Points FROM the light TOWARD the scene so it can be used
    // directly as a directional-light direction. Every lighting, shadow, fog,
    // cloud and GI pass keys off this one vector.
    float SunDirX = 0.0f, SunDirY = 1.0f, SunDirZ = 0.0f;

    // Normalised daylight sky colour (linear RGB, max channel = 1).
    float SkyR = 0.25f, SkyG = 0.45f, SkyB = 1.00f;

    // Normalised sun colour (linear RGB, max channel = 1).
    float SunR = 1.00f, SunG = 0.95f, SunB = 0.80f;

    // Whether the sun is above the horizon.
    bool SunAboveHorizon = true;

    // ---- Celestial bodies, for the sky pass --------------------------------------
    // The sun and the moon themselves, FROM the body TOWARD the scene, whichever of
    // them is lighting the scene.
    float SolarDirX = 0.0f, SolarDirY = 1.0f, SolarDirZ = 0.0f;
    float MoonDirX = 0.0f, MoonDirY = -1.0f, MoonDirZ = 0.0f;
    float MoonElevationRad = 0.0f;
    // Fraction of the moon's face that is lit, [0, 1].
    float MoonIllumination = 1.0f;

    // Rotation from world space into the frame the stars are fixed in, row-major:
    // star = (dot(row0, world), dot(row1, world), dot(row2, world)). It turns with
    // the hour about the celestial pole, so the stars wheel across the night.
    float StarRotation[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    // 0 in daylight, 1 once the sun is 14 degrees down.
    float StarVisibility = 0.0f;

    // ---- Lighting, in scene-linear units (lux already divided by the reference) --
    bool  KeyLightIsMoon = false;
    // Directional light colour and intensity, for whichever body SunDir is.
    float LightR = 0.0f, LightG = 0.0f, LightB = 0.0f;
    // Sky ambient, blending from daylight to the night sky through twilight.
    float AmbientR = 0.0f, AmbientG = 0.0f, AmbientB = 0.0f;
    // 1 in full daylight, 0 at night; how much of the ambient is daylight sky.
    float DaylightWeight = 1.0f;

    // Discs drawn by the sky pass. The sun disc carries the sun's colour and
    // intensity and fades out at the horizon; the moon disc is its full-face
    // radiance, which the sky pass shades by phase.
    float SunDiscR = 0.0f, SunDiscG = 0.0f, SunDiscB = 0.0f;
    float MoonDiscR = 0.0f, MoonDiscG = 0.0f, MoonDiscB = 0.0f;
    // Radiance of an average star before the sky pass's per-star variation.
    float StarRadiance = 0.0f;

    // ---- Exposure ----------------------------------------------------------------
    // Whether the time of day decides the exposure this frame, and the EV100 it
    // asks for when it does.
    bool  ControlsExposure = false;
    float Ev100 = 0.0f;

    // log2 of the luminance an open outdoor view is expected to meter at under
    // this hour's light, for eye adaptation to measure the camera's view against.
    float AdaptationReferenceLogLuminance = 0.0f;

    // Scale that brings this frame's lighting back to the brightness of midday,
    // exp2(DayEv100 - Ev100): 1 at noon, ~128 on a moonlit night. The GI passes
    // light with sun * PreExposure and their consumers divide it back out, so
    // reservoirs, denoiser history, the firefly clamp and the half-float targets
    // all see daylight magnitudes whatever the hour. 1 when the time of day does
    // not control exposure.
    float PreExposure = 1.0f;
};

// Evaluates the time of day from the given settings. With the time of day
// disabled, every light and the sky are black and the exposure is left alone.
HosekWilkieResult EvaluateHosekWilkie(const TimeOfDaySettings& settings);
