#pragma once

// How each of the game's items sits in the hand, and how the hand is posed.
//
// The game hangs a held item on its hand bone the way its own animations
// want it; here the hand bone follows the controller, so an item can sit at
// an odd angle (the fuel can, for example).
// Each "pose" (an item in a hand, or a hand at work: steadying the gun,
// carrying a bullet, pulling the flashlight's cord, holding a door's handle
// or a loose thing) may have, each one optional:
//   - its own placement (items only): where the item sits from the middle of
//     the palm and how it is turned, in the controller's own directions (x
//     right, y up, z backwards: the controller points along -z);
//   - its own fingers: how far each finger closes, how far it is spread
//     sideways, and how cupped the palm is.
// The game's body script places the item and poses the fingers each frame
// (VRReadGrips, VRPlaceHeldByGrip and UpdateVRFingersFull in
// Prop_PlayerBody.hps); what has no pose of its own is left as the game has
// it.
//
// Set in the headset on the "Grips" page of the mod's menu, and on the PC in
// the hand poses tool (posetool.h). Kept in BunkerVR_grips.txt next to the
// game (one line per pose; named poses of your own too).

// The game's item types, in the order of config\Inventory.cfg (the body
// script sends an item as its place in this list: see VRHeldItemIndex in
// PlayerBodyHandler.hps, which has the same list).
static const int kGripItemCount = 33;

// After the items: hands at work, fingers only (a few also have a placement:
// see GripSlotHasPlace).
enum
{
    kSlotGunSupport = kGripItemCount, // the left hand steadying the gun
    kSlotBullet,                      // the left hand carrying a bullet to the open revolver
    kSlotCord,                        // the right hand pulling the flashlight's cord
    kSlotDoor,                        // a hand holding a door's handle
    kSlotLoose,                       // a hand holding a loose thing (the game's grab or a hand's own hold)
    kSlotBandageRoll,                 // the right hand holding the bandage's roll while winding (it has a placement too)
    kSlotDogtag,                      // a hand holding a dogtag pinched to read it (the tag goes where the pose's thumb and first finger meet, or where its own placement puts it)
    kSlotShotgunPump,                 // the left hand holding the shotgun's pump (its placement is where the fore-end sits in the hand: the hand is drawn fixed on it)
    kGripSlotCount
};

const char* GripItemName(int slot);  // its name in the file ("Fuel", "GunSupport"); "" if out of range
const char* GripItemTitle(int slot); // its everyday name ("Fuel can", "Steadying the gun"), for people
bool GripSlotHasPlace(int slot);     // can it be moved in the hand (items, and the bandage's roll)
bool GripSlotHasHand(int slot, int hand); // can that hand (0 left, 1 right) be in it

struct HandPose
{
    float curl[5] = { 0.55f, 0.75f, 0.8f, 0.85f, 0.9f }; // thumb, first, middle, ring, little finger: 0 straight to 1 closed
    float spread[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };  // degrees sideways at the knuckle: + towards the little finger's side (the thumb: + out from the hand, - across the palm)
    float cup = 0.0f;                                    // the palm: 0 flat to 1 cupped
};

struct ItemGrip
{
    bool set = false;                                       // its own placement (items only; otherwise the game's own)
    float forward = 0.0f, up = 0.0f, right = 0.0f;          // meters: where the item's own center is from the center of the palm
    float turnRight = 0.0f, tipUp = 0.0f, leanRight = 0.0f; // degrees: how it is turned in the hand
    bool fingersSet = false;                                // its own fingers (otherwise as the game's animation has them)
    HandPose pose;
};

// Hand poses of your own, by name, to start others from.
static const int kPoseNameMost = 40;
static const int kPoseLibraryMost = 64;
struct NamedPose
{
    char name[kPoseNameMost] = "";
    HandPose pose;
};

// Reads BunkerVR_grips.txt (at start-up).
void LoadGrips();

