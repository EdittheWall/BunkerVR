#include "scriptfuncs.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <MinHook.h>

#include "camera.h"
#include "hints.h"
#include "import_hook.h"
#include "log.h"
#include "pattern.h"
#include "sight.h"
#include "testlist.h"
#include "vr.h"
#include "vrmenu.h"

#ifndef BUNKERVR_VERSION_HUNDREDTHS
#error "BUNKERVR_VERSION_HUNDREDTHS comes from CMakeLists.txt (the project's VERSION, 0.90 as 90)"
#endif

// What the engine does:
// - Game::RegisterLuxGlobals registers the scripts' cLux_
//   functions, last of the game's registering functions, run once as the
//   game starts and before any script is compiled (the scripts call cLux_
//   functions). It is given the script handler; handler + 0x30 is the
//   engine's script system (hpl::cLowLevelScriptAS), and that + 0x80 is
//   AngelScript's engine (asIScriptEngine, AngelScript 2.31.0).
// - The AngelScript engine's slot 10 is RegisterGlobalFunction(declaration,
//   function pointer, calling convention, auxiliary), slot 11
//   GetGlobalFunctionCount(), as in AngelScript's source.
// - Its calling convention 0 is asCALL_CDECL: a plain function; the game's
//   AngelScript calls those directly (it is not built "max portability").
static const char* kRegisterLuxGlobalsPattern =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D AC 24 90 72 FF FF B8 70 8E 00 00 E8 ?? ?? ?? ?? "
    "48 2B E0 48 8B F9 48 8B 41 30 48 89 05";
static const int kHandlerScriptSystem = 0x30;
static const int kScriptSystemEngine = 0x80;
static const int kSlotRegisterGlobalFunction = 10;
static const int kSlotGetGlobalFunctionCount = 11;
static const unsigned kCallCdecl = 0;

// AngelScript's function pointer (asSFuncPtr): room for a pointer to a
// method (25 bytes, made 32), then what kind it is (2: a plain function).
// The game builds it so (the flag at +0x20).
struct ScriptFuncPtr
{
    void* function;
    char rest[24];
    unsigned char flag;
};
static_assert(offsetof(ScriptFuncPtr, flag) == 0x20, "AngelScript's asSFuncPtr has its flag at +0x20");

typedef int (*RegisterGlobalFunctionFn)(void* engine, const char* declaration, const ScriptFuncPtr& function, unsigned callConv, void* auxiliary);
typedef unsigned (*GetGlobalFunctionCountFn)(void* engine);
typedef unsigned long long (*RegisterLuxGlobalsFn)(void* handler); // (what it gives back, if anything, is passed on as it is)
typedef bool (*SteamApiInitFn)();

static RegisterLuxGlobalsFn g_realRegisterLuxGlobals = nullptr;
static SteamApiInitFn g_realSteamApiInit = nullptr;
static void* g_registeredEngine = nullptr; // the engine the mod's functions are in (added once per engine)

// ---------------------------------------------------------------------------
// The functions. The development tools' script checker declares them too
// (bunkervr_api.hps); keep the two in step.
// ---------------------------------------------------------------------------

// int cVR_Version(): the mod's version, in hundredths (0.90 is 90).
static int ScriptVersion()
{
    return BUNKERVR_VERSION_HUNDREDTHS;
}

// bool cVR_Active(): is the mod working, its head followed? The same as the
// answer to roll 7006 (12345 and 1): the head's pose is fresh, and the game
// is not on the cutscene screen.
static bool ScriptActive()
{
    HeadPose pose;
    return VrGetHeadCentrePose(pose) && !CameraTheatre();
}

// Notes and timing: the same messages as rolls 7070 to 7079 and 7100 to 7199,
// which camera.cpp still accepts. A roll also rebuilds the camera, and each
// rebuild of the player's camera at once follows a turn of it about the head
// (VrFollowTurn); these calls don't. (The arms' questions and the start of
// each drawing still rebuild it many times a frame, which does nothing when
// the camera has not turned.)

