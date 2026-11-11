#include "sight.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>

#include <MinHook.h>

#include "log.h"
#include "pattern.h"
#include "sight_column.h"

// ---------------------------------------------------------------------------
// The engine functions. Each pattern must match exactly once.
// ---------------------------------------------------------------------------

// cLuxAgent::GetPlayerHeadPos: the top of the game's body plus the player's
// offset, with x and z at the body's center. Writes the given vector and
// returns it; the creature itself isn't used.
static const char* kHeadPosPattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 0D ?? ?? ?? ?? 48 8B F2 0F 29 74 24 30 0F 29 7C 24 20 "
    "48 8B 89 70 01 00 00 E8 ?? ?? ?? ?? 48";
// cLuxAgent::GetPlayerIsInLineOfSight(float afFOV, const cVector3f &in avForward, bool abCheckFOV):
// answers at once. The engine's own code calls it too.
static const char* kLookNowFovPattern =
    "40 57 48 83 EC 70 48 8B F9 0F 57 C0 48 8B 0D ?? ?? ?? ?? 48 8B 81 70 01 00 00 0F 2F 80 3C 02 00 00 72 09 "
    "80 B8 40 02 00 00 00 75 11 45 84 C9 74";
// cLuxAgent::GetPlayerIsInLineOfSight(): answers at once (used by the rats).
static const char* kLookNowPattern =
    "40 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 8B F9 0F 57 C0 48 8B 88 70 01 00 00 0F 2F 81 3C 02 00 00 72 11 "
    "80 B9 40 02 00 00 00 74 08 32 C0 48 83";
// cLuxAgent::GetPlayerIsInLineOfSight(const tString&in asCallbackFunc): answers later, through a callback in
// the creature's script (the Beast checks every 0.3 to 0.6 s in agent_stalker.hps UpdateCheckLos).
static const char* kLookLaterPattern =
    "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC B0 00 00 00 48 8B 01 48 8B DA 48 8B F9 FF 50 20 48 8B F0 48 85 "
    "C0 0F 84 ?? ?? ?? ?? 48 83 7B 18 10 48";
// cLuxAgent::GetPlayerIsInLineOfSight(const tString&in asCallbackFunc, float afFOV, const cVector3f &in
// avForward, bool abCheckFOV): the same, with a field of view.
static const char* kLookLaterFovPattern =
    "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 48 81 EC B0 00 00 00 0F 29 70 E8 49 8B E9 48 8B 01 0F 28 "
    "F2 48 8B DA 48 8B F9 FF 50 20 48 8B F0";
// The column test, answered at once: up to 12 rays from an eye to a column (its center and full size); two
// clear rays mean seen. The engine runs a similar test for the player looking at a creature, which is left
// alone: only a column test started inside one of the four checks above is moved.
static const char* kColumnNowPattern =
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 8D 68 D8 48 81 EC 10 01 00 00 "
    "F3 41 0F 10 20 49 8B D9 F3 0F 5C 22 F3";
// The column test, answered later: the same rays, handed to the physics to answer. The column itself is read
// immediately, since the rays are built before the function returns. The pattern is longer than the others
// because its first 54 bytes also match another function.
static const char* kColumnLaterPattern =
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 98 48 81 EC 60 01 00 00 0F 29 70 E8 "
    "0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 44 0F 29 50 A8 44 0F 29 58 98 44 0F 29 60 88 44 0F 29 A8 78 FF "
    "FF FF 44 0F 29 B0 68 FF FF FF 44 0F 29 B8 58 FF FF FF 49 8B D9 49 8B F0 4C 8B F2 F3 41 0F 10 20 F3 0F 5C 22";

typedef float* (*HeadPosFn)(void* agent, float* out);
typedef unsigned long long (*LookNowFovFn)(void* agent, float fov, const float* forward, unsigned char checkFov);
typedef unsigned long long (*LookNowFn)(void* agent);
typedef void (*LookLaterFn)(void* agent, const void* callback);
typedef void (*LookLaterFovFn)(void* agent, const void* callback, float fov, const float* forward, unsigned char checkFov);
typedef unsigned long long (*ColumnNowFn)(void* agent, const float* eye, const float* centre, const float* size);
typedef void (*ColumnLaterFn)(void* agent, const float* eye, const float* centre, const float* size, void* answer, unsigned int flags);

