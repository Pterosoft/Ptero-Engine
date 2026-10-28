#include "pch.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "Game.h"
#include "FarkleConfig.h"
#include "../Network/NetworkAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <vector>
// Online play: Farkle's side of it. Network.dll (Source/Network) does the Steam work - lobbies,
// Quick Match, invites, the peer-to-peer link, chat and voice - and this file decides what the
// messages mean.
//
// The host runs every rule for both seats; the joining player sends the actions their clicks
// make and draws the snapshots the host sends back after every change. Dice stay fair through
// commit-reveal: the host commits to a secret seed before seeing the joiner's, both seeds then
// seed the dice, and the joiner re-rolls each throw itself and leaves if a face differs.
//
// A public table is played against the AI until someone takes its seat (at the next turn, with
// the AI's score); a friends table starts a fresh match when the friend arrives. If either
// player leaves, the one who stays carries on against the AI.
namespace {
// Players on different builds never meet: Quick Match and invites compare this.
constexpr const char* kOnlineVersion="farkle-net-1";
enum Msg:unsigned char{Hello=1,Welcome,Seed,Reveal,State,Act};
constexpr int kChatLines=7;

struct Writer{
    std::vector<unsigned char> Bytes;
    template<class T>Writer& Put(T v){const auto* p=reinterpret_cast<const unsigned char*>(&v);Bytes.insert(Bytes.end(),p,p+sizeof(T));return *this;}
};
struct Reader{
    const unsigned char* Data;int Size,At=0;bool Ok=true;
    template<class T>T Get(){T v{};if(At+int(sizeof(T))>Size){Ok=false;return v;}std::memcpy(&v,Data+At,sizeof(T));At+=int(sizeof(T));return v;}
};
std::string Number(int n){auto s=std::to_string(n);for(int i=int(s.size())-3;i>0;i-=3)s.insert(i,",");return s;}
std::string Escape(const std::string& s){std::string r;for(char c:s){if(c=='&')r+="&amp;";else if(c=='<')r+="&lt;";else if(c=='>')r+="&gt;";else if(c=='"')r+="&quot;";else r+=c;}return r;}
// A player's own words - a Steam name or a chat line - escaped and marked (U+E000 ... U+E001)
// so the UI translation keeps it as written and lets the sentence around it be translated
// with {name} in its place: "{name} sat down at your table." (Renderer_DX12/UiLocalization).
std::string Name(const std::string& s){return "\xEE\x80\x80"+Escape(s)+"\xEE\x80\x81";}
}

struct Game::NetApi{
    bool Available=false;
    NetStartupFn Startup=nullptr;NetUpdateFn Update=nullptr;NetGetStateFn GetState=nullptr;NetGetNameFn LocalName=nullptr,PeerName=nullptr;
    NetHostTableFn HostTable=nullptr;NetBoolFn QuickMatch=nullptr,InviteFriends=nullptr,HasPeer=nullptr,IsPeerFriend=nullptr,GetMicrophone=nullptr,IsPeerMuted=nullptr;
    NetSetBoolFn SetSeatOpen=nullptr,SetMicrophone=nullptr,SetPeerMuted=nullptr;NetVoidFn Leave=nullptr;
    NetPollEventFn PollEvent=nullptr;NetSendFn Send=nullptr;NetSendChatFn SendChat=nullptr;NetIsTalkingFn IsTalking=nullptr;
    NetSha256Fn Sha256=nullptr;NetSecureRandom64Fn Random=nullptr;NetSetPresenceFn SetPresence=nullptr;
};

// ---- Setup ------------------------------------------------------------------------------

