#pragma once

// GameAPI.h - the contract between the engine host (editor / renderer DLL) and Game.dll.
//
// Both sides compile this header, so it deliberately stays free of engine, Windows and
// DirectX types: the game module only has to agree with the host on plain data. The host
// resolves the exported entry points with GetProcAddress, which is why the function
// pointer typedefs below are always visible while the dllexport declarations are only
// emitted while building Game.dll itself.

#include <cstdint>

// The character movement component is shared with the engine, so a C++ player controller
// and a Node Graph one move identically. Header-only and engine-free, like this file.
#include "../System/include/System/CharacterMovement.h"

// Bump this whenever the structures or the exported entry points change so the host can
// refuse a stale Game.dll instead of reading garbage out of it.
inline constexpr std::uint32_t GameApiVersion = 11;
// The oldest module the host still loads. Farkle's version 8 put GetWinningScore where
// Ptero's version 8 put the physics calls, so a version 8 module would read the wrong
// pointer; version 9 appends GetWinningScore after them.
inline constexpr std::uint32_t GameMinimumApiVersion = 9;

struct GameTransform { float Position[3]{}; float Rotation[3]{}; float Scale[3]{1,1,1}; };

// Nearest surface along a GameServices::Raycast. Entity is an index as FindEntity returns
// it, -1 when the surface belongs to no entity.
struct GameRayHit
{
    float Distance = 0.0f;
    float Position[3]{};
    float Normal[3]{ 0.0f, 0.0f, 1.0f };
    int Entity = -1;
};