// void cVR_TimeMark(int alCode): a timing mark, with the roll's own number
// (7070 + 2 x the piece of work's number at its start, 7071 + 2 x it at its
// end; VRTimeMark in the scripts).
static void ScriptTimeMark(int code)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the first timing mark came by the direct line (cVR_TimeMark %d).", code);
    }
    CameraScriptTimeMark(code);
}

// void cVR_Note(int alNote, float afA, float afB, float afC): a note for the
// log, "SCRIPT NOTE n: a b c" (note 0 to 99, as roll 7100 + n).
static void ScriptNote(int note, float a, float b, float c)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the first note came by the direct line (cVR_Note %d).", note);
    }
    if (note < 0 || note > 99) return;
    CameraScriptNote(note, a, b, c);
}

// void cVR_ComfortRing(float afClosed): the comfort ring of the knockdowns'
// vignette and gentle view (no roll carries this message): how far the dark
// ring is closed, 0 to 1, sent every update while it is wanted
// (VrSetComfortRing; VRVignetteView and VRGentleView in the body script).
static void ScriptComfortRing(float closed)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the comfort ring was first set by the direct line (cVR_ComfortRing %.2f).", closed);
    }
    VrSetComfortRing(closed);
}

// void cVR_InterfaceInPieces(bool abInPlay): the body script reports, every
// update, whether the player is in play: the game's
// interface may then be shown in pieces (the subtitles below the eyes, the
// hint card above them) instead of the big panel (VrSetInterfaceInPieces).
static void ScriptInterfaceInPieces(bool inPlay)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the interface's pieces were first asked about by the direct line (cVR_InterfaceInPieces %s).", inPlay ? "true" : "false");
    }
    VrSetInterfaceInPieces(inPlay);
}

// void cVR_InterfaceRoom(float afDistance): the body script reports, every
// update in play, how far the view is clear in front, in the game's own units
// (-1: nothing within its reach): the interface's pieces are not put beyond it
// (VrSetInterfaceRoom, VRSendInterfaceRoom in PlayerBodyHandler.hps).
static void ScriptInterfaceRoom(float distance)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the room in front of the interface's pieces was first said by the direct line (cVR_InterfaceRoom %.2f).", distance);
    }
    VrSetInterfaceRoom(distance);
}

// void cVR_PanelFocus(float afAcross, float afDown): the sketchbook reports, every frame a photo is open in the
// inventory, where its middle is in the interface's picture (fractions of it across and down): the mod puts that in
// front instead of the book's middle (VrSetPanelFocus, SketchbookUIHandler.hps DoPhotosPage).
static void ScriptPanelFocus(float across, float down)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: a point of the interface to be put in front was first said by the direct line (cVR_PanelFocus %.3f, %.3f).", across, down);
    }
    VrSetPanelFocus(across, down);
}

// bool cVR_OtherHandWaits(): is the left hand waiting for the game's interact
// button, with the gun or an item ready in the right hand? The scripts then
// leave that state at once (VrOtherHandWaits; see
// PlayerState_UsingItem_Base.hps and PlayerState_UsingWeapon_Aim_Base.hps).
static bool ScriptOtherHandWaits()
{
    return VrOtherHandWaits();
}

// void cVR_MenuRoom(float afCameraAboveFloor): the menu script reports, every
// frame while the main menu is up, how far the game's camera stands above
// the room's floor (VrSetMenuRoom, VRSendMenuRoom in MenuHandler.hps).
static void ScriptMenuRoom(float cameraAboveFloor)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the main menu's room was first said by the direct line (cVR_MenuRoom %.2f).", cameraAboveFloor);
    }
    VrSetMenuRoom(cameraAboveFloor);
}

// void cVR_StorageBoxAt(float afAngle, float afDistance, float afHeight): the
// storage box's book is open, and stands at the box: its middle afAngle
// radians round from the way the head faces (plus: to the left), afDistance
// level and afHeight up from the head, in the game's own units; reported
// every frame it is drawn (VrSetStorageBoxAt; VRSendStorageBoxAt
// in SketchbookUIHandler.hps).
static void ScriptStorageBoxAt(float angle, float distance, float height)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the storage box's book was first placed by the direct line (cVR_StorageBoxAt %.2f %.2f %.2f).", angle, distance, height);
    }
    VrSetStorageBoxAt(angle, distance, height);
}

