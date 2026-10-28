@echo off
rem Builds and runs the Animations.dll tests against the Debug|x64 build in Binaries\
rem (build Animations.vcxproj first). FBX files given as arguments are run through the
rem importer too, e.g.  Build.cmd "%~dp0..\..\SDKs\fbx\samples\ViewScene\humanoid.fbx"
rem Output goes to %TEMP% so the source tree stays clean. Exit code is the test result.

setlocal
set HERE=%~dp0
set SRC=%HERE%..\..
set BIN=%SRC%\..\Binaries
set OUT=%TEMP%\PteroAnimationTests
if not exist "%OUT%" mkdir "%OUT%"

call "K:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cl /nologo /std:c++20 /EHsc /O2 /W4 /permissive- /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRC%\Animations" ^
   /Fo"%OUT%\\" /Fe"%OUT%\AnimationTests.exe" ^
   "%HERE%AnimationTests.cpp" ^
   /link "%BIN%\Animations.lib" || exit /b 1

rem Run from Binaries\ so the skinned-mesh test can load System.dll (and what it needs).
copy /Y "%OUT%\AnimationTests.exe" "%BIN%\AnimationTests.exe" >nul || exit /b 1
pushd "%BIN%"
"%BIN%\AnimationTests.exe" %*
set RESULT=%ERRORLEVEL%
del "%BIN%\AnimationTests.exe"
popd
exit /b %RESULT%
