#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

// Captures the game's interface (HUD, menus, hints, cursor) into its own
// see-through picture instead of letting it be drawn on top of the 3D view.
//
// The mod shows it in the headset as a flat panel floating in front of the
// player at a comfortable size, and lays it back over the monitor picture.

// Hooks the engine's "draw the interface" function. Call once at startup.
bool GuiInstallHook();

// Call once per frame on the rendering thread (needs the game's OpenGL
// context). Sets up the redirection of the game's drawing target the first
// time it is possible.
void GuiOnFrame();

// If the interface was captured for headset frame `serial`, returns true and
// gives the picture it was drawn into.
bool GuiGetCaptured(unsigned long long serial, GLuint& texture, GLuint& framebuffer, int& width, int& height);

// The last interface captured, whichever frame it was (the big panel's
// pictures are brought up to date only while it is shown). Its picture is
// cleared only as the next capture starts. False if none has been captured yet.
bool GuiGetLastCaptured(GLuint& texture, GLuint& framebuffer, int& width, int& height);

// Writes to the log how long the processor spent on the game's interface
// stage (its menus, hints and the rest of its flat interface, scripts and
// all), per frame on average over `frames` frames.
void GuiLogTimings(unsigned long long frames);
