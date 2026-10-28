# Online multiplayer

Farkle plays online through Steam: Quick Match, open tables where another player can take
the AI's seat, Steam friend invites, text chat and voice chat. Ranked matches, leaderboards
and the store are not part of this yet.

## Where the code lives

| Part | Files | Does |
| --- | --- | --- |
| Network.dll | `Source/Network` (`NetworkAPI.h`, `SteamSession.*`, `VoiceChat.*`, `Network.cpp`) | Steam lobbies and Quick Match, the peer-to-peer link (ISteamNetworkingMessages over Steam Datagram Relay), invites and rich presence, lobby chat with Steam's text filter, voice (Steam's codec, played through XAudio2), SHA-256 and secure random numbers. Knows nothing about Farkle. |
| Game.dll | `Source/Game/GameOnline.cpp`, plus seat changes in `Game.cpp` / `Game.h` | What the messages mean: seats, the handshake, snapshots, actions, the turn timer, the HUD's online panel. |
| Host | `Renderer_DX12/DX12SceneRenderer.cpp`, `RmlUiRenderer.*` | Game API 11: `GetUiValue` (reads the chat box), Ui operation 6 (sets an input's value), the new button actions, Enter in an input with `data-submit`, and no single-key shortcuts while a text box has focus. |
| UI | `Data/UI/Farkle/online.rml`, `menu.rml`, `game.rml`, `game.rcss`, `menus.rcss`, `Localization/*.json` | The Play Online screen, the HUD's online panel, translations. |

Game.dll loads Network.dll with `LoadLibraryW` and resolves its exports, so a build without it
(or without Steam: editor play, a non-Steam build) simply has no online play. The Play Online
screen then says so. Build Game builds and ships Network.dll with the other engine modules.

Network.dll never starts Steam. The launcher does (`GameLauncher/SteamSupport.cpp`) and pumps
`SteamAPI_RunCallbacks` every frame, which also delivers Network.dll's callbacks. Like the
launcher, Network.dll delay-loads `steam_api64.dll` and only touches it once the launcher has
loaded it and the user is logged on.

## How a match works

- **Seats.** Seat 0 is the host, seat 1 the AI or the other player. The host runs every rule
  for both seats. The other player sends their clicks as actions; after every change the host
  sends a snapshot of the whole table (reliable, in order), and the other player draws it from
  their own side. Achievements are unchanged for the host; the joining player earns none online
  (their game runs no rules).
- **Open table (public).** Played against the AI. A stranger who joins takes the AI's seat at
  the next turn, with the AI's score. Quick Match lists only public tables with the seat free;
  if it finds none it opens one for you instead.
- **Friends table.** Hosted with Play with Friends (the Steam invite dialog opens). When a
  friend joins, a fresh match starts.
- **Fair dice.** On joining, the host commits to a secret seed (SHA-256), the joiner sends its
  own, then the host reveals. Both seed the dice from the two seeds; the joiner re-rolls every
  throw itself and leaves (carrying on against the AI) if a face differs.
- **Turn timer.** 30 seconds per decision (`FarkleConfig::OnlineTurnSeconds`), for both
  players. When it runs out the AI plays the rest of that turn.
- **Leaving.** Whoever stays carries on against the AI with the match as it stands. The host's
  public table opens its seat again.
- **Chat and voice.** Chat goes through the lobby, so Steam filters it for each player's own
  settings. The microphone starts on with Steam friends and off with strangers; the other
  player can always be muted.

## Testing

`Source/Tests/Online/Build.cmd` tests Farkle's side without Steam: two copies of the built
`Binaries/Game.dll` play each other in one process through a fake Network.dll
(`FakeNetwork.cpp`), with bots clicking for both players. It covers a full match and a
rematch, chat (with markup escaped), the turn timer, a host whose rolled faces are tampered
with, leaving mid-match, and Quick Match with no open tables. Build the Game project first;
the exit code is the number of failed checks.

It does not test Network.dll itself. Real online play needs two Steam accounts that own the game (or have keys), each running the built
game from Steam on its own PC. Steam does not run two clients on one PC. Steps:

1. Build Game, upload to a beta branch (or copy the build to both PCs with `steam_appid.txt`).
2. PC A: Play Online, Open Table. You play the AI and the HUD shows the online panel.
3. PC B: Play Online, Quick Match. B joins A's table; at the next turn B takes the AI's seat.
4. Try chat (Enter sends), Mic and Mute, letting the timer run out, and leaving on either side.
5. Play with Friends on A opens Steam's invite dialog; accepting on B starts a fresh match.

## Names in translated lines

Player names and chat lines are the players' own words, so the game wraps each in U+E000 ...
U+E001 (`Name()` in GameOnline.cpp). The UI translation (`Renderer_DX12/UiLocalization.cpp`)
cuts them out, looks the sentence up with `{name}` in their place, and puts them back, so a
translation can put the name wherever its grammar wants it:

    "{name} sat down at your table.": "{name} があなたのテーブルにつきました。"

A marked span is never translated itself: a chat line that happens to read "Cancel" stays
"Cancel". New lines with a name in them need a `{name}` key in each `Localization/*.json`.

## Friends list status

Under a player's name in the Steam friends list, friends see what they are doing ("Playing
online against Marta · 1,250 – 900"), in each friend's own language. `Game::UpdatePresence`
picks the line (a token such as `#Online`) and the score; Network.dll adds the opponent's
name, the Join link while a seat is free, and a player group so both players of a table are
listed together. The lines themselves live on Steamworks: upload
`Farkle_Steam/rich_presence/farkle_rich_presence.vdf` (see the README there). Until it is
uploaded, friends only see "Farkle: Tavern Dice".
