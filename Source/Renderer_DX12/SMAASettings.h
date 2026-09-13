#pragma once

// SMAA-style spatial anti-aliasing settings exposed to the renderer and UI.
struct SMAASettings
{
    bool  Enabled = false;
    float EdgeThreshold = 0.05f;
    float MaxSearchSteps = 16.0f;
    float MaxSearchStepsDiag = 8.0f;
    float CornerRounding = 25.0f;
    int   DebugView = 0; // 0 = final output, 1 = edges, 2 = blend weights
};
