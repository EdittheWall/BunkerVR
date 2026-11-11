#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "clock.h"
#include "import_hook.h"
#include "log.h"

// The game reads time in two ways, both through the SDL2 library: whole
// milliseconds (SDL_GetTicks) and a fine counter (SDL_GetPerformanceCounter).
typedef unsigned int (*GetTicksFunction)(void);
typedef unsigned long long (*GetCounterFunction)(void);
static GetTicksFunction g_realGetTicks = nullptr;
static GetCounterFunction g_realGetCounter = nullptr;

static volatile bool g_held = false;
static unsigned int g_heldTicks = 0;           // what the millisecond clock shows while held
static unsigned long long g_heldCounter = 0;   // what the fine counter showed when the hold began
static unsigned long long g_holdStartedMs = 0; // real time, from Windows
static int g_timeouts = 0;
static bool g_unavailable = true;
static unsigned long long g_restUntilMs = 0; // after several holds that timed out, no holding until then

// A hold must never last: if the game stopped drawing while the clock stands
// still, it would wait forever for time to pass.
static void LetGoIfTooLong()
{
    if (!g_held) return;
    if (GetTickCount64() - g_holdStartedMs < 80) return;

    g_held = false;
    ++g_timeouts;
    // The game only draws a picture without moving its world on when its
    // "limit FPS" setting is off (the mod's scripts switch it off). Until
    // then, holding is given a rest and tried again a little later.
    static int logsLeft = 6;
    if (g_timeouts >= 6)
    {
        g_timeouts = 0;
        g_restUntilMs = GetTickCount64() + 5000;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("CLOCK: the game did not finish its second picture while its clock was held (6 times); trying again in 5 seconds.");
        }
    }
}

static unsigned int HookedGetTicks()
{
    unsigned int real = g_realGetTicks();
    LetGoIfTooLong();
    return g_held ? g_heldTicks : real;
}

static unsigned long long HookedGetCounter()
{
    unsigned long long real = g_realGetCounter();
    LetGoIfTooLong();
    if (!g_held) return real;

    // Creep forwards at a thousandth of real speed, so that no time span the
    // game measures is ever exactly zero.
    return g_heldCounter + (real - g_heldCounter) / 1024;
}

bool InstallClockHooks()
{
    HMODULE game = GetModuleHandleW(nullptr); // the game's exe
    bool ticks = HookImport(game, "SDL2.dll", "SDL_GetTicks", (void*)HookedGetTicks, (void**)&g_realGetTicks);
    bool counter = HookImport(game, "SDL2.dll", "SDL_GetPerformanceCounter", (void*)HookedGetCounter,
                              (void**)&g_realGetCounter);
    g_unavailable = !(ticks && counter && g_realGetTicks != nullptr && g_realGetCounter != nullptr);
    Log("CLOCK: the game's clock %s.", g_unavailable ? "could NOT be reached (both eyes will not be perfectly in step)"
                                                      : "can be held between the two eyes' pictures");
    return !g_unavailable;
}

void ClockHold(bool hold)
{
    if (!hold)
    {
        if (g_held)
        {
            // The game drew its second picture while the clock was held: it works.
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log("CLOCK: holding works; both eyes show the same moment.");
            }
        }
        g_held = false;
        return;
    }
    if (g_unavailable || g_held) return;
    if (GetTickCount64() < g_restUntilMs) return;

    g_heldTicks = g_realGetTicks();
    g_heldCounter = g_realGetCounter();
    g_holdStartedMs = GetTickCount64();
    g_held = true;
}

bool ClockHoldUnavailable()
{
    return g_unavailable;
}
