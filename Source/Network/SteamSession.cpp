#include "pch.h"
#include "SteamSession.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Lobby data. Every Farkle lobby carries "game" so Quick Match never lists another
    // title's lobbies (Steam already scopes them to the App ID; this guards test App IDs).
    constexpr const char* kGameKey = "game";
    constexpr const char* kGameValue = "farkle";
    constexpr const char* kVersionKey = "ver";
    constexpr const char* kSeatKey = "seat";
    constexpr int kGameChannel = 0, kVoiceChannel = 1;
    constexpr int kMaxCandidates = 20;

    void Copy(char* to, int capacity, const std::string& from)
    {
        if (to == nullptr || capacity <= 0)
            return;
        const size_t count = (std::min)(from.size(), size_t(capacity - 1));
        std::memcpy(to, from.data(), count);
        to[count] = '\0';
    }

    // "+connect_lobby <id>" - what Steam passes when a friend's Join or an invite starts the
    // game, and what our rich presence "connect" string says.
    CSteamID ParseConnect(const std::string& text)
    {
        const auto at = text.find("+connect_lobby");
        if (at == std::string::npos)
            return CSteamID();
        const auto digits = text.find_first_of("0123456789", at + 14);
        if (digits == std::string::npos)
            return CSteamID();
        CSteamID lobby(static_cast<uint64>(std::strtoull(text.c_str() + digits, nullptr, 10)));
        return lobby.IsLobby() ? lobby : CSteamID();
    }

    std::string LaunchCommandLine()
    {
        const wchar_t* wide = GetCommandLineW();
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        std::string text(bytes > 0 ? bytes - 1 : 0, '\0');
        if (bytes > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
        return text;
    }
}

SteamSession::SteamSession(const std::string& version)
    : mVersion(version)
{
    // Relay access takes a few seconds to come up; start it before anyone wants a table.
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    mFilterReady = SteamUtils()->InitFilterText(0);
    // Started from an invite or a friend's Join while the game was not running.
    mPendingInvite = ParseConnect(LaunchCommandLine());
    if (mPendingInvite.IsValid())
        Push(NetEventInvite);
}

std::string SteamSession::LocalName() const
{
    return SteamFriends()->GetPersonaName();
}

void SteamSession::Push(int type, const std::string& name, const std::string& text)
{
    QueuedEvent event;
    event.Type = type;
    event.Name = name;
    event.Text = text;
    mEvents.push_back(std::move(event));
}

SteamNetworkingIdentity SteamSession::Identity(CSteamID user)
{
    SteamNetworkingIdentity identity;
    identity.SetSteamID(user);
    return identity;
}

// ---- Tables ---------------------------------------------------------------------------

bool SteamSession::HostTable(NetTableKind kind)
{
    if (mState != NetOffline)
        Leave();
    mKind = kind;
    mSeatOpen = kind == NetTablePublic;
    mState = NetCreating;
    const ELobbyType type = kind == NetTablePublic ? k_ELobbyTypePublic : k_ELobbyTypeFriendsOnly;
    mCreateCall.Set(SteamMatchmaking()->CreateLobby(type, NetMaxPlayers), this, &SteamSession::OnLobbyCreated);
    return true;
}

void SteamSession::OnLobbyCreated(LobbyCreated_t* result, bool ioFailure)
{
    if (mState != NetCreating)
        return; // left while it was being created; the lobby Steam made is abandoned with us
    if (ioFailure || result->m_eResult != k_EResultOK)
    {
        mState = NetOffline;
        Push(NetEventJoinFailed, {}, "Steam could not create a table.");
        return;
    }
    // The creator is already inside: no LobbyEnter_t is needed to start hosting.
    EnterLobby(CSteamID(result->m_ulSteamIDLobby), true);
}

