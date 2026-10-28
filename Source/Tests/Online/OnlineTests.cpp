// Headless tests for Farkle's online play (Source/Game/GameOnline.cpp).
//
// Real online play needs Steam on two PCs. This runs both players in one process instead:
// two copies of the built Game.dll (GameA.dll is the host, GameB.dll the player who joins),
// each with a fake engine host that records what the game draws and feeds it clicks, and a
// fake Network.dll (FakeNetwork.cpp) that passes their messages across. Build.cmd builds and
// runs it; the exit code is the number of failed checks.

#include "../../Game/GameAPI.h"
#include "../../Network/NetworkAPI.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int gChecks = 0, gFailures = 0;
void Check(bool ok, const std::string& what)
{
    ++gChecks;
    if (!ok) { ++gFailures; std::printf("  FAIL: %s\n", what.c_str()); }
}

// What a fake engine host remembers for its game: the document, element text, classes and
// disabled state, and the clicks queued for the game to poll.
struct Host
{
    std::vector<std::string> Names{ "Table", "Dice1", "Dice2", "Dice3", "Dice4", "Dice5", "Dice6" };
    std::vector<GameTransform> Poses = std::vector<GameTransform>(7);
    std::string Document;
    std::map<std::string, std::string> Text, Values;
    std::map<std::string, std::set<std::string>> Classes;
    std::set<std::string> Disabled;
    std::deque<int> Actions;
    int WinningScore = 1000;

    bool Has(const std::string& id, const std::string& cls) { return Classes[id].count(cls) != 0; }
    bool Enabled(const std::string& id) { return !Disabled.count(id); }
};

GameServices MakeServices(Host& host)
{
    GameServices s{};
    s.User = &host;
    s.FindEntity = [](void* u, const char* name) -> int {
        auto& h = *static_cast<Host*>(u);
        for (size_t i = 0; i < h.Names.size(); ++i) if (h.Names[i] == name) return int(i);
        return -1;
    };
    s.GetTransform = [](void* u, int i, GameTransform* t) -> bool { *t = static_cast<Host*>(u)->Poses[i]; return true; };
    s.SetTransform = [](void* u, int i, const GameTransform* t) { static_cast<Host*>(u)->Poses[i] = *t; };
    s.LoadUi = [](void* u, const char* path) -> bool {
        auto& h = *static_cast<Host*>(u);
        h.Document = path; h.Text.clear(); h.Classes.clear(); h.Disabled.clear(); h.Values.clear();
        return true;
    };
    s.Ui = [](void* u, int op, const char* id, const char* value) {
        auto& h = *static_cast<Host*>(u);
        switch (op)
        {
        case 0: case 1:
        {
            // As the real UI does, drop the markers the game puts round names and chat text.
            std::string text = value;
            for (const char* marker : { "\xEE\x80\x80", "\xEE\x80\x81" })
                for (size_t at; (at = text.find(marker)) != std::string::npos;) text.erase(at, 3);
            h.Text[id] = text;
            break;
        }
        case 2: h.Classes[id].insert(value); break;
        case 3: h.Classes[id].erase(value); break;
        case 4: h.Disabled.erase(id); break;
        case 5: h.Disabled.insert(id); break;
        case 6: h.Values[id] = value; break;
        }
    };
    s.PollAction = [](void* u) -> int {
        auto& h = *static_cast<Host*>(u);
        if (h.Actions.empty()) return NoAction;
        const int a = h.Actions.front(); h.Actions.pop_front(); return a;
    };
    s.RequestStop = [](void*) {};
    s.ToggleFullscreen = [](void*) {};
    s.Standalone = true;
    s.GetWinningScore = [](void* u) -> int { return static_cast<Host*>(u)->WinningScore; };
    s.GetUiValue = [](void* u, const char* id, char* value, int capacity) -> bool {
        auto& h = *static_cast<Host*>(u);
        const auto found = h.Values.find(id);
        if (found == h.Values.end()) return false;
        strncpy_s(value, size_t(capacity), found->second.c_str(), _TRUNCATE);
        return true;
    };
    return s;
}