void Game::StartOnline(){
    if(mNet)return;
    // Never unloaded: Network.dll's Steam callbacks stay registered for the whole process.
    HMODULE module=LoadLibraryW(L"Network.dll");if(!module)return;
    auto version=reinterpret_cast<NetGetApiVersionFn>(GetProcAddress(module,"Net_GetApiVersion"));
    if(!version||version()!=NetworkApiVersion)return;
    auto* net=new NetApi;
    auto get=[module](auto& fn,const char* name){fn=reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module,name));return fn!=nullptr;};
    bool ok=get(net->Startup,"Net_Startup")&&get(net->Update,"Net_Update")&&get(net->GetState,"Net_GetState")&&get(net->LocalName,"Net_GetLocalName")&&get(net->PeerName,"Net_GetPeerName")
        &&get(net->HostTable,"Net_HostTable")&&get(net->QuickMatch,"Net_QuickMatch")&&get(net->InviteFriends,"Net_InviteFriends")&&get(net->HasPeer,"Net_HasPeer")
        &&get(net->IsPeerFriend,"Net_IsPeerFriend")&&get(net->GetMicrophone,"Net_GetMicrophone")&&get(net->IsPeerMuted,"Net_IsPeerMuted")&&get(net->SetSeatOpen,"Net_SetSeatOpen")
        &&get(net->SetMicrophone,"Net_SetMicrophone")&&get(net->SetPeerMuted,"Net_SetPeerMuted")&&get(net->Leave,"Net_Leave")&&get(net->PollEvent,"Net_PollEvent")
        &&get(net->Send,"Net_Send")&&get(net->SendChat,"Net_SendChat")&&get(net->IsTalking,"Net_IsTalking")&&get(net->Sha256,"Net_Sha256")&&get(net->Random,"Net_SecureRandom64")&&get(net->SetPresence,"Net_SetPresence");
    if(!ok){delete net;return;}
    mNet=net;mNet->Available=mNet->Startup(kOnlineVersion);
    if(mNet->Available){char name[64]{};mNet->LocalName(name,int(sizeof(name)));mLocalName=name;}
    mStatusTimer=0;
}
void Game::StopOnline(){if(Online()&&mNet)mNet->Leave();mRole=Role::Offline;mSeat1Remote=mTakeoverPending=mAutopilot=mPeerReady=mSeedReady=false;}

// ---- Per frame ----------------------------------------------------------------------------

void Game::UpdateOnline(float dt){
    if(!mNet||!mNet->Available)return;
    mNet->Update(dt);
    NetEvent event;for(int n=0;n<64&&mNet->PollEvent(&event);++n)HandleNetEvent(event);
    if(mRole==Role::Host&&mSeat1Remote&&mPhase==Phase::Choosing&&!AiTurn()){
        mTurnClock-=dt;
        // Out of time: the AI finishes the turn, starting from the best scoring dice.
        if(mTurnClock<=0){
            mAutopilot=true;if(mHasRoll){unsigned best=BestSelection();for(int i=0;i<6;++i)mDice[i].Selected=(best&(1u<<i))!=0;}
            mPhase=Phase::Opponent;mTimer=0;Refresh();
        }
    }else if(mRole==Role::Client&&mTurnClock>0)mTurnClock-=dt;
    if(mPhase==Phase::Menu&&(mStatusTimer-=dt)<=0){mStatusTimer=0.5f;RefreshMenuStatus();}
    RefreshOnline(false);
    UpdatePresence();
}

// The line friends see under our name in Steam. The tokens and their translations are
// uploaded to Steamworks from Farkle_Steam/rich_presence/farkle_rich_presence.vdf; keep the
// two in step. Network.dll only talks to Steam when the line actually changes.
void Game::UpdatePresence(){
    const bool match=mPhase!=Phase::Menu;const int state=mNet->GetState();
    const char* token=!match?(state==NetSearching||state==NetJoining||state==NetCreating?"#Searching":"#Menu")
        :mRole==Role::Client||mSeat1Remote?"#Online":mRole==Role::Host?(mFriendsTable?"#WaitingFriends":"#Waiting"):"#VsAI";
    const int me=LocalSeat(),mine=me?mOpponentScore:mPlayerScore,theirs=me?mPlayerScore:mOpponentScore;
    // Number() groups digits the English way; Steam shows the score as it is sent.
    const std::string score=match?Number(mine)+" – "+Number(theirs):"";
    mNet->SetPresence(token,score.c_str());
}

