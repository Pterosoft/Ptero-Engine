#pragma once

// HosekWilkieSky
// Thin C++ wrapper around the Hosek-Wilkie RGB sky model SDK.
// Evaluates the average sky radiance (used as ambient/sky light colour)
// and the solar disc radiance (used as the directional sun light colour)
// from time-of-day parameters.
//
// All output radiance values are in W/(m²·sr) from the RGB model.
// The caller is responsible for scaling them to the desired lux target.

struct TimeOfDaySettings;

struct HosekWilkieResult
{
    // Solar elevation in radians above the horizon (negative = below horizon).
    float SolarElevationRad = 0.0f;

    // Sun direction (world-space, Z-up).  Points FROM the sun TOWARD the scene
    // origin so it can be used directly as a directional-light direction.
    float SunDirX = 0.0f, SunDirY = 1.0f, SunDirZ = 0.0f;

    // Normalised sky colour (linear RGB, sum-of-9-directions average radiance,
    // then normalised so the max channel = 1).
    float SkyR = 0.25f, SkyG = 0.45f, SkyB = 1.00f;

    // Normalised sun colour (linear RGB, solar disc radiance at the given
    // elevation, normalised so the max channel = 1).
    float SunR = 1.00f, SunG = 0.95f, SunB = 0.80f;

    // Whether the sun is above the horizon.
    bool SunAboveHorizon = true;
};

// Evaluates the Hosek-Wilkie model from the given settings.
// Returns default sky/sun colours when the sun is below the horizon or the
// SDK fails to initialise (turbidity out of range etc.).
HosekWilkieResult EvaluateHosekWilkie(const TimeOfDaySettings& settings);
