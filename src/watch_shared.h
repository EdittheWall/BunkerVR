#pragma once

// What the mod holds down in Windows, shared with its watcher (see
// watch.cpp). The mod (vr.cpp) keeps it up to date as it presses and lets go
// (SendModInput) and beats once a frame; the watcher, a small program of its
// own beside the game, lets go of all of it if the game ends without doing so
// (a crash, the game stopped from outside) or stops drawing for kWatchStallMs
// (it hangs), so that no button or key stays held for the PC's other programs.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

// What the mod (and its watcher) sends through Windows carries this mark
// (MOUSEINPUT / KEYBDINPUT dwExtraInfo), so that the mod's own watch on the
// PC's keyboard and mouse can tell that input from the player's.
static const ULONG_PTR kBunkerVRInputMark = 0x42564D52; // "BVMR"

static const LONG kWatchVersion = 1;
static const unsigned long long kWatchStallMs = 3000; // no frame for this long: the game counts as hung

struct WatchShared
{
    LONG version;                 // kWatchVersion
    volatile LONG mouseHeld;      // the mouse buttons held: 1 left, 2 right, 4 middle
    volatile LONG keyHeld[16];    // the keys held, a bit each, by scan code (extended ones from 256)
    volatile LONG64 beatMs;       // GetTickCount64 at the mod's last frame (0: none yet)
    volatile LONG letGo;          // set by the watcher when it let go of everything while the game was hung
};

// The shared memory's name for the game with this process id.
inline void WatchSharedName(DWORD processId, wchar_t* name, size_t size)
{
    swprintf(name, size, L"Local\\BunkerVR_Held_%lu", processId);
}
