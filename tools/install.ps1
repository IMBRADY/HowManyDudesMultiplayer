# SPDX-FileCopyrightText: 2026 Braden Atzert
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Installs HowManyDudesMultiplayer.dll into the game's mods\Aurie\ folder.
#
# Driven by install.bat in the project root, which is what a player double
# clicks. Written in PowerShell rather than batch because Steam's default
# install path contains both spaces and parentheses, which batch handles badly.
#
# This script only ever ADDS files under the game's mods\ folder. It never
# writes to, renames, or deletes data.win, HowManyDudes.exe, any .bak, or
# anything else the game shipped with.
#
[CmdletBinding()]
param(
    # Explicit game folder. Detected automatically when omitted.
    [string] $GameDir,

    # Report what was found and change nothing.
    [switch] $Detect
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$modDll = Join-Path $projectRoot 'build\HowManyDudesMultiplayer.dll'

function Write-Ok      { param($m) Write-Host "  [ok] $m" -ForegroundColor Green }
function Write-Missing { param($m) Write-Host "  [--] $m" -ForegroundColor Yellow }
function Write-Bad     { param($m) Write-Host "  [X]  $m" -ForegroundColor Red }

# Returns the game folder inside a Steam library root, or $null.
function Test-Library {
    param([string] $LibraryRoot)

    if ([string]::IsNullOrWhiteSpace($LibraryRoot)) { return $null }

    $candidate = Join-Path $LibraryRoot 'steamapps\common\How Many Dudes'
    if (Test-Path (Join-Path $candidate 'HowManyDudes.exe')) { return $candidate }

    return $null
}

# Every Steam library root on this machine: the install itself, plus anything
# listed in libraryfolders.vdf, plus the two usual default locations.
function Get-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]

    foreach ($key in @(
        @{ Path = 'HKCU:\Software\Valve\Steam';                    Name = 'SteamPath' },
        @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam';        Name = 'InstallPath' }
    )) {
        try {
            $value = (Get-ItemProperty -Path $key.Path -Name $key.Name -ErrorAction Stop).($key.Name)
            if ($value) { $roots.Add($value.Replace('/', '\')) }
        }
        catch {
            # Key absent - Steam may be installed without it, so keep looking.
        }
    }

    foreach ($root in @($roots.ToArray())) {
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path $vdf)) { continue }

        # Lines look like:    "path"    "D:\\SteamLibrary"
        foreach ($match in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $roots.Add($match.Groups[1].Value.Replace('\\', '\'))
        }
    }

    $roots.Add('C:\Program Files (x86)\Steam')
    $roots.Add('C:\Program Files\Steam')

    return $roots | Select-Object -Unique
}

function Find-GameDir {
    foreach ($root in Get-SteamLibraries) {
        $found = Test-Library $root
        if ($found) { return $found }
    }
    return $null
}

Write-Host ''
Write-Host '  How Many Dudes? - multiplayer mod installer'
Write-Host '  -------------------------------------------'
Write-Host ''

if (-not (Test-Path $modDll)) {
    Write-Bad 'build\HowManyDudesMultiplayer.dll is missing.'
    Write-Host '       Run build.bat first.'
    exit 1
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = Find-GameDir
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    Write-Bad 'Could not find "How Many Dudes" automatically.'
    Write-Host ''
    Write-Host '       Re-run with the game folder, for example:'
    Write-Host '         install.bat "D:\SteamLibrary\steamapps\common\How Many Dudes"'
    Write-Host ''
    Write-Host '       In Steam: right-click the game, Manage, Browse local files.'
    exit 1
}

if (-not (Test-Path (Join-Path $GameDir 'HowManyDudes.exe'))) {
    Write-Bad "No HowManyDudes.exe in: $GameDir"
    Write-Host '       That does not look like the game folder.'
    exit 1
}

Write-Host "  Game folder:"
Write-Host "      $GameDir"
Write-Host ''

# --- Report on the mod framework -------------------------------------------
# The layout below is the one documented by YYToolkit's own install guide:
#
#   <game>\HowManyDudes.exe
#   <game>\mods\Native\AurieCore.dll      <- the framework
#   <game>\mods\Aurie\YYToolkit.dll       <- GameMaker support
#   <game>\mods\Aurie\HowManyDudesMultiplayer.dll   <- this mod
#
# Older Aurie generations shipped a loader or proxy DLL instead, so those are
# accepted too. A bare mods\ folder is deliberately NOT a marker, since this
# script creates one itself.
$aurieMarkers = @(
    'mods\Native\AurieCore.dll',
    'AurieCore.dll',
    'AurieLoader.exe',
    'AurieManager.exe',
    'dinput8.dll',
    'version.dll'
)
$aurieFound = $aurieMarkers | Where-Object { Test-Path (Join-Path $GameDir $_) }

$yytkMarkers = @(
    'mods\Aurie\YYToolkit.dll',
    'mods\Native\YYToolkit.dll',
    'YYToolkit.dll'
)
$yytkFound = $yytkMarkers | Where-Object { Test-Path (Join-Path $GameDir $_) }

# Aurie v2 works by adding a ".aurie" section to the game executable rather than
# by dropping a loader beside it, so the presence of AurieCore.dll does not by
# itself mean the game will load it. The section table lives near the start of
# the file, so a small read is enough. Read-only - this never writes to the exe.
function Test-ExecutablePatched {
    param([string] $ExePath)

    try {
        $stream = [System.IO.File]::OpenRead($ExePath)
        try {
            $buffer = New-Object byte[] 8192
            $read = $stream.Read($buffer, 0, $buffer.Length)
            $text = [System.Text.Encoding]::ASCII.GetString($buffer, 0, $read)
            return $text.Contains('.aurie')
        }
        finally { $stream.Dispose() }
    }
    catch {
        return $false
    }
}

$exePath = Join-Path $GameDir 'HowManyDudes.exe'
$exePatched = Test-ExecutablePatched $exePath

if ($aurieFound) { Write-Ok "Aurie Framework found ($($aurieFound[0]))." }
else             { Write-Missing 'Aurie Framework NOT found  (mods\Native\AurieCore.dll)' }

if ($yytkFound) { Write-Ok "YYToolkit found ($($yytkFound[0]))." }
else            { Write-Missing 'YYToolkit NOT found  (mods\Aurie\YYToolkit.dll)' }

if ($exePatched) { Write-Ok 'Game executable is patched for Aurie.' }
else             { Write-Missing 'Game executable is NOT patched for Aurie.' }

Write-Host ''

if ($Detect) {
    Write-Host '  -Detect: nothing was changed.'
    Write-Host ''
    exit 0
}

# --- Install ----------------------------------------------------------------
# Both folders are created even though only one receives a file, because the
# player needs somewhere obvious to drop AurieCore.dll and YYToolkit.dll.
$targetDir = Join-Path $GameDir 'mods\Aurie'
$nativeDir = Join-Path $GameDir 'mods\Native'
$target = Join-Path $targetDir 'HowManyDudesMultiplayer.dll'

try {
    foreach ($dir in @($targetDir, $nativeDir)) {
        if (-not (Test-Path $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
    }
    Copy-Item -Path $modDll -Destination $target -Force
}
catch {
    Write-Bad "Could not install into $targetDir"
    Write-Host "       $($_.Exception.Message)"
    Write-Host '       Close the game if it is running, then try again.'
    exit 1
}

Write-Ok 'Installed HowManyDudesMultiplayer.dll'
Write-Host ''

if ($aurieFound -and $yytkFound -and $exePatched) {
    Write-Host '  Done. Launch the game - the mod writes its own settings file on'
    Write-Host '  the first run, so there is nothing else to configure.'
    Write-Host ''
    Write-Host '  To play: one player presses F9 to host, the other presses F10.'
    Write-Host '  On a local network F10 finds the host by itself - no addresses'
    Write-Host '  to swap. Over the internet, set peer_address in'
    Write-Host '  mods\Aurie\HowManyDudesMultiplayer.ini to the host''s address.'
    Write-Host ''
    Write-Host '  F8 status  |  F11 disconnect'
    Write-Host ''
    exit 0
}

Write-Host '  The mod is installed, but the game cannot load it yet. Finish the'
Write-Host '  framework setup below, then run "install.bat /detect" to confirm.'
Write-Host ''
Write-Host '  The folders are already created for you.'
Write-Host ''

if (-not $aurieFound) {
    Write-Host '  1. AurieCore.dll' -ForegroundColor Cyan
    Write-Host '     From: https://github.com/AurieFramework/Aurie/releases/latest'
    Write-Host '     Take the plain "AurieCore.dll" - NOT AurieCore-x86.dll, and'
    Write-Host '     not any .pdb file. Put it in:'
    Write-Host "       $nativeDir"
    Write-Host ''
}

if (-not $yytkFound) {
    Write-Host '  2. YYToolkit.dll' -ForegroundColor Cyan
    Write-Host '     From: https://github.com/AurieFramework/YYToolkit/releases/tag/v5.0.0c'
    Write-Host '     Use v5.0.0c. v4.0.1 crashes this game on startup - it cannot'
    Write-Host '     read this build''s GameMaker runtime. Take the plain'
    Write-Host '     "YYToolkit.dll", not -x86, not .pdb. Put it in:'
    Write-Host "       $targetDir"
    Write-Host ''
}

if (-not $exePatched) {
    Write-Host '  3. AuriePatcher.exe' -ForegroundColor Cyan
    Write-Host '     From the same Aurie release as AurieCore.dll. It only needs'
    Write-Host '     to run once, and it edits the game executable so Aurie loads.'
    Write-Host '     Save it anywhere, then run this exact command:'
    Write-Host ''
    Write-Host "       AuriePatcher.exe `"$exePath`" `"$nativeDir\AurieCore.dll`" install" -ForegroundColor White
    Write-Host ''
    Write-Host '     Steam verifying the game files will undo this. If the mod'
    Write-Host '     stops loading after an update, run the command again.'
    Write-Host ''
}

Write-Host '  Prefer a wizard? AurieInstaller.exe does all three steps, but it is'
Write-Host '  only in the v2.0.0b release (not the latest) and needs the .NET'
Write-Host '  Desktop Runtime 10 x64:'
Write-Host '    https://github.com/AurieFramework/Aurie/releases/tag/v2.0.0b'
Write-Host ''
exit 0
