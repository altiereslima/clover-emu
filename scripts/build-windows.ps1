# Builds clover-emu on Windows with MSVC (Visual Studio 2022 or Build Tools, "Desktop development with C++").
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 [-Config Release|RelWithDebInfo|Debug]
param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# dependencies (SDL2)
& (Join-Path $PSScriptRoot 'setup-windows.ps1')

# Visual Studio x64 developer environment
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio / Build Tools not found (vswhere.exe missing)' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 toolset was found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
& "$env:SystemRoot\System32\cmd.exe" /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

# CMake and Ninja ship with Visual Studio
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }
$ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }

$build = Join-Path $root 'build'
& $cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "`nBuilt: $build\Console Classics.exe"
