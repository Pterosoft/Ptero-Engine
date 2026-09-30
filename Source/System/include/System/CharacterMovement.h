#pragma once

// CharacterMovement - the engine's first-person character: a kinematic capsule that
// walks, sprints, crouches, jumps and falls, plus the eye camera that rides on it.
//
// This is the equivalent of Unreal's CharacterMovementComponent + camera. It only moves
// the character; deciding *when* to move is the job of a player controller, and the
// engine ships two interchangeable ones that drive the exact same component:
//
//   - FirstPersonCharacter in Game.dll (C++), and
//   - a Node Graph controller (.nodegraph), through the Character nodes.
//
// Both sides compile this header - Game.dll, which must stay free of engine types, and
// the renderer, which runs the Node Graph controller - so it is header-only, has no
// dependencies beyond the standard library and only talks to the world through a trace
// callback. Without one the world is an infinite floor at the spawn height, which is
// enough to try the controller in an empty level.
//
// World space is the engine's: metres, left-handed, Z up, angles in radians with
//   forward = (sin(Yaw) * cos(Pitch), cos(Yaw) * cos(Pitch), sin(Pitch))
//   right   = cross(up, forward) = (-cos(Yaw), sin(Yaw), 0)
// so turning right *decreases* yaw, exactly as the editor camera does.
//
// Collision is ray based rather than a true capsule sweep: the engine has no physics
// module, only a BVH over static meshes plus the terrain heightmap. A fan of horizontal
// rays at knee, waist and head height keeps the capsule out of walls, rays straight down
// find the floor, and one straight up finds the ceiling. Anything lower than StepHeight
// is below the knee rays, so the character walks into it and the floor probe lifts it
// on top - which is how stairs and kerbs are climbed without any special casing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// One hit returned by the world trace. Normal is the surface normal, flipped to face
// back along the ray.
struct CharacterTraceHit
{
    float Distance = 0.0f;
    float Position[3]{};
    float Normal[3]{ 0.0f, 0.0f, 1.0f };
};

// Casts a ray from `origin` along the unit vector `direction` and reports the nearest
// hit within maxDistance. Plain function pointer so it can cross the Game.dll boundary.
using CharacterTraceFn = bool (*)(void* user, const float origin[3], const float direction[3],
                                  float maxDistance, CharacterTraceHit* hit);

// How the capsule moves. Speeds in m/s, accelerations in m/s^2.
struct CharacterMovementSettings
{
    float WalkSpeed = 4.0f;
    float SprintSpeed = 7.0f;
    float CrouchSpeed = 2.0f;
    // How quickly the character reaches the target speed on the ground, and how quickly
    // it stops once the keys are let go.
    float Acceleration = 45.0f;
    float Braking = 30.0f;
    // Fraction of the ground acceleration available while airborne: 0 locks the jump
    // arc, 1 steers in the air as well as on the ground.
    float AirControl = 0.35f;

    // Launch speed of a jump. The apex is JumpVelocity^2 / (2 * Gravity): ~0.9 m here.
    float JumpVelocity = 5.2f;
    float Gravity = 15.0f;
    float MaxFallSpeed = 50.0f;
    // Letting go of jump while still rising multiplies the upward speed by this, so a
    // tap is a hop and a hold is a full jump. 1 disables variable jump height.
    float JumpReleaseDamping = 0.5f;
    // 1 = normal jump, 2 = double jump, ...
    int MaxJumpCount = 1;
    // Grace periods that make jumping feel responsive: jump still works this long after
    // walking off a ledge, and a press this long before landing is remembered.
    float CoyoteTime = 0.12f;
    float JumpBufferTime = 0.12f;

    // The capsule. Heights are measured from the feet.
    float CapsuleRadius = 0.35f;
    float StandingHeight = 1.8f;
    float CrouchedHeight = 1.1f;
    // Tallest ledge the character walks onto without jumping.
    float StepHeight = 0.4f;
    // Steeper floors than this are treated as walls.
    float MaxSlopeDegrees = 50.0f;
    // Below the spawn point by more than this, the character is put back at the spawn.
    float KillDepth = 500.0f;
};

// The eye camera that rides on the capsule.
struct CharacterCameraSettings
{
    // Vertical field of view, in degrees.
    float FieldOfView = 70.0f;
    // Added to the field of view while sprinting, for a sense of speed. 0 disables it.
    float SprintFovBoost = 6.0f;
    // Eye height above the feet while standing and crouched. Crouching blends between
    // the two rather than snapping.
    float EyeHeight = 1.65f;
    float CrouchedEyeHeight = 0.95f;
    // Exponential rate (per second) of the crouch and field-of-view blends.
    float BlendSpeed = 12.0f;
    // Vertical head bob while walking, in metres, and how many bob cycles one metre of
    // walking is worth. 0 amount disables bobbing.
    float HeadBobAmount = 0.03f;
    float HeadBobFrequency = 0.9f;
    // Dip of the camera on landing, per m/s of impact speed, capped at LandingDipMax.
    float LandingDip = 0.012f;
    float LandingDipMax = 0.18f;
    // Look limits, in degrees.
    float MinPitch = -88.0f;
    float MaxPitch = 88.0f;
};

