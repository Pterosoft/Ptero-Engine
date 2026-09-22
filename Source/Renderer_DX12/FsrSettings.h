#pragma once

#include <string>

struct FsrSettings
{
    // ---- Upscaling ----
    // Mutually exclusive with DLSS: when both are switched on, DLSS wins.
    bool  Enabled = false;
    int   Mode = 1; // 0=Native AA, 1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance
    // Robust Contrast-Adaptive Sharpening, run by FSR on its own output.
    bool  Sharpening = true;
    float Sharpness = 0.5f;

    // ---- Frame generation ----
    // Presents one interpolated frame between every pair of rendered ones. Works with
    // or without FSR upscaling. Needs the swap chain replaced by FSR's proxy, which
    // the renderer does at the next frame boundary.
    bool FrameGeneration = false;
    bool FrameGenerationDebugTearLines = false;
    bool FrameGenerationDebugResetIndicators = false;
    bool FrameGenerationDebugView = false;

    bool ResetHistory = false;
};

// What the renderer found at runtime, for the settings panel. Not saved.
struct FsrRuntimeStatus
{
    bool        ApiAvailable = false;
    bool        UpscalerActive = false;
    bool        FrameGenerationActive = false;
    // DLSS is on too, so the upscaler is not the one running.
    bool        OverriddenByDlss = false;
    unsigned    RenderWidth = 0;
    unsigned    RenderHeight = 0;
    unsigned    OutputWidth = 0;
    unsigned    OutputHeight = 0;
    std::string UpscalerVersion;
    std::string FrameGenerationVersion;
    std::string LastError;
};
