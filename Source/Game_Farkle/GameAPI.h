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
inline constexpr std::uint32_t GameApiVersion = 5;

struct GameTransform { float Position[3]{}; float Rotation[3]{}; float Scale[3]{1,1,1}; };
// UTF-8 strings are borrowed during the call. No STL or engine objects cross DLLs.
struct GameServices {
    void* User = nullptr;
    int (*FindEntity)(void*, const char*) = nullptr;
    bool (*GetTransform)(void*, int, GameTransform*) = nullptr;
    void (*SetTransform)(void*, int, const GameTransform*) = nullptr;
    bool (*LoadUi)(void*, const char*) = nullptr;
    // 0=text, 1=markup, 2=add class, 3=remove class, 4=enable, 5=disable.
    void (*Ui)(void*, int, const char*, const char*) = nullptr;
    int (*PollAction)(void*) = nullptr;
    void (*RequestStop)(void*) = nullptr;
    void (*ToggleFullscreen)(void*) = nullptr;
    // Audio. Events are named, not pathed: the host resolves "DiceRoll" against the
    // loaded FMOD banks, so moving an event between folders in FMOD Studio does not
    // break the game. PlaySound is fire-and-forget. PlayMusic replaces whatever is on
    // the single music channel and returns false when the track could not be started,
    // which is also how the game learns that this host has no audio at all.
    void (*PlaySound)(void*, const char*) = nullptr;
    bool (*PlayMusic)(void*, const char*) = nullptr;
    bool (*IsMusicPlaying)(void*) = nullptr;
    // Read once when a match starts, so edits cannot change an ongoing match.
    int (*GetWinningScore)(void*) = nullptr;
    // True in the standalone game, false when playing from the editor. The editor keeps
    // its testing HUD and menu; the standalone build gets the player menus and HUD.
    bool Standalone = false;
};
enum GameAction { NoAction, Roll, Bank, Clear, Help, Pause, Close, Rematch, MainMenu,
    Continue, Fullscreen, StartMatch, SelectDie = 20, RemoveDie = 30, ExitGame = 40 };

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
    __declspec(dllexport) bool __stdcall Game_Start(const GameCameraState* initialCamera, const GameServices* services);
    __declspec(dllexport) void __stdcall Game_Update(const GameFrameContext* frame, GameCameraState* camera);
    __declspec(dllexport) void __stdcall Game_Stop();
}
#endif

using GameGetApiVersionFn = std::uint32_t(__stdcall*)();
using GameGetNameFn = const char* (__stdcall*)();
using GameStartFn = bool(__stdcall*)(const GameCameraState*, const GameServices*);
using GameUpdateFn = void(__stdcall*)(const GameFrameContext*, GameCameraState*);
using GameStopFn = void(__stdcall*)();
