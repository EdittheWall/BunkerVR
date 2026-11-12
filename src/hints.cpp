#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hints.h"
#include "log.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// The table. Each hint says how the mod really works the thing, as VR games
// word their hints: the button, then what it does. Each label must fit on one
// line (VrHintFits in vrmenu.cpp measures it).
// ---------------------------------------------------------------------------

static const HintInfo kHints[] = {
    // What an empty hand is at (the scripts: VRUpdateHints, PlayerBodyHandler.hps).
    { "handle",       "Grip: Hold, then pull or push",       kHintUseSqueeze, 0.07f }, // doors, drawers, hatches (either squeeze takes hold)
    { "lever",        "Grip: Hold, then pull it",            kHintUseSqueeze, 0.07f },
    { "wheel",        "Grip: Hold, then turn it",            kHintUseSqueeze, 0.07f },
    { "take",         "Grip: Take",                          kHintUseSqueeze, 0.07f }, // a pick-up: into the inventory or the hand
    { "pick_up",      "Grip: Pick up",                       kHintUseSqueeze, 0.07f }, // a loose thing
    { "crate",        "Grip: Hold, then move it",            kHintUseSqueeze, 0.07f }, // crates and barrels
    { "button",       "Hand: Push it in",                    kHintUseSqueeze, 0.07f }, // pressed by the hand pushed in, or a squeeze
    { "press",        "Grip: Use it",                        kHintUseSqueeze, 0.07f }, // other buttons, switches, lamps (the game's own press)
    { "box",          "Grip: Open the storage box",          kHintUseSqueeze, 0.07f },
    { "note",         "Grip: Take the note",                 kHintUseSqueeze, 0.07f },
    { "dogtag",       "Grip: Take, then turn it to read",    kHintUseSqueeze, 0.07f },
    { "ladder",       "Grip: Climb on",                      kHintUseSqueeze, 0.07f },
    { "ladder_climb", "Grip a rung and pull: Climb",         kHintUseTold,    0.07f }, // on the ladder, hand over hand (the stick also climbs)
    { "map",          "Grip: Add it to your map",            kHintUseSqueeze, 0.07f }, // the wall maps
    { "exit",         "Grip: Go through",                    kHintUseSqueeze, 0.07f },
    { "throw",        "Swing and let go: Throw",             kHintUseTold,    0.08f }, // told as a thing is thrown (the mod's or the scripts')
    { "lock_touch",   "Fingertip: Touch a wheel",            kHintUseTold,    0.06f }, // a number lock
    { "lock_turn",    "Trigger: Turn this wheel",            kHintUseSqueeze, 0.05f },
    { "tear_pull",    "Pull hard: Tear it off",              kHintUseTold,    0.08f }, // a nailed plank held (PlayerState_Interact_Tear.hps)
    // The items' own (the scripts).
    { "pump_hold",    "Grip: Hold the pump",                 kHintUseSqueeze, 0.06f }, // the shotgun
    { "pump_work",    "Pull back, push forward: Pump",       kHintUseTold,    0.06f },
    { "shell_load",   "Push into the port below: Load",      kHintUseTold,    0.06f },
    { "wrench_fit",   "Push it onto a bolt: Fit",            kHintUseTold,    0.08f }, // the wrench
    { "wrench_turn",  "Hold Grip: Crank it anticlockwise",   kHintUseTold,    0.08f },
    { "cutters_hold", "Grip: Hold the other handle",         kHintUseSqueeze, 0.06f }, // the cutters
    { "cutters_aim",  "Open the jaws round a chain or lock", kHintUseTold,    0.08f },
    { "cutters_cut",  "Bring your hands together: Cut",      kHintUseTold,    0.08f },
    { "key_in",       "Push it into the lock",               kHintUseTold,    0.08f }, // the keys
    { "key_turn",     "Turn your wrist right: Unlock",       kHintUseTold,    0.08f },
    { "fuse_in",      "Push it into the fuse box",           kHintUseTold,    0.08f }, // the fuse
    { "fuse_turn",    "Hold Grip: Twist right to screw in",  kHintUseTold,    0.08f },
    { "lid_open",     "Grip: Open it",                       kHintUseSqueeze, 0.06f }, // the fuel can's lid, the bottle's cork
    { "pour",         "Tip it over the opening: Pour",       kHintUseTold,    0.08f },
    { "tap_turn",     "Left Grip, turn the handle: Fill",    kHintUseTold,    0.07f }, // the fuel tap's lever, an empty bottle held near it
    { "det_fit",      "Bring it to the detonator's top: Fit", kHintUseTold,   0.08f }, // the finale by hand: the detonator handle held near it
    { "det_nut",      "Pinch a wing nut, turn right: Tighten", kHintUseTold,  0.05f }, // ...its two wing nuts, a free hand near (trigger or grip)
    { "dyn_place",    "Lower it here: Set it down",          kHintUseTold,    0.08f }, // ...the dynamite bundle held near its place
    { "cable_take",   "Pinch: Take the cord's end",          kHintUseTold,    0.05f }, // ...the cord's free end, by the bundle (the cord is a rope)
    { "cable_on",     "Bring it to the dynamite: Connect",   kHintUseTold,    0.06f }, // ...the cord's end held: where it goes
    { "det_push",     "Both fists, push down hard: Fire",    kHintUseTold,    0.08f }, // ...the T-bar, at the end each hand would take (both hands)
    { "fuel_splash",  "Shove it out: Throw fuel",            kHintUseTold,    0.08f }, // the open fuel can away from where it is poured: a shove with both hands
    { "fuel_hold",    "Left Grip underneath: Hold it",       kHintUseSqueeze, 0.06f }, // the fuel can's bottom or end, for the left hand
    { "flare_cord",   "Left Grip: Pull the cord",            kHintUseTold,    -0.06f }, // the flare's cap, at the end of its handle: below it, the flare above
    { "light_it",     "Hold a flame to it: Light",           kHintUseTold,    0.06f }, // a molotov's cloth, a torch's head
    { "torch_out",    "Hold it out: Keep rats away",         kHintUseTold,    0.10f },
    { "watch_wind",   "Trigger: Wind and start",             kHintUseTold,    0.06f }, // the pocket watch (the right trigger)
    { "watch_press",  "Press the crown: Start",              kHintUseTold,    0.06f },
    { "mask_on",      "Hold it to your face: Put on",        kHintUseTold,    0.08f }, // the gas mask
    { "mask_off",     "Grip: Pull it off your face",         kHintUseTold,    0.10f },
    // The mod's own (UpdateHints in vr.cpp): the places on the body, the
    // revolver, the grenade, the lighter, healing.
    { "holster_draw", "Grip: Draw the revolver",             kHintUseGrip,    0.12f },
    { "holster_put",  "Grip: Holster the revolver",          kHintUseGrip,    0.12f },
    { "pouch_bullet", "Grip: Take a bullet",                 kHintUseSqueeze, 0.075f }, // (just above the pouch's count, 8 cm tall round the spot)
    { "pouch_shell",  "Grip: Take a shell",                  kHintUseSqueeze, 0.075f },
    { "light_take",   "Grip: Take the flashlight",           kHintUseSqueeze, 0.10f },
    { "light_put",    "Grip: Put the flashlight away",       kHintUseSqueeze, 0.10f },
    { "slot_take",    "Grip: Take out your chest item",      kHintUseTold,    0.10f }, // the right chest's item slot
    { "slot_put",     "Grip: Store it on your chest",        kHintUseTold,    0.10f },
    { "slot_let_go",  "Let go: Store it on your chest",      kHintUseTold,    0.10f }, // (a thing held by the grip to throw)
    { "gun_fire",     "Trigger: Fire",                       kHintUseTrigger, -0.08f }, // (below the hand: the sights stay clear)
    { "gun_open",     "Hold A: Flick sideways to open",      kHintUseTold,    -0.08f },
    { "gun_tip",      "Tip it up, shake: Drop the shells",   kHintUseTold,    -0.08f }, // (the cases stick a little; a shake frees them)
    { "gun_close",    "Flick it back: Close",                kHintUseTold,    -0.08f },
    { "pull_pin",     "Left Grip: Pull out the pin",         kHintUseTold,    0.08f },
    { "lighter",      "Trigger: Light",                      kHintUseTrigger, 0.08f },
    { "bandage_open", "Left Grip: Pull it open",             kHintUseTold,    0.08f },
    { "bandage_wind", "Circle your left forearm: Wrap it",   kHintUseTold,    0.10f },
};
static const int kHintTotal = (int)(sizeof(kHints) / sizeof(kHints[0]));