struct Player
{
    HMODULE Module = nullptr;
    GameStartFn Start = nullptr;
    GameUpdateFn Update = nullptr;
    GameStopFn Stop = nullptr;
    Host Ui;
    GameServices Services{};
    GameCameraState Camera{};
    // The bot: when it is this player's turn to choose, pick the best scoring dice once per
    // throw, then roll or bank. Idle skips its turns, to let the timer run out.
    bool Bot = true, Idle = false;
    std::string LastDecision;
    float Stall = 0;
};

using SetSideFn = void(__stdcall*)(int);
using VoidFn = void(__stdcall*)();
using GetStateFn = int(__stdcall*)(int);
using GetPresenceFn = const char*(__stdcall*)(int);
GetPresenceFn gPresence = nullptr;
SetSideFn gSetSide = nullptr;
VoidFn gCorrupt = nullptr, gReset = nullptr;
GetStateFn gNetState = nullptr;
Player gPlayers[2];
int gFaces[2][6]{};

bool Load(Player& p, const wchar_t* dll)
{
    p.Module = LoadLibraryW(dll);
    if (!p.Module) return false;
    p.Start = reinterpret_cast<GameStartFn>(GetProcAddress(p.Module, "Game_Start"));
    p.Update = reinterpret_cast<GameUpdateFn>(GetProcAddress(p.Module, "Game_Update"));
    p.Stop = reinterpret_cast<GameStopFn>(GetProcAddress(p.Module, "Game_Stop"));
    return p.Start && p.Update && p.Stop;
}

// Same rules as Game::Score: the points a set of dice is worth, 0 if any die in it scores nothing.
int Score(const int* faces, unsigned mask)
{
    int c[7]{}, total = 0;
    for (int i = 0; i < 6; ++i) if (mask & (1u << i)) { ++c[faces[i]]; ++total; }
    if (!total) return 0;
    if (total == 6) { int pairs = 0, singles = 0; for (int f = 1; f <= 6; ++f) { pairs += c[f] == 2; singles += c[f] == 1; } if (singles == 6) return 1500; if (pairs == 3) return 750; }
    int score = 0;
    for (int f = 1; f <= 6; ++f) { const int n = c[f]; if (n >= 3) score += (f == 1 ? 1000 : f * 100) * (1 << (n - 3)); else if (f == 1) score += n * 100; else if (f == 5) score += n * 50; else if (n) return 0; }
    return score;
}

int Number(const std::string& text);
void Think(Player& p, float dt)
{
    Host& ui = p.Ui;
    if (!p.Bot || ui.Document != "Farkle/game.rml" || ui.Text["turn-label"] != "Your Turn" || !ui.Enabled("clear-button")) return;
    if (p.Idle) return;
    int faces[6]; unsigned available = 0;
    for (int i = 0; i < 6; ++i)
    {
        const std::string id = "die-" + std::to_string(i + 1);
        faces[i] = 1; for (int f = 1; f <= 6; ++f) if (ui.Has(id, "face-" + std::to_string(f))) faces[i] = f;
        if (!ui.Has(id, "held")) available |= 1u << i;
    }
    const bool hasDice = ui.Has("play-area", "has-dice");
    // One decision per table state the host has sent; wait for the answer before the next.
    std::string state = ui.Text["round-score"] + "|" + ui.Text["turn-score"];
    for (int i = 0; i < 6; ++i) state += char('0' + faces[i]) + std::string(ui.Has("die-" + std::to_string(i + 1), "held") ? "h" : "") + (hasDice ? "d" : "");
    // A click can be lost (sent just as a table closes): like a player, try again after a while.
    p.Stall += dt;
    if (state == p.LastDecision && p.Stall < 2.0f) return;
    p.LastDecision = state; p.Stall = 0;
    if (!hasDice) { ui.Actions.push_back(Roll); return; }
    unsigned best = 0; int value = 0;
    for (unsigned m = available; m; m = (m - 1) & available) if (Score(faces, m) > value) { value = Score(faces, m); best = m; }
    if (!best) return;
    ui.Actions.push_back(Clear);
    int left = 0;
    for (int i = 0; i < 6; ++i) { if (best & (1u << i)) ui.Actions.push_back(SelectDie + i); else if (available & (1u << i)) ++left; }
    const int turn = Number(ui.Text["turn-score"]) + value;
    ui.Actions.push_back(turn >= 350 || left <= 2 ? Bank : Roll);
}

