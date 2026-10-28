@echo off
rem Applies the Steam DRM wrapper to a game exe made by the editor's Release > Build Game.
rem Run it after every Build Game, since each build writes a fresh, unwrapped exe.
rem
rem   SteamDrmWrap.bat <app id> <path to game exe> <steamworks account> [flags]
rem
rem   flags: 0 = normal (default), 6 = compatibility mode, 32 = skip debugger check, 38 = both
rem
rem steamcmd asks for the account's password and Steam Guard code itself the first time,
rem then caches the login. The account needs the app's "Edit App Metadata" permission.
rem See Documentation/Steam.md.

setlocal
if "%~3"=="" (
    echo Usage: %~nx0 ^<app id^> ^<path to game exe^> ^<steamworks account^> [flags]
    exit /b 1
)

set "APP_ID=%~1"
set "GAME_EXE=%~f2"
set "ACCOUNT=%~3"
set "FLAGS=%~4"
if "%FLAGS%"=="" set "FLAGS=0"

set "STEAMCMD=%~dp0..\Source\SDKs\SteamWorkssdk\tools\ContentBuilder\builder\steamcmd.exe"
if not exist "%STEAMCMD%" (
    echo steamcmd.exe not found at %STEAMCMD%
    exit /b 1
)
if not exist "%GAME_EXE%" (
    echo Game exe not found: %GAME_EXE%
    exit /b 1
)

set "WRAPPED_EXE=%GAME_EXE%.wrapped"
if exist "%WRAPPED_EXE%" del "%WRAPPED_EXE%"

"%STEAMCMD%" +login "%ACCOUNT%" +drm_wrap %APP_ID% "%GAME_EXE%" "%WRAPPED_EXE%" drmtoolp %FLAGS% +quit
if not exist "%WRAPPED_EXE%" (
    echo DRM wrap failed; %GAME_EXE% was left unwrapped.
    exit /b 1
)

move /y "%WRAPPED_EXE%" "%GAME_EXE%" >nul
echo Wrapped %GAME_EXE%
exit /b 0
