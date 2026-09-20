#pragma once
#include "GameAPI.h"
#include <array>
namespace FarkleConfig {
inline constexpr int WinningScore=3000, ThreeOnes=1000;
// Background playlist size: the host resolves "Music1".."Music5" against the FMOD banks.
inline constexpr int MusicTracks=5;
inline constexpr float Radians=0.017453292519943295f, RollSeconds=1.25f, CameraSeconds=2.4f;
// The camera looks almost straight down at the table, so PositionY slides the view
// sideways: lower Y moves it right on screen. This value lands the view ray at
// (1.620, -2.789) where it crosses the dice plane, which sits just off the Table entity's
// origin (0.207, -2.791) and is what centres the table in frame - the table's visible top
// is not centred on its origin, hence the offset.
// Moves with the table: the Table entity was shifted -8 on X, so PositionX went 10.81 ->
// 2.81 to keep the framing identical.
inline constexpr GameCameraState TableCamera{2.81f,-2.82f,17.87f,-81.4f*Radians,-88.5f*Radians};
// Landing grid for a throw: 3 dice across the screen, 2 deep, centred on what the camera
// is actually pointing at. At this framing one world unit is roughly 120 screen pixels.
inline constexpr float DiceSpacing=0.62f;
// Nudges the landing spot off dead-centre, in world units along the screen axes, for when
// the clear area between the UI panels is not quite the middle of the screen.
inline constexpr float ThrowOffsetRight=0.0f, ThrowOffsetUp=0.0f;
// The result shot looks across the room at the table from a low angle; its ray passes
// within ~1.3 units of the table top, so it is framed on the table too and tracks the same
// -8 X shift (20.05 -> 12.05).
inline constexpr GameCameraState ResultCamera{12.05f,-18.40f,10.54f,-6.1f*Radians,-40.4f*Radians};
// Calibrate for dice_low: local Euler XYZ radians, entry 0=face 1 up, entry 5=face 6 up.
// These are defaults, not verified face directions of the imported mesh.
inline constexpr std::array<std::array<float,3>,6> FaceRotations{{
    {{0,0,0}}, {{90*Radians,0,0}}, {{0,-90*Radians,0}},
    {{0,90*Radians,0}}, {{-90*Radians,0,0}}, {{180*Radians,0,0}}
}};
// Shape of the throw itself: how far a die winds back along its own path before it is
// released, how high it arcs on the way to its landing slot, and the settle bounce.
inline constexpr float ThrowDistance=0.8f, ThrowHeight=0.8f, BounceHeight=0.14f;
}
