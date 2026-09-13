#pragma once

// WindSettings
// ------------
// One scene-wide description of the wind, shared by everything that reacts to
// it.  Rain previously carried its own wind vector on RainComponent; that is
// now driven from here so a scene cannot end up with rain blowing one way and
// trees leaning the other.
//
// Strength is expressed in metres per second so it reads as a real quantity
// rather than an arbitrary slider, and the gust terms are layered on top:
// the instantaneous wind a shader sees is
//
//   Direction * (Strength + GustAmplitude * gust(worldPos, time))
//
// where gust() is a low-frequency noise scrolling across the world, so a gust
// travels through a field of grass instead of every blade moving in lockstep.

#include <DirectXMath.h>

#include <cmath>

struct WindSettings
{
    // Horizontal wind heading in radians, measured counter-clockwise from
    // world +X.  Kept as an angle rather than a vector so the editor can
    // expose a compass dial and so the direction cannot be denormalised.
    float DirectionRadians = 0.6f;

    // Sustained wind speed, metres per second.  Roughly: 2 = light air that
    // barely moves leaves, 8 = branches in motion, 15 = whole trees swaying.
    float Strength = 4.0f;

    // Peak additional speed contributed by gusts, metres per second.
    float GustAmplitude = 2.5f;

    // How quickly gusts pulse, in Hz.  Low values read as long rolling swells.
    float GustFrequency = 0.25f;

    // Size of one gust cell in metres.  This is what decides whether a gust
    // looks like a breeze crossing a meadow or like the whole field twitching
    // at once; it should be comfortably larger than the visible area.
    float GustWavelength = 40.0f;

    // Global multiplier on all vegetation bending, so an artist can dial the
    // whole scene's motion down without editing every layer.
    float VegetationBendScale = 1.0f;

    // Speed of the high-frequency leaf flutter, in Hz.
    float FlutterFrequency = 2.2f;

    bool Enabled = true;

    // Convenience for the passes that still want a plain vector.
    DirectX::XMFLOAT3 GetDirectionVector() const
    {
        return DirectX::XMFLOAT3(
            std::cos(DirectionRadians),
            std::sin(DirectionRadians),
            0.0f);
    }

    // Sustained wind as a velocity vector, metres per second.  Rain consumes
    // this directly in place of its old per-component wind fields.
    DirectX::XMFLOAT3 GetVelocityVector() const
    {
        const float speed = Enabled ? Strength : 0.0f;
        return DirectX::XMFLOAT3(
            std::cos(DirectionRadians) * speed,
            std::sin(DirectionRadians) * speed,
            0.0f);
    }
};
