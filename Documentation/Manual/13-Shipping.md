# 13. Shipping a game

## Game Settings

**Windows → Game Settings...** holds the project-wide setup, saved to
`Data/Game/GameSettings.json` (which ships inside the game):

| Setting | Meaning |
| --- | --- |
| Player Controller | **C++** (`Game.dll`, `FirstPersonCharacter`) or **Node Graph** with a `.nodegraph` file. See [11. Gameplay](11-Gameplay.md#the-player-character). |
| Export Node Graph Controller... | Writes a node-graph copy of the C++ controller to start a custom one from. |
| Startup Level | The level the built game opens. (The editor ignores it and plays whatever level is open.) |
| Steam App ID | 0 = not a Steam game. See [Steam](#steam). |

## Build Game

**Release → Build Game...** turns the project into a standalone game folder.

| Field | Meaning |
| --- | --- |
| Game Name | The `.exe`'s file name and the window title. |
| Icon | Any PNG, JPG or BMP; converted to the `.exe`'s icon. |
| Splash Screen | Shown centred from the moment the game launches until its first frame. PNG transparency is kept; large images are scaled to fit 1600 × 1000. |
| Output Folder | Where the game is written. |
| Data Folders to Package | Which `Data/` subfolders to include. Everything the game reads at runtime is ticked by default; the FMOD Studio project (authoring files) is not. |
| Compile Release DLLs automatically | The game uses optimised Release builds of the engine. With this on, they are compiled (with MSBuild) whenever they are missing or older than the editor's. This needs Visual Studio installed and can take several minutes the first time. |

Buttons:

- **Full Build**: the `.exe`, the engine DLLs and all the packages.
- **EXE Only**: just the executable and DLLs, when only code or settings changed.
- **Package Only**: just the data packages, when only content changed.

The log below the buttons reports every step, and says clearly whether the build succeeded.

### What a built game contains

| File | What it is |
| --- | --- |
| `<Game Name>.exe` | The launcher, with your name, icon, splash and packaging key built in |
| Engine DLLs | Renderer, audio, video, physics, game module and their third-party runtimes |
| `Content/*.ppak` | Your `Data/` folders, one archive per folder (Textures, Geometry, UI, ...) |
| `settings.cfg` | The player's options, created on first launch |

The game reads its content **straight out of the `.ppak` archives**; nothing is unpacked to disk.

### Packages and the packaging key

Each `.ppak` holds one `Data/` folder. Every file in it is compressed (LZ4) and encrypted
(libsodium) with your project's key, stored in `ProjectSettings/Packaging.key`. The key is
created once, the first time it is needed, and built into every game exe you ship, so the game
can open its own archives.

- **Keep the key private and backed up.** Anyone with it can extract your packages. Without it,
  you cannot rebuild packages that an already-shipped exe can read.
- Do not commit it to a public repository.

**Release → Extract Package...** unpacks a `.ppak` built with this project's key into a folder,
to check what shipped.

### Testing the build

Run the `.exe` in the output folder. Check:

- the startup level loads and plays;
- the UI, videos and sounds work (a missing folder in "Data Folders to Package" shows up here);
- the Logs folder beside the game for errors.

## The player's options

A shipped game can offer a settings menu built from RmlUi documents (the engine's example menus
are in `Data/UI/Farkle/settings-*.rml`). The player's choices are saved to `settings.cfg` in the
game folder (not in AppData), so the folder carries everything about the install.

Options include display mode and resolution, anti-aliasing, upscaler and its quality and
sharpness, FSR frame generation, GI method, quality and resolution, AO, shadows, reflections,
subsurface, fog, bloom, lens flares, chromatic aberration, model and texture quality, virtual
geometry, path tracing, statistics, and overall, music and sound volume.

**Quality tiers are relative to your level.** "High" is the level exactly as you tuned it.
"Low", "Medium" and "Ultra" scale the cost-heavy settings from there (ray counts, resolutions,
sample counts) without touching the look-defining ones (fog density and colour, GI intensity). So you author the look once, and the player's settings only trade quality for speed.

Settings the player's GPU cannot run (an upscaler it lacks, ray-traced GI without ray tracing)
are corrected automatically, and the menus show the corrected value. Ray-traced GI falls back to
voxel GI, which runs on any GPU.

Ray-traced AO and ray-traced shadows stay as the level set them. The player's **AO** Off
switches off whichever AO the level uses, ray-traced included. The player's **Shadows** Low
switches ray-traced shadows off (leaving the shadow maps), and Off switches every shadow off.
Without a ray-tracing GPU both quietly fall back: XeGTAO for the AO, the shadow maps for shadows.

## Steam

To ship on Steam, enter the game's **Steam App ID** in Game Settings. Then Build Game also ships
`steam_api64.dll`, and at startup the game:

1. relaunches itself through the Steam client if started outside Steam;
2. checks that Steam is running (otherwise shows an error and exits);
3. checks that the signed-in account owns the game (otherwise shows an error and exits).

**Testing outside Steam**: put a `steam_appid.txt` file containing just the App ID next to the
exe. Steam must still be running and the account must still own the app. **Never ship this
file.**

**DRM wrapping** (Valve's SteamStub) is applied to the finished exe, and has to be applied again
after every Build Game:

```bat
Tools\SteamDrmWrap.bat <app id> "<build folder>\<Game>.exe" <steamworks account> [flags]
```

Flags: 0 normal (default), 6 compatibility mode, 32 skip the debugger check, 38 both. If the
wrapped exe fails to start, try compatibility mode. You can also wrap the exe on the Steamworks
website (App Admin → Edit Steamworks Settings → Security → DRM). Test a wrapped build before
shipping it.
