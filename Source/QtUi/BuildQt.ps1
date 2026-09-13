param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$source = Join-Path $root 'Source\SDKs\qt\qtbase'
$install = Join-Path $root 'Source\SDKs\qt\install'
$build = Join-Path $root "Cache\Qt-$Configuration"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64 build tools are required.' }
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
New-Item -ItemType Directory -Force $build | Out-Null
# Keep the source checkout untouched: the supplied root contains an obsolete cache.
$script = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
"$cmake" -S "$source" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=$Configuration -DCMAKE_INSTALL_PREFIX="$install" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DFEATURE_sql=OFF -DFEATURE_network=ON -DFEATURE_opengl=ON -DFEATURE_printsupport=OFF
if errorlevel 1 exit /b %errorlevel%
"$cmake" --build "$build" --parallel 8
if errorlevel 1 exit /b %errorlevel%
"$cmake" --install "$build"
exit /b %errorlevel%
"@
$scriptPath = Join-Path $build 'build-qt.cmd'
Set-Content -LiteralPath $scriptPath -Value $script -Encoding ascii
& $env:ComSpec /d /c $scriptPath
if ($LASTEXITCODE) { throw "Qt $Configuration build failed with exit code $LASTEXITCODE" }
