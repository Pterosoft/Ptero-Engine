@echo off
setlocal
rem Run from any directory. Requires MSVC and the engine's bundled RmlUi 6.3 SDK.
cd /d "%~dp0"
if not defined VSCMD_VER call "K:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "RML_SDK=..\..\..\..\Source\SDKs\RmlUI"
cl /nologo /std:c++17 /EHsc /O2 /MD /utf-8 /DRMLUI_STATIC_LIB /I"%RML_SDK%\Include" verify.cpp /Fe:verify.exe /link /LIBPATH:"%RML_SDK%\Bin-Static\Release" /LIBPATH:"%RML_SDK%\Dependencies\Bin-Static\lib" rmlui.lib freetype.lib user32.lib ole32.lib windowscodecs.lib
if errorlevel 1 exit /b 1
verify.exe ..\farkle.rml ..\Preview\farkle-1600x900.png 1600 900
if errorlevel 1 exit /b 1
verify.exe ..\farkle.rml ..\Preview\farkle-1280x720.png 1280 720
if errorlevel 1 exit /b 1
verify.exe ..\farkle.rml ..\Preview\farkle-dice.png 1600 900 dice
if errorlevel 1 exit /b 1
verify.exe ..\farkle.rml ..\Preview\farkle-hover.png 1600 900 hover
if errorlevel 1 exit /b 1
verify.exe ..\farkle.rml ..\Preview\farkle-dialog.png 1600 900 dialog
exit /b %errorlevel%
