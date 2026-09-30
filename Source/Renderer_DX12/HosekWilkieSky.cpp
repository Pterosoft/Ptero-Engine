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
    constexpr float kPi       = 3.14159265358979f;
    constexpr float kDegToRad = kPi / 180.0f;

    float SmoothStep(float edge0, float edge1, float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    // The sky as a rotation. A body at hour angle h and declination d sits at
    // c = (cos d cos h, cos d sin h, sin d) in equatorial coordinates, and
    // world = Rows * c places it over the observer: the latitude tilts the
    // celestial pole up from the northern horizon, then NorthOffset turns
    // east/north onto world X/Y.
    //
    // The observer frame is east = +X, north = +Y, up = +Z. The sun is kept at
    // an equinox (declination 0), so it rises due east at 06:00, stands due
    // south at noon at 90 - latitude degrees - toward -Y, where the old
    // elevation-only sun always sat - and sets due west at 18:00. The old model
    // only raised and lowered the sun on the southern meridian, so 09:00 and
    // 15:00 lit the scene identically and the sun never crossed the sky.
    struct SkyFrame
    {
        float Rows[3][3];

        SkyFrame(float latitudeDeg, float northOffsetDeg)
        {
            const float lat = std::clamp(latitudeDeg, -89.0f, 89.0f) * kDegToRad;
            const float sinLat = std::sin(lat), cosLat = std::cos(lat);
            // Equatorial -> east / north / up.
            const float east[3]  = { 0.0f, -1.0f, 0.0f };
            const float north[3] = { -sinLat, 0.0f, cosLat };
            const float up[3]    = { cosLat, 0.0f, sinLat };

            const float a = northOffsetDeg * kDegToRad;
            const float cosA = std::cos(a), sinA = std::sin(a);
            for (int i = 0; i < 3; ++i)
            {
                Rows[0][i] = cosA * east[i] - sinA * north[i];
                Rows[1][i] = sinA * east[i] + cosA * north[i];
                Rows[2][i] = up[i];
            }
        }

        // Unit vector from the scene toward a body at hour angle h, declination d.
        void Toward(float hourAngle, float declination, float out[3]) const
        {
            const float c[3] =
            {
                std::cos(declination) * std::cos(hourAngle),
                std::cos(declination) * std::sin(hourAngle),
                std::sin(declination)
            };
            for (int i = 0; i < 3; ++i)
                out[i] = Rows[i][0] * c[0] + Rows[i][1] * c[1] + Rows[i][2] * c[2];
        }

        // World -> the frame the stars are fixed in, at the given solar hour
        // angle. Stars keep pace with the sun (solar rather than sidereal time),
        // which is indistinguishable within a night and keeps a given hour's
        // sky the same on every visit.
        void StarRotation(float hourAngle, float out[9]) const
        {
            const float cosH = std::cos(hourAngle), sinH = std::sin(hourAngle);
            for (int k = 0; k < 3; ++k)
            {
                // Columns of Rows are the equatorial axes in world space.
                const float col0 = Rows[k][0], col1 = Rows[k][1], col2 = Rows[k][2];
                out[0 + k] =  cosH * col0 + sinH * col1;
                out[3 + k] = -sinH * col0 + cosH * col1;
                out[6 + k] = col2;
            }
        }
    };

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

namespace
{
    // Normalised Hosek-Wilkie daylight colours at the given solar elevation.
    // Below the horizon the model is undefined, so it is evaluated just above it;
    // the caller blends into night from there.
    void EvaluateDaylightColours(const TimeOfDaySettings& settings, float elevRad, HosekWilkieResult& result)
    {
        const double elevation = static_cast<double>((std::max)(elevRad, 0.2f * kDegToRad));

        // --- Sun disc colour from Rayleigh scattering approximation ---
        // Sampling sky radiance at gamma=0 returns blue-dominated scattered sky colour,
        // not the actual sun disc chromaticity. Instead derive disc colour from elevation:
        // at the horizon (long atmospheric path) the sun is deep orange; near zenith it is white.
        const float t = static_cast<float>(std::sin(elevation)); // 0 at horizon, ~1 at zenith
        result.SunR = 1.0f;
        result.SunG = 0.70f + 0.30f * t;  // 0.70 at horizon -> 1.00 at zenith
        result.SunB = 0.35f + 0.65f * t;  // 0.35 at horizon -> 1.00 at zenith

        // Clamp turbidity to valid SDK range [1, 10].
        const double turbidity = settings.Turbidity < 1.0f ? 1.0 : (settings.Turbidity > 10.0f ? 10.0 : static_cast<double>(settings.Turbidity));
        const double albedo    = settings.GroundAlbedo < 0.0f ? 0.0 : (settings.GroundAlbedo > 1.0f ? 1.0 : static_cast<double>(settings.GroundAlbedo));

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
            return; // keep the defaults
        }

        // --- Sky colour ---
        float skyR = 0, skyG = 0, skyB = 0;
        SampleAverageSkyRadiance(states, elevation, skyR, skyG, skyB);
        result.SkyR = skyR; result.SkyG = skyG; result.SkyB = skyB;
        if (!NormaliseRGB(result.SkyR, result.SkyG, result.SkyB))
        {
            result.SkyR = 0.25f; result.SkyG = 0.45f; result.SkyB = 1.0f; // fallback
        }

        for (int i = 0; i < 3; ++i)
            arhosekskymodelstate_free(states[i]);
    }

    // The exposure the hour asks for: DayEv100 with the sun 25 degrees up or
    // more, SunsetEv100 with it on the horizon, NightEv100 from 12 degrees below
    // (the end of nautical twilight).
    float ExposureForElevation(const TimeOfDaySettings& settings, float elevRad)
    {
        if (elevRad >= 0.0f)
            return Lerp(settings.SunsetEv100, settings.DayEv100, SmoothStep(0.0f, 25.0f * kDegToRad, elevRad));
        return Lerp(settings.NightEv100, settings.SunsetEv100, SmoothStep(-12.0f * kDegToRad, 0.0f, elevRad));
    }
}

