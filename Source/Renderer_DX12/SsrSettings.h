#pragma once

// SsrSettings
// Screen-space reflection parameters, edited in the renderer settings window and
// persisted with the scene. Distances are in world units (metres).
struct SsrSettings
{
    bool Enabled = false;

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

    // 0 = composite, 1 = reflection contribution only, 2 = confidence mask.
    int DebugView = 0;
};