void Game::HandleNetEvent(const NetEvent& e){
    switch(e.Type){
    case NetEventHosting:
        mRole=Role::Host;mSeat1Remote=mTakeoverPending=mPeerReady=mSeedReady=false;mFriendsTable=std::strcmp(e.Text,"friends")==0;mChat.clear();
        ChatLine(mFriendsTable?"Your table is ready. Invite friends to join you."
            :mQuickFallback?"No open tables right now. You're playing the AI: the next player looking for a match takes its seat."
            :"Your table is open. You're playing the AI until another player takes its seat.");
        if(mInviteOnHost){mInviteOnHost=false;mNet->InviteFriends();}
        mOnlineStatus.clear();if(mPhase==Phase::Menu||mPhase==Phase::Result)BeginMatch();else Refresh();
        break;
    case NetEventJoined:
        mRole=Role::Client;mPeerName=e.Name;mSeedReady=mPeerReady=false;mLatest={};mMatchId=mCheckedRoll=mEpoch=0;mRollId=0;mChat.clear();
        mClientSeed=mNet->Random();mNet->SetMicrophone(mNet->IsPeerFriend());
        {Writer w;w.Put(Hello);mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);}
        ChatLine("You joined "+Name(mPeerName)+"'s table.");mOnlineStatus="Joining "+Name(mPeerName)+"'s table…";
        break;
    case NetEventJoinFailed:mOnlineStatus=Escape(e.Text);RefreshMenuStatus();break;
    case NetEventSearchEmpty:mQuickFallback=true;mNet->HostTable(NetTablePublic);mOnlineStatus="No open tables right now. Opening yours…";break;
    case NetEventPeerJoined:
        mPeerName=e.Name;mNet->SetMicrophone(mNet->IsPeerFriend());ChatLine(Name(mPeerName)+" sat down at your table.");Refresh();break;
    case NetEventPeerLeft:
        if(mRole==Role::Client){GoOffline(Name(e.Name)+" left the table. You play on against the AI.");break;}
        if(mRole==Role::Host){
            const bool wasPlaying=mSeat1Remote;mSeat1Remote=mTakeoverPending=mPeerReady=mSeedReady=false;
            // Their turn in progress goes straight to the AI.
            if(wasPlaying&&mOpponent&&mPhase==Phase::Choosing){mAutopilot=false;if(mHasRoll){unsigned best=BestSelection();for(int i=0;i<6;++i)mDice[i].Selected=(best&(1u<<i))!=0;}mPhase=Phase::Opponent;mTimer=0;}
            ChatLine(Name(e.Name)+(wasPlaying?" left. The AI takes their seat back.":" left the table."));mPeerName.clear();Refresh();
        }
        break;
    case NetEventMessage:HandleMessage(e.Data,e.Size);break;
    case NetEventChat:ChatLine("<span class=\"chat-name\">"+Name(e.Local?mLocalName:std::string(e.Name))+":</span> "+Name(e.Text));break;
    case NetEventInvite:
        // Network.dll has already left our table for the invite (Joined follows), so only
        // the game side goes offline here: leaving again would cancel the join.
        if(Online())GoOffline("",false);mOnlineStatus="Joining your friend's table…";RefreshMenuStatus();break;
    default:break;
    }
}

// ---- Protocol ---------------------------------------------------------------------------

unsigned long long Game::EpochSeed(unsigned epoch) const{
    unsigned char bytes[20],digest[32];std::memcpy(bytes,&mHostSeed,8);std::memcpy(bytes+8,&mClientSeed,8);std::memcpy(bytes+16,&epoch,4);
    mNet->Sha256(bytes,20,digest);unsigned long long seed;std::memcpy(&seed,digest,8);return seed;
}
// From here both players draw the same faces in the same order.
void Game::NewEpoch(){if(!mSeedReady||!mNet)return;++mEpoch;mDiceRandom.seed(EpochSeed(mEpoch));}
void Game::BeginOnlineTurn(){
    mTurnClock=FarkleConfig::OnlineTurnSeconds;
    if(mRole!=Role::Host||!mTakeoverPending)return;
    mTakeoverPending=false;mSeat1Remote=true;NewEpoch();ChatLine(Name(mPeerName)+" takes the AI's seat.");
}

