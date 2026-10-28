#pragma once

#include <algorithm>

// Sharpening controls that are independent of any anti-aliasing technique.
struct SharpenSettings
{
    bool  ImageSharpeningEnabled = true;
    float ImageSharpeningStrength = 0.2f;
    // Radius of the unsharp mask, in pixels. 1 is the original 3x3 kernel;
    // larger radii sharpen broader detail, not just pixel-level edges.
    float ImageSharpeningRadius = 2.0f;

    bool  TextureSharpeningEnabled = true;
    float TextureMipLODBias = -1.5f;

    void Validate()
    {
        ImageSharpeningStrength = (std::clamp)(ImageSharpeningStrength, 0.0f, 3.0f);
        ImageSharpeningRadius = (std::clamp)(ImageSharpeningRadius, 1.0f, 8.0f);
        TextureMipLODBias = (std::clamp)(TextureMipLODBias, -3.0f, 0.0f);
    }

    float GetEffectiveTextureMipLODBias() const
    {
        return TextureSharpeningEnabled ? TextureMipLODBias : 0.0f;
    }
};