HosekWilkieResult EvaluateHosekWilkie(const TimeOfDaySettings& settings)
{
    HosekWilkieResult result{};

    // ---- Where the sun and the moon are --------------------------------------
    const SkyFrame sky(settings.Latitude, settings.NorthOffset);

    const float hours = std::fmod(std::fmod(settings.TimeOfDay, 24.0f) + 24.0f, 24.0f);
    const float sunHourAngle = (hours - 12.0f) / 24.0f * 2.0f * kPi;

    float towardSun[3];
    sky.Toward(sunHourAngle, 0.0f, towardSun);
    const float sunElev = std::asin(std::clamp(towardSun[2], -1.0f, 1.0f));

    // The moon trails the sun by its phase: new moon rides with the sun, a full
    // moon sits opposite it. Its lit fraction follows from the same angle.
    const float phase = settings.MoonPhase - std::floor(settings.MoonPhase);
    float towardMoon[3];
    sky.Toward(sunHourAngle - phase * 2.0f * kPi, 0.0f, towardMoon);
    const float moonElev = std::asin(std::clamp(towardMoon[2], -1.0f, 1.0f));

    result.SolarElevationRad = sunElev;
    result.SunAboveHorizon   = sunElev > 0.0f;
    result.SolarDirX = -towardSun[0];  result.SolarDirY = -towardSun[1];  result.SolarDirZ = -towardSun[2];
    result.MoonDirX  = -towardMoon[0]; result.MoonDirY  = -towardMoon[1]; result.MoonDirZ  = -towardMoon[2];
    result.MoonElevationRad = moonElev;
    result.MoonIllumination = 0.5f * (1.0f - std::cos(phase * 2.0f * kPi));
    sky.StarRotation(sunHourAngle, result.StarRotation);

    EvaluateDaylightColours(settings, sunElev, result);

    // ---- Exposure --------------------------------------------------------------
    // Evaluated whether or not it drives the tonemapper: the GI pre-exposure
    // follows the same curve either way, so night GI keeps daylight magnitudes
    // even with a hand-set exposure.
    const float ev100 = ExposureForElevation(settings, sunElev);
    result.Ev100 = ev100;
    result.ControlsExposure = settings.Enabled && settings.ControlExposure;
    result.PreExposure = settings.Enabled
        ? std::clamp(std::exp2(settings.DayEv100 - ev100), 1.0f / 256.0f, 65536.0f)
        : 1.0f;

    // Time of day off: no sun, moon, sky or stars. The directions above are
    // still valid for anything that wants one.
    if (!settings.Enabled)
    {
        result.SunDirX = result.SolarDirX; result.SunDirY = result.SolarDirY; result.SunDirZ = result.SolarDirZ;
        return result;
    }

    // ---- Sun ---------------------------------------------------------------------
    // Fades out as the disc goes under, so the light does not snap off at 0 degrees
    // while it still grazes walls facing it.
    const float sunFade = SmoothStep(-0.5f * kDegToRad, 4.0f * kDegToRad, sunElev);
    const float sunLux  = (std::max)(settings.SunIntensityLux, 0.0f) / kTimeOfDayRefSunLux;
    const float sunColour[3] =
    {
        settings.OverrideSunColor ? settings.SunColorR : result.SunR,
        settings.OverrideSunColor ? settings.SunColorG : result.SunG,
        settings.OverrideSunColor ? settings.SunColorB : result.SunB,
    };

    // ---- Moon --------------------------------------------------------------------
    // Takes over as the directional light only once the sun is down. Both lights
    // are black at the handover (the sun below -0.5 degrees, the moon above -1),
    // so the light direction can jump from one to the other without a pop.
    const float nightness = 1.0f - SmoothStep(-8.0f * kDegToRad, -1.0f * kDegToRad, sunElev);
    const float moonUp    = SmoothStep(-0.5f * kDegToRad, 4.0f * kDegToRad, moonElev);
    const float moonLux   = settings.MoonEnabled
        ? (std::max)(settings.MoonIntensityLux, 0.0f) / kTimeOfDayRefSunLux : 0.0f;
    const float moonLight = moonLux * result.MoonIllumination * moonUp * nightness;

    result.KeyLightIsMoon = settings.MoonEnabled && sunElev < -0.75f * kDegToRad;
    if (result.KeyLightIsMoon)
    {
        result.SunDirX = result.MoonDirX; result.SunDirY = result.MoonDirY; result.SunDirZ = result.MoonDirZ;
        result.LightR = settings.MoonColorR * moonLight;
        result.LightG = settings.MoonColorG * moonLight;
        result.LightB = settings.MoonColorB * moonLight;
    }
    else
    {
        result.SunDirX = result.SolarDirX; result.SunDirY = result.SolarDirY; result.SunDirZ = result.SolarDirZ;
        result.LightR = sunColour[0] * sunLux * sunFade;
        result.LightG = sunColour[1] * sunLux * sunFade;
        result.LightB = sunColour[2] * sunLux * sunFade;
    }

    // ---- Sky ambient -------------------------------------------------------------
    // Full daylight from 25 degrees up, as before; below that it falls away roughly
    // as the real sky does and hands over to the night sky by the end of nautical
    // twilight. The daylight colour is pulled toward deep blue once the sun is under.
    const float dayWeight = SmoothStep(-12.0f * kDegToRad, 25.0f * kDegToRad, sunElev);
    const float daylight  = dayWeight * dayWeight;
    result.DaylightWeight = daylight;

    float dayColour[3] =
    {
        settings.OverrideSkyColor ? settings.SkyColorR : result.SkyR,
        settings.OverrideSkyColor ? settings.SkyColorG : result.SkyG,
        settings.OverrideSkyColor ? settings.SkyColorB : result.SkyB,
    };
    const float blueHour = SmoothStep(0.0f, -6.0f * kDegToRad, sunElev);
    const float blueHourColour[3] = { 0.30f, 0.42f, 1.00f };
    for (int i = 0; i < 3; ++i)
        dayColour[i] = Lerp(dayColour[i], blueHourColour[i], blueHour);

    const float skyLux   = (std::max)(settings.SkyIntensityLux, 0.0f) / kTimeOfDayRefSkyLux;
    const float nightLux = (std::max)(settings.NightSkyIntensityLux, 0.0f) / kTimeOfDayRefSkyLux;
    const float nightColour[3] = { settings.NightSkyColorR, settings.NightSkyColorG, settings.NightSkyColorB };
    result.AmbientR = dayColour[0] * skyLux * daylight + nightColour[0] * nightLux * (1.0f - daylight);
    result.AmbientG = dayColour[1] * skyLux * daylight + nightColour[1] * nightLux * (1.0f - daylight);
    result.AmbientB = dayColour[2] * skyLux * daylight + nightColour[2] * nightLux * (1.0f - daylight);

    // ---- Sky pass: discs and stars -----------------------------------------------
    const float sunDiscFade = SmoothStep(-1.0f * kDegToRad, 0.5f * kDegToRad, sunElev);
    result.SunDiscR = sunColour[0] * sunLux * sunDiscFade;
    result.SunDiscG = sunColour[1] * sunLux * sunDiscFade;
    result.SunDiscB = sunColour[2] * sunLux * sunDiscFade;

    // At the default intensity the disc reads as a bright object at night
    // exposure, the way the sun disc does by day; the gain is there to decouple
    // the two if they ever need to differ. It is added to the sky rather than
    // drawn over it, so in daylight it
    // is the faint pale moon it should be and never a dark hole.
    constexpr float kMoonDiscGain = 1.0f;
    const float moonDiscFade = SmoothStep(-1.0f * kDegToRad, 0.5f * kDegToRad, moonElev);
    const float moonDisc = moonLux * kMoonDiscGain * moonDiscFade;
    result.MoonDiscR = settings.MoonColorR * moonDisc;
    result.MoonDiscG = settings.MoonColorG * moonDisc;
    result.MoonDiscB = settings.MoonColorB * moonDisc;

    // Stars come out through nautical twilight, and scale with the night sky so
    // the two stay in proportion however bright the night is authored.
    result.StarVisibility = settings.StarsEnabled
        ? SmoothStep(-4.0f * kDegToRad, -14.0f * kDegToRad, sunElev) : 0.0f;
    result.StarRadiance = nightLux * 30.0f * (std::max)(settings.StarIntensity, 0.0f);

    // What an open outdoor view should meter at: ground of typical albedo under
    // the directional light and the sky, averaged with the sky itself. Only the
    // ratio to what the camera actually meters matters, and differences under a
    // stop are ignored, so this needs to be about right, not exact.
    const auto luma = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };
    const float horizontalIlluminance =
        luma(result.LightR, result.LightG, result.LightB) * (std::max)(-result.SunDirZ, 0.0f)
        + luma(result.AmbientR, result.AmbientG, result.AmbientB);
    constexpr float kTypicalAlbedo = 0.3f;
    result.AdaptationReferenceLogLuminance = std::log2((std::max)(kTypicalAlbedo * horizontalIlluminance, 1e-8f));

    return result;
}
