// Game.cpp : Game module runtime and the entry points the engine host calls into.

#include "pch.h"
#include "framework.h"
#include "Game.h"

Game& Game::Get()
{
    static Game game;
    return game;
}

bool Game::Start(const GameCameraState& initialCamera)
{
    // Continue from wherever the host camera was so pressing play does not teleport the view.
    mCamera.Reset(initialCamera);
    mIsRunning = true;
    return true;
}

void Game::Update(const GameFrameContext& frame, GameCameraState& camera)
{
    if (!mIsRunning)
        return;

    mCamera.Update(frame);
    camera = mCamera.GetState();
}

void Game::Stop()
{
    mIsRunning = false;
}

extern "C"
{
    std::uint32_t __stdcall Game_GetApiVersion()
    {
        return GameApiVersion;
    }

    const char* __stdcall Game_GetName()
    {
        return "Ptero Game";
    }

    bool __stdcall Game_Start(const GameCameraState* initialCamera)
    {
        const GameCameraState startCamera = (initialCamera != nullptr) ? *initialCamera : GameCameraState{};
        return Game::Get().Start(startCamera);
    }

    void __stdcall Game_Update(const GameFrameContext* frame, GameCameraState* camera)
    {
        if (frame == nullptr || camera == nullptr)
            return;

        Game::Get().Update(*frame, *camera);
    }

    void __stdcall Game_Stop()
    {
        Game::Get().Stop();
    }
}
