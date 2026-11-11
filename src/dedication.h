#pragma once

// The studio's card shown in the headset at the very start of the game,
// before any of its menus: "Mina STUDIO", with the name in the traditional
// Mongolian script beside it. Made once with Windows' own font drawing
// (Bodoni MT, Segoe UI Semilight, Mongolian Baiti), white on transparent, no
// glow. 4 bytes a pixel (red, green, blue, alpha), top row first, the colors
// premultiplied by alpha. Shown by vr.cpp (UpdateDedication, SendDedicationPicture).
static const int kDedicationWidth = 384;
static const int kDedicationHeight = 192;

// The card's two layers, so that they can fade in one after the other: the
// name, and the rest (STUDIO and the mark); together they make the card. Made
// the first time this is called. False if it could not be made (both left
// empty): the card is then not shown at all.
bool DedicationLayers(int& width, int& height, const unsigned char*& nameLayer, const unsigned char*& restLayer);

// Draws the card with any name into `picture` (width * height * 4 bytes); with
// `rest`, the name alone goes there and STUDIO and the mark into `rest`. Also
// used by a preview of the card. False if it could not
// be drawn (left black).
bool DedicationMake(const wchar_t* name, unsigned char* picture, int width, int height, unsigned char* rest = nullptr);

// The studio's name on the card ("Mina").
const wchar_t* DedicationName();
