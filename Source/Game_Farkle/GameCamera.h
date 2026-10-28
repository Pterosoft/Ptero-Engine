#pragma once

#include "GameAPI.h"

// The camera the game drives while a play session is running. It shares the engine's
// left-handed Z-up basis with EditorCamera so poses can be handed back and forth, but the
// movement itself is game code: motion is yaw-relative (looking up does not make you fly),
// speed is smoothed instead of snapping on and off, and sprint scales the target speed.
class GameCamera
{
public:
    void Reset(const GameCameraState& state);
    void Update(const GameFrameContext& frame);

    GameCameraState GetState() const;

    void SetMovementSpeed(float metersPerSecond);
    float GetMovementSpeed() const { return mMovementSpeed; }

private:
    float mPositionX = 0.0f;
    float mPositionY = 0.0f;
    float mPositionZ = 0.0f;
    float mPitch = 0.0f;
    float mYaw = 0.0f;

    // Current smoothed velocity in world space, so starts and stops ease in and out.
    float mVelocityX = 0.0f;
    float mVelocityY = 0.0f;
    float mVelocityZ = 0.0f;

    float mMovementSpeed = 6.0f;
    float mSprintMultiplier = 2.5f;
    float mLookSensitivity = 0.0035f;
    // Higher values reach the target velocity sooner; this is the exponential rate used
    // per second, not a per-frame lerp factor, so the feel is frame-rate independent.
    float mAccelerationRate = 14.0f;
};
