#pragma once

// Default exposure, in EV100. AgX is always encoded over its own fixed 16.5-stop
// window, so this is the only control that decides how bright the frame is.
//
// -4 is not arbitrary: it is the EV100 whose exposure scale puts white at the
// same scene-linear value (~1.21) that the old UI-driven log window put it at, so
// existing levels open at roughly the brightness they were authored against. The
// offset from a photographer's numbers is this engine's, not a mistake - lights
// here are normalised against an 800 lm reference rather than authored in cd/m2.
inline constexpr float kAgxDefaultEv100 = -4.0f;

// Widest range the exposure is allowed to occupy. These exist as the clamp an
// auto-exposure meter would settle inside; with no meter yet they simply pin the
// exposure when the user collapses them onto a single value.
inline constexpr float kAgxDefaultEv100Min = -16.0f;
inline constexpr float kAgxDefaultEv100Max = 16.0f;

// How the exposure is decided each frame.
enum class AgxExposureMode : int
{
    // Ev100 is used as authored.
    Manual = 0,
    // A 64-bin log-luminance histogram of the scene meters the exposure, which
    // is then clamped to [Ev100Min, Ev100Max] and eased towards over time.
    AutoHistogram = 1,
};

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

    // Trim on top of Ev100, in stops. 0 = no adjustment, positive = brighter.
    float Exposure = 0.0f;

    // Photographic exposure. Higher = darker, one unit per stop. Used directly
    // in Manual mode; the starting point and the fallback in automatic mode.
    float Ev100 = kAgxDefaultEv100;

    // Range Ev100 is held inside. In automatic mode this is the range the meter
    // is allowed to settle in; collapsing it pins the exposure.
    float Ev100Min = kAgxDefaultEv100Min;
    float Ev100Max = kAgxDefaultEv100Max;

    AgxExposureMode ExposureMode = AgxExposureMode::Manual;

    // --- Automatic exposure (histogram metering) ---------------------------

    // Percentile band of the luminance histogram that is actually averaged.
    // Discarding the tails is what stops a dark corner or a lamp in frame from
    // dragging the whole image with it.
    float AutoExposureLowPercent  = 0.10f;
    float AutoExposureHighPercent = 0.90f;

    // Adaptation rate in stops per second, split by direction the way an eye
    // is: dilating to a dark room is much slower than reacting to a bright one.
    float AutoExposureSpeedUp   = 3.0f;
    float AutoExposureSpeedDown = 1.0f;

    // Bounds of the histogram itself, in log2 luminance. Widening these costs
    // resolution per bin; they only need to span the scene's real range.
    float AutoExposureHistogramLogMin = -10.0f;
    float AutoExposureHistogramLogMax = 6.0f;

    // Scene luminance that the metered average is exposed onto. 0.18 is the
    // photographic middle grey.
    float AutoExposureGreyPoint = 0.18f;

    // 0 = meter the whole frame evenly, 1 = full centre bias.
    float AutoExposureMeteringMask = 0.5f;

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
