#pragma once

// Steamworks for the packaged game, switched on by Game Settings' SteamAppId (0 = the
// game is not a Steam build and nothing here touches steam_api64.dll).
//
// steam_api64.dll is delay-loaded (see GameLauncher.vcxproj), so a non-Steam build runs
// without it. Build Game copies it next to the exe only when an App ID is set.

#include <cstdint>
#include <string>

namespace SteamSupport
{
    enum class StartResult
    {
        // Steam is up (or the game is not a Steam build): carry on.
        Continue,
        // Steam is relaunching the game through the client; this process must exit now.
        Relaunching,
        // Steam could not be started; outError says why, and the game must not run.
        Failed
    };

    // Call before anything else touches the window or renderer. With appId 0 it does
    // nothing and returns Continue.
    StartResult Start(std::uint32_t appId, std::wstring& outError);

    // Once per frame: dispatches Steam callbacks (overlay, achievements, ...).
    void RunCallbacks();

    void Shutdown();
}
