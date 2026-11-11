#include <MinHook.h>

#include "gl_helpers.h"
#include "gui.h"
#include "log.h"
#include "pattern.h"
#include "vr.h"

// ---------------------------------------------------------------------------
// How the game draws a frame
// ---------------------------------------------------------------------------
// The engine's scene renderer works in stages (the names are its own):
//   RenderWorld, Render3DGui, RenderPostEffects, ...,
//   CopyBackBufferToScreen   - the finished 3D picture is copied to the window
//   RenderGUI                - the interface is drawn on top, in the window
//
// The mod hooks the function behind the RenderGUI stage. While it runs,
// anything the game tries to draw "to the window" goes into the mod's own
// picture instead.

// The first instructions of the engine's interface-drawing function.
static const char* kRenderGuiPattern =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 A9 "
    "48 81 EC 90 00 00 00 33 FF 48 8B DA 48 89 7D EF 8D 4F 30 E8";

typedef void (*RenderGuiFunction)(void* scene, void* viewport, float frameTime);
static RenderGuiFunction g_realRenderGui = nullptr;

// The game switches drawing targets through a function pointer kept inside
// glew32.dll (a helper library it ships with). The mod swaps that pointer for
// its own function, which passes every call on, except that "the window"
// (target 0) becomes the mod's picture while the interface is being drawn.
typedef void (APIENTRY* BindFramebufferFunction)(GLenum target, GLuint framebuffer);
static BindFramebufferFunction g_realBindFramebuffer = nullptr;
static bool g_bindWrapperInstalled = false;

static const GLenum kFramebuffer = 0x8D40;     // "both reading and drawing"
static const GLenum kDrawFramebuffer = 0x8CA9; // "drawing only"

static bool g_redirecting = false;

// The drawing target the game uses as "the screen" while it draws the
// interface. This is whatever target is selected when the interface stage
// starts. (It is not always the window itself: the engine can draw into an
// off-screen picture of its own and copy that to the window later.)
static GLuint g_screenTarget = 0;

// A short record of the first interface pass, written to the log for checking.
static int g_diagnosticBindsLeft = 0;

// The mod's picture of the interface.
static GLuint g_guiTexture = 0;
static GLuint g_guiFramebuffer = 0;
static int g_guiWidth = 0, g_guiHeight = 0;
static unsigned long long g_clearedSerial = 0;  // the frame the picture was last wiped for
static unsigned long long g_capturedSerial = 0; // the frame the interface was last drawn for

static void APIENTRY BindFramebufferWrapper(GLenum target, GLuint framebuffer)
{
    if (g_redirecting)
    {
        if (g_diagnosticBindsLeft > 0)
        {
            --g_diagnosticBindsLeft;
            Log("GUI:   during the interface stage the game selected drawing target %u (mode 0x%X)%s",
                (unsigned)framebuffer, (unsigned)target,
                framebuffer == g_screenTarget ? " - its screen, redirected to our picture" : "");
        }
        if (framebuffer == g_screenTarget && (target == kFramebuffer || target == kDrawFramebuffer))
            framebuffer = g_guiFramebuffer;
    }
    g_realBindFramebuffer(target, framebuffer);
}

void GuiOnFrame()
{
    if (g_bindWrapperInstalled) return;

    HMODULE glew = GetModuleHandleA("glew32.dll");
    if (!glew) return;

    // The address of glew's pointer to "glBindFramebuffer".
    BindFramebufferFunction* slot = (BindFramebufferFunction*)GetProcAddress(glew, "__glewBindFramebuffer");
    if (!slot || *slot == nullptr) return; // glew has not been set up by the game yet; try next frame

    g_realBindFramebuffer = *slot;
    *slot = BindFramebufferWrapper;
    g_bindWrapperInstalled = true;
    Log("GUI: now watching the game's drawing-target switches.");
}

static bool EnsureGuiPicture(int width, int height)
{
    if (g_guiTexture != 0 && width == g_guiWidth && height == g_guiHeight) return true;

    GlDeleteTarget(g_guiTexture, g_guiFramebuffer);
    if (!GlCreateTarget(width, height, g_guiTexture, g_guiFramebuffer)) return false;

    g_guiWidth = width;
    g_guiHeight = height;
    Log("GUI: created a %d x %d picture for the game's interface.", width, height);
    return true;
}

// The processor's time in the game's interface stage, for the log.
static double g_guiStageMs = 0;
static unsigned long long g_guiStages = 0;

static double GuiClockMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

