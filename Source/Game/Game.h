#pragma once
#include "GameAPI.h"
#include <array>
#include <deque>
#include <random>
#include <string>
class Game {
public:
    static Game& Get();
    bool Start(const GameCameraState&,const GameServices&);
    void Update(const GameFrameContext&,GameCameraState&);
    void Stop();
    bool IsRunning() const { return mRunning; }
private:
    enum class Phase { Menu, Choosing, Rolling, Opponent, Delay, Travelling, Result };
    struct Die { int Entity=-1,Value=1;bool Held=false,Selected=false,Rolling=false;GameTransform Home{},From{};float Spin=1; };
    GameServices mHost{};std::array<Die,6> mDice;
    std::mt19937 mRandom{std::random_device{}()};std::deque<std::string> mLog;
    // Dice faces only. Online, both players seed it from the commit-reveal exchange, so the
    // joining player can check every face the host rolls (GameOnline.cpp).
    std::mt19937_64 mDiceRandom{std::random_device{}()};
    Phase mPhase=Phase::Menu;GameCameraState mCamera{},mCameraFrom{},mCameraTo{};
    bool mRunning=false,mOpponent=false,mPaused=false,mHasRoll=false,mWon=false;
    // Seats: mPlayerScore is seat 0 and mOpponentScore seat 1, and mOpponent means it is seat
    // 1's turn. Seat 0 is always the local player offline and the host online; seat 1 is the
    // AI, or the other player once they have taken the AI's seat. The joining player keeps
    // the host's seat order and only flips the view (LocalSeat), so the rules, the AI and
    // the achievements never have to think about who is looking.
    int mPlayerScore=0,mOpponentScore=0,mTurn=0,mFinalPlayer=-1;std::array<int,2> mBestRoll{};
    int mWinningScore=3000;
    // What the status line says, as a cue rather than text: each player sees it from their side.
    enum class Note { Begin, SelectFirst, Rolling, Farkle, Choose };
    Note mNote=Note::Begin;float mTimer=0,mCameraTime=0;
    // Background playlist: one Music1..MusicN track at a time, reshuffled when it ends.
    // mTrack is the one currently on the music channel, kept only so the next draw can
    // avoid repeating it. Cleared when a Victory/Defeat sting takes the channel over.
    bool mMusic=false;int mTrack=-1;
    // Where a throw lands, derived from the camera rather than authored, so nudging the
    // view keeps the dice under the on-screen prompt instead of drifting off it.
    float mThrow[3]{};
    // World Z of the Table entity's top surface, from its transform (FarkleConfig::TableMeshTop).
    float mTableTop=0;
    int Score(unsigned) const;unsigned Available() const;unsigned Selection() const;unsigned BestSelection() const;
    void Action(int);void PlayAction(int);void BeginMatch();void BeginTurn();void RollDice();void FinishRoll();void EndTurn(bool);void FinishMatch();void Travel(bool);void Animate(float);void Refresh();
    void PlayAi();std::string Message() const;
    void Log(const std::string&);void Text(const char*,const std::string&);void Ui(int,const char*,const char* = "");void Load(const char*);
    void Sound(const char*);void Music(const char*);void UpdateMusic();
    // The editor tests with the original HUD (with its log) and menu; players get the new ones.
    const char* MenuUi() const { return mHost.Standalone?"Farkle/menu.rml":"Farkle/menu-editor.rml"; }
    const char* HudUi() const { return mHost.Standalone?"Farkle/game.rml":"Farkle/farkle.rml"; }
    void AimThrow();GameTransform Landing(int) const;void HideDice();

