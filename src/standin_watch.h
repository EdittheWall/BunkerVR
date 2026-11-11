#pragma once

// A watch for long stalls, for diagnosis. Only in a run with the stand-in OpenXR runtime
// (g_standIn in vr.cpp), never with a real headset: it logs which files the game opens,
// and where the thread that draws the frames is while a frame is long overdue.

// Starts the watch. Called once, at the start of such a run.
void StandInWatchStart();

// A frame was finished. Called by the thread that draws them, every frame.
void StandInWatchFrame();
