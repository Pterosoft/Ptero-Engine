#pragma once

// NetworkAPI.h - the contract between Network.dll and the game module.
//
// Network.dll owns everything Steam-specific about online play: lobbies and Quick Match,
// the peer-to-peer connection (Steam Datagram Relay through ISteamNetworkingMessages),
// friend invites and rich presence, lobby text chat with Steam's text filter, and voice
// chat (Steam's voice codec, played through XAudio2). The game module only sees plain
// data: it sends and receives opaque messages to the one other player at the table and
// decides what they mean. Like GameAPI.h, nothing here may use STL, engine or Windows
// types, and the game resolves the exports with GetProcAddress so a build without
// Network.dll still runs offline.
//
// Steam itself is started by the launcher (GameLauncher/SteamSupport), which also pumps
// SteamAPI_RunCallbacks every frame. Network.dll never initialises Steam: without a running
// Steam client (editor play, a non-Steam build) Net_Startup returns false and every other
// call is a harmless no-op.

#include <cstdint>

// Bump whenever a structure or an export changes.
inline constexpr std::uint32_t NetworkApiVersion = 2;

// Two seats at a Farkle table: the lobby holds the host and at most one other player.
inline constexpr int NetMaxPlayers = 2;
inline constexpr int NetMaxChat = 160;    // bytes of UTF-8 per chat line
inline constexpr int NetMaxMessage = 1024; // bytes per game message

enum NetState
{
    NetOffline = 0,   // not in a lobby
    NetCreating,      // creating a lobby to host
    NetSearching,     // Quick Match is looking for an open table
    NetJoining,       // entering someone else's lobby
    NetHosting,       // in our own lobby (with or without a second player)
    NetJoined         // in another player's lobby
};

enum NetTableKind
{
    NetTablePublic = 0, // listed for Quick Match; a stranger may take the AI's seat
    NetTableFriends = 1 // friends only: joined through an invite or the friends list
};

enum NetEventType
{
    NetEventNone = 0,
    NetEventHosting,     // our lobby exists; Text = its kind ("public" / "friends")
    NetEventJoined,      // we are in another player's lobby; Name = the host
    NetEventJoinFailed,  // Text says why
    NetEventSearchEmpty, // Quick Match found no open table
    NetEventPeerJoined,  // a second player entered our lobby; Name = them
    NetEventPeerLeft,    // the other player left or lost the connection; Name = them
    NetEventMessage,     // Data/Size: a game message from the other player
    NetEventChat,        // Name: who, Text: the filtered line, Local: our own line echoed back
    NetEventInvite,      // accepted an invite (or Join from the friends list) - leaving for it
    NetEventLeft         // we left the lobby (Net_Leave, or an invite took us elsewhere)
};

struct NetEvent
{
    int Type = NetEventNone;
    bool Local = false;
    int Size = 0;
    // Valid until the next Net_PollEvent.
    const unsigned char* Data = nullptr;
    char Name[64]{};
    char Text[NetMaxChat + 64]{};
};

#ifdef NETWORK_EXPORTS
#define NET_EXPORT __declspec(dllexport)
#else
#define NET_EXPORT
#endif

extern "C"
{
    NET_EXPORT std::uint32_t __stdcall Net_GetApiVersion();

    // True when Steam is running and online play is possible. Safe to call every frame.
    // `version` keeps players on different game builds apart: Quick Match only lists, and
    // invites only enter, tables of the same version. The first call's version sticks.
    NET_EXPORT bool __stdcall Net_Startup(const char* version);
    // Once per frame: delivers P2P messages, records and plays voice, polls lobby state.
    NET_EXPORT void __stdcall Net_Update(float deltaSeconds);
    NET_EXPORT void __stdcall Net_Shutdown();
    NET_EXPORT int __stdcall Net_GetState();
    // Our Steam persona name. Copied into `name` (capacity bytes, always terminated).
    NET_EXPORT void __stdcall Net_GetLocalName(char* name, int capacity);

    // Tables. Hosting replaces any table we were at; so does Quick Match.
    NET_EXPORT bool __stdcall Net_HostTable(int kind);
    NET_EXPORT bool __stdcall Net_QuickMatch();
    // Steam overlay's invite dialog for the current lobby (hosting a table first if needed).
    NET_EXPORT bool __stdcall Net_InviteFriends();
    // Host only: whether the free seat is listed for Quick Match.
    NET_EXPORT void __stdcall Net_SetSeatOpen(bool open);
    NET_EXPORT void __stdcall Net_Leave();
    NET_EXPORT bool __stdcall Net_HasPeer();
    NET_EXPORT bool __stdcall Net_IsPeerFriend();
    NET_EXPORT void __stdcall Net_GetPeerName(char* name, int capacity);

    NET_EXPORT bool __stdcall Net_PollEvent(NetEvent* event);

    // Game messages to the other player. Reliable messages arrive once and in order.
    NET_EXPORT bool __stdcall Net_Send(const void* data, int size, bool reliable);
    NET_EXPORT bool __stdcall Net_SendChat(const char* utf8);

    // Voice. The microphone is off until switched on; muting stops playback of the other
    // player only. Talking is true while a voice packet arrived or left recently.
    NET_EXPORT void __stdcall Net_SetMicrophone(bool on);
    NET_EXPORT bool __stdcall Net_GetMicrophone();
    NET_EXPORT void __stdcall Net_SetPeerMuted(bool muted);
    NET_EXPORT bool __stdcall Net_IsPeerMuted();
    NET_EXPORT bool __stdcall Net_IsTalking(bool local);

    // What friends see under our name in their Steam friends list (rich presence, API 2).
    // `token` is one of the status lines uploaded to Steamworks (Farkle_Steam/rich_presence),
    // such as "#Online"; Steam shows it in each friend's own language. `score` fills the
    // line's %score% ("1,250 – 900"), and Network.dll fills %opponent% with the other
    // player's name. An empty token shows nothing beyond the game's name.
    NET_EXPORT void __stdcall Net_SetPresence(const char* token, const char* score);

    // Fair dice: a SHA-256 for commit-reveal, and 64 bits from the OS's secure generator.
    NET_EXPORT void __stdcall Net_Sha256(const void* data, int size, unsigned char digest[32]);
    NET_EXPORT std::uint64_t __stdcall Net_SecureRandom64();
}

using NetGetApiVersionFn = std::uint32_t(__stdcall*)();
using NetStartupFn = bool(__stdcall*)(const char*);
using NetUpdateFn = void(__stdcall*)(float);
using NetShutdownFn = void(__stdcall*)();
using NetGetStateFn = int(__stdcall*)();
using NetGetNameFn = void(__stdcall*)(char*, int);
using NetHostTableFn = bool(__stdcall*)(int);
using NetBoolFn = bool(__stdcall*)();
using NetSetBoolFn = void(__stdcall*)(bool);
using NetVoidFn = void(__stdcall*)();
using NetPollEventFn = bool(__stdcall*)(NetEvent*);
using NetSendFn = bool(__stdcall*)(const void*, int, bool);
using NetSendChatFn = bool(__stdcall*)(const char*);
using NetIsTalkingFn = bool(__stdcall*)(bool);
using NetSha256Fn = void(__stdcall*)(const void*, int, unsigned char*);
using NetSecureRandom64Fn = std::uint64_t(__stdcall*)();
using NetSetPresenceFn = void(__stdcall*)(const char*, const char*);
