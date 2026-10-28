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
    // Terrain casts into the map (TerrainRenderer::RenderShadowDepthPages).
    bool  TerrainShadows = true;
    // ...tessellated and displaced as it is drawn, so ripples and small dunes cast
    // too. Far dearer: every page tessellates the terrain under it. Off casts the
    // undisplaced mesh.
    bool  TerrainDisplacementShadows = false;
    // Like ResolutionLodBias, for the local lights' cube mips.
    float LocalResolutionBias = 0.0f;

    // --- Local-light shadow budget (DX12SceneRenderer::SelectShadowedLocalLights) ---
    // Each trades a little quality for speed with many shadow-casting lights, and each
    // can be switched off to get the full-quality behaviour back.
    //
    // Only the MaxShadowedLights lights that matter most to the view (brightness over
    // squared distance, nothing outside the frustum) cast shadows; the rest light
    // unshadowed. A light entering or leaving the set fades over ShadowFadeSeconds, and
    // a light already in it is favoured so the set does not flicker.
    bool  ShadowBudget = true;
    int   MaxShadowedLights = 8;
    float ShadowFadeSeconds = 0.4f;
    // Lights past the first FullResolutionLights of that ranking read (and request) their
    // cube ReducedResolutionBias mips coarser: softer shadows, fewer pages.
    bool  ImportanceResolution = true;
    int   FullResolutionLights = 4;
    float ReducedResolutionBias = 1.0f;
    // A 2x2 shadow filter instead of the 4x4 for receivers farther than
    // CheapFilterDistance from the camera, and everywhere for the reduced lights above.
    bool  CheapDistantFiltering = true;
    float CheapFilterDistance = 20.0f;

    // --- Soft shadows (PCSS; VirtualShadowMap.hlsli, PteroVsmVisibilitySoft) ---
    // Penumbrae that widen with distance from the caster, from the size of the light:
    // the sun's angular diameter, and each local light's emitter (its Source Radius, or
    // a rect light's extents). Contact shadows stay sharp. Off = the fixed 4x4 filter.
    bool  SoftShadows = true;
    // Degrees. The real sun is about 0.53; larger is a hazier, softer sun.
    float SunAngularDiameter = 0.53f;
    // Multiplies every local light's emitter size for its shadow.
    float LocalLightSoftness = 1.0f;
    // Emitter radius, in metres, assumed for lights with a smaller Source Radius (a
    // point light is 0), so their shadows soften with distance too. 0 keeps them hard.
    float MinLocalLightSize = 0.03f;
    // Widest penumbra, in metres. Bounds the blocker search and the filter.
    float MaxPenumbra = 0.6f;

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
