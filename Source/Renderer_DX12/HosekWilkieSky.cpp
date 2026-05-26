#include "pch.h"
#include "HosekWilkieSky.h"
#include "TimeOfDaySettings.h"

// The SDK is plain C, so we need extern "C" to prevent C++ name mangling.
extern "C" {
#include "../SDKs/HosekWilkie/ArHosekSkyModel.h"
}

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace
{
    // Convert a time-of-day value [0,24) to solar elevation in radians.
    // We use a simplified model: solar noon = max elevation of 60° (mid-latitude summer),
    // sunrise ≈ 6h, sunset ≈ 18h.  This gives a plausible diurnal arc without
    // requiring latitude/longitude input.
    float TimeToSolarElevation(float timeHours)
    {
        // Map [0,24) to a normalised day fraction centred on noon (0.5).
        // sin-based model: elevation = maxElevation * sin(pi * (t - sunrise) / dayLength)
        constexpr float kSunrise    = 6.0f;
        constexpr float kSunset     = 18.0f;
        constexpr float kDayLength  = kSunset - kSunrise;
        constexpr float kMaxElevDeg = 60.0f; // approximate mid-latitude summer noon

        if (timeHours <= kSunrise || timeHours >= kSunset)
            return -0.1f; // slightly below horizon

        const float t = (timeHours - kSunrise) / kDayLength; // [0,1]
        const float sinE = std::sin(static_cast<float>(M_PI) * t);
        return sinE * (kMaxElevDeg * static_cast<float>(M_PI / 180.0));
    }

    // Compute sun direction from elevation (radians).
    // We fix the azimuth to south (Y+ in Z-up space) for simplicity.
    void ElevationToDirection(float elevRad,
        float& outX, float& outY, float& outZ)
    {
        // Direction from scene toward the sun.
        outX = 0.0f;
        outY = -std::cos(elevRad); // south horizon component (pointing away from scene)
        outZ = std::sin(elevRad);  // up component
        // Normalise (already unit length for pure elevation, but be safe).
        float len = std::sqrt(outX * outX + outY * outY + outZ * outZ);
        if (len > 1e-6f) { outX /= len; outY /= len; outZ /= len; }
        // Negate: convention is direction FROM sun TO scene (like a directional light vector).
        outX = -outX; outY = -outY; outZ = -outZ;
    }

    // Normalise an RGB triplet so the brightest channel = 1.
    // Returns false if the colour is black.
    bool NormaliseRGB(float& r, float& g, float& b)
    {
        float mx = r;
        if (g > mx) mx = g;
        if (b > mx) mx = b;
        if (mx < 1e-12f) return false;
        r /= mx; g /= mx; b /= mx;
        return true;
    }

    // Sample the sky model at several zenith/azimuth directions and
    // return the average radiance as an RGB triplet.
    void SampleAverageSkyRadiance(
        ArHosekSkyModelState* states[3],
        double solarElevation,
        float& outR, float& outG, float& outB)
    {
        // Sample a uniform set of directions over the hemisphere.
        // theta = zenith angle (0 = straight up), gamma = angle from sun.
        // We use a simple ring pattern for a quick average.
        const double sunTheta = M_PI / 2.0 - solarElevation; // sun's zenith angle

        double accumR = 0, accumG = 0, accumB = 0;
        int    count  = 0;

        // 5 elevation bands × 8 azimuths + zenith = 41 samples.
        const double elevations[] = { 10.0, 25.0, 45.0, 60.0, 80.0 };
        for (double elevDeg : elevations)
        {
            double theta = (90.0 - elevDeg) * M_PI / 180.0;
            for (int ai = 0; ai < 8; ++ai)
            {
                double azimuth = ai * (2.0 * M_PI / 8.0);
                // Compute gamma (angle between this direction and the sun direction).
                // Both sun and sample share the same azimuth reference frame.
                double cosSunEl  = std::cos(solarElevation);
                double sinSunEl  = std::sin(solarElevation);
                double cosTheta  = std::cos(theta);
                double sinTheta  = std::sin(theta);
                double cosGamma  = cosTheta * std::cos(sunTheta)
                                 + sinTheta * std::sin(sunTheta) * std::cos(azimuth);
                cosGamma = cosGamma < -1.0 ? -1.0 : (cosGamma > 1.0 ? 1.0 : cosGamma);
                double gamma = std::acos(cosGamma);

                accumR += arhosek_tristim_skymodel_radiance(states[0], theta, gamma, 0);
                accumG += arhosek_tristim_skymodel_radiance(states[1], theta, gamma, 1);
                accumB += arhosek_tristim_skymodel_radiance(states[2], theta, gamma, 2);
                ++count;
            }
        }
        // Zenith sample.
        {
            double gamma = sunTheta; // angle from sun at zenith
            accumR += arhosek_tristim_skymodel_radiance(states[0], 0.0, gamma, 0);
            accumG += arhosek_tristim_skymodel_radiance(states[1], 0.0, gamma, 1);
            accumB += arhosek_tristim_skymodel_radiance(states[2], 0.0, gamma, 2);
            ++count;
        }

        outR = (count > 0) ? static_cast<float>(accumR / count) : 0.0f;
        outG = (count > 0) ? static_cast<float>(accumG / count) : 0.0f;
        outB = (count > 0) ? static_cast<float>(accumB / count) : 0.0f;
    }
}

