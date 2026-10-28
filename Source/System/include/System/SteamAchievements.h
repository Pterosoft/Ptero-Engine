#pragma once

#include <map>
#include <set>
#include <string>

// Steam achievements and INT stats for a play session, addressed by the API names set up on
// Steamworks ("ACH_FIRST_VICTORY", "STAT_DICE_ROLLED"). Game.dll reaches it through
// GameServices and the Node Graph through NodeGraphHost, so both see the same state.
//
// Steam itself is started by the game launcher (GameLauncher/SteamSupport.cpp) for a build
// with a Steam App ID. This only borrows the steam_api64.dll the launcher already loaded,
// through the SDK's flat C exports, so the engine neither links against Steam nor loads it:
// the editor, and a build without an App ID, never touch the DLL. The renderer owns the one
// instance (DX12SceneRenderer::mAchievements) and hands it to both.
//
// Without Steam - playing from the editor, a non-Steam build - unlocks and stats are kept
// for the play session only and written to the log, so achievement logic can be tested
// without the Steam client. Nothing is saved; the next session starts clean.
class SteamAchievements
{
public:
    // Clears the session-only record. Called when a play session starts.
    void BeginSession();
    // Sends pending stat changes to Steam. Called when a session ends, and before a
    // standalone game quits.
    void Flush();
    // Stat changes are batched: Steam asks for StoreStats to be called sparingly, so they go
    // out a few seconds after the last change rather than with every die thrown.
    void Update(float deltaSeconds);

    bool IsSteamRunning();

    // True when the achievement is now unlocked, including when it already was. Unlocking
    // stores at once, since that is what makes Steam show its notification.
    bool Unlock(const std::string& name);
    bool IsUnlocked(const std::string& name);
    // Locks an achievement again - for testing; Steam only allows it for the developer's
    // own accounts in practice, but it works on any account.
    bool Clear(const std::string& name);
    // Shows Steam's "x / y" progress notification. Unlocks nothing.
    bool ShowProgress(const std::string& name, int progress, int maximum);

    // False when the stat does not exist on Steamworks (or, without Steam, was never set).
    bool GetStat(const std::string& name, int& value);
    // An achievement with this stat as its progress stat unlocks by itself on Steam once
    // the value reaches the achievement's maximum.
    bool SetStat(const std::string& name, int value);

private:
    // The flat API's functions, resolved from the loaded steam_api64.dll. Each takes the
    // ISteamUserStats pointer as its first argument.
    struct Api
    {
        bool Bound = false;
        void* UserStats = nullptr;
        bool (*GetAchievement)(void*, const char*, bool*) = nullptr;
        bool (*SetAchievement)(void*, const char*) = nullptr;
        bool (*ClearAchievement)(void*, const char*) = nullptr;
        bool (*IndicateAchievementProgress)(void*, const char*, unsigned, unsigned) = nullptr;
        bool (*GetStatInt32)(void*, const char*, int*) = nullptr;
        bool (*SetStatInt32)(void*, const char*, int) = nullptr;
        bool (*StoreStats)(void*) = nullptr;
    };

    // Binds once per session; Steam is either running for the whole process or not at all.
    Api& Steam();
    void Store();

    Api mApi;
    bool mApiChecked = false;
    bool mDirty = false;
    float mSecondsSinceChange = 0.0f;

    // Without Steam.
    std::set<std::string> mSessionUnlocked;
    std::map<std::string, int> mSessionStats;
};
