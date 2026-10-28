#pragma once

// RtGISettings.h
// Tunable parameters for the custom Ray Traced Global Illumination (RTGI) pass.
// These settings are exposed through the Graphics Settings window in the editor
// and passed to RtGlobalIllumination::Dispatch() each frame.
struct RtGISettings
{
    // Master toggle – when false the RTGI pass is skipped entirely.
    bool Enabled = true;

    // Enables NVIDIA NRD RELAX diffuse denoising for the RTGI result.
    bool UseNrdDenoiser = true;

    // Number of GI rays fired per pixel during the initial sampling pass.
    // Higher values improve quality but reduce performance. Range [1..8].
    int RaysPerPixel = 1;

    // Resolution the diffuse GI is traced and denoised at, as a divisor of the frame's:
    // 1 = full, 2 = half (a quarter of the rays), 4 = quarter (a sixteenth). A
    // depth- and normal-aware upsample brings it back to the frame; diffuse bounce
    // light is smooth enough that half rarely shows.
    int DiffuseResolution = 1;

    // Resolution the specular reflection rays are traced at: 1 = full, 2 = half.
    // Reflections carry sharp detail, so they stop at half.
    int SpecularResolution = 1;

    // Maximum indirect bounce depth for GI rays. Range [1..4].
    int MaxBounces = 2;

    // Explicitly samples the sun at the secondary hit using a shadow ray.
    // Improves direct-light transport in GI at the cost of extra ray work.
    bool NextEventEstimation = true;

    // Answers next-event visibility at a bounce hit from the virtual shadow map
    // where it holds a page fine enough, and traces the shadow ray only where it
    // does not. The map already exists for direct lighting, so a lookup is far
    // cheaper than a ray, and it sees what the raster shadows see - alpha-tested
    // foliage included. Needs the virtual shadow map to be active.
    bool VsmVisibility = true;

    // Coarsest shadow-map texel, in metres, trusted at a bounce hit. A coarse
    // page leaks light through anything about as thin as its texels - a wall
    // between a sunlit exterior and a room - so hits that only coarse pages
    // cover trace a ray instead.
    float VsmMaxTexelSize = 0.04f;

    // Next-event estimation at a bounce hit tests the visibility of LightSamples lights,
    // picked in proportion to how much each would add unshadowed, instead of every light
    // in reach. Unbiased, and the cost stops growing with the number of lights; the price
    // is a little noise in the GI where several similar lights overlap, which the
    // denoiser mostly absorbs. Off (or no more lights in reach than samples) tests them
    // all, as before.
    bool StochasticLights = true;
    int  LightSamples = 2;

    // ── Acceleration structure detail ─────────────────────────────────────
    // Meshes that have a virtualized-geometry cluster DAG are ray traced
    // against a static cut through it rather than their full source triangles.
    // GI is low frequency, so the lost detail does not show; the BLAS, its
    // build and the hit-shading geometry pool all shrink with the cut.
    bool VirtualGeometryBlas = true;

    // Simplification error, in metres, of the cut used for the nearest tier.
    // Keep it under a centimetre: the primary GI ray re-finds its origin on
    // the traced surface within max(1 cm, 1% of the distance).
    float BlasLodError = 0.005f;

    // Distance, in metres, at which the second tier starts. Each tier starts
    // four times further out than the one before and allows four times the
    // error. Range [1 .. 200].
    float BlasTierDistance = 15.0f;

    // Number of distance tiers. 1 = one cut for every distance. Range [1..3].
    int BlasTierCount = 3;

    // ── ReSTIR temporal resampling ─────────────────────────────────────────
    // Reuses GI path samples from previous frames via reservoir resampling.
    bool TemporalReuseEnabled = false;

    // Maximum number of frames a reservoir sample is allowed to persist.
    // Larger = less noise, slower response to changes. Range [1..32].
    int MaxHistoryLength = 10;

    // Relative depth tolerance for matching surfaces between frames.
    // Range [0.001 .. 0.5].
    float DepthThreshold = 0.1f;

