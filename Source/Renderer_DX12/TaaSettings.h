#pragma once

// TAA settings exposed to both the scene renderer and the Graphics Settings UI.
//
// The resolve is a port of Unreal Engine 5's temporal AA (TemporalAA.usf), and
// the controls below are its console variables under their Unreal names.
struct TaaSettings
{
    bool  Enabled = true;

    // r.TemporalAA.Quality: 0 Low, 1 Medium, 2 High, 3 Medium High.
    int   Quality = 2;
    // r.TemporalAACurrentFrameWeight: weight of the current frame once history
    // has converged. Lower accumulates longer; Unreal's default is 0.04.
    float CurrentFrameWeight = 0.04f;
    // r.TemporalAASamples: length of the Halton(2,3) jitter sequence at 100%
    // screen percentage. Lengthened by 1 / screen percentage^2 when upsampling.
    int   Samples = 8;
    // r.TemporalAAFilterSize: size of the reconstruction filter, in pixels.
    float FilterSize = 1.0f;
    // r.TemporalAACatmullRom: Catmull-Rom instead of Gaussian reconstruction weights.
    bool  CatmullRomFilter = false;
    // Sub-pixel jitter magnitude in pixels; 0 = no jitter. Not an Unreal setting.
    float JitterScale = 1.0f;

    // ---- Temporal upsampling (TAAU, r.TemporalAA.Upsampling) ----
    // Renders at ScreenPercentage of the output and lets TAA reconstruct the full
    // resolution, like DLSS/FSR/XeSS. Those take precedence when they are on.
    bool  Upscaling = false;
    // r.ScreenPercentage while upsampling, per axis.
    float ScreenPercentage = 67.0f;

    // Deprecated: the weight of the old same-pixel blend, kept so older levels load.
    float BlendFactor = 0.5f;
    bool  UseSharpening = true;      // deprecated; image sharpening now lives in SharpenSettings
    float SharpeningStrength = 0.2f; // deprecated; image sharpening now lives in SharpenSettings

    // Set to true by the scene renderer when the view jumps so the TAA resolve
    // ignores stale history for one frame. Cleared by TAARenderer::Resolve once used.
    bool  ResetHistory = false;

    void Validate()
    {
        if (Quality < 0) Quality = 0;
        if (Quality > 3) Quality = 3;
        if (CurrentFrameWeight < 0.001f) CurrentFrameWeight = 0.001f;
        if (CurrentFrameWeight > 1.0f) CurrentFrameWeight = 1.0f;
        if (Samples < 1) Samples = 1;
        if (Samples > 255) Samples = 255;
        if (FilterSize < 0.1f) FilterSize = 0.1f;
        if (FilterSize > 3.0f) FilterSize = 3.0f;
        if (ScreenPercentage < 25.0f) ScreenPercentage = 25.0f;
        if (ScreenPercentage > 100.0f) ScreenPercentage = 100.0f;
    }
};
