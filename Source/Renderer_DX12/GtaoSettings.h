#pragma once

// GtaoSettings.h
// Tunable parameters for the XeGTAO (Ground Truth Ambient Occlusion) pass.
// Exposed through the Graphics Settings window in the editor.

struct GtaoSettings
{
    // Master toggle – when false the XeGTAO pass is skipped entirely.
    bool Enabled = true;

    // Quality level: 0=Low, 1=Medium, 2=High, 3=Ultra
    int QualityLevel = 3;

    // World-space radius of the occlusion sphere. Range [0.01..5.0].
    float Radius = 1.0f;

    // Number of spatial denoise passes: 0=none, 1=sharp, 2=soft.
    int DenoisePasses = 0;

    // Final visibility power. Higher = darker occlusion. Range [0.5..5.0].
    float FinalValuePower = 2.55f;

    // Falloff range for samples outside the effect radius. Range [0.0..1.0].
    float FalloffRange = 0.615f;

    // Sample distribution power (smaller crevices vs large surfaces). Range [0.5..4.0].
    float SampleDistributionPower = 2.0f;

    // Thin occluder compensation heuristic. Range [0.0..0.7].
    float ThinOccluderCompensation = 0.0f;

    // Multiplier applied to the radius for screen-space coverage. Range [0.5..3.0].
    float RadiusMultiplier = 1.457f;

    // Depth MIP sampling offset. Range [0.0..6.0].
    float DepthMIPSamplingOffset = 3.30f;

    // Blend intensity applied on top of GBuffer AO. Range [0.0..1.0].
    float Intensity = 1.0f;

    // 0 = composite into lighting, 1 = visualise AO only
    int DebugView = 0;
};
