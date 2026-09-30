#pragma once

#include "GameAPI.h"

// The C++ player controller: turns the host's per-frame input into calls on the engine's
// CharacterMovement component, and reports where the eye camera is.
//
// This is the code-side twin of the Node Graph controller that Game Settings can export
// (FirstPersonController.nodegraph). Both drive the same component with the same rules,
// so switching between them in Game Settings changes who is in charge, not how the
// character feels:
//
//   WASD      move, relative to where the camera faces
//   Mouse     look (while the mouse is captured, or with the right button held)
//   Space     jump - tap for a hop, hold for the full height
//   Ctrl / C  crouch (hold, or toggle in ConfigureCharacter)
//   Shift     sprint (hold, or toggle in ConfigureCharacter); only while moving forward
class FirstPersonCharacter
{
public:
    // `spawn` is the eye pose to start from.
    void Begin(const GameCameraState& spawn, CharacterTraceFn trace, void* traceUser);

    // The character's movement, camera and control values - what the Node Graph
    // controller sets with its Set Movement/Camera/Control Settings nodes. Tune the C++
    // controller here.
    static void ConfigureCharacter(CharacterSettings& settings);
    void Update(const GameFrameContext& frame);

    GameCameraState GetCamera() const;

    CharacterMovement& GetMovement() { return mMovement; }
    const CharacterMovement& GetMovement() const { return mMovement; }

private:
    CharacterMovement mMovement;

    // Last frame's buttons, for turning held state into presses and releases.
    bool mJumpWasDown = false;
    bool mCrouchWasDown = false;
    bool mSprintWasDown = false;

    // Current on/off state while crouch or sprint is set to toggle.
    bool mCrouchToggled = false;
    bool mSprintToggled = false;
};
