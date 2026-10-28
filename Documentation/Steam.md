# Steam

The packaged game uses the Steamworks SDK from `Source/SDKs/SteamWorkssdk`. Only the
GameLauncher (the exe that Build Game produces) links it; the editor never talks to Steam.

## Turning it on

In the editor, open **Windows > Game Settings**, enter the game's **Steam App ID** in the Steam
section, and Save. It is stored as `SteamAppId` in `Data/Game/GameSettings.json`. `0` means the
game is not a Steam build: the launcher then never loads `steam_api64.dll`, which it delay-loads,
and Build Game does not ship it.

With an App ID, **Release > Build Game** copies
`SteamWorkssdk\redistributable_bin\win64\steam_api64.dll` next to the exe, and the game, at
startup and before it opens a window (`Source/Ptero-Engine/GameLauncher/SteamSupport.cpp`):

1. Calls `SteamAPI_RestartAppIfNecessary`. Started outside Steam, the game exits and Steam
   launches it again through the client.
2. Calls `SteamAPI_InitEx`. If Steam is not running, the game shows an error and exits.
3. Checks `ISteamApps::BIsSubscribedApp` for the App ID. If the signed-in account does not own
   the game, it shows an error and exits.

While running, the launcher calls `SteamAPI_RunCallbacks` every frame (overlay, and anything
Game.dll registers later) and `SteamAPI_Shutdown` on exit.

### Testing outside Steam

Put a `steam_appid.txt` file holding just the App ID next to the built exe. Steam then skips the
relaunch in step 1, though it must still be running and the account must still own the app. Never
ship this file, because it turns off the relaunch check.

## Steam DRM wrapper

The DRM wrapper (SteamStub) is applied by Valve's tools to the finished exe, not compiled in. It
has to be applied again after every Build Game, because each build writes a new, unwrapped exe.

```bat
Tools\SteamDrmWrap.bat <app id> "<build folder>\<Game>.exe" <steamworks account> [flags]
```

The script runs `steamcmd +drm_wrap ... drmtoolp <flags>` with the SDK's
`tools\ContentBuilder\builder\steamcmd.exe` and replaces the exe with the wrapped one. steamcmd asks
for the password and Steam Guard code itself. Flags: `0` is normal (the default), `6` is
compatibility mode, `32` skips the debugger check, and `38` combines both.

You can also wrap the exe on the Steamworks site, under App Admin > Edit Steamworks Settings >
Security > DRM. Upload the exe there, download the wrapped one, and put it back in the build
folder before uploading the depot.

Valve's notes on the wrapper:
- It only stops casual copying. The real protection is the API check above, plus Steam features
  that need a licence.
- Antivirus tools sometimes flag wrapped exes.
- If the wrapped exe fails to start, try compatibility mode (`6`). The launcher exports the
  `PteroData_*` functions that the engine DLLs look up on the exe, so test a wrapped build before
  you ship it.
