#pragma once

// The game's clock, held still on request.
//
// For "both eyes every frame" the game draws its picture twice per headset
// frame: once for the left eye, once for the right. Between those two the
// game's world must not move on, or the eyes would see slightly different
// worlds (things in the hand shimmer, depth wobbles). The game decides how far
// to move its world by reading the clock, so while the right eye's picture is
// being drawn the game is shown a clock that has (almost) stopped.
//
// Only the game's own exe sees the held clock; everything else on the
// computer, including this mod and the sound, keeps real time.

// Redirects the two clock functions the game's exe uses. Returns false if
// they could not be found (then holding does nothing).
bool InstallClockHooks();

// Starts or ends a hold. A hold that lasts longer than 80 ms lets go by
// itself, and after six of those holding rests for 5 seconds: it means the
// game is not drawing while the clock stands still.
void ClockHold(bool hold);

// Is holding unavailable (the hooks could not be installed)?
bool ClockHoldUnavailable();
