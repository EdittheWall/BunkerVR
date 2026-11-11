# install_dev.ps1: installs a BunkerVR build from this repository into a copy of Amnesia: The Bunker.
#
#   powershell -ExecutionPolicy Bypass -File tools\install_dev.ps1 -Game "C:\...\Amnesia The Bunker"
#   powershell -ExecutionPolicy Bypass -File tools\install_dev.ps1 -Game "..." -Build out\build\dev
#   powershell -ExecutionPolicy Bypass -File tools\install_dev.ps1 -Game "..." -Uninstall
#
# Installing: copies version.dll and BunkerVR_watch.exe from the build folder, copies the mod's own game files from
# game\, and applies every patch in patches\ to the game's files. The game's originals are kept in
# <game>\BunkerVR\dev_originals\ the first time, and patches are always applied to those originals, so installing
# again over an older build works. A game file that is neither the version a patch was made for nor the mod's
# result stops the install before anything is changed.
#
# -Uninstall puts the game's originals back and removes the mod's files. Steam's "Verify integrity of game files"
# does the same for the game's files.
#
# For players, BunkerVR Setup (see the releases) does all of this with a window and a rollback.

param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Build,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $PSScriptRoot 'BunkerVRPatch.cs')

$Game = (Resolve-Path -LiteralPath $Game).Path
if (-not (Test-Path -LiteralPath (Join-Path $Game 'AmnesiaTheBunker.exe'))) { throw "AmnesiaTheBunker.exe is not in $Game" }
if (Get-Process -Name 'AmnesiaTheBunker', 'AmnesiaTheBunker_NoSteam' -ErrorAction SilentlyContinue) { throw 'The game is running: close it first.' }

$originals = Join-Path $Game 'BunkerVR\dev_originals'
$patches = @(Get-ChildItem -LiteralPath (Join-Path $repo 'patches') -Recurse -File -Filter '*.bvrpatch')
$gameFiles = @(Get-ChildItem -LiteralPath (Join-Path $repo 'game') -Recurse -File)

function GamePath([string]$relative) { Join-Path $Game ($relative -replace '/', '\') }
function RepoRelative([IO.FileInfo]$file, [string]$under) { $file.FullName.Substring((Join-Path $repo $under).Length + 1) }

if ($Uninstall) {
    $restored = 0
    if (Test-Path -LiteralPath $originals) {
        foreach ($f in (Get-ChildItem -LiteralPath $originals -Recurse -File)) {
            $rel = $f.FullName.Substring($originals.Length + 1)
            Copy-Item -LiteralPath $f.FullName -Destination (GamePath $rel) -Force
            $restored++
        }
    }
    $removed = 0
    foreach ($f in $gameFiles) {
        $target = GamePath (RepoRelative $f 'game')
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force; $removed++ }
    }
    foreach ($name in 'version.dll', 'BunkerVR_watch.exe') {
        $target = Join-Path $Game $name
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force; $removed++ }
    }
    if (Test-Path -LiteralPath $originals) { Remove-Item -LiteralPath $originals -Recurse -Force }
    Write-Output "Uninstalled: $restored of the game's files put back, $removed of the mod's files removed."
    exit 0
}

if (-not $Build) {
    foreach ($candidate in 'out\build\player', 'out\build\dev') {
        if (Test-Path -LiteralPath (Join-Path $repo "$candidate\version.dll")) { $Build = Join-Path $repo $candidate; break }
    }
    if (-not $Build) { throw 'No build found in out\build\player or out\build\dev: build first, or pass -Build.' }
}
$Build = (Resolve-Path -LiteralPath $Build).Path
foreach ($name in 'version.dll', 'BunkerVR_watch.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $Build $name))) { throw "$name is not in $Build" }
}

# Work out every patched file first; nothing is written until all of them are known to apply.
$results = @()
foreach ($p in $patches) {
    $patch = [IO.File]::ReadAllBytes($p.FullName)
    $info = [BunkerVRPatch]::ReadInfo($patch)
    $target = GamePath $info.File
    $kept = Join-Path $originals ($info.File -replace '/', '\')
    $source = $null
    $nowHash = if (Test-Path -LiteralPath $target) { [BunkerVRPatch]::Sha256Hex([IO.File]::ReadAllBytes($target)) } else { '' }
    if ($nowHash -eq $info.ToSha256) { continue }  # already this build's
    if (Test-Path -LiteralPath $kept) { $source = $kept }
    elseif ($nowHash -eq $info.FromSha256) { $source = $target }
    if (-not $source) {
        throw "$($info.File) is not the version of the game's file BunkerVR was made for. If Steam updated the game, BunkerVR needs an update too. Nothing was changed."
    }
    $original = [IO.File]::ReadAllBytes($source)
    $results += [pscustomobject]@{ Info = $info; Target = $target; Kept = $kept; Original = $original; Result = [BunkerVRPatch]::Apply($original, $patch) }
}

foreach ($r in $results) {
    if (-not (Test-Path -LiteralPath $r.Kept)) {
        New-Item -ItemType Directory -Force (Split-Path $r.Kept) | Out-Null
        [IO.File]::WriteAllBytes($r.Kept, $r.Original)
    }
    [IO.File]::WriteAllBytes($r.Target, $r.Result)
}
foreach ($f in $gameFiles) {
    $target = GamePath (RepoRelative $f 'game')
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $target -Force
}
foreach ($name in 'version.dll', 'BunkerVR_watch.exe') { Copy-Item -LiteralPath (Join-Path $Build $name) -Destination (Join-Path $Game $name) -Force }

Write-Output ("Installed from {0}: {1} patched, {2} already up to date, {3} of the mod's own files." -f $Build, $results.Count, ($patches.Count - $results.Count), $gameFiles.Count)
