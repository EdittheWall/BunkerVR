# How BunkerVR works

Amnesia: The Bunker runs on HPL3, Frictional Games' own engine. It draws with OpenGL, keeps most of its gameplay in
AngelScript scripts that ship as plain text with the game, and knows nothing about VR. BunkerVR has two halves: a DLL
that hooks the engine and drives the headset, and a set of changes to the game's scripts that rebuild the player's
body, hands and items for VR.

## Getting in: version.dll

The game loads `SDL2.dll`, and SDL2 asks Windows for `version.dll`. Windows looks in the game's own folder before the
system folder, so the mod is a `version.dll` placed next to the game's exe. It passes every real `version.dll` call on
to the one in System32 ([src/proxy.cpp](../src/proxy.cpp)), then sets itself up:

- **Hooks.** The engine functions the mod needs are found by byte pattern in the running game
  ([src/pattern.cpp](../src/pattern.cpp)) and redirected with [MinHook](https://github.com/TsudaKageyu/minhook). Every
  pattern must match exactly once; if one doesn't (after a game update, for example), that feature turns itself off
  and says so in the log rather than guessing.
- **The script bridge.** Before the game starts its script engine, the mod registers its own script functions
  (`cVR_Version()`, `cVR_HandSqueeze()`, `cVR_BeastSight()` and many more), which the game's scripts then call like any
  of the game's own ([src/scriptfuncs.cpp](../src/scriptfuncs.cpp)).
- **The watcher.** `BunkerVR_watch.exe` runs next to the game. The mod presses keys and buttons on the game's behalf,
  so if the game ever closes unexpectedly, the watcher releases anything still held down
  ([src/watch.cpp](../src/watch.cpp)).

## Two eyes from one game

The mod talks to the headset through OpenXR with OpenGL, which the game already uses, so the eyes' pictures are drawn
straight into the headset's swapchains ([src/vr.cpp](../src/vr.cpp)).

- **The camera** ([src/camera.cpp](../src/camera.cpp)). The game's camera is the player's body; the headset's pose
  is added on top of it each frame, measured from where "straight ahead" was set when you last recentered. Turning
  with the stick turns the body, your real head turns the view.
- **Drawing** ([src/scene.cpp](../src/scene.cpp), [src/frame.cpp](../src/frame.cpp)). Within each game frame, the
  engine's "draw a viewport" function runs twice, once per eye, each with that eye's position and field of view.
  The right eye reuses work the left eye has just done where the result can't differ, such as the shadow maps of
  lights that haven't changed, which keeps the second eye cheap.
- **The interface** ([src/gui.cpp](../src/gui.cpp)). The game's menus, hints and HUD are captured into a picture of
  their own instead of being painted over the 3D view. The mod lays that picture out in the room as a panel at a
  comfortable distance, and the monitor gets a steady spectator view.

## The body and the hands

Almost all of the gameplay is script code, so that's where BunkerVR does most of its work. The DLL reports the
controllers' poses, buttons and finger positions to the scripts; the scripts decide what the hands do.

- **The body** (`PlayerBodyHandler.hps`). The game's player model gets full arms that reach for your controllers,
  bent at the elbows and wrists to match. Your real crouch, lean and room-scale steps move the character, and the
  pace of your steps decides how loud you are.
- **The hands** (`Prop_PlayerBody.hps`). Each hand finds what it touches, grabs and holds it, and is held back by
  what it can't pass through. Held items sit in the palm as they would in a real grip ([src/grips.cpp](../src/grips.cpp)
  and `BunkerVR_grips.txt`).
- **The items** (`helper_bunkervr_*.hps`, plus changes to each item's own script). The revolver, the shotgun, the
  tools, keys, fuses, the bandage and the rest each have their VR mechanics written out step by step: what counts as
  a pump, how far a key must turn, when a bandage wrap is complete.
- **The creatures' sight** ([src/sight.cpp](../src/sight.cpp)). The engine judges whether the Beast can see you from
  the game's standing or crouched height. The mod moves that test to your real head's height, so a real crouch behind
  cover hides you.

## Patches instead of game files

The mod's script changes are distributed as patches, never as copies of the game's files. Each file in
[patches/](../patches) holds the mod's own lines and instructions on where they go ("keep 120 lines, drop 2, insert
these"), plus a SHA-256 fingerprint of the exact game file it applies to and of the result. The format is described
at the top of [tools/BunkerVRPatch.cs](../tools/BunkerVRPatch.cs).

Setup applies the patches to the files on your PC. If a game file isn't the version a patch was made for (because
the game was updated, or another mod changed it), Setup stops and changes nothing rather than produce a broken mix.
It keeps the game's original files aside and puts them back when you uninstall.