bool SteamSession::QuickMatch()
{
    if (mState != NetOffline)
        Leave();
    mState = NetSearching;
    mCandidates.clear();
    ISteamMatchmaking* matchmaking = SteamMatchmaking();
    matchmaking->AddRequestLobbyListStringFilter(kGameKey, kGameValue, k_ELobbyComparisonEqual);
    matchmaking->AddRequestLobbyListStringFilter(kVersionKey, mVersion.c_str(), k_ELobbyComparisonEqual);
    matchmaking->AddRequestLobbyListStringFilter(kSeatKey, "open", k_ELobbyComparisonEqual);
    matchmaking->AddRequestLobbyListFilterSlotsAvailable(1);
    matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    matchmaking->AddRequestLobbyListResultCountFilter(kMaxCandidates);
    mListCall.Set(matchmaking->RequestLobbyList(), this, &SteamSession::OnLobbyList);
    return true;
}

void SteamSession::OnLobbyList(LobbyMatchList_t* result, bool ioFailure)
{
    if (mState != NetSearching)
        return;
    const CSteamID self = SteamUser()->GetSteamID();
    if (!ioFailure)
        for (uint32 i = 0; i < result->m_nLobbiesMatching; ++i)
        {
            const CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(int(i));
            if (lobby.IsValid() && SteamMatchmaking()->GetLobbyOwner(lobby) != self)
                mCandidates.push_back(lobby);
        }
    // Steam sorts by distance: try the nearest first.
    std::reverse(mCandidates.begin(), mCandidates.end());
    TryNextCandidate();
}

void SteamSession::TryNextCandidate()
{
    if (mCandidates.empty())
    {
        mState = NetOffline;
        Push(NetEventSearchEmpty);
        return;
    }
    const CSteamID lobby = mCandidates.back();
    mCandidates.pop_back();
    JoinLobby(lobby);
}

void SteamSession::JoinLobby(CSteamID lobby)
{
    if (mState != NetSearching)
    {
        mCandidates.clear();
        mState = NetJoining;
    }
    mEnterCall.Set(SteamMatchmaking()->JoinLobby(lobby), this, &SteamSession::OnLobbyEntered);
}

