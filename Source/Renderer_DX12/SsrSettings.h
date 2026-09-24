#pragma once

// SsrSettings
// Screen-space reflection parameters, edited in the renderer settings window and
// persisted with the scene. Distances are in world units (metres).
struct SsrSettings
{
    bool Enabled = false;

    // Which implementation traces the reflections.
    //   0 = the engine's own world-space ray march (one sharp ray per pixel).
    //   1 = AMD FidelityFX Stochastic Screen Space Reflections: hierarchical-depth
    //       traversal with GGX-sampled rays and AMD's temporal/spatial reflection
    //       denoiser, so glossy surfaces get blurred reflections instead of sharp ones.
    // Intensity, MaxRoughness and DebugView apply to both; the remaining fields each
    // belong to one technique, as grouped below.
    int Technique = 0;

    // Multiplies the reflection before it is added to the scene. 1 = physical strength.
    float Intensity = 1.0f;

    // Ray march budget. Steps grow geometrically, so this buys reach as well as precision.
    int MaxSteps = 48;

    // Length of the first march step, in metres. Smaller resolves contact reflections
    // better and costs more.
    float StepSize = 0.25f;

    // Geometric growth applied to the step each iteration. 1.0 marches uniformly.
    float StepGrowth = 1.05f;

    // How far behind the depth buffer a sample may be and still count as a hit, in metres.
    // Too small and rays tunnel through thin geometry; too large and they attach to the
    // wrong surface.
    float Thickness = 0.5f;

    // Surfaces rougher than this get no screen-space reflection at all - the result would
    // want a blurred cone that a single sharp ray cannot represent.
    float MaxRoughness = 0.6f;

    // Binary-search iterations used to pin down the intersection after a coarse hit.
    int RefineSteps = 6;

    // Longest ray, in metres. Also drives the distance fade.
    float MaxDistance = 60.0f;

    // Normalised screen distance at which reflections start fading toward the border,
    // where there is no longer any on-screen data to reflect.
    float EdgeFadeStart = 0.85f;

    // Ray march: 0 = composite, 1 = reflection contribution only, 2 = confidence mask.
    // FidelityFX:  0 = composite, 1 = denoised reflection only, 2 = raw traced reflection
    //              only (the denoiser is skipped).
    int DebugView = 0;

    // ---- FidelityFX SSSR (Technique 1). Defaults are the SSSR 1.3 sample's. ----

    // How far below the depth buffer a hit may land and still be trusted, in metres.
    // The traversal is exact against the depth pyramid, so this only has to absorb
    // the depth buffer having no thickness; it is far smaller than the ray march's.
    float SssrDepthThickness = 0.015f;

    // Traversal step budget per ray. The main cost control.
    int SssrMaxTraversalIntersections = 128;

    // Rays still running when this few lanes of a wave remain stop early, so one long
    // ray does not hold a whole wave hostage. Mirror rays are exempt.
    int SssrMinTraversalOccupancy = 4;

    // Depth pyramid level glossy rays start on. Higher is cheaper and coarser; mirror
    // rays always start at full resolution.
    int SssrMostDetailedMip = 0;

    // Rays per 2x2 quad on glossy surfaces (1, 2 or 4). The denoiser fills in the rest.
    int SssrSamplesPerQuad = 1;

    // Re-trace pixels skipped by SamplesPerQuad wherever last frame's result was still
    // unstable, spending rays where the denoiser needs them.
    bool SssrTemporalVarianceGuidedTracing = true;
    float SssrTemporalVarianceThreshold = 0.0f;

    // How hard the temporal resolve clips history to the current neighbourhood. Higher
    // is steadier; lower reacts faster and ghosts less.
    float SssrTemporalStability = 0.7f;
};
