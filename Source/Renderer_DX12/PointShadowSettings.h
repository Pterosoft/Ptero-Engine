#pragma once

struct PointShadowSettings
{
    int   MapSize = 1024;
    float Bias    = 0.002f;
    float SlopeScaledDepthBias = 2.0f;
    float NormalOffset = 1.0f;
    float SeamBlendDistance = 0.05f;
    int   FilterRadius = 2;
    int   DebugView = 0;
};