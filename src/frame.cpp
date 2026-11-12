#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include "camera.h"
#include "capture.h"
#include "enginetime.h"
#include "sampler.h"
#include "frame.h"
#include "grips.h"
#include "gui.h"
#include "import_hook.h"
#include "log.h"
#include "posetool.h"
#include "scene.h"
#include "vr.h"
#include "player_build.h"
#include "vrmenu.h"

// The game draws with OpenGL through the SDL2 library. When a frame is finished
// it calls SDL_GL_SwapWindow to put it on screen. That call is our "once per
// frame" moment, and where the finished picture is sent to the headset.

typedef void (*SwapWindowFunction)(void* window);
static SwapWindowFunction g_realSwapWindow = nullptr;

static bool g_loggedFirstFrame = false;
static unsigned long long g_frameCount = 0;

// The game window's own swap, the picture put on the PC's screen, timed. A swap that waits for the PC's
// screen, as a graphics driver's forced vertical sync makes it, shows here: it can hold the game to a
// 60 Hz screen's pace against the headset's 90, in time that nothing else measures.
static double g_swapMs = 0.0, g_swapLongestMs = 0.0, g_ticksToMs = 0.0;
static unsigned long long g_swaps = 0;
static unsigned long long g_lastReportTime = 0;
static unsigned long long g_lastReportFrame = 0;
static int g_reportsLeft = 6; // keep the log short: report speed for about a minute

static const char* TextOrUnknown(const GLubyte* text)
{
    return text ? (const char*)text : "(unknown)";
}

// How fast the game ran since the last report, and where the time went.
static void ReportSpeed(unsigned long long now)
{
    double seconds = (double)(now - g_lastReportTime) / 1000.0;
    double framesPerSecond = (double)(g_frameCount - g_lastReportFrame) / seconds;
    Log("Frames so far: %llu, running at about %.1f frames per second", g_frameCount, framesPerSecond);
    if (g_swaps > 0)
        Log("MONITOR: the game window's own swap (its picture put on the PC's screen): %.2f ms a frame on average, the longest %.2f ms (%llu swaps; "
            "long ones mean the PC's screen sets the pace).", g_swapMs / (double)g_swaps, g_swapLongestMs, g_swaps);
    g_swapMs = g_swapLongestMs = 0.0;
    g_swaps = 0;

    VrLogTimings();
    SceneLogTimings();
    CameraLogScriptTimings(g_frameCount - g_lastReportFrame);
    EngineTimeLog(g_frameCount - g_lastReportFrame);
    GuiLogTimings(g_frameCount - g_lastReportFrame);

    g_lastReportTime = now;
    g_lastReportFrame = g_frameCount;
    if (g_reportsLeft > 0) --g_reportsLeft;
}

// See FrameTimingsAgain in frame.h. Asked for here, done at the next frame shown: a whole report
// up to the mark, then the lines every 10 seconds again.
static volatile bool g_timingsAgainAsked = false;

void FrameTimingsAgain()
{
    g_timingsAgainAsked = true;
}

// Runs on the game's rendering thread, once per frame, just before the frame is shown.
static void OnFrame()
{
    unsigned long long now = GetTickCount64();
    EngineTimeFrame(); // this is the game's main thread
    SamplerMainThreadHere(); // and its stack, for the sampling profiler

    if (!g_loggedFirstFrame)
    {
        g_loggedFirstFrame = true;

        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);

        Log("First frame reached. The frame hook works.");
        Log("OpenGL version: %s", TextOrUnknown(glGetString(GL_VERSION)));
        Log("Graphics card:  %s (%s)", TextOrUnknown(glGetString(GL_RENDERER)),
            TextOrUnknown(glGetString(GL_VENDOR)));
        Log("Current viewport: %d x %d", viewport[2], viewport[3]);

        g_lastReportTime = now;
        g_lastReportFrame = 0;
    }

    ++g_frameCount;

    // A level has just come up after its loading: the time before it, the
    // loading too, is reported now on its own, and the first minute in the
    // level every 10 seconds again, so that a short session in the headset
    // gives numbers of the level alone. (Less than a second since the last
    // report: that little is left in the next one.)
    if (VrTakeWorldBack())
    {
        if (now - g_lastReportTime >= 1000u)
        {
            Log("Frame times: the level is shown; up to now (its loading included):");
            ReportSpeed(now);
        }
        g_reportsLeft = 6;
    }

    // A mark (FrameTimingsAgain): what came before it reported now, and the next
    // minute every 10 seconds, the game's own work by part with it.
    if (g_timingsAgainAsked)
    {
        g_timingsAgainAsked = false;
        if (now - g_lastReportTime >= 1000u)
        {
            Log("Frame times: a mark: up to now:");
            ReportSpeed(now);
        }
        g_reportsLeft = 6;
        Log("Frame times: a mark: the timing lines every 10 seconds for the next minute.");
    }

    // Report how fast the game is running: every 10 seconds for the first
    // minute (of the game, and of each level), then once a minute.
    if (now - g_lastReportTime >= (g_reportsLeft > 0 ? 10000u : 60000u)) ReportSpeed(now);
    EngineTimeActive(g_reportsLeft > 0); // the game's own update by part: only while the lines come every 10 seconds
}

