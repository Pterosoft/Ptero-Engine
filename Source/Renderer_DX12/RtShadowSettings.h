#pragma once

// RtShadowSettings.h
// Tunable parameters for the ray-traced sun shadow pass (RtShadows).
// Exposed through the Graphics Settings window in the editor and passed to
// RtShadows::Dispatch() each frame.

struct RtShadowSettings
{
    // Master toggle - when false the sun is shadowed by the (virtual) shadow map.
    bool Enabled = false;

    // Also ray trace point, spot and rect lights that cast shadows: up to
    // kMaxLocalLightsPerPixel of them per pixel, the rest keep their shadow maps.
    bool LocalLights = true;

    // Multiplies every local light's emitter size (Source Radius, or a rect light's
    // panel) for its ray-traced shadow. 0 = hard shadows. Range [0..4].
    float LocalLightSoftness = 1.0f;

    static constexpr int kMaxLocalLightsPerPixel = 8;

    // Shadow rays per pixel each frame, jittered across the sun's disc. Range [1..8].
    int RaysPerPixel = 1;

    // Apparent diameter of the sun in degrees. The real sun is about 0.53; larger is
    // a softer, hazier sun. 0 = hard shadows. Range [0..10].
    float SunAngularDiameter = 0.53f;

    // Longest shadow ray, in metres. Casters further away than this cast nothing.
    float MaxRayLength = 500.0f;

    // Offset of the ray origin along the surface normal, in metres, against
    // self-shadowing acne. Range [0.0001..0.1].
    float RayBias = 0.01f;

    // Weight of the current frame against the reprojected history. Lower = smoother
    // penumbrae but more lag behind moving casters. Range [0.02..1].
    float TemporalBlend = 0.1f;

    // Widest blur, in pixels, applied across a penumbra. 0 = no spatial filter.
    int MaxFilterRadius = 8;

    // 0 = off, 1 = denoised sun visibility, 2 = raw one-frame sun visibility,
    // 3 = darkest ray-traced local light visibility, 4 = the same before denoising
    int DebugView = 0;
};
