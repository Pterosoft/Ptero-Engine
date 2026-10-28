#include "pch.h"
#include "SteamSession.h"

#include <bcrypt.h>
#include <cstring>
#include <string>

// The exports of NetworkAPI.h. Everything goes through one SteamSession, created on the
// first Net_Startup that finds Steam running.
namespace
{
    SteamSession* gSession = nullptr;
    bool gChecked = false;

    // steam_api64.dll is delay-loaded: touching any Steam export without the launcher having
    // loaded and started it would try to load it from scratch (and fail in the editor). So
    // only ever look at the module the launcher loaded, and only once it is logged in.
    bool SteamIsRunning()
    {
        if (GetModuleHandleW(L"steam_api64.dll") == nullptr)
            return false;
        __try
        {
            return SteamAPI_GetHSteamUser() != 0 && SteamUser() != nullptr && SteamUser()->BLoggedOn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void Copy(char* to, int capacity, const std::string& from)
    {
        if (to == nullptr || capacity <= 0)
            return;
        const size_t count = (std::min)(from.size(), size_t(capacity - 1));
        std::memcpy(to, from.data(), count);
        to[count] = '\0';
    }
}

extern "C"
{
std::uint32_t __stdcall Net_GetApiVersion() { return NetworkApiVersion; }

bool __stdcall Net_Startup(const char* version)
{
    if (!gChecked)
    {
        gChecked = true;
        if (SteamIsRunning())
            gSession = new SteamSession(version ? version : "");
    }
    return gSession != nullptr;
}

void __stdcall Net_Update(float deltaSeconds) { if (gSession) gSession->Update(deltaSeconds); }
void __stdcall Net_Shutdown() { if (gSession) gSession->Leave(); }
int __stdcall Net_GetState() { return gSession ? gSession->GetState() : NetOffline; }
void __stdcall Net_GetLocalName(char* name, int capacity) { Copy(name, capacity, gSession ? gSession->LocalName() : std::string("Player")); }

bool __stdcall Net_HostTable(int kind) { return gSession && gSession->HostTable(kind == NetTableFriends ? NetTableFriends : NetTablePublic); }
bool __stdcall Net_QuickMatch() { return gSession && gSession->QuickMatch(); }
bool __stdcall Net_InviteFriends() { return gSession && gSession->InviteFriends(); }
void __stdcall Net_SetSeatOpen(bool open) { if (gSession) gSession->SetSeatOpen(open); }
void __stdcall Net_Leave() { if (gSession) gSession->Leave(); }
bool __stdcall Net_HasPeer() { return gSession && gSession->HasPeer(); }
bool __stdcall Net_IsPeerFriend() { return gSession && gSession->IsPeerFriend(); }
void __stdcall Net_GetPeerName(char* name, int capacity) { Copy(name, capacity, gSession ? gSession->PeerName() : std::string()); }

bool __stdcall Net_PollEvent(NetEvent* event) { return gSession && event && gSession->PollEvent(*event); }
bool __stdcall Net_Send(const void* data, int size, bool reliable) { return gSession && gSession->Send(data, size, reliable); }
bool __stdcall Net_SendChat(const char* utf8) { return gSession && utf8 && gSession->SendChat(utf8); }

void __stdcall Net_SetMicrophone(bool on) { if (gSession) gSession->Voice().SetMicrophone(on); }
bool __stdcall Net_GetMicrophone() { return gSession && gSession->Voice().GetMicrophone(); }
void __stdcall Net_SetPeerMuted(bool muted) { if (gSession) gSession->Voice().SetPeerMuted(muted); }
bool __stdcall Net_IsPeerMuted() { return gSession && gSession->Voice().IsPeerMuted(); }
bool __stdcall Net_IsTalking(bool local) { return gSession && gSession->Voice().IsTalking(local); }
void __stdcall Net_SetPresence(const char* token, const char* score) { if (gSession) gSession->SetPresence(token ? token : "", score ? score : ""); }

void __stdcall Net_Sha256(const void* data, int size, unsigned char digest[32])
{
    std::memset(digest, 0, 32);
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), digest, 32);
}

std::uint64_t __stdcall Net_SecureRandom64()
{
    std::uint64_t value = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return value;
}
}
