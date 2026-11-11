#pragma once

// The game's own update timed part by part.
// The engine marks the start and the end of many pieces of its work by name
// ("UpdatePhysics", "NewtonUpdate", ...) for a profiler that is off in the
// shipped game; these hooks time those marks themselves, on the game's main
// thread only, and nothing in the game changes.

// Hooks the engine's two marks; writes its own line to the log.
bool EngineTimeInstall();

// The game's main thread: the thread that shows each frame (frame.cpp calls
// this once a frame from there). Marks from other threads are not timed.
void EngineTimeFrame();

// Timing on or off (on while the timing lines come every 10 seconds: each
// mark costs a little, and there are some 900 a frame).
void EngineTimeActive(bool active);

// One line to the log: the parts that took the most, per frame on average
// over `frames` frames (each with what it calls inside it), then cleared.
void EngineTimeLog(unsigned long long frames);

// Which eye's world is being drawn (set by scene.cpp): 0 none, 1 the left,
// 2 the right. A part begun and ended within one eye's drawing is also counted
// for that eye, and the log then has a line of each eye's parts side by side:
// what the right eye's drawing costs that the left eye's does not.
void EngineTimeEye(int eye);
