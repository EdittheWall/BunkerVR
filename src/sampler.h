#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A sampling profiler of the game's main thread, to find where the processor waits inside one of the engine's
// steps. Started by the command "sample <mark> <eye> <seconds>" in a test run with a stand-in OpenXR runtime
// (UpdateStandInCommands in vr.cpp): while the engine's timing mark of that name is open within that eye's
// drawing (1 the left, 2 the right, 0 any), a thread of the mod stops the main thread for an instant about
// every 50 microseconds, notes where it is (its instruction) and the exe's return addresses found near the top
// of its stack, and lets it go on. Nothing is allocated, locked or logged while it is stopped. At the end: one
// report in the log, the places by how often they were seen (exe addresses at the exe's preferred base,
// 0x140000000, as disassemblers show them; other modules by name).

// Begins sampling (any sampling before is ended). The mark's name as the engine gives it, e.g. "ShadowMap".
void SamplerArm(const char* mark, int eye, double seconds);

// From the engine's timing marks (enginetime.cpp), on the main thread: a mark begun or ended in that eye's
// drawing (0 when no eye is drawn).
void SamplerMarkBegin(const char* name, int eye);
void SamplerMarkEnd(const char* name, int eye);
bool SamplerArmed();

// The main thread (frame.cpp, each frame, from the main thread): its id and its stack's top.
void SamplerMainThreadHere();