HosekWilkieResult EvaluateHosekWilkie(const TimeOfDaySettings& settings)
{
    HosekWilkieResult result{};

    const float elevRad = TimeToSolarElevation(settings.TimeOfDay);
    result.SolarElevationRad = elevRad;
    result.SunAboveHorizon   = elevRad > 0.0f;

    ElevationToDirection(elevRad,
        result.SunDirX, result.SunDirY, result.SunDirZ);

    if (!result.SunAboveHorizon)
    {
        // Night / twilight defaults.
        result.SkyR = 0.02f; result.SkyG = 0.03f; result.SkyB = 0.08f;
        result.SunR = 0.80f; result.SunG = 0.40f; result.SunB = 0.10f;
        return result;
    }

    // Clamp turbidity to valid SDK range [1, 10].
    const double turbidity = settings.Turbidity < 1.0f ? 1.0 : (settings.Turbidity > 10.0f ? 10.0 : static_cast<double>(settings.Turbidity));
    const double albedo    = settings.GroundAlbedo < 0.0f ? 0.0 : (settings.GroundAlbedo > 1.0f ? 1.0 : static_cast<double>(settings.GroundAlbedo));
    const double elevation = static_cast<double>(elevRad);

    // Initialise the RGB (3-channel) sky model.
    ArHosekSkyModelState* states[3] =
    {
        arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo, elevation),
        arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo, elevation),
        arhosek_rgb_skymodelstate_alloc_init(turbidity, albedo, elevation)
    };

    if (!states[0] || !states[1] || !states[2])
    {
        for (int i = 0; i < 3; ++i)
            if (states[i]) arhosekskymodelstate_free(states[i]);
        return result; // return defaults
    }

    // --- Sky colour ---
    float skyR = 0, skyG = 0, skyB = 0;
    SampleAverageSkyRadiance(states, elevation, skyR, skyG, skyB);
    result.SkyR = skyR; result.SkyG = skyG; result.SkyB = skyB;
    if (!NormaliseRGB(result.SkyR, result.SkyG, result.SkyB))
    {
        result.SkyR = 0.25f; result.SkyG = 0.45f; result.SkyB = 1.0f; // fallback
    }

    // --- Sun disc colour from Rayleigh scattering approximation ---
    // Sampling sky radiance at gamma=0 returns blue-dominated scattered sky colour,
    // not the actual sun disc chromaticity. Instead derive disc colour from elevation:
    // at the horizon (long atmospheric path) the sun is deep orange; near zenith it is white.
    const float t = static_cast<float>(std::sin(elevation)); // 0 at horizon, ~1 at zenith
    result.SunR = 1.0f;
    result.SunG = 0.70f + 0.30f * t;  // 0.70 at horizon -> 1.00 at zenith
    result.SunB = 0.35f + 0.65f * t;  // 0.35 at horizon -> 1.00 at zenith
    // Already normalised (SunR == 1.0 always).


    for (int i = 0; i < 3; ++i)
        arhosekskymodelstate_free(states[i]);

    return result;
}