// Jumps to a place in the game, as the Test List page's "Go to Test Location"
// and the Places page make them (testlist.h; VRUpdateJump in MenuHandler.hps).

// bool cVR_JumpWanted(): has a jump been asked for? True once for each.
static bool ScriptJumpWanted()
{
    return TestJumpWanted();
}

// The game's strings use the release layout of std::string (the mod's own
// build has another), as the game's code reads them: 16 bytes
// holding the text or a pointer to it (a pointer if the capacity is over 15),
// its length, its capacity.
struct GameString
{
    union
    {
        char inside[16];
        const char* outside;
    };
    size_t size;
    size_t capacity;
};
static_assert(sizeof(GameString) == 32, "the game's std::string is 32 bytes");

// const tString &cVR_JumpText(int alPart): a part of the jump (see
// TestJumpPart), "" past the end. The scripts only read it, or copy it as soon
// as they have it: the same string is used for the next call, and a string in
// the mod's memory never goes to the game's freeing of memory, since this is a
// reference the script does not own.
static const GameString* ScriptJumpText(int part)
{
    static GameString text;
    const char* words = TestJumpPart(part);
    size_t length = strlen(words);
    text = {};
    text.size = length;
    if (length < sizeof(text.inside))
    {
        memcpy(text.inside, words, length + 1);
        text.capacity = sizeof(text.inside) - 1;
    }
    else
    {
        text.outside = words;
        text.capacity = length;
    }
    return &text;
}

// void cVR_JumpDone(): the jump has arrived; what it wants made and pressed is
// asked of the body script.
static void ScriptJumpDone()
{
    VrJumpArrived();
}

// const tString &cVR_SurveyName(int alIndex): a name from the survey list an automated
// test run leaves (the survey.txt read below), "" past the end: the things in a level
// whose height is to be measured (VRStandInSurvey). Read again each time index 0 is
// asked for. As with cVR_JumpText, the script must read it at once.
static const GameString* ScriptSurveyName(int index)
{
    static std::vector<std::string> names;
    static GameString text;
    if (index == 0)
    {
        names.clear();
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (length > 0 && path[length - 1] != L'\\') --length;
        path[length] = L'\0';
        FILE* file = nullptr;
        if (length > 0 && length + 40 < MAX_PATH && (lstrcatW(path, L"BunkerVR\\tools\\fakexr\\run\\survey.txt"), _wfopen_s(&file, path, L"rb") == 0) && file)
        {
            char line[256];
            while (fgets(line, sizeof(line), file))
            {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0] != '\0') names.push_back(line);
            }
            fclose(file);
        }
    }
    const char* words = index >= 0 && index < (int)names.size() ? names[index].c_str() : "";
    size_t size = strlen(words);
    text = {};
    text.size = size;
    if (size < sizeof(text.inside))
    {
        memcpy(text.inside, words, size + 1);
        text.capacity = sizeof(text.inside) - 1;
    }
    else
    {
        text.outside = words;
        text.capacity = size;
    }
    return &text;
}

// float cVR_SurveyNumber(int alIndex): the same list's line as a number, 0 past the
// end (VRStandInMoveTo: where an automated test run wants the player). The list as
// cVR_SurveyName last read it: ask that for index 0 first.
static float ScriptSurveyNumber(int index)
{
    const GameString* line = ScriptSurveyName(index);
    return (float)atof(line->capacity >= sizeof(line->inside) ? line->outside : line->inside);
}

// void cVR_Say(const tString &in asWords): a line in the mod's log. The game
// keeps its own log locked while it runs; this one can be read meanwhile (by an
// automated test run, for example), and has the time.
static void ScriptSay(const GameString* words)
{
    Log("SCRIPT SAYS: %s", words->capacity >= sizeof(words->inside) ? words->outside : words->inside);
}

// bool cVR_Jumping(): is a jump under way? The level scripts ask it as they
// start, to skip their openings.
static bool ScriptJumping()
{
    return TestJumping();
}

