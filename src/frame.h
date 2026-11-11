#pragma once

// Hooks the point where the game shows a finished frame on screen.
// Returns true if the hook was installed.
bool InstallFrameHook();

// A mark: the timing lines every 10 seconds again, the game's own work by part with
// them, for the next minute, as at the start of a level. So a timed minute can start
// whenever the player is ready, with a mark.
void FrameTimingsAgain();
