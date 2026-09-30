#pragma once

// Virtual shadow map (VirtualShadowMapRenderer), for the sun and for shadow-casting
// point, spot and rect lights. With Enabled off the sun falls back to the single 2K
// ShadowMapRenderer map and lights to the PointShadowMapRenderer cubemaps.
struct VirtualShadowMapSettings
{
    bool  Enabled = true;

    // Shadow local lights through the map as well (a paged 4096^2 cube each, up to 16).
    // Off leaves them on the cubemap atlas.
    bool  LocalLights = true;
    // Like ResolutionLodBias, for the local lights' cube mips.
    float LocalResolutionBias = 0.0f;

    // Level 0 texel size, in metres. Every further level doubles it.
    float FirstLevelTexelSize = 0.004f;
    // Clipmap levels in use (1..16). Points no level covers are unshadowed.
    int   LevelCount = 13;
    // Shifts the level each pixel reads: +1 = half the resolution, a quarter of the pages.
    float ResolutionLodBias = 0.0f;

    // Physical page pool, in 128^2 pages. 2048 pages = 8192 x 4096 D32 = 128 MB.
    // Changing it rebuilds the pool.
    int   PhysicalPages = 2048;
    // Most pages rendered in one frame. The rest wait (coarser levels stand in).
    int   MaxPagesPerFrame = 160;

    // Metres of light-space depth kept either side of the camera. Casters further toward
    // the sun than this are clamped onto the near plane and still cast.
    float DepthRange = 2000.0f;
    // A sun rotation smaller than this (degrees) keeps the cached pages.
    float LightRotationThreshold = 0.1f;

    // Receiver-side bias, both in texels of the level being sampled.
    float NormalOffset = 1.5f;
    float ConstantBias = 1.0f;
    // Caster-side slope bias for the page render pipelines.
    float SlopeScaledDepthBias = 2.0f;

    // 0 off, 1 clipmap level tint, 2 raw sun visibility.
    int   DebugView = 0;
    // Re-render every page every frame (for comparing against the cache).
    bool  DisableCaching = false;
};