// A collision that began in the latest physics step. The player is not an entity: a hit
// on the player has HitPlayer set and -1 in its place.
struct GameHit
{
    int Entity = -1;
    int OtherEntity = -1;
    bool HitPlayer = false;
    float Position[3]{};
    float Normal[3]{ 0.0f, 0.0f, 1.0f }; // from Entity towards OtherEntity
    float ImpactSpeed = 0.0f;            // closing speed, m/s; ~0 for a body settling
};
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
    // Audio. Events are named, not pathed: the host resolves "Footstep" against the
    // loaded FMOD banks, so moving an event between folders in FMOD Studio does not
    // break the game. PlaySound is fire-and-forget. PlayMusic replaces whatever is on
    // the single music channel and returns false when the track could not be started,
    // which is also how the game learns that this host has no audio at all.
    void (*PlaySound)(void*, const char*) = nullptr;
    bool (*PlayMusic)(void*, const char*) = nullptr;
    bool (*IsMusicPlaying)(void*) = nullptr;
    // True in the standalone game, false when playing from the editor.
    bool Standalone = false;

    // ---- Player -----------------------------------------------------------------
    // Collision against the level: static meshes and terrain, in world space. The
    // signature is CharacterTraceFn's, so it plugs straight into CharacterMovement.
    // Null when the host has no collision, in which case the character walks on an
    // endless floor at the spawn height.
    CharacterTraceFn TraceRay = nullptr;
    // Grab (hide and lock) the mouse for looking, or hand it back, e.g. for a menu.
    void (*SetMouseCaptured)(void*, bool) = nullptr;
    // False when Game Settings (Editor: Windows > Game Settings) picked a Node Graph
    // player controller: the host then
    // runs the player itself and the game module must leave the camera alone.
    bool NativePlayerController = true;

    // ---- Physics (API 8) --------------------------------------------------------
    // Rigid bodies simulated by Physics.dll. Forces, impulses and velocities act on an
    // entity with a Dynamic Rigid Body (Properties > Physics) and return false for anything
    // else, or when the host runs no physics. World space, Z up: impulses in N*s (the change
    // in velocity is impulse / mass), forces in N for the coming frame only, velocities in
    // m/s, spin in radians per second. Moving a rigid body with SetTransform teleports it.
    bool (*AddImpulse)(void*, int entity, const float impulse[3]) = nullptr;
    bool (*AddForce)(void*, int entity, const float force[3]) = nullptr;
    bool (*SetVelocity)(void*, int entity, const float velocity[3]) = nullptr;
    bool (*GetVelocity)(void*, int entity, float velocity[3]) = nullptr;
    bool (*SetAngularVelocity)(void*, int entity, const float velocity[3]) = nullptr;
    // Level geometry, terrain and rigid bodies - never the player's own body, so a ray from
    // the camera does not hit the one looking. `direction` need not be normalised.
    bool (*Raycast)(void*, const float origin[3], const float direction[3], float maxDistance,
                    GameRayHit* hit) = nullptr;
    // The collisions that began in the latest physics step. The host steps physics after
    // Game_Update, so this frame's call returns the previous frame's hits, each exactly once.
    // Copies up to maxCount and returns how many there are; hits may be null to count.
    int (*GetHits)(void*, GameHit* hits, int maxCount) = nullptr;

    // ---- Farkle (API 9) ---------------------------------------------------------
    // The match target picked in the menu's options. Read once when a match starts, so
    // edits cannot change an ongoing match.
    int (*GetWinningScore)(void*) = nullptr;

    // ---- Achievements and stats (API 10) ------------------------------------------
    // Steam achievements and INT stats by their Steamworks API names ("ACH_FIRST_VICTORY",
    // "STAT_DICE_ROLLED"). Without Steam - editor play, a build with no App ID - they are
    // kept for the play session only, so the logic can be tested; nothing is saved.
    // UnlockAchievement is true once the achievement is unlocked, including when it
    // already was. An achievement whose progress stat reaches its maximum unlocks on its own.
    bool (*UnlockAchievement)(void*, const char* name) = nullptr;
    bool (*IsAchievementUnlocked)(void*, const char* name) = nullptr;
    // False for a stat Steamworks does not define (or, without Steam, one never set).
    bool (*GetStat)(void*, const char* name, int* value) = nullptr;
    bool (*SetStat)(void*, const char* name, int value) = nullptr;
    // A player setting as the Settings menu last applied it - the settings.cfg key and
    // value, e.g. "path-tracing" -> "realtime". Copied into `value` (capacity bytes, always
    // terminated); false for a key the menus do not know.
    bool (*GetSetting)(void*, const char* key, char* value, int capacity) = nullptr;

    // ---- Online play (API 11) -------------------------------------------------------
    // The current text of a form control in the loaded document, e.g. the chat box -
    // copied into `value` (capacity bytes, always terminated). False when there is no such
    // control. Ui operation 6 sets a control's value (6=set value), e.g. to clear it.
    bool (*GetUiValue)(void*, const char* id, char* value, int capacity) = nullptr;
};
// Host-side button and key mapping, polled through PollAction. Only Fullscreen (F11) and
// ExitGame mean anything to the character game; the rest are kept so the host's UI
// bindings keep compiling.
enum GameAction { NoAction, Roll, Bank, Clear, Help, Pause, Close, Rematch, MainMenu,
    Continue, Fullscreen, StartMatch, SelectDie = 20, RemoveDie = 30, ExitGame = 40,
    // Online play (API 11): the online menu's tables, the HUD's chat and voice controls.
    // ChatSend also comes from Enter while the chat box has focus.
    QuickMatch = 50, HostPublic, HostFriends, InviteFriends, LeaveTable, CancelSearch,
    ChatSend, ToggleMicrophone, ToggleMute, ToggleSeat };

// One frame of already-resolved input. The host owns key bindings and window focus rules;
// the game only sees the resulting intent so it never has to touch the Win32 input APIs.
// Buttons report whether they are held; the game detects presses itself.
struct GameInputState
{
    bool MoveForward = false;  // W
    bool MoveBackward = false; // S
    bool MoveLeft = false;     // A
    bool MoveRight = false;    // D
    bool Jump = false;         // Space
    bool Crouch = false;       // Ctrl or C
    bool Sprint = false;       // Shift

    // LookActive is false while the host owns the mouse (editor UI interaction, no focus,
    // mouse released with Escape), in which case the look deltas are zero and the game
    // should not rotate the camera. Deltas are in pixels; +X is right, +Y is down.
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
    // Vertical field of view in degrees. 0 leaves the engine's own lens untouched.
    float FieldOfView = 0.0f;
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

    // Start / Stop bracket a play session. Start receives the pose the player should
    // spawn at: a "PlayerStart" entity if the level has one, the editor camera otherwise.
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
