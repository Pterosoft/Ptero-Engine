#pragma once

// TAA settings exposed to both the scene renderer and the Graphics Settings UI.
struct TaaSettings
{
    bool  Enabled = true;
    float BlendFactor = 0.5f;        // weight of current frame; lower = more temporal accumulation
    float JitterScale = 1.0f;        // sub-pixel jitter magnitude in pixels; 0 = no jitter
    bool  UseSharpening = true;     // optional unsharp-mask pass applied on top of the resolved output
    float SharpeningStrength = 0.2f; // sharpening amount when UseSharpening is true

    // Set to true by the scene renderer when the camera has moved this frame so the TAA
    // resolve ignores stale history (blend factor forced to 1.0 for one frame).
    // Automatically cleared by TAARenderer::Resolve after it is consumed.
    bool  ResetHistory = false;
};
