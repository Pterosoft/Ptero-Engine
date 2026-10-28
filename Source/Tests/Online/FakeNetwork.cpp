// A stand-in Network.dll for OnlineTests: the same exports as Source/Network, but the "Steam"
// is two players in one process. The test loads two copies of Game.dll (each with its own
// statics) that both load this one module, and calls Fake_SetSide before updating each, so
// every Net_* call knows whose it is. Messages one side sends are queued for the other.

#include "../../Network/NetworkAPI.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace
{
    struct Queued { int Type = NetEventNone; bool Local = false; std::string Name, Text; std::vector<unsigned char> Data; };
    struct Side
    {
        NetState State = NetOffline;
        int Kind = NetTablePublic;
        bool Linked = false, Mic = false, Muted = false;
        std::string Presence;
        std::deque<Queued> Events;
        Queued Polled;
    };
    Side gSides[2];
    int gActive = 0;
    bool gCorruptNextRoll = false;
    int gDropMessages = 0;
    const char* const kNames[2] = { "Host", "Guest" };

    Side& Me() { return gSides[gActive]; }
    Side& Other() { return gSides[1 - gActive]; }
    void Push(Side& side, int type, std::string name = {}, std::string text = {})
    {
        Queued q; q.Type = type; q.Name = std::move(name); q.Text = std::move(text);
        side.Events.push_back(std::move(q));
    }
    void Copy(char* to, int capacity, const std::string& from)
    {
        if (!to || capacity <= 0) return;
        const size_t n = from.size() < size_t(capacity - 1) ? from.size() : size_t(capacity - 1);
        std::memcpy(to, from.data(), n); to[n] = '\0';
    }
}

extern "C"
{
__declspec(dllexport) void __stdcall Fake_SetSide(int side) { gActive = side; }
// The next state message carrying a throw arrives with one face changed, as a cheating host's would.
__declspec(dllexport) void __stdcall Fake_CorruptNextRoll() { gCorruptNextRoll = true; }
__declspec(dllexport) int __stdcall Fake_GetState(int side) { return gSides[side].State; }
__declspec(dllexport) bool __stdcall Fake_GetMicrophone(int side) { return gSides[side].Mic; }
__declspec(dllexport) void __stdcall Fake_Reset() { gSides[0] = Side{}; gSides[1] = Side{}; gCorruptNextRoll = false; }

__declspec(dllexport) std::uint32_t __stdcall Net_GetApiVersion() { return NetworkApiVersion; }
__declspec(dllexport) bool __stdcall Net_Startup(const char*) { return true; }
__declspec(dllexport) void __stdcall Net_Update(float) {}
__declspec(dllexport) void __stdcall Net_Shutdown() {}
__declspec(dllexport) int __stdcall Net_GetState() { return Me().State; }
__declspec(dllexport) void __stdcall Net_GetLocalName(char* name, int capacity) { Copy(name, capacity, kNames[gActive]); }

__declspec(dllexport) bool __stdcall Net_HostTable(int kind)
{
    Me() = Side{};
    Me().State = NetHosting; Me().Kind = kind;
    Push(Me(), NetEventHosting, {}, kind == NetTableFriends ? "friends" : "public");
    return true;
}
__declspec(dllexport) bool __stdcall Net_QuickMatch()
{
    Side& other = Other();
    if (other.State == NetHosting && other.Kind == NetTablePublic && !other.Linked)
    {
        Me().State = NetJoined; Me().Linked = other.Linked = true;
        Push(Me(), NetEventJoined, kNames[1 - gActive]);
        Push(other, NetEventPeerJoined, kNames[gActive]);
    }
    else
        Push(Me(), NetEventSearchEmpty);
    return true;
}
__declspec(dllexport) bool __stdcall Net_InviteFriends() { return true; }
__declspec(dllexport) void __stdcall Net_SetSeatOpen(bool) {}
__declspec(dllexport) void __stdcall Net_Leave()
{
    if (Me().State == NetOffline) return;
    if (Me().Linked)
    {
        Side& other = Other();
        other.Linked = false;
        Push(other, NetEventPeerLeft, kNames[gActive]);
        if (other.State == NetJoined) other.State = NetOffline; // the host left: a two-seat table ends
    }
    Me() = Side{};
    Push(Me(), NetEventLeft);
}
__declspec(dllexport) bool __stdcall Net_HasPeer() { return Me().Linked; }
__declspec(dllexport) bool __stdcall Net_IsPeerFriend() { return false; }
__declspec(dllexport) void __stdcall Net_GetPeerName(char* name, int capacity) { Copy(name, capacity, Me().Linked ? kNames[1 - gActive] : ""); }

__declspec(dllexport) bool __stdcall Net_PollEvent(NetEvent* event)
{
    Side& me = Me();
    if (me.Events.empty()) return false;
    me.Polled = std::move(me.Events.front());
    me.Events.pop_front();
    *event = NetEvent{};
    event->Type = me.Polled.Type; event->Local = me.Polled.Local;
    event->Size = int(me.Polled.Data.size()); event->Data = me.Polled.Data.empty() ? nullptr : me.Polled.Data.data();
    Copy(event->Name, int(sizeof(event->Name)), me.Polled.Name);
    Copy(event->Text, int(sizeof(event->Text)), me.Polled.Text);
    return true;
}
__declspec(dllexport) bool __stdcall Net_Send(const void* data, int size, bool)
{
    if (!Me().Linked || size <= 0) return false;
    Queued q; q.Type = NetEventMessage;
    const auto* bytes = static_cast<const unsigned char*>(data);
    q.Data.assign(bytes, bytes + size);
    // State messages end with six die bytes: value | held 8 | selected 16 | rolling 32.
    if (gCorruptNextRoll && q.Data[0] == 5 && size >= 6)
        for (int i = size - 6; i < size; ++i)
            if (q.Data[i] & 32) { const int v = q.Data[i] & 7; q.Data[i] = static_cast<unsigned char>((q.Data[i] & ~7) | (v % 6 + 1)); gCorruptNextRoll = false; break; }
    Other().Events.push_back(std::move(q));
    return true;
}
__declspec(dllexport) bool __stdcall Net_SendChat(const char* utf8)
{
    if (Me().State == NetOffline) return false;
    Queued mine; mine.Type = NetEventChat; mine.Local = true; mine.Name = kNames[gActive]; mine.Text = utf8;
    Queued theirs = mine; theirs.Local = false;
    Me().Events.push_back(std::move(mine));
    if (Me().Linked) Other().Events.push_back(std::move(theirs));
    return true;
}
__declspec(dllexport) void __stdcall Net_SetMicrophone(bool on) { Me().Mic = on; }
__declspec(dllexport) bool __stdcall Net_GetMicrophone() { return Me().Mic; }
__declspec(dllexport) void __stdcall Net_SetPeerMuted(bool muted) { Me().Muted = muted; }
__declspec(dllexport) bool __stdcall Net_IsPeerMuted() { return Me().Muted; }
__declspec(dllexport) bool __stdcall Net_IsTalking(bool) { return false; }
__declspec(dllexport) void __stdcall Net_SetPresence(const char* token, const char*) { Me().Presence = token ? token : ""; }
__declspec(dllexport) const char* __stdcall Fake_GetPresence(int side) { return gSides[side].Presence.c_str(); }
__declspec(dllexport) void __stdcall Net_Sha256(const void* data, int size, unsigned char digest[32])
{
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), digest, 32);
}
__declspec(dllexport) std::uint64_t __stdcall Net_SecureRandom64()
{
    std::uint64_t value = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return value;
}
}