static HeadPosFn g_realHeadPos = nullptr;
static LookNowFovFn g_realLookNowFov = nullptr;
static LookNowFn g_realLookNow = nullptr;
static LookLaterFn g_realLookLater = nullptr;
static LookLaterFovFn g_realLookLaterFov = nullptr;
static ColumnNowFn g_realColumnNow = nullptr;
static ColumnLaterFn g_realColumnLater = nullptr;

// ---------------------------------------------------------------------------
// What the body script reports (cVR_BeastSight), and what was done with it
// ---------------------------------------------------------------------------

// A report older than this (ms) no longer counts, and the game's own height is used.
static const unsigned long long kWordFreshMs = 300;

static bool g_on = false;
static float g_up = 0.0f;
static unsigned long long g_wordTime = 0;
static bool g_installed = false;

// How far the real head is above the game's eye, if a fresh report says it counts.
static bool RealHeadRise(float& up)
{
    if (!g_on || g_wordTime == 0 || GetTickCount64() - g_wordTime > kWordFreshMs) return false;
    up = SightUsableRise(g_up);
    return true;
}

// Which of the four line-of-sight checks is running on this thread (0 = none).
// Only a column test made inside one of them is a test of the player.
enum { kLookNone = 0, kLookNowFov = 1, kLookNow = 2, kLookLater = 3, kLookLaterFov = 4, kLookKinds = 5 };
static const char* const kLookNames[kLookKinds] = { "?", "at once, with its field of view", "at once",
    "answered later", "answered later, with its field of view" };
static thread_local int t_look = kLookNone;

struct LookUnderWay
{
    int was;
    explicit LookUnderWay(int kind) : was(t_look) { t_look = kind; }
    ~LookUnderWay() { t_look = was; }
};

// Log counters since the last summary: checks of each kind and how many were
// moved, and head-position queries and how many were moved.
static int g_looks[kLookKinds] = {};
static int g_looksMoved[kLookKinds] = {};
static int g_headAsked = 0, g_headMoved = 0;
// Individual log lines written: for each kind of check the first 8 moved and the first 2 unmoved, and the first 8 head queries.
static int g_moveNotes[kLookKinds] = {}, g_sameNotes[kLookKinds] = {}, g_headNotes = 0;
static const int kMoveNotes = 8, kSameNotes = 2, kHeadNotes = 8;
static unsigned long long g_summaryTime = 0;
static const unsigned long long kSummaryEveryMs = 30000;

