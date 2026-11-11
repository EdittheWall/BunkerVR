#pragma once

// The hand poses tool: a developer window on the PC's screen for editing how
// each item sits in the hand and how the fingers are posed. Built with Dear
// ImGui (third_party\imgui, MIT license), drawn with DirectX 11 in a window of
// its own, on a thread of its own inside the game (d3d11.dll is delay-loaded,
// so DirectX is only loaded when the tool first opens).
//
//   top:    whether the game is connected, "Freeze my hands", the hand,
//           Undo / Redo, Save, and what each hand is doing right now;
//   left:   every pose, by kind (items, hands at work), each for the right
//           and the left hand; and named poses of your own;
//   middle: the fingers: a drawing of the hand (palm and side) whose fingers
//           can be dragged, each finger's curl and spread, the whole hand's,
//           starting points, mirroring to the other hand;
//   right:  where the item sits in the hand and how it is turned, and a
//           camera on the hand (with the headset off, the PC's screen then
//           shows the hand from the chosen angle).
// Every change shows in the game immediately; Save writes BunkerVR_grips.txt
// (see grips.h). While the tool's window is in front, the controllers can't
// play (the mod only presses the game's keys while the game's window is in
// front).
//
// Opened and closed with F11 (while the game or the tool is in front), or
// with the Grips page's "Show it on the PC screen too".

void PoseToolSetOpen(bool open); // from the headset's menu: shown without taking the keyboard
// F11, pressed at the PC: if closed, the tool opens in front, ready for the
// mouse and keyboard; if open behind the game, it comes to the front; if in
// front, it closes (asking first if there are unsaved changes).
void PoseToolFromKey();
bool PoseToolIsOpen();       // open, or opening (not while closing)
bool PoseToolIsForeground(); // the tool's window is in front