// How the player's input maps onto the character. Only the controllers read these.
struct CharacterControlSettings
{
    // Degrees of turn per pixel of mouse movement.
    float MouseSensitivity = 0.12f;
    bool InvertY = false;
    // Toggle: one press starts, the next stops. Hold: active only while held.
    bool CrouchToggle = false;
    bool SprintToggle = false;
    // Grab the mouse for looking as soon as play starts. Escape releases it, a click in
    // the game view takes it back.
    bool CaptureMouse = true;
};

struct CharacterSettings
{
    CharacterMovementSettings Movement;
    CharacterCameraSettings Camera;
    CharacterControlSettings Controls;
};

// Every setting above by name, which is how the Node Graph's Set Movement/Camera/Control
// Settings and Get/Set Property nodes reach them. A new field needs a row here, and the
// catalogue's parameter and property lists (NodeGraphCatalog.cpp) must be kept in step
// by hand - including the defaults, which mirror the struct initialisers above.
enum class CharacterSettingType
{
    Float,
    Int,
    Bool
};

struct CharacterSettingField
{
    // "Movement", "Camera" or "Controls": the struct the field lives in, and the section
    // of the settings file it is saved under.
    const char* Group;
    const char* Name;
    CharacterSettingType Type;
    float Min;
    float Max;
    const char* Tooltip;
    void* (*Address)(CharacterSettings&);
};

