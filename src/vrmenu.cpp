#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_data.h"
#include "grips.h"
#include "settings.h"
#include "testlist.h"
#include "vrmenu.h"

// ---------------------------------------------------------------------------
// The picture
// ---------------------------------------------------------------------------

static const int kWidth = 840;
static const int kHeight = 744;
static unsigned char g_canvas[kWidth * kHeight * 4];

struct Colour { unsigned char r, g, b; };
static const Colour kBackground = { 18, 20, 24 };
static const Colour kRowA = { 26, 29, 34 };
static const Colour kRowB = { 31, 34, 40 };
static const Colour kButton = { 52, 57, 66 };
static const Colour kButtonHot = { 214, 162, 66 }; // the one the hand points at
static const Colour kText = { 232, 232, 228 };
static const Colour kTextDim = { 150, 154, 160 };
static const Colour kTextDark = { 20, 20, 20 };
static const Colour kAccent = { 214, 162, 66 };
static const Colour kButtonPass = { 44, 88, 58 };  // (the Checks page)
static const Colour kButtonFail = { 104, 46, 46 };

static void FillRect(int x0, int y0, int x1, int y1, Colour c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kWidth) x1 = kWidth;
    if (y1 > kHeight) y1 = kHeight;
    for (int y = y0; y < y1; ++y)
    {
        unsigned char* p = g_canvas + (y * kWidth + x0) * 4;
        for (int x = x0; x < x1; ++x, p += 4)
        {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = 255;
        }
    }
}

static int TextWidth(const char* text)
{
    int width = 0;
    for (const char* s = text; *s; ++s)
    {
        int index = (unsigned char)*s - kFontFirst;
        if (index < 0 || index >= kFontCount) index = '?' - kFontFirst;
        width += kFontGlyphs[index].advance;
    }
    return width;
}

// Draws text with its top-left corner at (x, y).
static void DrawString(int x, int y, const char* text, Colour c)
{
    for (const char* s = text; *s; ++s)
    {
        int index = (unsigned char)*s - kFontFirst;
        if (index < 0 || index >= kFontCount) index = '?' - kFontFirst;
        const FontGlyph& glyph = kFontGlyphs[index];
        const unsigned char* pixels = kFontPixels + glyph.offset;

        for (int row = 0; row < kFontHeight; ++row)
        {
            int py = y + row;
            if (py < 0 || py >= kHeight) continue;
            for (int column = 0; column < glyph.width; ++column)
            {
                int px = x + column;
                if (px < 0 || px >= kWidth) continue;
                int a = pixels[row * glyph.width + column];
                if (a == 0) continue;
                unsigned char* p = g_canvas + (py * kWidth + px) * 4;
                p[0] = (unsigned char)((p[0] * (255 - a) + c.r * a) / 255);
                p[1] = (unsigned char)((p[1] * (255 - a) + c.g * a) / 255);
                p[2] = (unsigned char)((p[2] * (255 - a) + c.b * a) / 255);
            }
        }
        x += glyph.advance;
    }
}

static void DrawStringCentred(int x0, int x1, int y, const char* text, Colour c)
{
    DrawString((x0 + x1 - TextWidth(text)) / 2, y, text, c);
}

// ---------------------------------------------------------------------------
// What is in the menu
// ---------------------------------------------------------------------------

enum ItemKind
{
    kSwitch, // on / off, or one of two names
    kNumber, // a number with - and + buttons
    kButtonItem, // does something when pressed
    kCycle, // one of two, three or four names, the next at each press
};

struct Item
{
    const char* label;
    ItemKind kind;
    bool* flag;          // kSwitch
    int* choice;         // kSwitch on an int that is 0 or 1; kCycle: 0, 1 or 2 (or 3)
    const char* offName; // kSwitch: what to show for off and on
    const char* onName;
    const char* thirdName; // kCycle: what to show for 2 (offName for 0, onName for 1)
    const char* fourthName; // kCycle: what to show for 3, or null for a cycle of three
    float* number;       // kNumber
    float step;          // added or taken away per press (if factor is 0)
    float factor;        // multiplied or divided by per press (if not 0)
    float lowest, highest;
    const char* format;  // how to show it, as for printf, given number * shown
    float shown;
    int action;          // kButtonItem, or sent after a kSwitch changes (0 = none)
};

static bool g_showHolster = false;

static Settings& S()
{
    return GetSettings();
}

// The Places page (see vrmenu.h). The start spots are each level's own (its .hpm_Area's
// PlayerStart areas).
static bool g_beastAway = false;
struct Place
{
    const char* name;
    const char* words;
};
static const Place kPlaces[] = {
    { "Hub: Sickroom", "map=officer_hub.hpm start=Start_Begin" },
    { "Hub: The Nest", "map=officer_hub.hpm start=Transfer_Nest" },
    { "Hub: Arsenal Door", "map=officer_hub.hpm start=Transfer_Arsenal" },
    { "Hub: Prison Door", "map=officer_hub.hpm start=Transfer_Prison" },
    { "Trenches: Crafting Table", "map=trenches.hpm start=Start_Crafting" },
    { "Arsenal", "map=arsenal.hpm start=Start_Begin" },
    { "Maintenance", "map=maintenance.hpm start=Start_Begin" },
    { "Maintenance: Pillbox", "map=maintenance.hpm start=Start_Pillbox" },
    { "Prison", "map=prison.hpm start=Start_Begin" },
    { "Soldier Quarters", "map=soldier_quarters.hpm start=Start_Begin" },
    { "Tunnels: Plaza", "map=tunnels.hpm start=Start_Plaza" },
    { "Training Grounds (Range)", "map=training_grounds.hpm start=Start_Begin" },
};
static const int kPlaceCount = (int)(sizeof(kPlaces) / sizeof(kPlaces[0]));

int VrPlaceCount()
{
    return kPlaceCount;
}

const char* VrPlaceName(int index)
{
    return index >= 0 && index < kPlaceCount ? kPlaces[index].name : "";
}

const char* VrPlaceWords(int index)
{
    return index >= 0 && index < kPlaceCount ? kPlaces[index].words : "";
}

bool VrBeastAway()
{
    // (Only after a jump, when nothing is saved: a Beast switched off must never be kept in
    // the player's own saves. A game started or loaded ends that, TestJumpsOver, and the body
    // script then switches back on what it switched off.)
    return g_beastAway && TestJumpedThisRun();
}

static const int kPageCount = 16;
// The pages, in two sets of tabs: the settings a player changes, named and
// grouped as VR games name and group them (Player, Comfort, Movement,
// Accessibility...), and behind the last of their tabs, Debug, the pages for
// testing the mod. The page numbers are fixed (the remote "page" command uses
// them), so a new page goes at the end. Each tab is as wide as its name, with
// an even share of what is left over: see Draw, and VrMenuLayoutProblems for
// the room.
static const char* const kPageNames[kPageCount] = { "Comfort", "Movement", "Hands", "Tuning", "Tools", "Test List", "Spawn", "Beast", "Bandage", "Physics", "Grips", "Holsters", "Player", "Accessibility", "System", "Places" };
// (kPageStartup has no tab: the start-up page is shown in their place. Nor has
// kPageControls, the whole layout of the controls: a button in the title line
// opens it, as VR games have their controls behind a button.)
enum { kPageComfort = 0, kPageMovement = 1, kPageGun = 2, kPageDeveloper = 3, kPageTesting = 4, kPageChecks = 5, kPageSpawn = 6, kPageBeast = 7, kPageWrap = 8, kPageFeel = 9, kPageGrips = 10, kPageBody = 11,
       kPagePlayer = 12, kPageAccess = 13, kPageSystem = 14, kPagePlaces = 15, kPageStartup = 16, kPageControls = 17 };
static const int kSettingsTabs[] = { kPagePlayer, kPageBody, kPageComfort, kPageMovement, kPageGun, kPageAccess, kPageSystem };
static const int kDebugTabs[] = { kPageDeveloper, kPageTesting, kPageChecks, kPageSpawn, kPageBeast, kPagePlaces, kPageWrap, kPageFeel, kPageGrips };
static const int kSettingsTabCount = (int)(sizeof(kSettingsTabs) / sizeof(kSettingsTabs[0]));
static const int kDebugTabCount = (int)(sizeof(kDebugTabs) / sizeof(kDebugTabs[0]));
static const int kTabToDebug = -60, kTabToSettings = -61; // (the two tabs that change the set; a page's own tab is -1 - its number)
static const int kTitleControls = -62, kControlsBack = -63; // (the title line's "Controls", and the Controls page's "< Back")
static const char* const kTabToDebugName = "Debug";
static const char* const kTabToSettingsName = "< Back";
static const char* const kControlsName = "Controls";
static const int kMaxItems = 15; // (12 at the full row height; a page with more has shorter rows: see Draw)
static Item g_pages[kPageCount + 2][kMaxItems];
static int g_pageItemCount[kPageCount + 2] = {};

static bool IsDebugPage(int page)
{
    for (int i = 0; i < kDebugTabCount; ++i)
        if (kDebugTabs[i] == page) return true;
    return false;
}
static float g_startupHeight = 1.75f; // the start-up page's height picker (meters)
static bool g_built = false;

static void AddSwitch(int page, const char* label, bool* flag, const char* offName, const char* onName, int action = 0)
{
    Item item = {};
    item.label = label;
    item.kind = kSwitch;
    item.flag = flag;
    item.offName = offName;
    item.onName = onName;
    item.action = action;
    g_pages[page][g_pageItemCount[page]++] = item;
}

static void AddChoice(int page, const char* label, int* choice, const char* offName, const char* onName, int action = 0)
{
    Item item = {};
    item.label = label;
    item.kind = kSwitch;
    item.choice = choice;
    item.offName = offName;
    item.onName = onName;
    item.action = action;
    g_pages[page][g_pageItemCount[page]++] = item;
}

// One of three (0, 1, 2), the next at each press.
static void AddCycle(int page, const char* label, int* choice, const char* name0, const char* name1, const char* name2, int action = 0)
{
    Item item = {};
    item.label = label;
    item.kind = kCycle;
    item.choice = choice;
    item.offName = name0;
    item.onName = name1;
    item.thirdName = name2;
    item.action = action;
    g_pages[page][g_pageItemCount[page]++] = item;
}

// The same with a fourth name.
static void AddCycle4(int page, const char* label, int* choice, const char* name0, const char* name1, const char* name2, const char* name3)
{
    AddCycle(page, label, choice, name0, name1, name2);
    g_pages[page][g_pageItemCount[page] - 1].fourthName = name3;
}

// The same with two names only (Head Bob: Reduced or Off), where a switch's on and off do not fit.
static void AddCycle2(int page, const char* label, int* choice, const char* name0, const char* name1)
{
    AddCycle(page, label, choice, name0, name1, nullptr);
}

// How many choices a cycle has: two, three or four (thirdName and fourthName null or not).
static int CycleChoices(const Item& item)
{
    return item.fourthName ? 4 : (item.thirdName ? 3 : 2);
}

// (action: sent after the number changes, 0 = none.)
static void AddNumber(int page, const char* label, float* number, float step, float factor, float lowest, float highest,
                      const char* format, float shown = 1.0f, int action = 0)
{
    Item item = {};
    item.label = label;
    item.kind = kNumber;
    item.number = number;
    item.step = step;
    item.factor = factor;
    item.lowest = lowest;
    item.highest = highest;
    item.format = format;
    item.shown = shown;
    item.action = action;
    g_pages[page][g_pageItemCount[page]++] = item;
}

static void AddButton(int page, const char* label, int action)
{
    Item item = {};
    item.label = label;
    item.kind = kButtonItem;
    item.action = action;
    g_pages[page][g_pageItemCount[page]++] = item;
}