// bool cVR_JumpSkipsOpening(): should a level skip its opening? Yes while a jump is
// under way, unless that jump asked to keep the opening (so that it can be watched).
static bool ScriptJumpSkipsOpening()
{
    return TestJumpSkipsOpening();
}

// bool cVR_JumpedThisRun(): has this run jumped, with no real game started
// since? Nothing is saved then (SaveRoomHandler.hps).
static bool ScriptJumpedThisRun()
{
    return TestJumpedThisRun();
}

// void cVR_JumpsOver(): a real game has been started or loaded.
static void ScriptJumpsOver()
{
    TestJumpsOver();
}

// bool cVR_JumpAgain(): a game over after a jump (GameOverHandler.hps
// VRJumpRestart): the last jump under way again, for the script to start its level
// afresh; false if this run has made no jump (TestJumpAgain).
static bool ScriptJumpAgain()
{
    return TestJumpAgain();
}

// bool cVR_RoomBlurred(): is the room to be blurred behind the mod's start-up
// page (MenuHandler.hps VRUpdateStartupBlur)? The mod darkens it.
static bool ScriptRoomBlurred()
{
    return VrStartupShown();
}

// void cVR_MenuInView(): the menu script reports, every frame while the main
// menu is all there: its level's fade from black over, its title shown, its
// first page up (MenuHandler.hps VRSendMenuRoom). The start-up page
// waits for it.
static void ScriptMenuInView()
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the main menu was first said to be all in view by the direct line (cVR_MenuInView).");
    }
    VrMenuInView();
}

// void cVR_MenuFocus(): a button of the game's menus has just come into focus,
// as its focus sound plays (OptionMenu_UpdateFocus in
// helper_imgui_options.hps, OptionMenu_UpdateFocusCustom in MenuHandler.hps):
// a light buzz in the hand that points, and the cursor swells.
static void ScriptMenuFocus()
{
    VrMenuFocus();
}

// float cVR_HandSqueeze(int alHand): how far a hand's grip button is squeezed
// (0 left, 1 right), from 0 to 1, also while it holds an item (a fuse in its
// box is twisted while the grip is held; VrGetGripSqueeze).
static float ScriptHandSqueeze(int hand)
{
    return VrGetGripSqueeze(hand);
}

// float cVR_HandTrigger(int alHand): how far a hand's trigger is pulled, 0 to 1 (the pocket watch,
// VRUpdateWatch in PlayerBodyHandler.hps; VrGetTrigger).
static float ScriptHandTrigger(int hand)
{
    return VrGetTrigger(hand);
}

// void cVR_RightFitted(): the body script reports, every update while the
// right hand's item is fitted into what it is used on (VRUpdateFit
// in PlayerBodyHandler.hps): a right squeeze is then the fit's own, not the
// game's interact button (VrRightFitted).
static void ScriptRightFitted()
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: an item in the right hand was first said to be fitted by the direct line (cVR_RightFitted): its squeeze is the fit's own meanwhile.");
    }
    VrRightFitted();
}

// void cVR_LeftAtPump(bool abAt): the shotgun's script reports, every update
// while the shotgun is held (Prop_Shotgun.hps): is the left palm at
// its fore-end? A left squeeze there then takes the pump (VrLeftAtPump).
static void ScriptLeftAtPump(bool at)
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the shotgun first said where the left hand is by the direct line (cVR_LeftAtPump %s).", at ? "true" : "false");
    }
    VrLeftAtPump(at);
}

// void cVR_NoTurning(bool abNone): the body script reports, every update while a
// knockdown, a kill or a death plays: no turn of the sticks reaches
// the game meanwhile (VrNoTurning).
static void ScriptNoTurning(bool none)
{
    static bool logged = false;
    if (!logged && none)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the body script first said a knockdown, a kill or a death plays (cVR_NoTurning): the sticks turn nothing meanwhile.");
    }
    VrNoTurning(none);
}

// bool cVR_BeastAway(): the menu's Places page has the Beast kept out of the level, so that
// places can be jumped to and looked at safely: the body script switches the game's Beasts
// off while this says so, and back on after.
static bool ScriptBeastAway()
{
    return VrBeastAway();
}

