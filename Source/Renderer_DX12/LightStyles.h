#pragma once

// LightStyles.h
// Animated brightness curves for lights.  A light style is a pure function of
// time that returns a multiplier applied to a light's intensity, so the same
// curve drives the deferred lighting pass, the RTGI point-light array, the
// volumetric fog injection and any particle system that borrows it - none of
// those need to know a style exists.
//
// Two families live here:
//
//   * Pattern styles - the classic id-Software light styles.  A pattern is a
//     string of letters where 'a' is black and 'm' is the light's authored
//     brightness, so 'z' is a little over twice as bright.  The pattern is
//     stepped through at StyleSpeed steps per second.  These are what level
//     designers reach for: they are recognisable, cheap, and they loop.
//
//   * Continuous styles - Fire and Torch.  A stepped pattern reads as a
//     repeating stutter once a flame is the only light in a room, so these
//     sum three octaves of value noise instead.  The result never repeats
//     inside a play session and keeps the slow "breathing" of a real flame
//     under the fast flicker, which is the part a pattern cannot express.

#include <cmath>
#include <cstdint>
#include <string>

enum class LightStyleId : int
{
    None               = 0,   // constant - no modulation at all
    Fire               = 1,   // continuous noise flicker tuned for an open flame
    Torch              = 2,   // continuous, faster and shallower than Fire
    Flicker            = 3,   // classic style 1
    SlowStrongPulse    = 4,   // classic style 2
    Candle             = 5,   // classic style 3
    FastStrobe         = 6,   // classic style 4
    GentlePulse        = 7,   // classic style 5
    FlickerAlt         = 8,   // classic style 6
    CandleAlt          = 9,   // classic style 7
    CandleSlow         = 10,  // classic style 8
    SlowStrobe         = 11,  // classic style 9
    FluorescentFlicker = 12,  // classic style 10
    SlowPulseNoBlack   = 13,  // classic style 11
    Custom             = 14,  // pattern typed by the artist in the inspector
};

inline constexpr int kLightStyleCount = 15;

namespace LightStyles
{
    // One entry per LightStyleId, in declaration order, so the editor combo box
    // and the evaluator stay in step with a single edit.
    struct StyleInfo
    {
        const char* DisplayName;
        // Pattern text for stepped styles; nullptr for continuous ones.
        const char* Pattern;
        // Interpolate between adjacent pattern letters.  Pulses want this;
        // strobes and flickers are defined by their hard edges and must not.
        bool        Interpolate;
        // Default steps per second.  The classic styles all run at 10.
        float       DefaultSpeed;
    };

    inline const StyleInfo& GetStyleInfo(LightStyleId styleId)
    {
        static const StyleInfo kStyles[kLightStyleCount] =
        {
            { "None",                nullptr,                                              false, 10.0f },
            { "Fire",                nullptr,                                              false,  1.0f },
            { "Torch",               nullptr,                                              false,  1.6f },
            { "Flicker",             "mmnmmommommnonmmonqnmmo",                            false, 10.0f },
            { "Slow Strong Pulse",   "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba", true,  10.0f },
            { "Candle",              "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",                 false, 10.0f },
            { "Fast Strobe",         "mamamamamama",                                       false, 20.0f },
            { "Gentle Pulse",        "jklmnopqrstuvwxyzyxwvutsrqponmlkj",                  true,  10.0f },
            { "Flicker (alt)",       "nmonqnmomnmomomno",                                  false, 10.0f },
            { "Candle (alt)",        "mmmaaaabcdefgmmmmaaaammmaamm",                       false, 10.0f },
            { "Candle (slow)",       "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",         false, 10.0f },
            { "Slow Strobe",         "aaaaaaaazzzzzzzz",                                   false, 10.0f },
            { "Fluorescent Flicker", "mmamammmmammamamaaamammma",                          false, 10.0f },
            { "Slow Pulse (no black)","abcdefghijklmnopqrrqponmlkjihgfedcba",              true,  10.0f },
            { "Custom",              nullptr,                                              false, 10.0f },
        };

        const int index = static_cast<int>(styleId);
        if (index < 0 || index >= kLightStyleCount)
            return kStyles[0];
        return kStyles[index];
    }

    inline const char* GetStyleDisplayName(LightStyleId styleId)
    {
        return GetStyleInfo(styleId).DisplayName;
    }

    // ---- Value noise -------------------------------------------------------
    // Deterministic in t, so two runs of the same level flicker identically and
    // a recorded video reproduces.  Style evaluation never touches rand().

