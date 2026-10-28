# Steam rich presence

The line under a player's name in the Steam friends list ("Playing online against Marta · 1,250 – 900").

**Upload:** Steamworks > App Admin > Community > Rich Presence > Localization. Upload
`farkle_rich_presence.vdf` (all 16 languages), or the files in `languages/` one at a time,
then Publish.

The game (`Source/Game/GameOnline.cpp`, `Game::UpdatePresence`) sets `steam_display` to one of
these tokens; `%score%` and `%opponent%` are filled from the rich presence keys of the same
name. Change a token in both places.

| Token | English |
| --- | --- |
| `#Menu` | In the tavern |
| `#VsAI` | Playing against the AI · %score% |
| `#Searching` | Looking for a match |
| `#Waiting` | Waiting at an open table |
| `#WaitingFriends` | Waiting for friends to join |
| `#Online` | Playing online against %opponent% · %score% |