// bool cVR_StudioCardHolds(): is the mod's studio card at the start holding the
// game back (waiting for the headset to be put on, or shown)? The menu script
// holds the game's own start, its logos, until it is not. The game's own
// developer logos always play after the card; never hide them.
static bool ScriptStudioCardHolds()
{
    return VrStudioCardHolds();
}

// bool cVR_HoldHeadTurn(bool abHold): the body script reports, every update while
// the Beast's kill from the front plays: the view does not turn with the head
// (VrHoldHeadTurn). True once this frame's pictures are drawn so.
static bool ScriptHoldHeadTurn(bool hold)
{
    static bool logged = false;
    if (!logged && hold)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the body script first asked for the head's turn to be held (cVR_HoldHeadTurn).");
    }
    return VrHoldHeadTurn(hold);
}

// void cVR_BeastSight(bool abOn, float afUp): the body script reports, every update,
// whether the real head counts for the creatures' sight of the player now, and how
// far it is above (+) or below (-) the game's own eye, in game meters (sight.cpp).
static void ScriptBeastSight(bool on, float up)
{
    SightFromScript(on, up);
}

// The hints on the objects (hints.h). The clock is vr.cpp's own
// (the same counter, in milliseconds).
static double HintClockMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static const char* GameStringText(const GameString* words)
{
    return words->capacity >= sizeof(words->inside) ? words->outside : words->inside;
}

// void cVR_Hint(int alHand, const tString &in asKey, float afX, float afY, float afZ):
// this hand (0 left, 1 right) is at what the hint asKey is for, at that place in
// the game's world (the label goes a little above it). Reported every update while it
// is so (VRUpdateHints in PlayerBodyHandler.hps).
static void ScriptHint(int hand, const GameString* key, float x, float y, float z)
{
    const float world[3] = { x, y, z };
    HintFromScript(hand, GameStringText(key), world, HintClockMs());
}

// bool cVR_HintWanted(const tString &in asKey): would that hint show now (the
// menu's Interaction Hints, and how often it has been used)? A script asks
// before choosing between an item's hints.
static bool ScriptHintWanted(const GameString* key)
{
    return HintWanted(GameStringText(key));
}

// void cVR_HintUsed(const tString &in asKey): what that hint teaches has just
// been done (a key turned, a bolt loosened...).
static void ScriptHintUsed(const GameString* key)
{
    HintUsed(GameStringText(key), HintClockMs());
}

// void cVR_HintCarried(int alFlags): what the player carries, every update
// (kCarryRevolver and the rest in hints.h), for the mod's own hints at the
// holster, the pouch and the chest.
static void ScriptHintCarried(int flags)
{
    HintCarried(flags, HintClockMs());
}

// void cVR_NewGame(): a new game has just been started from the main menu
// (MenuHandler.hps: StartNewGame, for the New Game button and the test maps'
// own start, and VRJumpStartGameAt, a jump from the main menu). Every hint is
// forgotten, so with Until Learned each shows again until it has been used
// three times in this game. Continuing or loading a saved game keeps what was
// learned.
static void ScriptNewGame()
{
    Log("SCRIPT FUNCTIONS: a new game was started (cVR_NewGame): the hints start again.");
    HintsForget();
}

// void cVR_Hurt(float afAmount, int alKind, float afX, float afY, float afZ): the player is hurt (the game's
// OnDamageTaken, Player_Custom.hps): how hard, and where from (alKind 1: a direction from the camera, x right,
// y up, z ahead; 2: from below, a fall; 0: all round). The hit indicator (vr.cpp VrHurt).
static void ScriptHurt(float amount, int kind, float x, float y, float z)
{
    VrHurt(amount, kind, x, y, z);
}

