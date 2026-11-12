#pragma once

// The mod's own menu, shown in the headset on a panel of its own (Y on the
// left controller opens and closes it). Its tabs come in two sets: the
// player's settings (Player, Holsters, Comfort, Movement, Hands,
// Accessibility, System), named as VR games name them, and behind "Debug" the
// pages for testing the mod (Tuning, Tools, Test List, Spawn, Beast, Places,
// Bandage, Physics, Grips).
//
// This file draws the menu into a plain picture in memory and reacts to a
// pointer. Showing that picture in the headset is done in vr.cpp.

void VrMenuSetOpen(bool open);
bool VrMenuIsOpen();

// Tell the menu where a hand is pointing on it: `u` across (0 = left edge,
// 1 = right edge) and `v` down (0 = top, 1 = bottom), or that no hand points
// at it. `squeezing` is whether that hand's trigger or grip is pressed.
void VrMenuPointer(bool onMenu, float u, float v, bool squeezing, double nowMs);
// Is the pointer on something to press (as last told)? A pull of the trigger
// there is a press, not the start-up page's "then pull the trigger".
bool VrMenuPointerOnButton();
// For the pointer's look and feel: counts up each time the pointer
// comes onto something to press (a light buzz), and when one was last pressed
// (the cursor's pulse), in the clock given to VrMenuPointer.
int VrMenuHoverCount();
double VrMenuLastPressMs();

// The menu's picture: 4 bytes per pixel (red, green, blue, alpha), top row
// first. `changed` is true when it differs from the last time this was called.
const unsigned char* VrMenuPicture(int& width, int& height, bool& changed);

// The bullet counter's picture: a small see-through tag with `text` on it
// (same layout as the menu's picture; made afresh on every call).
const unsigned char* VrCounterPicture(const char* text, int& width, int& height);

// The bullets at the pouch: the game's own picture of a bullet (its grayscale
// one at none), with "x3" on it as the game writes it, white with a shadow, in
// its menu lettering. Null if the game's files cannot be read.
// shells: the shotgun's shell instead (inventory_item_shell.dds).
const unsigned char* VrPouchPicture(int count, bool shells, int& width, int& height);

// The hints on the objects (hints.h): one line drawn as the game draws its own
// hints: its hint lettering (Work Sans Medium, fonts\work_56), white, with a
// thin hard shadow, the rest see-through (colors stored multiplied by how
// solid they are). Made afresh on every call.
// VrHintFits: do the words fit its line? VrHintFontIsGames: is the game's hint
// lettering in use (false: its files could not be read, the menu's own)?
const unsigned char* VrHintPicture(const char* words, int& width, int& height);
int VrHintTextWidth(const char* words);
bool VrHintFits(const char* words);
bool VrHintFontIsGames();

// One line of text shown at the bottom of the Tuning page (Debug).
void VrMenuSetInfo(const char* text);

// The same for the Player and Holsters pages and the start-up page (how
// "Calibrate Height" is going), in place of their usual line; "" for that
// line again. Up to two lines of the menu's width (VrMenuFooterFits).
void VrMenuSetComfortInfo(const char* text);

// Turns to a page, by its number (the remote "page" command):
// 0 Comfort, 1 Movement, 2 Hands, 3 Tuning, 4 Tools, 5 Test List, 6 Spawn,
// 7 Beast, 8 Bandage, 9 Physics, 10 Grips, 11 Holsters, 12 Player,
// 13 Accessibility, 14 System, 15 Places; 17 Controls (the whole layout,
// opened from the title line).
void VrMenuShowPage(int page);

// For the tests: every page's words measured against their room,
// each one too wide told to `report`; how many there are. And whether a
// footer line fits its two lines.
int VrMenuLayoutProblems(void (*report)(const char* what));
bool VrMenuFooterFits(const char* text);

// The Grips page (see grips.h): is it the page shown (or the one
// that will be, when the menu opens)? And its numbers have changed: draw it
// again if it is up.
bool VrMenuOnGripsPage();
void VrMenuGripsChanged();

// Buttons in the menu that need something done elsewhere.
enum VrMenuAction
{
    kMenuActionRecenter = 1,  // "Recenter View"
    kMenuActionCapture = 2,   // "Capture Eye Images and Hand Data"
    kMenuActionSeatedChanged = 3,
    kMenuActionFeelDefaults = 4, // the Physics page: "Restore Tested Defaults"
    kMenuActionGripChanged = 5,  // the Grips page: a number or the Finger Pose switch changed
    kMenuActionGripReset = 6,    // the Grips page: "Reset to Game Default"
    kMenuActionGripEditing = 7,  // the Grips page: "Adjust with Other Hand" switched
    kMenuActionGripHand = 8,     // the Grips page: the hand switched
    kMenuActionGripWindow = 9,   // the Grips page: "Mirror to Desktop Window" switched
    kMenuActionMeasureSize = 10, // the Player page: "Calibrate Height"
    // The Test List page (testlist.h).
    kMenuActionCheckJump = 11,   // "Go to Test Location"
    kMenuActionCheckPass = 12,
    kMenuActionCheckFail = 13,   // also a mark, the last two minutes and both eyes
    kMenuActionCheckSkip = 14,
    kMenuActionCheckBack = 15,
    kMenuActionCheckNext = 16,

