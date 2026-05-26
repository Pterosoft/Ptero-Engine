#pragma once

// AgX tonemapping settings exposed to both the renderer and the Graphics Settings UI.
struct AgxColorGradeControl
{
    float Total = 0.0f;
    float Red = 0.0f;
    float Green = 0.0f;
    float Blue = 0.0f;
    float Yellow = 0.0f;
};

struct AgxColorGradeRegion
{
    AgxColorGradeControl Contrast{};
    AgxColorGradeControl Gamma{};
    AgxColorGradeControl Gain{};
    AgxColorGradeControl Saturation{};
    AgxColorGradeControl Vibrance{};
};

struct AgxTonemapSettings
{
    bool  Enabled = true;

    // Exposure in EV (stops). 0 = no adjustment.
    float Exposure = 0.0f;

    // AgX input range in EV100.
    float Ev100Min = -1.0f;
    float Ev100Max = 16.0f;

    // Toe and shoulder slope adjustments (0.0 = flat, 1.0 = full).
    float ToeStrength      = 0.95f;
    float ShoulderStrength = 1.05f;

    AgxColorGradeRegion Global{};
    AgxColorGradeRegion Shadows{};
    AgxColorGradeRegion Midtones{};
    AgxColorGradeRegion Highlights{};

    AgxTonemapSettings()
    {
        Global.Contrast.Total = 0.10f;
        Global.Saturation.Total = 0.12f;
    }
};
