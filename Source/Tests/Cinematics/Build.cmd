@echo off
rem Builds and runs the cinematic runtime tests. No engine, no Qt: just the System
rem runtime sources compiled into one console exe. Output goes to %TEMP% so the source
rem tree stays clean. Exit code is the test result.

setlocal
set HERE=%~dp0
set SRC=%HERE%..\..
set OUT=%TEMP%\PteroCinematicTests
if not exist "%OUT%" mkdir "%OUT%"

call "K:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /std:c++20 /EHsc /O2 /W4 /permissive- /utf-8 /DNOMINMAX ^
   /I"%SRC%\System\include" /I"%SRC%\SDKs" /I"%SRC%\Editor\Sequencer" ^
   /Fo"%OUT%\\" /Fe"%OUT%\CinematicTests.exe" ^
   "%HERE%CinematicTests.cpp" ^
   "%SRC%\System\src\Guid.cpp" ^
   "%SRC%\System\src\PropertyRegistry.cpp" ^
   "%SRC%\System\src\Cinematics\CinematicCurve.cpp" ^
   "%SRC%\System\src\Cinematics\SequenceTrack.cpp" ^
   "%SRC%\System\src\Cinematics\CinematicSequence.cpp" ^
   "%SRC%\System\src\Cinematics\SequencePlayer.cpp" ^
   "%SRC%\Editor\Sequencer\SequencerDocument.cpp" ^
   "%SRC%\System\src\DataFiles.cpp" ^
   "%SRC%\System\src\PteroLog.cpp" ^
   /link dbghelp.lib user32.lib || exit /b 1

"%OUT%\CinematicTests.exe"
