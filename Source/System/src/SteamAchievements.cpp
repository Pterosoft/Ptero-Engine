#include "System/SteamAchievements.h"
#include "System/PteroLog.h"

#include <windows.h>

namespace
{
    // Seconds after the last stat change before it is sent to Steam.
    constexpr float kStoreDelaySeconds = 5.0f;

    template <typename Fn>
    void Resolve(HMODULE module, const char* name, Fn& function)
    {
        function = reinterpret_cast<Fn>(GetProcAddress(module, name));
    }
}

SteamAchievements::Api& SteamAchievements::Steam()
{
    if (mApiChecked)
        return mApi;
    mApiChecked = true;

    // Only ever the module the launcher loaded: GetModuleHandle, never LoadLibrary.
    const HMODULE module = GetModuleHandleW(L"steam_api64.dll");
    if (module == nullptr)
        return mApi;

    void* (*userStats)() = nullptr;
    Resolve(module, "SteamAPI_SteamUserStats_v013", userStats);
    Resolve(module, "SteamAPI_ISteamUserStats_GetAchievement", mApi.GetAchievement);
    Resolve(module, "SteamAPI_ISteamUserStats_SetAchievement", mApi.SetAchievement);
    Resolve(module, "SteamAPI_ISteamUserStats_ClearAchievement", mApi.ClearAchievement);
    Resolve(module, "SteamAPI_ISteamUserStats_IndicateAchievementProgress", mApi.IndicateAchievementProgress);
    Resolve(module, "SteamAPI_ISteamUserStats_GetStatInt32", mApi.GetStatInt32);
    Resolve(module, "SteamAPI_ISteamUserStats_SetStatInt32", mApi.SetStatInt32);
    Resolve(module, "SteamAPI_ISteamUserStats_StoreStats", mApi.StoreStats);

    // The accessor is null until SteamAPI_Init has succeeded, which is how a DLL that is
    // merely present (a launcher that decided not to start Steam) reads as no Steam.
    mApi.UserStats = userStats != nullptr ? userStats() : nullptr;
    mApi.Bound = mApi.UserStats != nullptr && mApi.GetAchievement && mApi.SetAchievement && mApi.ClearAchievement
        && mApi.IndicateAchievementProgress && mApi.GetStatInt32 && mApi.SetStatInt32 && mApi.StoreStats;
    if (!mApi.Bound && mApi.UserStats != nullptr)
        PTERO_LOG_WARNING("Achievements", "steam_api64.dll lacks the ISteamUserStats v013 exports; achievements are off.");
    return mApi;
}

void SteamAchievements::BeginSession()
{
    mSessionUnlocked.clear();
    mSessionStats.clear();
    mDirty = false;
    mSecondsSinceChange = 0.0f;
}

bool SteamAchievements::IsSteamRunning()
{
    return Steam().Bound;
}

void SteamAchievements::Store()
{
    Api& steam = Steam();
    if (steam.Bound && !steam.StoreStats(steam.UserStats))
        PTERO_LOG_WARNING("Achievements", "Steam did not accept the stats update.");
    mDirty = false;
}

void SteamAchievements::Flush()
{
    if (mDirty)
        Store();
}

void SteamAchievements::Update(float deltaSeconds)
{
    if (!mDirty)
        return;
    mSecondsSinceChange += deltaSeconds;
    if (mSecondsSinceChange >= kStoreDelaySeconds)
        Store();
}

bool SteamAchievements::Unlock(const std::string& name)
{
    if (name.empty())
        return false;
    Api& steam = Steam();
    if (!steam.Bound)
    {
        if (mSessionUnlocked.insert(name).second)
            PTERO_LOG_INFO("Achievements", "Unlocked %s (Steam is not running: kept for this session only).", name.c_str());
        return true;
    }

    bool achieved = false;
    if (!steam.GetAchievement(steam.UserStats, name.c_str(), &achieved))
    {
        PTERO_LOG_WARNING("Achievements", "%s is not an achievement of this app on Steamworks.", name.c_str());
        return false;
    }
    if (achieved)
        return true;
    if (!steam.SetAchievement(steam.UserStats, name.c_str()))
        return false;
    PTERO_LOG_INFO("Achievements", "Unlocked %s.", name.c_str());
    Store();
    return true;
}

bool SteamAchievements::IsUnlocked(const std::string& name)
{
    Api& steam = Steam();
    if (!steam.Bound)
        return mSessionUnlocked.count(name) != 0;
    bool achieved = false;
    return steam.GetAchievement(steam.UserStats, name.c_str(), &achieved) && achieved;
}

bool SteamAchievements::Clear(const std::string& name)
{
    Api& steam = Steam();
    if (!steam.Bound)
    {
        mSessionUnlocked.erase(name);
        return true;
    }
    if (!steam.ClearAchievement(steam.UserStats, name.c_str()))
        return false;
    Store();
    return true;
}

bool SteamAchievements::ShowProgress(const std::string& name, int progress, int maximum)
{
    if (progress < 0 || maximum <= 0 || progress >= maximum)
        return false;
    Api& steam = Steam();
    if (!steam.Bound)
    {
        PTERO_LOG_INFO("Achievements", "%s progress %d / %d.", name.c_str(), progress, maximum);
        return true;
    }
    return steam.IndicateAchievementProgress(steam.UserStats, name.c_str(),
        static_cast<unsigned>(progress), static_cast<unsigned>(maximum));
}

bool SteamAchievements::GetStat(const std::string& name, int& value)
{
    Api& steam = Steam();
    if (!steam.Bound)
    {
        const auto it = mSessionStats.find(name);
        value = it == mSessionStats.end() ? 0 : it->second;
        return it != mSessionStats.end();
    }
    return steam.GetStatInt32(steam.UserStats, name.c_str(), &value);
}

bool SteamAchievements::SetStat(const std::string& name, int value)
{
    if (name.empty())
        return false;
    Api& steam = Steam();
    if (!steam.Bound)
    {
        mSessionStats[name] = value;
        return true;
    }
    // Fails for a stat Steamworks does not define, and for one marked increment-only
    // whose value would go down.
    if (!steam.SetStatInt32(steam.UserStats, name.c_str(), value))
        return false;
    mDirty = true;
    mSecondsSinceChange = 0.0f;
    return true;
}
