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
// Farkle's Steam achievements. The API names match the Steamworks app (5368450); the stats
// they use as progress bars are STAT_MATCHES_PLAYED, STAT_BEST_WIN_STREAK, STAT_POINTS_BANKED
// and STAT_COMBOS_SCORED. STAT_DICE_ROLLED is kept up to date for later achievements.
// The host keeps all of it for the session only when Steam is not running (editor play).
namespace {
// Bits of Game::mAchCombos, in the order the Collector achievement counts them.
enum Combo:unsigned{SingleOne=1,SingleFive=2,ThreeKind=4,FourKind=8,FiveKind=16,SixKind=32,ThreePairs=64,Straight=128,AllCombos=255};
bool Is(const std::string& value,std::initializer_list<const char*> options){for(const char* o:options)if(value==o)return true;return false;}
const char* const kQualitySettings[]={"texture-quality","model-quality","gi-quality","ao-quality","fog-quality","shadow-quality"};
}
void Game::Unlock(const char* name){if(mHost.UnlockAchievement)mHost.UnlockAchievement(mHost.User,name);}
int Game::Stat(const char* name){int v=0;if(mHost.GetStat)mHost.GetStat(mHost.User,name,&v);return v;}
int Game::AddStat(const char* name,int delta){int v=Stat(name)+delta;if(mHost.SetStat)mHost.SetStat(mHost.User,name,v);return v;}
std::string Game::Setting(const char* key) const{char v[64]{};return mHost.GetSetting&&mHost.GetSetting(mHost.User,key,v,int(sizeof(v)))?v:"";}

