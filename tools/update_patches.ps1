# update_patches.ps1: turns your edits to the scripts in a game folder back into this repository's files.
#
#   powershell -ExecutionPolicy Bypass -File tools\update_patches.ps1 -Game "C:\...\Amnesia The Bunker"
#   powershell -ExecutionPolicy Bypass -File tools\update_patches.ps1 -Game "..." -Add script\player\Player_Jump.hps
#
# Works on a game folder that tools\install_dev.ps1 installed into: the game's untouched files are kept in
# <game>\BunkerVR\dev_originals. For every one of them, the file now in the game folder is compared with the
# original and written to patches\<path>.bvrpatch (the patch is applied back to the original and must give the
# edited file byte for byte, or nothing is written). The mod's own files (everything under game\) are copied back
# from the game folder.
#
# -Add <path> starts patching a game file the mod has not changed so far: run it before you edit that file, so
# its untouched original can be kept.
#
# Needs git (for its diff).

param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string[]]$Add
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Add-Type -Path (Join-Path $PSScriptRoot 'BunkerVRPatch.cs')
$Game = (Resolve-Path -LiteralPath $Game).Path
$originals = Join-Path $Game 'BunkerVR\dev_originals'
if (-not (Test-Path -LiteralPath (Join-Path $Game 'AmnesiaTheBunker.exe'))) { throw "AmnesiaTheBunker.exe is not in $Game" }
if (-not (Test-Path -LiteralPath $originals)) { throw "No originals kept in ${originals}: install with tools\install_dev.ps1 first." }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is needed (for its diff).' }

foreach ($a in $Add) {
    $rel = $a.TrimStart('\', '/').Replace('/', '\')
    $kept = Join-Path $originals $rel
    if (Test-Path -LiteralPath $kept) { Write-Output "Already patched: $rel"; continue }
    $file = Join-Path $Game $rel
    if (-not (Test-Path -LiteralPath $file)) { throw "No such game file: $rel" }
    New-Item -ItemType Directory -Force (Split-Path $kept) | Out-Null
    Copy-Item -LiteralPath $file -Destination $kept
    Write-Output "Kept the original of $rel. Edit it in the game folder, then run this again without -Add."
}

$written = 0; $same = 0; $dropped = 0
foreach ($o in (Get-ChildItem -LiteralPath $originals -Recurse -File)) {
    $rel = $o.FullName.Substring($originals.Length + 1)
    $path = $rel.Replace('\', '/')
    $now = Join-Path $Game $rel
    $patchFile = Join-Path $repo ('patches\' + $rel + '.bvrpatch')
    $original = [IO.File]::ReadAllBytes($o.FullName)
    $result = [IO.File]::ReadAllBytes($now)
    if ([Linq.Enumerable]::SequenceEqual([byte[]]$original, [byte[]]$result)) {
        if (Test-Path -LiteralPath $patchFile) { Remove-Item -LiteralPath $patchFile; $dropped++; Write-Output "No changes left: removed the patch for $path" }
        continue
    }
    $lines = [BunkerVRPatch]::Lines($result)
    $hunks = New-Object 'System.Collections.Generic.List[BunkerVRPatch+Hunk]'
    $cursor = 0
    $diff = & git -c core.autocrlf=false diff --no-index -U0 --no-color --diff-algorithm=histogram -- $o.FullName $now
    foreach ($h in ($diff | Where-Object { $_ -like '@@ *' })) {
        if ($h -notmatch '^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@') { throw "Unreadable diff for ${path}: $h" }
        $a1 = [int]$Matches[1]; $b = if ($null -ne $Matches[2]) { [int]$Matches[2] } else { 1 }
        $c = [int]$Matches[3]; $d = if ($null -ne $Matches[4]) { [int]$Matches[4] } else { 1 }
        $hunk = New-Object 'BunkerVRPatch+Hunk'
        if ($b -gt 0) { $hunk.Keep = ($a1 - 1) - $cursor; $hunk.Drop = $b; $cursor = $a1 - 1 + $b }
        else { $hunk.Keep = $a1 - $cursor; $hunk.Drop = 0; $cursor = $a1 }
        for ($k = 0; $k -lt $d; $k++) { $hunk.Insert.Add($lines[$c - 1 + $k]) }
        $hunks.Add($hunk)
    }
    $patch = [BunkerVRPatch]::Write($path, $original, $result, $hunks)
    $check = [BunkerVRPatch]::Apply($original, $patch)
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$check, [byte[]]$result)) { throw "The patch for $path does not rebuild your file: nothing written for it." }
    if ((Test-Path -LiteralPath $patchFile) -and [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($patchFile), [byte[]]$patch)) { $same++; continue }
    New-Item -ItemType Directory -Force (Split-Path $patchFile) | Out-Null
    [IO.File]::WriteAllBytes($patchFile, $patch)
    $written++
    Write-Output "Updated patches\$rel.bvrpatch"
}

$copied = 0
$own = Join-Path $repo 'game'
foreach ($f in (Get-ChildItem -LiteralPath $own -Recurse -File)) {
    $rel = $f.FullName.Substring($own.Length + 1)
    $now = Join-Path $Game $rel
    if (-not (Test-Path -LiteralPath $now)) { continue }
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($now), [byte[]][IO.File]::ReadAllBytes($f.FullName))) {
        Copy-Item -LiteralPath $now -Destination $f.FullName -Force
        $copied++
        Write-Output "Updated game\$rel"
    }
}
Write-Output ("Done: {0} patches written, {1} unchanged, {2} removed; {3} of the mod's own files updated." -f $written, $same, $dropped, $copied)