// Returns true once each time `key` goes from "up" to "down".
static bool KeyPressed(int key, bool& wasDown)
{
    bool isDown = (GetAsyncKeyState(key) & 0x8000) != 0;
    bool pressed = isDown && !wasDown;
    wasDown = isDown;
    return pressed;
}

// Keys:
//   Home  re-centers the view: wherever the headset faces becomes "straight ahead".
//   F2    opens or closes the BunkerVR menu (the same as Y on the left controller).
//   F3    switches between drawing the game's body at your size and at its own.
//   F4    switches between the game's own arms and plain blocks for the hands.
//   F5    makes thumbstick turning slower (by 15% per press).
//   F6    makes thumbstick turning faster (by 15% per press).
//   F7    makes the world look smaller (one 5% step of the World Scale row).
//   F8    makes the world look bigger (one 5% step of the World Scale row).
//   F9    saves what each eye sees to picture files (a testing aid).
//   F11   opens the hand poses tool on the PC's screen in front, or brings it
//         to the front (only while the game's window is in front:
//         the tool itself closes on F11 when it is in front).
//   End   switches "both eyes every frame" on or off.
static void CheckKeys()
{
    enum { kHome, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF11, kEnd, kKeyCount };
    static const int keys[kKeyCount] = { VK_HOME, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F11, VK_END };
    static bool wasDown[kKeyCount] = {};
    bool pressed[kKeyCount];
    for (int i = 0; i < kKeyCount; ++i) pressed[i] = KeyPressed(keys[i], wasDown[i]);

    // None of them while Alt or Ctrl is held: Alt+F4 closes the game, and
    // must not also switch the game's arms off or on (F4), which would leave
    // plain blocks for the hands.
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) return;

    if (pressed[kHome]) VrRecenter();
    if (pressed[kF9])
    {
        CaptureRequest();
        VrRequestArmNotes();
    }
    // Development keys: not in the tester build (kPlayerBuild, player_build.h).
    if (kPlayerBuild) return;
    if (pressed[kF2]) VrToggleMenu();
    if (pressed[kF3]) VrToggleBigBody();
    if (pressed[kF4]) VrToggleGameHands();
    if (pressed[kF5]) VrAdjustTurnSpeed(1.0f / 1.15f);
    if (pressed[kF6]) VrAdjustTurnSpeed(1.15f);
    if (pressed[kF7]) VrStepWorldScale(1);
    if (pressed[kF8]) VrStepWorldScale(-1);
    if (pressed[kEnd]) VrToggleBothEyes();
    if (pressed[kF11])
    {
        // (Not while another program is in front: F11 is that program's.)
        HWND inFront = GetForegroundWindow();
        if (inFront != nullptr && inFront == (HWND)VrGameWindow()) PoseToolFromKey();
    }
}

static void HookedSwapWindow(void* window)
{
    SceneFrameEnding(); // (timing: see SceneFrameStarted)
    OnFrame();
    CheckKeys();
    GuiOnFrame();
    // The poses: the Grips page's numbers kept in step with the held item,
    // and the hand poses tool's changes made (here on the game's own frame,
    // so that they also go on while the headset is off).
    if (GripsTick()) VrMenuGripsChanged();

    VrEndFrame(); // send the finished picture to the headset

    // With both eyes drawn every frame, the left eye's picture is not put on
    // the monitor by itself: the monitor is updated once the pair is done.
    if (!VrMidPair())
    {
        if (g_ticksToMs == 0.0)
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            g_ticksToMs = 1000.0 / (double)frequency.QuadPart;
        }
        LARGE_INTEGER before, after;
        QueryPerformanceCounter(&before);
        g_realSwapWindow(window);
        QueryPerformanceCounter(&after);
        double ms = (double)(after.QuadPart - before.QuadPart) * g_ticksToMs;
        g_swapMs += ms;
        if (ms > g_swapLongestMs) g_swapLongestMs = ms;
        ++g_swaps;
    }

    VrBeginFrame(); // read the head position for the next picture
    SceneFrameStarted(); // the game's own update is timed from here to the drawing of the world
}

bool InstallFrameHook()
{
    HMODULE game = GetModuleHandleW(nullptr); // the game's exe
    return HookImport(game, "SDL2.dll", "SDL_GL_SwapWindow",
                      (void*)HookedSwapWindow, (void**)&g_realSwapWindow);
}