int HintCount()
{
    return kHintTotal;
}

const HintInfo* HintAt(int index)
{
    return index >= 0 && index < kHintTotal ? &kHints[index] : nullptr;
}

const HintInfo* HintFind(const char* key)
{
    if (!key || !key[0]) return nullptr;
    for (const HintInfo& h : kHints)
        if (strcmp(h.key, key) == 0) return &h;
    return nullptr;
}

// A key not in the table: logged once each (up to 20 of them).
static void NoteUnknownKey(const char* key)
{
    static char told[20][32];
    static int toldCount = 0;
    for (int i = 0; i < toldCount; ++i)
        if (strncmp(told[i], key, sizeof(told[i]) - 1) == 0) return;
    if (toldCount >= 20) return;
    snprintf(told[toldCount++], sizeof(told[0]), "%s", key);
    Log("HINTS: \"%s\" is not a hint the mod knows (hints.cpp): left out.", key);
}

// ---------------------------------------------------------------------------
// How often each has been used: BunkerVR_hints.txt, beside the game, a line
// "key=times" each. Read once, written as a count changes.
// ---------------------------------------------------------------------------

static int g_used[sizeof(kHints) / sizeof(kHints[0])] = {};
static double g_usedAtMs[sizeof(kHints) / sizeof(kHints[0])] = {};
static bool g_loaded = false;
static const HintInfo* g_lastUsed = nullptr;
static double g_lastUsedMs = -100000.0;

static bool HintsPath(wchar_t* path)
{
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    if (length + 24 >= MAX_PATH) return false;
    lstrcatW(path, L"BunkerVR_hints.txt");
    return true;
}