void Game::HandleMessage(const unsigned char* data,int size){
    Reader r{data,size};const auto type=r.Get<unsigned char>();if(!r.Ok)return;
    if(mRole==Role::Host){
        if(type==Hello){
            // Commit to a secret seed before seeing theirs: neither side can pick the dice.
            mPeerReady=true;mSeedReady=false;mHostSeed=mNet->Random();mNet->Sha256(&mHostSeed,8,mCommit);
            Writer w;w.Put(Welcome);for(unsigned char c:mCommit)w.Put(c);mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);Sync();
        }else if(type==Seed&&mPeerReady&&!mSeedReady){
            mClientSeed=r.Get<unsigned long long>();if(!r.Ok)return;mSeedReady=true;
            Writer w;w.Put(Reveal).Put(mHostSeed);mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);
            mTakeoverPending=true;
            // A friend gets a fresh match; a stranger takes the AI's seat at the next turn.
            if(mFriendsTable||mPhase==Phase::Result||mPhase==Phase::Menu)BeginMatch();
            else{ChatLine(Name(mPeerName)+" will take the AI's seat at the next turn.");Refresh();}
        }else if(type==Act){const int a=r.Get<int>();if(r.Ok)RemoteAction(a);}
        return;
    }
    if(mRole!=Role::Client)return;
    if(type==Welcome){
        for(auto& c:mCommit)c=r.Get<unsigned char>();if(!r.Ok)return;
        Writer w;w.Put(Seed).Put(mClientSeed);mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);
    }else if(type==Reveal){
        mHostSeed=r.Get<unsigned long long>();if(!r.Ok)return;
        unsigned char check[32];mNet->Sha256(&mHostSeed,8,check);
        if(std::memcmp(check,mCommit,32)!=0){Desync("The host's dice seed did not match its commitment.");return;}
        mSeedReady=true;
    }else if(type==State){
        Snapshot s;s.Valid=true;
        s.State=Phase(r.Get<unsigned char>());const auto flags=r.Get<unsigned char>();s.FinalPlayer=r.Get<signed char>();
        s.MatchId=r.Get<unsigned>();s.RollId=r.Get<unsigned>();s.Epoch=r.Get<unsigned>();
        s.Scores[0]=r.Get<int>();s.Scores[1]=r.Get<int>();s.Turn=r.Get<int>();s.Winning=r.Get<int>();s.Best[0]=r.Get<int>();s.Best[1]=r.Get<int>();
        s.ClockTenths=r.Get<short>();s.Cue=Note(r.Get<unsigned char>());
        for(auto& d:s.Dice){const auto b=r.Get<unsigned char>();d.Value=std::clamp(b&7,1,6);d.Held=(b&8)!=0;d.Selected=(b&16)!=0;d.Rolling=(b&32)!=0;}
        if(!r.Ok||int(s.State)>int(Phase::Result))return;
        s.Opponent=(flags&1)!=0;s.HasRoll=(flags&2)!=0;s.Seat1Remote=(flags&4)!=0;s.Autopilot=(flags&8)!=0;s.Won=(flags&16)!=0;
        ApplySnapshot(s);
    }
}

// Host: the whole table after every change. Reliable and in order, so nothing is skipped.
void Game::Sync(){
    if(mRole!=Role::Host||!mPeerReady||!mNet)return;
    const bool human=mSeat1Remote&&mPhase==Phase::Choosing&&!AiTurn();
    Writer w;w.Put(State).Put((unsigned char)mPhase)
        .Put((unsigned char)((mOpponent?1:0)|(mHasRoll?2:0)|(mSeat1Remote?4:0)|(mAutopilot?8:0)|(mWon?16:0))).Put((signed char)mFinalPlayer)
        .Put(mMatchId).Put(mRollId).Put(mEpoch).Put(mPlayerScore).Put(mOpponentScore).Put(mTurn).Put(mWinningScore).Put(mBestRoll[0]).Put(mBestRoll[1])
        .Put((short)(human?int(mTurnClock*10):-1)).Put((unsigned char)mNote);
    for(const auto& d:mDice)w.Put((unsigned char)(d.Value|(d.Held?8:0)|(d.Selected?16:0)|(d.Rolling?32:0)));
    mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);
}

