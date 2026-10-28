#pragma once

// PathTracerSettings.h
// Tunable parameters for the reference path tracer (PathTracer.h).
//
// The path tracer is a viewing mode, not part of a level's look: it replaces the
// lit scene with a progressively refined, unbiased-by-default render of the same
// scene, accumulated for as long as nothing changes. It is not saved with the level.

enum class PathTracerMode : int
{
    // Accumulates full paths from the camera for as long as nothing changes:
    // converges to the ground truth, noisy while anything moves.
    Reference = 0,
    // One diffuse and one specular path per pixel per frame from the G-Buffer
    // surface, denoised by NVIDIA NRD (RELAX): stable in motion, interactive.
    RealTime = 1,
};

struct PathTracerSettings
{
    // Master toggle. Needs a DXR-capable GPU; it shares the RTGI acceleration structure.
    bool Enabled = false;

    PathTracerMode Mode = PathTracerMode::RealTime;

    // ---- Real time --------------------------------------------------------
    // Bounces after the G-Buffer surface. Each is a ray per lobe per pixel per
    // frame; 1 is one-bounce GI and reflections. Range [0..8].
    int RealTimeMaxBounces = 2;

    // Paths per lobe per pixel each frame, averaged before denoising. More is less
    // noise for proportionally more GPU time. Range [1..4].
    int RealTimeSamplesPerPixel = 1;

    // Paths that reach their last bounce on a surface that was on screen last frame
    // take that frame's denoised lighting there instead of stopping. Over a few
    // frames this gives unlimited bounces for no extra rays - what keeps interiors
    // lit only by bounced light from going black - at the cost of a little lag.
    bool ScreenCache = true;

    // NRD RELAX denoising. Off shows the raw one-path-per-lobe signal.
    bool Denoise = true;

    // Frames NRD accumulates history over. More is smoother and slower to follow
    // lighting changes. Range [4..60].
    int DenoiserHistoryFrames = 30;

    // ---- Reference ---------------------------------------------------------

    // Bounces after the camera ray's first hit. 0 = direct lighting only. Range [0..32].
    int MaxBounces = 6;

    // Paths traced per pixel each frame. Higher converges faster per frame and
    // costs proportionally more GPU time. Range [1..16].
    int SamplesPerFrame = 1;

    // Accumulation stops once this many samples per pixel have been gathered,
    // and the image is held. 0 = keep refining forever. Range [0..65536].
    int MaxSamples = 4096;

    // Luminance cap on each sample's indirect (bounced) light, in scene-linear
    // units. Removes fireflies at the cost of a little energy; 0 = off (unbiased).
    float IndirectClamp = 20.0f;

    // Multiplier on material emissive colour. The raster path does not draw
    // emissive at all, so it only lights the scene here.
    float EmissiveIntensity = 1.0f;

    // Rays that leave the scene downward - past the edge of the terrain, or in a
    // level without one - would otherwise see the sky from below. They see a flat
    // diffuse ground of this albedo instead, lit by the sun and sky without
    // shadows. 0 = black. Range [0..1].
    float GroundAlbedo = 0.2f;

    // Light sources sized like the real thing: the sun as a disc of its true
    // angular size and spherical / rect lights over their area, for soft shadows.
    // Off treats every light as a point, as the raster shadows do.
    bool SoftShadows = true;

    // Shadow rays leaving displaced terrain march its height maps, so sand ripples
    // and small dunes shadow themselves; the traced terrain is the undisplaced mesh.
    // 16 height lookups per shadow ray on terrain.
    bool TerrainDisplacementShadows = false;

    // Alpha-cut materials let light through their cut-away parts. Costs a
    // material lookup on every candidate hit; off treats them as solid.
    bool AlphaTest = true;

    // Set to throw away the accumulated image on the next frame.
    bool ResetAccumulation = false;

    // 0 = final image; 1 = first-hit base colour; 2 = first-hit shading normal;
    // 3 = first-hit roughness (R) / metallic (G). Debug views are not accumulated.
    // Real time only: 4 = diffuse lighting, 5 = specular lighting - what the
    // denoiser filters, before the surface colour is applied.
    int DebugView = 0;
};