    // The Tools page. These numbers are also the commands sent to the
    // game's body script (see VRDoTestCommand in PlayerBodyHandler.hps).
    kMenuActionTestFirst = 20,
    kMenuActionTestHurt = 20,
    kMenuActionTestHeal = 21,
    kMenuActionTestGiveAll = 22,
    kMenuActionTestHealthItems = 23,
    kMenuActionTestGrenades = 24,
    kMenuActionTestAmmo = 25,
    kMenuActionTestBeast = 26,
    kMenuActionTestRemoveBeast = 27,
    kMenuActionTestFlashlightEmpty = 28,
    kMenuActionTestProps = 29,
    kMenuActionSpawnFirst = 30, // 30 to 41: one thing each (the Spawn page); 42: the small things
    kMenuActionTestReload = 43, // the game's own reload: the scripts built again from their files
    // The Beast page: each a knockdown, a kill or a death, as the
    // game plays it (VRBeastTestStart in PlayerBodyHandler.hps).
    kMenuActionBeastThrown = 44,       // thrown by the Beast, facing it
    kMenuActionBeastThrownBehind = 45, // thrown by it from behind
    kMenuActionBeastFall = 46,         // a big fall
    kMenuActionBeastShotgun = 47,      // hit by a shotgun
    kMenuActionBeastKillBehind = 48,   // killed by the Beast from behind
    kMenuActionBeastKillFront = 49,    // killed by it from the front
    kMenuActionBeastDie = 50,          // an ordinary death
    // (The Beast's other kills, each as the game places it.)
    kMenuActionBeastKillHole = 51,     // killed through the nearest of its holes in the walls
    kMenuActionBeastKillHiding = 52,   // killed while hiding (under a bed or a table)
    kMenuActionBeastKillLadder = 53,   // killed on a ladder
    kMenuActionTestLast = 59,
    // The Checks page's reasons after a Fail: 70 to 75, the six
    // buttons of TestListWhyChoice.
    kMenuActionCheckWhyFirst = 70,

    // The measuring steps, on the Player and Holsters pages.
    kMenuActionBodyFloor = 80,   // "Calibrate Floor"
    kMenuActionBodyArms = 81,    // "Measure Arm Span"
    kMenuActionBodyHolster = 82, // "Set Holster Position"
    kMenuActionBodyPouch = 83,   // "Set Ammo Pouch Position"
    kMenuActionBodyLight = 84,   // "Set Flashlight Position"
    kMenuActionBodyUndo = 85,    // "Undo Calibration Changes"
    // The start-up page: its three big buttons, and the Player
    // page's buttons that bring it up.
    kMenuActionStartupButton = 86, // 86, 87, 88: the page's buttons, from the left
    kMenuActionStartupRun = 89,    // "Run Player Setup Again"
    kMenuActionStartupSit = 90,    // "Calibrate Seated Height" alone
    kMenuActionPouchMoved = 91,    // the Holsters page: one of the pouch's numbers changed
    kMenuActionHolsterMoved = 92,  // one of the holster's
    kMenuActionHintsReset = 93,    // the Accessibility page: "Show Learned Hints Again" (HintsForget)
    kMenuActionGameMenu = 94,      // the Player page: "Open Game Menu" (SteamVR gives the game no left menu button)
    kMenuActionBodySlot = 95,      // the Holsters page: "Set Chest Item Slot Position"
    kMenuActionWorldScaleMoved = 96, // the Player page: the calibrated world scale changed by hand
    kMenuActionPlaceFirst = 100,   // 100 to 111: the Places page's places (VrPlaceName, VrPlaceWords)
    kMenuActionPlaceLast = 111,
};

// The Places page, for visiting the game's own levels safely, to show or try a mechanic
// there: the places, by number (0 .. VrPlaceCount() - 1): their names as the page shows
// them, and where to go, in the jump's words ("map=... start=...": TestJumpToPlace).
int VrPlaceCount();
const char* VrPlaceName(int index);
const char* VrPlaceWords(int index);
// The page's "The Beast: Kept Away" (not saved: each start of the game has it in the level).
bool VrBeastAway();
// The same, set for an automated run (the remote command "set beast_away=1").
void VrSetBeastAway(bool away);
void VrMenuSetActionHandler(void (*handler)(int action));

// The start-up page: vr.cpp goes through it, step by step, and
// says what the menu shows; the menu draws it in place of its tabs, and its
// buttons send kMenuActionStartupButton + their number.
struct VrStartupView
{
    const char* title;      // the step's heading
    const char* text;       // what to do (wrapped to fit)
    const char* note;       // a dimmer line under it, or ""
    const char* buttons[3]; // its big buttons, "" for none
    bool heightPicker;      // the "Height" row, - and + (VrMenuStartupHeight)
};
// Shows the page (the menu must be open), or with null puts the tabs back.
// Call again whenever the view's words change.
void VrMenuSetStartup(const VrStartupView* view);
// The height on the page's picker, in meters.
float& VrMenuStartupHeight();
// Has the picture last drawn see-through parts (the start-up page in the game's
// own look)? The headset is then to blend it with what is behind.
bool VrMenuSeeThrough();

// The Tuning page's "Show Holster Zone": draw a marker where the holster is.
bool VrMenuShowHolster();

// Is the Holsters page up (kPageBody in vrmenu.cpp)? The holster's and the
// pouch's spots are then marked, to be moved with its numbers.
bool VrMenuOnBodyPage();

// Is the Controls page up?
bool VrMenuOnControlsPage();

// Is the left menu button the game's? Not on SteamVR, whose Touch
// controllers keep it: the Controls page then says the game's menu is on the
// Player tab. True until told otherwise.
void VrMenuSetGameMenuButton(bool works);
