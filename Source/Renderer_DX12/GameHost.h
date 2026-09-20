#pragma once

#include "..\Game\GameAPI.h"

#include <string>

// Loads Game.dll and owns a play session's lifetime.
//
// The module is loaded on Start and freed on Stop rather than held for the lifetime of the
// editor, so rebuilding the game project and pressing play again picks up the new code
// without restarting the editor. Every failure path leaves the host stopped and records a
// message the editor can show, because a missing or stale Game.dll must never take the
// editor down with it.
class GameHost
{
public:
    GameHost() = default;
    ~GameHost();

    GameHost(const GameHost&) = delete;
    GameHost& operator=(const GameHost&) = delete;

    bool Start(const GameCameraState& initialCamera, const GameServices& services);
    void Stop();
    void Update(const GameFrameContext& frame, GameCameraState& camera);

    bool IsRunning() const { return mIsRunning; }

    // Empty unless the last Start failed.
    const std::string& GetLastErrorMessage() const { return mLastErrorMessage; }

    // Name reported by the running game module; empty while stopped.
    const std::string& GetGameName() const { return mGameName; }

private:
    bool LoadModule();
    void UnloadModule();

    HMODULE mModule = nullptr;
    GameGetApiVersionFn mGetApiVersion = nullptr;
    GameGetNameFn mGetName = nullptr;
    GameStartFn mStart = nullptr;
    GameUpdateFn mUpdate = nullptr;
    GameStopFn mStop = nullptr;

    bool mIsRunning = false;
    std::string mLastErrorMessage;
    std::string mGameName;
};
