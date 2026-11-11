# Building BunkerVR

## What you need

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 or newer with the **Desktop development with C++** workload. It brings the compiler, CMake and
  Ninja.
- Git.
- An internet connection the first time you configure: CMake downloads the Khronos OpenXR SDK (release 1.1.63).
- A copy of Amnesia: The Bunker from Steam to test in.

Clone to a short path, such as `C:\src\BunkerVR`. The OpenXR loader's build creates deep folders, and a long path can
run past Windows' 260-character limit.

## Building the mod

From an **x64 Native Tools Command Prompt for VS** (or a Developer PowerShell), in the repository's folder:

```bat
cmake --preset player
cmake --build --preset player
```

That gives `out\build\player\version.dll` (the mod) and `out\build\player\BunkerVR_watch.exe` (the watcher, which
releases any key or button the mod holds if the game closes unexpectedly).

| Preset | What it builds |
|---|---|
| `player` | What Setup installs: no developer menu or developer keys, and Microsoft's C runtime linked in so it runs on any PC. |
| `dev` | The developer build. Y opens the mod's developer menu (tuning pages, item spawning, the Beast's test page, a checklist of in-headset tests), and the developer keys work: F11 opens the hand-pose tool on the PC's screen. |

Both presets currently build without optimizations (CMake's Debug configuration), as the released builds do.

To have every build copied straight into a game folder, add `-DGAME_DIR="C:/path/to/Amnesia The Bunker"` to the
configure command. The scripts still have to be installed once with `install_dev.ps1`, below.

## Installing your build

```bat
powershell -ExecutionPolicy Bypass -File tools\install_dev.ps1 -Game "C:\path\to\Amnesia The Bunker"
```

This copies `version.dll` and `BunkerVR_watch.exe` from `out\build\player` (or `out\build\dev`; pass `-Build` to
choose), copies the mod's own files from `game\`, and applies every patch in `patches\` to the game's scripts. The
game's untouched files are kept in `<game>\BunkerVR\dev_originals` the first time, and the patches are always applied
to those, so installing again over an older build just works.

`-Uninstall` puts the game's files back and removes the mod's. Steam's "Verify integrity of game files" restores the
game's own files too, after which the mod has to be installed again.

## Working on the scripts

Most of the mod is AngelScript, in the game's own script files. The repository keeps the mod's changes to them as
patches (`patches\`) and its own new files in `game\`.

1. Install your build into a game folder with `install_dev.ps1`.
2. Edit the scripts in that game folder's `script\` folder. In the `dev` build, **Y > Debug > Tools > Reload
   Scripts** picks up script changes without restarting the game.
3. Bring your edits back into the repository:

   ```bat
   powershell -ExecutionPolicy Bypass -File tools\update_patches.ps1 -Game "C:\path\to\Amnesia The Bunker"
   ```

   It rewrites the patch of every game file you changed (each new patch is proven to rebuild your file exactly) and
   copies back the mod's own files from `game\`.

To change a game file the mod hasn't touched before, run `update_patches.ps1 -Add <path inside the game folder>`
first, before editing it, so its untouched original is kept.

The patch format is described at the top of [tools/BunkerVRPatch.cs](../tools/BunkerVRPatch.cs). A patch carries only
the mod's lines and line counts for the rest, plus SHA-256 fingerprints of the exact game file it applies to and of
the result.

## Building Setup

```bat
powershell -ExecutionPolicy Bypass -File installer\make_setup.ps1 -GameExe "C:\path\to\Amnesia The Bunker\AmnesiaTheBunker.exe"
```

This packs `out\build\player`, `game\` and `patches\` into `out\setup\BunkerVR Setup <version>.exe`. `-GameExe` records
which version of the game the mod was made for, so Setup can tell players running a different one. The version comes
from `project(BunkerVR VERSION ...)` in `CMakeLists.txt`.

## Logs

| Log | Where |
|---|---|
| The mod's log | `BunkerVR.log` in the game's folder (the last few runs are kept as `BunkerVR_before*.log`) |
| The game's script log | `Documents\My Games\Amnesia The Bunker\Main\hpl.log` |
| The watcher's log | `BunkerVR_watch.log` in the game's folder |

Lines the mod's scripts write to the game's log start with `BunkerVR`. When something goes wrong in the headset, mark
the moment (hold the left thumbstick in and click the right thumbstick): both logs get a `MARK n` line at that moment.

## What's where

| Folder | What's in it |
|---|---|
| `src\` | The mod's DLL: OpenXR, the hooks into the engine, the menus, settings and hints |
| `game\` | The mod's own script files, models and effects, as they go into the game's folder |
| `patches\` | The mod's changes to the game's own scripts and text files |
| `installer\` | BunkerVR Setup |
| `tools\` | The patch format, and the scripts to install and to update patches |
| `assets\` | The studio card at the start, pre-drawn for player builds |
| `third_party\` | MinHook and Dear ImGui |
| `docs\` | These pages, and the third-party licenses |
