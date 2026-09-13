$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$qt = Join-Path $root 'Source\SDKs\qt\install'
$output = Join-Path $root 'Cache\QtTests'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64 build tools are required.' }
if (!(Test-Path "$qt\lib\Qt6Widgetsd.lib")) { throw 'Build Qt Debug first using BuildQt.ps1.' }
New-Item -ItemType Directory -Force $output | Out-Null
$script = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
cd /d "$root"
set "PATH=$root\Binaries;$qt\bin;%PATH%"
set "QT_QPA_PLATFORM_PLUGIN_PATH=$qt\plugins\platforms"
cl /nologo /Zc:__cplusplus /std:c++20 /EHsc /MDd /D_DEBUG /DQT_NO_KEYWORDS /DNOMINMAX /I "$qt\include" Source\QtUi\QtUi.cpp Source\QtUi\QtUiSmoke.cpp /Fo"$output\\" /Fd"$output\QtUiSmoke.pdb" /Fe"$output\QtUiSmoke.exe" /link /LIBPATH:"$qt\lib" Qt6Cored.lib Qt6Guid.lib Qt6Widgetsd.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
"$output\QtUiSmoke.exe"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /Zc:__cplusplus /std:c++20 /EHsc /MDd /D_DEBUG /DQT_NO_KEYWORDS /DNOMINMAX /I "$qt\include" Source\QtUi\QtRendererSmoke.cpp /Fo"$output\\" /Fd"$output\QtRendererSmoke.pdb" /Fe"$output\QtRendererSmoke.exe" /link /LIBPATH:"$qt\lib" Qt6Cored.lib Qt6Guid.lib Qt6Widgetsd.lib user32.lib
if errorlevel 1 exit /b %errorlevel%
"$output\QtRendererSmoke.exe"
exit /b %errorlevel%
"@
$scriptPath = Join-Path $output 'run-tests.cmd'
Set-Content -LiteralPath $scriptPath -Value $script -Encoding ascii
& $env:ComSpec /d /c $scriptPath
if ($LASTEXITCODE) { throw "Qt smoke tests failed with exit code $LASTEXITCODE" }