    inline float Hash1D(int32_t x)
    {
        uint32_t n = static_cast<uint32_t>(x) * 1664525u + 1013904223u;
        n ^= n >> 16;
        n *= 2246822519u;
        n ^= n >> 13;
        n *= 3266489917u;
        n ^= n >> 16;
        return static_cast<float>(n & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    inline float ValueNoise1D(float x)
    {
        const float floored = std::floor(x);
        const int32_t i = static_cast<int32_t>(floored);
        const float f = x - floored;
        // Smoothstep the interpolant: a linear ramp between samples reads as a
        // visible kink in the light level once it drives an entire room.
        const float u = f * f * (3.0f - 2.0f * f);
        return Hash1D(i) * (1.0f - u) + Hash1D(i + 1) * u;
    }

    // Three octaves is the fewest that still separates the slow breathing of a
    // flame from the fast flutter at its tip.  The offsets keep the octaves
    // from lining up at t = 0 and producing an identical opening frame for
    // every fire in the level.
    inline float FireNoise(float t)
    {
        return 0.55f * ValueNoise1D(t)
             + 0.30f * ValueNoise1D(t * 2.7f + 13.1f)
             + 0.15f * ValueNoise1D(t * 6.3f + 71.7f);
    }

    // ---- Pattern sampling --------------------------------------------------

    // 'a' = 0, 'm' = 1 (the light's authored brightness), 'z' = 2.083.
    inline float PatternLetterToScale(char letter)
    {
        if (letter < 'a' || letter > 'z')
        {
            // Accept upper case so a hand-typed custom pattern still works.
            if (letter >= 'A' && letter <= 'Z')
                letter = static_cast<char>(letter - 'A' + 'a');
            else
                return 1.0f;
        }
        return static_cast<float>(letter - 'a') / 12.0f;
    }

    inline float SamplePattern(const char* pattern, size_t length, float steps, bool interpolate)
    {
        if (pattern == nullptr || length == 0)
            return 1.0f;

        const float wrapped = steps - std::floor(steps / static_cast<float>(length)) * static_cast<float>(length);
        const size_t index = static_cast<size_t>(wrapped) % length;
        const float current = PatternLetterToScale(pattern[index]);
        if (!interpolate)
            return current;

        const float next = PatternLetterToScale(pattern[(index + 1) % length]);
        const float blend = wrapped - std::floor(wrapped);
        return current * (1.0f - blend) + next * blend;
    }

    // ---- Evaluation --------------------------------------------------------

    // Returns the multiplier to apply to a light's intensity this frame.
    //
    // timeSeconds   - scene time; any monotonic clock works.
    // speed         - pattern steps per second, or noise rate for Fire/Torch.
    //                 Values <= 0 freeze the style on its first sample.
    // amplitude     - 0 leaves the light constant, 1 applies the style in full,
    //                 and values above 1 exaggerate it.  Blending toward 1.0
    //                 rather than scaling the result keeps the average
    //                 brightness put as the artist dials the effect in.
    // phaseOffset   - seconds of offset, so two torches in the same room do not
    //                 flicker in lockstep.
    // customPattern - used only by LightStyleId::Custom.
    inline float Evaluate(
        LightStyleId       styleId,
        float              timeSeconds,
        float              speed,
        float              amplitude,
        float              phaseOffset,
        const std::string& customPattern)
    {
        if (styleId == LightStyleId::None || amplitude <= 0.0f)
            return 1.0f;

        const float t = (timeSeconds + phaseOffset) * (speed > 0.0f ? speed : 0.0f);

        float styleValue = 1.0f;
        switch (styleId)
        {
        case LightStyleId::Fire:
        {
            // Centred on 1 and biased slightly bright: a flame spends more time
            // near full output than near its dips, and a symmetric curve reads
            // as a lamp with a loose contact instead.
            const float noise = FireNoise(t);
            styleValue = 0.72f + noise * 0.62f;
            break;
        }
        case LightStyleId::Torch:
        {
            // Shallower and busier than Fire - a torch is a small flame being
            // pushed around, not a body of fire breathing on its own.
            const float noise = FireNoise(t);
            styleValue = 0.84f + noise * 0.34f;
            break;
        }
        case LightStyleId::Custom:
            styleValue = SamplePattern(customPattern.c_str(), customPattern.size(), t, false);
            break;
        default:
        {
            const StyleInfo& info = GetStyleInfo(styleId);
            if (info.Pattern == nullptr)
                return 1.0f;
            size_t length = 0;
            while (info.Pattern[length] != '\0')
                ++length;
            styleValue = SamplePattern(info.Pattern, length, t, info.Interpolate);
            break;
        }
        }

        return 1.0f + (styleValue - 1.0f) * amplitude;
    }
}
