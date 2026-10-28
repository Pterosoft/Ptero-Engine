# Pterosoft Launcher

A game sold on the Pterosoft store can require Pterosoft Launcher to start it. This uses the
launcher's online check at launch (launcher 0.8.1+; see `K:\Pterosoft Launcher\GAME-DRM.md`).

## Turning it on

In the editor, open **Windows > Game Settings**, enter the game's store product ID in the
**Pterosoft Launcher** section, and Save. It is stored as `PterosoftProductId` in
`Data/Game/GameSettings.json`; `0` means the game is not a Pterosoft Launcher build. Farkle is
product `90`. The ID ships inside the encrypted `Content\Game.ppak`, not as a loose file.

When the launcher starts an owned game it asks the store for a single-use ticket (valid for two
minutes) and passes it in the `PTEROSOFT_GAME_TICKET` environment variable. The packaged game,
before it opens a window (`Source/Ptero-Engine/GameLauncher/main.cpp`, after the Steam check),
sends the ticket to `https://pterolauncher.pterosoftstudio.com/wp-json/pterosoft/v1/games/verify`
using `PterosoftGameLicense.h` (a copy of the launcher's `GameSDK` header), and exits with a
message when:

- there is no ticket (the exe was started directly): "start this game from Pterosoft Launcher";
- the service cannot be reached: an internet connection is required;
- the ticket is expired or reused, or the account no longer owns the game.

The verification URL is compiled into the GameLauncher on purpose; it is never read from a file,
the command line or the environment. There is no offline grace period.

## Publishing

Rebuild the game with **Release > Build Game**, upload it as a release of the product, and set its
minimum launcher version to **0.8.1**. Older launchers do not pass a ticket, so the game would
refuse to start from them.

## Testing

Playing in the editor never checks. A built exe with a product ID, started directly, shows
"Please start this game from Pterosoft Launcher." To test the full path, install the build through
the launcher from an account that owns the product. Set the ID back to `0` for local builds that
should run without the launcher.

This is client-side licensing: it stops casual copying, not a determined patcher.