    // ---- Steam achievements (GameAchievements.cpp). Only the player's own play counts.
    // Per match: farkled at all, the player's turns and rolls, the widest the opponent led
    // by, a roll with fewer than three dice, path tracing on for every turn.
    bool mAchFarkled=false,mAchRolledFew=false,mAchPathTraced=true;int mAchTurns=0,mAchRolls=0,mAchMaxDeficit=0;
    // Per turn: hot dice so far, whether every die scored was a 5, dice scored, a roll with
    // one die left waiting for its result, and deselections per die.
    int mAchHotDice=0,mAchScoredDice=0;bool mAchOnlyFives=true,mAchOneDie=false;std::array<int,6> mAchDeselects{};
    // Across matches while the game runs: farkles in a row, wins in a row, and which of the
    // eight combination types (single 1, single 5, three/four/five/six of a kind, three
    // pairs, straight) have been scored.
    int mAchFarkleStreak=0,mAchWinStreak=0;unsigned mAchCombos=0;
    // ---- Online play (GameOnline.cpp) over Network.dll: Steam lobbies, P2P, chat and voice.
    // The host runs the rules for both seats; the other player sends their clicks as actions
    // and draws the host's snapshots. Nothing here runs when Network.dll or Steam is missing.
    enum class Role { Offline, Host, Client };
    struct Snapshot {
        bool Valid=false;Phase State=Phase::Menu;bool Opponent=false,HasRoll=false,Seat1Remote=false,Autopilot=false,Won=false;
        int FinalPlayer=-1;unsigned MatchId=0,RollId=0,Epoch=0;int Scores[2]{},Turn=0,Winning=3000,Best[2]{},ClockTenths=0;Note Cue=Note::Begin;
        struct { int Value=1;bool Held=false,Selected=false,Rolling=false; } Dice[6];
    };
    struct NetApi;
    NetApi* mNet=nullptr;Role mRole=Role::Offline;
    bool mSeat1Remote=false,mTakeoverPending=false,mAutopilot=false,mFriendsTable=false,mInviteOnHost=false,mQuickFallback=false;
    bool mSeedReady=false;unsigned long long mHostSeed=0,mClientSeed=0;unsigned char mCommit[32]{};
    bool mPeerReady=false;unsigned mMatchId=0,mRollId=0,mCheckedRoll=0,mEpoch=0;int mCheckedFaces[6]{};float mTurnClock=0,mStatusTimer=0;
    Snapshot mLatest;std::string mLocalName="Player",mPeerName,mOnlineStatus;std::deque<std::string> mChat;
    bool mUiMic=false,mUiMute=false,mUiLocalTalk=false,mUiPeerTalk=false;int mUiClock=-1;
    bool Online() const { return mRole!=Role::Offline; }
    int LocalSeat() const { return mRole==Role::Client?1:0; }
    // Whose turn: the local player's own (not handed to the AI for running out of time), and
    // whether the AI is playing it - its own seat, or a timed-out player's.
    bool LocalTurn() const { return (mOpponent?1:0)==LocalSeat()&&!mAutopilot; }
    bool AiTurn() const { return (mOpponent&&!mSeat1Remote)||mAutopilot; }
    void StartOnline();void StopOnline();void UpdateOnline(float);void HandleNetEvent(const struct NetEvent&);void HandleMessage(const unsigned char*,int);
    void OnlineAction(int);void SendAction(int);void RemoteAction(int);void Sync();void ApplySnapshot(const Snapshot&);void ApplyFields(const Snapshot&);
    void BeginOnlineTurn();void NewEpoch();unsigned long long EpochSeed(unsigned) const;void GoOffline(const std::string&,bool leave=true);void Desync(const std::string&);
    void ChatLine(const std::string&);void SendChat();void RefreshOnline(bool force);void RefreshMenuStatus();void UpdatePresence();

    void Unlock(const char*);int Stat(const char*);int AddStat(const char*,int);std::string Setting(const char*) const;
    void AchBeginMatch();void AchBeginTurn();void AchDeselect(int);void AchRoll(bool scored,bool hotDice,int rolling);
    void AchRollResult(bool farkle);void AchScored(unsigned);void AchBank(int gain);void AchMatchEnd();
};