struct ScriptFunction
{
    const char* declaration;
    void* function;
};
static const ScriptFunction kFunctions[] = {
    { "int cVR_Version()", (void*)&ScriptVersion },
    { "bool cVR_Active()", (void*)&ScriptActive },
    { "void cVR_TimeMark(int alCode)", (void*)&ScriptTimeMark },
    { "void cVR_Note(int alNote, float afA, float afB, float afC)", (void*)&ScriptNote },
    { "void cVR_ComfortRing(float afClosed)", (void*)&ScriptComfortRing },
    { "void cVR_InterfaceInPieces(bool abInPlay)", (void*)&ScriptInterfaceInPieces },
    { "void cVR_StorageBoxAt(float afAngle, float afDistance, float afHeight)", (void*)&ScriptStorageBoxAt },
    { "void cVR_InterfaceRoom(float afDistance)", (void*)&ScriptInterfaceRoom },
    { "void cVR_PanelFocus(float afAcross, float afDown)", (void*)&ScriptPanelFocus },
    { "bool cVR_OtherHandWaits()", (void*)&ScriptOtherHandWaits },
    { "void cVR_MenuRoom(float afCameraAboveFloor)", (void*)&ScriptMenuRoom },
    { "bool cVR_JumpWanted()", (void*)&ScriptJumpWanted },
    { "const tString& cVR_JumpText(int alPart)", (void*)&ScriptJumpText },
    { "void cVR_JumpDone()", (void*)&ScriptJumpDone },
    { "void cVR_Say(const tString &in asWords)", (void*)&ScriptSay },
    { "const tString& cVR_SurveyName(int alIndex)", (void*)&ScriptSurveyName },
    { "float cVR_SurveyNumber(int alIndex)", (void*)&ScriptSurveyNumber },
    { "bool cVR_Jumping()", (void*)&ScriptJumping },
    { "bool cVR_JumpSkipsOpening()", (void*)&ScriptJumpSkipsOpening },
    { "bool cVR_JumpedThisRun()", (void*)&ScriptJumpedThisRun },
    { "void cVR_JumpsOver()", (void*)&ScriptJumpsOver },
    { "bool cVR_JumpAgain()", (void*)&ScriptJumpAgain },
    { "bool cVR_RoomBlurred()", (void*)&ScriptRoomBlurred },
    { "void cVR_MenuInView()", (void*)&ScriptMenuInView },
    { "void cVR_MenuFocus()", (void*)&ScriptMenuFocus },
    { "float cVR_HandSqueeze(int alHand)", (void*)&ScriptHandSqueeze },
    { "float cVR_HandTrigger(int alHand)", (void*)&ScriptHandTrigger },
    { "void cVR_RightFitted()", (void*)&ScriptRightFitted },
    { "void cVR_LeftAtPump(bool abAt)", (void*)&ScriptLeftAtPump },
    { "void cVR_Hint(int alHand, const tString &in asKey, float afX, float afY, float afZ)", (void*)&ScriptHint },
    { "bool cVR_HintWanted(const tString &in asKey)", (void*)&ScriptHintWanted },
    { "void cVR_HintUsed(const tString &in asKey)", (void*)&ScriptHintUsed },
    { "void cVR_HintCarried(int alFlags)", (void*)&ScriptHintCarried },
    { "void cVR_NewGame()", (void*)&ScriptNewGame },
    { "void cVR_NoTurning(bool abNone)", (void*)&ScriptNoTurning },
    { "bool cVR_HoldHeadTurn(bool abHold)", (void*)&ScriptHoldHeadTurn },
    { "bool cVR_BeastAway()", (void*)&ScriptBeastAway },
    { "void cVR_BeastSight(bool abOn, float afUp)", (void*)&ScriptBeastSight },
    { "bool cVR_StudioCardHolds()", (void*)&ScriptStudioCardHolds },
    { "void cVR_Hurt(float afAmount, int alKind, float afX, float afY, float afZ)", (void*)&ScriptHurt },
};

// ---------------------------------------------------------------------------

