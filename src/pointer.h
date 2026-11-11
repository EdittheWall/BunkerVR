#pragma once

// BunkerVR: the pictures of the pointer in menus: a glowing orb at each hand, a
// beam and a cursor, all with soft edges (vr.cpp adds a buzz on hover). Each is a
// small see-through picture, its colors already multiplied by how solid they are
// (as the headset takes a see-through picture), shown by vr.cpp as a layer of its
// own in front of the menus:
//   the orb:    a soft round light at each hand, warm like an ember in the game's
//               amber; bright on the hand that points, dim on the other
//   the beam:   a thin light from the orb to the menu, soft at its sides, fading
//               out towards the menu (the picture's first row is the hand's end)
//   the cursor: a soft dot in a thin amber ring where the beam meets the menu
// No models: each is worked out once, here, from a few sums.

enum PointerPictureKind
{
    kPointerOrb,
    kPointerOrbDim,
    kPointerBeam,
    kPointerBeamDim,
    kPointerCursor,
    kPointerCursorDim,
    kPointerPictureCount
};

// The picture (worked out the first time it is asked for, then kept), four
// bytes a pixel (red, green, blue, how solid), row after row from the top.
const unsigned char* PointerPicture(int kind, int& width, int& height);
