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
    Phase mPhase=Phase::Menu;GameCameraState mCamera{},mCameraFrom{},mCameraTo{};
    bool mRunning=false,mOpponent=false,mPaused=false,mHasRoll=false,mWon=false;
    int mPlayerScore=0,mOpponentScore=0,mTurn=0,mFinalPlayer=-1,mBestRoll=0;
    int mWinningScore=3000;
    std::string mBestRollName="—",mMessage;float mTimer=0,mCameraTime=0;
    // Background playlist: one Music1..MusicN track at a time, reshuffled when it ends.
    // mTrack is the one currently on the music channel, kept only so the next draw can
    // avoid repeating it. Cleared when a Victory/Defeat sting takes the channel over.
    bool mMusic=false;int mTrack=-1;
    // Where a throw lands, derived from the camera rather than authored, so nudging the
    // view keeps the dice under the on-screen prompt instead of drifting off it.
    float mThrow[3]{};
    int Score(unsigned) const;unsigned Available() const;unsigned Selection() const;unsigned BestSelection() const;
    void Action(int);void BeginMatch();void BeginTurn();void RollDice();void FinishRoll();void EndTurn(bool);void FinishMatch();void Travel(bool);void Animate(float);void Refresh();
    void Log(const std::string&);void Text(const char*,const std::string&);void Ui(int,const char*,const char* = "");void Load(const char*);
    void Sound(const char*);void Music(const char*);void UpdateMusic();
    // The editor tests with the original HUD (with its log) and menu; players get the new ones.
    const char* MenuUi() const { return mHost.Standalone?"Farkle/menu.rml":"Farkle/menu-editor.rml"; }
    const char* HudUi() const { return mHost.Standalone?"Farkle/game.rml":"Farkle/farkle.rml"; }
    void AimThrow();GameTransform Landing(int) const;void HideDice();
};
