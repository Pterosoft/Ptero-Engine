#pragma once
#include "GameAPI.h"
#include <array>
namespace FarkleConfig {
inline constexpr int WinningScore=3000, ThreeOnes=1000;
// Background playlist size: the host resolves "Music1".."Music5" against the FMOD banks.
inline constexpr int MusicTracks=5;
// Online, a player has this long for each decision before the AI plays the rest of their turn.
inline constexpr float OnlineTurnSeconds=30.f;
inline constexpr float Radians=0.017453292519943295f, RollSeconds=1.25f, CameraSeconds=2.4f;
// Cameras are the editor's camera pose as its statistics overlay shows it (position, then
// pitch and yaw in degrees - a yaw past a full turn is the same view, so whole turns are
// dropped). Both are framed on the tavern's Table at (-18.86, -0.87): re-take them from the
// overlay if the table moves.
//
// The play camera looks almost straight down at the table (overlay -77.7, -1170.8). Its
// view ray crosses the table top at about (-18.84, -0.69), just off the table's centre,
// which is where the throw lands (AimThrow).
inline constexpr GameCameraState TableCamera{-18.37f,-0.68f,3.40f,-77.7f*Radians,-90.8f*Radians};
// Top of the Table mesh (SM_MET_Table) above its origin, in mesh units; times the entity's
// Z scale it is the table top's height. Dice land on it whatever height their authored
// rest pose has.
inline constexpr float TableMeshTop=0.702f;
// Half the height of the dice_low mesh, in mesh units (it is ~0.029 across), so a landed
// die sits on the table rather than in it. Scaled by each die's own scale.
inline constexpr float DieMeshHalfHeight=0.0145f;
// Landing grid for a throw: 3 dice across the screen, 2 deep, centred on what the camera
// is actually pointing at. Centre to centre, in world units: the dice are ~0.06 across at
// their authored scale of 2, so this leaves about a die's width between them.
inline constexpr float DiceSpacing=0.12f;
// Nudges the landing spot off dead-centre, in world units along the screen axes, for when
// the clear area between the UI panels is not quite the middle of the screen.
inline constexpr float ThrowOffsetRight=0.0f, ThrowOffsetUp=0.0f;
// The default shot, for the menu and the result screens: across the room at the table from
// a seated height (overlay -7.8, -1113.5).
inline constexpr GameCameraState ResultCamera{-16.69f,-4.39f,2.05f,-7.8f*Radians,-33.5f*Radians};
// Calibrate for dice_low: local Euler XYZ radians, entry 0=face 1 up, entry 5=face 6 up.
// These are defaults, not verified face directions of the imported mesh.
inline constexpr std::array<std::array<float,3>,6> FaceRotations{{
    {{0,0,0}}, {{90*Radians,0,0}}, {{0,-90*Radians,0}},
    {{0,90*Radians,0}}, {{-90*Radians,0,0}}, {{180*Radians,0,0}}
}};
// Shape of the throw itself: how far a die winds back along its own path before it is
// released, how high it arcs on the way to its landing slot, and the settle bounce. In
// world units, for ~0.06 dice under a camera ~2.2 above the table.
inline constexpr float ThrowDistance=0.25f, ThrowHeight=0.45f, BounceHeight=0.04f;
}