void SteamSession::OnLobbyEntered(LobbyEnter_t* result, bool ioFailure)
{
    const bool searching = mState == NetSearching;
    if (!searching && mState != NetJoining)
    {
        // Gave up on it meanwhile: do not stay in a lobby nobody is playing in.
        if (!ioFailure && result->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
            SteamMatchmaking()->LeaveLobby(CSteamID(result->m_ulSteamIDLobby));
        return;
    }
    const CSteamID lobby(result->m_ulSteamIDLobby);
    const bool entered = !ioFailure && result->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess;
    if (entered && searching)
    {
        // The seat may have been taken between the listing and the join.
        const char* seat = SteamMatchmaking()->GetLobbyData(lobby, kSeatKey);
        if (std::strcmp(seat, "open") != 0 || SteamMatchmaking()->GetNumLobbyMembers(lobby) > NetMaxPlayers)
        {
            SteamMatchmaking()->LeaveLobby(lobby);
            TryNextCandidate();
            return;
        }
    }
    if (!entered)
    {
        if (searching)
        {
            TryNextCandidate();
            return;
        }
        mState = NetOffline;
        const bool full = !ioFailure && result->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseFull;
        Push(NetEventJoinFailed, {}, full ? "That table is already full." : "Could not join that table.");
        return;
    }
    const char* version = SteamMatchmaking()->GetLobbyData(lobby, kVersionKey);
    if (std::strcmp(version, mVersion.c_str()) != 0)
    {
        SteamMatchmaking()->LeaveLobby(lobby);
        if (searching)
        {
            TryNextCandidate();
            return;
        }
        mState = NetOffline;
        Push(NetEventJoinFailed, {}, "That table is on a different version of the game.");
        return;
    }
    EnterLobby(lobby, false);
}

void SteamSession::EnterLobby(CSteamID lobby, bool asHost)
{
    mLobby = lobby;
    mCandidates.clear();
    ISteamMatchmaking* matchmaking = SteamMatchmaking();
    if (asHost)
    {
        mState = NetHosting;
        matchmaking->SetLobbyData(lobby, kGameKey, kGameValue);
        matchmaking->SetLobbyData(lobby, kVersionKey, mVersion.c_str());
        matchmaking->SetLobbyData(lobby, "kind", mKind == NetTablePublic ? "public" : "friends");
        PublishSeat();
        Push(NetEventHosting, {}, mKind == NetTablePublic ? "public" : "friends");
        if (mInviteAfterHosting)
        {
            mInviteAfterHosting = false;
            SteamFriends()->ActivateGameOverlayInviteDialog(lobby);
        }
    }
    else
    {
        mState = NetJoined;
        const char* kind = matchmaking->GetLobbyData(lobby, "kind");
        mKind = std::strcmp(kind, "friends") == 0 ? NetTableFriends : NetTablePublic;
        SetPeer(matchmaking->GetLobbyOwner(lobby));
        Push(NetEventJoined, PeerName());
    }
    UpdatePresence();
}

bool SteamSession::InviteFriends()
{
    if (mState == NetHosting || mState == NetJoined)
    {
        SteamFriends()->ActivateGameOverlayInviteDialog(mLobby);
        return true;
    }
    if (mState != NetOffline)
        return false;
    // Nothing to invite to yet: host a friends table, and open the dialog once it exists.
    mInviteAfterHosting = true;
    return HostTable(NetTableFriends);
}

void SteamSession::SetSeatOpen(bool open)
{
    mSeatOpen = open;
    if (mState == NetHosting)
        PublishSeat();
}

// Only a public table with nobody in the second seat is listed for Quick Match. A friends
// table stays joinable so invites and the friends list's Join keep working.
void SteamSession::PublishSeat()
{
    const bool listed = mKind == NetTablePublic && mSeatOpen && !mPeer.IsValid();
    SteamMatchmaking()->SetLobbyData(mLobby, kSeatKey, listed ? "open" : "taken");
    SteamMatchmaking()->SetLobbyJoinable(mLobby, mKind == NetTableFriends || listed);
}

void SteamSession::Leave()
{
    const bool wasInLobby = mLobby.IsValid();
    if (wasInLobby)
        SteamMatchmaking()->LeaveLobby(mLobby);
    ClearPeer(false);
    mLobby = CSteamID();
    mCandidates.clear();
    mInviteAfterHosting = false;
    mState = NetOffline;
    mVoice.Reset();
    UpdatePresence();
    if (wasInLobby)
        Push(NetEventLeft);
}

// ---- The other player -----------------------------------------------------------------

void SteamSession::SetPeer(CSteamID peer)
{
    mPeer = peer;
    // Messages may already be waiting: accepting up front avoids dropping the first ones.
    SteamNetworkingMessages()->AcceptSessionWithUser(Identity(peer));
}

void SteamSession::ClearPeer(bool announce)
{
    if (!mPeer.IsValid())
        return;
    const std::string name = PeerName();
    SteamNetworkingMessages()->CloseSessionWithUser(Identity(mPeer));
    mPeer = CSteamID();
    mVoice.Reset();
    if (announce)
        Push(NetEventPeerLeft, name);
}

bool SteamSession::IsPeerFriend() const
{
    return mPeer.IsValid() && SteamFriends()->HasFriend(mPeer, k_EFriendFlagImmediate);
}

std::string SteamSession::PeerName() const
{
    return mPeer.IsValid() ? SteamFriends()->GetFriendPersonaName(mPeer) : std::string();
}

bool SteamSession::IsLobbyMember(CSteamID user) const
{
    if (!mLobby.IsValid())
        return false;
    const int count = SteamMatchmaking()->GetNumLobbyMembers(mLobby);
    for (int i = 0; i < count; ++i)
        if (SteamMatchmaking()->GetLobbyMemberByIndex(mLobby, i) == user)
            return true;
    return false;
}

void SteamSession::OnLobbyChatUpdate(LobbyChatUpdate_t* update)
{
    if (CSteamID(update->m_ulSteamIDLobby) != mLobby)
        return;
    const CSteamID user(update->m_ulSteamIDUserChanged);
    const bool entered = (update->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) != 0;
    if (mState == NetHosting)
    {
        if (entered && !mPeer.IsValid() && user != SteamUser()->GetSteamID())
        {
            SetPeer(user);
            PublishSeat();
            Push(NetEventPeerJoined, PeerName());
        }
        else if (!entered && user == mPeer)
        {
            ClearPeer(true);
            PublishSeat();
        }
    }
    else if (mState == NetJoined && !entered && user == mPeer)
    {
        // The host left. A two-seat table has nobody else to hand it to, so we leave too;
        // the game carries on against the AI.
        ClearPeer(true);
        SteamMatchmaking()->LeaveLobby(mLobby);
        mLobby = CSteamID();
        mState = NetOffline;
        UpdatePresence();
    }
}

void SteamSession::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* request)
{
    // Only the player sharing our table may open a connection to us.
    const CSteamID user = request->m_identityRemote.GetSteamID();
    if (user.IsValid() && (user == mPeer || IsLobbyMember(user)))
        SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
}