inline const CharacterSettingField* CharacterSettingFields(std::size_t& count)
{
    using T = CharacterSettingType;
    static const CharacterSettingField kFields[] = {
        { "Movement", "Walk Speed", T::Float, 0.0f, 30.0f, "Ground speed without sprint, in m/s.",
          [](CharacterSettings& s) -> void* { return &s.Movement.WalkSpeed; } },
        { "Movement", "Sprint Speed", T::Float, 0.0f, 40.0f, "Ground speed while sprinting, in m/s.",
          [](CharacterSettings& s) -> void* { return &s.Movement.SprintSpeed; } },
        { "Movement", "Crouch Speed", T::Float, 0.0f, 20.0f, "Ground speed while crouched, in m/s.",
          [](CharacterSettings& s) -> void* { return &s.Movement.CrouchSpeed; } },
        { "Movement", "Acceleration", T::Float, 0.0f, 200.0f, "How fast the character reaches its speed, in m/s^2.",
          [](CharacterSettings& s) -> void* { return &s.Movement.Acceleration; } },
        { "Movement", "Braking", T::Float, 0.0f, 200.0f, "How fast it stops once the keys are released, in m/s^2.",
          [](CharacterSettings& s) -> void* { return &s.Movement.Braking; } },
        { "Movement", "Air Control", T::Float, 0.0f, 1.0f, "Share of the acceleration available in the air (0 = none, 1 = full).",
          [](CharacterSettings& s) -> void* { return &s.Movement.AirControl; } },
        { "Movement", "Jump Velocity", T::Float, 0.0f, 30.0f, "Upward launch speed of a jump, in m/s.",
          [](CharacterSettings& s) -> void* { return &s.Movement.JumpVelocity; } },
        { "Movement", "Gravity", T::Float, 0.0f, 100.0f, "Downward acceleration, in m/s^2 (Earth is 9.81).",
          [](CharacterSettings& s) -> void* { return &s.Movement.Gravity; } },
        { "Movement", "Max Fall Speed", T::Float, 1.0f, 200.0f, "Terminal falling speed, in m/s.",
          [](CharacterSettings& s) -> void* { return &s.Movement.MaxFallSpeed; } },
        { "Movement", "Jump Release Damping", T::Float, 0.0f, 1.0f, "Upward speed kept when jump is released early (1 = fixed-height jumps).",
          [](CharacterSettings& s) -> void* { return &s.Movement.JumpReleaseDamping; } },
        { "Movement", "Max Jump Count", T::Int, 1.0f, 5.0f, "Jumps before landing: 2 allows a double jump.",
          [](CharacterSettings& s) -> void* { return &s.Movement.MaxJumpCount; } },
        { "Movement", "Coyote Time", T::Float, 0.0f, 1.0f, "Seconds after walking off a ledge during which jumping still works.",
          [](CharacterSettings& s) -> void* { return &s.Movement.CoyoteTime; } },
        { "Movement", "Jump Buffer Time", T::Float, 0.0f, 1.0f, "Seconds a jump press is remembered before landing.",
          [](CharacterSettings& s) -> void* { return &s.Movement.JumpBufferTime; } },
        { "Movement", "Capsule Radius", T::Float, 0.05f, 2.0f, "Collision radius, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Movement.CapsuleRadius; } },
        { "Movement", "Standing Height", T::Float, 0.2f, 5.0f, "Collision height while standing, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Movement.StandingHeight; } },
        { "Movement", "Crouched Height", T::Float, 0.2f, 5.0f, "Collision height while crouched, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Movement.CrouchedHeight; } },
        { "Movement", "Step Height", T::Float, 0.0f, 1.5f, "Tallest ledge walked onto without jumping, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Movement.StepHeight; } },
        { "Movement", "Max Slope", T::Float, 0.0f, 89.0f, "Steepest walkable floor, in degrees.",
          [](CharacterSettings& s) -> void* { return &s.Movement.MaxSlopeDegrees; } },
        { "Movement", "Kill Depth", T::Float, 1.0f, 10000.0f, "Falling this far below the spawn respawns the character, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Movement.KillDepth; } },

        { "Camera", "Field of View", T::Float, 20.0f, 120.0f, "Vertical field of view, in degrees.",
          [](CharacterSettings& s) -> void* { return &s.Camera.FieldOfView; } },
        { "Camera", "Sprint FOV Boost", T::Float, 0.0f, 30.0f, "Degrees added to the field of view while sprinting.",
          [](CharacterSettings& s) -> void* { return &s.Camera.SprintFovBoost; } },
        { "Camera", "Eye Height", T::Float, 0.1f, 5.0f, "Camera height above the feet while standing, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Camera.EyeHeight; } },
        { "Camera", "Crouched Eye Height", T::Float, 0.1f, 5.0f, "Camera height above the feet while crouched, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Camera.CrouchedEyeHeight; } },
        { "Camera", "Blend Speed", T::Float, 0.0f, 50.0f, "How quickly crouch height and field of view changes settle.",
          [](CharacterSettings& s) -> void* { return &s.Camera.BlendSpeed; } },
        { "Camera", "Head Bob Amount", T::Float, 0.0f, 0.2f, "Vertical camera bob while walking, in metres (0 = off).",
          [](CharacterSettings& s) -> void* { return &s.Camera.HeadBobAmount; } },
        { "Camera", "Head Bob Frequency", T::Float, 0.0f, 5.0f, "Bob cycles per metre walked.",
          [](CharacterSettings& s) -> void* { return &s.Camera.HeadBobFrequency; } },
        { "Camera", "Landing Dip", T::Float, 0.0f, 0.1f, "Camera dip on landing, per m/s of impact speed.",
          [](CharacterSettings& s) -> void* { return &s.Camera.LandingDip; } },
        { "Camera", "Landing Dip Max", T::Float, 0.0f, 1.0f, "Largest landing dip, in metres.",
          [](CharacterSettings& s) -> void* { return &s.Camera.LandingDipMax; } },
        { "Camera", "Min Pitch", T::Float, -89.5f, 0.0f, "How far down the player can look, in degrees.",
          [](CharacterSettings& s) -> void* { return &s.Camera.MinPitch; } },
        { "Camera", "Max Pitch", T::Float, 0.0f, 89.5f, "How far up the player can look, in degrees.",
          [](CharacterSettings& s) -> void* { return &s.Camera.MaxPitch; } },

        { "Controls", "Mouse Sensitivity", T::Float, 0.005f, 2.0f, "Degrees of turn per pixel of mouse movement.",
          [](CharacterSettings& s) -> void* { return &s.Controls.MouseSensitivity; } },
        { "Controls", "Invert Y", T::Bool, 0.0f, 1.0f, "Moving the mouse up looks down.",
          [](CharacterSettings& s) -> void* { return &s.Controls.InvertY; } },
        { "Controls", "Crouch Toggle", T::Bool, 0.0f, 1.0f, "Crouch key toggles instead of being held.",
          [](CharacterSettings& s) -> void* { return &s.Controls.CrouchToggle; } },
        { "Controls", "Sprint Toggle", T::Bool, 0.0f, 1.0f, "Sprint key toggles instead of being held.",
          [](CharacterSettings& s) -> void* { return &s.Controls.SprintToggle; } },
        { "Controls", "Capture Mouse", T::Bool, 0.0f, 1.0f, "Grab the mouse for looking when play starts (Escape releases it).",
          [](CharacterSettings& s) -> void* { return &s.Controls.CaptureMouse; } },
    };
    count = sizeof(kFields) / sizeof(kFields[0]);
    return kFields;
}

// Name lookups for the table, matched exactly. Booleans read as 1/0 and take any
// non-zero as true; integers truncate.
inline const CharacterSettingField* FindCharacterSettingField(const char* name)
{
    std::size_t count = 0;
    const CharacterSettingField* fields = CharacterSettingFields(count);
    for (std::size_t i = 0; name != nullptr && i < count; ++i)
    {
        const char* a = fields[i].Name;
        const char* b = name;
        while (*a != '\0' && *a == *b)
        {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return &fields[i];
    }
    return nullptr;
}

inline double GetCharacterSetting(const CharacterSettings& settings, const CharacterSettingField& field)
{
    // The table's accessors take a mutable struct so one pointer serves reads and writes.
    void* address = field.Address(const_cast<CharacterSettings&>(settings));
    switch (field.Type)
    {
    case CharacterSettingType::Int: return static_cast<double>(*static_cast<int*>(address));
    case CharacterSettingType::Bool: return *static_cast<bool*>(address) ? 1.0 : 0.0;
    default: return static_cast<double>(*static_cast<float*>(address));
    }
}

inline void SetCharacterSetting(CharacterSettings& settings, const CharacterSettingField& field, double value)
{
    void* address = field.Address(settings);
    switch (field.Type)
    {
    case CharacterSettingType::Int: *static_cast<int*>(address) = static_cast<int>(value); break;
    case CharacterSettingType::Bool: *static_cast<bool*>(address) = value != 0.0; break;
    default: *static_cast<float*>(address) = static_cast<float>(value); break;
    }
}

// Where the eye is and where it looks: what the renderer's camera should be set to.
struct CharacterView
{
    float Position[3]{};
    float Pitch = 0.0f; // radians
    float Yaw = 0.0f;   // radians
    float FieldOfView = 70.0f; // degrees, vertical
};

class CharacterMovement
{
public:
    CharacterSettings Settings;

    void SetTrace(CharacterTraceFn trace, void* user)
    {
        mTrace = trace;
        mTraceUser = user;
    }

    // Places the character with its eye at the given pose - the pose an editor camera or
    // a Player Start marker describes - and drops its feet to the floor underneath.
    void SpawnAtEye(float x, float y, float z, float pitch, float yaw)
    {
        mPosition[0] = x;
        mPosition[1] = y;
        mPosition[2] = z - Settings.Camera.EyeHeight;
        mSpawn[0] = mPosition[0];
        mSpawn[1] = mPosition[1];
        mSpawn[2] = mPosition[2];
        mSpawnYaw = yaw;
        mFloorZ = mPosition[2];
        mYaw = WrapAngle(yaw);
        mPitch = ClampPitch(pitch);
        ResetMotion();

        // Settle onto whatever is below, as long as it is within reach: a spawn floating
        // high above the level should fall, not teleport.
        CharacterTraceHit floor;
        if (FindFloor(mPosition, Settings.Movement.StandingHeight, 4.0f, floor))
        {
            mPosition[2] = floor.Position[2];
            mGrounded = true;
        }
    }

    // Moves the character without any collision test, keeping the view pitch.
    void Teleport(float feetX, float feetY, float feetZ, float yaw)
    {
        mPosition[0] = feetX;
        mPosition[1] = feetY;
        mPosition[2] = feetZ;
        mYaw = WrapAngle(yaw);
        ResetMotion();
    }

    // ---- Input, consumed by the next Update ------------------------------------

    // Forward and Right are -1..1, relative to where the character faces. Calls in the
    // same frame add up, and the total is clamped so diagonals are not faster.
    void AddMovementInput(float forward, float right)
    {
        mInputForward += forward;
        mInputRight += right;
    }

    // Degrees. Positive turns right and looks up. Applied at once, so the view never
    // lags a frame behind the mouse.
    void AddLookInput(float turnRightDegrees, float lookUpDegrees)
    {
        constexpr float degreesToRadians = 0.017453292519943295f;
        mYaw = WrapAngle(mYaw - turnRightDegrees * degreesToRadians);
        mPitch = ClampPitch(mPitch + lookUpDegrees * degreesToRadians);
    }

    void SetViewRotation(float pitch, float yaw)
    {
        mPitch = ClampPitch(pitch);
        mYaw = WrapAngle(yaw);
    }

    // Jump is a press, not a hold: it is buffered for JumpBufferTime so a press shortly
    // before landing still jumps. StopJumping ends a variable-height jump early.
    void Jump()
    {
        mJumpBuffer = (std::max)(Settings.Movement.JumpBufferTime, 0.0001f);
        mJumpHeld = true;
    }

    void StopJumping()
    {
        if (mJumpHeld && mVelocity[2] > 0.0f && !mGrounded)
            mVelocity[2] *= std::clamp(Settings.Movement.JumpReleaseDamping, 0.0f, 1.0f);
        mJumpHeld = false;
    }

    // Jumps the crouch eye height and field of view straight to what the settings ask
    // for, instead of blending. For configuring the character before its first frame.
    void SnapCameraToSettings()
    {
        mEyeHeight = mCrouched ? Settings.Camera.CrouchedEyeHeight : Settings.Camera.EyeHeight;
        mFieldOfView = Settings.Camera.FieldOfView;
    }

    // False until the first Update, i.e. while the character is still being set up.
    bool HasUpdated() const { return mHasUpdated; }

    void Crouch() { mWantsCrouch = true; }
    void UnCrouch() { mWantsCrouch = false; }
    void SetSprinting(bool sprinting) { mWantsSprint = sprinting; }

    // ---- Simulation ------------------------------------------------------------

    void Update(float deltaSeconds)
    {
        // A stalled or rewound clock would otherwise integrate backwards, and a long
        // hitch would tunnel through walls in one step.
        deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);
        mHasUpdated = true;

        float forward = mInputForward;
        float right = mInputRight;
        mInputForward = 0.0f;
        mInputRight = 0.0f;
        const float inputLength = std::sqrt(forward * forward + right * right);
        if (inputLength > 1.0f)
        {
            forward /= inputLength;
            right /= inputLength;
        }
        mHasMovementInput = inputLength > 0.01f;
        mMovingForward = forward > 0.1f;

        UpdateCrouch();

        // Substeps keep a fast fall or a sprint from moving further in one step than the
        // collision rays reach.
        int steps = static_cast<int>(std::ceil(deltaSeconds / kMaxStep));
        steps = std::clamp(steps, 1, 8);
        const float stepSeconds = deltaSeconds / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i)
            Step(stepSeconds, forward, right);

        UpdateCamera(deltaSeconds);

        if (mPosition[2] < mSpawn[2] - Settings.Movement.KillDepth)
        {
            Teleport(mSpawn[0], mSpawn[1], mSpawn[2], mSpawnYaw);
        }
    }

    // ---- State -----------------------------------------------------------------

    CharacterView GetView() const
    {
        const float forwardX = std::sin(mYaw);
        const float forwardY = std::cos(mYaw);
        const float rightX = -forwardY;
        const float rightY = forwardX;
        const float sway = std::cos(mBobPhase * 0.5f) * mBobWeight * Settings.Camera.HeadBobAmount * 0.5f;

        CharacterView view;
        view.Position[0] = mPosition[0] + rightX * sway;
        view.Position[1] = mPosition[1] + rightY * sway;
        view.Position[2] = mPosition[2] + mEyeHeight + mStepSmoothing - mLandingDip
            + std::sin(mBobPhase) * mBobWeight * Settings.Camera.HeadBobAmount;
        view.Pitch = mPitch;
        view.Yaw = mYaw;
        view.FieldOfView = mFieldOfView;
        return view;
    }

    const float* GetPosition() const { return mPosition; }
    const float* GetVelocity() const { return mVelocity; }
    float GetPitch() const { return mPitch; }
    float GetYaw() const { return mYaw; }
    float GetHorizontalSpeed() const { return std::sqrt(mVelocity[0] * mVelocity[0] + mVelocity[1] * mVelocity[1]); }
    float GetCapsuleHeight() const { return mCrouched ? Settings.Movement.CrouchedHeight : Settings.Movement.StandingHeight; }

    bool IsGrounded() const { return mGrounded; }
    bool IsFalling() const { return !mGrounded; }
    bool IsCrouching() const { return mCrouched; }
    // Sprinting counts only while it actually changes the speed: standing still, walking
    // backwards or crouching with Shift held is not sprinting.
    bool IsSprinting() const { return mWantsSprint && !mCrouched && mMovingForward && mHasMovementInput; }
    bool WantsToCrouch() const { return mWantsCrouch; }

    // Bumped on every landing, so any number of listeners can each notice one without
    // consuming it from the others.
    std::uint32_t GetLandedCount() const { return mLandedCount; }
    float GetLastImpactSpeed() const { return mLastImpactSpeed; }
    std::uint32_t GetJumpedCount() const { return mJumpedCount; }

private:
    static constexpr float kMaxStep = 1.0f / 120.0f;
    // Gap kept between the capsule and a wall, so the next frame's rays start outside it.
    static constexpr float kSkin = 0.02f;
    static constexpr float kPi = 3.14159265358979f;
    // Wall ray rows: at most this far apart, and never more than this many.
    static constexpr float kMaxRowGap = 0.3f;
    static constexpr int kMaxRows = 12;

    static float WrapAngle(float radians)
    {
        return std::remainder(radians, 2.0f * kPi);
    }

    float ClampPitch(float pitch) const
    {
        constexpr float degreesToRadians = 0.017453292519943295f;
        // Never quite straight up or down: yaw is undefined there and the view flips.
        const float low = (std::max)(Settings.Camera.MinPitch, -89.5f) * degreesToRadians;
        const float high = (std::min)(Settings.Camera.MaxPitch, 89.5f) * degreesToRadians;
        return std::clamp(pitch, (std::min)(low, high), (std::max)(low, high));
    }

    void ResetMotion()
    {
        mVelocity[0] = mVelocity[1] = mVelocity[2] = 0.0f;
        mGrounded = false;
        mCoyoteTimer = 0.0f;
        mJumpBuffer = 0.0f;
        mJumpCount = 0;
        mStepSmoothing = 0.0f;
        mLandingDip = 0.0f;
        mCrouched = mWantsCrouch;
        mEyeHeight = mCrouched ? Settings.Camera.CrouchedEyeHeight : Settings.Camera.EyeHeight;
        mFieldOfView = Settings.Camera.FieldOfView;
    }

    bool Trace(const float origin[3], const float direction[3], float maxDistance, CharacterTraceHit& hit) const
    {
        if (mTrace != nullptr)
            return mTrace(mTraceUser, origin, direction, maxDistance, &hit);

        // No world to ask: an endless floor at the spawn height.
        if (direction[2] >= -1e-4f || origin[2] < mFloorZ)
            return false;
        const float distance = (origin[2] - mFloorZ) / -direction[2];
        if (distance > maxDistance)
            return false;
        hit.Distance = distance;
        for (int a = 0; a < 3; ++a)
            hit.Position[a] = origin[a] + direction[a] * distance;
        hit.Normal[0] = 0.0f;
        hit.Normal[1] = 0.0f;
        hit.Normal[2] = 1.0f;
        return true;
    }

    float WalkableNormalZ() const
    {
        return std::cos(std::clamp(Settings.Movement.MaxSlopeDegrees, 0.0f, 89.0f) * (kPi / 180.0f));
    }

    // Highest walkable floor under the capsule, probed from `probeHeight` above the feet
    // down to `reach` below them. Five rays - centre and four around the rim - so the
    // character can stand on a ledge with only part of its footprint.
    bool FindFloor(const float feet[3], float probeHeight, float reach, CharacterTraceHit& floor) const
    {
        const float rim = Settings.Movement.CapsuleRadius * 0.7f;
        const float offsets[5][2] = { { 0.0f, 0.0f }, { rim, 0.0f }, { -rim, 0.0f }, { 0.0f, rim }, { 0.0f, -rim } };
        const float down[3] = { 0.0f, 0.0f, -1.0f };
        const float walkable = WalkableNormalZ();

        bool found = false;
        for (const auto& offset : offsets)
        {
            const float origin[3] = { feet[0] + offset[0], feet[1] + offset[1], feet[2] + probeHeight };
            CharacterTraceHit hit;
            if (!Trace(origin, down, probeHeight + reach, hit))
                continue;
            if (hit.Normal[2] < walkable)
                continue;
            if (!found || hit.Position[2] > floor.Position[2])
            {
                floor = hit;
                found = true;
            }
        }
        return found;
    }

    // Distance to the nearest wall along the horizontal unit direction (dirX, dirY),
    // measured from the capsule's axis. Walkable slopes are not walls: the floor probe
    // climbs those.
    bool FindWall(float dirX, float dirY, float reach, CharacterTraceHit& wall) const
    {
        const float height = GetCapsuleHeight();
        const float radius = Settings.Movement.CapsuleRadius;
        const float stepClear = (std::min)(Settings.Movement.StepHeight + 0.05f, height * 0.5f);
        // Rows from just above step height to just under the head, no more than
        // kMaxRowGap apart: anything thinner than the gap - a beam, a railing, a shelf
        // at head height - would otherwise slip between two rows.
        const float top = height - 0.05f;
        const int rows = std::clamp(static_cast<int>(std::ceil((top - stepClear) / kMaxRowGap)) + 1, 2, kMaxRows);
        float heights[kMaxRows]{};
        for (int row = 0; row < rows; ++row)
            heights[row] = stepClear + (top - stepClear) * static_cast<float>(row) / static_cast<float>(rows - 1);
        // Two extra rays at the capsule's flanks, so a thin post between the centre rays
        // or the corner of a doorway still stops it.
        const float sideX = -dirY * radius * 0.75f;
        const float sideY = dirX * radius * 0.75f;
        const float sides[3] = { 0.0f, 1.0f, -1.0f };
        const float direction[3] = { dirX, dirY, 0.0f };
        const float walkable = WalkableNormalZ();

        bool found = false;
        for (int row = 0; row < rows; ++row)
        {
            const float z = heights[row];
            for (float side : sides)
            {
                // The flank rays start further back, so they are compared by how far the
                // axis may move rather than by their own length.
                const float setback = side == 0.0f ? 0.0f : radius * 0.66f;
                const float origin[3] = {
                    mPosition[0] + sideX * side - dirX * setback,
                    mPosition[1] + sideY * side - dirY * setback,
                    mPosition[2] + z
                };
                CharacterTraceHit hit;
                if (!Trace(origin, direction, reach + setback, hit))
                    continue;
                if (hit.Normal[2] >= walkable)
                    continue;
                hit.Distance -= setback;
                if (!found || hit.Distance < wall.Distance)
                {
                    wall = hit;
                    found = true;
                }
            }
        }
        return found;
    }

    // Lowest ceiling over the capsule's footprint, looking up from `fromHeight` above the
    // feet. Same five rays as the floor probe, so the edge of a low ceiling counts too.
    bool FindCeiling(float fromHeight, float reach, CharacterTraceHit& ceiling) const
    {
        const float rim = Settings.Movement.CapsuleRadius * 0.7f;
        const float offsets[5][2] = { { 0.0f, 0.0f }, { rim, 0.0f }, { -rim, 0.0f }, { 0.0f, rim }, { 0.0f, -rim } };
        const float up[3] = { 0.0f, 0.0f, 1.0f };

        bool found = false;
        for (const auto& offset : offsets)
        {
            const float origin[3] = { mPosition[0] + offset[0], mPosition[1] + offset[1], mPosition[2] + fromHeight };
            CharacterTraceHit hit;
            if (Trace(origin, up, reach, hit) && (!found || hit.Distance < ceiling.Distance))
            {
                ceiling = hit;
                found = true;
            }
        }
        return found;
    }

    bool HasHeadroom(float height) const
    {
        CharacterTraceHit hit;
        return !FindCeiling(0.1f, height - 0.1f, hit);
    }

    void UpdateCrouch()
    {
        if (mWantsCrouch && !mCrouched)
        {
            mCrouched = true;
        }
        else if (!mWantsCrouch && mCrouched && HasHeadroom(Settings.Movement.StandingHeight))
        {
            // Standing up under a low ceiling waits until there is room.
            mCrouched = false;
        }
    }

    void Step(float dt, float forward, float right)
    {
        const CharacterMovementSettings& move = Settings.Movement;

        // ---- Horizontal velocity ----
        const float forwardX = std::sin(mYaw);
        const float forwardY = std::cos(mYaw);
        const float rightX = -forwardY;
        const float rightY = forwardX;
        const float wishX = forwardX * forward + rightX * right;
        const float wishY = forwardY * forward + rightY * right;

        float targetSpeed = move.WalkSpeed;
        if (mCrouched)
            targetSpeed = move.CrouchSpeed;
        else if (IsSprinting())
            targetSpeed = move.SprintSpeed;

        const float targetX = wishX * targetSpeed;
        const float targetY = wishY * targetSpeed;
        const bool hasInput = wishX * wishX + wishY * wishY > 1e-4f;

        // Accelerate towards the target velocity at a fixed rate: speeding up uses the
        // acceleration, coming to a stop the braking, and in the air both shrink to the
        // air-control share (with no braking at all, so a jump keeps its momentum).
        float rate = hasInput ? move.Acceleration : move.Braking;
        if (!mGrounded)
            rate = hasInput ? move.Acceleration * std::clamp(move.AirControl, 0.0f, 1.0f) : 0.0f;
        const float deltaX = targetX - mVelocity[0];
        const float deltaY = targetY - mVelocity[1];
        const float deltaLength = std::sqrt(deltaX * deltaX + deltaY * deltaY);
        const float maxChange = rate * dt;
        if (deltaLength <= maxChange || deltaLength < 1e-5f)
        {
            if (rate > 0.0f)
            {
                mVelocity[0] = targetX;
                mVelocity[1] = targetY;
            }
        }
        else
        {
            mVelocity[0] += deltaX / deltaLength * maxChange;
            mVelocity[1] += deltaY / deltaLength * maxChange;
        }

        // ---- Jumping ----
        if (mJumpBuffer > 0.0f)
            mJumpBuffer -= dt;
        if (mCoyoteTimer > 0.0f)
            mCoyoteTimer -= dt;

        if (mJumpBuffer > 0.0f)
        {
            const bool onGround = mGrounded || mCoyoteTimer > 0.0f;
            const bool airJump = !onGround && mJumpCount > 0 && mJumpCount < move.MaxJumpCount;
            if ((onGround || airJump) && HasHeadroom(GetCapsuleHeight() + 0.05f))
            {
                mVelocity[2] = move.JumpVelocity;
                mGrounded = false;
                mCoyoteTimer = 0.0f;
                mJumpBuffer = 0.0f;
                mJumpCount = onGround ? 1 : mJumpCount + 1;
                ++mJumpedCount;
            }
        }

        // ---- Gravity ----
        if (!mGrounded)
            mVelocity[2] = (std::max)(mVelocity[2] - move.Gravity * dt, -move.MaxFallSpeed);

        MoveHorizontal(mVelocity[0] * dt, mVelocity[1] * dt);
        MoveVertical(dt);
    }

    void MoveHorizontal(float moveX, float moveY)
    {
        const float radius = Settings.Movement.CapsuleRadius;

        // Up to three slides: into a corner the second wall takes what the first left.
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            const float distance = std::sqrt(moveX * moveX + moveY * moveY);
            if (distance < 1e-6f)
                break;

            const float dirX = moveX / distance;
            const float dirY = moveY / distance;
            CharacterTraceHit wall;
            if (!FindWall(dirX, dirY, radius + distance + kSkin, wall))
            {
                mPosition[0] += moveX;
                mPosition[1] += moveY;
                break;
            }

            const float allowed = (std::max)(0.0f, wall.Distance - radius - kSkin);
            mPosition[0] += dirX * allowed;
            mPosition[1] += dirY * allowed;

            // Slide: keep only the part of the remaining move (and of the velocity) that
            // runs along the wall.
            float normalX = wall.Normal[0];
            float normalY = wall.Normal[1];
            const float normalLength = std::sqrt(normalX * normalX + normalY * normalY);
            if (normalLength < 1e-4f)
                break;
            normalX /= normalLength;
            normalY /= normalLength;

            const float remainingX = moveX - dirX * allowed;
            const float remainingY = moveY - dirY * allowed;
            const float into = remainingX * normalX + remainingY * normalY;
            moveX = remainingX - normalX * (std::min)(into, 0.0f);
            moveY = remainingY - normalY * (std::min)(into, 0.0f);

            const float velocityInto = mVelocity[0] * normalX + mVelocity[1] * normalY;
            if (velocityInto < 0.0f)
            {
                mVelocity[0] -= normalX * velocityInto;
                mVelocity[1] -= normalY * velocityInto;
            }
        }

        Depenetrate();
    }

    // Pushes the capsule back out of anything a ray found closer than its radius. The
    // forward rays only look where the character is going, so this is what keeps a wall
    // beside it - reached by turning, or by a slide - from ending up inside it.
    void Depenetrate()
    {
        const float radius = Settings.Movement.CapsuleRadius;
        const float height = GetCapsuleHeight();
        const float z = (std::min)(Settings.Movement.StepHeight + 0.05f, height * 0.5f);
        const float walkable = WalkableNormalZ();
        constexpr int kDirections = 8;

        for (int heightIndex = 0; heightIndex < 2; ++heightIndex)
        {
            const float originZ = mPosition[2] + (heightIndex == 0 ? z : height - 0.1f);
            for (int i = 0; i < kDirections; ++i)
            {
                const float angle = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kDirections);
                const float direction[3] = { std::cos(angle), std::sin(angle), 0.0f };
                const float origin[3] = { mPosition[0], mPosition[1], originZ };
                CharacterTraceHit hit;
                if (!Trace(origin, direction, radius, hit) || hit.Normal[2] >= walkable)
                    continue;
                const float push = radius + kSkin * 0.5f - hit.Distance;
                if (push > 0.0f)
                {
                    mPosition[0] -= direction[0] * push;
                    mPosition[1] -= direction[1] * push;
                }
            }
        }
    }

    void MoveVertical(float dt)
    {
        const CharacterMovementSettings& move = Settings.Movement;
        const float height = GetCapsuleHeight();
        float moveZ = mVelocity[2] * dt;

        if (moveZ > 0.0f)
        {
            // Rising: stop at the ceiling.
            CharacterTraceHit ceiling;
            if (FindCeiling(height - 0.05f, moveZ + 0.05f + kSkin, ceiling))
            {
                moveZ = (std::max)(0.0f, ceiling.Distance - 0.05f - kSkin);
                mVelocity[2] = 0.0f;
            }
            mPosition[2] += moveZ;
            mGrounded = false;
            return;
        }

        // Falling or walking: look for a floor from step height above the feet. When
        // already on the ground, reach a step's depth below too so walking down stairs
        // and slopes sticks to them instead of hopping off every edge.
        const float reach = -moveZ + (mGrounded ? move.StepHeight : kSkin);
        CharacterTraceHit floor;
        const bool wasGrounded = mGrounded;
        if (FindFloor(mPosition, move.StepHeight, reach, floor) && floor.Position[2] >= mPosition[2] + moveZ - kSkin)
        {
            const float rise = floor.Position[2] - mPosition[2];
            // A step up (or down) moves the camera smoothly rather than in one jolt.
            if (wasGrounded && std::abs(rise) > 0.02f)
                mStepSmoothing -= rise;
            mPosition[2] = floor.Position[2];

            if (!wasGrounded)
            {
                mLastImpactSpeed = -mVelocity[2];
                mLandingDip = (std::min)(Settings.Camera.LandingDipMax,
                                         mLandingDip + mLastImpactSpeed * Settings.Camera.LandingDip);
                ++mLandedCount;
            }

            mVelocity[2] = 0.0f;
            mGrounded = true;
            mJumpCount = 0;
            return;
        }

        if (wasGrounded)
        {
            // Walked off a ledge: jumping stays possible for a moment.
            mCoyoteTimer = move.CoyoteTime;
        }
        mGrounded = false;
        mPosition[2] += moveZ;
    }

    void UpdateCamera(float dt)
    {
        const CharacterCameraSettings& camera = Settings.Camera;
        const float blend = 1.0f - std::exp(-(std::max)(camera.BlendSpeed, 0.0f) * dt);

        const float targetEye = mCrouched ? camera.CrouchedEyeHeight : camera.EyeHeight;
        mEyeHeight += (targetEye - mEyeHeight) * blend;

        const float targetFov = camera.FieldOfView + (IsSprinting() && mGrounded ? camera.SprintFovBoost : 0.0f);
        mFieldOfView += (targetFov - mFieldOfView) * blend;

        // Step smoothing and the landing dip decay a little faster than the crouch blend,
        // so stairs read as a glide and a landing as a quick knee bend.
        const float fastBlend = 1.0f - std::exp(-(std::max)(camera.BlendSpeed, 0.0f) * 1.5f * dt);
        mStepSmoothing -= mStepSmoothing * fastBlend;
        mLandingDip -= mLandingDip * fastBlend;

        // Bob only while walking on the ground, fading in and out with the speed.
        const float speed = GetHorizontalSpeed();
        const float targetWeight = mGrounded ? std::clamp(speed / (std::max)(Settings.Movement.WalkSpeed, 0.1f), 0.0f, 1.5f) : 0.0f;
        mBobWeight += (targetWeight - mBobWeight) * blend;
        if (mGrounded)
            mBobPhase = std::fmod(mBobPhase + speed * dt * camera.HeadBobFrequency * 2.0f * kPi, 4.0f * kPi);
    }

    CharacterTraceFn mTrace = nullptr;
    void* mTraceUser = nullptr;

    float mPosition[3]{};
    float mVelocity[3]{};
    float mSpawn[3]{};
    float mSpawnYaw = 0.0f;
    float mFloorZ = 0.0f;
    float mPitch = 0.0f;
    float mYaw = 0.0f;

    bool mHasUpdated = false;
    float mInputForward = 0.0f;
    float mInputRight = 0.0f;
    bool mHasMovementInput = false;
    bool mMovingForward = false;
    bool mWantsCrouch = false;
    bool mWantsSprint = false;
    bool mJumpHeld = false;
    float mJumpBuffer = 0.0f;

    bool mGrounded = false;
    bool mCrouched = false;
    float mCoyoteTimer = 0.0f;
    int mJumpCount = 0;
    std::uint32_t mLandedCount = 0;
    std::uint32_t mJumpedCount = 0;
    float mLastImpactSpeed = 0.0f;

    float mEyeHeight = 1.65f;
    float mFieldOfView = 70.0f;
    float mStepSmoothing = 0.0f;
    float mLandingDip = 0.0f;
    float mBobPhase = 0.0f;
    float mBobWeight = 0.0f;
};
