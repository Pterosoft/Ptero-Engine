#include "pch.h"

#include "FirstPersonCharacter.h"

void FirstPersonCharacter::ConfigureCharacter(CharacterSettings& settings)
{
    // Every value is spelled out, even where it matches the engine default, so this is the
    // one place to look when tuning the C++ controller.
    CharacterMovementSettings& movement = settings.Movement;
    movement.WalkSpeed = 4.0f;
    movement.SprintSpeed = 7.0f;
    movement.CrouchSpeed = 2.0f;
    movement.Acceleration = 45.0f;
    movement.Braking = 30.0f;
    movement.AirControl = 0.35f;
    movement.JumpVelocity = 5.2f;
    movement.Gravity = 15.0f;
    movement.MaxFallSpeed = 50.0f;
    movement.JumpReleaseDamping = 0.5f;
    movement.MaxJumpCount = 1;
    movement.CoyoteTime = 0.12f;
    movement.JumpBufferTime = 0.12f;
    movement.CapsuleRadius = 0.35f;
    movement.StandingHeight = 1.8f;
    movement.CrouchedHeight = 1.1f;
    movement.StepHeight = 0.4f;
    movement.MaxSlopeDegrees = 50.0f;
    movement.KillDepth = 500.0f;

    CharacterCameraSettings& camera = settings.Camera;
    camera.FieldOfView = 70.0f;
    camera.SprintFovBoost = 6.0f;
    camera.EyeHeight = 1.65f;
    camera.CrouchedEyeHeight = 0.95f;
    camera.BlendSpeed = 12.0f;
    camera.HeadBobAmount = 0.03f;
    camera.HeadBobFrequency = 0.9f;
    camera.LandingDip = 0.012f;
    camera.LandingDipMax = 0.18f;
    camera.MinPitch = -88.0f;
    camera.MaxPitch = 88.0f;

    CharacterControlSettings& controls = settings.Controls;
    controls.MouseSensitivity = 0.12f;
    controls.InvertY = false;
    controls.CrouchToggle = false;
    controls.SprintToggle = false;
    controls.CaptureMouse = true;
}

void FirstPersonCharacter::Begin(const GameCameraState& spawn, CharacterTraceFn trace, void* traceUser)
{
    mMovement = CharacterMovement{};
    ConfigureCharacter(mMovement.Settings);
    mMovement.SetTrace(trace, traceUser);
    mMovement.SpawnAtEye(spawn.PositionX, spawn.PositionY, spawn.PositionZ, spawn.Pitch, spawn.Yaw);

    mJumpWasDown = false;
    mCrouchWasDown = false;
    mSprintWasDown = false;
    mCrouchToggled = false;
    mSprintToggled = false;
}

void FirstPersonCharacter::Update(const GameFrameContext& frame)
{
    const GameInputState& input = frame.Input;
    const CharacterControlSettings& controls = mMovement.Settings.Controls;

    // Look first, so this frame's movement already heads where the player now faces.
    // Screen +Y is down, so moving the mouse up (negative Y) looks up.
    if (input.LookActive)
    {
        const float lookUp = -input.LookDeltaY * (controls.InvertY ? -1.0f : 1.0f);
        mMovement.AddLookInput(input.LookDeltaX * controls.MouseSensitivity, lookUp * controls.MouseSensitivity);
    }

    float forward = 0.0f;
    float right = 0.0f;
    if (input.MoveForward)  forward += 1.0f;
    if (input.MoveBackward) forward -= 1.0f;
    if (input.MoveRight)    right += 1.0f;
    if (input.MoveLeft)     right -= 1.0f;
    mMovement.AddMovementInput(forward, right);

    // Jump is edge-triggered: holding Space does not bunny-hop, and letting go early
    // cuts the jump short.
    if (input.Jump && !mJumpWasDown)
        mMovement.Jump();
    else if (!input.Jump && mJumpWasDown)
        mMovement.StopJumping();
    mJumpWasDown = input.Jump;

    bool crouch = input.Crouch;
    if (controls.CrouchToggle)
    {
        if (input.Crouch && !mCrouchWasDown)
            mCrouchToggled = !mCrouchToggled;
        crouch = mCrouchToggled;
    }
    mCrouchWasDown = input.Crouch;
    if (crouch)
        mMovement.Crouch();
    else
        mMovement.UnCrouch();

    bool sprint = input.Sprint;
    if (controls.SprintToggle)
    {
        if (input.Sprint && !mSprintWasDown)
            mSprintToggled = !mSprintToggled;
        // A toggled sprint ends when the player stops, the way most shooters do it.
        if (forward <= 0.0f)
            mSprintToggled = false;
        sprint = mSprintToggled;
    }
    mSprintWasDown = input.Sprint;
    // Crouching wins over sprinting: the component only sprints while standing.
    mMovement.SetSprinting(sprint);

    mMovement.Update(frame.DeltaSeconds);
}

GameCameraState FirstPersonCharacter::GetCamera() const
{
    const CharacterView view = mMovement.GetView();

    GameCameraState camera{};
    camera.PositionX = view.Position[0];
    camera.PositionY = view.Position[1];
    camera.PositionZ = view.Position[2];
    camera.Pitch = view.Pitch;
    camera.Yaw = view.Yaw;
    camera.FieldOfView = view.FieldOfView;
    return camera;
}
