#pragma once

#include <algorithm>

// AAR - Analytic-Aware Reconstruction: a spatial upscaler built on the G-Buffer's
// triangle edges (see Data/Shaders/AnalyticAwareReconstruction.hlsl).
//
// The scene renders at a fraction of the output size. Output pixels crossed by a
// geometric edge get the exact share of their footprint each surface covers, clipped
// from the triangles' own screen-space edges, with each surface's colour reconstructed
// only from its own render samples; every other output pixel gets a depth- and
// normal-aware Catmull-Rom that never filters across surfaces. No history and no
// jitter: the result is stable by construction, but texture and shading detail is only
// as fine as the render resolution.
//
// Takes the upscaler slot after DLSS, FSR, XeSS and TAAU, and replaces TAA, SMAA and
// Analytic Edge AA while it runs. Needs SM 6.1 barycentrics, and does not run under
// MSAA (the edge target holds no single triangle there).
struct AarSettings
{
    bool  Enabled = false;

    // Render scale per axis.
    enum Mode : int
    {
        Native = 0,        // 100 %: reconstruction as anti-aliasing only
        UltraQuality = 1,  // 80 %
        Quality = 2,       // 75 %
        Balanced = 3,      // 66.7 %
        ModeCount
    };
    int   Mode = Quality;

    // Edge classification, as for Analytic Edge AA: the relative depth step (in 1/viewZ,
    // beyond the neighbouring plane) and the normal angle that make a geometric edge.
    float DepthThreshold = 0.01f;
    float CreaseAngleDegrees = 60.0f;

    // Reconstruction weights: how far, in relative 1/viewZ, a render sample may sit off
    // the target surface's plane before its weight falls to e^-0.5, and the exponent on
    // the normal agreement.
    float DepthSigma = 0.02f;
    float NormalPower = 4.0f;

    int   DebugView = 0; // 0 = result, 1 = reconstruction path, 2 = output coverage

    static float RenderScale(int mode)
    {
        switch (mode)
        {
        case Native:       return 1.0f;
        case UltraQuality: return 0.8f;
        case Balanced:     return 2.0f / 3.0f;
        case Quality:
        default:           return 0.75f;
        }
    }

    void Validate()
    {
        Mode = std::clamp(Mode, 0, static_cast<int>(ModeCount) - 1);
        DepthThreshold = std::clamp(DepthThreshold, 0.0001f, 1.0f);
        CreaseAngleDegrees = std::clamp(CreaseAngleDegrees, 1.0f, 180.0f);
        DepthSigma = std::clamp(DepthSigma, 0.0001f, 1.0f);
        NormalPower = std::clamp(NormalPower, 0.0f, 64.0f);
        DebugView = std::clamp(DebugView, 0, 2);
    }
};