// ---- Invites and rich presence -----------------------------------------------------------

void SteamSession::OnLobbyJoinRequested(GameLobbyJoinRequested_t* request)
{
    if (request->m_steamIDLobby == mLobby)
        return;
    mPendingInvite = request->m_steamIDLobby;
    Push(NetEventInvite);
}

void SteamSession::OnRichPresenceJoinRequested(GameRichPresenceJoinRequested_t* request)
{
    const CSteamID lobby = ParseConnect(request->m_rgchConnect);
    if (!lobby.IsValid() || lobby == mLobby)
        return;
    mPendingInvite = lobby;
    Push(NetEventInvite);
}

void SteamSession::SetPresence(const std::string& token, const std::string& score)
{
    if (token == mPresenceToken && score == mPresenceScore)
        return;
    mPresenceToken = token;
    mPresenceScore = score;
    UpdatePresence();
}

// steam_display picks one of the localized lines uploaded to Steamworks; the other keys are
// the values its %placeholders% read. connect is what a friend's Join (and an accepted
// invite) hands back to us; the player group puts both players of a table together in their
// friends' lists. A null value removes a key.
void SteamSession::UpdatePresence()
{
    ISteamFriends* friends = SteamFriends();
    const bool inLobby = mLobby.IsValid();
    friends->SetRichPresence("steam_display", mPresenceToken.empty() ? nullptr : mPresenceToken.c_str());
    friends->SetRichPresence("score", mPresenceScore.empty() ? nullptr : mPresenceScore.c_str());
    friends->SetRichPresence("opponent", mPeer.IsValid() ? PeerName().c_str() : nullptr);
    // Joinable only while the second seat is free.
    const std::string lobby = std::to_string(mLobby.ConvertToUint64());
    friends->SetRichPresence("connect", inLobby && !mPeer.IsValid() ? ("+connect_lobby " + lobby).c_str() : nullptr);
    friends->SetRichPresence("steam_player_group", inLobby ? lobby.c_str() : nullptr);
    friends->SetRichPresence("steam_player_group_size", inLobby ? std::to_string(SteamMatchmaking()->GetNumLobbyMembers(mLobby)).c_str() : nullptr);
}

// ---- Messages ---------------------------------------------------------------------------

bool SteamSession::Send(const void* data, int size, bool reliable)
{
    if (!mPeer.IsValid() || data == nullptr || size <= 0 || size > NetMaxMessage)
        return false;
    const int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    const EResult result = SteamNetworkingMessages()->SendMessageToUser(
        Identity(mPeer), data, uint32(size), flags | k_nSteamNetworkingSend_AutoRestartBrokenSession, kGameChannel);
    return result == k_EResultOK;
}

