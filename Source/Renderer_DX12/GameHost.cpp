#include "pch.h"

#include "GameHost.h"

#include <sstream>

GameHost::~GameHost()
{
    Stop();
}

bool GameHost::LoadModule()
{
    if (mModule != nullptr)
        return true;

    // Plain module name: Windows searches the executable's directory first, which is the
    // same Binaries folder the editor loads Renderer_DX12.dll from.
    mModule = LoadLibraryW(L"Game.dll");
    if (mModule == nullptr)
    {
        std::ostringstream errorBuilder;
        errorBuilder << "LoadLibraryW(\"Game.dll\") failed. Win32 error: " << GetLastError();
        mLastErrorMessage = errorBuilder.str();
        return false;
    }

    mGetApiVersion = reinterpret_cast<GameGetApiVersionFn>(GetProcAddress(mModule, "Game_GetApiVersion"));
    mGetName = reinterpret_cast<GameGetNameFn>(GetProcAddress(mModule, "Game_GetName"));
    mStart = reinterpret_cast<GameStartFn>(GetProcAddress(mModule, "Game_Start"));
    mUpdate = reinterpret_cast<GameUpdateFn>(GetProcAddress(mModule, "Game_Update"));
    mStop = reinterpret_cast<GameStopFn>(GetProcAddress(mModule, "Game_Stop"));

    if (mGetApiVersion == nullptr || mStart == nullptr || mUpdate == nullptr || mStop == nullptr)
    {
        std::ostringstream errorBuilder;
        errorBuilder
            << "Game.dll is missing required exports. "
            << "GetApiVersion=" << (mGetApiVersion != nullptr)
            << ", Start=" << (mStart != nullptr)
            << ", Update=" << (mUpdate != nullptr)
            << ", Stop=" << (mStop != nullptr);
        mLastErrorMessage = errorBuilder.str();
        UnloadModule();
        return false;
    }

    const std::uint32_t moduleApiVersion = mGetApiVersion();
    if (moduleApiVersion != GameApiVersion)
    {
        std::ostringstream errorBuilder;
        errorBuilder
            << "Game.dll reports API version " << moduleApiVersion
            << " but the editor expects " << GameApiVersion
            << ". Rebuild the Game project.";
        mLastErrorMessage = errorBuilder.str();
        UnloadModule();
        return false;
    }

    return true;
}

void GameHost::UnloadModule()
{
    if (mModule != nullptr)
    {
        FreeLibrary(mModule);
        mModule = nullptr;
    }

    mGetApiVersion = nullptr;
    mGetName = nullptr;
    mStart = nullptr;
    mUpdate = nullptr;
    mStop = nullptr;
    mGameName.clear();
}

bool GameHost::Start(const GameCameraState& initialCamera, const GameServices& services)
{
    if (mIsRunning)
        return true;

    mLastErrorMessage.clear();
    if (!LoadModule())
        return false;

    if (!mStart(&initialCamera, &services))
    {
        mStop();
        mLastErrorMessage = "Farkle could not start. Open Farkle.json and ensure Table and Dice1 through Dice6 (or Dice 1 through Dice 6) exist.";
        UnloadModule();
        return false;
    }

    if (mGetName != nullptr)
    {
        const char* name = mGetName();
        mGameName = (name != nullptr) ? name : "";
    }

    mIsRunning = true;
    return true;
}

void GameHost::Stop()
{
    if (mIsRunning && mStop != nullptr)
        mStop();

    mIsRunning = false;
    UnloadModule();
}

void GameHost::Update(const GameFrameContext& frame, GameCameraState& camera)
{
    if (!mIsRunning || mUpdate == nullptr)
        return;

    mUpdate(&frame, &camera);
}