void GuiLogTimings(unsigned long long frames)
{
    if (frames == 0 || g_guiStages == 0) return;
    Log("GUI: the processor's time for the game's interface (its menus, hints, scripts and drawing), per frame on average: %.3f ms (%.1f times a frame).",
        g_guiStageMs / (double)frames, (double)g_guiStages / (double)frames);
    g_guiStageMs = 0;
    g_guiStages = 0;
}

static void HookedRenderGuiWork(void* scene, void* viewport, float frameTime);

// Runs each time the engine draws the interface (timed for the log).
static void HookedRenderGui(void* scene, void* viewport, float frameTime)
{
    double started = GuiClockMs();
    HookedRenderGuiWork(scene, viewport, frameTime);
    g_guiStageMs += GuiClockMs() - started;
    ++g_guiStages;
}

static void HookedRenderGuiWork(void* scene, void* viewport, float frameTime)
{
    unsigned long long serial = VrGetFrameSerial(); // 0 while VR is not active
    int width = 0, height = 0;

    bool capture = serial != 0 && g_bindWrapperInstalled && GlLoadFunctions() &&
                   GlGetWindowSize(width, height) && EnsureGuiPicture(width, height);
    if (!capture)
    {
        g_realRenderGui(scene, viewport, frameTime);
        return;
    }

    // Start each frame with an empty (fully see-through) picture.
    GLuint drawBefore = GlGetDrawFramebuffer();
    if (g_clearedSerial != serial)
    {
        GlSetDrawFramebuffer(g_guiFramebuffer);
        GlClearTransparent();
        GlSetDrawFramebuffer(drawBefore);
        g_clearedSerial = serial;
    }

    // Whatever the game is aimed at right now is its "screen" for the
    // interface. Aim it at the mod's picture instead, and keep doing so if the game
    // re-selects its screen while it draws.
    g_screenTarget = drawBefore;

    static int diagnosticPasses = 2;
    bool diagnose = diagnosticPasses > 0;
    if (diagnose)
    {
        --diagnosticPasses;
        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);
        Log("GUI: interface stage starts. The game is aimed at drawing target %u (0 = the window itself), "
            "reading from %u, drawing area %d x %d.",
            (unsigned)drawBefore, (unsigned)GlGetReadFramebuffer(), viewport[2], viewport[3]);
        g_diagnosticBindsLeft = 12;
    }

    GlSetDrawFramebuffer(g_guiFramebuffer);

    g_redirecting = true;
    g_realRenderGui(scene, viewport, frameTime);
    g_redirecting = false;
    g_diagnosticBindsLeft = 0;

    if (diagnose)
    {
        Log("GUI: interface stage ends. The game is aimed at drawing target %u (ours is %u).",
            (unsigned)GlGetDrawFramebuffer(), (unsigned)g_guiFramebuffer);
    }

    // Aim the game back at its screen, which is where it believes it is.
    if (GlGetDrawFramebuffer() == g_guiFramebuffer) GlSetDrawFramebuffer(g_screenTarget);
    if (GlGetReadFramebuffer() == g_guiFramebuffer) GlSetReadFramebuffer(g_screenTarget);

    g_capturedSerial = serial;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("GUI: the game's interface is being captured.");
    }
}

bool GuiGetCaptured(unsigned long long serial, GLuint& texture, GLuint& framebuffer, int& width, int& height)
{
    if (serial == 0 || g_capturedSerial != serial || g_guiTexture == 0) return false;
    texture = g_guiTexture;
    framebuffer = g_guiFramebuffer;
    width = g_guiWidth;
    height = g_guiHeight;
    return true;
}

bool GuiGetLastCaptured(GLuint& texture, GLuint& framebuffer, int& width, int& height)
{
    if (g_capturedSerial == 0 || g_guiTexture == 0 || g_guiFramebuffer == 0) return false;
    texture = g_guiTexture;
    framebuffer = g_guiFramebuffer;
    width = g_guiWidth;
    height = g_guiHeight;
    return true;
}

bool GuiInstallHook()
{
    HMODULE game = GetModuleHandleW(nullptr);

    int matches = 0;
    void* target = FindPattern(game, kRenderGuiPattern, &matches);
    if (matches != 1)
    {
        Log("GUI ERROR: expected to find the interface-drawing function exactly once, found it %d times.", matches);
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("GUI ERROR: could not start the hooking library: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_CreateHook(target, (LPVOID)HookedRenderGui, (LPVOID*)&g_realRenderGui);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("GUI ERROR: could not hook the interface-drawing function: %s", MH_StatusToString(status));
        return false;
    }

    Log("GUI: hooked the interface-drawing function at exe+0x%llX",
        (unsigned long long)((char*)target - (char*)game));
    return true;
}
