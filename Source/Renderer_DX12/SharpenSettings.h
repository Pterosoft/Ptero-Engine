#pragma once

#include <algorithm>

// Sharpening controls that are independent of any anti-aliasing technique.
struct SharpenSettings
{
    bool  ImageSharpeningEnabled = true;
    float ImageSharpeningStrength = 0.2f;

    bool  TextureSharpeningEnabled = true;
    float TextureMipLODBias = -1.5f;

    void Validate()
    {
        ImageSharpeningStrength = (std::clamp)(ImageSharpeningStrength, 0.0f, 1.5f);
        TextureMipLODBias = (std::clamp)(TextureMipLODBias, -3.0f, 0.0f);
    }

    float GetEffectiveTextureMipLODBias() const
    {
        return TextureSharpeningEnabled ? TextureMipLODBias : 0.0f;
    }
};