void Game::AchBeginMatch(){mAchFarkled=mAchRolledFew=false;mAchPathTraced=true;mAchTurns=mAchRolls=mAchMaxDeficit=0;}
void Game::AchBeginTurn(){
    mAchMaxDeficit=std::max(mAchMaxDeficit,mOpponentScore-mPlayerScore);
    if(mOpponent)return;
    ++mAchTurns;mAchHotDice=mAchScoredDice=0;mAchOnlyFives=true;mAchOneDie=false;mAchDeselects.fill(0);
    if(Setting("path-tracing")=="off")mAchPathTraced=false;
}
void Game::AchDeselect(int die){if(!mOpponent&&++mAchDeselects[die]==10)Unlock("ACH_INDECISIVE");}
// Called as the player throws: `scored` when the selection was banked into the turn first
// (mTurn already includes it), `hotDice` when that used up all six, `rolling` dice thrown.
void Game::AchRoll(bool scored,bool hotDice,int rolling){
    if(mOpponent)return;
    // Banking would have ended the match in the player's favour: they are replying to the
    // opponent's final score and already ahead of it.
    if(scored&&mFinalPlayer==1&&mPlayerScore+mTurn>mOpponentScore)Unlock("ACH_COULDNT_RESIST");
    if(hotDice){Unlock("ACH_HOT_DICE");if(++mAchHotDice==2)Unlock("ACH_HOT_DICE_TWICE");}
    ++mAchRolls;if(rolling<3)mAchRolledFew=true;mAchOneDie=rolling==1;
    AddStat("STAT_DICE_ROLLED",rolling);
}
void Game::AchRollResult(bool farkle){
    if(mOpponent)return;
    if(farkle){
        mAchFarkled=true;Unlock("ACH_FIRST_FARKLE");
        if(++mAchFarkleStreak==3)Unlock("ACH_THREE_FARKLES");
        if(mAchRolls==1&&mAchTurns==1)Unlock("ACH_OPENING_FARKLE");
        if(mTurn>=1000)Unlock("ACH_SO_CLOSE");
        // Banking what was at risk would have won outright. Reaching the target only starts
        // the final round, so that is only ever the reply to the opponent's final score.
        if(mFinalPlayer==1&&mPlayerScore+mTurn>mOpponentScore)Unlock("ACH_HEARTBREAKER");
        return;
    }
    if(mAchOneDie)Unlock("ACH_ONE_DIE_LEFT");
    // A full throw of six: the straight and six 1s count as they land.
    int c[7]{},thrown=0;for(const auto& d:mDice)if(!d.Held){++c[d.Value];++thrown;}
    if(thrown==6){if(std::all_of(c+1,c+7,[](int n){return n==1;})){Unlock("ACH_STRAIGHT_SHOOTER");mAchCombos|=Straight;}if(c[1]==6)Unlock("ACH_SIX_ONES");}
}
// A selection of the player's dice that scored, whether thrown on or banked.
void Game::AchScored(unsigned mask){
    if(mOpponent||!mask)return;
    int c[7]{},total=0;for(int i=0;i<6;++i)if(mask&(1u<<i)){++c[mDice[i].Value];++total;}
    mAchScoredDice+=total;if(c[5]!=total)mAchOnlyFives=false;
    if(total==6){int pairs=0,singles=0;for(int f=1;f<=6;++f){pairs+=c[f]==2;singles+=c[f]==1;}
        if(singles==6){Unlock("ACH_STRAIGHT_SHOOTER");mAchCombos|=Straight;return;}
        if(pairs==3){Unlock("ACH_PAIR_UP");mAchCombos|=ThreePairs;return;}}
    for(int f=1;f<=6;++f){const int n=c[f];
        if(n==1||n==2)mAchCombos|=f==1?SingleOne:f==5?SingleFive:0u;
        if(n==3)mAchCombos|=ThreeKind;
        if(n==4){mAchCombos|=FourKind;Unlock("ACH_FOUR_OF_A_KIND");}
        if(n==5){mAchCombos|=FiveKind;Unlock("ACH_FIVE_OF_A_KIND");}
        if(n==6){mAchCombos|=SixKind;Unlock("ACH_SIX_OF_A_KIND");}}
    if(c[1]>=3)Unlock("ACH_THREE_ONES");
}
void Game::AchBank(int gain){
    if(mOpponent)return;
    mAchFarkleStreak=0;
    if(gain>=2000)Unlock("ACH_BANK_2000");if(gain>=3000)Unlock("ACH_BANK_3000");if(gain==50)Unlock("ACH_FIFTY_AND_DONE");
    if(mAchOnlyFives&&mAchScoredDice>0)Unlock("ACH_LUCKY_FIVES");
    if(AddStat("STAT_POINTS_BANKED",gain)>=100000)Unlock("ACH_BANKER");
}
void Game::AchMatchEnd(){
    const int played=AddStat("STAT_MATCHES_PLAYED",1);
    if(played>=10)Unlock("ACH_MATCHES_10");if(played>=50)Unlock("ACH_MATCHES_50");if(played>=100)Unlock("ACH_MATCHES_100");
    if(Setting("path-tracing")=="off")mAchPathTraced=false;
    if(mAchPathTraced)Unlock("ACH_BOUNCE_HOUSE");

    // Collector: the types scored this run, plus those earlier runs proved by unlocking.
    unsigned combos=mAchCombos;
    const std::pair<const char*,unsigned> proven[]={{"ACH_FOUR_OF_A_KIND",FourKind},{"ACH_FIVE_OF_A_KIND",FiveKind},{"ACH_SIX_OF_A_KIND",SixKind},{"ACH_PAIR_UP",ThreePairs},{"ACH_STRAIGHT_SHOOTER",Straight}};
    if(mHost.IsAchievementUnlocked)for(const auto& p:proven)if(mHost.IsAchievementUnlocked(mHost.User,p.first))combos|=p.second;
    const int kinds=std::popcount(combos);
    if(kinds>Stat("STAT_COMBOS_SCORED")&&mHost.SetStat)mHost.SetStat(mHost.User,"STAT_COMBOS_SCORED",kinds);
    if(combos==AllCombos)Unlock("ACH_COLLECTOR");

    if(!mWon){mAchWinStreak=0;if(mPlayerScore==0)Unlock("ACH_ROCK_BOTTOM");return;}
    Unlock("ACH_FIRST_VICTORY");
    if(!mAchFarkled)Unlock("ACH_FLAWLESS");
    if(mOpponentScore==0)Unlock("ACH_SHUTOUT");
    if(mAchMaxDeficit>=1500)Unlock("ACH_COMEBACK");
    if(mPlayerScore-mOpponentScore<=100)Unlock("ACH_NAIL_BITER");
    // mFinalPlayer is whoever reached the target first: the player, holding on through the
    // opponent's reply, or the opponent, overtaken in the player's reply.
    if(mFinalPlayer==0)Unlock("ACH_HELD_THE_LINE");else if(mFinalPlayer==1)Unlock("ACH_LAST_DITCH");
    if(!mAchRolledFew)Unlock("ACH_PLAYED_IT_SAFE");
    if(mAchTurns<=5)Unlock("ACH_SPEED_RUN");
    if(mWinningScore>=10000)Unlock("ACH_MARATHON");
    if(mWinningScore<=1000)Unlock("ACH_QUICK_DRAW");
    if(mWinningScore!=FarkleConfig::WinningScore)Unlock("ACH_HOUSE_RULES");

    ++mAchWinStreak;
    if(mAchWinStreak>Stat("STAT_BEST_WIN_STREAK")&&mHost.SetStat)mHost.SetStat(mHost.User,"STAT_BEST_WIN_STREAK",mAchWinStreak);
    if(mAchWinStreak>=3)Unlock("ACH_STREAK_3");if(mAchWinStreak>=5)Unlock("ACH_STREAK_5");if(mAchWinStreak>=10)Unlock("ACH_STREAK_10");

    // Settings the match was won with, as the Settings menu applied them.
    const std::string pathTracing=Setting("path-tracing"),upscaler=Setting("upscaler");
    if(pathTracing=="realtime")Unlock("ACH_FOLLOW_THE_LIGHT");
    bool potato=true,ultra=true;for(const char* key:kQualitySettings){const std::string v=Setting(key);potato=potato&&Is(v,{"low","off"});ultra=ultra&&v=="ultra";}
    if(potato&&pathTracing=="off")Unlock("ACH_POTATO_MODE");
    if(ultra&&(pathTracing=="realtime"||pathTracing=="reference"))Unlock("ACH_MAXED_OUT");
    if(upscaler=="none")Unlock("ACH_PURIST");
    if(upscaler=="aar")Unlock("ACH_IN_HOUSE");
}