// One frame for both players, host first.
void Frame(float dt = 1.0f / 30.0f)
{
    for (int side = 0; side < 2; ++side)
    {
        Player& p = gPlayers[side];
        gSetSide(side);
        Think(p, dt);
        GameFrameContext frame{}; frame.DeltaSeconds = dt;
        p.Update(&frame, &p.Camera);
    }
}
bool RunUntil(const std::function<bool()>& done, float seconds)
{
    for (int i = 0; i < int(seconds * 30); ++i) { Frame(); if (done()) return true; }
    return false;
}
int Number(const std::string& text) { std::string digits; for (char c : text) if (c >= '0' && c <= '9') digits += c; return digits.empty() ? -1 : std::stoi(digits); }
bool InMatch(Player& p) { return p.Ui.Document == "Farkle/game.rml"; }
bool AtResult(Player& p) { return p.Ui.Document == "Farkle/victory.rml" || p.Ui.Document == "Farkle/defeat.rml"; }

// The full match plays to a short target; the other tests use a long one, so the match is
// still running when they act.
void StartBoth(int target = 5000)
{
    for (int side = 0; side < 2; ++side)
    {
        Player& p = gPlayers[side];
        p.Ui = Host{};
        p.Ui.WinningScore = target;
        p.Services = MakeServices(p.Ui);
        gSetSide(side);
        p.Stop();
        Check(p.Start(&p.Camera, &p.Services), "Game_Start succeeds for player " + std::to_string(side));
        p.Bot = true; p.Idle = false; p.LastDecision.clear();
    }
}

// The host opens a public table, the guest Quick Matches into it and takes the AI's seat.
void Join()
{
    Frame();
    Check(std::string(gPresence(0)) == "#Menu", "friends see the host in the tavern menu");
    gPlayers[0].Ui.Actions.push_back(HostPublic);
    Check(RunUntil([] { return InMatch(gPlayers[0]); }, 10), "host opens a table and starts a match against the AI");
    Check(!gPlayers[0].Ui.Text["table-status"].empty(), "host HUD fills the online panel");
    gPlayers[1].Ui.Actions.push_back(QuickMatch);
    Check(RunUntil([] { return InMatch(gPlayers[1]); }, 10), "guest Quick Matches into the host's table");
    const bool seated = RunUntil([] { return gPlayers[0].Ui.Text["opponent-name"] == "Guest"; }, 60);
    Check(seated, "guest takes the AI's seat at a turn boundary");
    if (!seated)
        for (int side = 0; side < 2; ++side)
        {
            Host& ui = gPlayers[side].Ui;
            std::printf("    [%s] doc=%s turn=%s opp=%s status=%s msg=%s\n    chat=%s\n", side ? "guest" : "host", ui.Document.c_str(), ui.Text["turn-label"].c_str(),
                ui.Text["opponent-name"].c_str(), ui.Text["table-status"].c_str(), ui.Text["turn-message"].c_str(), ui.Text["chat-log"].c_str());
        }
    Check(RunUntil([] { return gPlayers[1].Ui.Text["opponent-name"] == "Host"; }, 5), "guest sees the host as its opponent");
    Check(std::string(gPresence(0)) == "#Online" && std::string(gPresence(1)) == "#Online", "friends see both players playing online");
}