static void RegisterOurFunctions(void* handler)
{
    if (!handler)
    {
        Log("SCRIPT FUNCTIONS ERROR: the game's script handler was not given; the mod's script functions were not added.");
        return;
    }
    void* system = *(void**)((char*)handler + kHandlerScriptSystem);
    void* engine = system ? *(void**)((char*)system + kScriptSystemEngine) : nullptr;
    if (!engine)
    {
        Log("SCRIPT FUNCTIONS ERROR: no script engine found (handler %p, script system %p); the mod's script functions were not added.", handler, system);
        return;
    }
    if (engine == g_registeredEngine) return;

    void** table = *(void***)engine;
    // The engine's functions must be in the game's own code.
    HMODULE game = GetModuleHandleW(nullptr);
    IMAGE_NT_HEADERS* headers = (IMAGE_NT_HEADERS*)((char*)game + ((IMAGE_DOS_HEADER*)game)->e_lfanew);
    char* codeStart = (char*)game;
    char* codeEnd = codeStart + headers->OptionalHeader.SizeOfImage;
    void* registerSlot = table[kSlotRegisterGlobalFunction];
    void* countSlot = table[kSlotGetGlobalFunctionCount];
    if ((char*)registerSlot < codeStart || (char*)registerSlot >= codeEnd || (char*)countSlot < codeStart || (char*)countSlot >= codeEnd)
    {
        Log("SCRIPT FUNCTIONS ERROR: the script engine's table does not point into the game (%p, %p); the mod's script functions were not added.",
            registerSlot, countSlot);
        return;
    }
    RegisterGlobalFunctionFn registerGlobalFunction = (RegisterGlobalFunctionFn)registerSlot;
    GetGlobalFunctionCountFn getGlobalFunctionCount = (GetGlobalFunctionCountFn)countSlot;

    unsigned before = getGlobalFunctionCount(engine);
    int added = 0;
    for (const ScriptFunction& f : kFunctions)
    {
        ScriptFuncPtr pointer = {};
        pointer.function = f.function;
        pointer.flag = 2;
        int result = registerGlobalFunction(engine, f.declaration, pointer, kCallCdecl, nullptr);
        if (result < 0) Log("SCRIPT FUNCTIONS ERROR: could not add \"%s\" (AngelScript's answer %d).", f.declaration, result);
        else ++added;
    }
    unsigned after = getGlobalFunctionCount(engine);
    g_registeredEngine = engine;
    Log("SCRIPT FUNCTIONS: %d of %d added to the game's script language (its global functions %u -> %u)%s.", added,
        (int)(sizeof(kFunctions) / sizeof(kFunctions[0])), before, after,
        after == before + (unsigned)added ? "" : " -- NOT the count expected");
}

static unsigned long long HookedRegisterLuxGlobals(void* handler)
{
    unsigned long long result = g_realRegisterLuxGlobals(handler); // the game's own first
    RegisterOurFunctions(handler);
    return result;
}

// The game's main calls SteamAPI_Init first, on its own thread, before it
// sets up its scripts: the hook on the registering function goes in here.
static bool HookedSteamApiInit()
{
    static bool done = false;
    if (!done)
    {
        done = true;
        HMODULE game = GetModuleHandleW(nullptr);
        int matches = 0;
        void* target = FindPattern(game, kRegisterLuxGlobalsPattern, &matches);
        if (matches != 1)
        {
            Log("SCRIPT FUNCTIONS ERROR: expected to find the game's cLux_ registering function exactly once, found it %d times. "
                "The game may have been updated; the mod's script functions are not added.", matches);
        }
        else
        {
            MH_STATUS status = MH_Initialize();
            if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
                status = MH_CreateHook(target, (LPVOID)HookedRegisterLuxGlobals, (LPVOID*)&g_realRegisterLuxGlobals);
            if (status == MH_OK) status = MH_EnableHook(target);
            if (status == MH_OK)
                Log("SCRIPT FUNCTIONS: hooked the game's cLux_ registering function at exe+0x%llX.",
                    (unsigned long long)((char*)target - (char*)game));
            else
                Log("SCRIPT FUNCTIONS ERROR: could not hook the game's cLux_ registering function: %s", MH_StatusToString(status));
        }
    }
    return g_realSteamApiInit();
}

void InstallScriptFunctions()
{
    HMODULE game = GetModuleHandleW(nullptr);
    if (HookImport(game, "steam_api64.dll", "SteamAPI_Init", (void*)HookedSteamApiInit, (void**)&g_realSteamApiInit))
        Log("SCRIPT FUNCTIONS: waiting for the game to start Steam (then the mod's script functions are added).");
    else
        Log("SCRIPT FUNCTIONS ERROR: the game does not import SteamAPI_Init; the mod's script functions are not added.");
}