// Joining player: check the dice, then follow the host's table.
void Game::ApplySnapshot(const Snapshot& s){
    if(s.Seat1Remote&&mSeedReady&&s.Epoch!=mEpoch){mEpoch=s.Epoch;mDiceRandom.seed(EpochSeed(mEpoch));}
    // Every throw since we took our seat is re-rolled here from the shared seed, and the faces
    // it showed may not change until the next throw.
    if(s.RollId!=mCheckedRoll){
        mCheckedRoll=s.RollId;
        if(s.Seat1Remote&&mSeedReady){std::uniform_int_distribution<int> face(1,6);
            for(const auto& d:s.Dice)if(d.Rolling&&face(mDiceRandom)!=d.Value){Desync("A die the host rolled did not match the agreed seed.");return;}}
        for(int i=0;i<6;++i)mCheckedFaces[i]=s.Dice[i].Value;
    }else if(s.Seat1Remote&&mSeedReady)
        for(int i=0;i<6;++i)if(s.Dice[i].Value!=mCheckedFaces[i]){Desync("The host changed a die after it was rolled.");return;}
    mLatest=s;
    if(s.MatchId!=mMatchId){
        mMatchId=s.MatchId;mPaused=false;Ui(2,"dialog","hidden");
        if(mCamera.PositionX!=FarkleConfig::TableCamera.PositionX){HideDice();Travel(false);}
        else if(mPhase!=Phase::Travelling){Load(HudUi());ApplyFields(s);}
        return;
    }
    if(mPhase==Phase::Travelling)return; // applied when the camera arrives
    if(s.State==Phase::Travelling||s.State==Phase::Result){
        if(mPhase!=Phase::Result){ApplyFields(s);mWon=mOpponentScore>mPlayerScore;HideDice();Travel(true);}
        return;
    }
    if(mPhase==Phase::Rolling||mPhase==Phase::Result)return; // the throw lands first; Animate applies mLatest
    ApplyFields(s);
}

void Game::ApplyFields(const Snapshot& s){
    if(!s.Valid)return;
    mPlayerScore=s.Scores[0];mOpponentScore=s.Scores[1];mTurn=s.Turn;mWinningScore=s.Winning;mFinalPlayer=s.FinalPlayer;mBestRoll={s.Best[0],s.Best[1]};
    mOpponent=s.Opponent;mHasRoll=s.HasRoll;mSeat1Remote=s.Seat1Remote;mAutopilot=s.Autopilot;mNote=s.Cue;mTurnClock=s.ClockTenths/10.f;
    for(int i=0;i<6;++i){auto& d=mDice[i];d.Value=s.Dice[i].Value;d.Held=s.Dice[i].Held;d.Selected=s.Dice[i].Selected;d.Rolling=false;}
    if(s.State==Phase::Rolling){
        if(s.RollId!=mRollId){
            // A new throw: animate it from the same faces the host rolled.
            mRollId=s.RollId;std::uniform_real_distribution<float> spin(0.85f,1.35f);
            for(int i=0;i<6;++i){auto& d=mDice[i];d.Rolling=s.Dice[i].Rolling;if(d.Rolling){d.From=d.Home;d.Spin=spin(mRandom);}}
            mPhase=Phase::Rolling;mTimer=0;Sound("DiceRoll");
        }else mPhase=Phase::Opponent; // landed before the host's result arrived: wait, dice showing
    }else if(s.State==Phase::Choosing||s.State==Phase::Opponent||s.State==Phase::Delay)mPhase=s.State;
    Refresh();
}

void Game::SendAction(int a){if(!mNet)return;Writer w;w.Put(Act).Put(a);mNet->Send(w.Bytes.data(),int(w.Bytes.size()),true);}

