#include "pch.h"

#include "GameCamera.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Stop just short of straight up / straight down so yaw never flips at the poles.
    constexpr float PitchLimit = 1.5607963f; // XM_PIDIV2 - 0.01
    constexpr float TwoPi = 6.2831853f;

    float WrapAngle(float radians)
    {
        radians = std::fmod(radians, TwoPi);
        if (radians > TwoPi * 0.5f)
            radians -= TwoPi;
        else if (radians < -TwoPi * 0.5f)
            radians += TwoPi;
        return radians;
    }
}

void GameCamera::Reset(const GameCameraState& state)
{
    mPositionX = state.PositionX;
    mPositionY = state.PositionY;
    mPositionZ = state.PositionZ;
    mPitch = std::clamp(state.Pitch, -PitchLimit, PitchLimit);
    mYaw = WrapAngle(state.Yaw);
    mVelocityX = 0.0f;
    mVelocityY = 0.0f;
    mVelocityZ = 0.0f;
}

void GameCamera::Update(const GameFrameContext& frame)
{
    // A stalled or rewound clock would otherwise integrate movement backwards.
    const float deltaSeconds = std::clamp(frame.DeltaSeconds, 0.0f, 0.1f);
    const GameInputState& input = frame.Input;

    if (input.LookActive)
    {
        // Dragging right looks right and dragging up looks up, matching the editor camera.
        mYaw = WrapAngle(mYaw - input.LookDeltaX * mLookSensitivity);
        mPitch = std::clamp(mPitch - input.LookDeltaY * mLookSensitivity, -PitchLimit, PitchLimit);
    }

    // Movement is yaw-relative: pitch aims the view, not the feet, so walking while looking
    // down keeps you level instead of driving the camera into the ground.
    const float forwardX = std::sin(mYaw);
    const float forwardY = std::cos(mYaw);
    const float rightX = forwardY;
    const float rightY = -forwardX;

    float inputForward = 0.0f;
    float inputRight = 0.0f;
    float inputUp = 0.0f;
    if (input.MoveForward)  inputForward += 1.0f;
    if (input.MoveBackward) inputForward -= 1.0f;
    if (input.MoveRight)    inputRight += 1.0f;
    if (input.MoveLeft)     inputRight -= 1.0f;
    if (input.MoveUp)       inputUp += 1.0f;
    if (input.MoveDown)     inputUp -= 1.0f;

    float directionX = forwardX * inputForward + rightX * inputRight;
    float directionY = forwardY * inputForward + rightY * inputRight;
    float directionZ = inputUp;

    // Normalize so holding two keys is not faster than holding one.
    const float directionLength =
        std::sqrt(directionX * directionX + directionY * directionY + directionZ * directionZ);
    if (directionLength > 0.0001f)
    {
        const float inverseLength = 1.0f / directionLength;
        directionX *= inverseLength;
        directionY *= inverseLength;
        directionZ *= inverseLength;
    }
    else
    {
        directionX = 0.0f;
        directionY = 0.0f;
        directionZ = 0.0f;
    }

    const float targetSpeed = mMovementSpeed * (input.Sprint ? mSprintMultiplier : 1.0f);
    const float targetVelocityX = directionX * targetSpeed;
    const float targetVelocityY = directionY * targetSpeed;
    const float targetVelocityZ = directionZ * targetSpeed;

    // Exponential approach keeps acceleration independent of the frame rate.
    const float blend = 1.0f - std::exp(-mAccelerationRate * deltaSeconds);
    mVelocityX += (targetVelocityX - mVelocityX) * blend;
    mVelocityY += (targetVelocityY - mVelocityY) * blend;
    mVelocityZ += (targetVelocityZ - mVelocityZ) * blend;

    mPositionX += mVelocityX * deltaSeconds;
    mPositionY += mVelocityY * deltaSeconds;
    mPositionZ += mVelocityZ * deltaSeconds;
}

GameCameraState GameCamera::GetState() const
{
    GameCameraState state{};
    state.PositionX = mPositionX;
    state.PositionY = mPositionY;
    state.PositionZ = mPositionZ;
    state.Pitch = mPitch;
    state.Yaw = mYaw;
    return state;
}

void GameCamera::SetMovementSpeed(float metersPerSecond)
{
    mMovementSpeed = (std::max)(0.0f, metersPerSecond);
}
