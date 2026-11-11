#pragma once

// Hints on the objects, explaining how to use each mechanic where it is used.
// A small label in the game's own lettering at the spot the hand reaches, facing
// you: "Grip: Hold, then pull or push". Each kind shows until it has been used a
// few times (kHintLearnedAfter), or always, or never (the Accessibility page's
// "Interaction Hints", Settings::interactionHints).
//
// Every hint's words are in the table in hints.cpp, so that all of them can be
// measured in one place. The game's scripts say which hint a hand is at and
// where (cVR_Hint, scriptfuncs.cpp); the mod finds some itself (the holster, the
// pouch, the flashlight at the chest, the revolver, the bandage: UpdateHints in
// vr.cpp). What a label shows, and where, is decided in vr.cpp.

// What counts as using a hint while it shows, besides being told
// (HintUsed): a press of the hand's own grip or trigger, of its grip only, or
// of its trigger only. kHintUseTold: only when told.
enum HintUse
{
    kHintUseTold = 0,
    kHintUseSqueeze = 1,
    kHintUseGrip = 2,
    kHintUseTrigger = 3,
};

struct HintInfo
{
    const char* key;   // its name, as the scripts and the mod give it
    const char* words; // what the label says; up to the first ':' in amber, as a button's name
    int use;           // HintUse
    float lift;        // the label this far above the spot it is given for (meters; below if less than 0)
};

// The table.
int HintCount();
const HintInfo* HintAt(int index);
const HintInfo* HintFind(const char* key);

// Used this many times, a hint is learned and stops showing (with "Until
// Learned").
static const int kHintLearnedAfter = 3;

// Would this hint show now (the setting, and how often it has been used)?
bool HintWanted(const HintInfo* info);
bool HintWanted(const char* key);
// It has been used: counted once (never twice within 1.5 s) and kept in
// BunkerVR_hints.txt, beside the game. (A key not in the table is logged once.)
void HintUsed(const HintInfo* info, double nowMs);
void HintUsed(const char* key, double nowMs);
int HintTimesUsed(const HintInfo* info);
// Every hint as never used ("Reset Interaction Hints").
void HintsForget();
// The last hint told used (HintUsed), and when: a label showing it goes.
const HintInfo* HintLastUsed(double& atMs);

// From the scripts (cVR_Hint): this hand (0 left, 1 right) is at what this
// hint is for, at this place in the game's world. Reported every update while
// it is so. A key not in the table is logged once and left out.
void HintFromScript(int hand, const char* key, const float world[3], double nowMs);
// What the scripts reported last for a hand, if within the last 250 ms.
bool HintScriptSays(int hand, double nowMs, const HintInfo*& info, float world[3]);

// What the player carries, as the scripts last reported (cVR_HintCarried), for
// the mod's own hints: 0 if not reported within the last second.
enum
{
    kCarryRevolver = 1,   // a revolver in the pockets (not in the hand)
    kCarryShotgun = 2,    // a shotgun in the pockets (not in the hand)
    kCarryFlashlight = 4, // the flashlight (the dynamo lantern), not in the hand
    kCarryBullets = 8,    // spare revolver bullets
    kCarryShells = 16,    // spare shotgun shells
    kCarryChestItem = 32, // the right chest slot's item (the game's hotkey 1) is carried, not in the right hand
    kCarryChestTakes = 64, // the item in the right hand can go into the right chest slot (as the game's hotkeys take it)
};
void HintCarried(int flags, double nowMs);
int HintCarriedFlags(double nowMs);
