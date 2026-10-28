#include "pch.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "Game.h"
#include "FarkleConfig.h"
#include <algorithm>
#include <bit>
#include <cmath>
namespace {
std::string Number(int n){auto s=std::to_string(n);for(int i=int(s.size())-3;i>0;i-=3)s.insert(i,",");return s;}
std::string Pips(int face){std::string s="<span class=\"die face-"+std::to_string(face)+"\">";for(int i=1;i<=7;++i)s+="<span class=\"pip p"+std::to_string(i)+"\"></span>";return s+"</span>";}
float Ease(float t){return t*t*t*(t*(t*6-15)+10);}
}
Game& Game::Get(){static Game instance;return instance;}
void Game::Ui(int op,const char* id,const char* value){mHost.Ui(mHost.User,op,id,value);}
void Game::Text(const char* id,const std::string& value){Ui(0,id,value.c_str());}
void Game::Load(const char* name){if(!mHost.LoadUi(mHost.User,name))mHost.RequestStop(mHost.User);}
void Game::Log(const std::string& s){mLog.push_back(s);if(mLog.size()>6)mLog.pop_front();}
void Game::Sound(const char* name){if(mHost.PlaySound)mHost.PlaySound(mHost.User,name);}
// Takes the music channel over for a sting. mTrack goes back to "nothing to avoid"
// so the playlist is free to pick any track once the sting has finished.
void Game::Music(const char* name){if(!mMusic)return;mTrack=-1;if(!mHost.PlayMusic(mHost.User,name))mMusic=false;}
void Game::UpdateMusic(){
    if(!mMusic||mHost.IsMusicPlaying(mHost.User))return;
    // Draw from the tracks other than the one that just ended, so nothing repeats back
    // to back: pick out of the remaining N-1 and skip past the excluded index.
    std::uniform_int_distribution<int> pick(0,FarkleConfig::MusicTracks-(mTrack<0?1:2));
    int next=pick(mRandom);if(mTrack>=0&&next>=mTrack)++next;
    if(mHost.PlayMusic(mHost.User,("Music"+std::to_string(next+1)).c_str()))mTrack=next;else mMusic=false;
}
// Intersect the table camera's view ray with the plane the dice rest on: that point is
// the middle of the screen, under the "Roll the dice" prompt, whatever the camera is set to.
void Game::AimThrow(){
    const auto& c=FarkleConfig::TableCamera;const float cp=std::cos(c.Pitch);
    const float fx=std::sin(c.Yaw)*cp,fy=std::cos(c.Yaw)*cp,fz=std::sin(c.Pitch);
    // Where a resting die's centre is: on the table top, half a die up. Not the dice's own
    // authored height - their rest pose is wherever they wait off the table.
    float scale=0;for(const auto& d:mDice)scale+=d.Home.Scale[2];scale/=float(mDice.size());
    const float plane=mTableTop+FarkleConfig::DieMeshHalfHeight*scale;
    const float t=std::abs(fz)>1e-4f?(plane-c.PositionZ)/fz:0.f;
    mThrow[0]=c.PositionX+fx*t;mThrow[1]=c.PositionY+fy*t;mThrow[2]=plane;
    // Screen axes on the table plane: up is where the camera is leaning, and right is a
    // quarter turn clockwise from it.
    const float len=std::sqrt(fx*fx+fy*fy);
    if(len>1e-4f){
        const float ux=fx/len,uy=fy/len,rx=-uy,ry=ux;
        mThrow[0]+=rx*FarkleConfig::ThrowOffsetRight+ux*FarkleConfig::ThrowOffsetUp;
        mThrow[1]+=ry*FarkleConfig::ThrowOffsetRight+uy*FarkleConfig::ThrowOffsetUp;
    }
}
// Slot i of the landing grid, keeping the die's authored scale. 3 across, 2 deep.
GameTransform Game::Landing(int i) const{
    auto pose=mDice[i].Home;const int col=i%3,row=i/3;
    pose.Position[0]=mThrow[0]+(float(row)-0.5f)*FarkleConfig::DiceSpacing;
    pose.Position[1]=mThrow[1]+(float(col)-1.f)*FarkleConfig::DiceSpacing;
    pose.Position[2]=mThrow[2];return pose;
}
// Drop the dice far under the table for the result screens. The host only exposes
// transforms, so this is how an entity goes away.
void Game::HideDice(){for(auto& d:mDice){auto pose=d.Home;pose.Position[2]-=100.f;mHost.SetTransform(mHost.User,d.Entity,&pose);}}
bool Game::Start(const GameCameraState&,const GameServices& host){
    mHost=host;
    if(!host.FindEntity||!host.GetTransform||!host.SetTransform||!host.LoadUi||!host.Ui||!host.PollAction||!host.RequestStop||!host.ToggleFullscreen)return false;
    const int table=host.FindEntity(host.User,"Table");GameTransform tablePose{};
    if(table<0||!host.GetTransform(host.User,table,&tablePose))return false;
    mTableTop=tablePose.Position[2]+FarkleConfig::TableMeshTop*tablePose.Scale[2];
    for(int i=0;i<6;++i){auto& d=mDice[i];d=Die{};d.Entity=host.FindEntity(host.User,("Dice"+std::to_string(i+1)).c_str());
        if(d.Entity<0)d.Entity=host.FindEntity(host.User,("Dice "+std::to_string(i+1)).c_str());
        if(d.Entity<0||!host.GetTransform(host.User,d.Entity,&d.Home))return false;}
    // Music is optional: a host without the audio callbacks simply plays the match dry.
    mMusic=host.PlayMusic&&host.IsMusicPlaying;mTrack=-1;
    AimThrow();
    mRunning=true;mCamera=FarkleConfig::ResultCamera;UpdateMusic();mPhase=Phase::Menu;HideDice();Load(MenuUi());StartOnline();return true;
}
void Game::Stop(){StopOnline();if(mRunning)for(auto& d:mDice)mHost.SetTransform(mHost.User,d.Entity,&d.Home);mRunning=false;}
unsigned Game::Available() const{unsigned m=0;for(int i=0;i<6;++i)if(!mDice[i].Held)m|=1u<<i;return m;}
unsigned Game::Selection() const{unsigned m=0;for(int i=0;i<6;++i)if(mDice[i].Selected&&!mDice[i].Held)m|=1u<<i;return m;}
int Game::Score(unsigned mask) const{
    int c[7]{},total=0;for(int i=0;i<6;++i)if(mask&(1u<<i)){++c[mDice[i].Value];++total;}
    if(!total)return 0;
    if(total==6){int pairs=0,singles=0;for(int f=1;f<=6;++f){pairs+=c[f]==2;singles+=c[f]==1;}if(singles==6)return 1500;if(pairs==3)return 750;}
    int score=0;for(int f=1;f<=6;++f){int n=c[f];if(n>=3)score+=(f==1?FarkleConfig::ThreeOnes:f*100)*(1<<(n-3));else if(f==1)score+=n*100;else if(f==5)score+=n*50;else if(n)return 0;}return score;
}
unsigned Game::BestSelection() const{unsigned best=0,a=Available();int value=0;for(unsigned m=a;m;m=(m-1)&a)if(Score(m)>value){best=m;value=Score(m);}return best;}
void Game::BeginMatch(){
    mWinningScore=mHost.GetWinningScore?std::clamp(mHost.GetWinningScore(mHost.User),1,9999999):FarkleConfig::WinningScore;
    mPlayerScore=mOpponentScore=mTurn=0;mBestRoll={};mFinalPlayer=-1;mOpponent=mPaused=mWon=mAutopilot=false;mLog.clear();AchBeginMatch();++mMatchId;
    // Someone waiting for the AI's seat between matches gets it from the first turn.
    if(mTakeoverPending){mTakeoverPending=false;mSeat1Remote=true;}if(mSeat1Remote)NewEpoch();
    if(mCamera.PositionX!=FarkleConfig::TableCamera.PositionX)Travel(false);else{Load(HudUi());BeginTurn();}
}
void Game::BeginTurn(){mTurn=0;mHasRoll=false;mTimer=0;mAutopilot=false;for(auto& d:mDice)d.Held=d.Selected=d.Rolling=false;HideDice();BeginOnlineTurn();AchBeginTurn();mPhase=AiTurn()?Phase::Opponent:Phase::Choosing;mNote=Note::Begin;Refresh();}
void Game::RollDice(){
    bool scored=false,hotDice=false;
    if(mHasRoll){unsigned selected=Selection();int value=Score(selected);if(!value){mNote=Note::SelectFirst;Refresh();return;}mTurn+=value;scored=true;AchScored(selected);
        for(int i=0;i<6;++i)if(selected&(1u<<i)){mDice[i].Held=true;mDice[i].Selected=false;}if(!Available()){hotDice=true;for(auto& d:mDice)d.Held=false;}}
    AchRoll(scored,hotDice,std::popcount(Available()));
    std::uniform_int_distribution<int> face(1,6);std::uniform_real_distribution<float> spin(0.85f,1.35f);
    // Always thrown from the authored rest position off the near edge, never from wherever
    // the die happens to be - between rolls they are parked out of sight, not on the table.
    // Faces come from mDiceRandom in die order, which is what lets the other player check them.
    for(auto& d:mDice){d.Rolling=!d.Held;d.Selected=false;if(d.Rolling){d.From=d.Home;d.Value=face(mDiceRandom);d.Spin=spin(mRandom);}}
    ++mRollId;mHasRoll=true;mTimer=0;mPhase=Phase::Rolling;mNote=Note::Rolling;Sound("DiceRoll");Refresh();
}
void Game::FinishRoll(){
    unsigned best=BestSelection();AchRollResult(!best);if(!best){Log(std::string(mOpponent?"Opponent":"You")+" farkled — turn lost");mNote=Note::Farkle;mPhase=Phase::Delay;mTimer=0;Refresh();return;}
    int points=Score(best),seat=mOpponent?1:0;mBestRoll[seat]=std::max(mBestRoll[seat],points);
    Log(std::string(mOpponent?"Opponent":"You")+" rolled a scoring combination");mNote=Note::Choose;
    if(AiTurn())for(int i=0;i<6;++i)mDice[i].Selected=(best&(1u<<i))!=0;mPhase=AiTurn()?Phase::Opponent:Phase::Choosing;mTimer=0;mTurnClock=FarkleConfig::OnlineTurnSeconds;Refresh();
}
void Game::EndTurn(bool bank){
    int& total=mOpponent?mOpponentScore:mPlayerScore;if(bank){int gain=mTurn+Score(Selection());AchScored(Selection());AchBank(gain);total+=gain;Log(std::string(mOpponent?"Opponent banked ":"You banked ")+Number(gain));}
    int current=mOpponent?1:0;
    if(mFinalPlayer>=0&&current!=mFinalPlayer){if(mPlayerScore!=mOpponentScore){FinishMatch();return;}mFinalPlayer=current;}
    else if(mFinalPlayer<0&&total>=mWinningScore){mFinalPlayer=current;Log("Final round — one reply turn remains");}
    mOpponent=!mOpponent;BeginTurn();
}
void Game::Travel(bool result){mCameraFrom=mCamera;mCameraTo=result?FarkleConfig::ResultCamera:FarkleConfig::TableCamera;mCameraTime=0;mPhase=Phase::Travelling;mHasRoll=false;Load("Farkle/transition.rml");Sync();}
void Game::FinishMatch(){mWon=mPlayerScore>mOpponentScore;AchMatchEnd();HideDice();Travel(true);}
void Game::Animate(float dt){
    if(mPhase==Phase::Travelling){
        mCameraTime+=dt;float t=Ease(std::min(1.f,mCameraTime/FarkleConfig::CameraSeconds));auto mix=[t](float a,float b){return a+(b-a)*t;};
        mCamera.PositionX=mix(mCameraFrom.PositionX,mCameraTo.PositionX);mCamera.PositionY=mix(mCameraFrom.PositionY,mCameraTo.PositionY);mCamera.PositionZ=mix(mCameraFrom.PositionZ,mCameraTo.PositionZ);mCamera.Pitch=mix(mCameraFrom.Pitch,mCameraTo.Pitch);mCamera.Yaw=mCameraFrom.Yaw+std::remainder(mCameraTo.Yaw-mCameraFrom.Yaw,6.2831853f)*t;
        if(mCameraTime>=FarkleConfig::CameraSeconds){mCamera=mCameraTo;if(mCameraTo.PositionX==FarkleConfig::TableCamera.PositionX){Load(HudUi());if(mRole==Role::Client)ApplyFields(mLatest);else BeginTurn();}else{mPhase=Phase::Result;Music(mWon?"Victory":"Defeat");Load(mWon?"Farkle/victory.rml":"Farkle/defeat.rml");Refresh();}}
    }else if(mPhase==Phase::Rolling){
        float t=std::min(1.f,mTimer/FarkleConfig::RollSeconds);
        for(int i=0;i<6;++i)if(mDice[i].Rolling){auto& d=mDice[i];auto pose=d.From;const auto land=Landing(i);
            // Gather backwards, throw across to the landing slot in an arc, then settle
            // with two small bounces. The wind-up runs back along the throw, whichever
            // way the die happens to be travelling.
            float gather=std::min(t/0.16f,1.f),flight=std::clamp((t-0.16f)/0.60f,0.f,1.f),settle=std::clamp((t-0.76f)/0.24f,0.f,1.f);
            float travel=Ease(flight);
            float dx=land.Position[0]-d.From.Position[0],dy=land.Position[1]-d.From.Position[1];
            float len=std::sqrt(dx*dx+dy*dy),ux=len>1e-4f?dx/len:-1.f,uy=len>1e-4f?dy/len:0.f;
            float pull=FarkleConfig::ThrowDistance*Ease(gather)*(1-travel);
            pose.Position[0]=d.From.Position[0]+dx*travel-ux*pull;
            pose.Position[1]=d.From.Position[1]+dy*travel-uy*pull+0.12f*std::sin(float(i)*2.1f)*std::sin(3.14159265f*flight);
            pose.Position[2]=d.From.Position[2]+(land.Position[2]-d.From.Position[2])*travel
                +FarkleConfig::ThrowHeight*std::sin(3.14159265f*flight)*(0.9f+i*0.04f)+FarkleConfig::BounceHeight*std::abs(std::sin(6.2831853f*settle))*(1-settle);
            if(t>=1)for(int a=0;a<3;++a)pose.Position[a]=land.Position[a];
            float blend=Ease(std::clamp((t-0.16f)/0.84f,0.f,1.f));
            for(int a=0;a<3;++a){float target=FarkleConfig::FaceRotations[d.Value-1][a];float turns=(a==0?2.f:1.f)*6.2831853f;pose.Rotation[a]=d.From.Rotation[a]*(1-blend)+(target+turns)*blend+0.18f*d.Spin*std::sin(12.56637f*settle)*(1-settle);if(t>=1)pose.Rotation[a]=target;}
            mHost.SetTransform(mHost.User,d.Entity,&pose);
        }if(t>=1){for(auto& d:mDice)d.Rolling=false;HideDice();if(mRole==Role::Client)ApplyFields(mLatest);else FinishRoll();}
    }
}
void Game::Action(int a){
    if(a==Fullscreen){mHost.ToggleFullscreen(mHost.User);return;}if(a==ExitGame){mHost.RequestStop(mHost.User);return;}
    if(a==MainMenu||a==Continue){if(mPhase!=Phase::Result&&!mPaused&&mPhase!=Phase::Menu)return;if(Online())GoOffline("");mPaused=false;mPhase=Phase::Menu;mCamera=FarkleConfig::ResultCamera;HideDice();Load(MenuUi());return;}
    if(a>=QuickMatch){OnlineAction(a);return;}
    // The host starts every match; the other player's Rematch asks for one.
    if(a==StartMatch||a==Rematch){if(mRole==Role::Client){if(a==Rematch&&mPhase==Phase::Result)SendAction(a);return;}if(mPhase==Phase::Menu||mPhase==Phase::Result)BeginMatch();return;}
    if(a==Pause||a==Help){if(mPhase==Phase::Menu||mPhase==Phase::Result||mPhase==Phase::Travelling)return;mPaused=!mPaused;Text("dialog-title",a==Help?"How to Play":"Paused");Text("dialog-text",a==Help?"Select scoring dice, then bank or roll again. A Farkle loses the turn. Three ones score 1,000. Reach "+Number(mWinningScore)+" and survive the final reply.":"Resume to return to the table. F11 switches fullscreen.");Ui(mPaused?3:2,"dialog","hidden");return;}
    if(a==Close){mPaused=false;Ui(2,"dialog","hidden");return;}
    if(mPaused||mPhase!=Phase::Choosing||!LocalTurn())return;
    if(mRole==Role::Client){SendAction(a);return;}
    PlayAction(a);
}
// A move by whoever's turn it is: the local player, or (on the host) the other player's action.
void Game::PlayAction(int a){
    if(a>=SelectDie&&a<SelectDie+6&&mHasRoll){auto& d=mDice[a-SelectDie];if(!d.Held){d.Selected=!d.Selected;if(!d.Selected)AchDeselect(a-SelectDie);}}
    else if(a>=RemoveDie&&a<RemoveDie+6){auto& d=mDice[a-RemoveDie];if(d.Selected)AchDeselect(a-RemoveDie);d.Selected=false;}
    else if(a==Clear){for(auto& d:mDice)d.Selected=false;}
    else if(a==Roll)RollDice();else if(a==Bank&&mHasRoll&&Score(Selection())>0)EndTurn(true);Refresh();
}
std::string Game::Message() const{
    const bool mine=(mOpponent?1:0)==LocalSeat();
    switch(mNote){
    case Note::Begin:return mine?(mAutopilot?"Out of time: the AI plays your turn":"Roll the dice to begin"):"Opponent is thinking…";
    case Note::SelectFirst:return mine?"Select a scoring combination first":"Opponent is choosing…";
    case Note::Rolling:return "Rolling…";case Note::Farkle:return "Farkle!";
    default:return mine?(mAutopilot?"Out of time: the AI plays your turn":"Select scoring dice, then Roll or Bank"):"Opponent is choosing…";}
}
// Drawn from the local player's side: online, the joining player sits in seat 1.
void Game::Refresh(){
    const int me=LocalSeat(),mine=me?mOpponentScore:mPlayerScore,theirs=me?mPlayerScore:mOpponentScore;
    if(mPhase==Phase::Result){Text("result-player-score",Number(mine));Text("result-opponent-score",Number(theirs));Text("result-best-roll",mBestRoll[me]?Number(mBestRoll[me])+" points":"—");Text("result-banked-total",Number(mine));Text("result-bonus-label","Match Target");Text("result-bonus",Number(mWinningScore));Text("result-missed-chance","Opponent finished ahead");Sync();return;}
    if(mPhase==Phase::Travelling||mPhase==Phase::Menu){Sync();return;}
    const std::string message=Message();
    Text("target-score",Number(mWinningScore));Text("player-score",Number(mine));Text("opponent-score",Number(theirs));Text("turn-score",Number(mTurn));Text("banked-score",Number(mine));Text("round-score",Number(mTurn+Score(Selection())));Text("turn-label",(mOpponent?1:0)==me?"Your Turn":"Opponent's Turn");Text("status-text",message);Text("turn-message",message);
    Ui(mHasRoll&&mPhase!=Phase::Rolling&&mPhase!=Phase::Delay?2:3,"play-area","has-dice");bool active=LocalTurn()&&mPhase==Phase::Choosing;
    Ui(active&&(!mHasRoll||Score(Selection())>0)?4:5,"roll-button");Ui(active&&mHasRoll&&Score(Selection())>0?4:5,"bank-button");Ui(active?4:5,"clear-button");
    for(int i=0;i<6;++i){std::string id="die-"+std::to_string(i+1),slot="slot-"+std::to_string(i+1);for(int f=1;f<=6;++f)Ui(f==mDice[i].Value?2:3,id.c_str(),("face-"+std::to_string(f)).c_str());Ui(mDice[i].Selected?2:3,id.c_str(),"selected");Ui(mDice[i].Held?2:3,id.c_str(),"held");Ui(active&&!mDice[i].Held?4:5,id.c_str());Ui(1,slot.c_str(),mDice[i].Selected||mDice[i].Held?Pips(mDice[i].Value).c_str():"+");Ui(active&&mDice[i].Selected?4:5,slot.c_str());}
    std::string log;for(const auto& s:mLog)log+="<div class=\"log-row\">"+s+"</div>";Ui(1,"game-log",log.c_str());
    RefreshOnline(true);Sync();
}
void Game::Update(const GameFrameContext& frame,GameCameraState& camera){
    if(!mRunning)return;for(int n=0;n<32;++n){int a=mHost.PollAction(mHost.User);if(!a)break;Action(a);}
    UpdateOnline(std::clamp(frame.DeltaSeconds,0.f,0.1f));
    UpdateMusic(); // Outside the pause check: the soundtrack keeps running while paused.
    // Online the table never pauses: the dialog only covers it, while the other player plays on.
    if(!mPaused||Online()){float dt=std::clamp(frame.DeltaSeconds,0.f,0.1f);mTimer+=dt;Animate(dt);
        // The joining player only draws what the host sends: the host runs every rule.
        if(mRole!=Role::Client){if(mPhase==Phase::Delay&&mTimer>1.4f)EndTurn(false);else if(mPhase==Phase::Opponent&&mTimer>0.9f)PlayAi();}}
    camera=mCamera;
}
// The AI's move for whichever seat it is playing: its own, or a player's who ran out of time.
void Game::PlayAi(){
    if(!mHasRoll){RollDice();return;}
    const int seat=mOpponent?1:0,own=seat?mOpponentScore:mPlayerScore,other=seat?mPlayerScore:mOpponentScore;
    int gain=mTurn+Score(Selection()),remaining=0;for(auto& d:mDice)remaining+=!d.Held&&!d.Selected;
    // Replying to the other seat's final score: banking short of it would lose outright.
    bool mustCatch=mFinalPlayer==1-seat&&own+gain<=other;
    if(!mustCatch&&(gain>=400||remaining<=2||own+gain>=mWinningScore))EndTurn(true);else RollDice();
}
extern "C" {
std::uint32_t __stdcall Game_GetApiVersion(){return GameApiVersion;}
const char* __stdcall Game_GetName(){return "Farkle";}
bool __stdcall Game_Start(const GameCameraState* c,const GameServices* s){try{return s&&Game::Get().Start(c?*c:GameCameraState{},*s);}catch(...){return false;}}
void __stdcall Game_Update(const GameFrameContext* f,GameCameraState* c){if(f&&c)Game::Get().Update(*f,*c);}
void __stdcall Game_Stop(){Game::Get().Stop();}
}
