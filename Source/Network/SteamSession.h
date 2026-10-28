#pragma once

// One Steam lobby and the peer-to-peer link to the other player in it. See NetworkAPI.h for
// what the game module sees; this is the Steamworks side of it.
//
// Constructed only once Steam is known to be running: the STEAM_CALLBACK members register
// with steam_api64.dll as they are built, and the launcher's SteamAPI_RunCallbacks delivers
// them. It is never destroyed - the process ends with SteamAPI_Shutdown, after which
// unregistering would talk to a Steam that is gone.

#include "NetworkAPI.h"
#include "VoiceChat.h"

#pragma warning(push)
#pragma warning(disable : 4996) // matchmakingtypes.h uses strncpy
#include <steam/steam_api.h>
#pragma warning(pop)

#include <deque>
#include <string>
#include <vector>

class SteamSession
{
public:
    explicit SteamSession(const std::string& version);

    void Update(float deltaSeconds);
    NetState GetState() const { return mState; }
    std::string LocalName() const;

    bool HostTable(NetTableKind kind);
    bool QuickMatch();
    bool InviteFriends();
    void SetSeatOpen(bool open);
    void Leave();

    bool HasPeer() const { return mPeer.IsValid(); }
    bool IsPeerFriend() const;
    std::string PeerName() const;

    bool PollEvent(NetEvent& event);
    bool Send(const void* data, int size, bool reliable);
    bool SendChat(const std::string& text);

    VoiceChat& Voice() { return mVoice; }
    void SetPresence(const std::string& token, const std::string& score);

private:
    struct QueuedEvent
    {
        int Type = NetEventNone;
        bool Local = false;
        std::string Name, Text;
        std::vector<unsigned char> Data;
    };

    void Push(int type, const std::string& name = {}, const std::string& text = {});
    void JoinLobby(CSteamID lobby);
    void TryNextCandidate();
    void EnterLobby(CSteamID lobby, bool asHost);
    void SetPeer(CSteamID peer);
    void ClearPeer(bool announce);
    void UpdatePresence();
    void PublishSeat();
    void ReceiveMessages();
    bool IsLobbyMember(CSteamID user) const;
    static SteamNetworkingIdentity Identity(CSteamID user);

    // Call results: one request of each kind is in flight at a time.
    void OnLobbyCreated(LobbyCreated_t* result, bool ioFailure);
    void OnLobbyList(LobbyMatchList_t* result, bool ioFailure);
    void OnLobbyEntered(LobbyEnter_t* result, bool ioFailure);
    CCallResult<SteamSession, LobbyCreated_t> mCreateCall;
    CCallResult<SteamSession, LobbyMatchList_t> mListCall;
    CCallResult<SteamSession, LobbyEnter_t> mEnterCall;

    STEAM_CALLBACK(SteamSession, OnLobbyChatUpdate, LobbyChatUpdate_t);
    STEAM_CALLBACK(SteamSession, OnLobbyChatMessage, LobbyChatMsg_t);
    STEAM_CALLBACK(SteamSession, OnLobbyJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(SteamSession, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t);
    STEAM_CALLBACK(SteamSession, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);

    NetState mState = NetOffline;
    NetTableKind mKind = NetTablePublic;
    std::string mVersion;
    CSteamID mLobby, mPeer;
    bool mSeatOpen = true;
    // An invite accepted while we were busy: joined once the current lobby is left.
    CSteamID mPendingInvite;
    // Quick Match: open tables still to try, nearest first.
    std::vector<CSteamID> mCandidates;
    bool mInviteAfterHosting = false;
    bool mFilterReady = false;
    bool mPresenceHadPeer = false;
    std::string mPresenceToken, mPresenceScore;

    std::deque<QueuedEvent> mEvents;
    QueuedEvent mPolled; // what the last NetEvent's pointers refer to
    VoiceChat mVoice;
};
