#pragma once

// A testing aid. Pressing Insert saves what each eye is being shown as two
// picture files next to the game's exe, writes the camera details to the log,
// and then measures how many frames pass between telling the game a head
// position and that position appearing in the picture.

// Asks for a capture (called when Insert is pressed).
void CaptureRequest();

// The same, for a failed check (testlist.h): the pictures go to
// "<prefix>_left.bmp", "_right" and "_interface", next to the game's exe, and
// the timing test is left out (it tilts a frame in the headset).
void CaptureRequestNamed(const char* prefix);

// Call once per frame after the game has finished drawing, with the game's
// OpenGL context active. `eye` is the eye this frame was drawn for (0 = left).
// `source`: the drawing target that holds the eye's picture (0 = the window).
void CaptureOnFrameEnd(int eye, unsigned long long serial, int width, int height, unsigned int source = 0);

// Once a capture has saved the eyes' pictures, the next call saves the
// game's interface picture as well (it shows what is drawn where, for the
// panel and its pieces). Call once per frame in which the interface was
// captured, with the drawing target that holds it.
void CaptureInterfaceOnFrameEnd(unsigned int framebuffer, int width, int height);

// During the timing measurement, one frame is deliberately tilted upwards.
// Returns the tilt (in radians) to add for this frame, usually 0.
float CaptureGetTestTilt(unsigned long long serial);