    // Minimum dot-product between surface normals for a temporal match.
    // Range [0.0 .. 1.0] (lower = more permissive).
    float NormalThreshold = 0.5f;

    // ── ReSTIR spatial resampling ──────────────────────────────────────────
    // Shares GI reservoirs with screen-space neighbours.
    bool SpatialReuseEnabled = false;

    // Number of spatial neighbour samples to combine per pixel. Range [1..8].
    int SpatialSamples = 4;

    // Sampling radius in screen pixels for the spatial neighbourhood search.
    // Range [1..32].
    float SpatialRadius = 20.0f;

    // ── Firefly suppression ───────────────────────────────────────────────
    // Clamp incoming radiance to this value. Set to 0 to disable clamping.
    float RadianceClamp = 10.0f;

    // ── Temporal accumulation denoiser ───────────────────────────────────
    // Blend between current-frame GI output (1.0) and accumulated history (0.0).
    // Lower values = smoother but ghost more on motion. Range [0.0 .. 1.0].
    float AccumulationBlend = 1.0f;

    // --- NVIDIA NRD RELAX diffuse denoiser ---
    // The time-based accumulation is converted to frame counts using the current FPS.
    float NrdMaxAccumulationTime = 0.5f;   // NRD's RELAX_DEFAULT_ACCUMULATION_TIME
    float NrdDisocclusionThreshold = 0.2f;
    int   NrdAtrousIterations = 5;
    float NrdSharpenAmount = 0.35f;

    // Intensity multiplier applied to the GI output before compositing.
    float GiIntensity = 1.0f;

    // Saturation boost applied to bounced diffuse transport.
    // 1.0 = physically based material tint, higher values exaggerate color bleed.
    float ColorLeakIntensity = 1.0f;

    // ── Specular reflections ──────────────────────────────────────────────
    // Enable GGX specular reflection rays on surfaces with roughness below the threshold.
    bool SpecularEnabled = true;

    // Surfaces with roughness above this value use the probe SH fallback instead of
    // a dedicated specular ray. Range [0.0 .. 1.0].
    float SpecularRoughnessThreshold = 0.6f;

    // Intensity multiplier for the specular reflection output.
    float SpecularIntensity = 0.10f;

    // ── Debug ─────────────────────────────────────────────────────────────
    // 0 = final lit, 1 = raw GI radiance, 2 = GI luminance,
    // 3 = specular reflections only, 4 = radiance probe debug (see RadianceProbeSettings)
    int DebugView = 0;

    // ── Ray visualisation overlay ─────────────────────────────────────────
    // Records the full GI paths of a sparse set of pixels - every bounce, the
    // sun shadow ray at each hit and optionally the camera ray that found the
    // surface - and draws them as lines over the lit image, depth tested.
    // Independent of DebugView; costs nothing while off.
    bool RayDebugEnabled = false;

    // Stops recording and keeps drawing what was captured, so the camera can
    // fly around the frozen rays and show them from the side.
    bool RayDebugFreeze = false;

    // 0 = a grid of pixels RayDebugSpacing apart, 1 = the centre pixel only.
    int RayDebugPattern = 0;

    // Pixels between recorded pixels in the grid pattern. Raised automatically
    // when the grid would not fit the capture buffer. Range [8 .. 512].
    int RayDebugSpacing = 64;

    // Frames of rays kept on screen, newest replacing oldest. With the centre
    // pattern this fills the hemisphere in over time. Range [1 .. 256].
    int RayDebugHistoryFrames = 1;

    // 0 = coloured by bounce, 1 = the radiance travelling back along each ray.
    int RayDebugColorMode = 0;

    bool RayDebugShowShadowRays = true;
    bool RayDebugShowCameraRays = false;

    // Line brightness; 1 is about as bright as a sunlit white surface.
    float RayDebugBrightness = 1.0f;

    // Line width in pixels. Range [0.5 .. 8].
    float RayDebugLineWidth = 1.5f;

    // Drawn length, in metres, of rays that escape to the sky and of sun
    // shadow rays (both are effectively infinite).
    float RayDebugMissLength = 4.0f;
    float RayDebugShadowLength = 1.0f;
};
