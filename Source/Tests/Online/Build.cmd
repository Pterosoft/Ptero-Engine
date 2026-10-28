@echo off
rem Builds and runs the online play tests against the built Game.dll (Binaries\Game.dll,
rem so build the Game project first). Two copies of it play each other through a fake
rem Network.dll. Output goes to %TEMP% so the source tree stays clean. Exit code is the
rem number of failed checks.

setlocal
set HERE=%~dp0
set SRC=%HERE%..\..
set OUT=%TEMP%\PteroOnlineTests
if not exist "%OUT%" mkdir "%OUT%"

call "K:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem Separate copies, so each player has its own statics.
copy /y "%SRC%\..\Binaries\Game.dll" "%OUT%\GameA.dll" >nul || exit /b 1
copy /y "%SRC%\..\Binaries\Game.dll" "%OUT%\GameB.dll" >nul || exit /b 1

cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /DNOMINMAX /DNETWORK_EXPORTS /LD ^
   /Fo"%OUT%\\" /Fe"%OUT%\Network.dll" "%HERE%FakeNetwork.cpp" /link bcrypt.lib || exit /b 1

cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /DNOMINMAX ^
   /Fo"%OUT%\\" /Fe"%OUT%\OnlineTests.exe" "%HERE%OnlineTests.cpp" || exit /b 1

"%OUT%\OnlineTests.exe"