static void BuildPages()
{
    if (g_built) return;
    g_built = true;
    Settings& s = S();

    // ===== The settings (named and grouped as VR games do; the game's own
    // ===== VR SETTINGS pages, MenuHandler.hps, use the same names).

    // --- Player: how you play, your height and size, the view's center, and
    // --- the measuring (each step taken when a trigger is pulled). The line
    // --- below says how each is going.
    AddSwitch(kPagePlayer, "Play Mode", &s.seated, "Standing", "Seated", kMenuActionSeatedChanged);
    AddNumber(kPagePlayer, "Height Offset", &s.heightOffset, 0.025f, 0, -0.6f, 0.6f, "%+.1f cm", 100.0f);
    AddNumber(kPagePlayer, "World Scale", &s.worldScale, 0.02f, 0, 0.5f, 2.5f, "%.0f %%", 100.0f);
    AddButton(kPagePlayer, "Recenter View (or hold Y)", kMenuActionRecenter);
    AddButton(kPagePlayer, "Open Game Menu (pause)", kMenuActionGameMenu); // (on SteamVR the left menu button is not the game's)
    AddButton(kPagePlayer, "Calibrate Height (stand up straight, then pull a trigger)", kMenuActionMeasureSize);
    AddButton(kPagePlayer, "Calibrate Seated Height (sit as you play, then pull)", kMenuActionStartupSit);
    AddButton(kPagePlayer, "Calibrate Floor (rest a controller on the floor, then pull)", kMenuActionBodyFloor);
    AddButton(kPagePlayer, "Measure Arm Span (arms straight out, then pull)", kMenuActionBodyArms);
    AddButton(kPagePlayer, "Run Player Setup Again", kMenuActionStartupRun);
    AddButton(kPagePlayer, "Undo Calibration Changes", kMenuActionBodyUndo);

    // --- Holsters: where the holster, the ammo pouch and the flashlight are,
    // --- set by a hand there or a centimeter at a time; both hips are marked
    // --- while this page is up. The pouch's "Left" is its "right" the other
    // --- way round.
    AddButton(kPageBody, "Set Holster Position (right hand on your hip, then pull)", kMenuActionBodyHolster);
    AddNumber(kPageBody, "   Holster: Right", &s.holsterRight, 0.01f, 0, -0.30f, 0.60f, "%.0f cm", 100.0f, kMenuActionHolsterMoved);
    AddNumber(kPageBody, "   Holster: Forward", &s.holsterForward, 0.01f, 0, -0.30f, 0.60f, "%.0f cm", 100.0f, kMenuActionHolsterMoved);
    AddNumber(kPageBody, "   Holster: Below Eye Level", &s.holsterDown, 0.01f, 0, 0.10f, 1.20f, "%.0f cm", 100.0f, kMenuActionHolsterMoved);
    AddButton(kPageBody, "Set Ammo Pouch Position (left hand on your hip, then pull)", kMenuActionBodyPouch);
    AddNumber(kPageBody, "   Ammo Pouch: Left", &s.pouchRight, -0.01f, 0, -0.60f, 0.30f, "%.0f cm", -100.0f, kMenuActionPouchMoved);
    AddNumber(kPageBody, "   Ammo Pouch: Forward", &s.pouchForward, 0.01f, 0, -0.30f, 0.60f, "%.0f cm", 100.0f, kMenuActionPouchMoved);
    AddNumber(kPageBody, "   Ammo Pouch: Below Eye Level", &s.pouchDown, 0.01f, 0, 0.10f, 1.20f, "%.0f cm", 100.0f, kMenuActionPouchMoved);
    AddButton(kPageBody, "Set Flashlight Position (left hand on your chest, then pull)", kMenuActionBodyLight);
    AddButton(kPageBody, "Set Chest Item Slot Position (right hand on your chest, then pull)", kMenuActionBodySlot);
    AddButton(kPageBody, "Undo Calibration Changes", kMenuActionBodyUndo);

    // --- Comfort: turning, and how much the view is moved for you.
    AddChoice(kPageComfort, "Turn Style", &s.turnMode, "Smooth", "Snap");
    AddNumber(kPageComfort, "Snap Turn Angle", &s.snapAngle, 15.0f, 0, 15.0f, 90.0f, "%.0f deg");
    AddNumber(kPageComfort, "Smooth Turn Speed", &s.turnSpeed, 0, 1.15f, 200.0f, 20000.0f, "%.0f", 0.01f);
    // (The edges of the view darkened while the sticks move or turn you; beside the turning, as Meta's
    // guidelines group the comfort options. Its name says what it darkens, since in a Meta survey 40% of MR
    // and VR players didn't recognize the term "vignette".)
    AddCycle4(kPageComfort, "Movement Vignette (Darkens Edges)", &s.moveVignette, "Off", "Weak", "Medium", "Strong");
    AddCycle2(kPageComfort, "Head Bob", &s.viewSway, "Reduced", "Off"); // (see Settings::viewSway)
    AddCycle(kPageComfort, "Knockdown and Death View", &s.roughView, "No Comfort", "Cutscene Screen", "Vignette"); // (see Settings::roughView)
    AddCycle4(kPageComfort, "Knockdown Vignette Strength", &s.vignetteStrength, "None", "Weak", "Medium", "Strong"); // ("Knockdown" tells it from the movement vignette)
    // (There is no cutscene view choice: the game's cutscenes are always on the cinema screen,
    // Settings::cutsceneView.)
    AddSwitch(kPageComfort, "Screen Effects (Blood, Gas Mask)", &s.faceEffects, "Off", "On");

    // --- Movement
    AddChoice(kPageMovement, "Movement Direction", &s.moveDir, "Head", "Left Controller");
    AddSwitch(kPageMovement, "Movement Speed", &s.analogWalk, "Always Full", "Analog");
    AddSwitch(kPageMovement, "Jump (Right Thumbstick Up)", &s.jumpStick, "Off", "On");
    AddSwitch(kPageMovement, "Physical Crouch", &s.duckCrouch, "Off", "On");
    AddNumber(kPageMovement, "Crouch Depth: Seated", &s.duckSeated, 0.01f, 0, 0.05f, 0.40f, "%.0f cm", 100.0f);
    AddNumber(kPageMovement, "Crouch Depth: Standing", &s.duckStanding, 0.02f, 0, 0.10f, 0.80f, "%.0f cm", 100.0f);
    AddSwitch(kPageMovement, "Room-Scale Movement", &s.bodyFollow, "Off", "On");

    // --- Hands: the hands, the weapons and where their ammunition comes from.
    AddSwitch(kPageGun, "Finger Tracking", &s.handPoses, "Off", "On");
    AddSwitch(kPageGun, "Weapon Draw", &s.holster, "A Button", "Hip Holster");
    AddSwitch(kPageGun, "Ammo Pickup", &s.ammoPouch, "Anywhere", "Hip Pouch");
    AddChoice(kPageGun, "Ammo Counter", &s.ammoCounter, "On Ammo Pouch", "On Weapon");
    AddSwitch(kPageGun, "Weapon Collision", &s.gunPush, "Off", "On");
    AddNumber(kPageGun, "Weapon Yaw Offset (+ right)", &s.gunTurn, 1.0f, 0, -30.0f, 30.0f, "%+.0f deg");
    AddNumber(kPageGun, "Weapon Pitch Offset (+ up)", &s.gunTip, 1.0f, 0, -30.0f, 30.0f, "%+.0f deg");
    AddNumber(kPageGun, "Weapon Roll Offset (+ right)", &s.gunLean, 1.0f, 0, -45.0f, 45.0f, "%+.0f deg");
    AddNumber(kPageGun, "Elbow Movement", &s.elbowMove, 0.1f, 0, 0.0f, 2.0f, "%.1f x");

    // --- Accessibility: the hints on the objects, easier throwing and
    // --- reaching, the controllers' vibration and a steadier menu pointer.
    // --- (Wrapping the arm is the only way to heal; how many wraps is set in
    // --- the game's own VR menu, Accessibility, Bandage Wraps.)
    AddCycle(kPageAccess, "Interaction Hints", &s.interactionHints, "Until Learned", "Always", "Off");
    AddButton(kPageAccess, "Show Learned Hints Again", kMenuActionHintsReset);
    AddCycle4(kPageAccess, "Hint Size", &s.hintSize, "Small", "Normal", "Large", "Very Large"); // (Settings::hintSize)
    AddNumber(kPageAccess, "Throw Assist", &s.throwBoost, 0.1f, 0, 0.5f, 3.0f, "%.1f x");
    AddNumber(kPageAccess, "Grab Reach", &s.handReach, 0.05f, 0, 0.10f, 1.0f, "%.0f cm", 100.0f);
    AddCycle(kPageAccess, "Distance Grab", &s.pullMode, "Off", "Seated Only", "Always"); // (see Settings::pullMode)
    AddNumber(kPageAccess, "Distance Grab Range", &s.pullReach, 0.25f, 0, 0.5f, 6.0f, "%.2f m"); // (see Settings::pullReach)
    AddNumber(kPageAccess, "Controller Vibration", &s.buzzScale, 0.25f, 0, 0.0f, 1.5f, "%.0f %%", 100.0f);
    AddNumber(kPageAccess, "Menu Pointer Smoothing", &s.pointerSmooth, 0.05f, 0, 0.0f, 0.95f, "%.0f %%", 100.0f);

    // --- System: the game's menus in the headset, the view on the monitor,
    // --- the keyboard and mouse, and how the game is drawn and run.
    AddNumber(kPageSystem, "Menu Distance", &s.hudDistance, 0.1f, 0, 0.5f, 4.0f, "%.1f m");
    AddNumber(kPageSystem, "Menu Width", &s.hudWidth, 0.1f, 0, 0.5f, 4.0f, "%.1f m");
    AddNumber(kPageSystem, "Spectator Smoothing (0 = Off)", &s.monitorSmooth, 0.05f, 0, 0.0f, 1.0f, "%.2f s");
    AddNumber(kPageSystem, "Spectator Field of View", &s.monitorWidth, 5.0f, 0, 40.0f, 85.0f, "%.0f deg");
    AddSwitch(kPageSystem, "Keyboard and Mouse (Ctrl+F12)", &s.pcInput, "Off", "On");
    AddSwitch(kPageSystem, "Render Both Eyes Every Frame", &s.bothEyes, "Off", "On");
    AddSwitch(kPageSystem, "Game Update Rate", &s.logicAtHeadsetRate, "Monitor", "Headset");

    // ===== Debug: the pages for testing the mod.

    // --- Tuning
    AddNumber(kPageDeveloper, "Holster Zone Radius", &s.holsterRadius, 0.025f, 0, 0.05f, 0.6f, "%.1f cm", 100.0f);
    AddSwitch(kPageDeveloper, "Show Holster Zone", &g_showHolster, "Off", "On");
    AddNumber(kPageDeveloper, "Support Hand on Gun: Left", &s.supportLeft, 0.005f, 0, -0.15f, 0.15f, "%.1f cm", 100.0f);
    AddNumber(kPageDeveloper, "Support Hand on Gun: Down", &s.supportDown, 0.005f, 0, -0.15f, 0.15f, "%.1f cm", 100.0f);
    AddNumber(kPageDeveloper, "Support Hand on Gun: Forward", &s.supportForward, 0.005f, 0, -0.15f, 0.15f, "%.1f cm", 100.0f);
    AddNumber(kPageDeveloper, "Support Hand on Gun: Grip", &s.supportCurl, 0.05f, 0, 0.0f, 1.0f, "%.0f %%", 100.0f);
    AddSwitch(kPageDeveloper, "Game Arms on Controllers", &s.gameHands, "Off", "On");
    AddSwitch(kPageDeveloper, "Scale Body to Player", &s.bigBody, "Off", "On");
    AddSwitch(kPageDeveloper, "Paired Eye Frames (Experimental)", &s.pairFrames, "Off", "On");
    AddSwitch(kPageDeveloper, "Single-Frame Stereo", &s.stereoFrame, "Off", "On"); // (on by default, see Settings::stereoFrame)
    AddSwitch(kPageDeveloper, "Right Eye Reuses Left Eye's Shadows", &s.rightEyeShadows, "Off", "On"); // (scene.cpp RightEyeShadowsIn)
    AddNumber(kPageDeveloper, "Revolver Open Start", &s.revolverOpenStart, 0.05f, 0, 0.0f, 0.8f, "%.2f s"); // (at most 0.8: the game's message at 0.86 s)
    AddNumber(kPageDeveloper, "Revolver Close Start", &s.revolverCloseStart, 0.05f, 0, 0.0f, 0.6f, "%.2f s"); // (at most 0.6: its message at 0.64 s)
    AddButton(kPageDeveloper, "Capture Eye Images and Hand Data (F9)", kMenuActionCapture);

    // --- Bandage: how the wrap sits on the left forearm
    AddNumber(kPageWrap, "Arm Radius (larger = looser)", &s.bandageRadius, 0.0025f, 0, 0.02f, 0.10f, "%.2f cm", 100.0f);
    AddNumber(kPageWrap, "Wrap Offset A", &s.bandageShiftA, 0.0025f, 0, -0.08f, 0.08f, "%+.2f cm", 100.0f);
    AddNumber(kPageWrap, "Wrap Offset B", &s.bandageShiftB, 0.0025f, 0, -0.08f, 0.08f, "%+.2f cm", 100.0f);

    // --- Tools (each button asks the game's body script to do it)
    AddButton(kPageTesting, "Reload Scripts (no restart needed)", kMenuActionTestReload);
    AddButton(kPageTesting, "Take Damage (One Hit)", kMenuActionTestHurt);
    AddButton(kPageTesting, "Restore Full Health", kMenuActionTestHeal);
    AddButton(kPageTesting, "Give All Items", kMenuActionTestGiveAll);
    AddButton(kPageTesting, "Give Bandage and Medkit", kMenuActionTestHealthItems);
    AddButton(kPageTesting, "Give 2 Grenades and a Gas Grenade", kMenuActionTestGrenades);
    AddButton(kPageTesting, "Give Bullets and Shotgun Shells", kMenuActionTestAmmo);
    AddButton(kPageTesting, "Drain Flashlight", kMenuActionTestFlashlightEmpty);
    AddButton(kPageTesting, "Spawn the Beast in Front of You", kMenuActionTestBeast);
    AddButton(kPageTesting, "Remove the Beast", kMenuActionTestRemoveBeast);
    AddButton(kPageTesting, "Spawn All Furniture Ahead", kMenuActionTestProps);
    AddButton(kPageTesting, "Spawn Dog Tags, Wheel, Cutters and Fuse", kMenuActionSpawnFirst + 12);
    AddSwitch(kPageTesting, "Right Eye Skips the Query Wait", &s.rightEyeEarlierQueries, "Off", "On"); // (scene.cpp RightEyeQueriesIn; here because the Tuning page is full)

    // --- Test List: the test list, one check at a time (testlist.h).
    // --- These are placed by DrawChecksPage, not in rows: six, then the six
    // --- reasons asked for after a Fail (their words come from the list).
    AddButton(kPageChecks, "Go to Test Location", kMenuActionCheckJump);
    AddButton(kPageChecks, "Pass", kMenuActionCheckPass);
    AddButton(kPageChecks, "Fail", kMenuActionCheckFail);
    AddButton(kPageChecks, "Skip", kMenuActionCheckSkip);
    AddButton(kPageChecks, "< Previous", kMenuActionCheckBack);
    AddButton(kPageChecks, "Next >", kMenuActionCheckNext);
    for (int i = 0; i < kTestListWhyButtons; ++i) AddButton(kPageChecks, "", kMenuActionCheckWhyFirst + i);

    // --- The start-up page: three buttons, their words from vr.cpp
    // --- (VrStartupView), and the height picker. Placed by DrawStartupPage.
    for (int i = 0; i < 3; ++i) AddButton(kPageStartup, "", kMenuActionStartupButton + i);
    AddNumber(kPageStartup, "Height", &g_startupHeight, 0.01f, 0, 1.20f, 2.20f, "%.0f cm", 100.0f);

    // --- Spawn: one piece of the game's working furniture at a time, two
    // --- meters ahead (the body script makes them, see VRDoTestCommand).
    static const char* const kSpawnNames[12] = { "Wooden Door", "Metal Door", "Locker", "Half Locker", "Lockbox", "Generator",
                                                 "Fuse Box", "Power Switch", "Lockdown Wheel", "Water Pump", "Door Chain", "Fuel Can" };
    for (int i = 0; i < 12; ++i) AddButton(kPageSpawn, kSpawnNames[i], kMenuActionSpawnFirst + i);

    // --- Beast: knockdowns, the Beast's kills and deaths, to see how each
    // --- looks in VR (the body script plays each the game's own way,
    // --- VRBeastTestStart; the menu closes itself and it starts at once). How
    // --- they are seen is the first line (also on the Comfort page).
    AddCycle(kPageBeast, "Knockdown and Death View", &s.roughView, "No Comfort", "Cutscene Screen", "Vignette");
    AddButton(kPageBeast, "Spawn the Beast in Front of You", kMenuActionTestBeast);
    AddButton(kPageBeast, "Remove the Beast", kMenuActionTestRemoveBeast);
    AddButton(kPageBeast, "Knockdown (Facing the Beast)", kMenuActionBeastThrown);
    AddButton(kPageBeast, "Knockdown (From Behind)", kMenuActionBeastThrownBehind);
    AddButton(kPageBeast, "Heavy Fall (Recoverable)", kMenuActionBeastFall);
    AddButton(kPageBeast, "Shotgun Hit", kMenuActionBeastShotgun);
    AddButton(kPageBeast, "Beast Kill (From Behind)", kMenuActionBeastKillBehind);
    AddButton(kPageBeast, "Beast Kill (From the Front)", kMenuActionBeastKillFront);
    AddButton(kPageBeast, "Standard Death", kMenuActionBeastDie);
    AddButton(kPageBeast, "Beast Kill (Through a Hole)", kMenuActionBeastKillHole); // (the nearest hole)
    AddButton(kPageBeast, "Beast Kill (While Hiding)", kMenuActionBeastKillHiding);
    AddButton(kPageBeast, "Beast Kill (On a Ladder)", kMenuActionBeastKillLadder);

    // --- Places: for visiting parts of the game's own levels safely, to show or try a mechanic
    // --- there: the Beast kept out of the level (VrBeastAway: the body script switches it off,
    // --- and on again), the Tools page's items (with bullets and shells), and places in the
    // --- levels to go to (a jump, testlist.h: nothing is saved after it until a game is started
    // --- or loaded). The Beast page's kills still play with the Beast kept away (its stand-in).
    // --- (14 rows: 15 do not fit the page's lettering.)
    AddSwitch(kPagePlaces, "The Beast", &g_beastAway, "In the Level", "Kept Away");
    AddButton(kPagePlaces, "Give All Items and Ammo", kMenuActionTestGiveAll);
    for (int i = 0; i < VrPlaceCount() && kMenuActionPlaceFirst + i <= kMenuActionPlaceLast; ++i)
        AddButton(kPagePlaces, VrPlaceName(i), kMenuActionPlaceFirst + i);

    // --- Physics: how held things feel, tried while holding them (the
    // --- game's scripts read these on every update; see Settings).
    AddSwitch(kPageFeel, "Held Object Weight", &s.feelWeight, "Off", "On");
    AddNumber(kPageFeel, "Hand Stiffness (Position)", &s.feelHandStiffness, 0, 1.15f, 200.0f, 10000.0f, "%.0f");
    AddNumber(kPageFeel, "Wrist Stiffness (Rotation)", &s.feelWristStiffness, 0, 1.15f, 5.0f, 400.0f, "%.0f");
    AddNumber(kPageFeel, "Light Object Follow (Position)", &s.feelMoveQuickest, 2.0f, 0, 10.0f, 60.0f, "%.0f");
    AddNumber(kPageFeel, "Light Object Follow (Rotation)", &s.feelTurnQuickest, 2.0f, 0, 10.0f, 60.0f, "%.0f");
    AddNumber(kPageFeel, "Weight Force", &s.feelWeightShown, 0.1f, 0, 0.0f, 2.0f, "%.0f%%", 100.0f); // (applied as a force)
    AddNumber(kPageFeel, "Wrist Strength (lower = heavier)", &s.feelWristStrength, 1.0f, 0, 1.0f, 60.0f, "%.0f N m");
    AddNumber(kPageFeel, "Steady Hand Threshold", &s.feelSteadyBelow, 0.1f, 0, 0.0f, 4.0f, "%.1f rad/s");
    AddSwitch(kPageFeel, "Lock Hand to Held Object", &s.feelHandOnThing, "Off", "On");
    AddSwitch(kPageFeel, "Even Smooth Turn While Holding", &s.feelEvenTurn, "Off", "On");
    AddNumber(kPageFeel, "Throw Speed Threshold", &s.feelThrowSpeed, 0.1f, 0, 0.5f, 4.0f, "%.1f m/s"); // (see Settings::feelThrowSpeed)
    AddButton(kPageFeel, "Restore Tested Defaults", kMenuActionFeelDefaults);

    // --- Grips: how the item in a hand sits in it (see grips.h).
    // --- The numbers are the held item's own grip, or where the game holds
    // --- it until one is changed; in the hand's own directions.
    GripView& v = GripsView();
    AddChoice(kPageGrips, "Hand", &v.hand, "Right", "Left", kMenuActionGripHand);
    AddSwitch(kPageGrips, "Adjust with Other Hand", &v.editing, "Off", "On", kMenuActionGripEditing);
    AddSwitch(kPageGrips, "Mirror to Desktop Window", &v.onPc, "Off", "On", kMenuActionGripWindow); // (the hand poses tool; also F11)
    // (50 cm each way, the limits grips.cpp keeps them to.)
    AddNumber(kPageGrips, "Forward", &v.forward, 0.005f, 0, -0.5f, 0.5f, "%+.1f cm", 100.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Up", &v.up, 0.005f, 0, -0.5f, 0.5f, "%+.1f cm", 100.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Right", &v.right, 0.005f, 0, -0.5f, 0.5f, "%+.1f cm", 100.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Yaw (+ right)", &v.turnRight, 5.0f, 0, -180.0f, 180.0f, "%+.0f deg", 1.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Pitch (+ up)", &v.tipUp, 5.0f, 0, -89.0f, 89.0f, "%+.0f deg", 1.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Roll (+ right)", &v.leanRight, 5.0f, 0, -180.0f, 180.0f, "%+.0f deg", 1.0f, kMenuActionGripChanged);
    AddSwitch(kPageGrips, "Finger Pose", &v.fingersSet, "Game Default", "Custom", kMenuActionGripChanged);
    AddNumber(kPageGrips, "Thumb Curl", &v.thumb, 0.05f, 0, 0.0f, 1.0f, "%.0f %%", 100.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Index Finger Curl", &v.first, 0.05f, 0, 0.0f, 1.0f, "%.0f %%", 100.0f, kMenuActionGripChanged);
    AddNumber(kPageGrips, "Other Fingers Curl", &v.others, 0.05f, 0, 0.0f, 1.0f, "%.0f %%", 100.0f, kMenuActionGripChanged);
    AddButton(kPageGrips, "Reset to Game Default", kMenuActionGripReset);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool g_open = false;
static int g_page = kPagePlayer;
static int g_lastSettingsPage = kPagePlayer; // the page each set of tabs comes back to (Debug, < Back)
static int g_lastDebugPage = kPageDeveloper;
static bool g_dirty = true;
static const VrStartupView* g_startupView = nullptr; // the start-up page, while it is up
static int g_pageBeforeStartup = 0;
static int g_pageBeforeControls = kPagePlayer; // the page the Controls page goes back to
static bool g_drawnSeeThrough = false; // the picture last drawn has see-through parts (the start-up page's own look)

void VrSetBeastAway(bool away)
{
    g_beastAway = away; // (the switch's own place: VrBeastAway still holds it to a jumped-to level)
    g_dirty = true;
}

static bool LoadGameFont();
static void DrawStartupStyled();
static char g_info[96] = "";
static char g_comfortInfo[96] = "";
static void (*g_actionHandler)(int) = nullptr;

// The areas that react to the pointer, noted while drawing.
struct HotArea
{
    int x0, y0, x1, y1;
    int item;      // index on the page, or -1 - page number for a tab
    int direction; // -1 or +1 for a number's buttons, 0 otherwise
};
static HotArea g_hot[64];
static int g_hotCount = 0;
static int g_hover = -1;       // index into g_hot the hand points at, or -1
static int g_hoverItem = -100; // the same, as item and direction (survives a redraw)
static int g_hoverDirection = 0;
static bool g_wasSqueezing = false;
static double g_nextRepeatMs = 0;
static int g_hoverCount = 0;              // up by one each time the pointer comes onto something to press
static double g_lastPressMs = -1000000.0; // when something was last pressed

void VrMenuSetOpen(bool open)
{
    if (open == g_open) return;
    g_open = open;
    g_dirty = true;
    if (open)
    {
        // A squeeze held from before it opened is no press here (the start-up
        // page can open from a click on the game's VR settings, the trigger
        // still down).
        g_wasSqueezing = true;
        // The test list as it is now. With checks still to answer, the first
        // opening of a run comes up on them. (Not the start-up page's opening:
        // the first one after it.)
        TestListLoad();
        static bool openedBefore = false;
        if (!openedBefore && !g_startupView)
        {
            openedBefore = true;
            int passed, failed, skipped, notDone;
            TestListCounts(passed, failed, skipped, notDone);
            if (notDone > 0) g_page = kPageChecks;
        }
    }
    g_hoverItem = -100;
    g_wasSqueezing = true; // a squeeze already held when it opens does not count
}

bool VrMenuIsOpen()
{
    return g_open;
}

bool VrMenuShowHolster()
{
    return g_showHolster;
}

bool VrMenuOnBodyPage()
{
    return g_open && g_page == kPageBody && !g_startupView;
}

bool VrMenuOnControlsPage()
{
    return g_open && g_page == kPageControls && !g_startupView;
}

void VrMenuSetActionHandler(void (*handler)(int action))
{
    g_actionHandler = handler;
}

void VrMenuSetInfo(const char* text)
{
    if (strncmp(g_info, text, sizeof(g_info) - 1) == 0) return;
    snprintf(g_info, sizeof(g_info), "%s", text);
    if (g_open && g_page == kPageDeveloper) g_dirty = true;
}

void VrMenuShowPage(int page)
{
    if (page < 0 || (page >= kPageCount && page != kPageControls)) return;
    if (page == kPageControls && g_page != kPageControls) g_pageBeforeControls = g_page;
    g_page = page;
    g_dirty = true;
}

void VrMenuSetComfortInfo(const char* text)
{
    if (strncmp(g_comfortInfo, text, sizeof(g_comfortInfo) - 1) == 0) return;
    snprintf(g_comfortInfo, sizeof(g_comfortInfo), "%s", text);
    if (g_open && (g_page == kPagePlayer || g_page == kPageBody || g_page == kPageStartup)) g_dirty = true; // (with the page's numbers: the world scale may have changed)
}

bool VrMenuOnGripsPage()
{
    return g_page == kPageGrips;
}

void VrMenuGripsChanged()
{
    if (g_open && g_page == kPageGrips) g_dirty = true;
}

void VrMenuSetStartup(const VrStartupView* view)
{
    BuildPages();
    if (view && !g_startupView) g_pageBeforeStartup = g_page;
    if (view) g_page = kPageStartup;
    else if (g_startupView) g_page = g_pageBeforeStartup;
    g_startupView = view;
    g_hoverItem = -100;
    g_dirty = true;
}

float& VrMenuStartupHeight()
{
    return g_startupHeight;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static const int kTabsTop = 64, kTabsBottom = 112;
static const int kRowsTop = 122, kRowHeight = 44; // twelve rows fit above the footer
static const int kMargin = 24;
// The footer: two lines of words under the rows (what the measuring says can
// be longer than one line), and the room for the rows.
static const int kFooterTop = kHeight - 80, kFooterStep = 32;
static const int kRowsRoom = kFooterTop - 6 - kRowsTop;
// A row's value: a switch's or a cycle's button, or a number between - and +.
static const int kValueWidth = 250, kNumberTextWidth = 130;

// The tabs of one set: the settings' pages and "Debug" after
// them, or "< Back" and the Debug pages; where each goes is worked out here
// for both Draw and VrMenuLayoutProblems.
struct TabRow
{
    int count;
    const char* names[16];
    int items[16];  // as the hot areas have them: -1 - page, or kTabToDebug / kTabToSettings
    int widths[16]; // each name's width and its share of the room left over
    int gap;
    int namesTotal; // the names alone, for VrMenuLayoutProblems
};

static void GatherTabs(bool debug, TabRow& row)
{
    row.count = 0;
    if (debug)
    {
        row.names[row.count] = kTabToSettingsName;
        row.items[row.count++] = kTabToSettings;
    }
    const int* pages = debug ? kDebugTabs : kSettingsTabs;
    int pageCount = debug ? kDebugTabCount : kSettingsTabCount;
    for (int i = 0; i < pageCount; ++i)
    {
        row.names[row.count] = kPageNames[pages[i]];
        row.items[row.count++] = -1 - pages[i];
    }
    if (!debug)
    {
        row.names[row.count] = kTabToDebugName;
        row.items[row.count++] = kTabToDebug;
    }
    row.namesTotal = 0;
    for (int i = 0; i < row.count; ++i) row.namesTotal += TextWidth(row.names[i]);
    const int room = kWidth - 2 * kMargin;
    row.gap = (room - row.namesTotal) / (row.count - 1);
    if (row.gap > 8) row.gap = 8;
    if (row.gap < 2) row.gap = 2;
    int spare = (room - (row.count - 1) * row.gap - row.namesTotal) / row.count;
    if (spare < 0) spare = 0;
    for (int i = 0; i < row.count; ++i) row.widths[i] = TextWidth(row.names[i]) + spare;
}

static int RowHeight(int page)
{
    int count = g_pageItemCount[page];
    return count * kRowHeight > kRowsRoom ? kRowsRoom / count : kRowHeight;
}

// What a switch or a cycle shows.
static const char* ValueName(const Item& item)
{
    if (item.kind == kSwitch)
    {
        bool on = item.flag ? *item.flag : (*item.choice != 0);
        return on ? item.onName : item.offName;
    }
    int which = *item.choice;
    return which == 3 && item.fourthName ? item.fourthName : (which == 2 && item.thirdName ? item.thirdName : (which == 1 ? item.onName : item.offName));
}

// A number as its row shows it.
static void NumberText(const Item& item, float number, char* text, size_t size)
{
    snprintf(text, size, item.format, (double)(number * item.shown));
    // Avoid showing "-0".
    if (strncmp(text, "-0 ", 3) == 0 || strncmp(text, "-0.0 ", 5) == 0) memmove(text, text + 1, strlen(text));
}

// The title line's controls: on the start-up page, and on the others.
static const char* const kHints[2] = { "Trigger: Select     Y: Skip", "Trigger: Select     Y: Close" };

// The words under a page's rows.
static const char* const kChecksFooters[2] = { "Fail saves a marker, the last 2 minutes of motion and both eyes.",
                                               "Autosave is off after a jump, until a new game is started." };

static const char* PageFooter(int page)
{
    switch (page)
    {
    case kPageDeveloper: return g_info;
    case kPageChecks: return kChecksFooters[TestJumpedThisRun() ? 1 : 0];
    case kPageTesting:
    case kPageSpawn: return "Close the menu to see the result.";
    case kPageBeast: return "The menu closes and the sequence starts immediately.";
    case kPageFeel: return "Applies immediately to held objects. Saved automatically.";
    case kPageGrips: return GripsFooter();
    case kPageBody: return g_comfortInfo[0] ? g_comfortInfo : "The markers at your hips show the holster and the ammo pouch.";
    case kPagePlayer: return g_comfortInfo[0] ? g_comfortInfo : "Calibration is saved automatically.";
    default: return "Changes are saved automatically.";
    }
}

static void AddHot(int x0, int y0, int x1, int y1, int item, int direction)
{
    if (g_hotCount >= 64) return;
    HotArea& hot = g_hot[g_hotCount++];
    hot.x0 = x0;
    hot.y0 = y0;
    hot.x1 = x1;
    hot.y1 = y1;
    hot.item = item;
    hot.direction = direction;
}

// A button: a filled rectangle with centered text, lit while pointed at.
static void DrawButton(int x0, int y0, int x1, int y1, const char* text, int item, int direction, Colour colour = kButton)
{
    bool hot = g_hoverItem == item && g_hoverDirection == direction;
    FillRect(x0, y0, x1, y1, hot ? kButtonHot : colour);
    DrawStringCentred(x0, x1, (y0 + y1 - kFontHeight) / 2, text, hot ? kTextDark : kText);
    AddHot(x0, y0, x1, y1, item, direction);
}

// The Checks page: the check shown, with big buttons under it.
static void DrawChecksPage()
{
    const int left = kMargin + 8, right = kWidth - kMargin - 8;
    const TestCheck* check = TestListCurrent();
    if (!check)
    {
        DrawString(left, kRowsTop + 8, "No tests found (BunkerVR\\test_list.txt is missing).", kTextDim);
        return;
    }

    int passed, failed, skipped, notDone;
    TestListCounts(passed, failed, skipped, notDone);
    char line[96];
    snprintf(line, sizeof(line), "Test %d of %d (%s)", TestListAt() + 1, TestListCount(), check->id);
    DrawString(left, kRowsTop + 2, line, kAccent);
    snprintf(line, sizeof(line), "%d passed, %d failed, %d skipped, %d left", passed, failed, skipped, notDone);
    DrawString(right - TextWidth(line), kRowsTop + 2, line, kTextDim);

    // What to do. (A check's words must fit in kLinesRoom lines; more are cut.)
    static const int kLinesRoom = 8, kLineStep = 33;
    const int boxTop = kRowsTop + 36;
    FillRect(kMargin, boxTop, kWidth - kMargin, boxTop + 16 + kLinesRoom * kLineStep, kRowA);
    char lines[kLinesRoom][kTestListLineChars];
    int lineCount = TestListWrap(check->text, right - left, TextWidth, lines, kLinesRoom);
    if (lineCount > kLinesRoom) lineCount = kLinesRoom;
    for (int i = 0; i < lineCount; ++i) DrawString(left, boxTop + 8 + i * kLineStep, lines[i], kText);

    int y = boxTop + 16 + kLinesRoom * kLineStep + 6;

    // After a Fail: why? Six buttons in place of the others, so that a failure
    // can be explained without typing.
    if (TestListAskingWhy())
    {
        DrawString(left, y, "Result saved. What went wrong?", kAccent);
        y += 34;
        const int half = (right - (kMargin + 8)) / 2;
        for (int i = 0; i < kTestListWhyButtons; ++i)
        {
            const char* words = TestListWhyChoice(i);
            if (words[0] == '\0') continue;
            int x0 = i % 2 == 0 ? kMargin + 8 : kMargin + 8 + half + 4;
            int x1 = i % 2 == 0 ? kMargin + 8 + half - 4 : right;
            int top = y + (i / 2) * 66;
            DrawButton(x0, top, x1, top + 58, words, 6 + i, 0, i == kTestListWhyButtons - 1 ? kButton : kButtonFail);
        }
        return;
    }

    const char* said = check->result == kCheckPassed ? "Result: Passed" : (check->result == kCheckFailed ? "Result: Failed" : (check->result == kCheckSkipped ? "Result: Skipped" : "Not tested yet"));
    DrawString(left, y, said, check->result == kCheckNotDone ? kTextDim : kAccent);

    y += 34;
    if (TestJumping()) DrawStringCentred(kMargin + 8, right, y + 14, "Loading the test location...", kAccent);
    else if (check->jump[0] != '\0') DrawButton(kMargin + 8, y, right, y + 56, g_pages[kPageChecks][0].label, 0, 0);
    else DrawStringCentred(kMargin + 8, right, y + 14, "Test this where you are", kTextDim);

    y += 64;
    const int third = (right - (kMargin + 8)) / 3;
    DrawButton(kMargin + 8, y, kMargin + 8 + third - 4, y + 70, g_pages[kPageChecks][1].label, 1, 0, kButtonPass);
    DrawButton(kMargin + 8 + third + 4, y, kMargin + 8 + 2 * third - 4, y + 70, g_pages[kPageChecks][2].label, 2, 0, kButtonFail);
    DrawButton(kMargin + 8 + 2 * third + 4, y, right, y + 70, g_pages[kPageChecks][3].label, 3, 0);

    y += 78;
    const int half = (right - (kMargin + 8)) / 2;
    DrawButton(kMargin + 8, y, kMargin + 8 + half - 4, y + 56, g_pages[kPageChecks][4].label, 4, 0);
    DrawButton(kMargin + 8 + half + 4, y, right, y + 56, g_pages[kPageChecks][5].label, 5, 0);
}

// The start-up page, in place of the tabs and rows: the step's
// heading, what to do, a dimmer note, the height picker if asked for, and up
// to three big buttons side by side.
static void DrawStartupPage()
{
    const VrStartupView& view = *g_startupView;
    const int left = kMargin + 8, right = kWidth - kMargin - 8;
    DrawString(left, (kTabsTop + kTabsBottom - kFontHeight) / 2 - 2, view.title ? view.title : "", kAccent);
    FillRect(kMargin, kTabsBottom - 4, kWidth - kMargin, kTabsBottom, kAccent);

    static const int kLinesRoom = 6, kLineStep = 33;
    const int boxTop = kRowsTop;
    FillRect(kMargin, boxTop, kWidth - kMargin, boxTop + 16 + kLinesRoom * kLineStep, kRowA);
    char lines[kLinesRoom][kTestListLineChars];
    int lineCount = TestListWrap(view.text ? view.text : "", right - left, TextWidth, lines, kLinesRoom);
    if (lineCount > kLinesRoom) lineCount = kLinesRoom;
    for (int i = 0; i < lineCount; ++i) DrawString(left, boxTop + 8 + i * kLineStep, lines[i], kText);

    int y = boxTop + 16 + kLinesRoom * kLineStep + 10;
    char noteLines[2][kTestListLineChars];
    int noteCount = TestListWrap(view.note ? view.note : "", right - left, TextWidth, noteLines, 2);
    if (noteCount > 2) noteCount = 2;
    for (int i = 0; i < noteCount; ++i) DrawString(left, y + i * kLineStep, noteLines[i], kTextDim);
    y += 2 * kLineStep + 8;

    if (view.heightPicker)
    {
        const Item& item = g_pages[kPageStartup][3];
        FillRect(kMargin, y, kWidth - kMargin, y + 64, kRowB);
        int textY = y + (64 - kFontHeight) / 2;
        DrawString(kMargin + 16, textY, item.label, kText);
        char value[32];
        snprintf(value, sizeof(value), item.format, (double)(*item.number * item.shown));
        DrawButton(right - 330, y + 6, right - 250, y + 58, "-", 3, -1);
        DrawStringCentred(right - 250, right - 80, textY, value, kText);
        DrawButton(right - 80, y + 6, right, y + 58, "+", 3, +1);
    }
    y += 76;

    int count = 0;
    for (int i = 0; i < 3; ++i)
        if (view.buttons[i] && view.buttons[i][0]) ++count;
    if (count == 0) return;
    const int gap = 8, width = (right - left - (count - 1) * gap) / count;
    int x = left;
    for (int i = 0; i < 3; ++i)
    {
        if (!view.buttons[i] || !view.buttons[i][0]) continue;
        g_pages[kPageStartup][i].label = view.buttons[i];
        DrawButton(x, y, x + width, y + 76, view.buttons[i], i, 0);
        x += width + gap;
    }
}

// The Controls page: the whole controls layout on one page, as VR games show it.
// Both controllers side by side, then the places on the body and what the hands
// do, as the settings have them now (Weapon Draw, Jump, Distance Grab).
struct ControlLine
{
    const char* control;
    const char* action;
};
static const int kControlsRow = 31;
static const int kControlsColumn = (kWidth - 2 * kMargin - 24) / 2; // each half's width
static const int kControlsNameRoom = 168;                           // the control's name, then what it does
static int ControlsNameRoom(int part) { return part == 2 ? 196 : kControlsNameRoom; } // (the body's places are longer words)
static const char* const kControlsFooters[2] = { "On the body: squeeze the grip there to take a thing out or put it back.",
                                                 "Testing: hold the left stick in and click the right stick for a marker." };

// Is the left menu button the game's? See VrMenuSetGameMenuButton.
static bool g_gameMenuButton = true;

void VrMenuSetGameMenuButton(bool works)
{
    if (g_gameMenuButton == works) return;
    g_gameMenuButton = works;
    g_dirty = true;
}

// One part's lines: 0 the left controller, 1 the right, 2 on the body, 3 with
// the hands. `holster`: the Hands page's Weapon Draw is Hip Holster; `jump`: the
// right stick up jumps; `pull`: Distance Grab is on as the player plays now.
// (Holding Y re-centers on every headset; the left menu button pauses only
// where it is the game's, and on SteamVR the Player tab's Open Game Menu does.)
static int ControlLines(int part, bool holster, bool jump, bool pull, ControlLine* out)
{
    int n = 0;
    auto add = [&](const char* control, const char* action) {
        out[n].control = control;
        out[n].action = action;
        ++n;
    };
    switch (part)
    {
    case 0:
        add("Stick", "Move");
        add("Click stick", "Sprint");
        add("Grip, Trigger", "Grab, use");
        add("X", "Put item away");
        add("Y", "This menu");
        add("Hold Y", "Recenter view");
        if (g_gameMenuButton) add("Menu", "Pause");
        else add("Pause", "Y, Player tab");
        break;
    case 1:
        add("Stick", "Turn");
        add("Stick up", jump ? "Jump, stand up" : "Stand up");
        add("Stick down", "Crouch");
        add("Click stick", "Flashlight");
        add("Grip, Trigger", "Grab, use, fire");
        if (!holster) add("A", "Draw revolver");
        add("Hold A, flick", "Open revolver");
        add("B", "Inventory");
        add("Hold B", "Check injuries");
        break;
    case 2:
        if (holster) add("Right hip", "Revolver");
        add("Left hip", "Bullets, shells");
        add("Left chest", "Flashlight");
        add("Right chest", "Stored item"); // (the item slot for any one item, the game's hotkey 1)
        add("Behind shoulder", "Shotgun");
        break;
    default:
        add("Handles", "Grip, pull or push");
        add("Doors", "Push, open hand");
        add("Throwing", "Swing, let go");
        if (pull) add("Far items", "Point, grip, flick");
        break;
    }
    return n;
}

static bool ControlsPullOn()
{
    return S().pullMode == 2 || (S().pullMode == 1 && S().seated);
}

static void DrawControlsPage()
{
    // The tab row: "< Back" and the page's name.
    int backWidth = TextWidth(kTabToSettingsName) + 40;
    DrawButton(kMargin, kTabsTop, kMargin + backWidth, kTabsBottom - 4, kTabToSettingsName, kControlsBack, 0);
    DrawString(kMargin + backWidth + 24, (kTabsTop + kTabsBottom - kFontHeight) / 2 - 2, kControlsName, kText);
    FillRect(kMargin, kTabsBottom - 4, kWidth - kMargin, kTabsBottom, kAccent);

    static const char* const kHeads[4] = { "LEFT CONTROLLER", "RIGHT CONTROLLER", "ON YOUR BODY", "WITH YOUR HANDS" };
    ControlLine lines[4][12];
    int counts[4];
    for (int part = 0; part < 4; ++part) counts[part] = ControlLines(part, S().holster, S().jumpStick, ControlsPullOn(), lines[part]);
    int controllerRows = counts[0] > counts[1] ? counts[0] : counts[1];
    int tops[2] = { kRowsTop + 6, kRowsTop + 6 + (controllerRows + 1) * kControlsRow + 14 };
    for (int part = 0; part < 4; ++part)
    {
        int x = kMargin + (part % 2) * (kControlsColumn + 24);
        int top = tops[part / 2];
        FillRect(x, top - 4, x + kControlsColumn, top + (1 + (part < 2 ? controllerRows : (counts[2] > counts[3] ? counts[2] : counts[3]))) * kControlsRow, (part % 2) ? kRowB : kRowA);
        DrawString(x + 10, top, kHeads[part], kAccent);
        for (int i = 0; i < counts[part]; ++i)
        {
            int y = top + (i + 1) * kControlsRow;
            DrawString(x + 10, y, lines[part][i].control, kTextDim);
            DrawString(x + 10 + ControlsNameRoom(part), y, lines[part][i].action, kText);
        }
    }
    for (int i = 0; i < 2; ++i) DrawString(kMargin, kFooterTop + i * kFooterStep, kControlsFooters[i], kTextDim);
}

static void Draw()
{
    BuildPages();
    g_hotCount = 0;

    // The start-up page in the game's own look, when its lettering can be read.
    g_drawnSeeThrough = false;
    if (g_startupView && g_page == kPageStartup && LoadGameFont())
    {
        g_drawnSeeThrough = true;
        DrawStartupStyled();
        return;
    }

    FillRect(0, 0, kWidth, kHeight, kBackground);

    // Title, and the controls (as VR games show them, "Trigger: Select").
    DrawString(kMargin, 20, "BunkerVR", kAccent);
    bool startup = g_startupView && g_page == kPageStartup;
    const char* hint = kHints[startup ? 0 : 1];
    DrawString(kWidth - kMargin - TextWidth(hint), 20, hint, kTextDim);

    // The start-up page: no tabs, no rows; the Player page's line below.
    if (startup)
    {
        DrawStartupPage();
        DrawString(kMargin, kHeight - 44, g_comfortInfo, kTextDim);
        return;
    }

    // The title line's "Controls", left of the controls' words; lit
    // while its page is up. The page instead of the tabs and rows.
    {
        int x1 = kWidth - kMargin - TextWidth(hint) - 28;
        int x0 = x1 - TextWidth(kControlsName) - 40;
        DrawButton(x0, 12, x1, 54, kControlsName, kTitleControls, 0, g_page == kPageControls ? kRowB : kButton);
        if (g_page == kPageControls)
        {
            FillRect(x0, 50, x1, 54, kAccent);
            DrawControlsPage();
            return;
        }
    }

    // Tabs: the set the page is in, each as wide as its name, with an even
    // share of the room left over, the gap narrowed when the names need the
    // room.
    if (IsDebugPage(g_page)) g_lastDebugPage = g_page;
    else g_lastSettingsPage = g_page;
    TabRow tabs;
    GatherTabs(IsDebugPage(g_page), tabs);
    int tabX = kMargin;
    for (int i = 0; i < tabs.count; ++i)
    {
        int tabWidth = tabs.widths[i];
        int x0 = tabX;
        tabX += tabWidth + tabs.gap;
        int item = tabs.items[i];
        bool hot = g_hoverItem == item;
        bool current = item == -1 - g_page;
        FillRect(x0, kTabsTop, x0 + tabWidth, kTabsBottom, hot ? kButtonHot : (current ? kRowB : kBackground));
        if (current) FillRect(x0, kTabsBottom - 4, x0 + tabWidth, kTabsBottom, kAccent);
        DrawStringCentred(x0, x0 + tabWidth, (kTabsTop + kTabsBottom - kFontHeight) / 2 - 2, tabs.names[i],
                        hot ? kTextDark : (current ? kText : kTextDim));
        AddHot(x0, kTabsTop, x0 + tabWidth, kTabsBottom, item, 0);
    }

    // Rows (a page of more than twelve has them a little shorter, to fit above
    // the footer: the Grips page's fourteen, 38 pixels each, for 27-pixel text)
    int rowHeight = RowHeight(g_page);
    int inset = rowHeight >= kRowHeight ? 6 : 4;
    if (g_page == kPageChecks) DrawChecksPage();
    else for (int i = 0; i < g_pageItemCount[g_page]; ++i)
    {
        const Item& item = g_pages[g_page][i];
        int y0 = kRowsTop + i * rowHeight;
        int y1 = y0 + rowHeight;
        FillRect(kMargin, y0, kWidth - kMargin, y1, (i & 1) ? kRowB : kRowA);

        int textY = (y0 + y1 - kFontHeight) / 2;
        int right = kWidth - kMargin - 8;

        if (item.kind == kButtonItem)
        {
            DrawButton(kMargin + 8, y0 + inset, right, y1 - inset, item.label, i, 0);
            continue;
        }

        DrawString(kMargin + 16, textY, item.label, kText);

        if (item.kind == kSwitch || item.kind == kCycle)
        {
            DrawButton(right - kValueWidth, y0 + inset, right, y1 - inset, ValueName(item), i, 0);
        }
        else
        {
            char value[32];
            NumberText(item, *item.number, value, sizeof(value));
            DrawButton(right - kValueWidth, y0 + inset, right - kValueWidth + 60, y1 - inset, "-", i, -1);
            DrawStringCentred(right - kValueWidth + 60, right - 60, textY, value, kText);
            DrawButton(right - 60, y0 + inset, right, y1 - inset, "+", i, +1);
        }
    }

    // Footer: up to two lines (the Test List page: one, under its buttons).
    const char* footer = PageFooter(g_page);
    if (g_page == kPageChecks)
    {
        DrawString(kMargin, kHeight - 44, footer, kTextDim);
        return;
    }
    char lines[2][kTestListLineChars];
    int lineCount = TestListWrap(footer, kWidth - 2 * kMargin, TextWidth, lines, 2);
    if (lineCount > 2) lineCount = 2;
    for (int i = 0; i < lineCount; ++i)
        DrawString(kMargin, kFooterTop + (lineCount == 1 ? kFooterStep / 2 : i * kFooterStep), lines[i], kTextDim);
}

// For the tests: every page's words measured against the room Draw gives
// them.
int VrMenuLayoutProblems(void (*report)(const char* what))
{
    BuildPages();
    int problems = 0;
    char what[256];
    auto problem = [&](const char* text) {
        ++problems;
        if (report) report(text);
    };
    const int room = kWidth - 2 * kMargin;
    const int right = kWidth - kMargin - 8, buttonText = right - (kMargin + 8) - 24;
    const int labelRoom = right - kValueWidth - 12 - (kMargin + 16);

    for (int set = 0; set < 2; ++set)
    {
        TabRow tabs;
        GatherTabs(set == 1, tabs);
        if (tabs.namesTotal + (tabs.count - 1) * 4 > room)
        {
            snprintf(what, sizeof(what), "the %s tabs' names take %d pixels with their gaps, the menu has %d", set ? "Debug" : "settings",
                     tabs.namesTotal + (tabs.count - 1) * 4, room);
            problem(what);
        }
    }
    for (const char* hint : kHints)
        if (kMargin + TextWidth("BunkerVR") + 16 > kWidth - kMargin - TextWidth(hint)) problem("the title line's words overlap");
    // The title line's "Controls" between the title and the controls' words
    // (as Draw places it, on every page but the start-up page).
    {
        int x1 = kWidth - kMargin - TextWidth(kHints[1]) - 28;
        int x0 = x1 - TextWidth(kControlsName) - 40;
        if (kMargin + TextWidth("BunkerVR") + 12 > x0) problem("the title line's Controls button overlaps the title");
    }
    // The Controls page: every line, with every choice of the
    // settings it follows, in its room; all of them above the footer.
    for (int variant = 0; variant < 8; ++variant)
    {
        ControlLine lines[4][12];
        int counts[4];
        for (int part = 0; part < 4; ++part) counts[part] = ControlLines(part, (variant & 1) != 0, (variant & 2) != 0, (variant & 4) != 0, lines[part]);
        for (int part = 0; part < 4; ++part)
            for (int i = 0; i < counts[part]; ++i)
            {
                if (TextWidth(lines[part][i].control) > ControlsNameRoom(part) - 12 ||
                    TextWidth(lines[part][i].action) > kControlsColumn - 10 - ControlsNameRoom(part) - 6)
                {
                    snprintf(what, sizeof(what), "Controls: \"%s\" / \"%s\" is too wide (%d and %d pixels; room %d and %d)", lines[part][i].control,
                             lines[part][i].action, TextWidth(lines[part][i].control), TextWidth(lines[part][i].action), ControlsNameRoom(part) - 12,
                             kControlsColumn - 10 - ControlsNameRoom(part) - 6);
                    problem(what);
                }
            }
        int controllerRows = counts[0] > counts[1] ? counts[0] : counts[1];
        int bodyRows = counts[2] > counts[3] ? counts[2] : counts[3];
        int bottom = kRowsTop + 6 + (controllerRows + 1) * kControlsRow + 14 + (bodyRows + 1) * kControlsRow;
        if (bottom > kFooterTop - 4)
        {
            snprintf(what, sizeof(what), "Controls: its lines reach down to %d, the footer starts at %d", bottom, kFooterTop);
            problem(what);
        }
    }
    for (const char* footer : kControlsFooters)
        if (TextWidth(footer) > room) problem("Controls: a footer line is wider than the page");

    for (int page = 0; page < kPageCount; ++page)
    {
        if (page == kPageChecks) continue;
        if (RowHeight(page) < kFontHeight + 10)
        {
            snprintf(what, sizeof(what), "%s: %d rows, %d pixels each", kPageNames[page], g_pageItemCount[page], RowHeight(page));
            problem(what);
        }
        for (int i = 0; i < g_pageItemCount[page]; ++i)
        {
            const Item& item = g_pages[page][i];
            if (item.kind == kButtonItem)
            {
                if (TextWidth(item.label) > buttonText)
                {
                    snprintf(what, sizeof(what), "%s: the button \"%s\" is %d pixels, its room %d", kPageNames[page], item.label, TextWidth(item.label), buttonText);
                    problem(what);
                }
                continue;
            }
            if (TextWidth(item.label) > labelRoom)
            {
                snprintf(what, sizeof(what), "%s: \"%s\" is %d pixels, its room %d", kPageNames[page], item.label, TextWidth(item.label), labelRoom);
                problem(what);
            }
            const char* names[4] = { item.offName, item.onName, item.thirdName, item.fourthName };
            int nameCount = item.kind == kSwitch ? 2 : (item.kind == kCycle ? CycleChoices(item) : 0);
            for (int n = 0; n < nameCount; ++n)
            {
                if (names[n] && TextWidth(names[n]) <= kValueWidth - 16) continue;
                snprintf(what, sizeof(what), "%s: \"%s\" shows \"%s\", too wide for its button", kPageNames[page], item.label, names[n] ? names[n] : "(nothing)");
                problem(what);
            }
            if (item.kind == kNumber)
            {
                float ends[2] = { item.lowest, item.highest };
                for (float end : ends)
                {
                    char value[32];
                    NumberText(item, end, value, sizeof(value));
                    if (TextWidth(value) <= kNumberTextWidth - 8) continue;
                    snprintf(what, sizeof(what), "%s: \"%s\" shows \"%s\", too wide between - and +", kPageNames[page], item.label, value);
                    problem(what);
                }
            }
        }
        // The page's own words (with nothing measured yet), in two lines.
        char lines[3][kTestListLineChars];
        const char* footer = PageFooter(page);
        if (page != kPageGrips && TestListWrap(footer, room, TextWidth, lines, 3) > 2)
        {
            snprintf(what, sizeof(what), "%s: its footer takes more than two lines", kPageNames[page]);
            problem(what);
        }
    }

    // The Test List page's buttons: the first across the page, three in a row, two in a row (DrawChecksPage).
    const int third = (right - (kMargin + 8)) / 3 - 8, half = (right - (kMargin + 8)) / 2 - 4;
    for (int i = 0; i < 6; ++i)
    {
        int roomHere = i == 0 ? buttonText : (i <= 3 ? third - 16 : half - 16);
        if (TextWidth(g_pages[kPageChecks][i].label) > roomHere)
        {
            snprintf(what, sizeof(what), "Test List: the button \"%s\" is too wide", g_pages[kPageChecks][i].label);
            problem(what);
        }
    }
    for (const char* footer : kChecksFooters)
        if (TextWidth(footer) > room) problem("Test List: its footer is wider than one line");
    return problems;
}

// How wide the footer's words may be, in two lines: for the tests, the
// measuring's lines from vr.cpp (VrMenuSetComfortInfo).
bool VrMenuFooterFits(const char* text)
{
    char lines[3][kTestListLineChars];
    return TestListWrap(text, kWidth - 2 * kMargin, TextWidth, lines, 3) <= 2;
}

// ---------------------------------------------------------------------------
// Reacting to the pointer
// ---------------------------------------------------------------------------

static void Press(int itemIndex, int direction)
{
    // The Controls page: the title line's button opens it (and,
    // pressed there again, closes it); its "< Back" goes back to the page it
    // was opened from.
    if (itemIndex == kTitleControls || itemIndex == kControlsBack)
    {
        if (g_page == kPageControls) g_page = g_pageBeforeControls;
        else if (itemIndex == kTitleControls)
        {
            g_pageBeforeControls = g_page;
            g_page = kPageControls;
        }
        g_dirty = true;
        return;
    }
    if (itemIndex < 0)
    {
        int page = itemIndex == kTabToDebug ? g_lastDebugPage : (itemIndex == kTabToSettings ? g_lastSettingsPage : -1 - itemIndex);
        if (page >= 0 && page < kPageCount) g_page = page;
        g_dirty = true;
        return;
    }
    if (itemIndex >= g_pageItemCount[g_page]) return;

    Item& item = g_pages[g_page][itemIndex];
    if (item.kind == kSwitch)
    {
        if (item.flag) *item.flag = !*item.flag;
        else *item.choice = *item.choice ? 0 : 1;
        SaveSettings();
        if (item.action && g_actionHandler) g_actionHandler(item.action);
    }
    else if (item.kind == kCycle)
    {
        *item.choice = (*item.choice + 1) % CycleChoices(item);
        SaveSettings();
        if (item.action && g_actionHandler) g_actionHandler(item.action);
    }
    else if (item.kind == kNumber)
    {
        float value = *item.number;
        if (item.factor != 0)
        {
            value = direction > 0 ? value * item.factor : value / item.factor;
        }
        else
        {
            // Keep to whole steps so repeated presses come back to round numbers.
            value = (float)floor((double)(value + direction * item.step) / item.step + 0.5) * item.step;
        }
        if (value < item.lowest) value = item.lowest;
        if (value > item.highest) value = item.highest;
        *item.number = value;
        SaveSettings();
        if (item.action && g_actionHandler) g_actionHandler(item.action);
    }
    else if (g_actionHandler)
    {
        g_actionHandler(item.action);
    }
    g_dirty = true;
}

void VrMenuPointer(bool onMenu, float u, float v, bool squeezing, double nowMs)
{
    if (!g_open) return;

    int hover = -1;
    if (onMenu)
    {
        int x = (int)(u * kWidth), y = (int)(v * kHeight);
        for (int i = 0; i < g_hotCount; ++i)
        {
            const HotArea& hot = g_hot[i];
            if (x >= hot.x0 && x < hot.x1 && y >= hot.y0 && y < hot.y1)
            {
                hover = i;
                break;
            }
        }
    }

    int item = hover >= 0 ? g_hot[hover].item : -100;
    int direction = hover >= 0 ? g_hot[hover].direction : 0;
    if (item != g_hoverItem || direction != g_hoverDirection)
    {
        g_hoverItem = item;
        g_hoverDirection = direction;
        g_dirty = true;
        if (hover >= 0) ++g_hoverCount;
    }
    g_hover = hover;

    if (squeezing && hover >= 0)
    {
        if (!g_wasSqueezing)
        {
            Press(item, direction);
            g_nextRepeatMs = nowMs + 450.0;
            g_lastPressMs = nowMs;
        }
        else if (direction != 0 && nowMs >= g_nextRepeatMs)
        {
            // Holding - or + keeps going.
            Press(item, direction);
            g_nextRepeatMs = nowMs + 110.0;
            g_lastPressMs = nowMs;
        }
    }
    g_wasSqueezing = squeezing;
}

bool VrMenuPointerOnButton()
{
    return g_open && g_hover >= 0;
}

int VrMenuHoverCount()
{
    return g_hoverCount;
}

double VrMenuLastPressMs()
{
    return g_lastPressMs;
}

// ---------------------------------------------------------------------------
// The bullet counter: a small see-through picture with a number on it, in the
// game's own menu lettering (its typewriter font, read from the game's
// "fonts" folder) and its menu colors: pale for ordinary text, amber for
// what matters. If the font cannot be read, a plain dark tag is drawn instead.
// ---------------------------------------------------------------------------

static const int kCounterWidth = 256, kCounterHeight = 96;
static unsigned char g_counterCanvas[kCounterWidth * kCounterHeight * 4];
static unsigned char g_counterInk[kCounterWidth * kCounterHeight];   // how much lettering covers each pixel
static unsigned char g_counterTint[kCounterWidth * kCounterHeight];  // its color there: 0 pale, 255 amber
static unsigned char g_counterShade[kCounterWidth * kCounterHeight]; // the soft dark patch behind the lettering
static unsigned char g_counterWork[kCounterWidth * kCounterHeight];

static const Colour kGamePale = { 236, 230, 214 };
static const Colour kGameAmber = { 255, 191, 64 }; // the game's color for the chosen menu row

struct GameGlyph
{
    short x, y, width, height, xOffset, yOffset, advance;
};
static GameGlyph g_gameGlyphs[128];
static unsigned char* g_gameFontPixels = nullptr; // the font's sheet of letters, one byte per pixel
static int g_gameFontSheetWidth = 0, g_gameFontSheetHeight = 0;
static int g_gameFontLine = 0;
static int g_gameFontState = 0; // 0 not tried yet, 1 ready, -1 not available

// Finds name="number" inside the tag that starts at `tag`.
static int XmlNumber(const char* tag, const char* name)
{
    const char* end = strchr(tag, '>');
    char key[32];
    snprintf(key, sizeof(key), " %s=\"", name);
    const char* at = strstr(tag, key);
    if (!at || (end && at > end)) return 0;
    return atoi(at + strlen(key));
}

static bool LoadGameFont()
{
    if (g_gameFontState != 0) return g_gameFontState > 0;
    g_gameFontState = -1;

    // The list of letters: where each one is on the sheet.
    int size = 0;
    unsigned char* list = ReadGameFile("fonts\\special_elite_56.fnt", size);
    if (!list) return false;
    const char* text = (const char*)list;
    const char* common = strstr(text, "<common ");
    if (common)
    {
        g_gameFontLine = XmlNumber(common, "lineHeight");
        g_gameFontSheetWidth = XmlNumber(common, "scaleW");
        g_gameFontSheetHeight = XmlNumber(common, "scaleH");
    }
    memset(g_gameGlyphs, 0, sizeof(g_gameGlyphs));
    int found = 0;
    for (const char* at = strstr(text, "<char "); at; at = strstr(at + 6, "<char "))
    {
        int id = XmlNumber(at, "id");
        if (id < 32 || id > 126 || XmlNumber(at, "page") != 0) continue;
        GameGlyph& glyph = g_gameGlyphs[id];
        glyph.x = (short)XmlNumber(at, "x");
        glyph.y = (short)XmlNumber(at, "y");
        glyph.width = (short)XmlNumber(at, "width");
        glyph.height = (short)XmlNumber(at, "height");
        glyph.xOffset = (short)XmlNumber(at, "xoffset");
        glyph.yOffset = (short)XmlNumber(at, "yoffset");
        glyph.advance = (short)XmlNumber(at, "xadvance");
        ++found;
    }
    delete[] list;
    if (!common || found < 11 || g_gameFontLine <= 0 || g_gameFontSheetWidth <= 0 || g_gameFontSheetHeight <= 0 ||
        g_gameFontSheetWidth > 4096 || g_gameFontSheetHeight > 4096)
        return false;

    // The sheet itself: a plain picture, one byte per pixel, after a header
    // of 128 bytes.
    unsigned char* sheet = ReadGameFile("fonts\\special_elite_56_0.dds", size);
    if (!sheet) return false;
    int pixels = g_gameFontSheetWidth * g_gameFontSheetHeight;
    bool plain = size >= 128 + pixels && memcmp(sheet, "DDS ", 4) == 0 && sheet[84] == 0 && sheet[88] == 8;
    if (plain)
    {
        g_gameFontPixels = new unsigned char[pixels];
        memcpy(g_gameFontPixels, sheet + 128, (size_t)pixels);
        g_gameFontState = 1;
    }
    delete[] sheet;
    return plain;
}

// Softens a one-byte-per-pixel picture: each pixel becomes the average of
// those up to `radius` to either side, across and then down.
static void SoftenCounter(unsigned char* picture, int radius)
{
    for (int y = 0; y < kCounterHeight; ++y)
    {
        for (int x = 0; x < kCounterWidth; ++x)
        {
            int sum = 0;
            for (int i = -radius; i <= radius; ++i)
            {
                int px = x + i;
                if (px >= 0 && px < kCounterWidth) sum += picture[y * kCounterWidth + px];
            }
            g_counterWork[y * kCounterWidth + x] = (unsigned char)(sum / (2 * radius + 1));
        }
    }
    for (int y = 0; y < kCounterHeight; ++y)
    {
        for (int x = 0; x < kCounterWidth; ++x)
        {
            int sum = 0;
            for (int i = -radius; i <= radius; ++i)
            {
                int py = y + i;
                if (py >= 0 && py < kCounterHeight) sum += g_counterWork[py * kCounterWidth + x];
            }
            picture[y * kCounterWidth + x] = (unsigned char)(sum / (2 * radius + 1));
        }
    }
}

// The plain tag, in the menu's own built-in lettering.
static void DrawCounterPlain(const char* text)
{
    // A dark tag with rounded ends, half see-through. (Colors are stored
    // already multiplied by how solid they are, which is what the headset
    // expects of a see-through picture.)
    const int radius = kCounterHeight / 2;
    for (int y = 0; y < kCounterHeight; ++y)
    {
        for (int x = 0; x < kCounterWidth; ++x)
        {
            int cx = x < radius ? radius : (x >= kCounterWidth - radius ? kCounterWidth - radius - 1 : x);
            int dx = x - cx, dy = y - radius;
            bool inside = dx * dx + dy * dy <= radius * radius;
            unsigned char* p = g_counterCanvas + (y * kCounterWidth + x) * 4;
            p[0] = inside ? 6 : 0;
            p[1] = inside ? 6 : 0;
            p[2] = inside ? 7 : 0;
            p[3] = inside ? 150 : 0;
        }
    }

    // The text, twice the menu's size, in the middle.
    const int scale = 2;
    int textWidth = TextWidth(text) * scale;
    int x = (kCounterWidth - textWidth) / 2;
    int y = (kCounterHeight - kFontHeight * scale) / 2;
    for (const char* s = text; *s; ++s)
    {
        int index = (unsigned char)*s - kFontFirst;
        if (index < 0 || index >= kFontCount) index = '?' - kFontFirst;
        const FontGlyph& glyph = kFontGlyphs[index];
        const unsigned char* pixels = kFontPixels + glyph.offset;
        for (int row = 0; row < kFontHeight * scale; ++row)
        {
            int py = y + row;
            if (py < 0 || py >= kCounterHeight) continue;
            for (int column = 0; column < glyph.width * scale; ++column)
            {
                int px = x + column;
                if (px < 0 || px >= kCounterWidth) continue;
                int a = pixels[(row / scale) * glyph.width + column / scale];
                if (a == 0) continue;
                unsigned char* p = g_counterCanvas + (py * kCounterWidth + px) * 4;
                p[0] = (unsigned char)((p[0] * (255 - a) + kText.r * a) / 255);
                p[1] = (unsigned char)((p[1] * (255 - a) + kText.g * a) / 255);
                p[2] = (unsigned char)((p[2] * (255 - a) + kText.b * a) / 255);
                p[3] = (unsigned char)((p[3] * (255 - a) + 255 * a) / 255);
            }
        }
        x += glyph.advance * scale;
    }
}

const unsigned char* VrCounterPicture(const char* text, int& width, int& height)
{
    width = kCounterWidth;
    height = kCounterHeight;

    if (!LoadGameFont())
    {
        DrawCounterPlain(text);
        return g_counterCanvas;
    }

    // "4 / 12": the bullets in the gun (before the stroke) are amber, the
    // rest pale. A number on its own is pale.
    bool twoParts = strchr(text, '/') != nullptr;

    int textWidth = 0;
    for (const char* s = text; *s; ++s)
    {
        int id = (unsigned char)*s;
        if (id < 32 || id > 126) id = '?';
        textWidth += g_gameGlyphs[id].advance;
    }

    memset(g_counterInk, 0, sizeof(g_counterInk));
    memset(g_counterTint, 0, sizeof(g_counterTint));
    int x = (kCounterWidth - textWidth) / 2;
    int y = (kCounterHeight - g_gameFontLine) / 2;
    bool amber = twoParts;
    for (const char* s = text; *s; ++s)
    {
        int id = (unsigned char)*s;
        if (id < 32 || id > 126) id = '?';
        if (id == ' ') amber = false;
        const GameGlyph& glyph = g_gameGlyphs[id];
        for (int row = 0; row < glyph.height; ++row)
        {
            int py = y + glyph.yOffset + row;
            int sy = glyph.y + row;
            if (py < 0 || py >= kCounterHeight || sy < 0 || sy >= g_gameFontSheetHeight) continue;
            for (int column = 0; column < glyph.width; ++column)
            {
                int px = x + glyph.xOffset + column;
                int sx = glyph.x + column;
                if (px < 0 || px >= kCounterWidth || sx < 0 || sx >= g_gameFontSheetWidth) continue;
                unsigned char a = g_gameFontPixels[sy * g_gameFontSheetWidth + sx];
                if (a > g_counterInk[py * kCounterWidth + px])
                {
                    g_counterInk[py * kCounterWidth + px] = a;
                    g_counterTint[py * kCounterWidth + px] = amber ? 255 : 0;
                }
            }
        }
        x += glyph.advance;
    }

    // A soft dark patch behind the lettering, so that it reads against a
    // bright wall as well as in the dark, with no box round it.
    memcpy(g_counterShade, g_counterInk, sizeof(g_counterShade));
    SoftenCounter(g_counterShade, 7);
    SoftenCounter(g_counterShade, 5);

    for (int i = 0; i < kCounterWidth * kCounterHeight; ++i)
    {
        int shade = g_counterShade[i] * 4; // strong near the letters, fading out
        if (shade > 190) shade = 190;
        int ink = g_counterInk[i];
        const Colour& c = g_counterTint[i] ? kGameAmber : kGamePale;
        unsigned char* p = g_counterCanvas + i * 4;
        // (Colors are stored already multiplied by how solid they are.)
        p[0] = (unsigned char)(c.r * ink / 255);
        p[1] = (unsigned char)(c.g * ink / 255);
        p[2] = (unsigned char)(c.b * ink / 255);
        p[3] = (unsigned char)(shade * (255 - ink) / 255 + ink);
    }
    return g_counterCanvas;
}

// ---------------------------------------------------------------------------
// The start-up page in the game's own look, to match its art direction. The
// game's menus (helper_imgui_options.hps, FontHandler.hps): its typewriter lettering
// (special_elite), words in pale off-white, the one pointed at in amber
// (gHoverColor 1, 0.75, 0.25), and no buttons drawn round them; its notes and
// cards dark and plain. So the page is lettering on a dark, worn patch that
// fades into the room at its edges, with no boxes: a thin worn line under the
// heading, and a short one under each choice that turns amber with it. The
// headset blends it with the room (VrMenuSeeThrough). Without the game's
// lettering, the plain page (DrawStartupPage).
// ---------------------------------------------------------------------------

static const Colour kPatch = { 15, 13, 11 }; // a warm near-black, as behind the game's words

// A fixed scatter of numbers for the grain and the worn edges.
static unsigned Grain(int x, int y)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// 0 to 1, changing smoothly over `size` pixels.
static float SmoothNoise(float x, float y, float size)
{
    float fx = x / size, fy = y / size;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - ix, ty = fy - iy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = (Grain(ix, iy) & 1023) / 1023.0f, b = (Grain(ix + 1, iy) & 1023) / 1023.0f;
    float c = (Grain(ix, iy + 1) & 1023) / 1023.0f, d = (Grain(ix + 1, iy + 1) & 1023) / 1023.0f;
    return (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
}

// Laid over the picture by `amount` (0 to 1); colors stored already
// multiplied by how solid they are, as the headset takes a see-through picture.
static void Over(int x, int y, Colour c, float amount)
{
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight || amount <= 0.0f) return;
    if (amount > 1.0f) amount = 1.0f;
    unsigned char* p = g_canvas + (y * kWidth + x) * 4;
    float keep = 1.0f - amount;
    p[0] = (unsigned char)(c.r * amount + p[0] * keep + 0.5f);
    p[1] = (unsigned char)(c.g * amount + p[1] * keep + 0.5f);
    p[2] = (unsigned char)(c.b * amount + p[2] * keep + 0.5f);
    p[3] = (unsigned char)(255.0f * amount + p[3] * keep + 0.5f);
}

// The dark patch from row `top` down to row `bottom`, the rest of the picture
// see-through: solid in the middle, fading out over the last 60 to 90 pixels to
// each side along a worn, uneven edge, with a fine grain in it. Worked out once
// for each size and kept: worked out afresh it takes about 44 ms, four of the
// headset's frames, each time the pointer moves from one choice to another, and
// the game stutters.
static unsigned char g_patchCanvas[kWidth * kHeight * 4];
static int g_patchTop = -1, g_patchBottom = -1;

static void DrawWornPatch(int top, int bottom)
{
    if (top < 0) top = 0;
    if (bottom > kHeight) bottom = kHeight;
    if (top == g_patchTop && bottom == g_patchBottom)
    {
        memcpy(g_canvas, g_patchCanvas, sizeof(g_canvas));
        return;
    }
    memset(g_canvas, 0, sizeof(g_canvas));
    for (int y = top; y < bottom; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            float toEdge = (float)(x < kWidth - 1 - x ? x : kWidth - 1 - x);
            float toEdgeY = (float)(y - top < bottom - 1 - y ? y - top : bottom - 1 - y);
            if (toEdgeY < toEdge) toEdge = toEdgeY;
            float fade = (toEdge - 30.0f * SmoothNoise((float)x, (float)y, 40.0f)) / 70.0f;
            if (fade <= 0.0f) continue;
            if (fade > 1.0f) fade = 1.0f;
            fade = fade * fade * (3.0f - 2.0f * fade);
            float grain = ((int)(Grain(x, y) & 15) - 7.5f) * 0.6f + (SmoothNoise((float)x, (float)y, 9.0f) - 0.5f) * 5.0f;
            float r = kPatch.r + grain, g = kPatch.g + grain, b = kPatch.b + grain;
            Colour c = { (unsigned char)(r < 0.0f ? 0.0f : r), (unsigned char)(g < 0.0f ? 0.0f : g), (unsigned char)(b < 0.0f ? 0.0f : b) };
            Over(x, y, c, 0.94f * fade);
        }
    }
    memcpy(g_patchCanvas, g_canvas, sizeof(g_canvas));
    g_patchTop = top;
    g_patchBottom = bottom;
}

// The game's lettering at `scale` of its 56-pixel size.
static float g_gameScale = 0.5f; // (for GameTextWidth, which TestListWrap calls with the words alone)

static int GameTextWidth(const char* text)
{
    int units = 0;
    for (const char* s = text; *s; ++s)
    {
        int id = (unsigned char)*s;
        if (id < 32 || id > 126) id = '?';
        units += g_gameGlyphs[id].advance;
    }
    return (int)(units * g_gameScale + 0.5f);
}

// A letter at one size: how much of each pixel it covers (0 to 255), each pixel
// taking the share of the letter's sheet it covers (a box filter: the lettering
// is drawn smaller than its sheet). Worked out once for each letter and size and
// kept (worked out afresh, it is most of a redraw), each letter put
// at the nearest whole pixel.
struct ScaledGlyph
{
    float scale;
    int width, height;
    unsigned char* cover; // (null: not made yet)
};
static const int kScaledSizes = 16;
static ScaledGlyph g_scaledGlyphs[kScaledSizes][128];
static int g_scaledSizeCount = 0;

// One letter of a font's sheet (one byte a pixel) at `scale`, into `made`
// (its cover made new).
static void ScaleGlyph(const GameGlyph& glyph, const unsigned char* sheet, int sheetWidth, int sheetHeight, float scale, ScaledGlyph& made)
{
    made.scale = scale;
    made.width = (int)ceilf(glyph.width * scale);
    made.height = (int)ceilf(glyph.height * scale);
    made.cover = new unsigned char[made.width * made.height + 1];
    for (int py = 0; py < made.height; ++py)
    {
        float sy0 = py / scale, sy1 = (py + 1) / scale;
        for (int px = 0; px < made.width; ++px)
        {
            float sx0 = px / scale, sx1 = (px + 1) / scale;
            float sum = 0.0f, area = 0.0f;
            for (int sy = (int)floorf(sy0); sy < (int)ceilf(sy1); ++sy)
            {
                float hy = fminf(sy1, sy + 1.0f) - fmaxf(sy0, (float)sy);
                if (hy <= 0.0f) continue;
                for (int sx = (int)floorf(sx0); sx < (int)ceilf(sx1); ++sx)
                {
                    float hx = fminf(sx1, sx + 1.0f) - fmaxf(sx0, (float)sx);
                    if (hx <= 0.0f) continue;
                    area += hx * hy;
                    if (sx < 0 || sy < 0 || sx >= glyph.width || sy >= glyph.height) continue;
                    int ax = glyph.x + sx, ay = glyph.y + sy;
                    if (ax < 0 || ay < 0 || ax >= sheetWidth || ay >= sheetHeight) continue;
                    sum += sheet[ay * sheetWidth + ax] * hx * hy;
                }
            }
            made.cover[py * made.width + px] = (unsigned char)(area > 0.0f ? sum / area + 0.5f : 0.0f);
        }
    }
}

static const ScaledGlyph& GlyphAt(int id, float scale)
{
    int size = 0;
    while (size < g_scaledSizeCount && g_scaledGlyphs[size][0].scale != scale) ++size;
    if (size == g_scaledSizeCount)
    {
        if (size == kScaledSizes) // (more sizes than ever drawn: the last one's letters made afresh)
        {
            size = kScaledSizes - 1;
            for (ScaledGlyph& made : g_scaledGlyphs[size])
            {
                delete[] made.cover;
                made.cover = nullptr;
            }
        }
        else ++g_scaledSizeCount;
        for (ScaledGlyph& made : g_scaledGlyphs[size]) made.scale = scale;
    }
    ScaledGlyph& made = g_scaledGlyphs[size][id];
    if (made.cover) return made;
    ScaleGlyph(g_gameGlyphs[id], g_gameFontPixels, g_gameFontSheetWidth, g_gameFontSheetHeight, scale, made);
    return made;
}

static void DrawGameText(int left, int top, const char* text, Colour c, float scale, float opacity)
{
    float pen = (float)left;
    for (const char* s = text; *s; ++s)
    {
        int id = (unsigned char)*s;
        if (id < 32 || id > 126) id = '?';
        const GameGlyph& glyph = g_gameGlyphs[id];
        const ScaledGlyph& made = GlyphAt(id, scale);
        int x0 = (int)floorf(pen + glyph.xOffset * scale + 0.5f), y0 = (int)floorf(top + glyph.yOffset * scale + 0.5f);
        for (int py = 0; py < made.height; ++py)
            for (int px = 0; px < made.width; ++px)
            {
                unsigned char cover = made.cover[py * made.width + px];
                if (cover) Over(x0 + px, y0 + py, c, cover / 255.0f * opacity);
            }
        pen += glyph.advance * scale;
    }
}

static void DrawGameTextCentred(int x0, int x1, int top, const char* text, Colour c, float scale, float opacity)
{
    g_gameScale = scale;
    DrawGameText((x0 + x1 - GameTextWidth(text)) / 2, top, text, c, scale, opacity);
}

// A thin hand-ruled line, uneven in strength and faded at its ends.
static void DrawWornLine(int x0, int x1, int y, Colour c, float opacity)
{
    for (int x = x0; x < x1; ++x)
    {
        float ends = fminf(fminf((x - x0) / 24.0f, (x1 - 1 - x) / 24.0f), 1.0f);
        float strength = opacity * ends * (0.55f + 0.45f * SmoothNoise((float)x, (float)y, 11.0f));
        Over(x, y, c, strength);
        Over(x, y + 1, c, strength * 0.45f);
    }
}

static void DrawStartupStyled()
{
    const VrStartupView& view = *g_startupView;
    const Colour pale = kGamePale, amber = kGameAmber;
    const int left = 70, right = kWidth - 70, middle = kWidth / 2;
    const int textLeft = 110, textRight = kWidth - 110; // (narrower lines read more easily, centered)
    const float line = (float)g_gameFontLine;

    // Calm and centered, as the game's cards are: a heading over a short ruled
    // line, a line or two of what to do, the choices, and nothing else but a
    // dim note, what the last step found, and how to skip. First where each
    // goes, from the top of the patch; then the patch, as tall as that, in the
    // middle of the picture (where the panel hangs, a little below the eyes).
    g_gameScale = 0.5f;
    static const int kLinesRoom = 4;
    char lines[kLinesRoom][kTestListLineChars];
    int count = (view.text && view.text[0]) ? TestListWrap(view.text, textRight - textLeft, GameTextWidth, lines, kLinesRoom) : 0;
    if (count > kLinesRoom) count = kLinesRoom;
    g_gameScale = 0.4f;
    char notes[2][kTestListLineChars];
    int noteCount = (view.note && view.note[0]) ? TestListWrap(view.note, textRight - textLeft, GameTextWidth, notes, 2) : 0;
    if (noteCount > 2) noteCount = 2;
    int choices = 0;
    for (int i = 0; i < 3; ++i)
        if (view.buttons[i] && view.buttons[i][0]) ++choices;
    const int step = (int)(line * 0.5f * 1.12f), noteStep = (int)(line * 0.4f * 1.1f);
    const int titleTop = 92, rule = titleTop + (int)(line * 0.78f) + 10;
    const int bodyTop = rule + 34, noteTop = bodyTop + count * step + (count > 0 ? 8 : 0);
    int bottom = noteTop + noteCount * noteStep;
    const int pickerTop = bottom + 34;
    if (view.heightPicker) bottom = pickerTop + 70;
    const int choiceTop = bottom + (view.heightPicker ? 30 : 48);
    if (choices > 0) bottom = choiceTop + (int)(line * 0.6f) + 8;
    const int footerTop = bottom + 30;
    if (g_comfortInfo[0]) bottom = footerTop + (int)(line * 0.36f);
    const int skipTop = bottom + 36;
    bottom = skipTop + (int)(line * 0.32f) + 80;
    int shift = (kHeight - bottom) / 2;
    if (shift < 0) shift = 0;
    DrawWornPatch(shift, shift + bottom);

    // The heading, and a short ruled line under it.
    DrawGameTextCentred(left, right, shift + titleTop, view.title ? view.title : "", pale, 0.78f, 1.0f);
    DrawWornLine(middle - 120, middle + 120, shift + rule, pale, 0.45f);

    // What to do, and the note under it, smaller and dimmer.
    for (int i = 0; i < count; ++i) DrawGameTextCentred(textLeft, textRight, shift + bodyTop + i * step, lines[i], pale, 0.5f, 0.9f);
    for (int i = 0; i < noteCount; ++i) DrawGameTextCentred(textLeft, textRight, shift + noteTop + i * noteStep, notes[i], pale, 0.4f, 0.55f);

    // Your height: the number between a "-" and a "+", each a word to point at.
    if (view.heightPicker)
    {
        const Item& item = g_pages[kPageStartup][3];
        char value[32];
        snprintf(value, sizeof(value), item.format, (double)(*item.number * item.shown));
        const int top = shift + pickerTop;
        bool lessHot = g_hoverItem == 3 && g_hoverDirection == -1, moreHot = g_hoverItem == 3 && g_hoverDirection == +1;
        DrawGameTextCentred(middle - 110, middle + 110, top, value, pale, 0.75f, 1.0f);
        DrawGameTextCentred(middle - 230, middle - 150, top - 6, "-", lessHot ? amber : pale, 0.95f, lessHot ? 1.0f : 0.7f);
        DrawGameTextCentred(middle + 150, middle + 230, top - 6, "+", moreHot ? amber : pale, 0.95f, moreHot ? 1.0f : 0.7f);
        if (lessHot) DrawWornLine(middle - 215, middle - 165, top + (int)(line * 0.75f) + 2, amber, 0.9f);
        if (moreHot) DrawWornLine(middle + 165, middle + 215, top + (int)(line * 0.75f) + 2, amber, 0.9f);
        AddHot(middle - 260, top - 20, middle - 120, top + 80, 3, -1);
        AddHot(middle + 120, top - 20, middle + 260, top + 80, 3, +1);
    }

    // The choices: words side by side; the one pointed at turns amber, with a
    // short ruled line under it, as in the game's menus.
    if (choices > 0)
    {
        const int span = choices == 1 ? 360 : (choices == 2 ? 600 : right - left);
        const int cell = span / choices, top = shift + choiceTop;
        int x = middle - span / 2;
        for (int i = 0; i < 3; ++i)
        {
            if (!view.buttons[i] || !view.buttons[i][0]) continue;
            g_pages[kPageStartup][i].label = view.buttons[i];
            bool hot = g_hoverItem == i && g_hoverDirection == 0;
            g_gameScale = 0.6f;
            int width = GameTextWidth(view.buttons[i]);
            int centre = x + cell / 2;
            DrawGameTextCentred(x, x + cell, top, view.buttons[i], hot ? amber : pale, 0.6f, hot ? 1.0f : 0.85f);
            if (hot) DrawWornLine(centre - width / 2 - 12, centre + width / 2 + 12, top + (int)(line * 0.6f) + 4, amber, 0.9f);
            AddHot(x + 4, top - 24, x + cell - 4, top + (int)(line * 0.6f) + 24, i, 0);
            x += cell;
        }
    }

    // What the last step found; then how to skip.
    if (g_comfortInfo[0]) DrawGameTextCentred(left, right, shift + footerTop, g_comfortInfo, amber, 0.36f, 0.6f);
    const char* skip = "Press Y to skip setup.";
    g_gameScale = 0.32f;
    int skipLeft = middle - GameTextWidth(skip) / 2;
    DrawGameText(skipLeft, shift + skipTop, skip, pale, 0.32f, 0.35f);
}

bool VrMenuSeeThrough()
{
    return g_drawnSeeThrough;
}

const unsigned char* VrMenuPicture(int& width, int& height, bool& changed)
{
    width = kWidth;
    height = kHeight;
    changed = g_dirty;
    if (g_dirty)
    {
        g_dirty = false;
        Draw();
    }
    return g_canvas;
}

// ---------------------------------------------------------------------------
// The bullets at the pouch, shown as the game shows its count at the bottom
// right of the screen (ItemHandler.hps, DoCounterGui): its picture of a bullet
// (graphics\inventory\items\inventory_item_bullet.dds; the grayscale one at
// none) and over its lower part the count as "x3" (InventoryUI's "Stack"),
// white with a dark shadow, in its menu lettering at a fifth of the picture's
// height.
// ---------------------------------------------------------------------------

static const int kPouchSize = 256;
static unsigned char g_pouchCanvas[kPouchSize * kPouchSize * 4];
static unsigned char g_bulletPictures[4][kPouchSize * kPouchSize * 4]; // a bullet ordinary, grayscale; a shotgun shell the same (straight colors)
static int g_bulletPictureState[4] = { 0, 0, 0, 0 }; // 0 not tried, 1 ready, -1 not available

// The top picture of a DXT5 file (four by four pixels in each 16-byte block:
// the alphas as two ends and eight steps between, the colors as two ends in
// 5-6-5 and four steps between) as straight red, green, blue and alpha.
static void DecodeDxt5(const unsigned char* blocks, int width, int height, unsigned char* rgba)
{
    for (int by = 0; by < height / 4; ++by)
        for (int bx = 0; bx < width / 4; ++bx)
        {
            const unsigned char* b = blocks + ((size_t)by * (width / 4) + bx) * 16;
            int alphas[8] = { b[0], b[1] };
            if (b[0] > b[1])
                for (int i = 1; i < 7; ++i) alphas[i + 1] = ((7 - i) * b[0] + i * b[1]) / 7;
            else
            {
                for (int i = 1; i < 5; ++i) alphas[i + 1] = ((5 - i) * b[0] + i * b[1]) / 5;
                alphas[6] = 0;
                alphas[7] = 255;
            }
            unsigned long long alphaBits = 0;
            for (int i = 0; i < 6; ++i) alphaBits |= (unsigned long long)b[2 + i] << (8 * i);
            unsigned c0 = b[8] | (b[9] << 8), c1 = b[10] | (b[11] << 8);
            int colours[4][3] = {
                { (int)((c0 >> 11) & 31) * 255 / 31, (int)((c0 >> 5) & 63) * 255 / 63, (int)(c0 & 31) * 255 / 31 },
                { (int)((c1 >> 11) & 31) * 255 / 31, (int)((c1 >> 5) & 63) * 255 / 63, (int)(c1 & 31) * 255 / 31 },
            };
            for (int c = 0; c < 3; ++c)
            {
                colours[2][c] = (2 * colours[0][c] + colours[1][c]) / 3;
                colours[3][c] = (colours[0][c] + 2 * colours[1][c]) / 3;
            }
            unsigned colourBits = b[12] | (b[13] << 8) | (b[14] << 16) | ((unsigned)b[15] << 24);
            for (int i = 0; i < 16; ++i)
            {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                unsigned char* p = rgba + ((size_t)y * width + x) * 4;
                const int* colour = colours[(colourBits >> (2 * i)) & 3];
                p[0] = (unsigned char)colour[0];
                p[1] = (unsigned char)colour[1];
                p[2] = (unsigned char)colour[2];
                p[3] = (unsigned char)alphas[(alphaBits >> (3 * i)) & 7];
            }
        }
}

static bool LoadBulletPicture(int which)
{
    if (g_bulletPictureState[which] != 0) return g_bulletPictureState[which] > 0;
    g_bulletPictureState[which] = -1;
    int size = 0;
    static const char* const kFiles[4] = { "graphics\\inventory\\items\\inventory_item_bullet.dds",
                                           "graphics\\inventory\\items\\inventory_item_bullet_greyscale.dds",
                                           "graphics\\inventory\\items\\inventory_item_shell.dds",
                                           "graphics\\inventory\\items\\inventory_item_shell_greyscale.dds" };
    unsigned char* file = ReadGameFile(kFiles[which], size);
    if (!file) return false;
    const int top = kPouchSize * kPouchSize; // (the top picture's bytes: one a pixel in DXT5)
    bool ok = size >= 128 + top && memcmp(file, "DDS ", 4) == 0 && memcmp(file + 84, "DXT5", 4) == 0 &&
              file[12] == 0 && file[13] == 1 && file[16] == 0 && file[17] == 1; // (256 by 256)
    if (ok)
    {
        DecodeDxt5(file + 128, kPouchSize, kPouchSize, g_bulletPictures[which]);
        g_bulletPictureState[which] = 1;
    }
    delete[] file;
    return ok;
}

// Laid over the pouch's picture by `amount` (colors stored already multiplied
// by how solid they are).
static void OverPouch(int x, int y, Colour c, float amount)
{
    if (x < 0 || y < 0 || x >= kPouchSize || y >= kPouchSize || amount <= 0.0f) return;
    if (amount > 1.0f) amount = 1.0f;
    unsigned char* p = g_pouchCanvas + (y * kPouchSize + x) * 4;
    float keep = 1.0f - amount;
    p[0] = (unsigned char)(c.r * amount + p[0] * keep + 0.5f);
    p[1] = (unsigned char)(c.g * amount + p[1] * keep + 0.5f);
    p[2] = (unsigned char)(c.b * amount + p[2] * keep + 0.5f);
    p[3] = (unsigned char)(255.0f * amount + p[3] * keep + 0.5f);
}

static void DrawPouchText(int left, int top, const char* text, Colour c, float scale)
{
    float pen = (float)left;
    for (const char* s = text; *s; ++s)
    {
        int id = (unsigned char)*s;
        if (id < 32 || id > 126) id = '?';
        const GameGlyph& glyph = g_gameGlyphs[id];
        const ScaledGlyph& made = GlyphAt(id, scale);
        int x0 = (int)floorf(pen + glyph.xOffset * scale + 0.5f), y0 = (int)floorf(top + glyph.yOffset * scale + 0.5f);
        for (int py = 0; py < made.height; ++py)
            for (int px = 0; px < made.width; ++px)
            {
                unsigned char cover = made.cover[py * made.width + px];
                if (cover) OverPouch(x0 + px, y0 + py, c, cover / 255.0f);
            }
        pen += glyph.advance * scale;
    }
}

const unsigned char* VrPouchPicture(int count, bool shells, int& width, int& height)
{
    width = kPouchSize;
    height = kPouchSize;
    int which = (count > 0 ? 0 : 1) + (shells ? 2 : 0);
    if (!LoadGameFont() || !LoadBulletPicture(which)) return nullptr;

    // The bullet, its colors multiplied by how solid it is.
    const unsigned char* bullet = g_bulletPictures[which];
    for (int i = 0; i < kPouchSize * kPouchSize; ++i)
    {
        unsigned a = bullet[i * 4 + 3];
        g_pouchCanvas[i * 4 + 0] = (unsigned char)(bullet[i * 4 + 0] * a / 255);
        g_pouchCanvas[i * 4 + 1] = (unsigned char)(bullet[i * 4 + 1] * a / 255);
        g_pouchCanvas[i * 4 + 2] = (unsigned char)(bullet[i * 4 + 2] * a / 255);
        g_pouchCanvas[i * 4 + 3] = (unsigned char)a;
    }

    // "x3", centered, its top at 65% of the way down, a fifth of the picture tall;
    // its shadow first, a little down and right.
    char text[16];
    snprintf(text, sizeof(text), "x%d", count < 0 ? 0 : count);
    float scale = kPouchSize * 0.2f / (float)g_gameFontLine * 1.2f;
    g_gameScale = scale;
    int left = (kPouchSize - GameTextWidth(text)) / 2, top = (int)(kPouchSize * 0.65f);
    const Colour black = { 0, 0, 0 }, white = { 255, 255, 255 };
    DrawPouchText(left + 3, top + 3, text, black, scale);
    DrawPouchText(left, top, text, white, scale);
    return g_pouchCanvas;
}

// ---------------------------------------------------------------------------
// The hints on the objects (hints.h): one line drawn as the game draws its own
// hints, to match their look. HintHandler.hps (OnGui) draws a hint in its hint
// lettering (FontHandler.hps: eFontType_Hints, fonts\work_56, Work Sans Medium,
// spaced by the font's own pairs of letters), white, then the same in black at
// half strength a step down and to the right: a thin hard shadow, with no patch
// behind and no color. Without the game's files: the menu's own lettering, the
// same way.
// ---------------------------------------------------------------------------

static const int kHintWidth = 1024, kHintHeight = 72;
static const float kHintScale = 0.6f;    // of the 56-pixel lettering
static const int kHintMargin = 24;       // kept clear at each end
static const int kHintShadowStep = 2;    // the shadow's step down and to the right, pixels (the game's 1 at its 22-pixel size is 1.5 at this one)
static const int kHintShadowAlpha = 128; // half strength (the game's gfHintShadowAlphaMul)
static unsigned char g_hintCanvas[kHintWidth * kHintHeight * 4];
static unsigned char g_hintInk[kHintWidth * kHintHeight];

struct HintFont
{
    GameGlyph glyphs[128];
    short kerning[128][128];  // added to the step from one letter to the next, pixels at full size
    unsigned char* sheet;     // the sheet of letters, one byte a pixel
    int sheetWidth, sheetHeight, line;
    int state;                // 0 not tried yet, 1 ready, -1 not available
    ScaledGlyph scaled[128];  // each letter at kHintScale, made as it is first drawn
};
static HintFont g_hintFont = {};

static bool LoadHintFont()
{
    HintFont& font = g_hintFont;
    if (font.state != 0) return font.state > 0;
    font.state = -1;

    // The list of letters (where each is on the sheet) and of pairs of letters
    // set closer or further apart.
    int size = 0;
    unsigned char* list = ReadGameFile("fonts\\work_56.fnt", size);
    if (!list) return false;
    const char* text = (const char*)list;
    const char* common = strstr(text, "<common ");
    if (common)
    {
        font.line = XmlNumber(common, "lineHeight");
        font.sheetWidth = XmlNumber(common, "scaleW");
        font.sheetHeight = XmlNumber(common, "scaleH");
    }
    int found = 0;
    for (const char* at = strstr(text, "<char "); at; at = strstr(at + 6, "<char "))
    {
        int id = XmlNumber(at, "id");
        if (id < 32 || id > 126 || XmlNumber(at, "page") != 0) continue;
        GameGlyph& glyph = font.glyphs[id];
        glyph.x = (short)XmlNumber(at, "x");
        glyph.y = (short)XmlNumber(at, "y");
        glyph.width = (short)XmlNumber(at, "width");
        glyph.height = (short)XmlNumber(at, "height");
        glyph.xOffset = (short)XmlNumber(at, "xoffset");
        glyph.yOffset = (short)XmlNumber(at, "yoffset");
        glyph.advance = (short)XmlNumber(at, "xadvance");
        ++found;
    }
    for (const char* at = strstr(text, "<kerning "); at; at = strstr(at + 9, "<kerning "))
    {
        int first = XmlNumber(at, "first"), second = XmlNumber(at, "second");
        if (first < 32 || first > 126 || second < 32 || second > 126) continue;
        font.kerning[first][second] = (short)XmlNumber(at, "amount");
    }
    delete[] list;
    if (!common || found < 11 || font.line <= 0 || font.sheetWidth <= 0 || font.sheetHeight <= 0 ||
        font.sheetWidth > 4096 || font.sheetHeight > 4096)
        return false;

    // The sheet: a plain picture, one byte a pixel, after a header of 128 bytes.
    unsigned char* sheet = ReadGameFile("fonts\\work_56_0.dds", size);
    if (!sheet) return false;
    int pixels = font.sheetWidth * font.sheetHeight;
    bool plain = size >= 128 + pixels && memcmp(sheet, "DDS ", 4) == 0 && sheet[84] == 0 && sheet[88] == 8;
    if (plain)
    {
        font.sheet = new unsigned char[pixels];
        memcpy(font.sheet, sheet + 128, (size_t)pixels);
        font.state = 1;
    }
    delete[] sheet;
    return plain;
}

static int HintLetter(char c)
{
    int id = (unsigned char)c;
    return id < 32 || id > 126 ? '?' : id;
}

static const ScaledGlyph& HintGlyph(int id)
{
    ScaledGlyph& made = g_hintFont.scaled[id];
    if (!made.cover) ScaleGlyph(g_hintFont.glyphs[id], g_hintFont.sheet, g_hintFont.sheetWidth, g_hintFont.sheetHeight, kHintScale, made);
    return made;
}

// How wide the words are in the hint lettering, pixels of the picture.
static float HintFontWidth(const char* words)
{
    float units = 0.0f;
    int previous = -1;
    for (const char* s = words; *s; ++s)
    {
        int id = HintLetter(*s);
        if (previous >= 0) units += g_hintFont.kerning[previous][id];
        units += g_hintFont.glyphs[id].advance;
        previous = id;
    }
    return units * kHintScale;
}

int VrHintTextWidth(const char* words)
{
    if (LoadHintFont()) return (int)ceilf(HintFontWidth(words));
    return TextWidth(words);
}

bool VrHintFits(const char* words)
{
    return VrHintTextWidth(words) + kHintShadowStep <= kHintWidth - 2 * kHintMargin;
}

bool VrHintFontIsGames()
{
    return LoadHintFont();
}

const unsigned char* VrHintPicture(const char* words, int& width, int& height)
{
    width = kHintWidth;
    height = kHintHeight;
    memset(g_hintInk, 0, sizeof(g_hintInk));
    int textWidth = VrHintTextWidth(words);
    float pen = (float)((kHintWidth - textWidth - kHintShadowStep) / 2);
    if (pen < kHintMargin) pen = (float)kHintMargin;
    if (LoadHintFont())
    {
        int top = (int)((kHintHeight - g_hintFont.line * kHintScale) / 2.0f + 0.5f);
        int previous = -1;
        for (const char* s = words; *s; ++s)
        {
            int id = HintLetter(*s);
            if (previous >= 0) pen += g_hintFont.kerning[previous][id] * kHintScale;
            previous = id;
            const GameGlyph& glyph = g_hintFont.glyphs[id];
            const ScaledGlyph& made = HintGlyph(id);
            int x0 = (int)floorf(pen + glyph.xOffset * kHintScale + 0.5f), y0 = (int)floorf(top + glyph.yOffset * kHintScale + 0.5f);
            for (int py = 0; py < made.height; ++py)
                for (int px = 0; px < made.width; ++px)
                {
                    int x = x0 + px, y = y0 + py;
                    if (x < 0 || y < 0 || x >= kHintWidth || y >= kHintHeight) continue;
                    unsigned char cover = made.cover[py * made.width + px];
                    if (cover > g_hintInk[y * kHintWidth + x]) g_hintInk[y * kHintWidth + x] = cover;
                }
            pen += glyph.advance * kHintScale;
        }
    }
    else
    {
        int top = (kHintHeight - kFontHeight) / 2;
        for (const char* s = words; *s; ++s)
        {
            int index = (unsigned char)*s - kFontFirst;
            if (index < 0 || index >= kFontCount) index = '?' - kFontFirst;
            const FontGlyph& glyph = kFontGlyphs[index];
            const unsigned char* pixels = kFontPixels + glyph.offset;
            for (int row = 0; row < kFontHeight; ++row)
                for (int column = 0; column < glyph.width; ++column)
                {
                    int x = (int)pen + column, y = top + row;
                    if (x < 0 || y < 0 || x >= kHintWidth || y >= kHintHeight) continue;
                    unsigned char a = pixels[row * glyph.width + column];
                    if (a > g_hintInk[y * kHintWidth + x]) g_hintInk[y * kHintWidth + x] = a;
                }
            pen += glyph.advance;
        }
    }

    // White letters over their shadow (colors stored multiplied by how solid
    // they are).
    for (int y = 0; y < kHintHeight; ++y)
        for (int x = 0; x < kHintWidth; ++x)
        {
            int i = y * kHintWidth + x;
            int ink = g_hintInk[i];
            int sx = x - kHintShadowStep, sy = y - kHintShadowStep;
            int shadow = sx >= 0 && sy >= 0 ? g_hintInk[sy * kHintWidth + sx] * kHintShadowAlpha / 255 : 0;
            unsigned char* p = g_hintCanvas + i * 4;
            p[0] = p[1] = p[2] = (unsigned char)ink;
            p[3] = (unsigned char)(ink + shadow * (255 - ink) / 255);
        }
    return g_hintCanvas;
}
