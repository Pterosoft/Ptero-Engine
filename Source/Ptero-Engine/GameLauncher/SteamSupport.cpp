#include "SteamSupport.h"

#include <windows.h>

// The SDK's matchmakingtypes.h uses strncpy, which /sdl turns into an error.
#pragma warning(push)
#pragma warning(disable : 4996)
#include "steam/steam_api.h"
#pragma warning(pop)

namespace
{
    bool gSteamRunning = false;

    std::wstring Widen(const char* text)
    {
        if (text == nullptr || text[0] == '\0')
            return std::wstring();
        const int required = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (required <= 0)
            return std::wstring();
        std::wstring wide(static_cast<size_t>(required) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), required);
        return wide;
    }
}

namespace SteamSupport
{
    StartResult Start(std::uint32_t appId, std::wstring& outError)
    {
        if (appId == 0)
            return StartResult::Continue;

        // Delay-loaded: check the DLL is really there first, since a missing one would
        // otherwise surface as an unhandled delay-load exception on the first Steam call.
        if (LoadLibraryW(L"steam_api64.dll") == nullptr)
        {
            outError = L"steam_api64.dll is missing from the game folder. Verify the game files in Steam.";
            return StartResult::Failed;
        }

        // Started outside Steam (double-clicking the exe): Steam launches it again
        // through the client, which is what puts the ownership check and overlay in
        // place. A steam_appid.txt next to the exe turns this off for local testing.
        if (SteamAPI_RestartAppIfNecessary(appId))
            return StartResult::Relaunching;

        SteamErrMsg errorMessage{};
        if (SteamAPI_InitEx(&errorMessage) != k_ESteamAPIInitResult_OK)
        {
            outError = L"Steam must be running to play this game.";
            const std::wstring detail = Widen(errorMessage);
            if (!detail.empty())
                outError += L"\n\n" + detail;
            return StartResult::Failed;
        }
        gSteamRunning = true;

        if (SteamApps() == nullptr || !SteamApps()->BIsSubscribedApp(appId))
        {
            Shutdown();
            outError = L"This copy of the game is not licensed for the signed-in Steam account.";
            return StartResult::Failed;
        }

        return StartResult::Continue;
    }

    void RunCallbacks()
    {
        if (gSteamRunning)
            SteamAPI_RunCallbacks();
    }

    void Shutdown()
    {
        if (!gSteamRunning)
            return;
        SteamAPI_Shutdown();
        gSteamRunning = false;
    }
}
