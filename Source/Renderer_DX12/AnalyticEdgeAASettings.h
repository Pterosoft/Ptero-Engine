#pragma once

#include <algorithm>

// Analytic Edge AA: frequency-aware anti-aliasing of geometric edges.
//
// The G-Buffer pass stores the screen-space edges of the triangle that won each pixel
// (SV_Target3, AnalyticEdge.hlsli). A fullscreen resolve then finds the pixels where
// the geometry actually breaks - depth that the neighbouring plane does not predict,
// or a crease in the normal - and replaces their single point sample with the exact
// area of the pixel each surface covers, by clipping the pixel square against those
// edges. Pixels with no geometric break are not touched, so shading and texture
// aliasing stay with TAA / SMAA, which combine with this pass.
//
// Needs Shader Model 6.1 barycentrics; without them the G-Buffer stores no edges and
// the pass is skipped. MSAA already resolves geometric edges and also skips it.
struct AnalyticEdgeAASettings
{
    bool  Enabled = false;

    // Smallest relative change in 1/viewZ, beyond what the neighbouring plane predicts,
    // that counts as a geometric edge. Lower finds shallower steps.
    float DepthThreshold = 0.01f;

    // Surfaces whose normals differ by more than this, in degrees, meet at a crease
    // edge even with continuous depth. 180 disables the normal test.
    float CreaseAngleDegrees = 60.0f;

    int   DebugView = 0; // 0 = result, 1 = classification, 2 = coverage

    void Validate()
    {
        DepthThreshold = std::clamp(DepthThreshold, 0.0001f, 1.0f);
        CreaseAngleDegrees = std::clamp(CreaseAngleDegrees, 1.0f, 180.0f);
        DebugView = std::clamp(DebugView, 0, 2);
    }
};
