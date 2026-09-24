#pragma once

// SubsurfaceSettings
// Scene-wide subsurface scattering controls, edited in the renderer settings window and
// persisted with the level. What each surface scatters like is a material property
// (MaterialDefinition::UseSubsurfaceScattering and friends); these only choose how the
// renderer evaluates it.
struct SubsurfaceSettings
{
    // Master switch. With no subsurface material on screen the passes are skipped
    // regardless, so leaving this on costs nothing in scenes that do not use it.
    bool Enabled = true;

    // 0 = Screen space: Jimenez et al.'s separable SSS, a two-pass depth-aware blur of the
    //     diffuse lighting. Cheap and stable; cannot see past the screen edge or
    //     around silhouettes, and measures transmission thickness from shadow maps.
    // 1 = Ray traced, world space: scatter samples are found by probing the real mesh
    //     with rays from a disc around each pixel and lit there by every light with
    //     ray-traced shadows - nothing is read from the screen, so scattering follows
    //     ears, noses and fingers and works behind silhouettes. Denoised edge-aware
    //     afterwards; transmission thickness is measured through the mesh itself.
    //     Needs DXR and falls back to screen space without it.
    int Mode = 0;

    // Screen-space kernel size: 0 = 11 taps, 1 = 17 taps, 2 = 25 taps per direction.
    int Quality = 1;

    // Screen space: stop the blur at depth discontinuities so it does not bleed across
    // a silhouette onto whatever is behind it.
    bool FollowSurface = true;

    // Back-lighting through thin parts. The per-material Translucency sets how much.
    bool Transmission = true;
    float TransmissionIntensity = 1.0f;

    // Ray traced: surface probes per pixel. Rotated every frame, so temporal AA
    // integrates them; 8-16 is plenty under TAA/DLSS/FSR.
    int RtSamples = 12;

    // 0 = composite, 1 = scattered diffuse only, 2 = subsurface mask.
    int DebugView = 0;
};
