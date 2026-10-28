#pragma once

#include <string>

// Physically based lens flares: ghost reflections traced through a real lens prescription
// (Data/LensFlares/Lenses/*.xml), plus an aperture-diffraction starburst on each light.
// Based on Hullin et al. 2011 and the reference implementation in
// Source/SDKs/LensFlareFramework-master. See LensFlareRenderer.h.
struct LensFlareSettings
{
    bool  Enabled = false;

    // File name (without extension) of the lens under Data/LensFlares/Lenses.
    std::string Lens = "heliar-tronnier";

    // Overall multiplier on everything below. 1 = the energy a real coated lens reflects;
    // real flares are faint, so games usually run this well above 1.
    float Intensity = 1.0f;

    // ---- Ghosts ----------------------------------------------------------------------
    // Tuned on the bundled lenses so a noon sun gives clearly visible but not veiling
    // ghosts; zoom lenses (more surfaces, more ghosts) come out brighter, as real ones do.
    float GhostIntensity = 25.0f;

    // f-number of the virtual lens. Stopping down shrinks the aperture, which makes the
    // ghosts smaller, sharper-edged and more clearly shaped like the iris. 0 = the lens
    // file's own f-number.
    float FNumber = 0.0f;

    // Aperture shape shared by the ghosts and the starburst.
    int   ApertureBlades = 6;
    float ApertureRotation = 15.0f;   // degrees
    float ApertureRoundness = 0.15f;  // 0 = straight blades, 1 = circular iris

    // The brightest N two-bounce ghosts of the lens are drawn (ranked once per lens).
    int   MaxGhosts = 64;

    // Rays per side of each ghost's grid. Higher resolves sharper caustics.
    int   RayGridSize = 32;

    // Wavelengths traced per ghost (1 = monochrome, 3 = RGB fringes, up to 6).
    int   Wavelengths = 3;

    // ---- Starburst -------------------------------------------------------------------
    float StarburstIntensity = 1.0f;
    // Height of the starburst sprite as a fraction of the screen height.
    float StarburstSize = 0.35f;

    // ---- Light sources ---------------------------------------------------------------
    bool  SunFlares = true;
    bool  LocalLightFlares = true;
    // Most lights flaring at once, including the sun.
    int   MaxLights = 4;
    // A local light flares only when the light it throws on the lens is at least this
    // bright (engine light units; the noon sun is about 1).
    float LocalLightThreshold = 0.02f;
    // A light hidden this far (metres) behind the depth buffer still counts as visible,
    // so a bulb inside its own lamp mesh is not occluded by that mesh.
    float OcclusionDepthTolerance = 0.15f;
    // Sun only: dim the flare when the sun disc is darker than expected (clouds, fog).
    bool  SunCloudOcclusion = true;
};