// Host: the other player's move, accepted only when it is theirs to make.
void Game::RemoteAction(int a){
    if(a==Rematch){if(mPhase==Phase::Result)BeginMatch();return;}
    if(!mSeat1Remote||!mOpponent||mAutopilot||mPhase!=Phase::Choosing)return;
    const bool move=a==Roll||a==Bank||a==Clear||(a>=SelectDie&&a<SelectDie+6)||(a>=RemoveDie&&a<RemoveDie+6);
    if(move)PlayAction(a);
}

// ---- Leaving ------------------------------------------------------------------------------

// Back to a game against the AI, keeping the match. The joining player turns the table
// round first: offline, the local player is always seat 0.
void Game::GoOffline(const std::string& message,bool leave){
    if(!Online())return;
    const bool client=mRole==Role::Client;
    if(leave)mNet->Leave();
    if(client){std::swap(mPlayerScore,mOpponentScore);std::swap(mBestRoll[0],mBestRoll[1]);mOpponent=!mOpponent;if(mFinalPlayer>=0)mFinalPlayer=1-mFinalPlayer;mWon=mPlayerScore>mOpponentScore;}
    mRole=Role::Offline;mSeat1Remote=mTakeoverPending=mAutopilot=mPeerReady=mSeedReady=mFriendsTable=false;mPeerName.clear();
    mDiceRandom.seed(std::random_device{}());
    const bool inMatch=mPhase==Phase::Choosing||mPhase==Phase::Opponent||mPhase==Phase::Rolling||mPhase==Phase::Delay;
    if(mOpponent&&mPhase==Phase::Choosing){if(mHasRoll){unsigned best=BestSelection();for(int i=0;i<6;++i)mDice[i].Selected=(best&(1u<<i))!=0;}mPhase=Phase::Opponent;mTimer=0;}
    else if(!mOpponent&&mPhase==Phase::Opponent)mPhase=Phase::Choosing; // our own turn we were waiting on
    if(!message.empty()){
        mOnlineStatus=message;
        if(inMatch){mPaused=true;Text("dialog-title","Online");Text("dialog-text",message);Ui(3,"dialog","hidden");}
    }
    Refresh();RefreshMenuStatus();
}
void Game::Desync(const std::string& why){GoOffline(why+" The online match was ended. You play on against the AI.");}

// ---- Buttons ------------------------------------------------------------------------------

void Game::OnlineAction(int a){
    if(!mNet||!mNet->Available){mOnlineStatus="Online play needs Steam. Start Farkle from your Steam library.";RefreshMenuStatus();return;}
    const bool menu=mPhase==Phase::Menu;const int state=mNet->GetState();
    switch(a){
    case QuickMatch:if(menu&&state==NetOffline){mQuickFallback=false;mNet->QuickMatch();mOnlineStatus="Looking for an open table…";}break;
    case HostPublic:if(menu&&state==NetOffline){mQuickFallback=false;mNet->HostTable(NetTablePublic);mOnlineStatus="Opening your table…";}break;
    case HostFriends:if(menu&&state==NetOffline){mQuickFallback=false;mInviteOnHost=true;mNet->HostTable(NetTableFriends);mOnlineStatus="Setting up a table for friends…";}break;
    case InviteFriends:if(Online())mNet->InviteFriends();break;
    case CancelSearch:if(!Online()){mNet->Leave();mOnlineStatus.clear();}break;
    case LeaveTable:GoOffline("You left the table. You play on against the AI.");break;
    case ChatSend:SendChat();break;
    case ToggleMicrophone:mNet->SetMicrophone(!mNet->GetMicrophone());break;
    case ToggleMute:mNet->SetPeerMuted(!mNet->IsPeerMuted());break;
    case ToggleSeat:if(mRole==Role::Host&&!mFriendsTable)mNet->SetSeatOpen(!mNet->HasPeer());break;
    default:break;
    }
    RefreshMenuStatus();RefreshOnline(true);
}