void TestFullMatch()
{
    std::printf("Full online match\n");
    gReset(); StartBoth(1000); Join();
    // Scores agree from both sides every frame: the host's "player" is the guest's "opponent".
    bool agreed = true, guestActed = false;
    const bool finished = RunUntil([&] {
        Host& a = gPlayers[0].Ui; Host& b = gPlayers[1].Ui;
        if (InMatch(gPlayers[0]) && InMatch(gPlayers[1]) && a.Text["turn-label"] == "Opponent's Turn" && b.Text["turn-label"] == "Your Turn") guestActed = true;
        return AtResult(gPlayers[0]) && AtResult(gPlayers[1]);
    }, 600);
    Check(finished, "both players reach the result screen");
    Check(guestActed, "the guest played its own turns");
    Check(gNetState(1) != 0, "the guest never left (no dice mismatch)");
    const bool hostWon = gPlayers[0].Ui.Document == "Farkle/victory.rml";
    Check(hostWon == (gPlayers[1].Ui.Document == "Farkle/defeat.rml"), "one player wins and the other loses");
    const int hostScore = Number(gPlayers[0].Ui.Text["result-player-score"]), guestSeen = Number(gPlayers[1].Ui.Text["result-opponent-score"]);
    const int guestScore = Number(gPlayers[1].Ui.Text["result-player-score"]), hostSeen = Number(gPlayers[0].Ui.Text["result-opponent-score"]);
    Check(hostScore == guestSeen && guestScore == hostSeen && hostScore >= 0, "final scores match from both sides ("
        + std::to_string(hostScore) + " / " + std::to_string(guestScore) + ")");
    Check((hostScore > guestScore) == hostWon, "the higher score wins");

    // A rematch asked for by the guest starts a fresh match for both.
    gPlayers[1].Ui.Actions.push_back(Rematch);
    Check(RunUntil([] { return InMatch(gPlayers[0]) && InMatch(gPlayers[1]); }, 10), "the guest's Rematch starts a new match for both");
    Check(Number(gPlayers[1].Ui.Text["player-score"]) == 0 && Number(gPlayers[1].Ui.Text["opponent-score"]) == 0, "the rematch starts from zero");
}

void TestChat()
{
    std::printf("Chat\n");
    gPlayers[0].Ui.Values["chat-input"] = "Good luck <b>friend</b>";
    gPlayers[0].Ui.Actions.push_back(ChatSend);
    Frame(); Frame();
    Check(gPlayers[1].Ui.Text["chat-log"].find("Good luck &lt;b&gt;friend&lt;/b&gt;") != std::string::npos, "the guest sees the host's line, with markup escaped");
    Check(gPlayers[0].Ui.Text["chat-log"].find("Good luck") != std::string::npos, "the host sees its own line");
    Check(gPlayers[0].Ui.Values["chat-input"].empty(), "the chat box is cleared after sending");
}

void TestTimer()
{
    std::printf("Turn timer\n");
    gReset(); StartBoth(); Join();
    gPlayers[1].Idle = true;
    Check(RunUntil([] { return gPlayers[1].Ui.Text["turn-label"] == "Your Turn" && !gPlayers[1].Ui.Text["turn-timer"].empty(); }, 60), "the guest's turn shows the timer");
    const int before = Number(gPlayers[0].Ui.Text["opponent-score"]);
    // The turn ends either way: the host plays next, or the AI's points end the match.
    Check(RunUntil([] { return gPlayers[1].Ui.Text["turn-label"] == "Opponent's Turn" || AtResult(gPlayers[1]); }, 60), "after the timer runs out the AI finishes the guest's turn");
    Check(!InMatch(gPlayers[0]) || Number(gPlayers[0].Ui.Text["opponent-score"]) >= before, "the guest's score is kept");
    gPlayers[1].Idle = false;
}