// --- From the game's scripts (camera.cpp) ---
// Roll 7084: which item each hand holds: (left, right, 0), each its place in
// the list above, or -1 (nothing, or the gun: it has its own angles).
void GripsHeldNews(const float numbers[3]);
// Rolls 7086 .. 7091: where the item in a hand is as the game places it, in
// the palm's terms: part 0 its center, 1 its own z direction, 2 its own
// y direction (7086 .. 7088 the left hand, 7089 .. 7091 the right).
void GripsGamePlacingNews(int hand, int part, const float numbers[3]);
// Roll 7085: a pose, for a hand. which: 0 the item it holds, or 1 + a slot:
// of a hand at work (kSlotGunSupport...), or of an item the hand holds in a
// way the game doesn't report (the bandage's tin in the left hand while the
// bandage is wound); asking also tells the mod that the hand is doing that.
// part 0: (12345, flags: 1 its own placement, 2 its own fingers,
// 4 send the game's placing; the item as the mod knows it), 1: the item's
// center, 2: its z direction, 3: its y direction, 4: curls (thumb,
// first, middle), 5: (ring, little, cup), 6: spreads (thumb, first, middle;
// degrees), 7: (ring, little, 0).
void GripsAnswer(int hand, int part, int which, float out[3]);

// --- Each frame (frame.cpp: on the game's own frame, also while the
// --- headset is off) ---
// Keeps the menu's numbers in step with the held item, makes the hand poses
// tool's changes, and copies the state for it. True if the menu's numbers
// changed (the menu is then drawn again).
bool GripsTick();
// While a grip is being set by hand (the menu's "Move it with the other
// hand"), both hands' squeezes are taken: the other hand's squeeze moves the
// item as if that hand held it. squeezed: each hand squeezing now. Returns
// 0 not setting a grip (the squeezes are the game's), 1 setting one, 2 the
// other hand has just taken hold of the item, 3 it has just let go (saved).
int GripsTakeSqueezes(const bool squeezed[2]);

// --- The menu's Grips page (vrmenu.cpp) ---
struct GripView
{
    int hand = 0;           // 0 right, 1 left (as the menu's choice shows it)
    bool editing = false;   // "Move it with the other hand"
    bool onPc = false;      // the hand poses tool is open on the PC's screen
    float forward = 0.0f, up = 0.0f, right = 0.0f;
    float turnRight = 0.0f, tipUp = 0.0f, leanRight = 0.0f;
    bool fingersSet = false;
    float thumb = 0.55f, first = 0.75f, others = 0.85f; // (others: the middle, ring and little fingers together)
};
GripView& GripsView();
void GripsMenuEdited();   // a number or the fingers switch changed
void GripsMenuReset();    // the held item back to the game's own way
void GripsMenuEditing();  // "Move it with the other hand" switched
void GripsMenuHand();     // the hand switched
const char* GripsFooter();

// --- The hand poses tool on the PC's screen (posetool.cpp) ---
// It runs on a thread of its own, so it never touches the poses itself: it
// reads a copy that GripsTick makes on each frame, and asks for changes,
// which GripsTick makes on the next frame (on the game's own thread, as
// everything else here).
struct GripDeskHand
{
    int item = -1;          // the item it holds (-1 nothing, or the gun)
    bool known = false;     // where the game holds that item is known (game)
    ItemGrip game;          // that (placement only)
    int atWork = -1;        // what the hand is doing (kSlotGunSupport...), or an item it holds that the game doesn't report (the bandage's tin while winding), or -1
    // known and game describe atWork's item if it is one, otherwise item.
};
struct GripDeskState
{
    bool fresh = false;       // the game's scripts are saying what the hands hold (not in a menu or a loading screen)
    bool gunRight = false;    // the gun is out in the right hand
    bool headsetOff = false;  // the headset is off: head and hands held where they were
    bool handsFrozen = false; // "Freeze my hands"
    bool bandageWinding = false; // the bandage is being wound (VrGetBandage): the right hand holds the roll, its tin is in the left hand
    bool dirty = false;       // changed since last saved
    unsigned version = 0;     // goes up with every change, from anywhere
    unsigned long long tickMs = 0; // when the game's frame made this copy (GetTickCount64); if it is old, the game isn't running its frames
    GripDeskHand hand[2];     // 0 left, 1 right
    ItemGrip table[kGripSlotCount][2];
    int libraryCount = 0;
    NamedPose library[kPoseLibraryMost];
};
void GripsDeskSnapshot(GripDeskState& out);
void GripsDeskSetSlot(int slot, int hand, const ItemGrip& grip); // a pose becomes this (on the next frame)
void GripsDeskSetLibrary(const NamedPose* poses, int count);    // the named poses become these
void GripsDeskSave();   // writes BunkerVR_grips.txt
void GripsDeskRevert(); // reads it again (changes since it was saved are lost)
// Whether the tool is open (the scripts then say where the game holds
// items), and whether it shows the game's own fingers for comparing.
void GripsSetDeskOpen(bool open);
void GripsSetShowGameFingers(bool show);
