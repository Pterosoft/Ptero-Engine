#pragma once

// ChromaticAberrationSettings
// Lateral chromatic aberration, applied last in the post chain on the tonemapped image.
struct ChromaticAberrationSettings
{
    bool Enabled = false;

    // Channel displacement at the corner of the frame, in pixels. The split scales toward
    // the edges and is zero at the optical centre, so this is the maximum, not a uniform
    // offset. Subtle lens character sits around 1-3; above ~8 it reads as an effect.
    float Strength = 2.0f;

    // Exponent on the normalised distance from centre. 1 ramps linearly; higher values
    // hold the middle of the frame clean and concentrate the fringing at the rim.
    float Falloff = 2.0f;

    // 1 samples the three channels once each - cheapest, and enough at low strength.
    // Higher counts smear along the same line and weight the taps spectrally, which stops
    // the fringe banding into visible red/blue edges once the strength is pushed up.
    int SampleCount = 6;

    // Optical centre in UV space. Off-centre values simulate a decentred lens; the
    // aberration is always zero at this point and grows with distance from it.
    float CenterX = 0.5f;
    float CenterY = 0.5f;
};