void Game::SendChat(){
    char line[NetMaxChat+1]{};
    if(!mHost.GetUiValue||!mHost.GetUiValue(mHost.User,"chat-input",line,int(sizeof(line))))return;
    std::string text=line;const auto first=text.find_first_not_of(" \t"),last=text.find_last_not_of(" \t");
    if(first==std::string::npos)return;
    // Our own line comes back through the lobby chat, so it appears once, in order.
    if(mNet&&mNet->SendChat(text.substr(first,last-first+1).c_str()))Ui(6,"chat-input","");
}

void Game::ChatLine(const std::string& markup){
    mChat.push_back(markup);while(int(mChat.size())>kChatLines)mChat.pop_front();
    std::string rows;for(const auto& s:mChat)rows+="<div class=\"chat-row\">"+s+"</div>";Ui(1,"chat-log",rows.c_str());
}

// ---- Drawing ------------------------------------------------------------------------------

// The HUD's online panel. Called every frame: only what changed is sent, unless forced (a
// newly loaded document starts from its defaults).
void Game::RefreshOnline(bool force){
    if(mPhase!=Phase::Choosing&&mPhase!=Phase::Opponent&&mPhase!=Phase::Rolling&&mPhase!=Phase::Delay)return;
    if(force)Ui(Online()?3:2,"online-panel","online-hidden");
    if(!Online()||!mNet)return;
    const bool peer=mNet->HasPeer(),mic=mNet->GetMicrophone(),mute=mNet->IsPeerMuted(),localTalk=mNet->IsTalking(true),peerTalk=mNet->IsTalking(false);
    const bool human=mSeat1Remote&&mPhase==Phase::Choosing&&!mAutopilot;const int clock=human?std::max(0,int(std::ceil(mTurnClock))):-1;
    if(force){
        Text("player-name",Name(mLocalName));Text("opponent-name",mSeat1Remote&&!mPeerName.empty()?Name(mPeerName):"Opponent");
        std::string rows;for(const auto& s:mChat)rows+="<div class=\"chat-row\">"+s+"</div>";Ui(1,"chat-log",rows.c_str());
        Text("table-status",mRole==Role::Client?"Playing at "+Name(mPeerName)+"'s table":peer?(mSeat1Remote?"Playing online":"A player is taking a seat…")
            :mFriendsTable?"Waiting for friends to join":"Table open: waiting for a player");
        Ui(mRole==Role::Host&&!peer?4:5,"invite-button");Ui(peer?4:5,"mute-button");
    }
    if(force||mic!=mUiMic){mUiMic=mic;Text("mic-button",mic?"Mic On":"Mic Off");Ui(mic?2:3,"mic-button","on");}
    if(force||mute!=mUiMute){mUiMute=mute;Text("mute-button",mute?"Unmute":"Mute");}
    if(force||localTalk!=mUiLocalTalk){mUiLocalTalk=localTalk;Ui(localTalk?2:3,"mic-button","talking");}
    if(force||peerTalk!=mUiPeerTalk){mUiPeerTalk=peerTalk;Ui(peerTalk?2:3,"opponent-name","talking");}
    if(force||clock!=mUiClock){mUiClock=clock;Text("turn-timer",clock>=0?std::to_string(clock)+"s":"");Ui(clock>=0&&clock<=10?2:3,"turn-timer","urgent");}
}

// online.rml: the status line, and which buttons can be used right now.
void Game::RefreshMenuStatus(){
    std::string status;int state=NetOffline;
    if(!mNet)status="Online play is not available in this build.";
    else if(!mNet->Available)status="Online play needs Steam. Start Farkle from your Steam library.";
    else{state=mNet->GetState();status=state==NetSearching?"Looking for an open table…":state==NetCreating?"Opening your table…":state==NetJoining?"Joining a table…":mOnlineStatus;}
    const bool idle=mNet&&mNet->Available&&state==NetOffline;
    Text("online-status",status);
    for(const char* id:{"quick-match-button","host-public-button","host-friends-button"})Ui(idle?4:5,id);
    Ui(idle||!mNet||!mNet->Available?2:3,"cancel-search-button","online-hidden");
}