static void LoadUsed()
{
    if (g_loaded) return;
    g_loaded = true;
    for (int i = 0; i < kHintTotal; ++i) g_usedAtMs[i] = -100000.0;
    wchar_t path[MAX_PATH];
    if (!HintsPath(path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return;
    char line[128];
    int known = 0;
    while (fgets(line, sizeof(line), file))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char* equals = strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        int times = 0;
        if (sscanf(equals + 1, "%d", &times) != 1 || times < 0) continue;
        for (int i = 0; i < kHintTotal; ++i)
        {
            if (strcmp(kHints[i].key, line) != 0) continue;
            g_used[i] = times > 1000 ? 1000 : times;
            ++known;
        }
    }
    fclose(file);
    int learned = 0;
    for (int i = 0; i < kHintTotal; ++i)
        if (g_used[i] >= kHintLearnedAfter) ++learned;
    Log("HINTS: %d of the %d hints have been used before (%d learned).", known, kHintTotal, learned);
}

static void SaveUsed()
{
    wchar_t path[MAX_PATH];
    if (!HintsPath(path)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) return;
    for (int i = 0; i < kHintTotal; ++i)
        if (g_used[i] > 0) fprintf(file, "%s=%d\r\n", kHints[i].key, g_used[i]);
    fclose(file);
}

bool HintWanted(const HintInfo* info)
{
    if (!info) return false;
    int mode = GetSettings().interactionHints;
    if (mode == 2) return false;  // Off
    if (mode == 1) return true;   // Always
    LoadUsed();
    return g_used[info - kHints] < kHintLearnedAfter;
}

bool HintWanted(const char* key)
{
    const HintInfo* info = HintFind(key);
    if (!info && key && key[0]) NoteUnknownKey(key);
    return HintWanted(info);
}

void HintUsed(const HintInfo* info, double nowMs)
{
    if (!info) return;
    LoadUsed();
    int i = (int)(info - kHints);
    g_lastUsed = info;
    g_lastUsedMs = nowMs;
    if (nowMs - g_usedAtMs[i] < 1500.0) return;
    g_usedAtMs[i] = nowMs;
    if (g_used[i] < 1000) ++g_used[i];
    SaveUsed();
    static int logsLeft = 60;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("HINTS: \"%s\" used (%d time%s%s).", info->key, g_used[i], g_used[i] == 1 ? "" : "s",
            g_used[i] == kHintLearnedAfter ? ": learned, it no longer shows with Until Learned" : "");
    }
}

void HintUsed(const char* key, double nowMs)
{
    const HintInfo* info = HintFind(key);
    if (!info)
    {
        if (key && key[0]) NoteUnknownKey(key);
        return;
    }
    HintUsed(info, nowMs);
}

int HintTimesUsed(const HintInfo* info)
{
    if (!info) return 0;
    LoadUsed();
    return g_used[info - kHints];
}

void HintsForget()
{
    LoadUsed();
    for (int i = 0; i < kHintTotal; ++i)
    {
        g_used[i] = 0;
        g_usedAtMs[i] = -100000.0;
    }
    SaveUsed();
    Log("HINTS: every hint forgotten: each shows again until used %d times.", kHintLearnedAfter);
}

const HintInfo* HintLastUsed(double& atMs)
{
    atMs = g_lastUsedMs;
    return g_lastUsed;
}

// ---------------------------------------------------------------------------
// What the scripts report.
// ---------------------------------------------------------------------------

static const HintInfo* g_scriptHint[2] = { nullptr, nullptr };
static float g_scriptWorld[2][3] = {};
static double g_scriptMs[2] = { -100000.0, -100000.0 };
static int g_carried = 0;
static double g_carriedMs = -100000.0;

void HintFromScript(int hand, const char* key, const float world[3], double nowMs)
{
    if (hand < 0 || hand > 1) return;
    const HintInfo* info = HintFind(key);
    if (!info)
    {
        if (key && key[0]) NoteUnknownKey(key);
        return;
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCRIPT FUNCTIONS: the first hint came by the direct line (cVR_Hint %d, \"%s\").", hand, key);
    }
    g_scriptHint[hand] = info;
    for (int i = 0; i < 3; ++i) g_scriptWorld[hand][i] = world[i];
    g_scriptMs[hand] = nowMs;
}

bool HintScriptSays(int hand, double nowMs, const HintInfo*& info, float world[3])
{
    info = nullptr;
    if (hand < 0 || hand > 1 || !g_scriptHint[hand] || nowMs - g_scriptMs[hand] > 250.0 || nowMs < g_scriptMs[hand]) return false;
    info = g_scriptHint[hand];
    for (int i = 0; i < 3; ++i) world[i] = g_scriptWorld[hand][i];
    return true;
}

void HintCarried(int flags, double nowMs)
{
    g_carried = flags;
    g_carriedMs = nowMs;
}

int HintCarriedFlags(double nowMs)
{
    return nowMs - g_carriedMs < 1000.0 && nowMs >= g_carriedMs ? g_carried : 0;
}