void TestCheatingHost()
{
    std::printf("Dice verification\n");
    gReset(); StartBoth(); Join();
    gCorrupt();
    Check(RunUntil([] { return gNetState(1) == 0; }, 30), "the guest leaves when a rolled face does not match the shared seed");
    Check(gPlayers[1].Ui.Text["dialog-title"] == "Online", "the guest is told why");
    Check(RunUntil([] { return gPlayers[0].Ui.Text["opponent-name"] == "Opponent"; }, 10), "the host's AI takes the seat back");
    // Both carry on offline.
    gPlayers[1].Ui.Actions.push_back(Close);
    const bool finished = RunUntil([] { return AtResult(gPlayers[0]) && AtResult(gPlayers[1]); }, 600);
    Check(finished, "both finish their matches against the AI");
    if (!finished)
        for (int side = 0; side < 2; ++side)
        {
            Host& ui = gPlayers[side].Ui;
            std::printf("    [%s] doc=%s turn=%s msg=%s dice=%d roll=%d bank=%d clear=%d scores=%s/%s dialog=%d\n", side ? "guest" : "host", ui.Document.c_str(),
                ui.Text["turn-label"].c_str(), ui.Text["turn-message"].c_str(), ui.Has("play-area", "has-dice"), ui.Enabled("roll-button"), ui.Enabled("bank-button"),
                ui.Enabled("clear-button"), ui.Text["player-score"].c_str(), ui.Text["opponent-score"].c_str(), !ui.Has("dialog", "hidden"));
        }
}

void TestLeave()
{
    std::printf("Leaving\n");
    gReset(); StartBoth(); Join();
    for (int i = 0; i < 90; ++i) Frame();
    const int guestScore = Number(gPlayers[1].Ui.Text["player-score"]), hostScore = Number(gPlayers[1].Ui.Text["opponent-score"]);
    gPlayers[1].Ui.Actions.push_back(LeaveTable);
    Frame(); Frame();
    Check(gNetState(1) == 0, "Leave Table leaves the lobby");
    Check(Number(gPlayers[1].Ui.Text["player-score"]) == guestScore && Number(gPlayers[1].Ui.Text["opponent-score"]) == hostScore,
        "the guest keeps its match, from its own side");
    // (Short test matches can also have ended by now, which replaces the HUD.)
    Check(RunUntil([] { return gPlayers[0].Ui.Text["opponent-name"] == "Opponent" || AtResult(gPlayers[0]); }, 10), "the host's AI takes the empty seat");
    gPlayers[1].Ui.Actions.push_back(Close);
    Check(RunUntil([] { return AtResult(gPlayers[0]) && AtResult(gPlayers[1]); }, 600), "both finish against the AI");
}

void TestQuickMatchFallback()
{
    std::printf("Quick Match with no tables\n");
    gReset(); StartBoth();
    gPlayers[1].Ui.Actions.push_back(QuickMatch);
    Check(RunUntil([] { return InMatch(gPlayers[1]); }, 10), "with no open tables, Quick Match opens one and plays the AI");
    Check(gNetState(1) == NetHosting, "the searcher now hosts the table");
    Check(std::string(gPresence(1)) == "#Waiting", "friends see the searcher waiting at an open table");
}
}

int main(int argc, char** argv)
{
    HMODULE fake = LoadLibraryW(L"Network.dll");
    gSetSide = fake ? reinterpret_cast<SetSideFn>(GetProcAddress(fake, "Fake_SetSide")) : nullptr;
    gCorrupt = fake ? reinterpret_cast<VoidFn>(GetProcAddress(fake, "Fake_CorruptNextRoll")) : nullptr;
    gReset = fake ? reinterpret_cast<VoidFn>(GetProcAddress(fake, "Fake_Reset")) : nullptr;
    gNetState = fake ? reinterpret_cast<GetStateFn>(GetProcAddress(fake, "Fake_GetState")) : nullptr;
    gPresence = fake ? reinterpret_cast<GetPresenceFn>(GetProcAddress(fake, "Fake_GetPresence")) : nullptr;
    if (!gSetSide || !gCorrupt || !gReset || !gNetState || !gPresence) { std::printf("The fake Network.dll is missing.\n"); return 1; }
    if (!Load(gPlayers[0], L"GameA.dll") || !Load(gPlayers[1], L"GameB.dll")) { std::printf("Could not load GameA.dll / GameB.dll.\n"); return 1; }

    TestFullMatch();
    TestChat();
    TestTimer();
    TestCheatingHost();
    TestLeave();
    TestQuickMatchFallback();

    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures;
}
