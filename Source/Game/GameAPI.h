#pragma once

// GameAPI.h - the contract between the engine host (editor / renderer DLL) and Game.dll.
//
// Both sides compile this header, so it deliberately stays free of engine, Windows and
// DirectX types: the game module only has to agree with the host on plain data. The host
// resolves the exported entry points with GetProcAddress, which is why the function
// pointer typedefs below are always visible while the dllexport declarations are only
// emitted while building Game.dll itself.

#include <cstdint>

// Bump this whenever the structures or the exported entry points change so the host can
// refuse a stale Game.dll instead of reading garbage out of it.
inline constexpr std::uint32_t GameApiVersion = 1;

// One frame of already-resolved input. The host owns key bindings and window focus rules;
// the game only sees the resulting intent so it never has to touch the Win32 input APIs.
struct GameInputState
{
    bool MoveForward = false;
    bool MoveBackward = false;
    bool MoveLeft = false;
    bool MoveRight = false;
    bool MoveUp = false;
    bool MoveDown = false;
    bool Sprint = false;

    // LookActive is false while the host owns the mouse (editor UI interaction, no focus),
    // in which case the look deltas are zero and the game should not rotate the camera.
    bool LookActive = false;
    float LookDeltaX = 0.0f;
    float LookDeltaY = 0.0f;
};

// Camera pose in the engine's left-handed, Z-up world space, matching EditorCamera:
//   forward = (sin(Yaw) * cos(Pitch), cos(Yaw) * cos(Pitch), sin(Pitch))
// so a pose handed to the game round-trips back into the engine camera unchanged.
struct GameCameraState
{
    float PositionX = 0.0f;
    float PositionY = 0.0f;
    float PositionZ = 0.0f;
    float Pitch = 0.0f; // radians
    float Yaw = 0.0f;   // radians
};

struct GameFrameContext
{
    float DeltaSeconds = 0.0f;
    GameInputState Input;
};

#ifdef GAME_EXPORTS
extern "C"
{
    __declspec(dllexport) std::uint32_t __stdcall Game_GetApiVersion();
    __declspec(dllexport) const char* __stdcall Game_GetName();

    // Start / Stop bracket a play session. Start receives the camera pose the host was
    // using so the game can continue from wherever the editor camera was left.
    __declspec(dllexport) bool __stdcall Game_Start(const GameCameraState* initialCamera);
    __declspec(dllexport) void __stdcall Game_Update(const GameFrameContext* frame, GameCameraState* camera);
    __declspec(dllexport) void __stdcall Game_Stop();
}
#endif

using GameGetApiVersionFn = std::uint32_t(__stdcall*)();
using GameGetNameFn = const char* (__stdcall*)();
using GameStartFn = bool(__stdcall*)(const GameCameraState*);
using GameUpdateFn = void(__stdcall*)(const GameFrameContext*, GameCameraState*);
using GameStopFn = void(__stdcall*)();
