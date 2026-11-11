#pragma once

// Hooks the engine function that draws one viewport (the 3D world, screen
// effects and the flat interface). Used to time the drawing, and to draw the
// world twice inside one game frame, once for each eye (see scene.cpp).
// Returns true if the hook was installed.
bool InstallSceneHook();

// Writes to the log how long drawing the world took since the last call.
void SceneLogTimings();

// The frame hook says when a game frame starts (after the mod has begun its
// headset frame) and when it is about to end, so that the processor's time
// for the game's own update (from the start to the drawing of the world) and
// after the world (to the end) can be logged too.
void SceneFrameStarted();
void SceneFrameEnding();

// The last game frame's processor time by part, in ms (for the late frames
// in vr.cpp): [0] the game's own update, [1] the left eye's world, [2]
// the rest of the drawing (the left eye's picture taken, the right eye's world
// and the interface), [3] after the world. toTheEnd: that frame waited for the
// graphics card to finish (one in 450, for the log); worlds: how many worlds it
// drew. Read before the next frame starts (SceneFrameStarted clears it).
void SceneLastFrame(double parts[4], bool& toTheEnd, int& worlds);
