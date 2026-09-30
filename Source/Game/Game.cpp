#include "pch.h"

#include "Game.h"

Game& Game::Get()
{
    static Game instance;
    return instance;
}

bool Game::Start(const GameCameraState& spawn, const GameServices& services)
{
    mHost = services;
    mNativePlayer = services.NativePlayerController;

    if (mNativePlayer)
    {
        mPlayer.Begin(spawn, services.TraceRay, services.User);

        // A game that wants a cursor from the first frame (a main menu, say) turns
        // CaptureMouse off; everything else looks with the mouse straight away.
        if (mHost.SetMouseCaptured != nullptr)
            mHost.SetMouseCaptured(mHost.User, mPlayer.GetMovement().Settings.Controls.CaptureMouse);
    }

    mRunning = true;
    return true;
}

void Game::Stop()
{
    if (mRunning && mHost.SetMouseCaptured != nullptr)
        mHost.SetMouseCaptured(mHost.User, false);
    mRunning = false;
}

void Game::HandleAction(int action)
{
    if (action == Fullscreen && mHost.ToggleFullscreen != nullptr)
        mHost.ToggleFullscreen(mHost.User);
    else if (action == ExitGame && mHost.RequestStop != nullptr)
        mHost.RequestStop(mHost.User);
}

void Game::Update(const GameFrameContext& frame, GameCameraState& camera)
{
    if (!mRunning)
        return;

    if (mHost.PollAction != nullptr)
    {
        // Bounded, in case a host ever reports the same action forever.
        for (int i = 0; i < 32; ++i)
        {
            const int action = mHost.PollAction(mHost.User);
            if (action == NoAction)
                break;
            HandleAction(action);
        }
    }

    if (!mNativePlayer)
        return;

    mPlayer.Update(frame);
    camera = mPlayer.GetCamera();
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

    bool __stdcall Game_Start(const GameCameraState* spawn, const GameServices* services)
    {
        try
        {
            return services != nullptr && Game::Get().Start(spawn ? *spawn : GameCameraState{}, *services);
        }
        catch (...)
        {
            return false;
        }
    }

    void __stdcall Game_Update(const GameFrameContext* frame, GameCameraState* camera)
    {
        if (frame != nullptr && camera != nullptr)
            Game::Get().Update(*frame, *camera);
    }

    void __stdcall Game_Stop()
    {
        Game::Get().Stop();
    }
}
