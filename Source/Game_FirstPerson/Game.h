#pragma once

#include "FirstPersonCharacter.h"
#include "GameAPI.h"

// The game module: a first-person character walking around whatever level is open.
//
// This is the starting point for game code. The player itself lives in
// FirstPersonCharacter; this class owns the session - it spawns the player, forwards
// the host's input to it, and handles the few actions every game needs (fullscreen,
// quit). When Game Settings selects a Node Graph player controller, the host runs the
// player and this module keeps its hands off the camera.
class Game
{
public:
    static Game& Get();

    bool Start(const GameCameraState& spawn, const GameServices& services);
    void Update(const GameFrameContext& frame, GameCameraState& camera);
    void Stop();

    bool IsRunning() const { return mRunning; }

private:
    void HandleAction(int action);

    GameServices mHost{};
    FirstPersonCharacter mPlayer;
    bool mRunning = false;
    bool mNativePlayer = true;
};
