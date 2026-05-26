#pragma once

// Bloom settings exposed to both the scene renderer and the Graphics Settings UI.
struct BloomSettings
{
    bool  Enabled         = true;

    // Overall bloom intensity multiplied onto the accumulated scatter result.
    float Intensity       = 0.03f;

    // Threshold (in linear scene luminance) below which pixels do not contribute.
    float Threshold       = 0.9f;

    // Soft knee width around the threshold. 0 = hard cut, 1 = very gradual.
    float Knee            = 0.35f;

    // Radius scale: multiplies the scatter radius at each mip level.
    float Radius          = 1.0f;

    // Number of downsample mip levels. Range [2..8].
    int   MipLevels       = 6;
};