void SteamSession::ReceiveMessages()
{
    ISteamNetworkingMessages* messages = SteamNetworkingMessages();
    SteamNetworkingMessage_t* incoming[32];
    for (int channel : { kGameChannel, kVoiceChannel })
    {
        int count;
        while ((count = messages->ReceiveMessagesOnChannel(channel, incoming, 32)) > 0)
            for (int i = 0; i < count; ++i)
            {
                SteamNetworkingMessage_t* message = incoming[i];
                // Anything not from the player at our table is dropped unread.
                if (mPeer.IsValid() && message->m_identityPeer.GetSteamID() == mPeer && message->m_cbSize > 0)
                {
                    if (channel == kVoiceChannel)
                        mVoice.Play(message->m_pData, uint32(message->m_cbSize));
                    else if (message->m_cbSize <= NetMaxMessage)
                    {
                        QueuedEvent event;
                        event.Type = NetEventMessage;
                        const auto* bytes = static_cast<const unsigned char*>(message->m_pData);
                        event.Data.assign(bytes, bytes + message->m_cbSize);
                        mEvents.push_back(std::move(event));
                    }
                }
                message->Release();
            }
    }
}

bool SteamSession::SendChat(const std::string& text)
{
    if (!mLobby.IsValid() || text.empty())
        return false;
    const std::string line = text.substr(0, NetMaxChat);
    return SteamMatchmaking()->SendLobbyChatMsg(mLobby, line.c_str(), int(line.size() + 1));
}

void SteamSession::OnLobbyChatMessage(LobbyChatMsg_t* message)
{
    if (CSteamID(message->m_ulSteamIDLobby) != mLobby)
        return;
    CSteamID user;
    EChatEntryType type = k_EChatEntryTypeInvalid;
    char text[NetMaxChat + 1]{};
    const int size = SteamMatchmaking()->GetLobbyChatEntry(mLobby, int(message->m_iChatID), &user, text, int(sizeof(text)), &type);
    if (size <= 0 || type != k_EChatEntryTypeChatMsg)
        return;
    text[sizeof(text) - 1] = '\0';

    QueuedEvent event;
    event.Type = NetEventChat;
    event.Local = user == SteamUser()->GetSteamID();
    event.Name = event.Local ? LocalName() : std::string(SteamFriends()->GetFriendPersonaName(user));
    // Steam's filter honours each player's own chat filtering settings (profanity, slurs).
    char filtered[sizeof(text) * 2]{};
    if (!event.Local && mFilterReady
        && SteamUtils()->FilterText(k_ETextFilteringContextChat, user, text, filtered, uint32(sizeof(filtered))) >= 0)
        event.Text = filtered;
    else
        event.Text = text;
    mEvents.push_back(std::move(event));
}

bool SteamSession::PollEvent(NetEvent& event)
{
    if (mEvents.empty())
        return false;
    mPolled = std::move(mEvents.front());
    mEvents.pop_front();
    event = NetEvent{};
    event.Type = mPolled.Type;
    event.Local = mPolled.Local;
    event.Size = int(mPolled.Data.size());
    event.Data = mPolled.Data.empty() ? nullptr : mPolled.Data.data();
    Copy(event.Name, int(sizeof(event.Name)), mPolled.Name);
    Copy(event.Text, int(sizeof(event.Text)), mPolled.Text);
    return true;
}

void SteamSession::Update(float deltaSeconds)
{
    // An accepted invite: leave whatever we are doing and go to it.
    if (mPendingInvite.IsValid() && mState != NetCreating && mState != NetJoining && mState != NetSearching)
    {
        const CSteamID lobby = mPendingInvite;
        mPendingInvite = CSteamID();
        if (mState != NetOffline)
            Leave();
        JoinLobby(lobby);
    }

    ReceiveMessages();

    const std::vector<unsigned char>& voice = mVoice.Capture(deltaSeconds, mPeer.IsValid());
    if (!voice.empty() && mPeer.IsValid())
        SteamNetworkingMessages()->SendMessageToUser(Identity(mPeer), voice.data(), uint32(voice.size()),
            k_nSteamNetworkingSend_UnreliableNoDelay | k_nSteamNetworkingSend_AutoRestartBrokenSession, kVoiceChannel);

    if (mPresenceHadPeer != mPeer.IsValid())
    {
        mPresenceHadPeer = mPeer.IsValid();
        UpdatePresence();
    }
}
