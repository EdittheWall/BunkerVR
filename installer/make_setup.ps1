# make_setup.ps1: builds BunkerVR Setup from this repository. It packs the player build, the mod's own game files
# (game\) and the patches to the game's own files (patches\) into a single installer.
#
#   powershell -ExecutionPolicy Bypass -File installer\make_setup.ps1
#   powershell -ExecutionPolicy Bypass -File installer\make_setup.ps1 -GameExe "<game folder>\AmnesiaTheBunker.exe"
#
# Build the player preset first (docs\building.md). Needs Visual Studio's C++ tools, found through vswhere.
# -GameExe records which version of the game the mod was made for; Setup then tells players with another version.
# Writes out\setup\BunkerVR Setup <version>.exe (the staging files stay in out\setup\stage).

param(
    [string]$Build,
    [string]$GameExe,
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$installer = $PSScriptRoot
$repo = Split-Path -Parent $installer
if (-not $Build) { $Build = Join-Path $repo 'out\build\player' }
if (-not $Out) { $Out = Join-Path $repo 'out\setup' }
$stage = Join-Path $Out 'stage'
$payloadRoot = Join-Path $stage 'game'

foreach ($name in 'version.dll', 'BunkerVR_watch.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $Build $name))) { throw "$name is not in $Build. Build the player preset first." }
}
if ($GameExe -and -not (Test-Path -LiteralPath $GameExe)) { throw "No such file: $GameExe" }

$cmake = [IO.File]::ReadAllText((Join-Path $repo 'CMakeLists.txt'))
if ($cmake -notmatch 'project\(BunkerVR VERSION (\d+)\.(\d+)') { throw 'The version was not found in CMakeLists.txt.' }
$major = [int]$Matches[1]; $minor = [int]$Matches[2]
$version = '{0}.{1:00}' -f $major, $minor

# The staging folder: the payload laid out as it goes into the game's folder.
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $payloadRoot | Out-Null
$list = New-Object System.Collections.Generic.List[string]
foreach ($name in 'version.dll', 'BunkerVR_watch.exe') {
    Copy-Item -LiteralPath (Join-Path $Build $name) -Destination $payloadRoot
    $list.Add($name)
}
foreach ($folder in 'game', 'patches') {
    $from = Join-Path $repo $folder
    foreach ($f in (Get-ChildItem -LiteralPath $from -Recurse -File | Sort-Object FullName)) {
        $relative = $f.FullName.Substring($from.Length + 1)
        $to = Join-Path $payloadRoot $relative
        New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
        Copy-Item -LiteralPath $f.FullName -Destination $to
        $list.Add($relative.Replace('\', '/'))
    }
}
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllLines((Join-Path $stage 'list.txt'), $list.ToArray())
[IO.File]::WriteAllText((Join-Path $stage 'version.txt'), "$version`r`n$major`r`n$minor`r`n", $utf8)
[IO.File]::WriteAllText((Join-Path $stage 'version_rc.h'), "#define BVR_MAJOR $major`r`n#define BVR_MINOR $minor`r`n#define BVR_VERSION_TEXT `"$version`"`r`n", $utf8)
Copy-Item -LiteralPath (Join-Path $installer 'setup.rc'), (Join-Path $installer 'setup.manifest') -Destination $stage
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installer 'make_icon.ps1') -Out (Join-Path $stage 'setup.ico')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $stage 'setup.ico'))) { throw 'The icon could not be made.' }

# Visual Studio's C++ tools.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio was not found (no vswhere.exe).' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ tools was not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$exe = Join-Path $Out "BunkerVR Setup $version.exe"
$game = if ($GameExe) { "`"$GameExe`"" } else { '-' }
$flags = '/nologo /EHsc /O2 /MT /W4 /utf-8 /std:c++17 /DUNICODE /D_UNICODE'
$script = @"
@echo off
call "$vcvars" >nul 2>&1 || exit /b 1
cd /d "$stage" || exit /b 1
cl $flags "$installer\pack.cpp" /Fe"pack.exe" || exit /b 1
.\pack.exe "$payloadRoot" list.txt payload.bin $version $game || exit /b 1
cl /c $flags "$installer\setup.cpp" /I"$installer" /Fo"setup.obj" || exit /b 1
rc /nologo /fo setup.res setup.rc || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS "/OUT:$exe" setup.obj setup.res || exit /b 1
"@
$cmd = Join-Path $stage 'build.cmd'
[IO.File]::WriteAllText($cmd, $script, [Text.Encoding]::ASCII)
& cmd /c "`"$cmd`""
if ($LASTEXITCODE -ne 0) { throw 'Setup did not build (see the output above).' }
Write-Output "Built $exe ($($list.Count) files packed)."
