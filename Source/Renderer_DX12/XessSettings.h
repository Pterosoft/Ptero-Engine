#pragma once

#include <string>

// Intel XeSS 3 settings, shared by the scene renderer, the Graphics Settings
// panel, the level file and the player's game settings.
struct XessSettings
{
    // ---- Super resolution ----
    // One upscaler at a time: DLSS and FSR take precedence when they are on too.
    bool Enabled = false;
    // 0=Native AA, 1=Ultra Quality Plus, 2=Ultra Quality, 3=Quality,
    // 4=Balanced, 5=Performance, 6=Ultra Performance
    int  Mode = 3;

    // ---- Frame generation ----
    // Presents generated frames between rendered ones through XeSS-FG's proxy swap
    // chain. Works with or without XeSS upscaling. Needs Xe Low Latency, which is
    // switched on with it. FSR frame generation takes precedence when both are on.
    bool FrameGeneration = false;
    // Generated frames per rendered frame. Clamped to what the GPU supports
    // (always 1 on non-Intel GPUs).
    int  InterpolatedFrames = 1;
    // Composite the game UI over generated frames instead of interpolating it.
    bool UiComposition = false;

    // ---- Xe Low Latency ----
    // Paces the CPU so frames do not queue up. Always on with frame generation.
    bool LowLatency = false;

    bool ResetHistory = false;
};

// What the renderer found at runtime, for the settings panel. Not saved.
struct XessRuntimeStatus
{
    bool        SuperResolutionAvailable = false;
    bool        FrameGenerationAvailable = false;
    bool        UpscalerActive = false;
    bool        FrameGenerationActive = false;
    bool        LowLatencyActive = false;
    // DLSS or FSR is on too, so the XeSS upscaler is not the one running.
    bool        OverriddenByOtherUpscaler = false;
    // FSR frame generation is on too and owns the swap chain.
    bool        FrameGenerationOverriddenByFsr = false;
    unsigned    RenderWidth = 0;
    unsigned    RenderHeight = 0;
    unsigned    OutputWidth = 0;
    unsigned    OutputHeight = 0;
    unsigned    MaxInterpolatedFrames = 1;
    std::string SuperResolutionVersion;
    std::string FrameGenerationVersion;
    std::string LastError;
};