static void NoteColumn(const float centre[3], const float size[3], const float movedCentre[3], const float movedSize[3], float up, bool moved)
{
    const int kind = t_look;
    ++g_looks[kind];
    if (moved) ++g_looksMoved[kind];
    if (moved ? g_moveNotes[kind] >= kMoveNotes : g_sameNotes[kind] >= kSameNotes) return;
    if (moved) ++g_moveNotes[kind]; else ++g_sameNotes[kind];
    const float bottom = centre[1] - size[1] * 0.5f;
    if (moved)
        Log("SIGHT: a look at the player (%s): its column from %.3f up to %.3f -> %.3f (the highest point looked at %.3f -> %.3f): "
            "the real head %.0f cm %s the game's eye.", kLookNames[kind], bottom, SightColumnTop(centre, size),
            SightColumnTop(movedCentre, movedSize), SightHighestLookedAt(centre, size), SightHighestLookedAt(movedCentre, movedSize),
            fabsf(up) * 100.0f, up < 0.0f ? "below" : "above");
    else
        Log("SIGHT: a look at the player (%s), as the game's own: its column from %.3f up to %.3f (the highest point looked at %.3f; "
            "the body script %s).", kLookNames[kind], bottom, SightColumnTop(centre, size), SightHighestLookedAt(centre, size),
            g_wordTime == 0 ? "has not said where the real head is" : (g_on ? "has not said lately" : "says the game's own now"));
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

static float* HookedHeadPos(void* agent, float* out)
{
    float* given = g_realHeadPos(agent, out);
    ++g_headAsked;
    float up = 0.0f;
    if (out != nullptr && RealHeadRise(up))
    {
        const float was = out[1];
        out[1] = was + up;
        ++g_headMoved;
        if (g_headNotes < kHeadNotes)
        {
            ++g_headNotes;
            Log("SIGHT: the player's head asked for (GetPlayerHeadPos): %.3f -> %.3f, the real head %.0f cm %s the game's eye.",
                was, out[1], fabsf(up) * 100.0f, up < 0.0f ? "below" : "above");
        }
    }
    return given;
}

static unsigned long long HookedLookNowFov(void* agent, float fov, const float* forward, unsigned char checkFov)
{
    LookUnderWay look(kLookNowFov);
    return g_realLookNowFov(agent, fov, forward, checkFov);
}

static unsigned long long HookedLookNow(void* agent)
{
    LookUnderWay look(kLookNow);
    return g_realLookNow(agent);
}

static void HookedLookLater(void* agent, const void* callback)
{
    LookUnderWay look(kLookLater);
    g_realLookLater(agent, callback);
}

static void HookedLookLaterFov(void* agent, const void* callback, float fov, const float* forward, unsigned char checkFov)
{
    LookUnderWay look(kLookLaterFov);
    g_realLookLaterFov(agent, callback, fov, forward, checkFov);
}

// Moves the player's column to the real head's height when that counts, writing `movedCentre` and
// `movedSize`. Returns true if it moved it. Only applies inside one of the four checks.
static bool MovePlayerColumn(const float* centre, const float* size, float movedCentre[3], float movedSize[3])
{
    if (t_look == kLookNone || centre == nullptr || size == nullptr) return false;
    float up = 0.0f;
    const bool counts = RealHeadRise(up);
    if (counts) SightMoveColumn(centre, size, up, movedCentre, movedSize);
    NoteColumn(centre, size, counts ? movedCentre : centre, counts ? movedSize : size, up, counts);
    return counts;
}

static unsigned long long HookedColumnNow(void* agent, const float* eye, const float* centre, const float* size)
{
    float movedCentre[3], movedSize[3];
    if (MovePlayerColumn(centre, size, movedCentre, movedSize)) return g_realColumnNow(agent, eye, movedCentre, movedSize);
    return g_realColumnNow(agent, eye, centre, size);
}

static void HookedColumnLater(void* agent, const float* eye, const float* centre, const float* size, void* answer, unsigned int flags)
{
    float movedCentre[3], movedSize[3];
    if (MovePlayerColumn(centre, size, movedCentre, movedSize)) g_realColumnLater(agent, eye, movedCentre, movedSize, answer, flags);
    else g_realColumnLater(agent, eye, centre, size, answer, flags);
}

// ---------------------------------------------------------------------------

void SightFromScript(bool on, float up)
{
    const unsigned long long now = GetTickCount64();
    if (g_wordTime == 0)
        Log("SIGHT: the body script first said where the real head is (cVR_BeastSight: %s, %.0f cm %s the game's eye)%s.",
            on ? "counts" : "the game's own", fabsf(up) * 100.0f, up < 0.0f ? "below" : "above",
            g_installed ? "" : "; but the sight hooks are not in, so the game's own");
    g_on = on;
    g_up = up;
    g_wordTime = now;

    if (g_summaryTime == 0) g_summaryTime = now;
    if (now - g_summaryTime < kSummaryEveryMs) return;
    g_summaryTime = now;
    int looks = 0, moved = 0;
    for (int i = 1; i < kLookKinds; ++i) { looks += g_looks[i]; moved += g_looksMoved[i]; }
    if (looks > 0 || g_headAsked > 0)
        Log("SIGHT: in the last %llu s, %d looks at the player by the game's creatures (at once %d and %d with the field of view, "
            "answered later %d and %d), %d of them at the real head's height; the head asked for %d times, %d moved; the real head "
            "now %.0f cm %s the game's eye (%s).", kSummaryEveryMs / 1000, looks, g_looks[kLookNow], g_looks[kLookNowFov],
            g_looks[kLookLater], g_looks[kLookLaterFov], moved, g_headAsked, g_headMoved, fabsf(up) * 100.0f,
            up < 0.0f ? "below" : "above", on ? "counts" : "the game's own");
    for (int i = 0; i < kLookKinds; ++i) g_looks[i] = g_looksMoved[i] = 0;
    g_headAsked = g_headMoved = 0;
}

bool SightInstall()
{
    struct Hook { const char* name; const char* pattern; LPVOID detour; LPVOID* original; void* target; };
    Hook hooks[] = {
        { "GetPlayerHeadPos", kHeadPosPattern, (LPVOID)HookedHeadPos, (LPVOID*)&g_realHeadPos, nullptr },
        { "the look at once with the field of view", kLookNowFovPattern, (LPVOID)HookedLookNowFov, (LPVOID*)&g_realLookNowFov, nullptr },
        { "the look at once", kLookNowPattern, (LPVOID)HookedLookNow, (LPVOID*)&g_realLookNow, nullptr },
        { "the look answered later", kLookLaterPattern, (LPVOID)HookedLookLater, (LPVOID*)&g_realLookLater, nullptr },
        { "the look answered later with the field of view", kLookLaterFovPattern, (LPVOID)HookedLookLaterFov, (LPVOID*)&g_realLookLaterFov, nullptr },
        { "the column looked at at once", kColumnNowPattern, (LPVOID)HookedColumnNow, (LPVOID*)&g_realColumnNow, nullptr },
        { "the column looked at later", kColumnLaterPattern, (LPVOID)HookedColumnLater, (LPVOID*)&g_realColumnLater, nullptr },
    };
    const int count = (int)(sizeof(hooks) / sizeof(hooks[0]));
    HMODULE game = GetModuleHandleW(nullptr);

    // Find every function first: nothing is hooked unless all of them are found.
    for (int i = 0; i < count; ++i)
    {
        int matches = 0;
        hooks[i].target = FindPattern(game, hooks[i].pattern, &matches);
        if (matches != 1)
        {
            Log("SIGHT ERROR: expected to find the engine's %s exactly once, found it %d times. The game may have been updated; "
                "the creatures see the player as the game has it.", hooks[i].name, matches);
            return false;
        }
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("SIGHT ERROR: could not start the hooking library: %s", MH_StatusToString(status));
        return false;
    }
    int made = 0;
    for (; made < count; ++made)
    {
        status = MH_CreateHook(hooks[made].target, hooks[made].detour, hooks[made].original);
        if (status != MH_OK) break;
    }
    if (status == MH_OK)
    {
        for (int i = 0; i < count && status == MH_OK; ++i) status = MH_QueueEnableHook(hooks[i].target);
        if (status == MH_OK) status = MH_ApplyQueued();
    }
    if (status != MH_OK)
    {
        Log("SIGHT ERROR: could not hook the engine's sight (%s): the creatures see the player as the game has it.", MH_StatusToString(status));
        for (int i = 0; i < made; ++i) MH_RemoveHook(hooks[i].target);
        return false;
    }
    g_installed = true;
    Log("SIGHT: the creatures' sight of the player hooked (GetPlayerHeadPos at exe+0x%llX, the four looks at exe+0x%llX, +0x%llX, "
        "+0x%llX, +0x%llX, the columns at exe+0x%llX and +0x%llX): at the real head's height while the body script says so.",
        (unsigned long long)((char*)hooks[0].target - (char*)game), (unsigned long long)((char*)hooks[1].target - (char*)game),
        (unsigned long long)((char*)hooks[2].target - (char*)game), (unsigned long long)((char*)hooks[3].target - (char*)game),
        (unsigned long long)((char*)hooks[4].target - (char*)game), (unsigned long long)((char*)hooks[5].target - (char*)game),
        (unsigned long long)((char*)hooks[6].target - (char*)game));
    return true;
}
