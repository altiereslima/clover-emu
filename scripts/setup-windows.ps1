# Downloads the Windows build dependencies of clover-emu into third_party/ (not committed).
#   SDL2 (official Visual C++ development package)
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'

$sdlVer = '2.32.10'
$sdlDir = Join-Path $tp 'SDL2-win'
if (-not (Test-Path (Join-Path $sdlDir 'cmake\sdl2-config.cmake'))) {
    $zip = Join-Path $env:TEMP "SDL2-devel-$sdlVer-VC.zip"
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVer/SDL2-devel-$sdlVer-VC.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $tmp = Join-Path $env:TEMP "sdl2-extract-$PID"
    Expand-Archive -Path $zip -DestinationPath $tmp -Force
    if (Test-Path $sdlDir) { Remove-Item -Recurse -Force $sdlDir }
    Move-Item (Join-Path $tmp "SDL2-$sdlVer") $sdlDir
    Remove-Item -Recurse -Force $tmp
    Remove-Item -Force $zip
}
Write-Host "SDL2 $sdlVer ready in $sdlDir"

# Boost headers needed by dynarmic (icl, variant): the trimmed set Azahar uses.
$boostDir = Join-Path $tp 'boost'
$boostRev = '6a85c3100499e886e11c87a5c2109eedacea0a61'
if (-not (Test-Path (Join-Path $boostDir 'boost\icl\interval_set.hpp'))) {
    git clone --filter=blob:none https://github.com/azahar-emu/ext-boost.git $boostDir
    if ($LASTEXITCODE) { throw 'git clone of ext-boost failed' }
    git -C $boostDir checkout -q $boostRev
}
Write-Host "Boost headers ready in $boostDir"
