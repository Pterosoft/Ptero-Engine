#pragma once

#include "GameAPI.h"
#include "GameCamera.h"

// The game module's runtime state. One instance lives for the lifetime of the loaded DLL;
// Game_Start / Game_Stop bracket a play session inside it, so anything that should survive
// a stop-and-play cycle belongs here rather than in the session itself.
class Game
{
public:
    static Game& Get();

    bool Start(const GameCameraState& initialCamera);
    void Update(const GameFrameContext& frame, GameCameraState& camera);
    void Stop();

    bool IsRunning() const { return mIsRunning; }

private:
    GameCamera mCamera;
    bool mIsRunning = false;
};
