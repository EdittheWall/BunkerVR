#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h> // needed by the OpenXR Windows header

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "camera.h"
#include "clock.h"
#include "capture.h"
#include "dedication.h"
#include "frame.h"
#include "gl_helpers.h"
#include "grips.h"
#include "gui.h"
#include "hints.h"
#include "log.h"
#include "motion.h"
#include "pointer.h"
#include "posetool.h"
#include "sampler.h"
#include "scene.h"
#include "settings.h"
#include "player_build.h"
#ifdef BUNKERVR_COPY_LOCK
#include "licence.h"
#endif
#include "standin_watch.h"
#include "testlist.h"
#include "vr.h"
#include "vrmenu.h"
#include "watch_shared.h"

// ---------------------------------------------------------------------------
// Pending OpenGL errors are cleared before the headset software uses OpenGL.
// SteamVR's OpenXR runtime reads OpenGL's errors after its own OpenGL calls and
// fails on any it finds, even one left over from the game's drawing or ours.
// Its log for the game (Steam\logs\xrclient_AmnesiaTheBunker.txt) then shows
// "SXR_GL_CHECK: error:0x501 ... glGenTextures" and "Failed to create opengl
// swapchain image", and the call returns XR_ERROR_RUNTIME_FAILURE: swapchains
// fail to be created, so the menu and the start-up page never appear. The
// Oculus runtime doesn't check. So every OpenXR call that may use OpenGL goes
// through these wrappers: they clear what is pending first and log where it
// was found (the first 30 times, then every 1000th). Swapchains are also
// created with no pixel buffer bound, since a runtime filling new images would
// read from it (the frame's GlStateGuard restores the game's binding).
// ---------------------------------------------------------------------------

static int g_glErrorsTakenTimes = 0;

static void TakeGlErrorsBefore(const char* what)
{
    unsigned first = 0;
    int count = GlTakeErrors(first);
    if (count == 0) return;
    ++g_glErrorsTakenTimes;
    if (g_glErrorsTakenTimes <= 30 || g_glErrorsTakenTimes % 1000 == 0)
        Log("GL: %d OpenGL error%s (the first 0x%X) left waiting before %s: taken, so that the headset software does not take them for its own (%d times so far).",
            count, count == 1 ? "" : "s", first, what, g_glErrorsTakenTimes);
}

// On SteamVR the mod draws its panels into the eye images itself and sends
// SteamVR only the eye layers (see PanelsIntoEyes). Set when connecting to the
// headset software, before any set of pictures is made.
static bool g_panelsInEyes = false;
static bool g_runtimeSteamVr = false; // the headset software is SteamVR (its play area is always set from its room setup)

// Every swapchain created, its images, and which image was released last. On
// SteamVR the mod draws the panels into the eye images itself (PanelsIntoEyes),
// each from its swapchain's last released image. Kept up to date by the
// wrappers below, which every swapchain goes through.
//
// SteamVR creates at most 16 swapchains for a session and refuses every one
// after that with XR_ERROR_LIMIT_REACHED, and it never sees the panels. So on
// SteamVR every swapchain except the eyes' is the mod's own ("own"): plain
// OpenGL textures, made and handed out by the wrappers below without calling
// SteamVR, with the address of its entry here as its handle. SteamVR then
// holds only the eyes' two.
struct KnownSet
{
    XrSwapchain set;
    int width, height;
    uint32_t count;     // its images (up to 8 tracked)
    GLuint pictures[8];
    int acquired;       // the image acquired for drawing (-1: none)
    int released;       // the image released last (-1: none yet)
    bool own;           // the mod's own pictures (SteamVR's panels), not the headset software's
};
static KnownSet g_knownSets[64] = {};
static const uint32_t kOwnSetPictures = 3; // images in an own set, as many as SteamVR's swapchains have

static KnownSet* FindKnownSet(XrSwapchain set)
{
    if (set == XR_NULL_HANDLE) return nullptr;
    for (KnownSet& known : g_knownSets)
        if (known.set == set) return &known;
    return nullptr;
}

static void NoteSetMade(XrSwapchain set, const XrSwapchainCreateInfo* info)
{
    // A swapchain destroyed with its session (not through XrDestroySwapchainGl) may have its handle reused: the entry is the new one's.
    KnownSet* old = FindKnownSet(set);
    if (old) *old = {};
    for (KnownSet& known : g_knownSets)
    {
        if (known.set != XR_NULL_HANDLE) continue;
        known = {};
        known.set = set;
        known.width = (int)info->width;
        known.height = (int)info->height;
        known.acquired = -1;
        known.released = -1;
        return;
    }
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("VR: more than 64 sets of pictures at once; a panel may be left out of the eyes' pictures.");
    }
}

// SteamVR can still fail swapchain creation with 0x501 at glGenTextures when no
// OpenGL error is pending in the game's context. Its swapchain code
// (vrclient_x64.dll, CSxrSwapchainGLOpenVR) makes its OWN OpenGL context current
// to create the images and reads that context's errors; its per-frame handover
// of images to its compositor (glCopyImageSubData or a blit, in the same
// context) checks nothing, so an error from that work stays pending there, out
// of the mod's reach, and the next swapchain created takes it for its own.
// glGetError returns one error and clears it, so the failed call has used it
// up: retried immediately (no frame in between, so nothing new is pending),
// creation succeeds. A retry a second later can fail again.
static int g_swapchainRetriesLogged = 0;

// A swapchain of the mod's own (see KnownSet), made the way SteamVR makes its
// swapchains: `info`'s size and pixel format, one mip level whatever
// `mipCount` asks. The panels are drawn into the eyes from their full size
// (GlDrawPanelIntoTexture).
static XrResult MakeOwnSet(const XrSwapchainCreateInfo* info, XrSwapchain* swapchain)
{
    *swapchain = XR_NULL_HANDLE;
    if (info->width == 0 || info->height == 0 || info->faceCount != 1 || info->arraySize != 1 || info->sampleCount != 1)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!GlLoadFunctions()) return XR_ERROR_RUNTIME_FAILURE;
    KnownSet* entry = nullptr;
    for (KnownSet& known : g_knownSets)
        if (known.set == XR_NULL_HANDLE)
        {
            entry = &known;
            break;
        }
    if (!entry)
    {
        Log("VR: more than 64 sets of pictures at once; the mod's own set (%u x %u) not made.", (unsigned)info->width,
            (unsigned)info->height);
        return XR_ERROR_LIMIT_REACHED;
    }

    TakeGlErrorsBefore("making a set of the mod's own pictures");
    GLuint pictures[kOwnSetPictures] = {};
    bool made = true;
    for (uint32_t i = 0; i < kOwnSetPictures && made; ++i)
    {
        pictures[i] = GlCreatePicture((int)info->width, (int)info->height, (GLint)info->format);
        made = pictures[i] != 0;
    }
    unsigned firstError = 0;
    if (GlTakeErrors(firstError) > 0) made = false; // (such as no memory left on the graphics card)
    if (!made)
    {
        for (GLuint& picture : pictures)
            if (picture != 0) glDeleteTextures(1, &picture);
        Log("VR: the mod's own set of pictures (%u x %u) could not be made (OpenGL error 0x%X).", (unsigned)info->width,
            (unsigned)info->height, firstError);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    *entry = {};
    entry->set = (XrSwapchain)entry;
    entry->width = (int)info->width;
    entry->height = (int)info->height;
    entry->count = kOwnSetPictures;
    for (uint32_t i = 0; i < kOwnSetPictures; ++i) entry->pictures[i] = pictures[i];
    entry->acquired = -1;
    entry->released = -1;
    entry->own = true;
    *swapchain = entry->set;
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("VR: SteamVR: the panels' pictures are the mod's own (SteamVR makes at most 16 sets for a game, and holds only the "
            "eyes' two).");
    }
    return XR_SUCCESS;
}

// `forEyes`: the eyes' pictures, the only sets the headset software shows on
// SteamVR; every other set there is the mod's own.
static XrResult XrCreateSwapchainGl(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain,
                                    bool forEyes = false)
{
    if (g_panelsInEyes && !forEyes) return MakeOwnSet(info, swapchain);
    TakeGlErrorsBefore("making a set of pictures");
    GlUnbindPixelBuffers();
    XrResult result = xrCreateSwapchain(session, info, swapchain);
    for (int again = 1; result == XR_ERROR_RUNTIME_FAILURE && again <= 4; ++again)
    {
        *swapchain = XR_NULL_HANDLE;
        result = xrCreateSwapchain(session, info, swapchain);
        if (g_swapchainRetriesLogged < 40)
        {
            ++g_swapchainRetriesLogged;
            Log("VR: the headset software refused a set of pictures (%u x %u), as SteamVR does with an OpenGL error left in its own "
                "context; try %d: %s.", (unsigned)info->width, (unsigned)info->height, again + 1,
                XR_SUCCEEDED(result) ? "made" : "refused again");
        }
    }
    if (XR_SUCCEEDED(result) && *swapchain != XR_NULL_HANDLE) NoteSetMade(*swapchain, info);
    return result;
}

static XrResult XrEnumerateSwapchainImagesGl(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    KnownSet* known = FindKnownSet(swapchain);
    if (known && known->own)
    {
        *count = known->count;
        if (capacity == 0) return XR_SUCCESS;
        if (capacity < known->count) return XR_ERROR_SIZE_INSUFFICIENT;
        if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR) return XR_ERROR_VALIDATION_FAILURE;
        XrSwapchainImageOpenGLKHR* pictures = (XrSwapchainImageOpenGLKHR*)images;
        for (uint32_t i = 0; i < known->count; ++i) pictures[i].image = known->pictures[i];
        return XR_SUCCESS;
    }
    TakeGlErrorsBefore("asking for a set's pictures");
    XrResult result = xrEnumerateSwapchainImages(swapchain, capacity, count, images);
    if (XR_SUCCEEDED(result) && known && images && capacity > 0 && images->type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR)
    {
        const XrSwapchainImageOpenGLKHR* pictures = (const XrSwapchainImageOpenGLKHR*)images;
        uint32_t given = *count < capacity ? *count : capacity;
        known->count = given < 8 ? given : 8;
        for (uint32_t i = 0; i < known->count; ++i) known->pictures[i] = pictures[i].image;
    }
    return result;
}

static XrResult XrAcquireSwapchainImageGl(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info, uint32_t* index)
{
    KnownSet* known = FindKnownSet(swapchain);
    if (known && known->own)
    {
        // (The one after the picture handed back last: never the one being shown.)
        if (known->acquired >= 0) return XR_ERROR_CALL_ORDER_INVALID;
        known->acquired = (known->released + 1) % (int)known->count;
        *index = (uint32_t)known->acquired;
        return XR_SUCCESS;
    }
    TakeGlErrorsBefore("taking a picture to draw into");
    XrResult result = xrAcquireSwapchainImage(swapchain, info, index);
    if (XR_SUCCEEDED(result) && known) known->acquired = (int)*index;
    return result;
}

static XrResult XrWaitSwapchainImageGl(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info)
{
    const KnownSet* known = FindKnownSet(swapchain);
    if (known && known->own) return known->acquired >= 0 ? XR_SUCCESS : XR_ERROR_CALL_ORDER_INVALID;
    TakeGlErrorsBefore("waiting for a picture");
    return xrWaitSwapchainImage(swapchain, info);
}

static XrResult XrReleaseSwapchainImageGl(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info)
{
    KnownSet* known = FindKnownSet(swapchain);
    if (known && known->own)
    {
        if (known->acquired < 0) return XR_ERROR_CALL_ORDER_INVALID;
        known->released = known->acquired;
        known->acquired = -1;
        return XR_SUCCESS;
    }
    TakeGlErrorsBefore("handing a picture back");
    XrResult result = xrReleaseSwapchainImage(swapchain, info);
    if (XR_SUCCEEDED(result) && known)
    {
        known->released = known->acquired;
        known->acquired = -1;
    }
    return result;
}

static XrResult XrDestroySwapchainGl(XrSwapchain swapchain)
{
    KnownSet* known = FindKnownSet(swapchain);
    if (known && known->own)
    {
        // (Without an OpenGL context, at shutdown, the textures go with the game's context.)
        if (wglGetCurrentContext() != nullptr) glDeleteTextures((GLsizei)known->count, known->pictures);
        *known = {};
        return XR_SUCCESS;
    }
    TakeGlErrorsBefore("letting a set of pictures go");
    if (known) *known = {}; // the handle is invalid after this, whatever the result
    return xrDestroySwapchain(swapchain);
}

static XrResult XrBeginFrameGl(XrSession session, const XrFrameBeginInfo* info)
{
    TakeGlErrorsBefore("beginning a headset frame");
    return xrBeginFrame(session, info);
}

static XrResult XrEndFrameGl(XrSession session, const XrFrameEndInfo* info)
{
    TakeGlErrorsBefore("sending a frame to the headset");
    return xrEndFrame(session, info);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class VrState
{
    NotStarted, // nothing done yet
    Ready,      // connected to the headset software
    Failed,     // something went wrong; leave the game alone from now on
};

static VrState g_state = VrState::NotStarted;

static XrInstance g_instance = XR_NULL_HANDLE; // our connection to the VR software
static XrSystemId g_systemId = XR_NULL_SYSTEM_ID; // the headset
static XrSession g_session = XR_NULL_HANDLE;   // an active link between the game's graphics and the headset
static XrSpace g_floorSpace = XR_NULL_HANDLE;  // "the room": a fixed point to measure from
static XrSpace g_headSpace = XR_NULL_HANDLE;   // "the head": moves with the headset
static XrSpace g_stageSpace = XR_NULL_HANDLE;  // "the play area": its origin on the floor (none if the headset has none)
static bool g_sessionRunning = false;
// The headset software has the game in focus: shown and given the controllers
// (not while SteamVR's dashboard, its laser mouse or the headset's own menu has
// them). The game's window is kept in front on the PC only then.
static volatile bool g_sessionFocused = false;
// When the PC's own mouse or keyboard was last used (GetTickCount64; 0: not
// since the watcher started), seen by the watcher (PcInputThread).
static volatile unsigned long long g_lastPcInputMs = 0;
// The headset has been taken off (its session stopped). The game does NOT go
// back to the flat screen: the head and hands are held where they last were,
// and the view keeps its VR shape, until the headset is put back on.
static bool g_headsetOff = false;
static bool g_quitAsked = false; // the headset software said EXITING: the game is to close (VrQuitAsked)
// "Freeze my hands" (the hand poses tool): the hands' poses are
// not updated from the controllers, and are held as while the headset is off.
static volatile bool g_handsFrozen = false;
// "Hand in front of me" (the hand poses tool; see VrSetHandInFront): one hand
// floats where it was put, in front of the eyes, whatever its controller does
// (put down, or not tracked with the headset off). Its pose, measured like the
// head's, replaces the controller's for every caller (VrGetGripHandPose,
// VrGetPointingHandPose, VrGetGrabHandPose).
static SRWLOCK g_inFrontLock = SRWLOCK_INIT;
static bool g_inFrontOn = false;
static int g_inFrontHand = 1;
static HeadPose g_inFrontPose = {};

// If `hand` is the one floating in front, writes its pose and returns true.
static bool HandInFrontPose(int hand, HeadPose& pose)
{
    AcquireSRWLockShared(&g_inFrontLock);
    bool on = g_inFrontOn && g_inFrontHand == hand;
    if (on) pose = g_inFrontPose;
    ReleaseSRWLockShared(&g_inFrontLock);
    return on;
}

static unsigned long long g_lastPoseLogTime = 0;
static int g_poseLogsLeft = 10; // a short run of once-per-second lines
static bool g_loggedEyes = false;

// The headset frame currently in progress. A frame is "open" from the moment
// we read the head position until the game has drawn its picture with it.
//
// The game can only draw one picture per frame, so the eyes take turns: one
// frame is drawn from the left eye's position, the next from the right eye's.
static bool g_frameOpen = false;
static bool g_frameShouldShow = false;   // the VR software wants a picture for this frame
static XrTime g_frameDisplayTime = 0;    // when this frame will appear in the headset
static bool g_framePoseValid = false;
static int g_frameEye = 0;               // which eye this frame is drawn for: 0 = left, 1 = right
static XrPosef g_frameEyePose = {};      // where that eye was when this frame's picture was drawn
static XrPosef g_frameHeadPose = {};     // where the middle of the head was
static XrFovf g_frameEyeFov = {};        // the angles that eye sees through its lens
static unsigned long long g_frameNumber = 0;
static unsigned long long g_frameSerialAtBegin = 0; // g_frameNumber as it was when this frame began

// What each eye is currently showing.
struct EyeImage
{
    bool hasPicture;      // has this eye received a picture yet?
    XrPosef pose;         // where the eye was when its picture was drawn
    XrFovf fov;           // exactly which angles the picture covers (left, right, up, down)
};
static EyeImage g_eyes[2] = {};

// The "swapchains": for each eye, a small set of pictures shared with the
// headset. Each frame we take the next one, copy the game's picture into it
// and hand it back.
static XrSwapchain g_swapchains[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static int g_swapchainWidth = 0;
static int g_swapchainHeight = 0;
static XrSwapchainImageOpenGLKHR g_swapchainImages[2][16];
static uint32_t g_swapchainImageCounts[2] = { 0, 0 };
static unsigned long long g_framesShown = 0;

// On SteamVR the mod draws its panels into the eye images itself and sends
// SteamVR only the eye layers (see PanelsIntoEyes). The eye images are then
// the size the headset software recommends, and each eye's latest picture of
// the world is kept here, to be copied into a fresh swapchain image every
// frame with the panels drawn over it. (g_panelsInEyes: above, with the sets' wrappers.)
static int g_recommendedWidth = 0, g_recommendedHeight = 0; // the headset software's recommended eye image size
static GLuint g_eyeKeepTexture[2] = { 0, 0 }, g_eyeKeepFramebuffer[2] = { 0, 0 };
static int g_eyeKeepWidth = 0, g_eyeKeepHeight = 0;
static unsigned long long g_eyeKeepSerial[2] = { 0, 0 }; // the game frame each was taken in

// The same kind of shared pictures for the game's interface (HUD and menus),
// which is shown in the headset as a flat panel floating in front of the player.
static XrSwapchain g_guiSwapchain = XR_NULL_HANDLE;
static int g_guiSwapchainWidth = 0;
static int g_guiSwapchainHeight = 0;
static XrSwapchainImageOpenGLKHR g_guiSwapchainImages[16];
static uint32_t g_guiSwapchainImageCount = 0;
static unsigned long long g_guiLastSentSerial = 0; // last frame with a fresh interface picture for the panel (copied into the panel's swapchain only once shown)
static unsigned long long g_guiPanelCopiedSerial = 0; // which of those frames' interface the panel's own pictures hold (0: none, or the cutscene screen's picture)

// The two hand controllers. OpenXR reports each one in two ways:
//   "grip": where the palm is, for drawing something held in the hand
//   "aim":  where the controller points, for drawing a pointing finger
static bool g_handsReady = false;
static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction g_gripAction = XR_NULL_HANDLE;
static XrAction g_aimAction = XR_NULL_HANDLE;
static XrSpace g_gripSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE }; // 0 = left, 1 = right
static XrSpace g_aimSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

// The two "squeezing" controls on each controller: the grip button under the
// middle finger and the trigger under the index finger. Either one grabs.
static XrAction g_squeezeAction = XR_NULL_HANDLE;
static XrAction g_triggerAction = XR_NULL_HANDLE;
static XrPath g_handPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };
static bool g_grabButtonsReady = false;
static bool g_grabHeld = false; // are we currently holding the game's "interact" button down?

// The thumbsticks: left one walks, right one turns.
static XrAction g_moveAction = XR_NULL_HANDLE;
static XrAction g_turnAction = XR_NULL_HANDLE;
static bool g_sticksReady = false;
static bool g_moveKeyDown[4] = { false, false, false, false }; // W, A, S, D
static float g_turnRemainder = 0.0f; // fraction of a mouse step carried over to the next frame
static int g_grabHand = 1;      // which hand squeezed last (it is also the one used for pointing): 0 = left, 1 = right

// Buttons: B on the right controller opens the inventory (its action is named
// "crouch"), clicking the left stick in sprints, and the menu button on the
// left controller is the Escape key.
static XrAction g_crouchAction = XR_NULL_HANDLE;
static XrAction g_sprintAction = XR_NULL_HANDLE;
static XrAction g_menuAction = XR_NULL_HANDLE;
static double g_gameMenuEscapeFromMs = -1000000.0; // the game's Escape is held for 80 ms from then ("Open Game Menu" on the Player page)
static double g_menuKeySentMs = -1000000.0; // when the mod last sent the game's menu key while not paused (see GamePausedOrAsked)
static XrAction g_gunAction = XR_NULL_HANDLE;    // A on the right controller: open the revolver, check the shotgun's shells
static XrAction g_aimAction2 = XR_NULL_HANDLE;   // X on the left controller: puts away what is held
static XrAction g_vrMenuAction = XR_NULL_HANDLE; // Y on the left controller: the mod's own menu
static bool g_vrMenuReady = false;
static XrAction g_hapticAction = XR_NULL_HANDLE; // the controllers' buzz
static bool g_hapticReady = false;

// The controllers' finger sensors (see the bindings in SetupHands).
static XrAction g_thumbTouchAction = XR_NULL_HANDLE;   // the thumb rests on the stick, a button or the thumb rest
static XrAction g_triggerTouchAction = XR_NULL_HANDLE; // the first finger rests on the trigger
static bool g_touchReady = false;
static bool g_thumbTouchBound = false, g_triggerTouchBound = false; // did the headset software accept them?

// The Valve Index controller's grip: its squeeze value is how far the fingers
// are closed around the handle, so a hand resting closed near an item would
// grab it. Like SteamVR's own "Grab" for it, a grab starts with the fingers
// closed and a light squeeze (the force sensor), holds while they stay closed,
// and lets go as they open (UpdateGrabInput).
static XrAction g_gripForceAction = XR_NULL_HANDLE;
static bool g_gripForceBound = false;                   // the Index bindings were accepted with it
static bool g_indexHand[2] = { false, false };          // that hand is read as an Index controller
static bool g_indexGrabbing[2] = { false, false };      // squeezed into a grab, fingers still closed
static const float kIndexForceGrab = 0.2f;              // the squeeze force that starts a grab (SteamVR's own: 0.25)
static const float kIndexOpenMost = 0.5f;               // cap on a closed hand's grip value until then (below the grab's 0.6)

// How far each hand's thumb, first finger and other three fingers are closed
// (0 to 1), worked out from the sensors and squeezes for posing empty hands.
static float g_fingerCurl[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
static XrAction g_inventoryAction = XR_NULL_HANDLE; // right stick click: the flashlight (despite the name)
static bool g_inventoryReady = false;
static bool g_gunButtonsReady = false;
static bool g_rightMouseDown = false;
static bool g_gunButtonBusy = false; // the A button is being worked through (holster or ammo check)
static bool g_reloadKeyDown = false; // the ammo-check key is being held down for the player
static bool g_crouchReady = false;
static bool g_moreButtonsReady = false;
static bool g_sprinting = false;
// The movement vignette (Settings::moveVignette): how far it is closed, 0 to 1 before its strength is applied, and
// when that was worked out (UpdateMoveInput). Not drawn if not worked out for 0.25 s (DrawComfortRing).
static float g_moveRing = 0.0f;
static double g_moveRingMs = -100000.0;
static bool g_steamOverlayOpen = false; // is Steam's overlay open over the game, taking the mouse or keyboard (see SteamOverlayInUse)?

// Where each hand points, measured in the room (for pointing at the menu panel).
static bool g_aimValid[2] = { false, false };
static XrPosef g_aimRoom[2] = {};
static bool g_pointerActive = false; // is a hand steering the mouse pointer on the panel right now?
static int g_pointerHand = 1;
static float g_pointerLength = 0.0f; // meters from that hand to the panel
// Both hands can point at the panel. Each one that does gets a beam; the mouse
// follows the hand that pressed last (g_menuHand), or the only one pointing.
static bool g_pointerHit[2] = { false, false };
static float g_pointerHitLength[2] = { 0.0f, 0.0f };
static int g_menuHand = 1;
// For the pointer's layers (AddPointerLayers), as the hands were last
// drawn: is a menu up (the game's arms not moving), and is a hand pointing at a
// panel (the game's or the mod's)?
static bool g_handsInMenuNow = false;
static bool g_menuPointerNow = false;

// The interface panel floats at a fixed place in the room. When the player
// turns or walks away from it, it glides back in front of them.
static bool g_panelPlaced = false;
static bool g_panelShown = false;   // was it shown in the last frame?
static bool g_dedicationHolding = false; // the dedication comes first: the game's panel and controls wait (see UpdateDedication)
static bool g_dedicationSqueezed = false; // a fresh squeeze while it waits: the headset is on (UpdateGrabInput)
static float DedicationDark();           // how dark the eyes are for it, 0 to 1 (DrawFade)
static bool g_panelMoving = false;  // is it gliding back in front of the player?
static bool g_panelSnapRequested = false;
static unsigned long long g_panelLastShownSerial = 0;
static float g_panelPosition[3] = { 0, 0, 0 }; // its center, in the room
static float g_panelYaw = 0.0f;                // which way it faces, radians
static float g_panelWidth = 1.8f, g_panelHeight = 1.0f; // meters

// Where each hand points from, shared with the camera hook (which passes it
// on to the game's grab code).
static HeadPose g_sharedHandPose[2] = {};
static unsigned long long g_sharedHandTime[2] = { 0, 0 }; // 0 = never seen

// Where each palm is, shared the same way (for the game's own arms).
static HeadPose g_sharedGripPose[2] = {};
static unsigned long long g_sharedGripTime[2] = { 0, 0 };

// The same two, in room space. They are converted to the game's "straight
// ahead" frame (ToRelativePose) only when the game asks, so they use the very
// same center as that frame's head pose. Stick turning moves that center a
// little every frame (see PublishHeadPose): converted at the end of the
// previous frame, the hands lag a turn step behind the head and shake while
// turning.
static XrPosef g_sharedHandRoom[2] = {};
static XrPosef g_sharedGripRoom[2] = {};

// The left eye's newest picture is held back until the right eye's is ready,
// so that both eyes always change at the same moment.
static GLuint g_holdTexture = 0, g_holdFramebuffer = 0;
static int g_holdWidth = 0, g_holdHeight = 0;
static bool g_holdValid = false;
static XrPosef g_holdPose = {};
static XrFovf g_holdFov = {};

// How long our own work takes, for the log.
static double g_statEndMs = 0, g_statBeginMs = 0, g_statWaitMs = 0;
static double g_statLastWaitMs = 0;
static unsigned long long g_statFrames = 0;
// The mod's frame submission time (g_statEndMs) split by part, for the log. See EndFrameWork.
enum
{
    kSendPartHands,     // the hands, the screen's effects, the comfort ring and the fade, drawn into the pictures
    kSendPartEyes,      // the eyes' pictures taken, held and handed to the headset
    kSendPartInterface, // the game's interface: its big panel's pictures and the pieces'
    kSendPartOverlays,  // the mod's menu, the ammo counter, the hints' labels and the pointer
    kSendPartEndFrame,  // the frame handed over (and, with SteamVR, the panels drawn into the eyes)
    kSendPartMonitor,   // the monitor's picture
    kSendParts
};
static double g_sendPartMs[kSendParts] = {};
static unsigned long long g_statPanelCopies = 0; // times the big panel's swapchain was brought up to date
// How evenly the headset's frames come: the time from one to the next.
static double g_statLastFrameMs = 0, g_statWorstGapMs = 0, g_statPeriodMs = 0;
static double g_displayPeriodMs = 0; // the headset's frame time, as it last said (not cleared with the statistics)

// Whether the game's logic is at half the headset's rate at the moment (see
// VrGetLogicRate and UpdateLogicHalf), and the last ten headset frames, late
// or not (bit 0 the latest), counted since the last change.
static bool g_logicHalf = false;
static unsigned g_recentLate = 0;
static int g_recentFrames = 0;
static unsigned long long g_statLogicChanges = 0, g_statHalfFrames = 0;

// The rate the game's own logic should run at: the headset's (the VR menu's
// "Game logic at the headset's rate"), or 0 to leave the game's own. Asked by
// the body script (roll 7066). The game otherwise runs its logic, physics and
// scripts at the monitor's rate; at 120 Hz that can take about 12 ms of each
// frame in a real level, while the graphics card has time to spare. Half the
// headset's rate while the headset has fallen back to half its rate (see
// UpdateLogicHalf).
int VrGetLogicRate()
{
    if (!GetSettings().logicAtHeadsetRate || g_displayPeriodMs < 5.0) return 0;
    int rate = (int)floor(1000.0 / g_displayPeriodMs + 0.5);
    if (rate < 60 || rate > 144) return 0;
    if (g_logicHalf && GetSettings().logicHalfRate) rate /= 2;
    return rate;
}

// The slowest rate the game keeps up with by running more than one logic
// update per frame: the same as the rate above, so at most one update per
// frame (the game divides one by the other). Below it the game slows down
// rather than piling up more updates: a frame that took longer than its share
// leaves the game's clock behind, and the next frame still runs one update
// (the engine lets the clock go once it is two updates behind). 0: leave the
// game's own. Don't allow two updates per frame: after a frame a little over
// its 11.1 ms, the game makes up the lost time with a second update (11 to
// 13 ms of work instead of about 5), so a frame 1 ms late becomes one 7 ms
// late, which takes the headset to half its rate, back and forth even in a
// light level.
int VrGetLogicSlowest()
{
    return VrGetLogicRate();
}

// Called for each headset frame: was it late (it came more than one and a
// half of the headset's frame times after the one before)? The game's logic
// goes to half the headset's rate once seven of the last ten frames came
// late (the headset has fallen back to every other refresh), and back to the
// full rate once no more than two of the last ten did. After a change the
// next ten frames are waited for. Without this, at 45 frames a second the
// game runs two logic updates per frame (7.3 ms together in a heavy part), so
// each frame takes some 13 ms of work, more than the 11.1 ms a frame has at
// 90 Hz, and the headset can't climb back to its full rate. With one update
// per frame the work is some 9 ms.
static void UpdateLogicHalf(bool late)
{
    g_recentLate = ((g_recentLate << 1) | (late ? 1u : 0u)) & 0x3FFu;
    if (g_recentFrames < 10) ++g_recentFrames;
    if (g_logicHalf) ++g_statHalfFrames;
    if (g_recentFrames < 10) return;

    int lateCount = 0;
    for (unsigned bits = g_recentLate; bits != 0; bits >>= 1) lateCount += (int)(bits & 1u);

    bool half = g_logicHalf;
    if (!half && lateCount >= 7) half = true;
    else if (half && lateCount <= 2) half = false;
    if (half == g_logicHalf) return;

    g_logicHalf = half;
    g_recentLate = 0;
    g_recentFrames = 0;
    ++g_statLogicChanges;
    static int logsLeft = 10;
    if (logsLeft > 0 && GetSettings().logicAtHeadsetRate && GetSettings().logicHalfRate)
    {
        --logsLeft;
        Log(half ? "VR: the headset has fallen back to half its rate (%d of the last 10 frames late): the game's logic goes to half its rate too."
                 : "VR: the headset is back at its full rate (%d of the last 10 frames late): so is the game's logic.",
            lateCount);
    }
}

// Back to the full rate, with no frames counted (the headset session starts
// or stops).
static void ResetLogicHalf()
{
    g_logicHalf = false;
    g_recentLate = 0;
    g_recentFrames = 0;
}

// A level has come up after its loading: the world shown again after at
// least kWorldBackBlackFrames black frames (half a second). Told once to the
// frame timings (frame.cpp), which then report the time before it on its own,
// so the loading isn't mixed into the once-a-minute timing line.
static const int kWorldBackBlackFrames = 45;
static bool g_worldBack = false;

// Late frames: even when the game's frames take about 6 of the headset's
// 11.1 ms on average, a few in a hundred can come late, and each run of them
// can take the game's logic to half rate and back. For the first late frame of
// each run, the log gets what the frame before it spent its time on
// (NoteLateFrame), the first 30 in each level. Kept here: the mod's own work
// in the last frame (its start and its submission: RunProtected), the game's
// logic updates counted at the last headset frame, and the frame before.
static double g_lastBeginTookMs = 0, g_lastEndTookMs = 0;
static unsigned long long g_updatesAtLastWait = 0;
static bool g_lastFrameLate = false;
static double g_prevWorkMs = 0;
static int g_prevUpdates = 0;
static int g_lateLogsLeft = 30;
static unsigned long long g_statLateRuns = 0, g_statLateRunsOver = 0, g_statLateRunsTwo = 0, g_statLateRunsFinish = 0;
static unsigned long long g_statFramesOver = 0;
static double g_statLongestWorkMs = 0, g_statLongestUpdateMs = 0;

bool VrTakeWorldBack()
{
    bool back = g_worldBack;
    g_worldBack = false;
    if (back) g_lateLogsLeft = 30;
    return back;
}

// Called for each headset frame, as the headset gives the next one: was it
// late, how long after the last one it came (gapMs), and how long the frame
// before worked (workMs: from the last headset frame to this wait). The first
// late frame of a run is logged with the parts of the frame before it
// (SceneLastFrame, the mod's own start and submission; "the rest" is the
// remainder: the game's swap of the monitor's picture, the mod's menu, and so
// on). A run's later frames are paced by the headset itself, which has fallen
// back to every other refresh, so only the first tells what made it late.
static void NoteLateFrame(bool late, double gapMs, double workMs)
{
    unsigned long long updatesNow = CameraPlayerUpdates();
    int updates = (int)(updatesNow - g_updatesAtLastWait);
    g_updatesAtLastWait = updatesNow;
    double parts[4] = {};
    bool toTheEnd = false;
    int worlds = 0;
    SceneLastFrame(parts, toTheEnd, worlds);
    double period = g_statPeriodMs;
    if (period > 1.0 && workMs > period) ++g_statFramesOver;
    if (workMs > g_statLongestWorkMs) g_statLongestWorkMs = workMs;
    if (parts[0] > g_statLongestUpdateMs) g_statLongestUpdateMs = parts[0];

    if (late && !g_lastFrameLate)
    {
        ++g_statLateRuns;
        if (workMs > period) ++g_statLateRunsOver;
        if (updates >= 2) ++g_statLateRunsTwo;
        if (toTheEnd) ++g_statLateRunsFinish;
        if (g_lateLogsLeft > 0)
        {
            --g_lateLogsLeft;
            double rest = workMs - (g_lastBeginTookMs + parts[0] + parts[1] + parts[2] + parts[3] + g_lastEndTookMs);
            Log("LATE: a headset frame came %.1f ms after the one before (it wants one every %.1f): the frame before it worked %.1f ms "
                "(the mod's start %.2f | the game's update %.2f, %d logic update%s at %s rate | the left eye's world %.2f | "
                "the rest of the drawing %.2f | after the world %.2f | the mod's sending %.2f | the rest %.2f%s%s); "
                "the frame before that worked %.1f ms (%d logic update%s).",
                gapMs, period, workMs, g_lastBeginTookMs, parts[0], updates, updates == 1 ? "" : "s", g_logicHalf ? "half" : "full",
                parts[1], parts[2], parts[3], g_lastEndTookMs, rest,
                toTheEnd ? "; it waited for the graphics card (measured to the end)" : "",
                worlds == 1 ? "" : (worlds == 0 ? "; no world drawn" : "; more than one world drawn"),
                g_prevWorkMs, g_prevUpdates, g_prevUpdates == 1 ? "" : "s");
        }
    }
    g_lastFrameLate = late;
    g_prevWorkMs = workMs;
    g_prevUpdates = updates;
}
static unsigned long long g_statHeadsetFrames = 0, g_statLateFrames = 0;
// Frames while playing with no fresh picture of the world (see EndFrameWork),
// and of those, how many had no head pose; and the last frame whose left
// eye's picture could not be taken (stereo in one frame).
static unsigned long long g_statMissedFrames = 0, g_statMissedNoPose = 0;
static unsigned long long g_midFailedSerial = 0;
// How many new pictures each eye got (they should match), and how many headset
// frames went out with both eyes showing.
static unsigned long long g_statLeftPictures = 0, g_statRightPictures = 0, g_statEyesShown = 0;

// For the monitor: a copy of the latest left-eye picture, so the window can
// show one steady eye instead of flickering between the two.
static GLuint g_monitorTexture = 0;
static GLuint g_monitorFramebuffer = 0;
static int g_monitorWidth = 0, g_monitorHeight = 0;
static bool g_monitorHasPicture = false;
// Copies of the mod's own panels for the monitor (the headset gets them as
// panels of their own, which the window never sees): the BunkerVR menu and
// the bullet counter. And whether the counter was shown this frame.
static GLuint g_monitorMenuTexture = 0, g_monitorMenuFramebuffer = 0;
static int g_monitorMenuWidth = 0, g_monitorMenuHeight = 0;
static GLuint g_monitorCounterTexture = 0, g_monitorCounterFramebuffer = 0;
static int g_monitorCounterWidth = 0, g_monitorCounterHeight = 0;
static bool g_counterShownNow = false;

// Keeps a picture in a texture of its own for the monitor (made, or made again
// at a new size, as needed).
static void KeepForMonitor(const unsigned char* pixels, int width, int height, GLuint& texture, GLuint& framebuffer,
                           int& keptWidth, int& keptHeight)
{
    if (texture == 0 || keptWidth != width || keptHeight != height)
    {
        GlDeleteTarget(texture, framebuffer);
        if (!GlCreateTarget(width, height, texture, framebuffer)) return;
        keptWidth = width;
        keptHeight = height;
    }
    GlUploadPicture(texture, width, height, pixels);
}

// The latest head pose, shared with the camera hook.
static SRWLOCK g_poseLock = SRWLOCK_INIT;
static HeadPose g_sharedPose = {};
static HeadPose g_sharedCentrePose = {};        // the middle of the head (between the eyes)
static unsigned long long g_sharedPoseTime = 0; // when it was last updated; 0 = never
static unsigned long long g_sharedSerial = 0;   // which headset frame it belongs to
static int g_sharedEye = 0;                     // and which eye that frame is drawn for (0 = left, 1 = right)

static bool g_swapEyes = false; // testing aid: give each eye the other eye's position

// The drawing target that holds the game's finished picture, which the eyes'
// pictures are cut from and the hands are drawn into. Normally the window
// itself (0), read when the game's frame is over. With stereo in one game
// frame the pictures are taken while the frame is still being drawn, when the
// finished picture sits in the engine's own "screen" target, so both eyes'
// pictures are taken from there instead.
static GLuint g_gamePicture = 0;

// "Straight ahead": the facing direction and position the head pose is measured from.
static bool g_haveReference = false;
static bool g_recenterRequested = false;
// The recenter under way, if any (see VrRecenter).
static double NowMs();
static double g_recentreStartMs = -1.0; // when the current recenter began (below 0: none under way)
static bool g_recentreDone = false;     // has its centering been done yet?
static unsigned g_recentreCount = 0;    // how many times the view has been centered (the first time included)
static const double kRecentreFadeOutMs = 150.0, kRecentreBlackMs = 300.0, kRecentreFadeInMs = 250.0;
static float g_referenceYaw = 0.0f; // radians
// For the next centering only, the facing to take as "straight ahead" instead
// of the head's own (the cutscene screen's, see FaceAfterScene).
static bool g_recentreYawGiven = false;
static float g_recentreYaw = 0.0f;
static float g_referencePosition[3] = { 0, 0, 0 };
static float g_referenceNeckHeight = 0.0f; // how high the neck was then (see NeckHeight)

// How high the top of the neck is, worked out from the head: the point the
// head nods about, 7.5 cm below the eyes and 8 cm behind them. Nodding the
// head down to look at the player's own body swings the eyes forward and DOWN, by
// 10 cm and more; the neck stays where it is. Ducking is judged by the neck,
// so that looking down is not taken for ducking.
static float NeckHeight(const XrPosef& head)
{
    const XrQuaternionf& q = head.orientation;
    const float below = 0.075f, behind = 0.0805f;
    // The height part of that point turned the way the head is turned.
    return head.position.y - below * (1 - 2 * (q.x * q.x + q.z * q.z)) + behind * 2 * (q.y * q.z - q.w * q.x);
}

// The same point in full (room meters): it stays put when the head nods or
// turns, as the eyes do not.
static XrVector3f NeckPoint(const XrPosef& head)
{
    const XrQuaternionf& q = head.orientation;
    const float below = 0.075f, behind = 0.0805f;
    // The head's own "up" and "back" (+Z) directions.
    float ux = 2 * (q.x * q.y - q.w * q.z), uy = 1 - 2 * (q.x * q.x + q.z * q.z), uz = 2 * (q.y * q.z + q.w * q.x);
    float bx = 2 * (q.x * q.z + q.w * q.y), by = 2 * (q.y * q.z - q.w * q.x), bz = 1 - 2 * (q.x * q.x + q.y * q.y);
    XrVector3f neck;
    neck.x = head.position.x - below * ux + behind * bx;
    neck.y = head.position.y - below * uy + behind * by;
    neck.z = head.position.z - below * uz + behind * bz;
    return neck;
}

// The floor (see "Measure my size"): how high it is in the room's
// measure (g_floorSpace, whose zero height is the head's when the headset's
// software last set the room; for some, the floor's). From the play area, whose origin is on the
// floor, each frame (UpdateStageFloor); or, when the headset has none, from
// a controller touched to it while measuring. (With a play area, a touch is
// only a check, in the log: a hand that went low without reaching the floor
// must not move a floor that is right.) The touched one is forgotten when
// the headset's own "Reset view" moves the room.
static bool g_stageFloorKnown = false;
static float g_stageFloorY = 0.0f;
static bool g_touchFloorKnown = false;
static float g_touchFloorY = 0.0f;

// A floor a controller rested on (the start-up page, or the Body page's "Touch
// the floor") is saved as its height above the play area's floor, and the play
// area's floor is offset by that in every later run: a play area's floor can be
// tens of centimeters off (some 35 cm too low on a Rift). `real` (optional):
// set when it is a floor a controller rested on, not the play area's own (nor
// one guessed from the player's height, see SaveFloor).
static bool FloorY(float& y, bool* real = nullptr)
{
    const Settings& s = GetSettings();
    if (real) *real = false;
    if (g_stageFloorKnown)
    {
        y = g_stageFloorY + (s.floorSaved ? s.floorFromPlayArea : 0.0f);
        if (real) *real = s.floorSaved && !s.floorGuessed;
        return true;
    }
    if (g_touchFloorKnown)
    {
        y = g_touchFloorY;
        if (real) *real = !s.floorGuessed;
        return true;
    }
    return false;
}

// A controller has rested on the floor, at `floorY` in the room: kept for this
// run, and (with a play area) as its height above the play area's floor for
// every later run. `guessed`: not rested on, but placed below the seated eyes
// from the player's height (StartupMeasureSitting).
static void SaveFloor(float floorY, const char* how, bool guessed = false)
{
    Settings& s = GetSettings();
    g_touchFloorKnown = true;
    g_touchFloorY = floorY;
    s.floorGuessed = guessed;
    if (g_stageFloorKnown && fabsf(floorY - g_stageFloorY) < 1.0f)
    {
        s.floorSaved = true;
        s.floorFromPlayArea = floorY - g_stageFloorY;
        Log("VR: the floor (%s) is %+.1f cm from the play area's: kept, and used from now on.", how, s.floorFromPlayArea * 100.0f);
    }
    else
    {
        s.floorSaved = false;
        Log("VR: the floor (%s): %s, so it is kept for this run only.", how,
            g_stageFloorKnown ? "more than a metre from the play area's" : "the headset has no play area");
    }
    SaveSettings();
}

// The saved floor belongs to the headset it was measured with. An offset from
// one headset's play area is wrong on another's: a Rift floor kept 38.5 cm above
// its play area's, added to SteamVR's correct play area floor, puts the floor
// that much too high, so the view is centered far too low and the game crouches
// at once. Each headset (its runtime's name and its own) has its own floor: this
// one's in floorHeadset..., one other's kept aside and swapped back when that
// headset is used again. A headset with none of its own yet uses its play
// area's floor unchanged, until a controller rests on the floor (the Body page,
// or the start-up page).
static const char* const kFloorBeforeHeadsets = "Oculus"; // settings saved without a headset key: taken as the Oculus runtime's
static char g_headsetKey[160] = "";

static void ChooseHeadsetFloor(const char* runtimeName, const char* systemName)
{
    Settings& s = GetSettings();
    snprintf(g_headsetKey, sizeof(g_headsetKey), "%s | %s", runtimeName, systemName);
    if (strlen(g_headsetKey) >= sizeof(s.floorHeadset)) g_headsetKey[sizeof(s.floorHeadset) - 1] = '\0';
    for (char* c = g_headsetKey; *c; ++c)
        if (*c == '\r' || *c == '\n') *c = ' ';
    bool oculusSoftware = strncmp(runtimeName, "Oculus", 6) == 0;
    auto isThisHeadset = [&](const char* saved) {
        return strcmp(saved, g_headsetKey) == 0 || (oculusSoftware && strcmp(saved, kFloorBeforeHeadsets) == 0);
    };
    if (s.floorHeadset[0] == '\0') lstrcpynA(s.floorHeadset, kFloorBeforeHeadsets, (int)sizeof(s.floorHeadset));

    const char* how = nullptr;
    if (isThisHeadset(s.floorHeadset))
    {
        how = "this headset's own";
    }
    else if (s.floorOtherHeadset[0] != '\0' && isThisHeadset(s.floorOtherHeadset))
    {
        char otherHeadset[sizeof(s.floorHeadset)];
        lstrcpynA(otherHeadset, s.floorHeadset, (int)sizeof(otherHeadset));
        bool saved = s.floorSaved, guessed = s.floorGuessed;
        float fromPlayArea = s.floorFromPlayArea;
        s.floorSaved = s.floorOtherSaved;
        s.floorFromPlayArea = s.floorOtherFromPlayArea;
        s.floorGuessed = s.floorOtherGuessed;
        lstrcpynA(s.floorOtherHeadset, otherHeadset, (int)sizeof(s.floorOtherHeadset));
        s.floorOtherSaved = saved;
        s.floorOtherFromPlayArea = fromPlayArea;
        s.floorOtherGuessed = guessed;
        how = "this headset's own, kept from when it was used before";
    }
    else
    {
        // The previous headset's floor is kept aside, unless it had none of its own and another is kept already.
        if (s.floorSaved || s.floorOtherHeadset[0] == '\0')
        {
            lstrcpynA(s.floorOtherHeadset, s.floorHeadset, (int)sizeof(s.floorOtherHeadset));
            s.floorOtherSaved = s.floorSaved;
            s.floorOtherFromPlayArea = s.floorFromPlayArea;
            s.floorOtherGuessed = s.floorGuessed;
        }
        s.floorSaved = false;
        s.floorFromPlayArea = 0.0f;
        s.floorGuessed = false;
        how = "a new headset: its play area's floor as it is, until a controller rests on the floor";
    }
    lstrcpynA(s.floorHeadset, g_headsetKey, (int)sizeof(s.floorHeadset));
    if (s.floorSaved)
        Log("VR: the floor for \"%s\": %s, %+.1f cm from its play area's (%s).", g_headsetKey, how, s.floorFromPlayArea * 100.0f,
            s.floorGuessed ? "guessed from your seated height" : "a controller rested on it");
    else
        Log("VR: the floor for \"%s\": %s.", g_headsetKey, how);
    if (s.floorOtherHeadset[0] != '\0' && s.floorOtherSaved)
        Log("VR: kept aside for \"%s\": its floor %+.1f cm from its play area's.", s.floorOtherHeadset, s.floorOtherFromPlayArea * 100.0f);
    SaveSettings();
}

// Once per headset frame. A play area is believed when it is well below the
// room's zero (the head's height when the headset's software set the room),
// or, on SteamVR, whose play area always comes from its room setup and which
// can set the room on the floor itself (fed by Virtual Desktop: the head some
// 1.6 m up in the room), when the head is well above it (kStageHeadAbove);
// then for as long as it stays put. (Elsewhere one near the room's zero is not
// believed: some headsets give the room itself when no floor is set, and a
// room set with the headset lying low would pass for a floor.)
static const float kStageHeadAbove = 0.8f;
static void UpdateStageFloor(XrTime time, bool headKnown, float headY)
{
    if (g_stageSpace == XR_NULL_HANDLE) return;
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
    bool located = XR_SUCCEEDED(xrLocateSpace(g_stageSpace, g_floorSpace, time, &location)) &&
                   (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    float y = location.pose.position.y;
    bool known = located && (y < -0.30f || (g_runtimeSteamVr && headKnown && headY - y > kStageHeadAbove) ||
                             (g_stageFloorKnown && fabsf(y - g_stageFloorY) < 0.02f));
    if (known != g_stageFloorKnown)
    {
        static int logsLeft = 10;
        if (logsLeft > 0)
        {
            --logsLeft;
            if (known && y < -0.30f) Log("VR: the play area's floor is %.0f cm below the room's zero height.", -y * 100.0f);
            else if (known) Log("VR: the play area's floor is %.0f cm from the room's zero height, the head %.0f cm above it: the room is set on the floor.",
                                y * 100.0f, (headY - y) * 100.0f);
            else Log("VR: the play area's floor is no longer known.");
        }
    }
    // Logged in case the play area's floor moves when the headset software resets the view.
    else if (known && fabsf(location.pose.position.y - g_stageFloorY) > 0.02f)
    {
        static int moveLogsLeft = 10;
        if (moveLogsLeft > 0)
        {
            --moveLogsLeft;
            Log("VR: the play area's floor moved to %.0f cm below the room's zero height.", -location.pose.position.y * 100.0f);
        }
    }
    g_stageFloorKnown = known;
    if (known) g_stageFloorY = location.pose.position.y;
}

// The saved height: at every centering (the player's own, after a scene,
// leaving the main menu, a new level), the view's "straight ahead" height is
// put at the measured eye height above the floor, whatever the head's height
// at the time. Standing: the standing eye height, so the game's floor is where
// the real floor is. Seated: the seated eye height from the start-up page
// (saved, never re-measured), so a centering made while leaning or reaching
// doesn't leave the player higher or lower for the rest of the level. With no
// height or floor known, the head's own height is kept. (The neck's height is
// a level head's, see NeckHeight.)
// A seated height that was only guessed from the player's height, with the
// floor then placed below it on another headset, isn't used on a headset with
// no floor of its own: there the view keeps the head's own height when
// centered, as with no height saved, until "Calibrate Seated Height" measures
// it there. Such a guess can be several centimeters off on another headset.
static bool SavedHeightApplies()
{
    const Settings& s = GetSettings();
    return !(s.seated && s.seatedEyesGuessed && !s.floorSaved);
}

static void SavedHeightFromFloor(const XrPosef& head, const char* when)
{
    const Settings& s = GetSettings();
    float eyes = s.seated ? s.seatedEyes : s.eyeHeight;
    float floorY = 0.0f;
    if (eyes <= 0.0f || !FloorY(floorY)) return;
    if (!SavedHeightApplies())
    {
        static int skippedLogsLeft = 5;
        if (skippedLogsLeft > 0)
        {
            --skippedLogsLeft;
            Log("VR: playing seated (%s): the view keeps your head's own height, %.1f cm above this headset's floor (your seated eyes, %.1f cm, "
                "were guessed from your height on another headset; Calibrate Seated Height measures them here).", when,
                (head.position.y - floorY) * 100.0f, eyes * 100.0f);
        }
        return;
    }
    g_referencePosition[1] = floorY + eyes;
    g_referenceNeckHeight = floorY + eyes - 0.075f;
    static int logsLeft = 20;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: playing %s (%s): the view's height is your eyes' measured %.1f cm above the floor (the head was %.1f cm up).",
            s.seated ? "seated" : "standing", when, eyes * 100.0f, (head.position.y - floorY) * 100.0f);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Logs a failed OpenXR call in readable form. Returns true if the call worked.
static bool Check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result)) return true;

    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (g_instance != XR_NULL_HANDLE)
        xrResultToString(g_instance, result, name);

    Log("VR ERROR: %s failed: %s (code %d)", what, name, (int)result);
    return false;
}

static const char* SessionStateName(XrSessionState state)
{
    switch (state)
    {
    case XR_SESSION_STATE_IDLE:         return "IDLE (waiting)";
    case XR_SESSION_STATE_READY:        return "READY (headset ready to start)";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED (running, not shown in headset)";
    case XR_SESSION_STATE_VISIBLE:      return "VISIBLE (shown in headset)";
    case XR_SESSION_STATE_FOCUSED:      return "FOCUSED (shown in headset and has input)";
    case XR_SESSION_STATE_STOPPING:     return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING (headset software is going away)";
    case XR_SESSION_STATE_EXITING:      return "EXITING";
    default:                            return "(other)";
    }
}

static float ToDegrees(float radians)
{
    return radians * 57.29578f;
}

// Turns the headset's rotation (stored as a quaternion) into three angles
// that are easier to read in a log.
//   yaw:   turning left (+) or right (-)
//   pitch: looking up (+) or down (-)
//   roll:  tilting the head towards the left shoulder (+) or right shoulder (-)
static void ToYawPitchRoll(const XrQuaternionf& q, float& yaw, float& pitch, float& roll)
{
    float sinPitch = 2.0f * (q.w * q.x - q.y * q.z);
    if (sinPitch > 1.0f) sinPitch = 1.0f;
    if (sinPitch < -1.0f) sinPitch = -1.0f;

    pitch = ToDegrees(asinf(sinPitch));
    yaw = ToDegrees(atan2f(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)));
    roll = ToDegrees(atan2f(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z)));
}

// Converts a pose measured in the room into one measured from "straight
// ahead" (the remembered facing direction and position), with the world scale
// applied to distances.
static void ToRelativePose(const XrPosef& pose, HeadPose& result)
{
    const XrQuaternionf& q = pose.orientation;

    // The rotation as a 3x3 grid.
    float r[9] = {
        1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.w * q.z),     2 * (q.x * q.z + q.w * q.y),
        2 * (q.x * q.y + q.w * q.z),     1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.w * q.x),
        2 * (q.x * q.z - q.w * q.y),     2 * (q.y * q.z + q.w * q.x),     1 - 2 * (q.x * q.x + q.y * q.y),
    };

    // Undo the remembered facing direction: turn everything by -referenceYaw around the up axis.
    float c = cosf(g_referenceYaw);
    float s = sinf(g_referenceYaw);

    for (int column = 0; column < 3; ++column)
    {
        float x = r[0 + column], y = r[3 + column], z = r[6 + column];
        result.rotation[0 + column] = c * x - s * z;
        result.rotation[3 + column] = y;
        result.rotation[6 + column] = s * x + c * z;
    }

    // World scale: multiplying how far the eye has moved (which includes the
    // distance between the two eyes) makes the player bigger or smaller in
    // the game's world, so the world looks smaller or bigger in return.
    float scale = GetSettings().worldScale;
    float dx = (pose.position.x - g_referencePosition[0]) * scale;
    // The height setting lifts (or lowers) everything measured this way: head and hands alike.
    float dy = (pose.position.y - g_referencePosition[1] + GetSettings().heightOffset) * scale;
    float dz = (pose.position.z - g_referencePosition[2]) * scale;
    result.position[0] = c * dx - s * dz;
    result.position[1] = dy;
    result.position[2] = s * dx + c * dz;
}

// Turning with the stick about the head (see PublishHeadPose): the game's
// camera turn last followed, and when it was last followed as the world's
// drawing started.
static bool g_turnHaveYaw = false;
static float g_turnLastYaw = 0.0f;
static double g_turnAtDrawingMs = -1000000.0;

// The game's camera has turned to `yawNow`: the middle of the play area is
// turned the other way about the head by as much as it turned since last
// time, so that the head stays where it is in the game's world.
static void TurnAboutHead(float yawNow, const XrPosef& headPose)
{
    if (g_turnHaveYaw)
    {
        float turned = yawNow - g_turnLastYaw;
        if (turned > 3.14159265f) turned -= 6.28318531f;
        if (turned < -3.14159265f) turned += 6.28318531f;
        if (fabsf(turned) > 0.00001f && fabsf(turned) < 1.75f)
        {
            float dx = headPose.position.x - g_referencePosition[0];
            float dz = headPose.position.z - g_referencePosition[2];
            float c = cosf(turned), s = sinf(turned);
            // The offset turned the other way (to the right for a turn to the left).
            float nx = c * dx - s * dz;
            float nz = s * dx + c * dz;
            g_referencePosition[0] = headPose.position.x - nx;
            g_referencePosition[2] = headPose.position.z - nz;
        }
    }
    g_turnLastYaw = yawNow;
    g_turnHaveYaw = true;
}

// ---- Holding the head's turn (cVR_HoldHeadTurn, PlayerBodyHandler.hps) ----
// Used during the monster's front kill, so the player can't turn away from it.
// While the body script asks for it (every update; kept kHoldTurnLapseMs without a
// new request), the world's pictures are drawn as from a head that doesn't turn:
// level, facing the way the real head faced when first asked (flattened as the
// script's VRHeadFacing does it: LevelFacing), at the real head's place, with the
// eye spacing turned with it. The headset is still told where the eyes really are
// (g_frameEyePose is left alone), so it shows each picture straight in front of the
// eyes wherever the real head turns: the view stays on what the kill shows. The
// scripts' head position (roll 7005, the center of the head) has the held turn
// too, so their math for lining up the kill agrees with what is drawn. Started and
// stopped only with the first picture of a game frame, so both eyes of a pair
// always agree. Dropped once not asked for kHoldTurnLapseMs, unless the game is
// paused (asked again as it goes on, the same facing is held).
static const double kHoldTurnLapseMs = 1000.0;
static double g_holdTurnAskedMs = 0.0;                        // when the body script last asked (0: not asked)
static bool g_holdTurnKnown = false;                          // the turn to hold is known (g_holdTurn)
static float g_holdTurn[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // the held turn, row-major, in the same frame as the head's pose
static bool g_holdTurnOn = false;                             // this game frame's pictures are drawn with it

// The horizontal facing of a turn (row-major: column 1 its up, column 2 its back):
// its forward with its up added as far as it is tipped (so it is known when looking
// far down too), as VRHeadFacing in PlayerBodyHandler.hps and the walking stick
// (UpdateMoveInput) compute it. False when it can't be told (rolled on its side).
static bool LevelFacing(const float* r, float& x, float& z)
{
    float forwardY = -r[5];
    x = -r[2] - forwardY * r[1];
    z = -r[8] - forwardY * r[7];
    float length = sqrtf(x * x + z * z);
    if (length < 0.3f) return false;
    x /= length;
    z /= length;
    return true;
}

// The level turn that faces (x, 0, z) (a unit long), row after row: its right
// (-z, 0, x), its up straight up, its back (-x, 0, -z).
static void LevelTurnFacing(float x, float z, float turn[9])
{
    turn[0] = -z;   turn[1] = 0.0f; turn[2] = -x;
    turn[3] = 0.0f; turn[4] = 1.0f; turn[5] = 0.0f;
    turn[6] = x;    turn[7] = 0.0f; turn[8] = -z;
}

// The held turn put on an eye's pose and the head's middle (both measured from
// "straight ahead"): the real head's turn taken off about its middle and the held
// one put on, so the eye keeps its place on the head, and the middle its place.
static void HoldTurnOn(const float held[9], HeadPose& eye, HeadPose& centre)
{
    // held times the inverse of the real head's turn (a rotation's inverse is its transpose)
    const float* real = centre.rotation;
    float change[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            change[row * 3 + column] = held[row * 3 + 0] * real[column * 3 + 0] + held[row * 3 + 1] * real[column * 3 + 1] +
                                       held[row * 3 + 2] * real[column * 3 + 2];
    float turned[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            turned[row * 3 + column] = change[row * 3 + 0] * eye.rotation[0 * 3 + column] + change[row * 3 + 1] * eye.rotation[1 * 3 + column] +
                                       change[row * 3 + 2] * eye.rotation[2 * 3 + column];
    float fromMiddle[3] = { eye.position[0] - centre.position[0], eye.position[1] - centre.position[1], eye.position[2] - centre.position[2] };
    for (int row = 0; row < 3; ++row)
        eye.position[row] = centre.position[row] + change[row * 3 + 0] * fromMiddle[0] + change[row * 3 + 1] * fromMiddle[1] +
                            change[row * 3 + 2] * fromMiddle[2];
    for (int i = 0; i < 9; ++i)
    {
        eye.rotation[i] = turned[i];
        centre.rotation[i] = held[i];
    }
}

// With a game frame's first picture: is the head's turn held for this frame's
// pictures (g_holdTurnOn)?
static void UpdateHoldTurn()
{
    double now = NowMs();
    bool asked = g_holdTurnAskedMs > 0.0 && now - g_holdTurnAskedMs < kHoldTurnLapseMs;
    if (!asked && g_holdTurnKnown && g_holdTurnAskedMs > 0.0 && !CameraGamePaused())
    {
        g_holdTurnKnown = false;
        Log("VR: the head's turn is no longer held: not asked for %.1f s.", (now - g_holdTurnAskedMs) / 1000.0);
    }
    bool on = asked && g_holdTurnKnown;
    if (on != g_holdTurnOn)
        Log("VR: the world's pictures are drawn %s.", on ? "with the head's turn held (the view stays put in front of the eyes as the head turns)"
                                                       : "with the head's own turn again");
    g_holdTurnOn = on;
}

// Works out a pose relative to "straight ahead" and shares it with the camera
// hook. `headPose` is the middle of the head and is only used for centering;
// `pose` is what gets shared (the position of the eye being drawn this frame).
static void PublishHeadPose(const XrPosef& headPose, const XrPosef& pose, bool firstPictureOfFrame = true)
{
    // The first time (or when asked), remember which way the head faces and
    // where it is. Only the left/right facing is remembered, so "level" stays
    // level even if the player was looking down a little at that moment.
    if (!g_haveReference || g_recenterRequested)
    {
        const XrQuaternionf& h = headPose.orientation;
        g_referenceYaw = atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));
        if (g_recentreYawGiven) g_referenceYaw = g_recentreYaw; // see FaceAfterScene
        g_recentreYawGiven = false;
        g_referencePosition[0] = headPose.position.x;
        g_referencePosition[1] = headPose.position.y;
        g_referencePosition[2] = headPose.position.z;
        g_referenceNeckHeight = NeckHeight(headPose);
        g_haveReference = true;
        g_recenterRequested = false;
        ++g_recentreCount;
        Log("VR: view centred. Current head direction is now \"straight ahead\".");
        SavedHeightFromFloor(headPose, "the view centred"); // standing or seated
    }

    // Stick turning turns the player about their own head. The game turns its
    // camera, and the real head's offset from the middle of the play area is
    // turned along with it, so a head away from that middle (leaning, or
    // sitting forward of where the view was centered) would swing around in a
    // circle. So whenever the game's camera turns, the middle is turned the
    // other way about the head by as much, and the head stays where it is in
    // the game's world. (Not on the cutscene screen, and not for a jump of
    // more than about 100 degrees in one frame, which is the game placing the
    // camera, not a turn.)
    // Only for the first picture of a game frame: both eyes are drawn in one
    // game frame, and the right eye's picture is set up halfway through it,
    // after the game has turned its camera for this frame. Done there too,
    // the right eye would be a turn step ahead of the left one while turning,
    // and near things (the hands, what they hold) would lose their depth and
    // shake. The step is made with the next frame's first picture instead,
    // for both eyes alike.
    // Here, at the start of a frame, the game's known turn is the previous
    // frame's, from when its view was built, so the step would come a frame
    // late: the head would swing by a frame's turn as a turn begins and come
    // back as it ends, more the further the head is from the middle (as when
    // playing standing). So the step is normally made as the world's drawing
    // starts, with the turn the game has made for this frame
    // (VrWorldDrawingStarts, from scene.cpp); here only if that hasn't
    // happened lately.
    if (firstPictureOfFrame && NowMs() - g_turnAtDrawingMs > 500.0)
    {
        float yawNow = 0.0f;
        if (g_haveReference && CameraGetGameYaw(yawNow) && !CameraTheatre()) TurnAboutHead(yawNow, headPose);
        else g_turnHaveYaw = false;
    }

    HeadPose result, centre;
    ToRelativePose(pose, result);
    ToRelativePose(headPose, centre);

    // The head's turn held (see g_holdTurnOn): started or stopped only with a
    // game frame's first picture, the left eye's.
    if (firstPictureOfFrame && g_frameEye == 0) UpdateHoldTurn();
    if (g_holdTurnOn) HoldTurnOn(g_holdTurn, result, centre);

    AcquireSRWLockExclusive(&g_poseLock);
    g_sharedPose = result;
    g_sharedCentrePose = centre;
    g_sharedPoseTime = GetTickCount64();
    g_sharedSerial = g_frameNumber;
    g_sharedEye = g_frameEye;
    ReleaseSRWLockExclusive(&g_poseLock);
}

unsigned long long VrGetFrameSerial()
{
    AcquireSRWLockShared(&g_poseLock);
    unsigned long long serial = g_sharedSerial;
    unsigned long long time = g_sharedPoseTime;
    ReleaseSRWLockShared(&g_poseLock);

    if (time == 0 || GetTickCount64() - time >= 500) return 0;
    return serial;
}

bool VrGetGrabHandPose(HeadPose& pose, int& handIndex)
{
    if (!g_grabHeld) return false;

    int hand = g_grabHand;
    AcquireSRWLockShared(&g_poseLock);
    XrPosef room = g_sharedHandRoom[hand];
    unsigned long long time = g_sharedHandTime[hand];
    ReleaseSRWLockShared(&g_poseLock);
    ToRelativePose(room, pose); // converted at call time: see g_sharedHandRoom
    HandInFrontPose(hand, pose); // (or floating in front: see g_inFrontOn)

    handIndex = hand;
    return time != 0 && GetTickCount64() - time < 300;
}

bool VrGetPointingHandPose(HeadPose& pose, int& handIndex)
{
    unsigned long long now = GetTickCount64();
    bool found = false;
    XrPosef room = {};

    AcquireSRWLockShared(&g_poseLock);
    // Prefer the hand that squeezed last, then the right hand, then the left.
    int order[3] = { g_grabHand, 1, 0 };
    for (int i = 0; i < 3 && !found; ++i)
    {
        int hand = order[i];
        if (g_sharedHandTime[hand] != 0 && (g_headsetOff || g_handsFrozen || now - g_sharedHandTime[hand] < 300))
        {
            room = g_sharedHandRoom[hand];
            handIndex = hand;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_poseLock);
    if (found) ToRelativePose(room, pose); // converted at call time: see g_sharedHandRoom
    if (found) HandInFrontPose(handIndex, pose); // (or floating in front: see g_inFrontOn)
    return found;
}

void VrStepWorldScale(int steps)
{
    Settings& s = GetSettings();
    SetSizeStep(s.sizeStep + steps);
    SaveSettings();
    Log("VR: world scale is now %.3f (calibrated %.3f, adjusted %+d%%; the world looks %.0f%% of the size it does at 1.00).",
        s.worldScale, s.worldScaleCalibrated, s.sizeStep * 5, 100.0f / s.worldScale);
}

void VrToggleSwapEyes()
{
    g_swapEyes = !g_swapEyes;
    Log("VR: eyes are now %s.", g_swapEyes ? "SWAPPED (left picture uses the right eye's position)" : "normal");
}

bool VrGetHeadPose(HeadPose& pose)
{
    AcquireSRWLockShared(&g_poseLock);
    pose = g_sharedPose;
    unsigned long long time = g_sharedPoseTime;
    ReleaseSRWLockShared(&g_poseLock);

    // Only use a pose that is fresh (updated within the last half second).
    return time != 0 && (g_headsetOff || GetTickCount64() - time < 500); // (held while the headset is off: see g_headsetOff)
}

bool VrGetHeadCentrePose(HeadPose& pose)
{
    AcquireSRWLockShared(&g_poseLock);
    pose = g_sharedCentrePose;
    unsigned long long time = g_sharedPoseTime;
    ReleaseSRWLockShared(&g_poseLock);

    return time != 0 && (g_headsetOff || GetTickCount64() - time < 500);
}

bool VrHoldHeadTurn(bool hold)
{
    if (!hold)
    {
        if (g_holdTurnKnown) Log("VR: the head's turn is let go (the body script's word); from the next picture the view turns with the head again.");
        g_holdTurnKnown = false;
        g_holdTurnAskedMs = 0.0;
        return false;
    }
    if (!g_holdTurnKnown)
    {
        // The facing to hold: the real head's as the scripts last read it (the
        // middle of the head as shared this frame). While the pictures still have
        // an earlier hold on (let go this very frame), that one is held again, so
        // that the two eyes of this frame agree.
        if (!g_holdTurnOn)
        {
            HeadPose centre;
            float x = 0.0f, z = 0.0f;
            if (!VrGetHeadCentrePose(centre) || !LevelFacing(centre.rotation, x, z)) return false;
            LevelTurnFacing(x, z, g_holdTurn);
        }
        g_holdTurnKnown = true;
        Log("VR: the head's turn is held from the next picture (facing %.0f degrees left of straight ahead, level); its place is still followed.",
            ToDegrees(atan2f(g_holdTurn[2], g_holdTurn[8]))); // (the back's opposite, as the walking stick's turn: + to the left)
    }
    g_holdTurnAskedMs = NowMs();
    return g_holdTurnOn;
}

void VrAdjustTurnSpeed(float factor)
{
    float speed = GetSettings().turnSpeed * factor;
    if (speed < 200.0f) speed = 200.0f;
    if (speed > 20000.0f) speed = 20000.0f;
    GetSettings().turnSpeed = speed;
    SaveSettings();
    Log("VR: thumbstick turn speed is now %.0f.", speed);
}

// The recenter, done the same way whatever asks for it (the left menu
// button held, the VR menu, the Home key, the headset's own "Reset view",
// the headset picked up at the start): the eyes darken over
// kRecentreFadeOutMs, the view is centered while they are black (and the
// scripts can see it was, VrGetRecentreCount, so the body is fitted to the
// player again), both controllers buzz, the eyes stay black kRecentreBlackMs
// while the body settles, and clear over kRecentreFadeInMs. A fade instead
// of a sudden jump makes it feel like part of the game.

void VrRecenter()
{
    // Asked again while one is under way: that one carries on.
    if (g_recentreStartMs >= 0.0) return;
    g_recentreStartMs = NowMs();
    g_recentreDone = false;
}

// The same, but black at once (the headset's own "Reset view" has already
// moved its view, so there is nothing worth fading from).
static void RecenterBlackNow()
{
    g_recentreStartMs = NowMs() - kRecentreFadeOutMs;
    g_recentreDone = false;
}

unsigned VrGetRecentreCount()
{
    return g_recentreCount;
}

// How dark the eyes are for a recenter (0 to 1).
static float RecentreFade()
{
    if (g_recentreStartMs < 0.0) return 0.0f;
    double t = NowMs() - g_recentreStartMs;
    if (t < kRecentreFadeOutMs) return (float)(t / kRecentreFadeOutMs);
    t -= kRecentreFadeOutMs;
    if (t < kRecentreBlackMs) return 1.0f;
    t -= kRecentreBlackMs;
    if (t < kRecentreFadeInMs) return 1.0f - (float)(t / kRecentreFadeInMs);
    return 0.0f;
}

int VrGetFrameEye()
{
    if (VrGetFrameSerial() == 0) return -1;

    // The eye the frame was really begun for. Don't work it out from the
    // frame number (odd = left, even = right): that holds while the eyes
    // strictly take turns, but with both eyes drawn in one game frame a
    // single frame that can't be done that way shifts the count by one, and
    // from then on each eye is taken for the other: the left eye's picture is
    // drawn from where the game's camera was a frame earlier, so one eye
    // trails the other whenever the player moves or turns.
    AcquireSRWLockShared(&g_poseLock);
    int eye = g_sharedEye;
    ReleaseSRWLockShared(&g_poseLock);
    return eye;
}

bool VrGetGripHandPose(int hand, HeadPose& pose)
{
    if (hand < 0 || hand > 1) return false;
    if (HandInFrontPose(hand, pose)) return true; // (floating in front: see g_inFrontOn)

    AcquireSRWLockShared(&g_poseLock);
    XrPosef room = g_sharedGripRoom[hand];
    unsigned long long time = g_sharedGripTime[hand];
    ReleaseSRWLockShared(&g_poseLock);
    ToRelativePose(room, pose); // converted at call time: see g_sharedGripRoom

    return time != 0 && (g_headsetOff || g_handsFrozen || GetTickCount64() - time < 300);
}

bool VrGameHandsEnabled()
{
    return GetSettings().gameHands;
}

static float g_handCurl[2] = { 0.0f, 0.0f }; // how far each hand's fingers are closed, 0 to 1

float VrGetHandCurl(int hand)
{
    if (hand != 0 && hand != 1) return 0.0f;

    // A hand that holds one of the game's things (flashlight, bandage,
    // grenade, tools) keeps the fingers the game's own animation gives it,
    // wrapped around the thing. Closing them further with the squeeze would
    // push them through it.
    if ((hand == 0 ? CameraLeftItemKind() : CameraRightItemKind()) != kItemNone) return 0.0f;

    return g_handCurl[hand];
}

int VrGetFingers(int hand, float curls[3])
{
    curls[0] = curls[1] = curls[2] = 0.0f;
    if (hand != 0 && hand != 1) return 0;

    // A hand that holds one of the game's things keeps the game's fingers.
    if ((hand == 0 ? CameraLeftItemKind() : CameraRightItemKind()) != kItemNone) return 2;

    for (int i = 0; i < 3; ++i) curls[i] = g_fingerCurl[hand][i];
    return 1;
}
static bool g_armNotesRequested = false;   // the arm script should send its numbers to the log once
static bool g_handAxesLogRequested = false;

void VrRequestArmNotes()
{
    g_armNotesRequested = true;
    g_handAxesLogRequested = true;
}

bool VrTakeArmNotesRequest()
{
    bool requested = g_armNotesRequested;
    g_armNotesRequested = false;
    return requested;
}

void VrToggleBigBody()
{
    GetSettings().bigBody = !GetSettings().bigBody;
    SaveSettings();
    Log("VR: the game's body is now drawn at %s size.", GetSettings().bigBody ? "the player's (world scale)" : "its own");
}

float VrGetBodyScale()
{
    // In VR the player is `worldScale` times bigger than the character, so its body
    // (hands and arms included) is drawn that many times bigger to match.
    if (!GetSettings().bigBody) return 1.0f;
    float scale = GetSettings().worldScale;
    if (scale < 0.6f) scale = 0.6f;
    if (scale > 2.9f) scale = 2.9f;
    return scale;
}

float VrGetArmScale()
{
    // Resizing the arm bones on their own turns the arm meshes into ribbons,
    // so the arms are never resized separately from the body.
    return 1.0f;
}

void VrToggleGameHands()
{
    GetSettings().gameHands = !GetSettings().gameHands;
    SaveSettings();
    Log("VR: the game's own arms %s follow the controllers.", GetSettings().gameHands ? "now" : "no longer");
}

void VrLogTimings()
{
    if (g_statFrames == 0) return;
    double frames = (double)g_statFrames;
    Log("VR: per frame, the mod's own work took %.2f ms (sending) + %.2f ms (starting); "
        "%.2f ms was spent waiting for the headset (near 0 means the game cannot keep up).",
        g_statEndMs / frames, g_statBeginMs / frames, g_statWaitMs / frames);
    {
        // The submission time by part. "The rest" is what the parts leave out: the checks
        // of what to show, the cutscene screen, the dedication, the captures for marks.
        double parts = 0.0;
        for (int i = 0; i < kSendParts; ++i) parts += g_sendPartMs[i];
        Log("VR: the mod's sending by part, per frame (ms): the hands and effects drawn into the pictures %.2f | the eyes' pictures %.2f | "
            "the interface's pictures %.2f (the big panel's brought up to date %.2f times a frame) | the menu, counter, hints and pointer %.2f | "
            "handing the frame over %.2f | the monitor's picture %.2f | the rest %.2f.",
            g_sendPartMs[kSendPartHands] / frames, g_sendPartMs[kSendPartEyes] / frames, g_sendPartMs[kSendPartInterface] / frames,
            (double)g_statPanelCopies / frames, g_sendPartMs[kSendPartOverlays] / frames, g_sendPartMs[kSendPartEndFrame] / frames,
            g_sendPartMs[kSendPartMonitor] / frames, (g_statEndMs - parts) / frames);
        for (int i = 0; i < kSendParts; ++i) g_sendPartMs[i] = 0.0;
        g_statPanelCopies = 0;
    }
    g_statEndMs = g_statBeginMs = g_statWaitMs = 0;
    g_statFrames = 0;

    if (g_statHeadsetFrames > 0)
    {
        const Settings& settings = GetSettings();
        Log("VR: headset frames: %llu, of which %llu came late (longest gap %.1f ms; the headset wants one every %.1f ms). "
            "Eyes: %s.",
            g_statHeadsetFrames, g_statLateFrames, g_statWorstGapMs, g_statPeriodMs,
            settings.stereoFrame ? "both drawn inside one game frame"
                                 : (settings.pairFrames ? "both drawn per headset frame"
                                                        : (settings.bothEyes ? "taking turns, same moment" : "taking turns")));
        // If one eye ever lags the other, these two numbers will differ.
        Log("VR: new pictures: left eye %llu, right eye %llu; frames shown with both eyes %llu.",
            g_statLeftPictures, g_statRightPictures, g_statEyesShown);
        if (settings.logicAtHeadsetRate && settings.logicHalfRate)
            Log("VR: the game's logic was at half the headset's rate for %llu of those frames; it changed rate %llu times.",
                g_statHalfFrames, g_statLogicChanges);
        Log("VR: frames with no fresh picture of the world while playing: %llu (with no head pose: %llu); the last pictures were shown again for them.",
            g_statMissedFrames, g_statMissedNoPose);
        // See NoteLateFrame.
        Log("VR: late frames came in %llu runs; the frame before the first of a run had worked longer than the headset's frame time "
            "%llu times, run two or more of the game's logic updates %llu times, waited for the graphics card %llu times. "
            "Frames that worked longer than the headset's frame time: %llu (the longest %.1f ms; the longest update of the game %.1f ms).",
            g_statLateRuns, g_statLateRunsOver, g_statLateRunsTwo, g_statLateRunsFinish, g_statFramesOver,
            g_statLongestWorkMs, g_statLongestUpdateMs);
    }
    g_statLateRuns = g_statLateRunsOver = g_statLateRunsTwo = g_statLateRunsFinish = g_statFramesOver = 0;
    g_statLongestWorkMs = g_statLongestUpdateMs = 0;
    g_statLeftPictures = g_statRightPictures = g_statEyesShown = 0;
    g_statHeadsetFrames = g_statLateFrames = 0;
    g_statHalfFrames = g_statLogicChanges = 0;
    g_statMissedFrames = g_statMissedNoPose = 0;
    g_statWorstGapMs = 0;
}

static double NowMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

// ---------------------------------------------------------------------------
// Hand controllers
// ---------------------------------------------------------------------------

static XrPath ToPath(const char* text)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(g_instance, text, &path);
    return path;
}

// Tells the VR software which controller readings we want. Runs once, after
// the session has been created. If anything fails, VR carries on without hands.
static bool SetupHands()
{
    XrPath hands[2] = { ToPath("/user/hand/left"), ToPath("/user/hand/right") };
    g_handPaths[0] = hands[0];
    g_handPaths[1] = hands[1];

    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    lstrcpynA(setInfo.actionSetName, "bunkervr", XR_MAX_ACTION_SET_NAME_SIZE);
    lstrcpynA(setInfo.localizedActionSetName, "BunkerVR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    if (!Check(xrCreateActionSet(g_instance, &setInfo, &g_actionSet), "Creating the controller input set"))
        return false;

    XrActionCreateInfo gripInfo = { XR_TYPE_ACTION_CREATE_INFO };
    gripInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    lstrcpynA(gripInfo.actionName, "hand_grip", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(gripInfo.localizedActionName, "Hand position", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    gripInfo.countSubactionPaths = 2;
    gripInfo.subactionPaths = hands;
    if (!Check(xrCreateAction(g_actionSet, &gripInfo, &g_gripAction), "Creating the hand position input"))
        return false;

    XrActionCreateInfo aimInfo = { XR_TYPE_ACTION_CREATE_INFO };
    aimInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    lstrcpynA(aimInfo.actionName, "hand_aim", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(aimInfo.localizedActionName, "Hand pointing direction", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    aimInfo.countSubactionPaths = 2;
    aimInfo.subactionPaths = hands;
    if (!Check(xrCreateAction(g_actionSet, &aimInfo, &g_aimAction), "Creating the hand pointing input"))
        return false;

    // The grab controls: how far each grip button and trigger is squeezed (0 to 1).
    XrActionCreateInfo squeezeInfo = { XR_TYPE_ACTION_CREATE_INFO };
    squeezeInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    lstrcpynA(squeezeInfo.actionName, "grab_squeeze", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(squeezeInfo.localizedActionName, "Grab (grip button)", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    squeezeInfo.countSubactionPaths = 2;
    squeezeInfo.subactionPaths = hands;

    XrActionCreateInfo triggerInfo = { XR_TYPE_ACTION_CREATE_INFO };
    triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    lstrcpynA(triggerInfo.actionName, "grab_trigger", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(triggerInfo.localizedActionName, "Grab (trigger)", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    triggerInfo.countSubactionPaths = 2;
    triggerInfo.subactionPaths = hands;

    g_grabButtonsReady =
        Check(xrCreateAction(g_actionSet, &squeezeInfo, &g_squeezeAction), "Creating the grip button input") &&
        Check(xrCreateAction(g_actionSet, &triggerInfo, &g_triggerAction), "Creating the trigger input");

    // The thumbsticks: each reports how far it is pushed sideways and forward (-1 to 1).
    XrActionCreateInfo moveInfo = { XR_TYPE_ACTION_CREATE_INFO };
    moveInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    lstrcpynA(moveInfo.actionName, "move", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(moveInfo.localizedActionName, "Walk", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrActionCreateInfo turnInfo = { XR_TYPE_ACTION_CREATE_INFO };
    turnInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    lstrcpynA(turnInfo.actionName, "turn", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(turnInfo.localizedActionName, "Turn", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_sticksReady = g_grabButtonsReady &&
        Check(xrCreateAction(g_actionSet, &moveInfo, &g_moveAction), "Creating the walk input") &&
        Check(xrCreateAction(g_actionSet, &turnInfo, &g_turnAction), "Creating the turn input");

    // B: pressed or not. Named "crouch", it opens the inventory.
    XrActionCreateInfo crouchInfo = { XR_TYPE_ACTION_CREATE_INFO };
    crouchInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(crouchInfo.actionName, "crouch", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(crouchInfo.localizedActionName, "Crouch", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_crouchReady = g_sticksReady &&
        Check(xrCreateAction(g_actionSet, &crouchInfo, &g_crouchAction), "Creating the crouch input");

    // Two more buttons: sprint and menu.
    XrActionCreateInfo sprintInfo = { XR_TYPE_ACTION_CREATE_INFO };
    sprintInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(sprintInfo.actionName, "sprint", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(sprintInfo.localizedActionName, "Sprint", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrActionCreateInfo menuInfo = { XR_TYPE_ACTION_CREATE_INFO };
    menuInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(menuInfo.actionName, "menu", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(menuInfo.localizedActionName, "Menu", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_moreButtonsReady = g_crouchReady &&
        Check(xrCreateAction(g_actionSet, &sprintInfo, &g_sprintAction), "Creating the sprint input") &&
        Check(xrCreateAction(g_actionSet, &menuInfo, &g_menuAction), "Creating the menu input");

    // And two more: A for the gun, and X. (The A action keeps its first name,
    // "holster", which the runtimes' saved bindings know it by.)
    XrActionCreateInfo gunInfo = { XR_TYPE_ACTION_CREATE_INFO };
    gunInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(gunInfo.actionName, "holster", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(gunInfo.localizedActionName, "Open revolver", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    XrActionCreateInfo aimInfo2 = { XR_TYPE_ACTION_CREATE_INFO };
    aimInfo2.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(aimInfo2.actionName, "aim_gun", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(aimInfo2.localizedActionName, "Aim / second action", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_gunButtonsReady = g_moreButtonsReady &&
        Check(xrCreateAction(g_actionSet, &gunInfo, &g_gunAction), "Creating the A button input") &&
        Check(xrCreateAction(g_actionSet, &aimInfo2, &g_aimAction2), "Creating the aim input");

    // And one for the mod's own menu.
    XrActionCreateInfo vrMenuInfo = { XR_TYPE_ACTION_CREATE_INFO };
    vrMenuInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(vrMenuInfo.actionName, "vr_menu", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(vrMenuInfo.localizedActionName, "BunkerVR menu", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_vrMenuReady = g_gunButtonsReady &&
        Check(xrCreateAction(g_actionSet, &vrMenuInfo, &g_vrMenuAction), "Creating the BunkerVR menu input");

    // And the right stick's click. Named "inventory", it takes out or puts away the flashlight.
    XrActionCreateInfo inventoryInfo = { XR_TYPE_ACTION_CREATE_INFO };
    inventoryInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(inventoryInfo.actionName, "inventory", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(inventoryInfo.localizedActionName, "Inventory", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);

    g_inventoryReady = g_vrMenuReady &&
        Check(xrCreateAction(g_actionSet, &inventoryInfo, &g_inventoryAction), "Creating the inventory input");

    // And one output: the buzz in each controller.
    XrActionCreateInfo hapticInfo = { XR_TYPE_ACTION_CREATE_INFO };
    hapticInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    lstrcpynA(hapticInfo.actionName, "buzz", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(hapticInfo.localizedActionName, "Buzz", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    hapticInfo.countSubactionPaths = 2;
    hapticInfo.subactionPaths = hands;

    g_hapticReady = g_inventoryReady &&
        Check(xrCreateAction(g_actionSet, &hapticInfo, &g_hapticAction), "Creating the controller buzz");

    // And the controllers' finger sensors: is the thumb resting on anything
    // (stick, buttons, thumb rest), and is the first finger on the trigger?
    // Empty hands are posed from these.
    XrActionCreateInfo thumbTouchInfo = { XR_TYPE_ACTION_CREATE_INFO };
    thumbTouchInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(thumbTouchInfo.actionName, "thumb_touch", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(thumbTouchInfo.localizedActionName, "Thumb resting", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    thumbTouchInfo.countSubactionPaths = 2;
    thumbTouchInfo.subactionPaths = hands;

    XrActionCreateInfo triggerTouchInfo = { XR_TYPE_ACTION_CREATE_INFO };
    triggerTouchInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    lstrcpynA(triggerTouchInfo.actionName, "trigger_touch", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(triggerTouchInfo.localizedActionName, "Finger on trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    triggerTouchInfo.countSubactionPaths = 2;
    triggerTouchInfo.subactionPaths = hands;

    g_touchReady = g_hapticReady &&
        Check(xrCreateAction(g_actionSet, &thumbTouchInfo, &g_thumbTouchAction), "Creating the thumb sensor input") &&
        Check(xrCreateAction(g_actionSet, &triggerTouchInfo, &g_triggerTouchAction), "Creating the trigger sensor input");

    // And how hard the Valve Index's handle is squeezed (only its bindings use it).
    XrActionCreateInfo forceInfo = { XR_TYPE_ACTION_CREATE_INFO };
    forceInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    lstrcpynA(forceInfo.actionName, "grip_force", XR_MAX_ACTION_NAME_SIZE);
    lstrcpynA(forceInfo.localizedActionName, "Grab force (Valve Index)", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    forceInfo.countSubactionPaths = 2;
    forceInfo.subactionPaths = hands;
    bool forceMade = g_touchReady && Check(xrCreateAction(g_actionSet, &forceInfo, &g_gripForceAction), "Creating the grip force input");

    // Say which physical controls feed those inputs. First for Touch controllers...
    XrActionSuggestedBinding touchBindings[29] = {
        { g_gripAction, ToPath("/user/hand/left/input/grip/pose") },
        { g_gripAction, ToPath("/user/hand/right/input/grip/pose") },
        { g_aimAction, ToPath("/user/hand/left/input/aim/pose") },
        { g_aimAction, ToPath("/user/hand/right/input/aim/pose") },
        { g_squeezeAction, ToPath("/user/hand/left/input/squeeze/value") },
        { g_squeezeAction, ToPath("/user/hand/right/input/squeeze/value") },
        { g_triggerAction, ToPath("/user/hand/left/input/trigger/value") },
        { g_triggerAction, ToPath("/user/hand/right/input/trigger/value") },
        { g_moveAction, ToPath("/user/hand/left/input/thumbstick") },
        { g_turnAction, ToPath("/user/hand/right/input/thumbstick") },
        { g_crouchAction, ToPath("/user/hand/right/input/b/click") },
        { g_sprintAction, ToPath("/user/hand/left/input/thumbstick/click") },
        { g_menuAction, ToPath("/user/hand/left/input/menu/click") },
        { g_gunAction, ToPath("/user/hand/right/input/a/click") },
        { g_aimAction2, ToPath("/user/hand/left/input/x/click") },
        { g_vrMenuAction, ToPath("/user/hand/left/input/y/click") },
        { g_inventoryAction, ToPath("/user/hand/right/input/thumbstick/click") },
        { g_hapticAction, ToPath("/user/hand/left/output/haptic") },
        { g_hapticAction, ToPath("/user/hand/right/output/haptic") },
        { g_triggerTouchAction, ToPath("/user/hand/left/input/trigger/touch") },
        { g_triggerTouchAction, ToPath("/user/hand/right/input/trigger/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/left/input/thumbstick/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/right/input/thumbstick/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/left/input/x/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/left/input/y/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/right/input/a/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/right/input/b/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/left/input/thumbrest/touch") },
        { g_thumbTouchAction, ToPath("/user/hand/right/input/thumbrest/touch") },
    };
    bool anyAccepted = false;
    {
        XrInteractionProfileSuggestedBinding suggestion = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestion.interactionProfile = ToPath("/interaction_profiles/oculus/touch_controller");
        suggestion.suggestedBindings = touchBindings;

        // Offer everything first. If the headset software turns the list down
        // (one control it does not know spoils the whole list), offer shorter
        // lists that end before the newer controls.
        const uint32_t counts[12] = { 29, 27, 21, 19, 17, 16, 15, 13, 11, 10, 8, 4 };
        uint32_t most = g_touchReady ? 29 : g_hapticReady ? 19 : g_inventoryReady ? 17 : g_vrMenuReady ? 16 : g_gunButtonsReady ? 15 : (g_moreButtonsReady ? 13 : (g_crouchReady ? 11 : (g_sticksReady ? 10 : (g_grabButtonsReady ? 8 : 4))));
        for (int i = 0; i < 12 && !anyAccepted; ++i)
        {
            if (counts[i] > most) continue;
            suggestion.countSuggestedBindings = counts[i];
            if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(g_instance, &suggestion)))
            {
                anyAccepted = true;
                Log("VR: the headset software accepted %u of 29 controller bindings.", (unsigned)counts[i]);
                // 21 = with the trigger sensors only; 27 = all but the thumb rests.
                g_triggerTouchBound = counts[i] >= 21;
                g_thumbTouchBound = counts[i] >= 27;
                if (counts[i] < 19) g_hapticReady = false;
                if (counts[i] < 17) g_inventoryReady = false;
                if (counts[i] < 16) g_vrMenuReady = false;
            }
        }
    }
    // ...then for the Valve Index's controllers, the same layout by position. Each has
    // A and B, a thumbstick, a trackpad, the trigger and the squeezed handle (no
    // X, Y or menu button; its system button is SteamVR's): the left A and B do
    // the Touch's X and Y, a firm press on the left trackpad its menu button.
    // (Other controllers SteamVR knows are offered the Touch layout by it, or
    // rebound in its own bindings page.)
    if (g_touchReady)
    {
        XrActionSuggestedBinding indexBindings[] = {
            { g_gripAction, ToPath("/user/hand/left/input/grip/pose") },
            { g_gripAction, ToPath("/user/hand/right/input/grip/pose") },
            { g_aimAction, ToPath("/user/hand/left/input/aim/pose") },
            { g_aimAction, ToPath("/user/hand/right/input/aim/pose") },
            { g_squeezeAction, ToPath("/user/hand/left/input/squeeze/value") },
            { g_squeezeAction, ToPath("/user/hand/right/input/squeeze/value") },
            { g_triggerAction, ToPath("/user/hand/left/input/trigger/value") },
            { g_triggerAction, ToPath("/user/hand/right/input/trigger/value") },
            { g_moveAction, ToPath("/user/hand/left/input/thumbstick") },
            { g_turnAction, ToPath("/user/hand/right/input/thumbstick") },
            { g_crouchAction, ToPath("/user/hand/right/input/b/click") },
            { g_sprintAction, ToPath("/user/hand/left/input/thumbstick/click") },
            { g_menuAction, ToPath("/user/hand/left/input/trackpad/force") },
            { g_gunAction, ToPath("/user/hand/right/input/a/click") },
            { g_aimAction2, ToPath("/user/hand/left/input/a/click") },
            { g_vrMenuAction, ToPath("/user/hand/left/input/b/click") },
            { g_inventoryAction, ToPath("/user/hand/right/input/thumbstick/click") },
            { g_hapticAction, ToPath("/user/hand/left/output/haptic") },
            { g_hapticAction, ToPath("/user/hand/right/output/haptic") },
            { g_triggerTouchAction, ToPath("/user/hand/left/input/trigger/touch") },
            { g_triggerTouchAction, ToPath("/user/hand/right/input/trigger/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/left/input/thumbstick/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/right/input/thumbstick/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/left/input/a/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/left/input/b/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/right/input/a/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/right/input/b/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/left/input/trackpad/touch") },
            { g_thumbTouchAction, ToPath("/user/hand/right/input/trackpad/touch") },
            // The handle's force, for starting a grab. Must stay the last two: left off if the action couldn't be
            // created or the list is refused with them.
            { forceMade ? g_gripForceAction : g_squeezeAction, ToPath("/user/hand/left/input/squeeze/force") },
            { forceMade ? g_gripForceAction : g_squeezeAction, ToPath("/user/hand/right/input/squeeze/force") },
        };
        const uint32_t allIndex = (uint32_t)(sizeof(indexBindings) / sizeof(indexBindings[0]));
        XrInteractionProfileSuggestedBinding suggestion = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestion.interactionProfile = ToPath("/interaction_profiles/valve/index_controller");
        suggestion.suggestedBindings = indexBindings;
        suggestion.countSuggestedBindings = forceMade ? allIndex : allIndex - 2;
        XrResult indexResult = xrSuggestInteractionProfileBindings(g_instance, &suggestion);
        if (XR_FAILED(indexResult) && forceMade)
        {
            Log("VR: the headset software did not take the Valve Index controllers' bindings with the grip force (code %d); offered without it.", (int)indexResult);
            suggestion.countSuggestedBindings = allIndex - 2;
            indexResult = xrSuggestInteractionProfileBindings(g_instance, &suggestion);
        }
        g_gripForceBound = XR_SUCCEEDED(indexResult) && suggestion.countSuggestedBindings == allIndex;
        if (XR_SUCCEEDED(indexResult)) Log("VR: the headset software accepted the Valve Index controllers' %u bindings%s.", suggestion.countSuggestedBindings,
                                          g_gripForceBound ? " (a grab starts with a squeeze of the handle)" : "");
        else Log("VR: the headset software did not take the Valve Index controllers' bindings (code %d).", (int)indexResult);
    }
    // ...and, as a fallback, positions only for the plainest kind of controller.
    {
        XrInteractionProfileSuggestedBinding suggestion = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggestion.interactionProfile = ToPath("/interaction_profiles/khr/simple_controller");
        suggestion.countSuggestedBindings = 4;
        suggestion.suggestedBindings = touchBindings;
        if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(g_instance, &suggestion))) anyAccepted = true;
    }
    if (!anyAccepted)
    {
        Log("VR ERROR: the headset software did not accept our controller setup.");
        return false;
    }

    // A "space" for each hand lets us ask where it is, the same way as for the head.
    for (int hand = 0; hand < 2; ++hand)
    {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.subactionPath = hands[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;

        spaceInfo.action = g_gripAction;
        if (!Check(xrCreateActionSpace(g_session, &spaceInfo, &g_gripSpaces[hand]), "Creating a hand reference point"))
            return false;

        spaceInfo.action = g_aimAction;
        if (!Check(xrCreateActionSpace(g_session, &spaceInfo, &g_aimSpaces[hand]), "Creating a pointing reference point"))
            return false;
    }

    XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &g_actionSet;
    if (!Check(xrAttachSessionActionSets(g_session, &attachInfo), "Connecting the controller inputs"))
        return false;

    Log("VR: hand controllers set up.");
    return true;
}

// Writes into `out` (16 numbers, row after row) the placement of a box as seen
// from the eye.
//   eye, hand: where each is in the room
//   offset:    where the box's center is, measured from the hand's own origin
//   size:      the box's width, height and length in meters
//   scale:     the world scale (the player, hands included, is this many times bigger)
static void BoxSeenFromEye(const XrPosef& eye, const XrPosef& hand, const float offset[3], const float size[3],
                           float scale, float out[16])
{
    // Each rotation as a 3x3 grid.
    const XrQuaternionf& a = eye.orientation;
    float e[9] = {
        1 - 2 * (a.y * a.y + a.z * a.z), 2 * (a.x * a.y - a.w * a.z),     2 * (a.x * a.z + a.w * a.y),
        2 * (a.x * a.y + a.w * a.z),     1 - 2 * (a.x * a.x + a.z * a.z), 2 * (a.y * a.z - a.w * a.x),
        2 * (a.x * a.z - a.w * a.y),     2 * (a.y * a.z + a.w * a.x),     1 - 2 * (a.x * a.x + a.y * a.y),
    };
    const XrQuaternionf& b = hand.orientation;
    float h[9] = {
        1 - 2 * (b.y * b.y + b.z * b.z), 2 * (b.x * b.y - b.w * b.z),     2 * (b.x * b.z + b.w * b.y),
        2 * (b.x * b.y + b.w * b.z),     1 - 2 * (b.x * b.x + b.z * b.z), 2 * (b.y * b.z - b.w * b.x),
        2 * (b.x * b.z - b.w * b.y),     2 * (b.y * b.z + b.w * b.x),     1 - 2 * (b.x * b.x + b.y * b.y),
    };

    // The hand's rotation as seen from the eye: (eye rotation undone) x (hand rotation).
    float r[9];
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            r[row * 3 + column] = e[0 + row] * h[0 + column] + e[3 + row] * h[3 + column] + e[6 + row] * h[6 + column];

    // The hand's position as seen from the eye.
    float d[3] = { hand.position.x - eye.position.x, hand.position.y - eye.position.y, hand.position.z - eye.position.z };
    float t[3];
    for (int row = 0; row < 3; ++row)
        t[row] = e[0 + row] * d[0] + e[3 + row] * d[1] + e[6 + row] * d[2];

    // Put it together: size the box, move it to its offset within the hand,
    // turn and move it to where the hand is, then apply the world scale.
    for (int row = 0; row < 3; ++row)
    {
        float centre = r[row * 3 + 0] * offset[0] + r[row * 3 + 1] * offset[1] + r[row * 3 + 2] * offset[2] + t[row];
        out[row * 4 + 0] = scale * r[row * 3 + 0] * size[0];
        out[row * 4 + 1] = scale * r[row * 3 + 1] * size[1];
        out[row * 4 + 2] = scale * r[row * 3 + 2] * size[2];
        out[row * 4 + 3] = scale * centre;
    }
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
}

static bool LocateHand(XrSpace space, XrTime time, XrPosef& pose)
{
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(space, g_floorSpace, time, &location))) return false;
    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) return false;
    if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) return false;
    pose = location.pose;
    return true;
}

// The same, with each part's state: whether the position and the
// turn are known at all, and whether the position is really being tracked
// (not the headset's own guess). True if both are known, as LocateHand.
static bool LocateHandParts(XrSpace space, XrTime time, XrPosef& pose, bool& positionKnown, bool& turnKnown, bool& positionTracked)
{
    positionKnown = turnKnown = positionTracked = false;
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(space, g_floorSpace, time, &location))) return false;
    positionKnown = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    turnKnown = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    positionTracked = (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    pose = location.pose;
    return positionKnown && turnKnown;
}

// A hand whose controller loses tracking. The headset stops giving the
// controller's position; left alone, the hand's pose would go stale after
// 0.3 s, the game's arms would stop following it (VrGetGripHandPose, roll
// 7010) and the game's own flat-screen arm animation would show, the hands
// floating in front of the face. Instead the game is given the hand where it
// was last tracked (in the room, so it goes along with the character as the
// sticks walk and turn it, as a real hand would), turned as the controller
// still reports it if the headset still knows its orientation; what it holds
// stays held. Tracked again, it glides back to the controller in a quarter of
// a second instead of jumping. (It isn't faded: both hands, and both arms, are
// each one part of the game's body model, player_body_soldier_hand and _arms,
// so one hand alone can't be faded.)
// A position the headset only guesses (valid, not tracked: a Rift controller
// hidden from its sensors is guessed from its own movement, drifts, and jumps
// back when seen) counts as lost too, once that controller has been seen
// tracked (g_trackBitSeen: a headset that never reports "tracked" is
// believed). Such spells are short (25 to 110 ms) but make the hands and held
// items jitter or jump, such as the left hand jumping along the shotgun's
// barrel. The glide back is shorter after a short spell: twice the spell, 90
// to 250 ms.
struct LostHand
{
    bool held = false;             // lost, and held where it was
    double lostMs = 0.0;           // since when
    double glideStartMs = -1.0;    // tracked again: gliding back since then (below 0: not gliding)
    double glideMs = 250.0;        // ...over this long
    XrPosef aim = {}, grip = {};   // where it is held, or glides back from (in the room)
};
static LostHand g_lostHand[2];
static const double kLostHandGlideMs = 250.0;   // the longest glide back...
static const double kLostHandGlideLeastMs = 90.0; // ...and the shortest
static bool g_trackBitSeen[2] = { false, false };  // has the headset said this controller is tracked (not guessed)?

// Part of the way from one pose to another (t 0 to 1): the place in a straight
// line, the turn the shorter way around.
static XrPosef BlendPose(const XrPosef& from, const XrPosef& to, float t)
{
    XrPosef out;
    out.position.x = from.position.x + (to.position.x - from.position.x) * t;
    out.position.y = from.position.y + (to.position.y - from.position.y) * t;
    out.position.z = from.position.z + (to.position.z - from.position.z) * t;
    const XrQuaternionf& a = from.orientation;
    const XrQuaternionf& b = to.orientation;
    float side = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0.0f ? -1.0f : 1.0f;
    XrQuaternionf q;
    q.x = a.x + (side * b.x - a.x) * t;
    q.y = a.y + (side * b.y - a.y) * t;
    q.z = a.z + (side * b.z - a.z) * t;
    q.w = a.w + (side * b.w - a.w) * t;
    float length = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length < 0.0001f) q = b;
    else { q.x /= length; q.y /= length; q.z /= length; q.w /= length; }
    out.orientation = q;
    return out;
}

// The poses the game is given for one hand this frame (in the room): the
// controller's own while it is tracked; where it was last given while it is
// not (turned as the controller still reports it, if the headset knows that:
// liveTurn); and a short glide from there to the controller once it is
// tracked again. aim and grip come in as the controller's own (grip with the
// aim's turn, as the game's arms are given it). False: nothing to give (it
// has never been tracked).
static bool LostHandPoses(int hand, bool tracked, const XrQuaternionf* liveTurn, XrPosef& aim, XrPosef& grip)
{
    LostHand& lost = g_lostHand[hand];
    double now = NowMs();
    static int logsLeft = 30;
    if (!tracked)
    {
        if (!lost.held)
        {
            if (g_sharedHandTime[hand] == 0 || g_sharedGripTime[hand] == 0) return false; // (never tracked yet)
            lost.held = true;
            lost.lostMs = now;
            lost.glideStartMs = -1.0;
            lost.aim = g_sharedHandRoom[hand];
            lost.grip = g_sharedGripRoom[hand];
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: the %s controller has lost tracking (its place not given, or only guessed by the headset; its turn %s): its hand is held where it was.",
                    hand == 0 ? "left" : "right", liveTurn ? "still known" : "not known either");
            }
        }
        aim = lost.aim;
        grip = lost.grip;
        if (liveTurn)
        {
            aim.orientation = *liveTurn;
            grip.orientation = *liveTurn;
        }
        return true;
    }
    if (lost.held)
    {
        lost.held = false;
        lost.glideStartMs = now;
        double spell = now - lost.lostMs;
        lost.glideMs = spell * 2.0;
        if (lost.glideMs < kLostHandGlideLeastMs) lost.glideMs = kLostHandGlideLeastMs;
        if (lost.glideMs > kLostHandGlideMs) lost.glideMs = kLostHandGlideMs;
        lost.aim = g_sharedHandRoom[hand]; // (where it was last given, turned as it was)
        lost.grip = g_sharedGripRoom[hand];
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: the %s controller is tracked again after %.2f s: its hand glides back in %.0f ms.", hand == 0 ? "left" : "right",
                spell / 1000.0, lost.glideMs);
        }
    }
    if (lost.glideStartMs >= 0.0)
    {
        double t = (now - lost.glideStartMs) / lost.glideMs;
        if (t >= 1.0)
        {
            lost.glideStartMs = -1.0;
        }
        else
        {
            float s = (float)(t * t * (3.0 - 2.0 * t)); // (eased in and out)
            aim = BlendPose(lost.aim, aim, s);
            grip = BlendPose(lost.grip, grip, s);
        }
    }
    return true;
}

// When the last buzz sent to each controller ends (NowMs), for the hit's
// heartbeat (UpdateHurtFeel): a thump does not cut a longer buzz short.
static double g_buzzEndsMs[2] = { -1.0, -1.0 };

// A short buzz in one controller (0 = left, 1 = right). `strength` runs from 0
// to 1.
static void Buzz(int hand, float seconds, float strength)
{
    if (!g_hapticReady || g_session == XR_NULL_HANDLE || hand < 0 || hand > 1) return;

    // The "Vibration" setting: weaker, stronger or none at all.
    strength *= GetSettings().buzzScale;
    if (strength <= 0.01f) return;
    if (strength > 1.0f) strength = 1.0f;

    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
    vibration.duration = (XrDuration)(seconds * 1000000000.0f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = strength;

    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = g_hapticAction;
    info.subactionPath = g_handPaths[hand];
    xrApplyHapticFeedback(g_session, &info, (const XrHapticBaseHeader*)&vibration);
    g_buzzEndsMs[hand] = NowMs() + seconds * 1000.0;
}

// Moves a recenter along (see VrRecenter); once per headset frame, before
// the head is published. Once the eyes are black the view is centered (by
// PublishHeadPose, with this frame's head) and both controllers buzz.
static void UpdateRecentre()
{
    if (g_recentreStartMs < 0.0) return;
    double t = NowMs() - g_recentreStartMs;
    if (!g_recentreDone && t >= kRecentreFadeOutMs)
    {
        g_recentreDone = true;
        g_recenterRequested = true;
        g_panelSnapRequested = true;
        Buzz(0, 0.05f, 0.5f);
        Buzz(1, 0.05f, 0.5f);
    }
    if (t >= kRecentreFadeOutMs + kRecentreBlackMs + kRecentreFadeInMs) g_recentreStartMs = -1.0;
}

// ---------------------------------------------------------------------------
// The main menu's room. The game's menu camera stands 94 cm above that room's
// floor, wherever the real head is, so the player would be sunk into it up to
// the chest. While the menu script says so (cVR_MenuRoom, every frame, in the
// main_menu map):
// - the room's floor is the player's real floor: "straight ahead" is put at
//   the height that makes it so, each frame, however the head moves;
// - beyond kMenuRoomFar from the middle, the view is centered where the
//   player is, through the recenter's short black, keeping its facing, so they
//   can't walk into the scenery and see through it;
// - the menu stands still in the room, in front of its middle (PlaceMenuInRoom,
//   which never follows the head), kMenuRoomLift above the eyes.
// Leaving the menu, the view is centered afresh for the level.
// ---------------------------------------------------------------------------
static const double kMenuRoomStaleMs = 250.0;
static const float kMenuRoomFar = 0.60f;  // meters from the middle
static const float kMenuRoomLift = 0.10f; // the menu's middle above the eyes as they were when centered, meters
// Seated, with no seated eye height saved: how far the floor is taken to be
// below the eyes as they were when centered. The play area's floor can't be
// used: it can be far below the real floor (on a Rift, 157 to 158 cm below a
// seated head), leaving the hands unable to reach it. The Comfort page's
// Height setting still moves it.
static const float kMenuRoomSeatedEyes = 1.20f;
static bool StartupRunning(); // the start-up page: below
static double g_menuRoomMs = -1000000.0;
static float g_menuRoomAbove = -1.0f;     // how far the game's camera stands above the floor, in the game's units (below 0: not known)
static float g_menuRoomEyes = 0.0f;       // the head's height when the view was last centered here
static unsigned g_menuRoomRecentres = 0;

void VrSetMenuRoom(float cameraAboveFloor)
{
    g_menuRoomAbove = cameraAboveFloor;
    g_menuRoomMs = NowMs();
}

static bool MenuRoomOn()
{
    return g_haveReference && NowMs() - g_menuRoomMs < kMenuRoomStaleMs;
}

// Once per headset frame, before the head is published.
static void UpdateMenuRoom(const XrPosef& head)
{
    static bool wasOn = false;
    if (!MenuRoomOn())
    {
        if (wasOn)
        {
            wasOn = false;
            g_recenterRequested = true; // (the height was the room's: the level starts from the head's own)
            Log("VR: the main menu's room is left: the view is centred afresh.");
        }
        return;
    }
    if (!wasOn || g_menuRoomRecentres != g_recentreCount)
    {
        g_menuRoomEyes = head.position.y;
        g_menuRoomRecentres = g_recentreCount;
        g_panelSnapRequested = true;
    }
    float floorY = 0.0f, scale = GetSettings().worldScale;
    bool seated = GetSettings().seated;
    bool floorKnown = FloorY(floorY);
    if (seated)
    {
        // A floor a controller rested on (the start-up page); else taken
        // from the head as the view was centered here.
        bool real = false;
        float restedOn = 0.0f;
        if (FloorY(restedOn, &real) && real) floorY = restedOn;
        else floorY = g_menuRoomEyes - (GetSettings().seatedEyes > 0.0f ? GetSettings().seatedEyes : kMenuRoomSeatedEyes); // seatedEyes: measured with a floor touch on the Body page
        floorKnown = true;
    }
    if (!wasOn)
    {
        wasOn = true;
        Log("VR: the main menu's room: the game's camera %.0f cm above its floor; your floor %s; the eyes %.0f cm above it.", g_menuRoomAbove * 100.0f,
            seated ? "taken from your seated eyes" : (floorKnown ? "known" : "not known, so the room's floor is not moved"), floorKnown ? (head.position.y - floorY) * 100.0f : 0.0f);
    }
    if (g_menuRoomAbove > 0.0f && scale > 0.01f && floorKnown)
        g_referencePosition[1] = floorY + g_menuRoomAbove / scale + GetSettings().heightOffset;

    // Not while the start-up page is up: reaching down to the floor or standing
    // up for it isn't walking away.
    float dx = head.position.x - g_referencePosition[0], dz = head.position.z - g_referencePosition[2];
    if (g_recentreStartMs < 0.0 && !StartupRunning() && sqrtf(dx * dx + dz * dz) > kMenuRoomFar)
    {
        g_recentreYaw = g_referenceYaw;
        g_recentreYawGiven = true;
        VrRecenter();
        static int logsLeft = 10;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: %.0f cm from the middle of the main menu's room: brought back to it.", sqrtf(dx * dx + dz * dz) * 100.0f);
        }
    }
}

// At the start the view is centered on the very first frame, wherever the
// headset is (it may still be lying on a desk, before the headset shows a
// picture). If, before the game's first level, the headset is then moved more
// than 30 cm from there and held still (within 3 cm) for a second, it is
// centered once more where it is then: it has been put on.
static void CheckPickedUp()
{
    static bool done = false, have = false;
    static XrVector3f first = {}, still = {};
    static double stillSinceMs = 0.0;
    if (done || !g_framePoseValid || !g_haveReference) return;
    if (CameraBodyNewsSeen() || g_recentreCount > 1 || StartupRunning())
    {
        done = true; // a level has begun, or the view was centered by hand (or the start-up page is up: standing up for it isn't putting the headset on)
        return;
    }
    const XrVector3f& p = g_frameHeadPose.position;
    if (!have)
    {
        first = still = p;
        stillSinceMs = NowMs();
        have = true;
        return;
    }
    float sx = p.x - still.x, sy = p.y - still.y, sz = p.z - still.z;
    if (sqrtf(sx * sx + sy * sy + sz * sz) > 0.03f)
    {
        still = p;
        stillSinceMs = NowMs();
    }
    float fx = p.x - first.x, fy = p.y - first.y, fz = p.z - first.z;
    if (sqrtf(fx * fx + fy * fy + fz * fz) > 0.30f && NowMs() - stillSinceMs > 1000.0)
    {
        done = true;
        Log("VR: the headset was picked up and put on after the start: the view is centred again where it now is.");
        VrRecenter();
    }
}

// ---------------------------------------------------------------------------
// "Measure my size": a world scale that makes the player's real body the
// soldier's size. The soldier's eyes are 1.838 m above his feet
// (player_bunker.ent, its CameraSocket), and the view hangs there (the body
// script keeps it there however big the body is drawn). With the world scale
// at 1.838 over the player's own eye height, the player's real body is his
// size: the real floor is his floor, things are where the hands find them,
// and a door is as tall to the player as to him. So the button on the Comfort
// page measures the eye height above the floor while the player stands up
// straight and pulls a trigger, sets the world scale from it, and keeps both
// (Settings::eyeHeight). The eyes' height is taken from the neck
// (NeckHeight), so a nod while standing still doesn't change it; a seated
// head isn't taken (below kMeasureLowest: it waits for the player to stand).
// The floor: the play area's (UpdateStageFloor), or a controller touched to
// it. Two short buzzes: measured. One long one: not, and why in the menu.
// ---------------------------------------------------------------------------

static const float kGameEyeHeight = 1.838f;                  // the soldier's eyes above his feet
static const float kMeasureLowest = 1.35f, kMeasureHighest = 2.10f; // standing eye heights believed
static const double kMeasureGiveUpMs = 60000.0;              // long: it waits for a trigger pull
static const float kMeasureSteady = 0.02f;                   // within this of where the stretch began (meters)
static const float kMeasureLevel = 0.42f;                    // looking within 25 degrees of level (its sine)
static const float kTouchBelowEyes = 1.15f;                  // a controller this far below the standing eyes touched the floor
static const float kGripAboveFloor = 0.05f;                  // the controller's grip point with the controller on the floor (about)

// Each measurement is taken when a trigger is pulled, not by waiting for the
// player to hold still. A new pull, not one held from before, and not one that
// presses something on the mod's menu (UpdateTakePulls, once a headset frame);
// true for that one frame.
static bool g_takePulled[2] = { false, false };
static const int kTakeSteadyFrames = 10; // the head held level and steady this many frames up to the pull (about 0.1 s)

// A pull that took nothing: why, under the page, and a short low buzz.
static void TakeRefused(const char* text)
{
    VrMenuSetComfortInfo(text);
    Buzz(0, 0.25f, 0.5f);
    Buzz(1, 0.25f, 0.5f);
    static int logsLeft = 20;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: a trigger pulled to measure, not taken: %s", text);
    }
}

static double g_measureStartMs = -1.0;  // below 0: not measuring
static double g_measureStretchMs = 0.0; // when the present steady stretch began
static float g_measureStretchFirst = 0.0f;
static double g_measureStretchSum = 0.0;
static int g_measureStretchCount = 0;
static float g_measureLowestGrip = 1000.0f; // the lowest a tracked controller has been while measuring (room meters)
static const char* g_measureWaiting = "";   // what it is waiting for, if it has to give up
static double g_measureBuzzAtMs = -1.0;     // the second of the two short buzzes, when due

static void StartMeasure(bool quiet = false) // (quiet: see StartBodyStep)
{
    g_measureStartMs = NowMs();
    g_measureStretchCount = 0;
    g_measureLowestGrip = 1000.0f;
    g_measureWaiting = "a trigger pulled, head level";
    float floorY = 0.0f;
    bool floorKnown = FloorY(floorY);
    if (!quiet || !floorKnown)
        VrMenuSetComfortInfo(floorKnown ? "Stand up straight, look straight ahead, then pull either trigger."
                                        : "Touch the floor with a controller, then stand up straight.");
    Log("VR: measuring your size (%s).", g_stageFloorKnown ? "the floor from the play area" : (floorKnown ? "the floor where it was touched" : "no floor known yet: touch it"));
}

// A tracked controller's height this frame (DrawHands).
static void MeasureNoteHand(float y)
{
    if (g_measureStartMs >= 0.0 && y < g_measureLowestGrip) g_measureLowestGrip = y;
}

static bool g_measureLastDone = false; // the start-up page follows it

static void EndMeasure(bool measured, const char* menuText)
{
    g_measureStartMs = -1.0;
    g_measureLastDone = measured;
    VrMenuSetComfortInfo(menuText);
    if (measured)
    {
        Buzz(0, 0.08f, 0.6f);
        Buzz(1, 0.08f, 0.6f);
        g_measureBuzzAtMs = NowMs() + 200.0;
    }
    else
    {
        Buzz(0, 0.5f, 0.6f);
        Buzz(1, 0.5f, 0.6f);
    }
}

// Once per headset frame, with the head.
static void UpdateMeasure(const XrPosef& head)
{
    double now = NowMs();
    if (g_measureBuzzAtMs >= 0.0 && now >= g_measureBuzzAtMs)
    {
        g_measureBuzzAtMs = -1.0;
        Buzz(0, 0.08f, 0.6f);
        Buzz(1, 0.08f, 0.6f);
    }
    if (g_measureStartMs < 0.0) return;

    // The steady stretch: level eyes held within kMeasureSteady of where it began.
    float eyes = NeckHeight(head) + 0.075f;
    const XrQuaternionf& q = head.orientation;
    bool level = fabsf(2 * (q.y * q.z - q.w * q.x)) < kMeasureLevel;
    if (!level || g_measureStretchCount == 0 || fabsf(eyes - g_measureStretchFirst) > kMeasureSteady)
    {
        g_measureStretchMs = now;
        g_measureStretchFirst = eyes;
        g_measureStretchSum = 0.0;
        g_measureStretchCount = 0;
    }
    if (level)
    {
        g_measureStretchSum += eyes;
        ++g_measureStretchCount;
    }

    // Taken when a trigger is pulled, from the level eyes held steady up to
    // then.
    bool pulled = g_takePulled[0] || g_takePulled[1];
    if (pulled && (!level || g_measureStretchCount < kTakeSteadyFrames))
        TakeRefused("Hold your head level and still, then pull the trigger again.");
    else if (pulled)
    {
        float eyeY = (float)(g_measureStretchSum / g_measureStretchCount);
        double heldMs = now - g_measureStretchMs;
        int frames = g_measureStretchCount;
        g_measureStretchCount = 0; // (if it goes on waiting, a new stretch)

        // The floor: touched, if a controller went far enough below the eyes
        // (then forgotten for the next stretch, so that it is noted once).
        if (g_measureLowestGrip < eyeY - kTouchBelowEyes)
        {
            g_touchFloorKnown = true;
            g_touchFloorY = g_measureLowestGrip - kGripAboveFloor;
            g_measureLowestGrip = 1000.0f;
            if (g_stageFloorKnown)
                Log("VR: measuring: the floor you touched is %+.1f cm from the play area's (a check: the play area's is used).",
                    (g_touchFloorY - g_stageFloorY) * 100.0f);
        }
        float floorY = 0.0f;
        if (!FloorY(floorY))
        {
            g_measureWaiting = "touch the floor with a controller";
            TakeRefused("Touch the floor with a controller, then stand up straight.");
        }
        else
        {
            float eyeHeight = eyeY - floorY;
            if (eyeHeight < kMeasureLowest)
            {
                // Sitting, or bent: it waits for the player to stand up.
                g_measureWaiting = "stand up straight";
                TakeRefused("Stand up straight, then pull the trigger again.");
                Log("VR: measuring: your eyes %.1f cm above the floor: too low for standing.", eyeHeight * 100.0f);
            }
            else if (eyeHeight > kMeasureHighest)
            {
                Log("VR: measuring: your eyes came out %.1f cm above the floor, more than can be right; nothing changed.", eyeHeight * 100.0f);
                EndMeasure(false, "Calibration failed: the floor height looks wrong. Calibrate the floor, then try again.");
                return;
            }
            else
            {
                Settings& s = GetSettings();
                float was = s.worldScale;
                s.eyeHeight = eyeHeight;
                s.bodyHeight = 0.0f; // measured, not set by hand
                SetWorldScaleCalibrated(kGameEyeHeight / eyeHeight); // (0.88 to 1.36; the player's adjustment kept)
                bool heightWas = !s.seated && s.heightOffset != 0.0f;
                if (!s.seated) s.heightOffset = 0.0f; // standing, the floor sets the height
                SaveSettings();
                Log("VR: measured your size: your eyes are %.1f cm above the floor (%s; held steady %.1f s, %d frames). "
                    "World scale %.3f (was %.3f): the world as big to you as to the soldier, whose eyes are %.1f cm up.%s",
                    eyeHeight * 100.0f, g_stageFloorKnown ? (s.floorSaved ? "the floor a controller rested on, kept" : "the floor from the play area") : "the floor where you touched it",
                    heldMs / 1000.0, frames, s.worldScale, was, kGameEyeHeight * 100.0f,
                    heightWas ? " The Height setting is back to 0 (playing standing)." : "");
                if (!s.seated) SavedHeightFromFloor(head, "just measured");
                char text[96];
                snprintf(text, sizeof(text), "Height calibrated. Eye height: %.0f cm. World scale: %.0f%% (was %.0f%%).", eyeHeight * 100.0f,
                         s.worldScale * 100.0f, was * 100.0f);
                EndMeasure(true, text);
                return;
            }
        }
    }

    if (!StartupRunning() && now - g_measureStartMs > kMeasureGiveUpMs) // the start-up page waits: it has its own way on
    {
        Log("VR: measuring your size gave up after %.0f s (waiting: %s); nothing changed.", kMeasureGiveUpMs / 1000.0, g_measureWaiting);
        EndMeasure(false, "Calibration timed out. Nothing was changed.");
    }
}

// The headset's own "Reset view" moved the room (see the events): a floor
// touched in the old one, and a measuring under way, start again.
static void MeasureRoomMoved()
{
    g_touchFloorKnown = false;
    g_measureStretchCount = 0;
    g_measureLowestGrip = 1000.0f;
}

// ---------------------------------------------------------------------------
// The Body page: body measurements and the places of the body slots. Nothing
// here acts until its button is pressed, and "Back to how they were" undoes it
// all.
//   Touch the floor: seated, a hand resting on the floor tells how far the eyes
//     are above it (the play area's floor can't be trusted: on a Rift it can be
//     157 to 158 cm below a seated head). The main menu's room stands on it.
//   Measure my arms: both arms straight out. Only kept and logged so far.
//   Holster, pouch, flashlight: where that hand is.
// Each is taken when the hand's trigger is pulled there, not by waiting for the
// hand to hold still (a hand at the hip, seated, can keep losing tracking). A
// pull that clicks the menu is not taken (g_takePulled).
// ---------------------------------------------------------------------------

enum { kBodyNone, kBodyFloor, kBodyArms, kBodyHolster, kBodyPouch, kBodyLight, kBodySlot }; // kBodySlot: the right chest's item slot
static const double kBodyGiveUpMs = 60000.0;  // long: it waits for a trigger pull
static const float kBodyFloorBelowEyes = 0.45f; // a hand at least this far below the eyes may be on the floor
static const float kBodyShoulders = 0.19f;    // half the width of a person's shoulders

static int g_bodyStep = kBodyNone;
static double g_bodyStartMs = 0.0;
static float g_bodyEyesAtStart = 0.0f;
static bool g_gripTrackedNow[2] = { false, false }; // (set with the hands each headset frame: for the log)
static bool g_bodyHaveBefore = false;
static Settings g_bodyBefore;
static double g_bodyBuzzAtMs = -1.0;

static bool g_bodyLastDone = false; // did the last step end with something kept? (the start-up page follows it)

// What "Back to how they were" goes back to: the settings as they were before
// the first change made here or on the start-up page.
static void BodyKeepBefore()
{
    if (g_bodyHaveBefore) return;
    g_bodyHaveBefore = true;
    g_bodyBefore = GetSettings();
}

// (`quiet`: the start-up page says what to do itself, and the line below keeps
// what the step before it found.)
static void StartBodyStep(int step, bool quiet = false)
{
    BodyKeepBefore();
    g_bodyStep = step;
    g_bodyStartMs = NowMs();
    g_bodyEyesAtStart = 0.0f;
    const char* text = step == kBodyFloor ? "Sit as you play, rest a controller on the floor, then pull its trigger."
                     : step == kBodyArms ? "Hold both arms straight out to your sides, then pull either trigger."
                     : step == kBodyHolster ? "Place your right hand where you want the holster, then pull its trigger."
                     : step == kBodyPouch ? "Place your left hand where you want the ammo pouch, then pull its trigger."
                     : step == kBodySlot ? "Place your right hand where you want the chest item slot, then pull its trigger."
                     : "Place your left hand where you want the flashlight, then pull its trigger.";
    if (!quiet) VrMenuSetComfortInfo(text);
    Log("VR: the Body page: %s", text);
}

static void EndBodyStep(bool done, const char* text)
{
    g_bodyStep = kBodyNone;
    g_bodyLastDone = done;
    VrMenuSetComfortInfo(text);
    if (done)
    {
        SaveSettings();
        Buzz(0, 0.08f, 0.6f);
        Buzz(1, 0.08f, 0.6f);
        g_bodyBuzzAtMs = NowMs() + 200.0;
    }
    else
    {
        Buzz(0, 0.5f, 0.6f);
        Buzz(1, 0.5f, 0.6f);
    }
    Log("VR: the Body page: %s", text);
}

static void UndoBody()
{
    g_bodyStep = kBodyNone;
    if (!g_bodyHaveBefore)
    {
        VrMenuSetComfortInfo("There are no calibration changes to undo.");
        return;
    }
    Settings& s = GetSettings();
    s.seatedEyes = g_bodyBefore.seatedEyes;
    s.seatedEyesGuessed = g_bodyBefore.seatedEyesGuessed;
    s.floorSaved = g_bodyBefore.floorSaved;
    s.floorFromPlayArea = g_bodyBefore.floorFromPlayArea;
    s.floorGuessed = g_bodyBefore.floorGuessed;
    s.eyeHeight = g_bodyBefore.eyeHeight;
    s.bodyHeight = g_bodyBefore.bodyHeight;
    SetWorldScaleCalibrated(g_bodyBefore.worldScaleCalibrated);
    s.armLength = g_bodyBefore.armLength;
    s.holsterRight = g_bodyBefore.holsterRight;
    s.holsterDown = g_bodyBefore.holsterDown;
    s.holsterForward = g_bodyBefore.holsterForward;
    s.pouchSet = g_bodyBefore.pouchSet;
    s.pouchRight = g_bodyBefore.pouchRight;
    s.pouchDown = g_bodyBefore.pouchDown;
    s.pouchForward = g_bodyBefore.pouchForward;
    s.lightLeft = g_bodyBefore.lightLeft;
    s.lightDown = g_bodyBefore.lightDown;
    s.lightForward = g_bodyBefore.lightForward;
    s.slotRight = g_bodyBefore.slotRight;
    s.slotDown = g_bodyBefore.slotDown;
    s.slotForward = g_bodyBefore.slotForward;
    EndBodyStep(true, "Calibration restored to how it was when the game started.");
}

static bool MenuFacingYaw(float& yaw); // the way the mod's menu faces the player: below, with the menu

// Once per headset frame, with the head and the hands.
static void UpdateBodyPage(const XrPosef& head, const XrPosef* grip, const bool* haveGrip)
{
    BodyKeepBefore(); // "Back to how they were": as when the game started, before any change, including
                      // changes made with the page's numbers
    double now = NowMs();
    if (g_bodyBuzzAtMs >= 0.0 && now >= g_bodyBuzzAtMs)
    {
        g_bodyBuzzAtMs = -1.0;
        Buzz(0, 0.08f, 0.6f);
        Buzz(1, 0.08f, 0.6f);
    }
    if (g_bodyStep == kBodyNone) return;
    if (!StartupRunning() && now - g_bodyStartMs > kBodyGiveUpMs) // the start-up page waits: it has its own way on
    {
        EndBodyStep(false, "Calibration timed out. Nothing was changed.");
        return;
    }

    Settings& s = GetSettings();
    float eyes = NeckHeight(head) + 0.075f;
    if (g_bodyEyesAtStart == 0.0f) g_bodyEyesAtStart = eyes;

    char text[96];
    if (g_bodyStep == kBodyFloor)
    {
        for (int hand = 0; hand < 2; ++hand)
        {
            if (!g_takePulled[hand]) continue;
            if (!haveGrip[hand])
            {
                TakeRefused("That controller is not tracked. Keep it in view, then pull again.");
                continue;
            }
            const XrVector3f& at = grip[hand].position;
            float floorY = at.y - kGripAboveFloor;
            float above = g_bodyEyesAtStart - floorY;
            if (at.y > g_bodyEyesAtStart - kBodyFloorBelowEyes || above < 0.7f || above > 2.1f)
            {
                TakeRefused("The controller is not on the floor. Rest it on the floor, then pull again.");
                continue;
            }
            float stageWas = 0.0f;
            bool stageKnown = g_stageFloorKnown;
            if (stageKnown) stageWas = g_bodyEyesAtStart - g_stageFloorY;
            if (s.seated)
            {
                s.seatedEyes = above;
                s.seatedEyesGuessed = false;
            }
            Log("VR: the Body page: the floor touched, your eyes %.1f cm above it (%s; the controller %s); the play area's floor would make it %s.", above * 100.0f,
                s.seated ? "seated: your seated eyes from now on" : "standing", g_gripTrackedNow[hand] ? "tracked" : "not tracked, where it was last seen",
                stageKnown ? "as below" : "unknown");
            if (stageKnown) Log("VR: the Body page: the play area's floor is %.1f cm below those eyes.", stageWas * 100.0f);
            SaveFloor(floorY, "a controller rested on it"); // kept: the main menu's room and the saved height stand on it
            snprintf(text, sizeof(text), "Floor calibrated. Eye height: %.0f cm.", above * 100.0f);
            EndBodyStep(true, text);
            return;
        }
    }
    else if (g_bodyStep == kBodyArms)
    {
        if (!g_takePulled[0] && !g_takePulled[1]) return;
        if (!haveGrip[0] || !haveGrip[1])
        {
            TakeRefused("A controller is not tracked. Keep both in view, then pull again.");
            return;
        }
        const XrVector3f& a = grip[0].position;
        const XrVector3f& b = grip[1].position;
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        float span = sqrtf(dx * dx + dy * dy + dz * dz);
        bool out = span > 0.9f && a.y > eyes - 0.6f && b.y > eyes - 0.6f;
        if (!out)
        {
            TakeRefused("Hold both arms straight out to your sides, then pull again.");
            return;
        }
        s.armLength = span * 0.5f - kBodyShoulders;
        Log("VR: the Body page: hand to hand %.1f cm: your arms about %.1f cm from the shoulder. Kept; nothing uses it as yet.", span * 100.0f, s.armLength * 100.0f);
        snprintf(text, sizeof(text), "Arm length measured: about %.0f cm.", s.armLength * 100.0f);
        EndBodyStep(true, text);
        return;
    }
    else
    {
        int hand = g_bodyStep == kBodyHolster || g_bodyStep == kBodySlot ? 1 : 0;
        if (!g_takePulled[hand]) return;
        if (!haveGrip[hand])
        {
            TakeRefused("That controller is not tracked. Keep it in view, then pull again.");
            return;
        }
        const XrVector3f& at = grip[hand].position;
        // Measured along the way the player faces: the menu's (the player faces
        // it, while the head may be turned to look at the hand, which would put
        // the spot far forward), else the head's, kept level.
        float rx = 0.0f, rz = 0.0f, yaw = 0.0f;
        if (MenuFacingYaw(yaw))
        {
            rx = cosf(yaw);
            rz = -sinf(yaw);
        }
        else
        {
            const XrQuaternionf& q = head.orientation;
            rx = 1 - 2 * (q.y * q.y + q.z * q.z);
            rz = 2 * (q.x * q.z - q.w * q.y); // the head's "right"
            float length = sqrtf(rx * rx + rz * rz);
            if (length < 0.25f) return;
            rx /= length;
            rz /= length;
        }
        float fx = rz, fz = -rx; // level "forward"
        XrVector3f neck = NeckPoint(head);
        float dx = at.x - neck.x, dz = at.z - neck.z;
        float right = dx * rx + dz * rz;
        float forward = dx * fx + dz * fz;
        if (sqrtf(right * right + forward * forward) > 0.8f || at.y > neck.y) // (out of the way: not on the body)
        {
            TakeRefused("Your hand is too far from your body. Move it closer, then pull again.");
            return;
        }
        Log("VR: the Body page: a trigger pulled at %.0f cm right, %.0f cm forwards, %.0f cm below the neck (the controller %s; measured along %s).",
            right * 100.0f, forward * 100.0f, (neck.y - at.y) * 100.0f, g_gripTrackedNow[hand] ? "tracked" : "not tracked, where it was last seen",
            MenuFacingYaw(yaw) ? "the way the menu faces you" : "the head's way");
        if (g_bodyStep == kBodyHolster)
        {
            s.holsterRight = right;
            s.holsterForward = forward;
            s.holsterDown = neck.y + 0.075f - at.y;
            snprintf(text, sizeof(text), "Holster set: %.0f cm right, %.0f cm below eye level.", right * 100.0f, s.holsterDown * 100.0f);
        }
        else if (g_bodyStep == kBodyPouch)
        {
            s.pouchSet = true;
            s.pouchRight = right;
            s.pouchForward = forward;
            s.pouchDown = neck.y + 0.075f - at.y;
            snprintf(text, sizeof(text), "Ammo pouch set: %.0f cm left, %.0f cm below eye level.", -right * 100.0f, s.pouchDown * 100.0f);
        }
        else if (g_bodyStep == kBodySlot)
        {
            s.slotRight = right;
            s.slotForward = forward;
            s.slotDown = neck.y - at.y;
            snprintf(text, sizeof(text), "Chest item slot set: %.0f cm right, %.0f cm below the neck.", right * 100.0f, s.slotDown * 100.0f);
        }
        else
        {
            s.lightLeft = -right;
            s.lightForward = forward;
            s.lightDown = neck.y - at.y;
            snprintf(text, sizeof(text), "Flashlight set: %.0f cm left, %.0f cm below the neck.", -right * 100.0f, s.lightDown * 100.0f);
        }
        EndBodyStep(true, text);
    }
}

static bool HurtHoldsBuzz(int hand, float seconds, float strength); // (the hit indicator's, below)

void VrScriptBuzz(int hand, float seconds, float strength)
{
    if (hand == 2) hand = g_grabHand; // "the hand that holds"
    if (seconds < 0.005f) seconds = 0.005f;
    if (seconds > 1.0f) seconds = 1.0f;
    if (HurtHoldsBuzz(hand, seconds, strength)) return;
    Buzz(hand, seconds, strength);
}

// ---------------------------------------------------------------------------
// The mod's own presses and clicks
// ---------------------------------------------------------------------------
// Everything the mod sends through Windows carries this mark, so that the
// watcher of the PC's keyboard and mouse (PcInputThread) can tell it from the
// PC's own and from what other programs send.
static const ULONG_PTR kModInputMark = kBunkerVRInputMark; // "BVMR" (watch_shared.h: the watcher's too)

// What the mod holds down in Windows right now: its mouse buttons (1 left,
// 2 right, 4 middle) and keys (by scan code, extended ones from 256). A game
// that ends with one held leaves it held for every program (a right button
// left held stops every click on the PC from working). Released at the end:
// VrLetGoAtExit.
static volatile LONG g_modMouseHeld = 0;
static volatile bool g_modKeyHeld[512] = {};
// The same, shared with the mod's watcher (watch.cpp): it lets go
// of all of it if the game crashes, is stopped from outside or hangs. Null
// until the watcher is started (StartWatcher).
static WatchShared* g_watchShared = nullptr;

static UINT SendModInput(INPUT& input)
{
    if (input.type == INPUT_MOUSE)
    {
        input.mi.dwExtraInfo = kModInputMark;
        DWORD flags = input.mi.dwFlags;
        LONG held = g_modMouseHeld;
        if (flags & MOUSEEVENTF_LEFTDOWN) held |= 1;
        if (flags & MOUSEEVENTF_LEFTUP) held &= ~1;
        if (flags & MOUSEEVENTF_RIGHTDOWN) held |= 2;
        if (flags & MOUSEEVENTF_RIGHTUP) held &= ~2;
        if (flags & MOUSEEVENTF_MIDDLEDOWN) held |= 4;
        if (flags & MOUSEEVENTF_MIDDLEUP) held &= ~4;
        g_modMouseHeld = held;
        if (g_watchShared) InterlockedExchange(&g_watchShared->mouseHeld, held);
    }
    else if (input.type == INPUT_KEYBOARD)
    {
        input.ki.dwExtraInfo = kModInputMark;
        if (input.ki.dwFlags & KEYEVENTF_SCANCODE)
        {
            int index = (input.ki.wScan & 0xFF) + ((input.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) ? 256 : 0);
            bool down = (input.ki.dwFlags & KEYEVENTF_KEYUP) == 0;
            g_modKeyHeld[index] = down;
            if (g_watchShared)
            {
                LONG bit = (LONG)(1u << (index & 31));
                if (down) InterlockedOr(&g_watchShared->keyHeld[index >> 5], bit);
                else InterlockedAnd(&g_watchShared->keyHeld[index >> 5], ~bit);
            }
        }
    }
    return SendInput(1, &input, sizeof(INPUT));
}

// The game closing (DllMain, the process ending normally): every button and
// key the mod still holds down is let go (see g_modMouseHeld). Only SendInput:
// nothing that waits or writes the log (Windows holds its loader lock then).
void VrLetGoAtExit()
{
    INPUT inputs[32] = {};
    UINT count = 0;
    LONG held = g_modMouseHeld;
    const DWORD ups[3] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    for (int button = 0; button < 3; ++button)
    {
        if ((held & (1 << button)) == 0) continue;
        INPUT& input = inputs[count++];
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = ups[button];
        input.mi.dwExtraInfo = kModInputMark;
    }
    for (int index = 0; index < 512 && count < 32; ++index)
    {
        if (!g_modKeyHeld[index]) continue;
        INPUT& input = inputs[count++];
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = (WORD)(index & 0xFF);
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (index >= 256 ? KEYEVENTF_EXTENDEDKEY : 0);
        input.ki.dwExtraInfo = kModInputMark;
    }
    g_modMouseHeld = 0;
    for (int index = 0; index < 512; ++index) g_modKeyHeld[index] = false;
    if (count > 0) SendInput(count, inputs, sizeof(INPUT));
    if (g_watchShared) // nothing left for the watcher to let go of
    {
        InterlockedExchange(&g_watchShared->mouseHeld, 0);
        for (int word = 0; word < 16; ++word) InterlockedExchange(&g_watchShared->keyHeld[word], 0);
    }
}

// A window as the log names it: its program and its kind (its class), never
// its title (a title can hold anything private).
static void DescribeWindow(HWND window, char* text, size_t size)
{
    if (window == nullptr)
    {
        snprintf(text, size, "no window");
        return;
    }
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t path[MAX_PATH] = L"";
    DWORD length = MAX_PATH;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (handle != nullptr)
    {
        if (!QueryFullProcessImageNameW(handle, 0, path, &length)) path[0] = L'\0';
        CloseHandle(handle);
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    wchar_t kind[64] = L"";
    GetClassNameW(window, kind, 64);
    char nameText[MAX_PATH] = "", kindText[128] = "";
    WideCharToMultiByte(CP_UTF8, 0, name, -1, nameText, sizeof(nameText), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, kind, -1, kindText, sizeof(kindText), nullptr, nullptr);
    snprintf(text, size, "%s (a \"%s\" window%s)", nameText[0] ? nameText : "a program the mod cannot name", kindText,
             process == GetCurrentProcessId() ? ", the game's own" : "");
}

// The game's window put above the windows that stay on top of all others:
// see ClickLandsOnGame; let go again in UpdateGameFront.
static bool g_gameMadeTopmost = false;

// A click the mod sends lands on whatever window is under the PC's pointer
// (with the game's mouse held in its window while playing, the window's
// middle). A window set to stay on top of every window, such as SteamVR's
// status window ("StatusAlwaysOnTop" in steamvr.vrsettings), takes a click
// under it: the game's window loses the front, and from then on the mod
// (which never clicks into another program) points and clicks at nothing.
// So before each click: if another window is over the game's at that spot,
// the game's window is put on top of those too; if it still isn't under the
// pointer, the click is held back.
static bool ClickLandsOnGame(HWND gameWindow, const char* what)
{
    POINT at;
    if (!GetCursorPos(&at)) return true; // can't tell: let the click through
    HWND under = WindowFromPoint(at);
    if (under != nullptr && GetAncestor(under, GA_ROOT) == gameWindow) return true;

    SetWindowPos(gameWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_gameMadeTopmost = true;
    HWND now = WindowFromPoint(at);
    bool lands = now != nullptr && GetAncestor(now, GA_ROOT) == gameWindow;
    static int logsLeft = 12;
    if (logsLeft > 0)
    {
        --logsLeft;
        char over[400];
        DescribeWindow(under, over, sizeof(over));
        Log("VR: a %s would have landed on another window over the game's, %s, at %d,%d: the game's window is put on top of it; %s",
            what, over, (int)at.x, (int)at.y, lands ? "the click goes to the game." : "still covered: the click is held back.");
    }
    return lands;
}

// Presses or releases the game's "interact" control, which is the left mouse
// button. The game already uses that one control to pick up, open, pull and
// press things, so squeezing a controller does the same as clicking.
static void SetInteractHeld(bool held)
{
    if (held == g_grabHeld) return;

    // Only click while the game's window is the one in use, so a squeeze can
    // never click on some other program.
    HWND gameWindow = WindowFromDC(wglGetCurrentDC());
    if (held && (gameWindow == nullptr || GetForegroundWindow() != gameWindow || g_steamOverlayOpen)) return; // (nor into Steam's overlay)
    if (held && !ClickLandsOnGame(gameWindow, "squeeze's click")) return; // (nor on a window over the game's)

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = held ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendModInput(input);
    g_grabHeld = held;

    static int logsLeft = 6;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: controller squeeze %s -> the game's interact button is %s.", held ? "pressed" : "let go",
            held ? "held down" : "released");
    }
}

static void SendScriptCommand(int command);

// The game's loading screen that ends "Press a button to continue..." (as a level loads, after a death, and after the
// restart that follows a jump): A and X continue as a squeeze does. Otherwise only a squeeze's click of the game's
// interact button (the left mouse button) continues there; A, the button most players press for "a button", would send
// nothing (it works the gun, and there is none). A fresh press of A or X while the loading screen is up gives one short
// click of its own (90 ms, as the menus' trigger click), only into the game's own window, never while a squeeze holds
// that button (g_grabHeld) and never into the mod's menu.
static bool ReadButton(XrAction action);
static void UpdateLoadingContinue(bool modMenuOpen)
{
    static bool wasDown = false;
    static double upAtMs = 0.0;
    double now = NowMs();
    if (upAtMs > 0.0 && (now >= upAtMs || g_grabHeld))
    {
        if (!g_grabHeld) // (a squeeze that took the button meanwhile lets it go itself)
        {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendModInput(input);
        }
        upAtMs = 0.0;
    }
    bool down = ReadButton(g_gunAction) || ReadButton(g_aimAction2);
    bool fresh = down && !wasDown;
    wasDown = down;
    if (!fresh || upAtMs > 0.0 || modMenuOpen || g_dedicationHolding || g_grabHeld || !CameraLoadingScreen()) return;
    HWND gameWindow = WindowFromDC(wglGetCurrentDC());
    if (gameWindow == nullptr || GetForegroundWindow() != gameWindow || g_steamOverlayOpen) return;
    if (!ClickLandsOnGame(gameWindow, "A's click on the loading screen")) return;
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendModInput(input);
    upAtMs = now + 90.0;
    static int logsLeft = 6;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: A or X pressed on the game's loading screen -> a click of the game's interact button, as a squeeze gives (\"Press a button to continue\").");
    }
}

// What each hand's squeeze is being used for.
enum HandUse
{
    kHandFree,    // not squeezing
    kHandGame,    // the game's own "interact" button (grab, open, press, pick up)
    kHandScript,  // holding a loose object by itself (see VRUpdateFreeHands in PlayerBodyHandler.hps)
    kHandOnGun,   // left hand steadying the gun
    kHandBullet,  // left hand carrying a bullet to the gun
    kHandHeal,    // unused (a right hand pressing a bandage or medkit to the chest)
    kHandTin,     // left hand on the bandage tin (or medkit) in the right hand, to pull the roll out
    kHandCord,    // right hand on the flashlight's cord (the flashlight is in the left)
    kHandPin,     // left hand on the pin of the grenade in the right hand
    kHandGrenade, // right hand gripping a grenade, ready to throw it
    kHandReach,   // left hand squeezed with the gun or an item ready in the right, touching nothing yet
    kHandNothing  // squeezing, but with nothing to do until it lets go
};

// Set in DrawHands each frame.
static bool g_leftAtLightHolster = false; // is the left palm at the left hip (the ammo pouch while the revolver is open)?
static float g_leftToPouch = 1.0f;        // ...how far the left palm is from the pouch's spot (meters)

// The pouch's reach is the holster's, 22 cm across the floor and from 15 cm above
// its spot to 30 cm below: most of the lap in front, where things lie to be taken,
// so a squeeze at an item close in front would take a bullet instead. So a left
// hand that touches a thing takes the thing, unless it is this near the pouch's
// spot. (With nothing else touched, a bullet is taken anywhere in the pouch's
// reach; typically 11 to 20 cm from its spot.)
static const float kPouchOverThing = 0.10f;

// Does a left squeeze at the pouch go to a thing the hand touches instead?
static bool LeftTakesThingNotBullet()
{
    return CameraTouchKind(0) != kTouchNothing && g_leftToPouch > kPouchOverThing;
}
static bool g_rightAtShoulder = false; // is the right palm behind the right shoulder (the shotgun's place)?
// Until when the aim (the right mouse button the mod holds while a gun is out)
// is let go of for the shotgun's put-away behind the shoulder, as X's put-away
// does: held, the game takes the gun up again at once.
static double g_shoulderQuietUntilMs = 0.0;
// Until when it is let go of for the inventory: the game opens its inventory
// only in its normal state, and with the aim held it is in its aiming state, so
// B's Tab would do nothing while a gun is out. B lets go of the aim (and of an
// item kept ready), and its Tab waits for the normal state (UpdateMoveInput).
static double g_inventoryAimFreeUntilMs = 0.0;
static float g_pouchRoom[3] = { 0, 0, 0 }; // where that spot on the left hip is, in the room
static bool g_pouchValid = false;
static bool g_leftNearPouch = false;       // is the left hand within reach of it (a wider ring, for the bullet counter)?
static bool g_leftAtChestLight = false;   // is the left palm at the left side of the chest (where the flashlight hangs)?
static bool g_rightAtChestSlot = false;   // is the right palm at the right chest's item slot (never where the holster or shoulder takes it)?
static float g_chestSlotRoom[3] = { 0, 0, 0 }; // ...and its label's spot, in front of the body (OutOfTheChest)
static bool g_slotDone = false;          // ...something was taken out or stored there: nothing more until the hand has left it (as the shoulder's)
// Where those places are in the room this frame, for the hints' labels (set
// with the flags above in DrawHands; each only while its hand is known).
static float g_holsterRoomNow[3] = { 0, 0, 0 };
static float g_chestLightRoom[3] = { 0, 0, 0 }; // at the chest: its label's spot, in front of the body (OutOfTheChest)
static bool g_holsterRoomKnown = false;
static XrPosef g_hintGrip[2] = {};                  // and each palm this frame (the grip pose)
static bool g_hintGripKnown[2] = { false, false };

// A label at the chest goes at least this far in front of the neck, level.
// The chest places themselves can be about 13 cm in front of the neck: inside
// the drawn jacket, where a label would be hidden in the torso. 28 cm is clear
// of it and still in front of the place, as the hand comes to it.
static const float kChestLabelAhead = 0.28f;

// `spot` moved straight forward (level, `fx` `fz`) until it is
// kChestLabelAhead in front of the neck; left where it is if it is already.
static void OutOfTheChest(const float spot[3], const XrVector3f& neck, float fx, float fz, float out[3])
{
    float ahead = (spot[0] - neck.x) * fx + (spot[2] - neck.z) * fz;
    float push = ahead < kChestLabelAhead ? kChestLabelAhead - ahead : 0.0f;
    out[0] = spot[0] + fx * push;
    out[1] = spot[1];
    out[2] = spot[2] + fz * push;
}

// Where the left palm is from the pouch's spot, along the head's level right,
// up and forward, in words for the log (where the hand really takes a bullet,
// to compare with where the counter is drawn).
static void PouchOffsetWords(char* out, int size)
{
    out[0] = '\0';
    if (!g_hintGripKnown[0] || !g_pouchValid) return;
    const XrQuaternionf& q = g_frameHeadPose.orientation;
    float rx = 1 - 2 * (q.y * q.y + q.z * q.z), rz = 2 * (q.x * q.z - q.w * q.y);
    float length = sqrtf(rx * rx + rz * rz);
    if (length < 0.25f) return;
    rx /= length;
    rz /= length;
    float dx = g_hintGrip[0].position.x - g_pouchRoom[0], dy = g_hintGrip[0].position.y - g_pouchRoom[1], dz = g_hintGrip[0].position.z - g_pouchRoom[2];
    float right = dx * rx + dz * rz, ahead = dx * rz - dz * rx; // (level forward is (rz, -rx))
    snprintf(out, (size_t)size, " (the palm %.0f cm %s, %.0f cm %s, %.0f cm %s of the pouch's spot)",
             fabsf(right) * 100.0f, right >= 0.0f ? "right" : "left", fabsf(dy) * 100.0f, dy >= 0.0f ? "above" : "below",
             fabsf(ahead) * 100.0f, ahead >= 0.0f ? "in front" : "behind");
}

static float g_rightHandSpeed = 0.0f;     // the fastest the right hand moved in the last moments, meters a second
static float g_rightHandSpeedNow = 0.0f;  // and how fast it is moving this frame

// A grenade's pin has been pulled; letting go of it in a throw sends it off.
static bool g_grenadeArmed = false;

// The right grip has taken hold of the item in the right hand to throw it
// (the hands-on items' shared throw move; see UpdateGrabInput).
static bool g_throwHeld = false;

// Pulling things to the hand, for items the player can't reach: point, hold
// the grip, flick the wrist back, and the item flies into the hand, as in
// Half-Life: Alyx (Settings::pullMode: Off / Seated only / Always). The body
// script finds the item an empty hand points at (it glows) and says which
// hands have one (roll 7096, VrPullTargetsNews); here a fresh press of that
// hand's grip takes aim at it (locked, a small buzz), and while the grip is
// held a quick move of the hand back towards the head or upwards pulls it
// (a stronger buzz). The body script asks how each hand stands (roll 7097,
// VrPullState), flies the item to the hand and picks it up the game's way
// as it comes. (UpdatePull, called from UpdateGrabInput.)
enum { kPullIdle = 0, kPullLocked = 1, kPullPulled = 2 };
static int g_pullState[2] = { kPullIdle, kPullIdle };
static double g_pullSinceMs[2] = { 0.0, 0.0 };
static bool g_pullTarget[2] = { false, false };
static unsigned long long g_pullTargetTime = 0;
static const float kPullFlickSpeed = 1.2f;   // meters a second, back towards the head and up (half each)
static const double kPullLockedMostMs = 2500.0; // locked this long with no flick: let go of
static const double kPullPulledMs = 400.0;     // "pulled" is told this long, for the script to see once

void VrPullTargetsNews(bool left, bool right)
{
    g_pullTarget[0] = left;
    g_pullTarget[1] = right;
    g_pullTargetTime = GetTickCount64();
}

static bool PullOn()
{
    const Settings& s = GetSettings();
    return s.pullMode == 2 || (s.pullMode == 1 && s.seated);
}

int VrPullState(int hand)
{
    if (hand < 0 || hand > 1 || !PullOn()) return -1;
    HeadPose unused;
    if (!VrGetGripHandPose(hand, unused)) return -2; // (that hand's palm is not known)
    return g_pullState[hand];
}

// Healing by hand: 0 = the bandage is still in its tin (or the medkit shut),
// 1 = the roll is out and being wound around the left forearm, 2 = done.
// And how many times it has been wound around so far.
static int g_bandagePhase = 0;
static float g_bandageTurns = 0.0f;
static double g_bandageDoneAtMs = 0;

int VrGetBandage(float& turns)
{
    turns = g_bandageTurns;
    return g_bandagePhase;
}

// When the revolver's spent shells were last tipped out (0 = not yet).
static double g_shellsOutAtMs = 0;
static int g_handUse[2] = { kHandFree, kHandFree };
static bool g_handDown[2] = { false, false };
static int g_pendingHand = -1;    // hand whose squeeze has not been passed to the game yet
static int g_pendingFrames = 0;
static bool g_pendingNeedNormal = false; // ...and it waits for the game to be back in its normal state first (the hands swapped)

// How long a squeeze made a moment before the hand touches anything still
// takes what the hand comes to, while it stays squeezed, when the other hand
// has the gun out, an item ready or something carried (the same wait as
// gfVRInteractWait in PlayerState_Normal.hps, for the hand alone).
static const double kReachWaitMs = 350.0;
static double g_reachUntilMs[2] = { 0.0, 0.0 };
// With the gun out, a left hand this near the gun steadies it even if it
// touches something (farther off, within kHandOnGun's reach, it takes what
// it touches; meters, from a point a little in front of the right palm).
static const float kOnGunTouching = 0.10f;
static bool g_fireHeld = false;   // the right trigger is holding the interact button down (firing)
static bool g_bulletTap = false;  // the interact button is down for a moment to load a bullet
static double g_bulletTapUntil = 0;
static bool g_aimPaused = false;  // aiming is let go so the left hand can use the interact button
static float g_gripValue[2] = { 0.0f, 0.0f };    // how far each grip button is squeezed, 0 to 1
static float g_triggerValue[2] = { 0.0f, 0.0f }; // and each trigger
static bool g_rightInHolster = false;   // is the right palm at the holster (set in DrawHands)?
static bool g_rightGripDown = false;
static double g_virtualGunTapUntil = 0; // until then, the gun is to be drawn or put away (UpdateGunButton)
static bool g_virtualGunTapHip = false;  // that tap is the right hip's: the revolver's own (script command 78)

// Once a headset frame, after the mod's menu has had the pointer (`pointing`:
// a hand, g_pointerHand, points at something on it to press: that pull is a
// press): a new pull of each trigger, for the measurements (g_takePulled).
static void UpdateTakePulls(bool pointing)
{
    static bool down[2] = { false, false };
    for (int hand = 0; hand < 2; ++hand)
    {
        bool now = g_triggerValue[hand] > (down[hand] ? 0.35f : 0.6f);
        g_takePulled[hand] = now && !down[hand] && !(pointing && g_pointerHand == hand);
        down[hand] = now;
    }
}

// In the game's menus (a hand is pointing at the panel):
//   trigger = a click. The button goes down and up by itself, and the pointer
//             holds still meanwhile, so a click can never turn into a drag.
//   grip    = hold. The button stays down while the grip is squeezed, for
//             dragging things (inventory) and sliders.
static bool g_menuPressByGrip = false;
static double g_pointerFrozenUntil = 0;
static double g_gameMenuPressMs = -1000000.0; // when a hand last pressed on the game's menus (the cursor's pulse)
static double g_gameMenuFocusMs = -1000000.0; // when a button of the game's menus last came into focus (cVR_MenuFocus)

// A light tick in the hand as the pointer comes onto something to press, as
// native VR menus do. Not more than one in 60 ms.
static void HoverBuzz(int hand)
{
    static double lastMs = -1000.0;
    double now = NowMs();
    if (now - lastMs < 60.0) return;
    lastMs = now;
    Buzz(hand, 0.015f, 0.35f);
}

void VrMenuFocus()
{
    g_gameMenuFocusMs = NowMs();
    if (g_pointerActive) HoverBuzz(g_pointerHand); // (only while a hand steers the game's mouse: not for the keyboard)
}

bool VrMenuPressWasGrip()
{
    return g_menuPressByGrip;
}

int VrGetScriptHoldMask()
{
    int mask = 0;
    HeadPose unused;
    if (g_handUse[0] == kHandScript && VrGetGripHandPose(0, unused)) mask |= 1;
    if (g_handUse[1] == kHandScript && VrGetGripHandPose(1, unused)) mask |= 2;
    return mask;
}

// A held with the shotgun out shows the ammo in the gun, as the open revolver
// does: told to the shotgun's script (+8 below), whose pump then only draws
// back partway, throwing nothing out (a press-check), and says so in its news
// (CameraShotgunCheckShown): the bullet counter then shows above the gun.
static bool g_shotgunCheckHeld = false;

int VrGetGunHandFlags()
{
    return (g_reloadKeyDown ? 1 : 0) | (g_handUse[0] == kHandOnGun ? 2 : 0) | (g_handUse[0] == kHandBullet ? 4 : 0) | (g_shotgunCheckHeld ? 8 : 0);
}

// The game's interact button goes to this hand: the other hand lets go of it
// and keeps what it carries by itself.
static void GiveInteractButtonTo(int hand)
{
    int other = 1 - hand;
    g_handUse[other] = kHandScript;
    g_reachUntilMs[other] = 0.0;
    if (g_pendingHand == other) g_pendingHand = -1;
    SetInteractHeld(false);
    g_handUse[hand] = kHandGame;
    g_pendingHand = hand;
    g_pendingFrames = 0;
    g_pendingNeedNormal = true;
    g_grabHand = hand;
    static int logsLeft = 20;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: the %s hand reaches for something only the game can use: it takes the game's interact button; "
            "the %s hand keeps what it carries by itself.", hand == 0 ? "left" : "right", other == 0 ? "left" : "right");
    }
}

bool VrOtherHandWaits()
{
    return g_aimPaused;
}

float VrGetGripSqueeze(int hand)
{
    return (hand == 0 || hand == 1) ? g_gripValue[hand] : 0.0f;
}

float VrGetTrigger(int hand)
{
    return (hand == 0 || hand == 1) ? g_triggerValue[hand] : 0.0f;
}

// When the body script last said the right hand's item is fitted
// (cVR_RightFitted, VRUpdateFit in PlayerBodyHandler.hps), and for how long
// that holds without a new report (it reports every update).
static double g_rightFittedMs = 0.0;
static const double kRightFittedMs = 300.0;

void VrRightFitted()
{
    g_rightFittedMs = NowMs();
}

static bool RightFittedNow()
{
    return g_rightFittedMs > 0.0 && NowMs() - g_rightFittedMs < kRightFittedMs;
}

// When the shotgun's script last said the left palm is at its fore-end
// (cVR_LeftAtPump, Prop_Shotgun.hps), and for how long that holds without a
// new report (it reports every update while the shotgun is held).
static double g_leftAtPumpMs = 0.0;

void VrLeftAtPump(bool at)
{
    g_leftAtPumpMs = at ? NowMs() : 0.0;
}

static bool LeftAtPumpNow()
{
    return g_leftAtPumpMs > 0.0 && NowMs() - g_leftAtPumpMs < kRightFittedMs;
}

// When the body script last said a knockdown, a kill or a death is playing
// (cVR_NoTurning, PlayerBodyHandler.hps: it says so every update while one is).
// While that holds, no turn of the stick reaches the game: no mouse steps, no
// snap turn and no turn handed to the body script (which would turn the
// character, and the kill's animation with it, away from the Beast).
static double g_noTurningMs = 0.0;

void VrNoTurning(bool none)
{
    g_noTurningMs = none ? NowMs() : 0.0;
}

static bool NoTurningNow()
{
    return g_noTurningMs > 0.0 && NowMs() - g_noTurningMs < kRightFittedMs;
}

// Does the left hand stay on the shotgun's pump, even when let go of, because
// the hand poses tool (F11) is open, so that its pose can be set with the
// controllers put down? Also while the tool's window has the PC's focus (with
// the game's window out of use, UpdateMoveInput lets go of everything, the pump
// too), and while the game, in the background, sends no news of the gun: only
// fresh news that the shotgun is gone ends it.
static bool LeftPumpKeptForPoseTool()
{
    if (g_handUse[0] != kHandOnGun || !PoseToolIsOpen()) return false;
    return CameraShotgunHeld() || !CameraGunNewsFresh();
}

// Reads how hard each grip button and trigger is squeezed and decides what
// each hand does with it. Call after the controller readings were fetched.
//
// The game has ONE interact button. The first hand to squeeze gets it (it can
// grab, open doors, pick things up). If the other hand squeezes while the
// first still holds, it holds a loose object by itself instead: the body
// script moves that object with the hand.
//
// With a gun in the right hand:
//   right trigger   fires
//   right grip      only closes the fingers
//   left squeeze    next to the gun: steadies it. Elsewhere: aiming is let go
//                   for a moment so the left hand can use the interact button.
//   ammo check open (A held): squeeze the left hand and touch it to the gun
//                   to load one bullet per touch.
// Pulling things to the hand: each hand's aim and flick (see
// g_pullState). A fresh press of an empty hand's grip while the body script
// says that hand points at a thing it may pull takes aim at it; then, while
// the grip is held, the palm moving back towards the head and upwards
// (half each) at kPullFlickSpeed or more pulls it. Not a press that is the
// holster's or the flashlight's.
static void UpdatePull(const float grip[2], float press, float release, bool gunMode, int leftKind, int rightKind, bool holsterGrab,
                       bool lightGrab)
{
    static bool gripWasDown[2] = { false, false };
    static XrVector3f lastPalm[2] = {};
    static double lastPalmMs[2] = { 0.0, 0.0 };
    static int logsLeft = 40;
    double now = NowMs();
    bool on = PullOn();
    bool targetNews = g_pullTargetTime != 0 && GetTickCount64() - g_pullTargetTime < 400;
    for (int hand = 0; hand < 2; ++hand)
    {
        bool down = grip[hand] > (gripWasDown[hand] ? release : press);
        bool pressed = down && !gripWasDown[hand];
        gripWasDown[hand] = down;

        // How fast the palm moves back towards the head and upwards (room
        // meters a second), from where it was at the last frame.
        float flick = 0.0f;
        {
            AcquireSRWLockShared(&g_poseLock);
            XrVector3f palm = g_sharedGripRoom[hand].position;
            unsigned long long palmTime = g_sharedGripTime[hand];
            ReleaseSRWLockShared(&g_poseLock);
            bool fresh = palmTime != 0 && GetTickCount64() - palmTime < 100 && !g_lostHand[hand].held;
            double seconds = (now - lastPalmMs[hand]) / 1000.0;
            if (fresh && lastPalmMs[hand] > 0.0 && seconds > 0.001 && seconds < 0.1 && g_framePoseValid)
            {
                float v[3] = { (float)((palm.x - lastPalm[hand].x) / seconds), (float)((palm.y - lastPalm[hand].y) / seconds),
                               (float)((palm.z - lastPalm[hand].z) / seconds) };
                const XrVector3f& head = g_frameHeadPose.position;
                float toHead[3] = { head.x - palm.x, head.y - palm.y, head.z - palm.z };
                float length = sqrtf(toHead[0] * toHead[0] + toHead[1] * toHead[1] + toHead[2] * toHead[2]);
                if (length > 0.05f)
                {
                    float d[3] = { toHead[0] / length, toHead[1] / length + 1.0f, toHead[2] / length };
                    float dLength = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (dLength > 0.01f) flick = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / dLength;
                }
            }
            lastPalm[hand] = palm;
            lastPalmMs[hand] = fresh ? now : 0.0;
        }

        if (!on)
        {
            g_pullState[hand] = kPullIdle;
            continue;
        }
        bool empty = hand == 1 ? (rightKind == kItemNone && !gunMode) : (leftKind == kItemNone);
        const char* name = hand == 0 ? "left" : "right";
        switch (g_pullState[hand])
        {
        case kPullIdle:
            if (pressed && empty && targetNews && g_pullTarget[hand] && !(hand == 1 && holsterGrab) && !(hand == 0 && lightGrab))
            {
                g_pullState[hand] = kPullLocked;
                g_pullSinceMs[hand] = now;
                Buzz(hand, 0.03f, 0.4f);
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("PULL: the %s hand takes aim at the thing it points at.", name);
                }
            }
            break;
        case kPullLocked:
            if (!down || !empty || now - g_pullSinceMs[hand] > kPullLockedMostMs)
            {
                g_pullState[hand] = kPullIdle;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("PULL: the %s hand %s.", name, !down ? "let go without pulling" : (!empty ? "took something else" : "waited too long: not pulled"));
                }
            }
            else if (flick > kPullFlickSpeed)
            {
                g_pullState[hand] = kPullPulled;
                g_pullSinceMs[hand] = now;
                Buzz(hand, 0.08f, 0.9f);
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("PULL: the %s hand pulls (moving back and up at %.1f m/s; %.1f needed).", name, flick, kPullFlickSpeed);
                }
            }
            break;
        default:
            if (now - g_pullSinceMs[hand] > kPullPulledMs) g_pullState[hand] = kPullIdle;
            break;
        }
    }
}

static void UpdateGrabInput()
{
    if (!g_grabButtonsReady) return;

    bool gunMode = CameraGunHeld();
    const float kPress = 0.6f, kRelease = 0.35f;

    float grip[2] = { 0.0f, 0.0f }, trigger[2] = { 0.0f, 0.0f };
    XrAction actions[2] = { g_squeezeAction, g_triggerAction };
    for (int hand = 0; hand < 2; ++hand)
    {
        for (int i = 0; i < 2; ++i)
        {
            XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = actions[i];
            getInfo.subactionPath = g_handPaths[hand];

            XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_FAILED(xrGetActionStateFloat(g_session, &getInfo, &state)) || !state.isActive) continue;
            (i == 0 ? grip : trigger)[hand] = state.currentState;
        }
        // The Valve Index's handle (see g_gripForceAction): until a squeeze starts the grab, a closed hand reads
        // as half closed (its fingers still close that far), below the grab's press. Only where the headset
        // software gives the force: without it, the grip value is used unchanged.
        if (g_gripForceBound && g_indexHand[hand])
        {
            XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = g_gripForceAction;
            getInfo.subactionPath = g_handPaths[hand];
            XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &getInfo, &state)) && state.isActive)
            {
                if (!g_indexGrabbing[hand]) g_indexGrabbing[hand] = grip[hand] > kPress && state.currentState >= kIndexForceGrab;
                else g_indexGrabbing[hand] = grip[hand] > kRelease;
                if (!g_indexGrabbing[hand] && grip[hand] > kIndexOpenMost) grip[hand] = kIndexOpenMost;
            }
            else g_indexGrabbing[hand] = false;
        }
        g_gripValue[hand] = grip[hand];
        g_triggerValue[hand] = trigger[hand];
    }

    // The game's fingers close as the hand squeezes. They follow the squeeze a
    // little behind (a fixed share of the way each frame) so they never snap.
    for (int hand = 0; hand < 2; ++hand)
    {
        float squeezed = grip[hand] > trigger[hand] ? grip[hand] : trigger[hand];
        float step = 0.14f;
        float curl = g_handCurl[hand];
        if (squeezed > curl + step) curl += step;
        else if (squeezed < curl - step) curl -= step;
        else curl = squeezed;
        g_handCurl[hand] = curl;
    }

    // The same per finger, for posing empty hands:
    //   the three outer fingers follow the grip button;
    //   the first finger is straight while it is off the trigger, lightly
    //     bent while it rests on it, and closes as the trigger is pulled;
    //   the thumb is out while it touches nothing, and folds in when it rests
    //     on the stick or a button: a little with an open hand, fully on a fist.
    // That gives an open hand, a fist, a pointing finger and a thumbs-up from
    // what the real hand is doing.
    for (int hand = 0; hand < 2; ++hand)
    {
        bool thumbOn = true, triggerOn = trigger[hand] > 0.05f;
        if (g_touchReady)
        {
            XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.subactionPath = g_handPaths[hand];
            XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (g_thumbTouchBound)
            {
                getInfo.action = g_thumbTouchAction;
                if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &getInfo, &state)) && state.isActive)
                    thumbOn = state.currentState == XR_TRUE;
            }
            if (g_triggerTouchBound)
            {
                getInfo.action = g_triggerTouchAction;
                state = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &getInfo, &state)) && state.isActive)
                    triggerOn = triggerOn || state.currentState == XR_TRUE;
            }
        }
        if (!g_triggerTouchBound) triggerOn = triggerOn || grip[hand] > 0.5f; // no sensor: the finger closes with the fist

        float wanted[3];
        wanted[2] = grip[hand];
        float pull = trigger[hand];
        if (!g_triggerTouchBound && grip[hand] > pull) pull = grip[hand];
        wanted[1] = triggerOn ? 0.3f + 0.7f * pull : 0.0f;
        float fist = wanted[1] > wanted[2] ? wanted[1] : wanted[2];
        wanted[0] = thumbOn ? 0.45f + 0.55f * fist : 0.0f;

        for (int i = 0; i < 3; ++i)
        {
            const float step = 0.16f;
            float curl = g_fingerCurl[hand][i];
            if (wanted[i] > curl + step) curl += step;
            else if (wanted[i] < curl - step) curl -= step;
            else curl = wanted[i];
            g_fingerCurl[hand][i] = curl;
        }
    }

    // While the mod's own menu is open, squeezing clicks in that menu and
    // nothing reaches the game. (Nor while the dedication is shown at the
    // start, with the game's menu held back.)
    if (VrMenuIsOpen() || g_dedicationHolding)
    {
        if (g_grabHeld) SetInteractHeld(false);
        for (int hand = 0; hand < 2; ++hand)
        {
            float squeezed = grip[hand] > trigger[hand] ? grip[hand] : trigger[hand];
            // A fresh squeeze while the studio's card waits for the headset
            // to be put on says it is on: someone is holding the controllers
            // and looking at the black. The main menu's room doesn't let the
            // card go, so nothing else would let it go for a player who keeps
            // quite still. Not while the mod's menu is open.
            if (g_dedicationHolding && !VrMenuIsOpen() && squeezed > kPress && !g_handDown[hand]) g_dedicationSqueezed = true;
            g_handDown[hand] = squeezed > kRelease; // so that it is not taken as a new squeeze when the menu closes
            // A left hand on the gun that still squeezes keeps it, so the
            // shotgun's pump stays held while its grip is set on the Grips
            // page. On the shotgun's pump it stays even when let go of, until
            // the menu is shut, so it doesn't keep coming off the gun while
            // the grip is being set.
            bool keepOnGun = hand == 0 && g_handUse[0] == kHandOnGun && (g_handDown[0] || CameraShotgunHeld() || LeftPumpKeptForPoseTool());
            g_handUse[hand] = keepOnGun ? kHandOnGun : (g_handDown[hand] ? kHandNothing : kHandFree);
        }
        g_rightGripDown = grip[1] > kRelease;
        g_pendingHand = -1;
        g_fireHeld = false;
        g_bulletTap = false;
        g_aimPaused = false;
        return;
    }

    // Which buttons went down this very frame.
    static bool gripWasDown[2] = { false, false }, triggerWasDown[2] = { false, false };
    bool gripDown[2], triggerDown[2], gripPressed = false, triggerPressed = false;
    int pressHand = -1; // the hand that pressed this frame (a grip counts before a trigger)
    for (int hand = 0; hand < 2; ++hand)
    {
        gripDown[hand] = grip[hand] > (gripWasDown[hand] ? kRelease : kPress);
        triggerDown[hand] = trigger[hand] > (triggerWasDown[hand] ? kRelease : kPress);
        if (gripDown[hand] && !gripWasDown[hand] && !gripPressed) pressHand = hand;
        else if (triggerDown[hand] && !triggerWasDown[hand] && !gripPressed && !triggerPressed) pressHand = hand;
        gripPressed = gripPressed || (gripDown[hand] && !gripWasDown[hand]);
        triggerPressed = triggerPressed || (triggerDown[hand] && !triggerWasDown[hand]);
        gripWasDown[hand] = gripDown[hand];
        triggerWasDown[hand] = triggerDown[hand];
    }

    // --- In one of the game's menus: the trigger clicks, the grip holds.
    static bool menuMode = false, menuHold = false, menuHoldFromBefore = false, menuClick = false;
    static double menuClickUntil = 0;
    if (g_pointerActive)
    {
        bool anyGrip = gripDown[0] || gripDown[1];
        bool anyTrigger = triggerDown[0] || triggerDown[1];
        double now = NowMs();

        if (!menuMode)
        {
            // Just arrived in a menu. If the button was already held for
            // something in the game, it stays held until that hand lets go.
            menuMode = true;
            menuClick = false;
            menuHold = g_grabHeld;
            menuHoldFromBefore = g_grabHeld;
        }

        if (menuClick && now > menuClickUntil)
        {
            SetInteractHeld(false);
            menuClick = false;
        }
        if (menuHold && !anyGrip && !(menuHoldFromBefore && anyTrigger))
        {
            SetInteractHeld(false);
            menuHold = false;
            menuHoldFromBefore = false;
        }
        // A press by the hand that is not steering the pointer: the pointer
        // goes to that hand first, and the press follows a moment later, once
        // the game has seen the pointer move.
        static int waitingHand = -1;
        static bool waitingGrip = false;
        static double waitingUntil = 0;
        if (pressHand >= 0 && pressHand != g_pointerHand && g_pointerHit[pressHand] && !menuHold && !menuClick && !g_grabHeld)
        {
            g_menuHand = pressHand;
            waitingHand = pressHand;
            waitingGrip = gripPressed;
            waitingUntil = now + 90.0;
            gripPressed = false;
            triggerPressed = false;
        }
        else if (pressHand >= 0)
        {
            g_menuHand = pressHand;
        }
        if (waitingHand >= 0 && now >= waitingUntil)
        {
            if (waitingGrip) gripPressed = gripDown[waitingHand]; // still squeezed?
            else triggerPressed = true;
            waitingHand = -1;
        }

        if (!menuHold && !menuClick && !g_grabHeld)
        {
            if (gripPressed)
            {
                g_menuPressByGrip = true;
                SetInteractHeld(true);
                menuHold = g_grabHeld;
                if (g_grabHeld) g_gameMenuPressMs = now; // (the cursor's pulse)
            }
            else if (triggerPressed)
            {
                g_menuPressByGrip = false;
                SetInteractHeld(true);
                menuClick = g_grabHeld;
                menuClickUntil = now + 90.0;
                g_pointerFrozenUntil = now + 170.0;
                if (g_grabHeld) g_gameMenuPressMs = now;
            }
        }

        for (int hand = 0; hand < 2; ++hand)
        {
            g_handDown[hand] = gripDown[hand] || triggerDown[hand]; // not a new squeeze when the menu closes
            g_handUse[hand] = g_handDown[hand] ? kHandNothing : kHandFree;
        }
        g_rightGripDown = gripDown[1];
        g_pendingHand = -1;
        g_fireHeld = false;
        g_bulletTap = false;
        g_aimPaused = false;
        return;
    }
    if (menuMode)
    {
        // Left the menu (or stopped pointing at it): let go of the button.
        menuMode = false;
        if (menuHold || menuClick) SetInteractHeld(false);
        menuHold = false;
        menuHoldFromBefore = false;
        menuClick = false;
    }

    // --- Setting an item's grip by hand (the Grips page's "Move it with the
    // --- other hand"; see grips.h): neither hand's squeeze
    // --- reaches the game (the item is not used, nothing is taken hold of);
    // --- the other hand's squeeze moves the item as if that hand held it.
    {
        bool squeezedNow[2];
        for (int hand = 0; hand < 2; ++hand)
        {
            float squeezed = grip[hand] > trigger[hand] ? grip[hand] : trigger[hand];
            squeezedNow[hand] = squeezed > (g_handDown[hand] ? kRelease : kPress);
        }
        int gripSetting = GripsTakeSqueezes(squeezedNow);
        if (gripSetting != 0)
        {
            if (g_grabHeld) SetInteractHeld(false);
            for (int hand = 0; hand < 2; ++hand)
            {
                g_handDown[hand] = squeezedNow[hand];
                g_handUse[hand] = g_handDown[hand] ? kHandNothing : kHandFree;
            }
            g_rightGripDown = grip[1] > kRelease;
            g_pendingHand = -1;
            g_fireHeld = false;
            g_bulletTap = false;
            g_aimPaused = false;
            g_throwHeld = false; // (the right grip moves or holds the item here: no throw when it lets go)
            int otherHand = GripsView().hand == 0 ? 0 : 1; // (the page's choice: 0 the right hand holds the item, so the left moves it)
            if (gripSetting == 2) Buzz(otherHand, 0.04f, 0.5f); // took hold of it
            if (gripSetting == 3) Buzz(otherHand, 0.03f, 0.3f); // let go: saved
            return;
        }
    }

    // --- The holster: squeezing the right grip at the right hip draws the gun
    // --- or puts it away (the only way to do either: UpdateGunButton).
    bool rightGripNow = grip[1] > (g_rightGripDown ? kRelease : kPress);
    bool holsterGrab = rightGripNow && !g_rightGripDown && g_rightInHolster && !g_reloadKeyDown;

    // --- Behind the right shoulder: the shotgun. Out, the hand
    // --- held there a quarter of a second puts it away; carried, a right
    // --- squeeze there takes it out (script command 75 does either). Once
    // --- done, nothing more until the hand has left the place.
    bool shoulderGrab = false; // (that squeeze is the shoulder's own: nothing else is done with it)
    {
        static double shoulderSince = 0;
        static bool shoulderDone = false;
        double now = NowMs();
        bool shotgunOut = gunMode && CameraShotgunHeld();
        // (Not while the right hand's item is fitted into something, the wrench
        // on a bolt say: its squeeze is the fit's own.)
        shoulderGrab = rightGripNow && !g_rightGripDown && g_rightAtShoulder && !shotgunOut && !g_reloadKeyDown && !shoulderDone && !RightFittedNow();
        const char* shoulderWhat = nullptr;
        if (!g_rightAtShoulder)
        {
            shoulderSince = 0;
            shoulderDone = false;
        }
        else if (shotgunOut && !shoulderDone)
        {
            if (shoulderSince == 0) shoulderSince = now;
            else if (now - shoulderSince > 250.0) shoulderWhat = "the shotgun put away";
        }
        if (shoulderGrab) shoulderWhat = "the shotgun taken out (if carried)";
        if (shoulderWhat != nullptr)
        {
            SendScriptCommand(75);
            if (shotgunOut) g_shoulderQuietUntilMs = now + 500.0; // (the aim let go of meanwhile)
            // And an item kept ready in the right hand, a bottle say, is let go of as the shotgun is taken out: the
            // game takes the shotgun only from its normal state, which the held ready button keeps it out of. The
            // body script waits for that up to a second: VRDoBodySlotWait.
            else if (!gunMode && (HintCarriedFlags(now) & kCarryShotgun) != 0) g_shoulderQuietUntilMs = now + 500.0;
            shoulderDone = true;
            Buzz(1, 0.05f, 0.6f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: right hand behind the right shoulder -> %s.", shoulderWhat);
            }
        }
    }
    // --- The right chest's item slot: the flat game's hotkey 1. Like Half-Life: Alyx's wrist
    // --- pockets: a place on the body that holds one item of any kind, put in and taken out
    // --- by hand, with a light tick as the hand comes to it able to do either. The item is the
    // --- game's own hotkey 1, so the game's inventory marks it and a saved game keeps it. An
    // --- empty right hand squeezing there takes it out (script command 79: the game's hotkey,
    // --- as the key 1 does); an item in the right hand squeezed there, or a thing held by the
    // --- grip to throw (or an unarmed grenade) let go of there, is put in: made the slot's
    // --- item and put away the game's way (command 80). The scripts say whether the slot's
    // --- item is carried and whether the item in the hand may go in (kCarryChestItem,
    // --- kCarryChestTakes: the game's hotkeys take only items used on things or on the
    // --- player). Not an armed grenade, nor a bandage whose roll is out (the right hand
    // --- winding it around the left forearm passes the slot, and a squeeze there would put it
    // --- away half wound; X still puts it away). A squeeze the slot has no use for is the
    // --- hand's own, and a hand touching a thing it can take takes the thing.
    bool slotGrab = false;  // (this squeeze is the slot's: nothing else is done with it)
    bool slotTakes = false; // (the item in the right hand would go in here: let go of here, a thing held to throw goes in)
    {
        int carried = HintCarriedFlags(NowMs());
        int kind = CameraRightItemKind();
        bool freshGrip = rightGripNow && !g_rightGripDown;
        // (An empty hand takes out with its fresh squeeze, grip or trigger, as it takes hold of anything:
        // a hand closed trigger first would otherwise be squeezing already when its grip came, and take
        // nothing. With the hand's squeeze, the loop below gives that squeeze to the slot: slotGrab.)
        float rightSqueeze = grip[1] > trigger[1] ? grip[1] : trigger[1];
        bool freshSqueeze = rightSqueeze > kPress && !g_handDown[1];
        // (Once a thing is taken out or stored, nothing more until the hand has left the slot, as behind the
        // shoulder: a grip closing a moment after a trigger that took the item out would store it again.)
        if (!g_rightAtChestSlot) g_slotDone = false;
        bool here = !g_slotDone && g_rightAtChestSlot && !holsterGrab && !shoulderGrab && !gunMode && !g_reloadKeyDown && !RightFittedNow() && !CameraInventoryOpen();
        bool takeOut = here && kind == kItemNone && !g_handDown[1] && (carried & kCarryChestItem) != 0 && CameraTouchKind(1) == kTouchNothing;
        slotTakes = here && kind != kItemNone && (carried & kCarryChestTakes) != 0 && !(kind == kItemGrenade && g_grenadeArmed) &&
                    !(kind == kItemHealth && g_bandagePhase == 1);
        static bool wasReady = false;
        bool ready = takeOut || slotTakes;
        if (ready && !wasReady) Buzz(1, 0.015f, 0.25f);
        wasReady = ready;
        const char* slotWhat = nullptr;
        if (freshSqueeze && takeOut)
        {
            SendScriptCommand(79);
            HintUsed("slot_take", NowMs());
            slotWhat = "the slot's item taken out (the game's hotkey 1)";
        }
        else if (freshGrip && slotTakes)
        {
            SendScriptCommand(80);
            g_shoulderQuietUntilMs = NowMs() + 500.0; // (the item kept ready let go of meanwhile, as for the shoulder: the game puts it away only out of its own state)
            HintUsed("slot_put", NowMs());
            slotWhat = "the item in the right hand stored in the slot (made the game's hotkey 1, put away)";
        }
        if (slotWhat)
        {
            slotGrab = true;
            g_slotDone = true;
            Buzz(1, 0.05f, 0.6f);
            static int logsLeft = 20;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: right hand squeezed at the right chest's item slot -> %s.", slotWhat);
            }
        }
    }
    g_rightGripDown = rightGripNow;
    if (holsterGrab)
    {
        // The hip is the revolver's. The gun button's tap draws the gun used last, which can be
        // the shotgun, so for the hip UpdateGunButton sends command 78 (the revolver's own) instead.
        g_virtualGunTapUntil = NowMs() + 120.0;
        g_virtualGunTapHip = true;
        // An item kept ready in the right hand is let go of for half a second, so the game is back in its normal
        // state to take the revolver: the body script waits for that up to a second (VRDoBodySlotWait).
        if (!gunMode && (HintCarriedFlags(NowMs()) & kCarryRevolver) != 0) g_shoulderQuietUntilMs = NowMs() + 500.0;

        static int logsLeft = 4;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: right hand squeezed at the holster -> %s.",
                !gunMode ? "the revolver is drawn" : (CameraShotgunHeld() ? "the revolver is taken out, the shotgun put away" : "the revolver is put away"));
        }
    }

    // How far apart the hands are, and how far the left hand is from the gun
    // (a point a little in front of the right palm).
    float handsApart = 10.0f, leftToGun = 10.0f;
    {
        HeadPose left, right;
        if (VrGetGripHandPose(0, left) && VrGetGripHandPose(1, right))
        {
            float d[3], g[3];
            for (int i = 0; i < 3; ++i)
            {
                d[i] = left.position[i] - right.position[i];
                // "Forward" for the right hand is its rotation's third column, negated.
                g[i] = left.position[i] - (right.position[i] - right.rotation[i * 3 + 2] * 0.08f);
            }
            handsApart = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            leftToGun = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        }
    }

    // What the game says is in each hand (bandage, grenade, flashlight).
    int rightKind = CameraRightItemKind(), leftKind = CameraLeftItemKind();
    if (rightKind != kItemGrenade) g_grenadeArmed = false;
    static double grenadeArmedAt = 0;
    static bool cordPulled = false;

    // The flashlight hangs on the left side of the chest: squeezing the left
    // grip there takes it out or puts it away.
    static bool leftGripWasDown = false;
    bool leftGripNow = grip[0] > (leftGripWasDown ? kRelease : kPress);
    // (While the revolver is open, the left hip is the ammo pouch: unused here,
    // since the flashlight is at the chest.)
    bool revolverOpen = gunMode && g_reloadKeyDown;
    // Not twice within half a second: a second squeeze right after the first
    // would put it straight back.
    static double lastLightGrabMs = 0;
    // Not when the left palm is at the other handle of a tool held in the
    // right hand (the bolt cutters, often held up by the chest): that squeeze
    // takes the handle.
    bool atToolHandle = !gunMode && rightKind == kItemOther && leftKind == kItemNone && LeftAtPumpNow();
    bool lightGrab = leftGripNow && !leftGripWasDown && g_leftAtChestLight && NowMs() - lastLightGrabMs > 500.0 && !atToolHandle;
    (void)revolverOpen;
    leftGripWasDown = leftGripNow;
    if (lightGrab)
    {
        lastLightGrabMs = NowMs();
        SendScriptCommand(5);
        static int logsLeft = 4;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: left hand squeezed at the left of the chest -> flashlight taken out / put away.");
        }
    }

    // --- The lighter in the right hand: lit while the right trigger is held
    // --- (script command 13), shut when it is let go (14). Its grip is the
    // --- hand's own squeeze, as an empty hand's.
    {
        static bool lighterLit = false;
        bool litWanted = rightKind == kItemLighter && trigger[1] > (lighterLit ? kRelease : kPress);
        if (litWanted != lighterLit)
        {
            lighterLit = litWanted;
            SendScriptCommand(litWanted ? 13 : 14);
            if (litWanted) Buzz(1, 0.03f, 0.4f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: right trigger %s with the lighter -> it %s.", litWanted ? "held" : "let go", litWanted ? "opens and lights" : "is shut");
            }
        }
    }
    // --- The game's own lighter in the left hand (the game puts it there
    // --- while a molotov is held ready, and the body
    // --- script says so with the left hand's kind): lit while the left
    // --- trigger is held (script command 16), shut when it is let go (17).
    // --- Its flame held to the molotov's cloth lights the cloth (the body
    // --- script's VRUpdateFlames). Its hand squeezes with its grip alone.
    {
        static bool lighterLit = false;
        bool litWanted = leftKind == kItemLighter && trigger[0] > (lighterLit ? kRelease : kPress);
        if (litWanted != lighterLit)
        {
            lighterLit = litWanted;
            SendScriptCommand(litWanted ? 16 : 17);
            if (litWanted) Buzz(0, 0.03f, 0.4f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: left trigger %s with the game's lighter -> it %s.", litWanted ? "held" : "let go", litWanted ? "opens and lights" : "is shut");
            }
        }
    }
    // --- The torch in the right hand: held out (the hand more than 45 cm
    // --- from the head, measured level; back within 38 cm ends it): script
    // --- commands 18 and 19; the body script brandishes it then if it burns
    // --- (VRUpdateBrandish: the game's own brandish, brighter, wider light,
    // --- its brandish sounds, rats threatened, burning faster). Waved, to
    // --- scare off the rats, by swinging it around in front of the player:
    // --- the hand, held out at least 25 cm from the head, goes at least 70
    // --- degrees around the head within 0.7 s, passing within 50 degrees of
    // --- where the head faces, at 1 m/s or more (command 60, the game's wave:
    // --- the rats run off); once in 0.8 s at most. Not the game's "ready" state, whose arm
    // --- animations move the torch in the hand (it is not kept ready, nor
    // --- made ready by X). Its hand squeezes with its trigger alone.
    {
        static bool heldOut = false;
        bool out = false;
        if (rightKind == kItemTorch)
        {
            HeadPose head, hand;
            if (VrGetHeadCentrePose(head) && VrGetGripHandPose(1, hand))
            {
                float dx = hand.position[0] - head.position[0], dz = hand.position[2] - head.position[2];
                out = sqrtf(dx * dx + dz * dz) > (heldOut ? 0.38f : 0.45f);
            }
            else
            {
                out = heldOut; // (not known for a moment: as it was)
            }
        }
        if (out != heldOut)
        {
            heldOut = out;
            SendScriptCommand(out ? 18 : 19);
            if (out) Buzz(1, 0.03f, 0.3f);
            static int logsLeft = 10;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: the torch %s.", out ? "is held out (brandished if it burns)" : "is brought back in");
            }
        }

        // The swing: where the hand has been around the head (its angle seen
        // from above, 0 straight ahead in the room, + to the right) in the
        // last moments, while held out.
        const int kSwingSamples = 128;           // (more than 0.7 s of frames at 90 a second)
        const float kSwingAngle = 70.0f * 3.14159265f / 180.0f, kSwingFront = 50.0f * 3.14159265f / 180.0f;
        const double kSwingWithinMs = 700.0, kWaveGapMs = 800.0;
        static float swingAngle[kSwingSamples];
        static bool swingInFront[kSwingSamples];
        static double swingMs[kSwingSamples];
        static int swingCount = 0, swingNewest = -1;
        static double lastWaveMs = -10000.0;
        HeadPose swingHead, swingHand;
        bool swingKnown = rightKind == kItemTorch && VrGetHeadCentrePose(swingHead) && VrGetGripHandPose(1, swingHand);
        float sx = swingKnown ? swingHand.position[0] - swingHead.position[0] : 0.0f;
        float sz = swingKnown ? swingHand.position[2] - swingHead.position[2] : 0.0f;
        if (!swingKnown || sqrtf(sx * sx + sz * sz) < 0.25f)
        {
            swingCount = 0; // (not held out: a swing starts afresh)
        }
        else
        {
            // Straight ahead of the head: -z turned by the head (row-major).
            float facing = atan2f(-swingHead.rotation[2], swingHead.rotation[8]);
            float angle = atan2f(sx, -sz);
            float fromFacing = angle - facing;
            while (fromFacing > 3.14159265f) fromFacing -= 6.2831853f;
            while (fromFacing < -3.14159265f) fromFacing += 6.2831853f;
            double now = NowMs();
            swingNewest = (swingNewest + 1) % kSwingSamples;
            swingAngle[swingNewest] = angle;
            swingInFront[swingNewest] = fabsf(fromFacing) < kSwingFront;
            swingMs[swingNewest] = now;
            if (swingCount < kSwingSamples) ++swingCount;

            // Back through the last 0.7 s: an angle 70 degrees from this one,
            // with the hand in front of the head somewhere between.
            bool front = false;
            float swept = 0.0f;
            double took = 0.0;
            for (int n = 0; n < swingCount; ++n)
            {
                int i = (swingNewest - n + kSwingSamples) % kSwingSamples;
                if (now - swingMs[i] > kSwingWithinMs) break;
                if (swingInFront[i]) front = true;
                float d = angle - swingAngle[i];
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                if (fabsf(d) > swept) { swept = fabsf(d); took = now - swingMs[i]; }
                if (front && swept >= kSwingAngle) break;
            }
            if (front && swept >= kSwingAngle && g_rightHandSpeed >= 1.0f && now - lastWaveMs > kWaveGapMs)
            {
                lastWaveMs = now;
                swingCount = 0; // (the next wave is a swing of its own)
                SendScriptCommand(60);
                Buzz(1, 0.05f, 0.5f);
                static int logsLeft = 10;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: the torch waved: swung %.0f degrees round the head in %.2f s, in front of it (the hand at %.1f m/s).",
                        swept * 180.0f / 3.14159265f, took / 1000.0, g_rightHandSpeed);
                }
            }
        }
    }

    // --- The hands-on items' shared moves. Which moves the item in the right
    // --- hand takes part in is told by the game's scripts, from their item
    // --- profiles (roll 7093, CameraRightItemMoves; BunkerVR_ItemMoves in
    // --- helper_bunkervr_items.hps): an item is given a move, and what the
    // --- move does with it is decided, there. This part only sees the moves
    // --- and sends them; the scripts act on them.
    // ---   Hold and let go, swing and throw (kMoveThrow; the bottle's, like
    // ---   the grenade's): a press of the right grip takes hold of the item
    // ---   to throw it (a small buzz);
    // ---   let go while the hand swings (at least the Feel page's "Throw: hand
    // ---   faster than") and it is thrown with the hand's speed (script
    // ---   command 7, as the grenade). Let go with the hand slower than that:
    // ---   put down where it is (command 15) if its profile says so
    // ---   (kMoveLetGoPutsDown: the meat and the rabbit doll), otherwise it
    // ---   stays in the hand (a bottle or a molotov would break at the
    // ---   player's feet). Its trigger squeezes the hand (see the loop below): the
    // ---   game's use of it on things (the fuel tap, the save lamp), never
    // ---   the game's own throw (PlayerState_UsingThrowable.hps).
    // --- (The grenades keep their own move, further down: the left hand
    // --- pulls the pin, then letting go of the right hand's squeeze throws.)
    // --- Every attempt is noted in the log (MOVES: ...), up to 200 a run.
    int rightMovesItem = -1;
    int rightMoves = gunMode ? 0 : CameraRightItemMoves(&rightMovesItem);
    {
        static bool gripWasDown = false;
        bool gripDown = grip[1] > (gripWasDown ? kRelease : kPress);
        bool gripPressed = gripDown && !gripWasDown;
        gripWasDown = gripDown;
        static int logsLeft = 200;
        const char* itemName = GripItemName(rightMovesItem);
        if (itemName[0] == '\0') itemName = "The item";

        if (!(rightMoves & kMoveThrow))
        {
            g_throwHeld = false; // (it went, or the gun is out: nothing to let go of)
        }
        else if (!g_throwHeld)
        {
            // Only a fresh press of the grip takes hold: not one still held as
            // the item came into the hand (from the inventory, say), and not
            // one at the holster (that draws the gun).
            if (gripPressed && !holsterGrab && !slotGrab && !CameraInventoryOpen()) // (nor one at the chest's item slot, which stores it)
            {
                g_throwHeld = true;
                Buzz(1, 0.02f, 0.3f);
            }
        }
        else if (!gripDown)
        {
            g_throwHeld = false;
            float needed = GetSettings().feelThrowSpeed;
            const char* outcome = "kept in the hand";
            if (g_rightHandSpeed >= needed)
            {
                SendScriptCommand(7);
                HintUsed("throw", NowMs());
                Buzz(1, 0.06f, 0.8f);
                outcome = "thrown";
            }
            else if (slotTakes)
            {
                // Let go of at the right chest's item slot (see slotGrab above): stored there.
                SendScriptCommand(80);
                g_shoulderQuietUntilMs = NowMs() + 500.0; // (the item kept ready let go of meanwhile, as for the shoulder: the game puts it away only out of its own state)
                g_slotDone = true;
                HintUsed("slot_let_go", NowMs());
                Buzz(1, 0.05f, 0.6f);
                outcome = "stored in the right chest's item slot";
            }
            else if (rightMoves & kMoveLetGoPutsDown)
            {
                SendScriptCommand(15);
                Buzz(1, 0.03f, 0.4f);
                outcome = "put down where it is";
            }
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("MOVES: %s in the right hand: the grip let go with the hand at %.1f m/s (a throw needs %.1f) -> %s.", itemName,
                    g_rightHandSpeed, needed, outcome);
            }
        }
    }

    // --- Pulling things to the hand (see UpdatePull).
    UpdatePull(grip, kPress, kRelease, gunMode, leftKind, rightKind, holsterGrab || shoulderGrab || slotGrab, lightGrab);

    for (int hand = 0; hand < 2; ++hand)
    {
        int other = 1 - hand;
        float squeezed = grip[hand] > trigger[hand] ? grip[hand] : trigger[hand];
        // (The lighter's hand squeezes with its grip alone, its trigger
        // working the lighter; the hand of an item thrown by hand with its
        // trigger alone, its grip throwing it: see above. The left hand with
        // the game's lighter too.)
        if (hand == 1 && rightKind == kItemLighter) squeezed = grip[1];
        if (hand == 0 && leftKind == kItemLighter) squeezed = grip[0];
        if (hand == 1 && rightKind == kItemTorch) squeezed = trigger[1]; // (the torch's hand squeezes with its trigger alone: see above)
        if (hand == 1 && (rightMoves & kMoveThrow)) squeezed = trigger[1];
        if (gunMode && hand == 1)
        {
            // The gun hand does not grab. The one thing its grip can still
            // do is pull the flashlight's cord.
            bool cord = leftKind == kItemFlashlight && (handsApart < 0.22f || g_handUse[1] == kHandCord);
            squeezed = cord ? grip[1] : 0.0f;
        }

        bool down = squeezed > (g_handDown[hand] ? kRelease : kPress);
        bool justDown = down && !g_handDown[hand];
        g_handDown[hand] = down;

        // The left hand on the shotgun's pump stays on it, even when let go
        // of, while the hand poses tool (F11) is open, so that its pose there
        // can be set with the controllers put down. With the tool shut, the
        // hand lets go if not squeezing.
        if (!down && hand == 0 && LeftPumpKeptForPoseTool()) continue;

        if (!down)
        {
            // Letting go of an armed grenade throws it, with whatever speed the
            // hand has (let go of with a still hand, it drops at the player's feet).
            if (hand == 1 && g_handUse[1] == kHandGrenade && g_grenadeArmed)
            {
                SendScriptCommand(7);
                HintUsed("throw", NowMs());
                g_grenadeArmed = false;
                Buzz(1, 0.06f, 0.8f);
                Log("VR: grenade thrown by letting go (the hand was moving at %.1f m/s).", g_rightHandSpeed);
            }
            // An unarmed grenade let go of at the right chest's item slot: stored there.
            else if (hand == 1 && g_handUse[1] == kHandGrenade && !g_grenadeArmed && slotTakes && g_rightHandSpeed < GetSettings().feelThrowSpeed)
            {
                SendScriptCommand(80);
                g_shoulderQuietUntilMs = NowMs() + 500.0; // (the item kept ready let go of meanwhile, as for the shoulder: the game puts it away only out of its own state)
                g_slotDone = true;
                HintUsed("slot_let_go", NowMs());
                Buzz(1, 0.05f, 0.6f);
                Log("VR: an unarmed grenade let go of at the right chest's item slot -> stored there.");
            }

            if (g_pendingHand == hand) g_pendingHand = -1;
            else if (g_handUse[hand] == kHandGame)
            {
                SetInteractHeld(false);

                // The other hand is on the door this hand held (see
                // VRUpdateHelpHand in PlayerState_Interact_SwingDoor.hps):
                // it gets the game's interact button, so that the game takes
                // the door up again in it, once the game is back in its normal
                // state and has looked at that hand.
                if (g_handUse[other] == kHandScript && g_handDown[other] && !gunMode && !g_fireHeld && !g_bulletTap &&
                    CameraTouchKind(other) == kTouchHeldDoor)
                {
                    g_handUse[other] = kHandGame;
                    g_pendingHand = other;
                    g_pendingFrames = 0;
                    g_pendingNeedNormal = true;
                    g_grabHand = other;
                    static int logsLeft = 20;
                    if (logsLeft > 0)
                    {
                        --logsLeft;
                        Log("VR: the %s hand lets go of the door the %s hand holds too: that hand takes the game's interact button, to keep the door.",
                            hand == 0 ? "left" : "right", other == 0 ? "left" : "right");
                    }
                }
            }
            g_handUse[hand] = kHandFree;
            continue;
        }
        if (!justDown) continue;

        if (hand == 0 && gunMode && CameraShotgunHeld() && leftKind == kItemNone && g_leftAtLightHolster && LeftTakesThingNotBullet())
        {
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: left hand squeezed in the ammo pouch's reach with the shotgun, %.0f cm from its spot, touching a thing: the thing, not a shell.", g_leftToPouch * 100.0f);
            }
        }

        if (hand == 1 && RightFittedNow())
        {
            // The right hand's item is fitted into what it is used on (a fuse
            // in its box): its squeeze is the fit's own (the fuse is twisted
            // while the grip is held), not the game's interact button, which
            // would use the item at once (the fuse used as the grip is pressed
            // to twist it) or take hold of the thing beside it.
            g_handUse[hand] = kHandNothing;
        }
        else if (hand == 1 && (holsterGrab || shoulderGrab || slotGrab || NowMs() < g_virtualGunTapUntil))
        {
            g_handUse[hand] = kHandNothing; // this squeeze was for the holster (or the shotgun behind the shoulder, or the chest's item slot)
        }
        else if (hand == 0 && lightGrab)
        {
            g_handUse[hand] = kHandNothing; // this squeeze was for the flashlight
        }
        else if (hand == 1 && leftKind == kItemFlashlight && handsApart < 0.22f)
        {
            g_handUse[hand] = kHandCord;
            cordPulled = false;
        }
        else if (hand == 0 && rightKind == kItemHealth && g_bandagePhase == 0 && handsApart < 0.22f)
        {
            g_handUse[hand] = kHandTin; // pulling the hands apart now takes the roll out (see below)
        }
        else if (hand == 0 && rightKind == kItemGrenade && handsApart < 0.22f)
        {
            g_handUse[hand] = kHandPin;
        }
        else if (hand == 1 && rightKind == kItemGrenade)
        {
            g_handUse[hand] = kHandGrenade;
        }
        else if (gunMode && hand == 0 && CameraShotgunHeld() && leftKind == kItemNone && LeftAtPumpNow())
        {
            // The shotgun: a left squeeze at its fore-end takes the
            // pump. The barrel is then aimed through the left hand, and the
            // pump slides with it (Prop_PlayerBody.hps, Prop_Shotgun.hps).
            g_handUse[hand] = kHandOnGun;
            Buzz(0, 0.03f, 0.4f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: left hand squeezed at the shotgun's fore-end -> it holds the pump.");
            }
        }
        else if (gunMode && hand == 0 && CameraShotgunHeld() && leftKind == kItemNone && g_leftAtLightHolster && !LeftTakesThingNotBullet())
        {
            // ... and at the ammo pouch (the left hip) a shell is taken out, with
            // no button: the shotgun is loaded at any time, a shell at a time,
            // at the loading port under it (Prop_Shotgun.hps).
            g_handUse[hand] = kHandBullet;
            Buzz(0, 0.04f, 0.5f);
            static int logsLeft = 12;
            if (logsLeft > 0)
            {
                --logsLeft;
                char where[128];
                PouchOffsetWords(where, sizeof(where));
                Log("VR: left hand squeezed at the ammo pouch (left hip) with the shotgun -> carrying a shell%s.", where);
            }
        }
        else if (gunMode && hand == 0 && CameraShotgunHeld() && handsApart < 0.22f)
        {
            // (Not the revolver's steadying hand beside the right one: the
            // shotgun's left hand belongs on its fore-end. Here the squeeze only
            // closes the hand.)
            g_handUse[hand] = kHandNothing;
        }
        else if (!gunMode && hand == 0 && rightKind == kItemOther && leftKind == kItemNone && LeftAtPumpNow())
        {
            // A tool held with both hands (the bolt cutters): a left
            // squeeze at its other handle takes it (the body script says the
            // hand is there, cVR_LeftAtPump, as the shotgun's script does for
            // its fore-end), held until let go; the handles open and shut with
            // the hands (VRUpdateCutters in PlayerBodyHandler.hps). Told to the
            // scripts as the left hand on the gun.
            g_handUse[hand] = kHandOnGun;
            Buzz(0, 0.03f, 0.4f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: left hand squeezed at the other handle of the tool in the right hand -> it holds that handle.");
            }
        }
        else if (gunMode && hand == 0 && g_reloadKeyDown && leftKind != kItemNone)
        {
            // The left hand already holds something (the flashlight): it
            // takes no bullet.
            g_handUse[hand] = kHandNothing;
        }
        else if (gunMode && hand == 0 && g_reloadKeyDown)
        {
            // The revolver is open: a squeeze at the ammo pouch, on the left
            // hip, takes a bullet out.
            if (g_leftAtLightHolster && !LeftTakesThingNotBullet())
            {
                g_handUse[hand] = kHandBullet;
                Buzz(0, 0.04f, 0.5f);
                static int logsLeft = 12;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    char where[128];
                    PouchOffsetWords(where, sizeof(where));
                    Log("VR: left hand squeezed at the ammo pouch (left hip) -> carrying a bullet%s.", where);
                }
            }
            else
            {
                g_handUse[hand] = kHandNothing;
                static int logsLeft = 6;
                if (g_leftAtLightHolster && logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: left hand squeezed in the ammo pouch's reach, %.0f cm from its spot, touching a thing: no bullet taken.", g_leftToPouch * 100.0f);
                }
            }
        }
        else if (gunMode && hand == 0 && handsApart < 0.22f && leftKind == kItemFlashlight)
        {
            // Holding the flashlight, the left hand does not take hold of the
            // gun: it would take the gun hand's turn, and the lamp in it with
            // it, so the flashlight would be drawn wrong. The squeeze only
            // closes the hand.
            g_handUse[hand] = kHandNothing;
        }
        else if (gunMode && hand == 0 && handsApart < 0.22f && (CameraTouchKind(0) == kTouchNothing || leftToGun < kOnGunTouching))
        {
            // A handle or a thing the left hand touches comes first, unless
            // it is right at the gun: otherwise, with the gun near, a squeeze
            // on a handle would steady the gun instead.
            g_handUse[hand] = kHandOnGun;
        }
        else if (g_handUse[other] == kHandGame && !gunMode && !g_fireHeld && !g_bulletTap && CameraTouchKind(hand) == kTouchGameOnly)
        {
            // The game's interact button goes to the hand that needs it, so
            // either hand can open doors, use levers, pick up and read things
            // even while the other carries something. This hand reaches for something only
            // the game can use (a door, a lever, a pick-up, a note, a crate for
            // both hands): the other hand lets go of the game's button, and what
            // the game's grab held in it passes to that hand's own hold (the
            // body script sees the grab in that same hand and takes the thing
            // over as soon as the grab lets go: VRUpdateFreeHand in
            // PlayerBodyHandler.hps). This hand gets the button once the game
            // is back in its normal state and has looked at this hand.
            GiveInteractButtonTo(hand);
        }
        else if (g_handUse[other] == kHandGame || g_fireHeld || g_bulletTap)
        {
            g_handUse[hand] = kHandScript; // the interact button is taken
            g_reachUntilMs[hand] = NowMs() + kReachWaitMs; // (see below)
        }
        else if (hand == 0 && (gunMode || rightKind == kItemOther || rightKind == kItemBottle) && CameraTouchKind(0) == kTouchNothing)
        {
            // The gun or an item is ready in the right hand, and this hand
            // touches nothing yet: the right hand keeps it ready, unless this
            // hand comes to something in a moment (see below). Otherwise any
            // squeeze would let go of the aim for a second or more.
            g_handUse[hand] = kHandReach;
            g_reachUntilMs[hand] = NowMs() + kReachWaitMs;
        }
        else
        {
            // The game grabs whatever it found at the pointing hand during its
            // last update. If the other hand was the pointing one until now,
            // give the game a few frames to look at this hand first.
            g_handUse[hand] = kHandGame;
            g_pendingHand = hand;
            g_pendingFrames = (g_grabHand == hand && !gunMode) ? 100 : 0;
            g_pendingNeedNormal = false;
            g_grabHand = hand;
        }
    }

    // --- Healing by hand: the only way to heal in the headset. A bandage (in
    // --- its tin) or a medkit is in the right hand. Take hold of it with the
    // --- left hand and pull the hands apart: the roll of bandage comes out.
    // --- Then wind it three times around the left forearm with the right
    // --- hand. Each turn ticks; the third heals. (The game's own use, the
    // --- trigger and its long animation, is shut off in the headset by its
    // --- script: PlayerState_UsingHealthItem OnUse.)
    {
        const int turnsNeeded = 3;

        // The winding's count: the angle the right hand has gone around the
        // forearm, added up, and the turns told so far. Every pull out of the
        // tin starts it from nothing, and so does the bandage leaving the hand,
        // so a bandage put away while being wound doesn't pass its turns on to
        // the next one.
        static float lastAngle = 0.0f, travelled = 0.0f;
        static bool haveAngle = false;
        static double awaySince = 0;
        static int turnsDone = 0;
        auto windFromNothing = []() { lastAngle = 0.0f; travelled = 0.0f; haveAngle = false; awaySince = 0; turnsDone = 0; };

        if (rightKind != kItemHealth)
        {
            if (g_bandagePhase == 1 && travelled != 0.0f)
                Log("VR: bandage left the hand %.2f turns round the forearm: the next one starts from nothing.", fabsf(travelled) / 6.28318f);
            g_bandagePhase = 0;
            g_bandageTurns = 0.0f;
            windFromNothing();
            if (g_handUse[0] == kHandTin) g_handUse[0] = kHandNothing;
        }
        else if (g_bandagePhase == 0)
        {
            // The pull that opens it.
            if (g_handUse[0] == kHandTin && handsApart > 0.30f)
            {
                g_bandagePhase = 1;
                g_bandageTurns = 0.0f;
                windFromNothing();
                g_handUse[0] = kHandNothing;
                SendScriptCommand(10); // the script plays the sound
                HintUsed("bandage_open", NowMs());
                Buzz(0, 0.08f, 0.8f);
                Buzz(1, 0.05f, 0.5f);
                Log("VR: bandage pulled out of its tin. Wind it round the left forearm (%d times to heal).", turnsNeeded);
            }
        }
        else if (g_bandagePhase == 1)
        {
            // Winding. The left forearm runs back from the left hand along the
            // controller's "back" direction. The right hand's place around that
            // line is followed as an angle; the angle travelled, added up, is
            // how far the bandage has been wound.

            HeadPose left, right;
            bool atArm = false;
            if (VrGetGripHandPose(0, left) && VrGetGripHandPose(1, right))
            {
                float scale = GetSettings().worldScale; // hand positions are measured in the game's meters
                float axis[3] = { left.rotation[2], left.rotation[5], left.rotation[8] };  // back along the forearm
                float side[3] = { left.rotation[0], left.rotation[3], left.rotation[6] };  // the controller's right
                float d[3], along = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    d[i] = right.position[i] - (left.position[i] + axis[i] * 0.14f * scale);
                    along += d[i] * axis[i];
                }
                float out[3], outLength = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    out[i] = d[i] - axis[i] * along;
                    outLength += out[i] * out[i];
                }
                outLength = sqrtf(outLength);

                // Close enough to the forearm to count: within 22 cm along it
                // and between 3 and 25 cm out from it.
                atArm = fabsf(along) < 0.22f * scale && outLength > 0.03f * scale && outLength < 0.25f * scale;
                if (atArm)
                {
                    // Two directions across the forearm to measure the angle against.
                    float u[3], uLength = 0.0f, sideAlong = side[0] * axis[0] + side[1] * axis[1] + side[2] * axis[2];
                    for (int i = 0; i < 3; ++i)
                    {
                        u[i] = side[i] - axis[i] * sideAlong;
                        uLength += u[i] * u[i];
                    }
                    uLength = sqrtf(uLength);
                    if (uLength > 0.01f)
                    {
                        for (int i = 0; i < 3; ++i) u[i] /= uLength;
                        float v[3] = { axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0] };
                        float angle = atan2f(out[0] * v[0] + out[1] * v[1] + out[2] * v[2], out[0] * u[0] + out[1] * u[1] + out[2] * u[2]);
                        if (haveAngle)
                        {
                            float step = angle - lastAngle;
                            if (step > 3.14159f) step -= 6.28318f;
                            if (step < -3.14159f) step += 6.28318f;
                            if (fabsf(step) < 1.2f) travelled += step; // a bigger jump is a tracking slip
                        }
                        lastAngle = angle;
                        haveAngle = true;
                    }
                }
            }

            // Taking the hand away for more than a second starts the winding again.
            double nowMs = NowMs();
            if (atArm) awaySince = 0;
            else
            {
                haveAngle = false;
                if (awaySince == 0) awaySince = nowMs;
                if (nowMs - awaySince > 1000.0 && travelled != 0.0f)
                {
                    travelled = 0.0f;
                    turnsDone = 0;
                    Log("VR: bandage: the hand left the forearm; the winding starts again.");
                }
            }

            g_bandageTurns = fabsf(travelled) / 6.28318f;
            if ((int)g_bandageTurns > turnsDone)
            {
                turnsDone = (int)g_bandageTurns;
                Buzz(1, 0.06f, 0.7f);
                Buzz(0, 0.04f, 0.4f);
                if (turnsDone < turnsNeeded) SendScriptCommand(11); // one turn done: the script plays a sound
                if (turnsDone == 1) HintUsed("bandage_wind", NowMs()); // (once a bandage)
                Log("VR: bandage wound %d time%s round the forearm.", turnsDone, turnsDone == 1 ? "" : "s");
            }
            if (turnsDone >= turnsNeeded)
            {
                SendScriptCommand(4); // use it: the health item's own script heals and takes it away
                Buzz(1, 0.30f, 1.0f);
                Buzz(0, 0.20f, 0.8f);
                g_bandagePhase = 2;
                g_bandageDoneAtMs = NowMs();
                windFromNothing();
                Log("VR: bandage wound %d time(s) -> used.", turnsNeeded);
            }
        }
        else if (NowMs() - g_bandageDoneAtMs > 1500.0)
        {
            // Still in the hand a while after being used: the game did not
            // take it (full health, perhaps). It can be wound again, from
            // nothing.
            g_bandagePhase = 1;
            g_bandageTurns = 0.0f;
            windFromNothing();
        }
    }

    // --- The flashlight's cord: take hold next to the flashlight and pull the
    // --- hands apart. Each pull charges it once; bring the hands back
    // --- together (still squeezing) for the next pull.
    if (g_handUse[1] == kHandCord)
    {
        if (leftKind != kItemFlashlight)
        {
            g_handUse[1] = kHandNothing;
        }
        else if (!cordPulled && handsApart > 0.38f)
        {
            cordPulled = true;
            SendScriptCommand(6);
            Buzz(1, 0.05f, 0.7f);
            static int logsLeft = 6;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: flashlight cord pulled.");
            }
        }
        else if (cordPulled && handsApart < 0.24f)
        {
            cordPulled = false;
        }
    }

    // --- The grenade's pin: take hold next to the grenade with the left hand
    // --- and pull the hands apart.
    if (g_handUse[0] == kHandPin)
    {
        if (rightKind != kItemGrenade)
        {
            g_handUse[0] = kHandNothing;
        }
        else if (handsApart > 0.30f)
        {
            if (!g_grenadeArmed)
            {
                SendScriptCommand(8); // the script takes the pin off the grenade and plays its sound
                Buzz(0, 0.10f, 1.0f);
                Buzz(1, 0.05f, 0.5f);
                grenadeArmedAt = NowMs();
            }
            g_grenadeArmed = true;
            g_handUse[0] = kHandNothing;
            HintUsed("pull_pin", NowMs());
            Log("VR: grenade pin pulled. Throw it: hold the right grip, swing and let go.");
        }
    }

    // An armed grenade is thrown only by holding the right grip and letting
    // go (see the top of the loop above), never by a swing of the arm alone,
    // so grenadeArmedAt isn't used.
    (void)grenadeArmedAt;

    // A squeeze made a moment before the hand touches anything still counts,
    // so handles and items are easy to grab while the other hand holds
    // something. With the gun or an item ready in the right hand,
    // the first thing the left hand comes to is taken as if the squeeze had
    // come then. With something carried in the other hand, a thing only the
    // game can use takes the game's button from it as a squeeze on it would.
    if (g_handUse[0] == kHandReach)
    {
        if (CameraTouchKind(0) != kTouchNothing)
        {
            g_handUse[0] = kHandGame;
            g_pendingHand = 0;
            g_pendingFrames = 0;
            g_pendingNeedNormal = false;
            g_grabHand = 0;
        }
        else if (NowMs() > g_reachUntilMs[0])
        {
            g_handUse[0] = kHandNothing;
        }
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        if (g_handUse[hand] == kHandScript && g_handDown[hand] && NowMs() < g_reachUntilMs[hand] && g_handUse[1 - hand] == kHandGame &&
            !gunMode && !g_fireHeld && !g_bulletTap && CameraTouchKind(hand) == kTouchGameOnly)
        {
            GiveInteractButtonTo(hand);
        }
    }

    // Aiming is let go while the left hand uses (or waits for) the interact button.
    // (The same goes for any other item held ready in the right hand.)
    bool itemReady = !gunMode && (CameraRightItemKind() == kItemOther || CameraRightItemKind() == kItemBottle);
    g_aimPaused = (gunMode || itemReady) && g_handUse[0] == kHandGame;

    // (How long a squeeze waited, in the log. The scripts leave the gun's or
    // the item's state at once, see VrOtherHandWaits, and the wait for the
    // game to look at this hand is short, two of its updates.)
    static double pendingSinceMs = 0.0;
    if (g_pendingHand >= 0)
    {
        if (pendingSinceMs == 0.0) pendingSinceMs = NowMs();
        // With a gun out, the game must first be back in its normal state.
        // (Only the left hand waits for that: the right hand's squeeze is what uses a readied item.)
        // (The same after the hands swapped the game's button.)
        bool waitForNormal = ((gunMode || itemReady) && g_pendingHand == 0) || g_pendingNeedNormal;
        if (!waitForNormal || CameraPlayerStateNormal()) ++g_pendingFrames;
        if (g_pendingFrames > 3)
        {
            SetInteractHeld(true);
            if (g_grabHeld)
            {
                static int logsLeft = 40;
                if (waitForNormal && logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: the %s hand's squeeze reached the game %.0f ms after the other hand's %s let it.", g_pendingHand == 0 ? "left" : "right",
                        NowMs() - pendingSinceMs, gunMode ? "gun" : (itemReady ? "item" : "carrying"));
                }
                g_pendingHand = -1;
                g_pendingNeedNormal = false;
            }
        }
    }
    else
    {
        pendingSinceMs = 0.0;
    }

    // --- Firing.
    bool mayFire = gunMode && !g_reloadKeyDown && !g_bulletTap && g_handUse[0] != kHandGame;
    bool wantFire = mayFire && trigger[1] > (g_fireHeld ? kRelease : kPress);
    if (wantFire != g_fireHeld)
    {
        if (wantFire)
        {
            SetInteractHeld(true);
            g_fireHeld = g_grabHeld;
        }
        else
        {
            SetInteractHeld(false);
            g_fireHeld = false;
        }
    }

    // --- Loading bullets by hand, while the ammo check is held open: a bullet
    // --- taken at the ammo pouch (above), carried to the gun, loads it.
    //
    // For the revolver its own script does the loading (Prop_Gun.hps): it
    // sees the bullet reach the cylinder, puts it in at once and says so. It
    // also tips the spent shells out when the open gun is held muzzle-up.
    // For a gun whose script doesn't report, a squeezed left hand away from
    // the gun is carrying a bullet, and touching the gun with it clicks the
    // interact button once to load it.
    bool byScript = CameraRevolverScriptActive();
    int happened = CameraTakeRevolverEvent();
    if (happened == 1)
    {
        // A bullet went in: it has left the hand.
        if (g_handUse[0] == kHandBullet) g_handUse[0] = kHandNothing;
        Buzz(0, 0.06f, 0.9f);
        Buzz(1, 0.04f, 0.5f);
    }
    else if (happened == 2)
    {
        Buzz(1, 0.12f, 0.7f); // the shells fell out
        g_shellsOutAtMs = NowMs();
        HintUsed("gun_tip", g_shellsOutAtMs);
    }
    else if (happened == 3)
    {
        Buzz(0, 0.03f, 0.3f); // refused: a light tap
    }

    if (gunMode && g_reloadKeyDown && g_handDown[0])
    {
        if (g_handUse[0] == kHandNothing && leftToGun > 0.24f && !byScript && happened != 1 &&
            leftKind == kItemNone) // (not with something already in the left hand, as above)
            g_handUse[0] = kHandBullet;
        else if (g_handUse[0] == kHandBullet && leftToGun < 0.15f && !g_grabHeld && !byScript)
        {
            SetInteractHeld(true);
            g_bulletTap = g_grabHeld;
            g_bulletTapUntil = NowMs() + 90.0;
            g_handUse[0] = kHandNothing;

            static int logsLeft = 3;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: left hand touched the gun with a bullet -> one click of the game's interact button.");
            }
        }
    }
    else if (g_handUse[0] == kHandBullet && !(gunMode && CameraShotgunHeld() && g_handDown[0]))
    {
        // (A shell for the shotgun is carried as long as the hand squeezes:
        // it needs no ammo check held open.)
        g_handUse[0] = kHandNothing;
    }
    // The shotgun: to make shells easier to grab, a left hand
    // that still squeezes after pushing a shell in takes the next one as it
    // comes back into the pouch, without letting go. (Only coming into it: a
    // hand that loaded inside the pouch's reach does not take another there.)
    static bool leftWasAtPouch = false;
    if (gunMode && CameraShotgunHeld() && g_handDown[0] && g_handUse[0] == kHandNothing && g_leftAtLightHolster && !leftWasAtPouch &&
        leftKind == kItemNone && happened != 1 && !LeftTakesThingNotBullet())
    {
        g_handUse[0] = kHandBullet;
        Buzz(0, 0.04f, 0.5f);
        static int logsLeft = 6;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: left hand came back to the ammo pouch still squeezing, with the shotgun -> carrying the next shell.");
        }
    }
    leftWasAtPouch = g_leftAtLightHolster;
    if (g_bulletTap && NowMs() > g_bulletTapUntil)
    {
        SetInteractHeld(false);
        g_bulletTap = false;
    }
}

// ---------------------------------------------------------------------------
// Walking and turning with the thumbsticks
// ---------------------------------------------------------------------------
// The game is driven with W, A, S, D and the mouse, so the sticks press those
// keys and move the mouse for you. This assumes the game's default key layout.

static bool GameWindowIsInUse()
{
    HWND gameWindow = WindowFromDC(wglGetCurrentDC());
    return gameWindow != nullptr && GetForegroundWindow() == gameWindow;
}

// Is Steam's overlay (its own key: Shift+Tab) open over the game, taking the
// mouse or the keyboard? Asks Steam's overlay library, which Steam loads into
// the game itself (GameOverlayRenderer64.dll: SteamOverlayIsUsingMouse and
// SteamOverlayIsUsingKeyboard take no arguments and return a bool). The
// overlay can't be seen in the headset, it takes the controllers' keys, and
// with the PC's mouse kept out of the game it can't be closed, so while it is
// open the controllers' keys are held back and the PC's keyboard and mouse
// work. Sprint (left Shift) plus inventory (Tab) can open it by accident: see
// kShiftTabApartMs. Asked once a frame (UpdatePcInputBlock).
static bool SteamOverlayInUse()
{
    typedef bool (*OverlayAskFunction)();
    static HMODULE overlay = nullptr;
    static OverlayAskFunction usingMouse = nullptr, usingKeyboard = nullptr;
    static unsigned long long lookedFor = 0;
    if (overlay == nullptr)
    {
        unsigned long long now = GetTickCount64();
        if (lookedFor != 0 && now - lookedFor < 2000) return false; // (looked for again every 2 s: Steam loads it into the game on its own)
        lookedFor = now;
        overlay = GetModuleHandleW(L"GameOverlayRenderer64.dll");
        if (overlay == nullptr) return false;
        usingMouse = (OverlayAskFunction)GetProcAddress(overlay, "SteamOverlayIsUsingMouse");
        usingKeyboard = (OverlayAskFunction)GetProcAddress(overlay, "SteamOverlayIsUsingKeyboard");
        Log("VR: Steam's overlay is in the game (%s); while it is open the controllers' keys are held back and the PC's keyboard and mouse work.",
            usingMouse || usingKeyboard ? "it can be asked whether it is open" : "but it cannot be asked whether it is open");
    }
    return (usingMouse != nullptr && usingMouse()) || (usingKeyboard != nullptr && usingKeyboard());
}

// When the mod last pressed or released left Shift and Tab (the time, and the
// count of input updates), for keeping the two apart (see ShiftMayGoDown
// below). The input is updated once a headset frame, from within the game's
// own frame, in every state (paused too, unlike g_framesShown, which waits for
// new pictures).
static double g_shiftChangedMs = -1.0e9, g_tabChangedMs = -1.0e9;
static unsigned long long g_inputUpdates = 0, g_shiftChangedFrame = 0, g_tabChangedFrame = 0;

// Stand-in mode: an automated test run with a stand-in OpenXR runtime instead
// of a headset, and nobody at the PC. It only happens when the test runner has
// left its note file (see Setup): the mod then uses the stand-in runtime as its
// VR software, skips the dedication, presses nothing into another window and
// takes commands from a file (UpdateStandInCommands).
static bool g_standIn = false;
static bool g_standInFrontTest = false; // (command "front 1": keep the game's window in front anyway, UpdateGameFront)

static bool StandInWindowIsElsewhere()
{
    return g_standIn && GetForegroundWindow() != WindowFromDC(wglGetCurrentDC());
}

// Presses or releases a key, named by its position on the keyboard (which is
// what the game reads).
static void SendScanCode(WORD scanCode, bool down)
{
    if (down && StandInWindowIsElsewhere()) return;

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendModInput(input);

    if (scanCode == 0x2A)
    {
        g_shiftChangedMs = NowMs();
        g_shiftChangedFrame = g_inputUpdates;
    }
    else if (scanCode == 0x0F)
    {
        g_tabChangedMs = NowMs();
        g_tabChangedFrame = g_inputUpdates;
    }
}

// Presses or releases one of the four walking keys (0 = W, 1 = A, 2 = S, 3 = D).
static void SetMoveKey(int key, bool down)
{
    if (g_moveKeyDown[key] == down) return;
    if (down && g_steamOverlayOpen) return; // (nothing pressed into Steam's overlay; releases always go through)

    static const WORD scanCodes[4] = { 0x11, 0x1E, 0x1F, 0x20 };
    SendScanCode(scanCodes[key], down);
    g_moveKeyDown[key] = down;
}

// Presses or releases one of the other keys the controllers stand in for.
enum { kKeyCrouch = 0, kKeySprint = 1, kKeyMenu = 2, kKeyHolster = 3, kKeyInventory = 4, kKeyJump = 5, kKeyCombine = 6, kKeyDrop = 7, kExtraKeyCount = 8 };
static bool g_extraKeyDown[kExtraKeyCount] = { false, false, false, false, false, false, false, false };

static void SetExtraKey(int key, bool down)
{
    if (g_extraKeyDown[key] == down) return;
    if (down && g_steamOverlayOpen) return; // (nothing pressed into Steam's overlay; releases always go through)

    static const WORD scanCodes[kExtraKeyCount] = { 0x1D, 0x2A, 0x01, 0x13, 0x0F, 0x39, 0x39, 0x2D }; // left Ctrl, left Shift, Escape, R, Tab, Space, Space, X
    static const char* const names[kExtraKeyCount] = { "crouch (left Ctrl)", "sprint (left Shift)", "menu (Escape)",
                                                       "gun (R)", "inventory (Tab)", "jump (Space)",
                                                       "combine in the inventory (Space)", "drop from the inventory (X)" };
    SendScanCode(scanCodes[key], down);
    g_extraKeyDown[key] = down;

    static bool logged[kExtraKeyCount] = { false, false, false, false, false, false, false, false };
    if (down && !logged[key])
    {
        logged[key] = true;
        Log("VR: controller button pressed -> the game's %s key was sent.", names[key]);
    }
}

// Steam's overlay opens on its own key, Shift+Tab, and the mod presses left
// Shift for the sprint and Tab for the inventory, so Steam must never see the
// two together. Releasing Shift right before pressing Tab is not enough.
// Steam's overlay library (GameOverlayRenderer64.dll) does not take Tab from
// the game's key messages. Once a frame, as the picture is shown, it asks
// Windows whether Tab is down, and when Tab has just gone down or up it posts
// the game's window a message of its own. Handling that, it takes Shift as
// held if the game's window, by the key messages it has read so far, still
// has Shift down. Windows hands a window the messages posted to it before the
// keys pressed for it, so a Shift released in the same frame as Tab went down
// still counts as held there, and the overlay opens. It also checks when
// Shift goes down while it still takes Tab as held. So each of the two goes
// down only once the other has been up a while (kShiftTabApartMs and 3 input
// updates), long enough for the game to have read it: B pressed while
// sprinting opens the inventory 0.15 s later, and a sprint started right
// after B waits as long.
static const double kShiftTabApartMs = 150.0;
static const double kInventoryTabMostMs = 150.0; // B's Tab held at most this long (Tab held 0.4 s is the game's health check)
static double g_inventoryWaitingMs = -1.0; // when B was pressed while its Tab waits (below 0: no Tab waiting)
static bool g_inventoryButtonWas = false;  // B as last seen by the inventory (UpdateMoveInput)

static bool KeyUpLongEnough(double changedMs, unsigned long long changedFrame)
{
    return NowMs() - changedMs >= kShiftTabApartMs && g_inputUpdates >= changedFrame + 3;
}

// May the sprint's left Shift go down now? Not while Tab is down or waits to
// go down, nor until Tab has been up long enough.
static bool ShiftMayGoDown()
{
    return !g_extraKeyDown[kKeyInventory] && g_inventoryWaitingMs < 0.0 && KeyUpLongEnough(g_tabChangedMs, g_tabChangedFrame);
}

// May the inventory's Tab go down now? Not while the sprint's Shift is down,
// nor until it has been up long enough.
static bool TabMayGoDown()
{
    return !g_extraKeyDown[kKeySprint] && KeyUpLongEnough(g_shiftChangedMs, g_shiftChangedFrame);
}

// Closing Steam's overlay with B: Steam's own Shift+Tab, pressed as a person
// presses it, one step every kShiftTabApartMs: left Shift down, Tab down, Tab
// up, Shift up. Sent all at once, Tab is down and up again before Steam's
// overlay asks about it (once a frame, see above), so it stays open. Called
// every headset frame; if the game's window has left the front at a step, the
// remaining keys are released instead of pressed.
static int g_overlayCloseStep = 0; // 0 none; 1 Shift down; 2 Tab down too; 3 Tab let go
static double g_overlayCloseStepMs = 0.0;

static void UpdateOverlayClose()
{
    if (g_overlayCloseStep == 0 || NowMs() - g_overlayCloseStepMs < kShiftTabApartMs) return;
    bool inFront = GameWindowIsInUse();
    g_overlayCloseStepMs = NowMs();
    if (g_overlayCloseStep == 1 && inFront)
    {
        SendScanCode(0x0F, true); // Tab, with Shift held for a while
        g_overlayCloseStep = 2;
        return;
    }
    if (g_overlayCloseStep == 2)
    {
        SendScanCode(0x0F, false);
        if (inFront)
        {
            g_overlayCloseStep = 3;
            return;
        }
    }
    SendScanCode(0x2A, false); // Shift let go last
    g_overlayCloseStep = 0;
    Log("VR: Steam's overlay: the Shift+Tab to close it is done (%s).", inFront ? "pressed in full" : "let go early: the game's window is no longer in front");
}

// Presses or releases the right mouse button, which the game uses to aim a
// drawn gun (and to throw, use an item or cancel).
static void SetRightMouse(bool down)
{
    if (g_rightMouseDown == down) return;
    if (down && (g_steamOverlayOpen || StandInWindowIsElsewhere())) return; // (no clicks into Steam's overlay)
    // (Nor into another program, nor on a window over the game's, as in SetInteractHeld.)
    HWND gameWindow = WindowFromDC(wglGetCurrentDC());
    if (down && (gameWindow == nullptr || GetForegroundWindow() != gameWindow || !ClickLandsOnGame(gameWindow, "right click (aiming)")))
        return;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    SendModInput(input);
    g_rightMouseDown = down;

    static bool logged = false;
    if (down && !logged)
    {
        logged = true;
        Log("VR: controller button pressed -> the game's right mouse button (aim) was sent.");
    }
}

static bool ReadButton(XrAction action);

// The A button on the right controller and the gun (see UpdateGunButton):
//   no gun out:  a tap draws it (with "A button" chosen for drawing).
//   gun out:     a tap puts it away (the same); A held and the gun swung opens
//                the revolver, which stays open until it is swung shut.
// While the gun is out this mod keeps the game in its "aiming" state so the
// trigger fires at once. The game only listens to the gun key (R) outside that
// state, so while the revolver is open the aim is let go of and R is held down.
// A quick turn of the right wrist (a "swing"), worked out in DrawHands from how
// fast the controller is turning.
static bool g_rightSwing = false;
static bool g_rightSwingAtOnce = false; // the same, but true from the first fast frame
static float g_rightSwingSpeed = 0.0f; // radians a second
static float g_gunTipUp = 0.0f;        // how far the right controller points upwards, -1 to 1
static float g_rightSwingAxis[3] = { 0, 0, 0 }; // which way the right wrist is turning, in the controller's own directions

// Things the game's body script is asked to do straight away (it asks for
// them with roll 7016): 1 = draw the gun, 2 = put it away, 3 = open the
// revolver. Doing these in the script is much quicker than pressing the
// game's keys for it, which only work in some of its states and after delays.
//   4 = use the bandage or medkit in the right hand on yourself
//   5 = take out / put away the flashlight      6 = one pull of its cord
//   7 = throw the grenade in the right hand (the script works out how fast
//       from where the hand has been; also any item whose profile has the
//       throw move, see UpdateGrabInput)
//   8 = the grenade's pin was pulled (the script takes it off the model)
//  15 = the right grip let go of an item to throw with the hand still: put
//       it down where it is
// A few can wait in line; one that has waited over half a second is dropped.
static int g_scriptCommands[4] = { 0, 0, 0, 0 };
static unsigned long long g_scriptCommandTimes[4] = { 0, 0, 0, 0 };

static void SendScriptCommand(int command)
{
    for (int i = 0; i < 4; ++i)
    {
        if (g_scriptCommands[i] != 0) continue;
        g_scriptCommands[i] = command;
        g_scriptCommandTimes[i] = GetTickCount64();
        return;
    }
}

int VrTakeScriptCommand()
{
    int command = g_scriptCommands[0];
    unsigned long long time = g_scriptCommandTimes[0];
    for (int i = 0; i < 3; ++i)
    {
        g_scriptCommands[i] = g_scriptCommands[i + 1];
        g_scriptCommandTimes[i] = g_scriptCommandTimes[i + 1];
    }
    g_scriptCommands[3] = 0;
    if (command == 0 || GetTickCount64() - time > 500) return 0;
    return command;
}

void VrJumpArrived()
{
    if (TestJumpPart(30)[0] != '\0') SendScriptCommand(61); // (VRJumpMakeThings)
    if (TestJumpDoCommand() != 0) SendScriptCommand(TestJumpDoCommand());
    TestJumpFinished();
    Buzz(0, 0.05f, 0.4f);
    Buzz(1, 0.05f, 0.4f);
    Log("TEST LIST: the jump has arrived.");
}

// A mark: the player flags a moment that felt wrong. Both controllers buzz; the
// mod notes it in its log with its timings, and the game's scripts write what
// they kept of the last few seconds (held things, the turn) into theirs, with
// the same number (command 1000 + the mark's number; see VRMark in
// PlayerBodyHandler.hps). The last two minutes of the head, hands and buttons
// are saved with it (motion.h). Returns the mark's number and the name its
// files start with.
static int MakeMark(const char* why, char* prefix, int prefixSize)
{
    static int marks = 0;
    ++marks;
    SendScriptCommand(1000 + marks);
    Buzz(0, 0.08f, 0.6f);
    Buzz(1, 0.08f, 0.6f);
    Log("MARK %d (%s): the game's log has the same number.", marks, why);
    VrLogTimings();
    FrameTimingsAgain(); // (and every 10 seconds for the next minute)
    MotionFilePrefix(prefix, prefixSize, marks);
    MotionSave(prefix, why, marks, BUNKERVR_VERSION_HUNDREDTHS);
    return marks;
}

// The gun's controls.
//   Drawing and putting away: squeeze the right grip at the holster (or X puts
//     it away): a tap of the gun's controls (g_virtualGunTapUntil).
//   Opening the revolver: hold A and swing the gun. It stays open, with the
//     game's "check ammo" key held down for you, until you swing it shut.
static void UpdateGunButton(bool gunMode)
{
    enum { kIdle, kOpen, kSettling };
    static int phase = kIdle;
    static bool swingArmed = true;
    static double keyDownAt = 0, doneAt = 0, gunGoneSince = 0;
    static float openAxis[3] = { 0, 0, 0 }; // which way the wrist turned in the flick that opened the revolver

    double now = NowMs();
    bool button = ReadButton(g_gunAction);
    bool holsterTap = now < g_virtualGunTapUntil;

    // One swing counts once: the wrist has to slow down before the next.
    // To open (A is held, which already rules out accidents) the first fast
    // frame is enough; to close it takes two in a row.
    bool swing = g_rightSwing && swingArmed;
    bool swingAtOnce = g_rightSwingAtOnce && swingArmed;
    bool opening = phase == kIdle && gunMode && button;
    if (g_rightSwing || (opening && g_rightSwingAtOnce)) swingArmed = false;
    else if (g_rightSwingSpeed < 2.5f) swingArmed = true;

    // The shotgun's press-check (see g_shotgunCheckHeld): A held with it out.
    {
        bool checkHeld = gunMode && CameraShotgunHeld() && button;
        if (checkHeld != g_shotgunCheckHeld)
        {
            static int logsLeft = 8;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: A %s with the shotgun out: %s.", checkHeld ? "held" : "let go",
                    checkHeld ? "its pump draws back partway to show the shells (a press-check), throwing nothing out" : "the pump works as before");
            }
        }
        g_shotgunCheckHeld = checkHeld;
    }

    switch (phase)
    {
    case kIdle:
        if (!gunMode)
        {
            if (holsterTap)
            {
                SendScriptCommand(g_virtualGunTapHip ? 78 : 1); // draw (at the hip: the revolver; else the gun used last)
                g_virtualGunTapUntil = 0;
                g_virtualGunTapHip = false;
                doneAt = now;
                phase = kSettling;
            }
        }
        else if (button && swingAtOnce && !CameraShotgunHeld())
        {
            // A held and the gun swung: open the revolver (not the shotgun: it
            // is loaded at any time, by hand, and the game's own ammo check
            // would put it in the left hand). The key goes down now and stays
            // down; the script puts the game into its reload state at once.
            SetExtraKey(kKeyHolster, true);
            SendScriptCommand(3);
            HintUsed("gun_open", now);
            keyDownAt = now;
            gunGoneSince = 0;
            phase = kOpen;
            for (int i = 0; i < 3; ++i) openAxis[i] = g_rightSwingAxis[i];
            Log("VR: gun swung with A held (%.1f rad/s, turning about %+.2f %+.2f %+.2f) -> opening the revolver.",
                g_rightSwingSpeed, openAxis[0], openAxis[1], openAxis[2]);
        }
        else if (holsterTap)
        {
            // (At the hip with the shotgun out, the revolver is taken out in its place,
            // as behind the shoulder the shotgun takes the revolver's.)
            SendScriptCommand(g_virtualGunTapHip && CameraShotgunHeld() ? 78 : 2); // put away
            g_virtualGunTapUntil = 0;
            g_virtualGunTapHip = false;
            doneAt = now;
            phase = kSettling;
        }
        break;

    case kOpen: // the revolver is open: the key stays down until it is swung shut
    {
        if (gunMode) gunGoneSince = 0;
        else if (gunGoneSince == 0) gunGoneSince = now;
        bool gunGone = gunGoneSince != 0 && now - gunGoneSince > 800;

        // Shutting it takes a flick the other way from the one that opened it,
        // so tipping the muzzle up to empty the shells, or reaching for a
        // bullet, doesn't shut it by accident. It also stays open while the
        // muzzle is tipped up and while the left hand carries a bullet. (With
        // A held, any flick shuts it.)
        bool flickBack = false;
        if (now - keyDownAt > 600 && swing)
        {
            float against = -(g_rightSwingAxis[0] * openAxis[0] + g_rightSwingAxis[1] * openAxis[1] + g_rightSwingAxis[2] * openAxis[2]);

            // What shuts it: a roll of the wrist to the left, which the controller feels as
            // a turn about its "back" direction (the third number, positive).
            // That is the flick that throws a real cylinder back in. If the
            // opening flick was not a roll at all, a flick against it will do.
            bool rollLeft = g_rightSwingAxis[2] > 0.45f;
            bool againstOpening = fabsf(openAxis[2]) < 0.5f && against > 0.5f;

            // The muzzle being up only holds it open while there are spent
            // shells to tip out (and for the moment they take to fall): once
            // they are out, it can be flicked shut straight from there.
            bool shellsBusy = g_gunTipUp > 0.45f && (CameraRevolverHasSpent() || now - g_shellsOutAtMs < 500.0);
            bool busy = shellsBusy || g_handUse[0] == kHandBullet;
            flickBack = button || ((rollLeft || againstOpening) && !busy);

            static int logsLeft = 20;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: fast turn of the open revolver (%.1f rad/s, about %+.2f %+.2f %+.2f; %.2f against the opening flick%s) -> %s.",
                    g_rightSwingSpeed, g_rightSwingAxis[0], g_rightSwingAxis[1], g_rightSwingAxis[2], against,
                    busy ? "; shells still to tip out, or a bullet in hand" : "", flickBack ? "shut" : "left open");
            }
        }

        if (gunGone || flickBack)
        {
            SetExtraKey(kKeyHolster, false);
            doneAt = now;
            phase = kSettling;
            if (!gunGone)
            {
                HintUsed("gun_close", now);
                Log("VR: revolver closed (swung shut).");
            }
        }
        break;
    }

    case kSettling: // a short pause before anything else is taken
        if (now - doneAt > 300) phase = kIdle;
        break;

    default:
        break;
    }

    g_gunButtonBusy = phase == kOpen || phase == kSettling;
    g_reloadKeyDown = phase == kOpen;
}

static void ReleaseMoveKeys()
{
    for (int key = 0; key < 4; ++key) SetMoveKey(key, false);
    for (int key = 0; key < kExtraKeyCount; ++key) SetExtraKey(key, false);
    SetRightMouse(false);
    g_sprinting = false;
    g_inventoryWaitingMs = -1.0;
    g_gunButtonBusy = false;
    g_reloadKeyDown = false;
    g_turnRemainder = 0.0f;
    // (The left hand on the shotgun's pump stays while the hand poses tool is
    // open: its window takes the PC's focus from the game's.)
    bool keepPump = LeftPumpKeptForPoseTool();
    if (keepPump)
    {
        static int logsLeft = 4;
        static double loggedAt = 0;
        if (logsLeft > 0 && NowMs() - loggedAt > 5000.0)
        {
            --logsLeft;
            loggedAt = NowMs();
            Log("VR: the controls are let go of (the game's window out of use, or the like), but the left hand stays on the shotgun's pump: the hand poses tool is open.");
        }
    }
    g_handUse[0] = keepPump ? kHandOnGun : kHandFree;
    g_handUse[1] = kHandFree;
    g_handDown[0] = g_handDown[1] = false;
    g_pendingHand = -1;
    g_fireHeld = false;
    g_bulletTap = false;
    g_aimPaused = false;
    g_virtualGunTapUntil = 0;
    g_virtualGunTapHip = false;
}

static bool ReadStick(XrAction action, float& x, float& y)
{
    XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = action;
    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(g_session, &getInfo, &state)) || !state.isActive) return false;
    x = state.currentState.x;
    y = state.currentState.y;
    return true;
}

static bool ReadButton(XrAction action)
{
    if (action == XR_NULL_HANDLE) return false;
    XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = action;
    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &getInfo, &state)) && state.isActive &&
           state.currentState == XR_TRUE;
}

// The VR page that MenuHandler.hps adds to the game's own options menu shows
// each setting as a short list of choices. These two turn a setting into its
// place in the list and back.
//   0 playing: seated, standing          4 height: -20 .. +20 cm in 5 cm steps
//   1 turning: smooth, snap              6 cutscenes: on a screen, through your eyes
//   2 snap angle: 30, 45, 60, 90
//   3 turn speed: slow .. very fast
static const float kMenuSnapAngles[4] = { 30.0f, 45.0f, 60.0f, 90.0f };
static const float kMenuTurnSpeeds[4] = { 2000.0f, 3000.0f, 4500.0f, 6500.0f };
//  10 walking follows: head, left controller    22 throw help: off .. strong
//  11 walking speed: full, follows the stick    23 hand reach: short .. very long
//  12 crouch by ducking: off, on                24 vibration: off .. strong
//  13 how far to duck: a little .. a lot        25 (unused)
//  14 world scale: -10 .. +10 % of the calibrated scale, in 5 % steps
//  15 game menu distance: near .. far           29 PC keyboard and mouse: off, on
//  16 game menu size: small .. large            30 door handles need turning: off, on
//  17 blood across the eyes: off, on
//  18 pointer steadying: off .. strong
//  33 monitor view steadying: off .. strong      34 monitor view width: narrow, normal, wide
//  35 knockdowns and death: through your eyes, on the cutscene screen, faded to black
//  39 (button) recalibrate: the start-up page from the beginning
//  44 not a choice: the calibrated world scale in thousandths (the World Scale row's labels)
static const float kMenuDuckSeated[3] = { 0.08f, 0.12f, 0.18f };
static const float kMenuDuckStanding[3] = { 0.20f, 0.30f, 0.45f };
static const float kMenuHudDistances[3] = { 1.0f, 1.5f, 2.2f };
static const float kMenuHudWidths[3] = { 1.4f, 1.8f, 2.4f };
static const float kMenuPointerSmooth[4] = { 0.0f, 0.5f, 0.75f, 0.9f };
static const float kMenuThrowBoosts[4] = { 1.0f, 1.3f, 1.7f, 2.2f };
static const float kMenuHandReaches[4] = { 0.20f, 0.30f, 0.45f, 0.60f };
static const float kMenuBuzzScales[4] = { 0.0f, 0.5f, 1.0f, 1.5f };
static const float kMenuMonitorSmooth[4] = { 0.0f, 0.15f, 0.30f, 0.50f }; // seconds (0 = off)
static const float kMenuMonitorWidths[3] = { 60.0f, 70.0f, 80.0f };       // degrees

static int NearestOf(const float* values, int count, float value)
{
    int best = 0;
    for (int i = 1; i < count; ++i)
        if (fabsf(values[i] - value) < fabsf(values[best] - value)) best = i;
    return best;
}

static int StartedForTestMap(); // (below, with the dedication)

int VrGetMenuOption(int index)
{
    const Settings& s = GetSettings();
    switch (index)
    {
    case 90: return StartedForTestMap(); // not a setting: was the game started for a test map? (1 the grab test, 2 the item test, 3 the drawer test, 4 the sight test)
    case 0: return s.seated ? 0 : 1;
    case 1: return s.turnMode == 1 ? 1 : 0;
    case 2: return NearestOf(kMenuSnapAngles, 4, s.snapAngle);
    case 3: return NearestOf(kMenuTurnSpeeds, 4, s.turnSpeed);
    case 4:
    {
        int step = (int)floorf(s.heightOffset / 0.05f + 0.5f) + 4;
        return step < 0 ? 0 : (step > 8 ? 8 : step);
    }
    case 6: return 0; // unused: cutscenes always play on a screen
    case 10: return s.moveDir == 1 ? 1 : 0;
    case 11: return s.analogWalk ? 1 : 0;
    case 12: return s.duckCrouch ? 1 : 0;
    case 13: return NearestOf(kMenuDuckSeated, 3, s.duckSeated);
    case 14: return s.sizeStep + 2;
    case 15: return NearestOf(kMenuHudDistances, 3, s.hudDistance);
    case 16: return NearestOf(kMenuHudWidths, 3, s.hudWidth);
    case 17: return s.faceEffects ? 1 : 0;
    case 18: return NearestOf(kMenuPointerSmooth, 4, s.pointerSmooth);
    case 22: return NearestOf(kMenuThrowBoosts, 4, s.throwBoost);
    case 23: return NearestOf(kMenuHandReaches, 4, s.handReach);
    case 24: return NearestOf(kMenuBuzzScales, 4, s.buzzScale);
    case 25: return 0; // unused: healing is always by wrapping the arm
    case 29: return s.pcInput ? 1 : 0;
    case 30: return s.doorTurn ? 1 : 0;
    case 32: return s.logicAtHeadsetRate ? 1 : 0;
    case 33: return NearestOf(kMenuMonitorSmooth, 4, s.monitorSmooth);
    case 34: return NearestOf(kMenuMonitorWidths, 3, s.monitorWidth);
    case 35: return s.roughView < 0 ? 0 : (s.roughView > 2 ? 2 : s.roughView); // (read by the body script, VRUpdateRoughMove; three choices)
    case 36: return (int)(s.pullReach * 100.0f + 0.5f); // not a choice: the pull's reach in centimeters (read by the body script, VRFindPullTarget)
    case 37: return s.viewSway == 1 ? 1 : 0; // (read by the body script, VRSteadyEyes: the default or off)
    case 38: return 0; // unused: cutscenes always play on the cinema screen
    case 39: return 0; // a button: "Recalibrate"
    case 40: return s.interactionHints < 0 ? 0 : (s.interactionHints > 2 ? 2 : s.interactionHints); // (Until Learned, Always, Off)
    case 41: return 0; // a button: "Show Learned Hints Again"
    case 42: return s.vignetteStrength < 0 ? 0 : (s.vignetteStrength > 3 ? 3 : s.vignetteStrength); // (the vignette's strength)
    case 43: return s.moveVignette < 0 ? 0 : (s.moveVignette > 3 ? 3 : s.moveVignette); // (the movement vignette)
    case 44: return (int)(s.worldScaleCalibrated * 1000.0f + 0.5f); // not a choice: for the World Scale row's labels
    default: return 0;
    }
}

static void StartupWantedFromSettings(); // (the start-up page, below)

void VrSetMenuOption(int index, int choice)
{
    if (choice < 0 || VrGetMenuOption(index) == choice) return;
    Settings& s = GetSettings();
    switch (index)
    {
    case 0:
        if (choice > 1) return;
        s.seated = choice == 0;
        s.holsterDown = s.seated ? 0.50f : 0.70f; // the hip is nearer the head when sitting
        break;
    case 1: if (choice > 1) return; s.turnMode = choice; break;
    case 2: if (choice > 3) return; s.snapAngle = kMenuSnapAngles[choice]; break;
    case 3: if (choice > 3) return; s.turnSpeed = kMenuTurnSpeeds[choice]; break;
    case 4: if (choice > 8) return; s.heightOffset = (float)(choice - 4) * 0.05f; break;
    case 6: return;
    case 10: if (choice > 1) return; s.moveDir = choice; break;
    case 11: if (choice > 1) return; s.analogWalk = choice == 1; break;
    case 12: if (choice > 1) return; s.duckCrouch = choice == 1; break;
    case 13:
        if (choice > 2) return;
        s.duckSeated = kMenuDuckSeated[choice];
        s.duckStanding = kMenuDuckStanding[choice];
        break;
    case 14: if (choice > 4) return; SetSizeStep(choice - 2); break;
    case 15: if (choice > 2) return; s.hudDistance = kMenuHudDistances[choice]; break;
    case 16: if (choice > 2) return; s.hudWidth = kMenuHudWidths[choice]; break;
    case 17: if (choice > 1) return; s.faceEffects = choice == 1; break;
    case 18: if (choice > 3) return; s.pointerSmooth = kMenuPointerSmooth[choice]; break;
    case 22: if (choice > 3) return; s.throwBoost = kMenuThrowBoosts[choice]; break;
    case 23: if (choice > 3) return; s.handReach = kMenuHandReaches[choice]; break;
    case 24: if (choice > 3) return; s.buzzScale = kMenuBuzzScales[choice]; break;
    case 25: return; // (unused: healing is always by wrapping the arm)
    case 29: if (choice > 1) return; s.pcInput = choice == 1; break;
    case 30: if (choice > 1) return; s.doorTurn = choice == 1; break;
    case 32: if (choice > 1) return; s.logicAtHeadsetRate = choice == 1; break;
    case 33: if (choice > 3) return; s.monitorSmooth = kMenuMonitorSmooth[choice]; break;
    case 34: if (choice > 2) return; s.monitorWidth = kMenuMonitorWidths[choice]; break;
    case 35: if (choice > 2) return; s.roughView = choice; break;
    case 37: if (choice > 1) return; s.viewSway = choice; break;
    case 38: return; // (unused: not a choice)
    case 40: if (choice > 2) return; s.interactionHints = choice; break;
    case 42: if (choice > 3) return; s.vignetteStrength = choice; break;
    case 43: if (choice > 3) return; s.moveVignette = choice; break;
    case 41:
        HintsForget();
        return;
    case 39:
        StartupWantedFromSettings(); // (the start-up page, from the beginning)
        return;
    default: return;
    }
    SaveSettings();
    Log("VR: option %d set to choice %d from the game's options menu.", index, choice);
}

// A snap turn waiting to be done by the game's body script: radians, positive
// = to the left.
static float g_pendingSnap = 0.0f;
static unsigned long long g_pendingSnapTime = 0;
// Is a snap turn's step in it (not only a smooth turn's small steps, made by
// the body script while a hand holds something)? And was there one in the
// turn last taken (VrLastTurnWasSnap)? Don't tell the two apart by size (say
// 7 degrees): a fast smooth turn passes that whenever a frame comes late, the
// whole queue is then turned at once, up to 16 degrees, and the held thing
// and the hand fall an update's turn behind the view.
static bool g_pendingSnapHasStep = false;
static bool g_lastTurnWasSnap = false;

// The game's own turning speed. A smooth turn with the stick moves the game's
// mouse, and the game turns as far as its own mouse settings have it
// (sensitivity, smoothing). But while a hand holds the game's interact button
// on something (its grab, a door...), the game takes the mouse for that
// thing, so the turn is made by the body script instead, at a speed of the
// mod's own (1.75 rad/s at turn speed 3000), which can be far from the
// mouse's: the smooth turn would change speed whenever a hand holds
// something. So each whole smooth turn made with
// the mouse, in the game's ordinary state, is measured: the mouse steps sent
// against how far the game's camera turned, counted until it has stopped (the
// game smooths the mouse). The body script's turn then goes at that same
// speed (Settings::turnRadPerCount, kept for the next run).
static float g_gameYawSeen = 0.0f;			// the game camera's turn as last seen (VrFollowTurn)
static bool g_gameYawSeenKnown = false;
static float g_turnMeasureYawWas = 0.0f;	// ...as at the last call of the turning code
static bool g_turnMeasureYawWasKnown = false;
static bool g_turnMeasuring = false;		// a smooth turn with the mouse being measured
static bool g_turnMeasureSpoilt = false;	// ...not to be used (another kind of turn, or a jump, came in)
static double g_turnMeasureCounts = 0.0;	// ...the mouse steps sent in it
static double g_turnMeasureYaw = 0.0;		// ...and how far the game turned since it started
static double g_turnMeasureLastSendMs = 0.0;
static int g_turnMeasureLogs = 0;
static float g_turnRadPerCountSaved = 0.0f;	// (written to the settings file when it moves away from this)

// For the log: the first five snap turns are watched for 20 frames each,
// frame by frame, to see where the eye goes in the game's world and when (see
// WatchSnapFrame), so any shift of the view in a snap turn can be traced.
static int g_snapWatchFrames = 0;
static int g_snapWatchesLeft = 5;
// When the body script last took a snap turn (for VrSnapTurnJustTaken).
static double g_snapTakenMs = -1000000.0;

bool VrSnapTurnJustTaken()
{
    return NowMs() - g_snapTakenMs < 300.0;
}

bool VrLastTurnWasSnap()
{
    return g_lastTurnWasSnap;
}

float VrTakeSnapTurn()
{
    float angle = g_pendingSnap;
    bool snap = g_pendingSnapHasStep;
    g_pendingSnap = 0.0f;
    g_pendingSnapHasStep = false;
    g_lastTurnWasSnap = false;
    // A step asked for while no script was listening (a menu) is forgotten.
    if (g_pendingSnapTime == 0 || GetTickCount64() - g_pendingSnapTime > 500) return 0.0f;
    g_lastTurnWasSnap = snap;
    if (snap && fabsf(angle) > 0.1f) g_snapTakenMs = NowMs(); // (see VrSnapTurnJustTaken)
    if (snap && fabsf(angle) > 0.1f && g_snapWatchesLeft > 0)
    {
        --g_snapWatchesLeft;
        g_snapWatchFrames = 20;
        Log("SNAP: the body script takes a turn of %.0f degrees (headset frame %llu).", angle * 57.29578f, g_frameNumber);
    }
    return angle;
}

// One line a frame while a snap turn is watched: the game's turn, its
// camera's own position, where the eye was in the game's world (the last
// view built, the right eye's with stereo in one frame) and where the head
// is in the room. With the head held still the eye should stay put.
static void WatchSnapFrame()
{
    if (g_snapWatchFrames <= 0) return;
    --g_snapWatchFrames;
    float game[3], eye[3], yaw = 0.0f;
    unsigned long long serial = 0;
    if (!CameraLastViewNumbers(game, eye, yaw, serial)) return;
    const XrVector3f& head = g_frameHeadPose.position;
    // (And which way the eye looked in the world and the head in the room:
    // through the character's turn while walking both should change only as
    // the real head turns.)
    float eyeFacing = 0.0f, headYaw = 0.0f, headPitch = 0.0f, headRoll = 0.0f;
    if (!CameraLastEyeFacing(eyeFacing)) eyeFacing = 0.0f;
    ToYawPitchRoll(g_frameHeadPose.orientation, headYaw, headPitch, headRoll);
    Log("SNAP frame %llu: game turn %+.1f deg | game camera %.3f %.3f %.3f | eye in the world %.3f %.3f %.3f, facing %+.1f deg | "
        "head in the room %.3f %.3f %.3f, facing %+.1f deg | view built for frame %llu",
        g_frameNumber, yaw * 57.29578f, game[0], game[1], game[2], eye[0], eye[1], eye[2], eyeFacing,
        head.x, head.y, head.z, headYaw, serial);
}

// The left stick's push, turned to the game body's own directions: to its
// right and the way it faces, each -1 to 1. Kept for the player script.
static float g_stickMove[2] = { 0.0f, 0.0f };
static unsigned long long g_stickMoveTime = 0;

bool VrGetStickMove(float& sideways, float& forwards)
{
    sideways = forwards = 0.0f;
    if (!GetSettings().analogWalk) return false;
    if (g_stickMoveTime == 0 || GetTickCount64() - g_stickMoveTime > 250) return false;
    sideways = g_stickMove[0];
    forwards = g_stickMove[1];

    static bool logged = false;
    if (!logged && (sideways != 0.0f || forwards != 0.0f))
    {
        logged = true;
        Log("VR: the player script asked for the stick's push and walks by it (any direction, any speed).");
    }
    return true;
}

bool VrWalkingWithStick()
{
    if (g_stickMoveTime == 0 || GetTickCount64() - g_stickMoveTime > 250) return false;
    if (fabsf(g_stickMove[0]) > 0.05f || fabsf(g_stickMove[1]) > 0.05f) return true;
    for (int i = 0; i < 4; ++i)
        if (g_moveKeyDown[i]) return true;
    return false;
}

void VrShiftCentre(const float shift[3])
{
    // ToRelativePose: the pose's position is the room's turned by the
    // remembered facing direction, times the world scale, from the middle.
    // So for the pose to change by `shift` from now on, the middle moves by
    // the opposite, turned back and divided by the scale. (Sideways only.)
    float scale = GetSettings().worldScale;
    if (scale < 0.01f || !g_haveReference) return;
    float c = cosf(g_referenceYaw), s = sinf(g_referenceYaw);
    g_referencePosition[0] -= (c * shift[0] + s * shift[2]) / scale;
    g_referencePosition[2] -= (-s * shift[0] + c * shift[2]) / scale;
    if (g_framePoseValid) PublishHeadPose(g_frameHeadPose, g_frameEyePose, false);
}

// The character turns under the player while walking. The game's character
// only turns with the right stick, so a player who turns for real faces away
// from it, and walking where you look is, to the game, walking sideways: its
// sideways legs and its slower sideways run (forwards 5.25 m/s running,
// sideways 3.4, backwards 2; MoveState_Normal.hps), and the difference in
// speeds bends the path towards its own forwards. So the body script turns
// the character towards the head while the stick walks (VRDoWalkTurn in
// PlayerBodyHandler.hps) and tells the mod first (roll 7095).
// The view is the game's camera turned by the head's pose from "straight
// ahead" (ToRelativePose: positions and turns alike), so "straight ahead"
// is turned with the camera by as much: the head, the hands and all they hold
// stay where they are in the game's world and look the same way. (The middle
// of the play area stays: TurnAboutHead's turn about the head is for a turn
// the view goes with.) The camera hook's own following of that turn is taken
// off beforehand (g_turnLastYaw), and so is the measuring of the game's own
// turning speed. The first few times, the eye's place in the world is written
// to the log frame by frame for a while (as for a snap turn: WatchSnapFrame).
static double g_absorbLastMs = -1000000.0;
static int g_absorbWatchesLeft = 3;
static int g_absorbLogsLeft = 12;

void VrAbsorbTurn(float turned)
{
    if (!g_haveReference) return;
    g_referenceYaw += turned;
    if (g_referenceYaw > 3.14159265f) g_referenceYaw -= 6.28318531f;
    if (g_referenceYaw < -3.14159265f) g_referenceYaw += 6.28318531f;
    if (g_turnHaveYaw) g_turnLastYaw += turned;
    if (g_turnMeasureYawWasKnown) g_turnMeasureYawWas += turned;
    if (g_turnMeasuring) g_turnMeasureSpoilt = true;
    if (g_framePoseValid) PublishHeadPose(g_frameHeadPose, g_frameEyePose, false);

    // A new turning (none for half a second): noted, and the first few watched.
    double now = NowMs();
    if (now - g_absorbLastMs > 500.0)
    {
        if (g_absorbLogsLeft > 0)
        {
            --g_absorbLogsLeft;
            Log("WALK TURN: the character turns with you while you walk (the view kept as it is); first step %+.2f degrees.",
                turned * 57.29578f);
        }
        if (g_absorbWatchesLeft > 0 && g_snapWatchFrames <= 0)
        {
            --g_absorbWatchesLeft;
            g_snapWatchFrames = 30;
        }
    }
    g_absorbLastMs = now;
}

// Ducking: how far the real head is below where it was when the view was
// centered, and whether that counts as ducked (see UpdateDuck).
static bool g_ducked = false;
static unsigned long long g_duckTime = 0;

int VrGetDuck(float& maxLift)
{
    const Settings& settings = GetSettings();
    // Seated, the duck is small and the game's own crouch does the lowering;
    // only the duck itself is taken back off. Standing, the real crouch is the
    // whole of it, so all of the game's lowering is taken off (99 = no limit).
    // (The body script takes 99 to mean "playing standing": the
    // crouch is then the player's own, and the game's crouch neither poses the
    // body nor lowers the view at all; see VRDoDuck in PlayerBodyHandler.hps.)
    maxLift = settings.seated ? settings.duckSeated * settings.worldScale : 99.0f;
    if (!settings.duckCrouch) return 0;
    if (g_duckTime == 0 || GetTickCount64() - g_duckTime > 500) return 0;
    return g_ducked ? 2 : 1;
}

// Called once per headset frame with where the real head is.
static void UpdateDuck(const XrPosef& head)
{
    const Settings& settings = GetSettings();
    if (!g_haveReference || !settings.duckCrouch)
    {
        g_ducked = false;
        g_duckTime = 0;
        return;
    }

    float depth = settings.seated ? settings.duckSeated : settings.duckStanding;
    // How far the NECK has come down (not the eyes: they dip whenever the
    // head nods). And looking down, the shoulders round and the neck itself
    // sinks a little: up to 7 cm more is asked for the steeper the look.
    float drop = g_referenceNeckHeight - NeckHeight(head);
    const XrQuaternionf& q = head.orientation;
    float lookingDown = 2 * (q.y * q.z - q.w * q.x); // 0 level, 1 straight down
    if (lookingDown < 0.0f) lookingDown = 0.0f;
    if (lookingDown > 1.0f) lookingDown = 1.0f;
    depth += 0.07f * lookingDown;
    // Down past the depth = ducked; back up to within 60% of it = up again.
    // The gap between the two stops it flickering at the edge.
    bool was = g_ducked;
    if (drop > depth) g_ducked = true;
    else if (drop < depth * 0.6f) g_ducked = false;
    g_duckTime = GetTickCount64();

    static int logsLeft = 12;
    if (g_ducked != was && logsLeft > 0)
    {
        --logsLeft;
        Log("VR: head %s (%.0f cm below where the view was centred; the mark is %.0f cm, %s).",
            g_ducked ? "ducked -> crouch" : "back up -> stand", drop * 100.0f, depth * 100.0f,
            settings.seated ? "seated" : "standing");
    }
}

// Call after the controller readings were fetched. `menuPointer` is true
// while a hand is steering the mouse pointer on the menu panel.
static void UpdateMoveInput(bool menuPointer)
{
    if (!g_sticksReady) return;
    ++g_inputUpdates;
    UpdateOverlayClose(); // (before the window check: a Shift+Tab under way is always let go)
    if (!GameWindowIsInUse() || g_dedicationHolding) // (nothing reaches the game while the dedication is shown at the start)
    {
        ReleaseMoveKeys();
        g_stickMove[0] = g_stickMove[1] = 0.0f;
        return;
    }

    // Steam's overlay open over the game (see SteamOverlayInUse): nothing is
    // held down or pressed into it, and a fresh press of B (the key that
    // opened it, by accident) sends Steam's own Shift+Tab to close it, as it
    // cannot be seen in the headset (one key at a time, see
    // UpdateOverlayClose; the controllers wait until it is done).
    static bool overlayB = true;
    if (g_steamOverlayOpen || g_overlayCloseStep != 0)
    {
        ReleaseMoveKeys();
        SetInteractHeld(false);
        g_stickMove[0] = g_stickMove[1] = 0.0f;
        g_inventoryButtonWas = true; // (B must be let go before it opens the inventory again)
        bool b = ReadButton(g_crouchAction);
        if (b && !overlayB && g_steamOverlayOpen && g_overlayCloseStep == 0)
        {
            SendScanCode(0x2A, true); // left Shift; Tab follows (UpdateOverlayClose)
            g_overlayCloseStep = 1;
            g_overlayCloseStepMs = NowMs();
            Log("VR: B pressed with Steam's overlay open -> Shift+Tab sent to close it (Shift, then Tab 0.15 s later).");
        }
        overlayB = b;
        return;
    }
    overlayB = true; // (a B already held as the overlay opens does not close it at once)

    // --- Walking: left stick -------------------------------------------------
    // The walking keys are still pressed (the game's own rules about when you
    // may move hang on them), but they only give eight directions at full
    // speed. So the exact push is also kept for the game's player script,
    // which asks for it and walks by that instead (see VrGetStickMove).
    const bool analog = GetSettings().analogWalk;
    // (A quarter of the way: a worn stick can rest well off its middle, up to
    // about 0.17 with the stick untouched.)
    const float deadZone = analog ? 0.25f : 0.3f;
    float stickX = 0, stickY = 0;
    bool wantKey[4] = { false, false, false, false };
    bool pushed = false;
    float moveSide = 0.0f, moveForward = 0.0f;
    // (Each start of walking is logged with what the stick read, for the
    // first dozen, so a stick that drifts past the dead zone shows.)
    static bool walkWasPushed = false;
    static int walkStartLogs = 12;
    if (ReadStick(g_moveAction, stickX, stickY))
    {
        float push = sqrtf(stickX * stickX + stickY * stickY);
        if (push > deadZone)
        {
            pushed = true;
            if (!walkWasPushed && walkStartLogs > 0)
            {
                --walkStartLogs;
                Log("VR: walking starts: left stick at %+.2f %+.2f (pushed %.2f of the way; below %.2f counts as still).",
                    stickX, stickY, push, deadZone);
            }

            // "Forwards" should mean the way you are facing (or, if chosen,
            // the way the left controller points). The game only knows which
            // way its body faces, so turn the stick's direction by how far
            // your head (or hand) is turned away from straight ahead.
            float turn = 0.0f; // radians, positive = turned left
            bool fromHand = false;
            AcquireSRWLockShared(&g_poseLock);
            if (GetSettings().moveDir == 1 && g_sharedHandTime[0] != 0 && GetTickCount64() - g_sharedHandTime[0] < 300)
            {
                // Not while the controller points nearly straight up or down:
                // its left-right direction means little then.
                const float* r = g_sharedHandPose[0].rotation;
                if (r[2] * r[2] + r[8] * r[8] > 0.12f)
                {
                    turn = atan2f(r[2], r[8]);
                    fromHand = true;
                }
            }
            if (!fromHand && g_sharedPoseTime != 0)
            {
                // Which way the head faces: its forwards laid flat, with its
                // top's share added as far as it is tipped, so that it is
                // known also looking far down (as VRHeadFacing in
                // PlayerBodyHandler.hps, which the character's turn while
                // walking follows: the two must agree). Looking level it is
                // atan2 of the head's back, laid flat.
                const float* r = g_sharedPose.rotation; // (row after row: column 1 the head's up, column 2 its back)
                float forwardY = -r[5];
                float facingX = -r[2] - forwardY * r[1], facingZ = -r[8] - forwardY * r[7];
                if (facingX * facingX + facingZ * facingZ > 0.09f) turn = atan2f(-facingX, -facingZ);
            }
            ReleaseSRWLockShared(&g_poseLock);

            float c = cosf(turn), s = sinf(turn);
            float sideways = (stickX * c - stickY * s) / push; // + = to the body's right
            float forwards = (stickY * c + stickX * s) / push; // + = the way the body faces

            // Keys are on or off, so this gives eight directions.
            wantKey[0] = forwards > 0.38f;
            wantKey[2] = forwards < -0.38f;
            wantKey[3] = sideways > 0.38f;
            wantKey[1] = sideways < -0.38f;

            // How hard the stick is pushed, from 0 at the edge of the dead
            // zone to 1 at (nearly) full push.
            float amount = (push - deadZone) / (0.95f - deadZone);
            if (amount > 1.0f) amount = 1.0f;
            moveSide = sideways * amount;
            moveForward = forwards * amount;

            static bool loggedHand = false;
            if (fromHand && !loggedHand)
            {
                loggedHand = true;
                Log("VR: walking follows the left controller's direction.");
            }

            // For the log: while walking, every 3 seconds at most (20 in
            // all), how the stick is really pushed and how far the head faces
            // from the character. With the character turning under you while
            // walking (VrAbsorbTurn) the head should be near 0; a stick pushed
            // off straight shows here as itself.
            static double walkNoteMs = -10000.0;
            static int walkNotesLeft = 20;
            if (walkNotesLeft > 0 && NowMs() - walkNoteMs > 3000.0)
            {
                --walkNotesLeft;
                walkNoteMs = NowMs();
                Log("VR: walking: left stick at %+.2f %+.2f; the head faces %+.0f degrees from the character (plus to the left).",
                    stickX, stickY, turn * 57.29578f);
            }
        }
    }
    walkWasPushed = pushed;
    for (int key = 0; key < 4; ++key) SetMoveKey(key, wantKey[key]);
    g_stickMove[0] = moveSide;
    g_stickMove[1] = moveForward;
    g_stickMoveTime = GetTickCount64();

    // --- Sprinting: click the left stick in -----------------------------------
    // One click starts the sprint; it lasts until the stick is let go, so the
    // stick does not have to be held pressed in while pushing it. A second
    // click stops it. The stick already held in as the walk starts starts it
    // too.
    // (Not started while B, the inventory's Tab, is held: Shift with Tab is
    // Steam's own key for its overlay, see the inventory below. Its Shift goes
    // down only once Tab has been up a while, see ShiftMayGoDown.)
    {
        static bool clickWas = false, walkWas = false;
        bool click = ReadButton(g_sprintAction);
        bool clicked = click && !clickWas;
        if (!pushed) g_sprinting = false;
        else if (clicked && g_sprinting) g_sprinting = false;
        else if ((clicked || (click && !walkWas)) && !ReadButton(g_crouchAction)) g_sprinting = true;
        clickWas = click;
        walkWas = pushed;
    }
    SetExtraKey(kKeySprint, g_sprinting && (g_extraKeyDown[kKeySprint] || ShiftMayGoDown()));

    // --- Turning: right stick, sideways ---------------------------------------
    static LARGE_INTEGER lastTime = {};
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    float seconds = lastTime.QuadPart == 0 ? 0.0f : (float)(now.QuadPart - lastTime.QuadPart) / (float)frequency.QuadPart;
    lastTime = now;
    if (seconds > 0.05f) seconds = 0.05f; // after a pause, do not lurch

    float turnX = 0, turnY = 0;
    bool haveTurn = !menuPointer && ReadStick(g_turnAction, turnX, turnY);
    // (None while a knockdown, a kill or a death plays, see VrNoTurning.)
    if (NoTurningNow())
    {
        static int logsLeft = 4;
        if (haveTurn && fabsf(turnX) > 0.2f && logsLeft > 0)
        {
            --logsLeft;
            Log("VR: the turning stick was pushed during a knockdown, a kill or a death: not turned.");
        }
        haveTurn = false;
        g_pendingSnap = 0.0f;
        g_pendingSnapHasStep = false;
    }

    // How far the game's camera has turned since the last call, for the
    // measuring of its own turning speed (see g_turnMeasuring): added up while
    // a turn is measured; a jump (a snap turn, a cutscene) spoils it.
    if (g_gameYawSeenKnown && g_turnMeasureYawWasKnown && g_turnMeasuring)
    {
        float turned = g_gameYawSeen - g_turnMeasureYawWas;
        if (turned > 3.14159265f) turned -= 6.28318531f;
        if (turned < -3.14159265f) turned += 6.28318531f;
        if (fabsf(turned) > 0.5f) g_turnMeasureSpoilt = true;
        g_turnMeasureYaw += turned;
    }
    g_turnMeasureYawWas = g_gameYawSeen;
    g_turnMeasureYawWasKnown = g_gameYawSeenKnown;
    if (g_turnMeasuring && (!CameraPlayerStateNormal() || CameraTheatre())) g_turnMeasureSpoilt = true;

    if (GetSettings().turnMode == 1)
    {
        if (g_turnMeasuring) g_turnMeasureSpoilt = true; // (a snap turn's mode: not the mouse's turning)
        // Snap turning: one push of the stick is one step. The stick has to
        // come back towards the middle before the next step. The step itself
        // is done by the game's body script, which asks for it (roll 7015).
        static bool armed = true;
        if (!haveTurn || fabsf(turnX) < 0.3f)
        {
            armed = true;
        }
        else if (armed && fabsf(turnX) > 0.6f)
        {
            armed = false;
            float angle = GetSettings().snapAngle * 0.0174533f;
            g_pendingSnap += turnX > 0 ? -angle : angle; // positive = to the left
            g_pendingSnapTime = GetTickCount64();
            g_pendingSnapHasStep = true;
        }
        g_turnRemainder = 0.0f;
    }
    else if (haveTurn && fabsf(turnX) > 0.2f)
    {
        // Ease in from the edge of the dead zone so small pushes turn slowly.
        float amount = (fabsf(turnX) - 0.2f) / 0.8f;
        if (turnX < 0) amount = -amount;

        // While a hand holds the game's interact button on something (a door,
        // a drawer, a carried thing), the game takes mouse movement for that
        // thing, so turning with the mouse does nothing: you could not turn
        // while opening a door. Then the body is turned by the body script
        // instead, the way a snap turn is, a little on every update. (About
        // 100 degrees a second at the middle turn speed.)
        if (g_grabHeld && !CameraPlayerStateNormal())
        {
            // (As fast as the game's own turning with the mouse, once it
            // has been measured; see g_turnMeasuring.)
            float fullSpeed = 1.75f * (GetSettings().turnSpeed / 3000.0f); // radians a second, the stick pushed fully
            if (GetSettings().turnRadPerCount > 0.0f) fullSpeed = GetSettings().turnSpeed * GetSettings().turnRadPerCount;
            if (g_turnMeasuring) g_turnMeasureSpoilt = true; // (this turn is not the mouse's)
            static float carried = 0.0f;
            carried += -amount * fullSpeed * seconds; // radians, positive = to the left
            if (fabsf(carried) > 0.004f)
            {
                g_pendingSnap += carried;
                g_pendingSnapTime = GetTickCount64();
                carried = 0.0f;
            }
            g_turnRemainder = 0.0f;
            amount = 0.0f;
        }

        g_turnRemainder += amount * GetSettings().turnSpeed * seconds;
        int steps = (int)g_turnRemainder;
        g_turnRemainder -= (float)steps;
        if (steps != 0)
        {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = steps;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendModInput(input);

            // Measured (see g_turnMeasuring): the steps of a turn the game
            // makes itself, in its ordinary state.
            if (CameraPlayerStateNormal() && !CameraTheatre())
            {
                if (!g_turnMeasuring)
                {
                    g_turnMeasuring = true;
                    g_turnMeasureSpoilt = false;
                    g_turnMeasureCounts = 0.0;
                    g_turnMeasureYaw = 0.0;
                }
                g_turnMeasureCounts += steps;
                g_turnMeasureLastSendMs = NowMs();
            }
            else if (g_turnMeasuring) g_turnMeasureSpoilt = true;
        }
    }
    else
    {
        g_turnRemainder = 0.0f;
    }

    // A measured turn is over once no steps have been sent for 0.4 s (the
    // game's mouse smoothing has turned the rest by then). Used if it was all
    // the mouse's and long enough (200 steps and 10 degrees or more): its
    // speed (how far for each step, whichever way the game counts its turn)
    // is mixed into what is known (a third of the way), and kept in the
    // settings file when it has moved by 2% or more.
    if (g_turnMeasuring && NowMs() - g_turnMeasureLastSendMs > 400.0)
    {
        g_turnMeasuring = false;
        double counts = g_turnMeasureCounts, yaw = g_turnMeasureYaw;
        bool usable = !g_turnMeasureSpoilt && fabs(counts) >= 200.0 && fabs(yaw) >= 0.17;
        double perCount = usable ? fabs(yaw / counts) : 0.0;
        if (usable && perCount > 0.00001 && perCount < 0.01)
        {
            Settings& settings = GetSettings();
            float was = settings.turnRadPerCount;
            settings.turnRadPerCount = was > 0.0f ? (float)(was * 0.67 + perCount * 0.33) : (float)perCount;
            if (g_turnMeasureLogs < 6)
            {
                ++g_turnMeasureLogs;
                Log("VR: the game's own turning, measured: %.0f degrees for %.0f mouse steps (%.4f degrees a step; now taken as %.4f). "
                    "Turning while a hand holds something goes at that speed: %.0f degrees a second with the stick pushed fully.",
                    yaw * 57.29578, counts, perCount * 57.29578, settings.turnRadPerCount * 57.29578f,
                    settings.turnSpeed * settings.turnRadPerCount * 57.29578f);
            }
            if (g_turnRadPerCountSaved <= 0.0f || fabsf(settings.turnRadPerCount - g_turnRadPerCountSaved) > 0.02f * g_turnRadPerCountSaved)
            {
                g_turnRadPerCountSaved = settings.turnRadPerCount;
                SaveSettings();
            }
        }
        else if (g_turnMeasureLogs < 6 && fabs(counts) >= 200.0)
        {
            ++g_turnMeasureLogs;
            Log("VR: a smooth turn was not used to measure the game's turning (%.0f mouse steps, %.0f degrees%s).",
                counts, yaw * 57.29578, g_turnMeasureSpoilt ? ", another kind of turn or a jump came in" : "");
        }
    }

    // --- The movement vignette (Settings::moveVignette): the edges of the view darkened while the
    // --- thumbsticks move or turn you, never while you move yourself. Meta's locomotion guidance
    // --- recommends vignettes that darken the screen edges during movement; its Interaction SDK's
    // --- tunneling closes for the locomotor's turning and its speed, sized in degrees, as the ring
    // --- here is (DrawComfortRing: by the view's own angles, the same on every headset). Studies
    // --- (Fernandes and Feiner, 2016, and later ones) find that a field of view narrowed only while
    // --- moving reduces the sickness of moving by stick. Walking: up to three quarters of the ring,
    // --- by how far the stick is pushed (all the way with Movement Speed "Always Full"); sprinting:
    // --- all of it; only while the game's camera really moves (a stick pushed against a wall moves
    // --- nothing). Smooth turning: by the turning stick (snap turns are the comfort way to turn
    // --- already, and none while turning is held off). Only in play: not paused, not in the
    // --- inventory, not on the cutscene screen, in the game's ordinary state (not on a ladder, where
    // --- the hands climb). Closes in 0.25 s, opens in 0.4 s.
    {
        double now = NowMs();
        // How fast the game's camera really goes across, a tenth of a second at a time (meters a second;
        // below 0: not known, and walking is taken as moving).
        static float lastPlace[3] = { 0.0f, 0.0f, 0.0f };
        static double lastPlaceMs = 0.0;
        static float realSpeed = -1.0f;
        float gamePosition[3], eyePosition[3], gameYaw = 0.0f;
        unsigned long long viewSerial = 0;
        if (CameraLastViewNumbers(gamePosition, eyePosition, gameYaw, viewSerial))
        {
            double apart = now - lastPlaceMs;
            if (lastPlaceMs <= 0.0 || apart > 500.0)
            {
                realSpeed = -1.0f; // (a pause or the first look: measured from here)
                for (int i = 0; i < 3; ++i) lastPlace[i] = gamePosition[i];
                lastPlaceMs = now;
            }
            else if (apart >= 100.0)
            {
                float dx = gamePosition[0] - lastPlace[0], dz = gamePosition[2] - lastPlace[2];
                realSpeed = sqrtf(dx * dx + dz * dz) / (float)(apart / 1000.0);
                for (int i = 0; i < 3; ++i) lastPlace[i] = gamePosition[i];
                lastPlaceMs = now;
            }
        }
        else
        {
            realSpeed = -1.0f;
            lastPlaceMs = 0.0;
        }
        bool inPlay = !CameraGamePaused() && !CameraInventoryOpen() && !CameraTheatre() && CameraPlayerStateNormal();
        float wanted = 0.0f;
        int why = 0; // (for the log: 0 nothing, 1 walking, 2 sprinting, 3 turning, 4 the stick pushed but not going anywhere)
        if (inPlay && pushed && (realSpeed < 0.0f || realSpeed > 0.2f))
        {
            float amount = analog ? sqrtf(moveSide * moveSide + moveForward * moveForward) : 1.0f;
            if (amount > 1.0f) amount = 1.0f;
            wanted = amount * (g_sprinting ? 1.0f : 0.75f);
            why = g_sprinting ? 2 : 1;
        }
        else if (inPlay && pushed) why = 4;
        if (inPlay && GetSettings().turnMode == 0 && haveTurn && fabsf(turnX) > 0.2f)
        {
            float turning = (fabsf(turnX) - 0.2f) / 0.8f;
            if (turning > 1.0f) turning = 1.0f;
            if (turning > wanted)
            {
                wanted = turning;
                why = 3;
            }
        }
        // (Logged as it changes, the first 40 a run, with the vignette on.)
        static int whyWas = 0;
        static int whyLogsLeft = 40;
        if (why != whyWas && GetSettings().moveVignette > 0 && whyLogsLeft > 0)
        {
            --whyLogsLeft;
            static const char* const kWhy[5] = { "nothing moves you: opening", "walking by stick: closing", "sprinting: closing",
                                                  "turning by stick: closing", "the stick pushed, going nowhere: opening" };
            Log("VR: the movement vignette: %s (to %.2f before its strength; the camera at %.1f m/s).", kWhy[why], wanted, realSpeed);
        }
        whyWas = why;
        static float shown = 0.0f;
        if (wanted > shown) shown = shown + seconds / 0.25f > wanted ? wanted : shown + seconds / 0.25f;
        else shown = shown - seconds / 0.4f < wanted ? wanted : shown - seconds / 0.4f;
        g_moveRing = shown;
        g_moveRingMs = now;
    }

    // --- Jumping: push the right stick up (it has to come back down before
    // --- the next jump). A short press of the game's Space key.
    {
        static bool armed = true;
        static double releaseAt = 0;
        // While the game has you crouched (by the stick or by ducking), up
        // stands you up instead: the body script does it (command 9).
        bool pushedUp = haveTurn && turnY > 0.75f && fabsf(turnX) < 0.5f;
        if (!haveTurn || turnY < 0.4f) armed = true;
        if (pushedUp && armed && CameraGameCrouched())
        {
            armed = false;
            SendScriptCommand(9);
            static int logsLeft = 3;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: right stick up while crouched -> stand up.");
            }
        }
        else if (pushedUp && armed)
        {
            armed = false;
            SetExtraKey(kKeyJump, true);
            releaseAt = NowMs() + 120.0;
        }
        if (g_extraKeyDown[kKeyJump] && NowMs() > releaseAt) SetExtraKey(kKeyJump, false);
    }

    // --- Crouching: pull the right stick down (it has to come back up before
    // --- the next one). The game's crouch is a toggle, so one pull crouches
    // --- and the next stands up.
    {
        static bool armed = true;
        static double releaseAt = 0;
        bool down = haveTurn && turnY < -0.75f && fabsf(turnX) < 0.5f;
        if (!haveTurn || turnY > -0.4f) armed = true;
        if (down && armed)
        {
            armed = false;
            SetExtraKey(kKeyCrouch, true);
            releaseAt = NowMs() + 120.0;
        }
        if (g_extraKeyDown[kKeyCrouch] && NowMs() > releaseAt) SetExtraKey(kKeyCrouch, false);
    }

    // --- Menu: the menu button on the left controller. A short press is the
    // --- game's Escape key (sent as it is let go); held for kRecentreHoldMs
    // --- it recenters the view instead (VrRecenter), and no Escape is sent.
    {
        static const double kRecentreHoldMs = 600.0;
        static bool wasDown = false, heldLong = false;
        static double downSinceMs = 0.0, escapeUntilMs = 0.0;
        bool down = ReadButton(g_menuAction);
        double nowMs = NowMs();
        if (down && !wasDown)
        {
            downSinceMs = nowMs;
            heldLong = false;
        }
        if (down && !heldLong && nowMs - downSinceMs >= kRecentreHoldMs)
        {
            heldLong = true;
            VrRecenter();
            Log("VR: left menu button held -> the view is re-centred.");
        }
        if (!down && wasDown && !heldLong) escapeUntilMs = nowMs + 80.0; // a short press: Escape, for a moment
        wasDown = down;
        bool fromMenu = nowMs >= g_gameMenuEscapeFromMs && nowMs < g_gameMenuEscapeFromMs + 80.0; // ("Open Game Menu" on the VR menu)
        bool menuKey = nowMs < escapeUntilMs || fromMenu;
        // (The moment the game's menu is asked for, unless it is open already (then the key shuts it): a scene
        // on the cutscene screen holds its picture from then, before the game blurs it; see GamePausedOrAsked.)
        static bool menuKeyWas = false;
        if (menuKey && !menuKeyWas && !CameraGamePaused()) g_menuKeySentMs = nowMs;
        menuKeyWas = menuKey;
        SetExtraKey(kKeyMenu, menuKey);
    }
    // --- Inventory: B on the right controller (the game's Tab key). The
    // --- action is named "crouch" from an earlier button layout.
    // (A sprint is ended first, its left Shift released before Tab goes down:
    // Shift with Tab is Steam's own key for its overlay, so B pressed while
    // sprinting would open it over the game. Tab then waits until that Shift
    // has been up a while, see ShiftMayGoDown; a B released meanwhile still
    // gives a short Tab.)
    {
        static double tabDownMs = 0.0;
        static int waitLogsLeft = 6;
        bool down = ReadButton(g_crouchAction);
        double nowMs = NowMs();
        if (down && g_sprinting)
        {
            g_sprinting = false;
            SetExtraKey(kKeySprint, false);
        }
        if (down && !g_inventoryButtonWas && !g_extraKeyDown[kKeyInventory]) g_inventoryWaitingMs = nowMs;
        g_inventoryButtonWas = down;
        // (With a gun out, or an item kept ready, the mod holds the aim, and
        // the game opens its inventory only in its normal state: the aim is let go of
        // while B waits and its Tab is down (g_inventoryAimFreeUntilMs, keepAiming
        // below), and the Tab waits for the normal state.)
        int readyKindNow = CameraRightItemKind();
        bool aimHeldByMod = CameraGunHeld() || readyKindNow == kItemOther || readyKindNow == kItemBottle;
        if (aimHeldByMod && (g_inventoryWaitingMs >= 0.0 || g_extraKeyDown[kKeyInventory])) g_inventoryAimFreeUntilMs = nowMs + 400.0;
        if (g_inventoryWaitingMs >= 0.0)
        {
            bool stateReady = !aimHeldByMod || CameraPlayerStateNormal();
            if (TabMayGoDown() && stateReady)
            {
                if (aimHeldByMod && nowMs - g_inventoryWaitingMs > 20.0)
                {
                    static int aimLogsLeft = 6;
                    if (aimLogsLeft > 0)
                    {
                        --aimLogsLeft;
                        Log("VR: B with %s: the aim let go of, Tab sent %.0f ms after B, the game back in its normal state.",
                            CameraGunHeld() ? "a gun out" : "an item kept ready", nowMs - g_inventoryWaitingMs);
                    }
                }
                if (!aimHeldByMod && nowMs > g_inventoryWaitingMs && waitLogsLeft > 0)
                {
                    --waitLogsLeft;
                    Log("VR: B pressed just after Shift was let go (the sprint's): Tab sent %.0f ms after B, kept apart from Shift (Steam's overlay key).",
                        nowMs - g_inventoryWaitingMs);
                }
                SetExtraKey(kKeyInventory, true);
                tabDownMs = nowMs;
                g_inventoryWaitingMs = -1.0;
            }
            else if (nowMs - g_inventoryWaitingMs > 1000.0)
            {
                Log("VR: B's Tab waited a second for %s and was dropped (the inventory was not opened).",
                    stateReady ? "Shift to stay up" : "the game to leave its aiming state");
                g_inventoryWaitingMs = -1.0;
            }
        }
        // (A tap, never held past kInventoryTabMostMs however long B is: the game opens
        // its inventory as Tab comes up, and Tab held 0.4 s is its health check instead,
        // which can freeze the player with the shotgun hanging in the air.)
        else if (g_extraKeyDown[kKeyInventory] && nowMs - tabDownMs >= 80.0 && (!down || nowMs - tabDownMs >= kInventoryTabMostMs))
        {
            SetExtraKey(kKeyInventory, false);
        }
    }

    // --- Flashlight: click the right stick in to take it out or put it away
    // --- (the action is named "inventory" from an earlier button layout).
    // --- With the left stick held in, the click is a mark instead (see MakeMark).
    {
        static bool wasDown = false;
        bool down = g_inventoryReady && ReadButton(g_inventoryAction);
        if (down && !wasDown && !menuPointer)
        {
            if (ReadButton(g_sprintAction))
            {
                char prefix[128];
                int mark = MakeMark("the left stick held in, the right stick clicked", prefix, sizeof(prefix));
                CaptureRequestNamed(prefix);
                TestListNoteMark(mark, prefix);
            }
            else
            {
                SendScriptCommand(5);
            }
        }
        wasDown = down;
    }

    // --- The gun: A on the right controller draws it, puts it away, or (held)
    // --- opens the ammo check. While the gun is out and you are not sprinting,
    // --- the game is kept in its aiming state (right mouse button held), so the
    // --- trigger fires straight away. (X on the left controller puts away what
    // --- is held, see below.)
    bool gunMode = CameraGunHeld();
    // While the game is paused (the inventory, the menus) A and X do the
    // inventory's two keyboard-only jobs instead: A combines two items (the
    // game's Space) and X drops the chosen item (the game's X). The gun is
    // left alone meanwhile. (X is the gas mask's key during play, so it is only
    // sent while paused.) The inventory pauses the game only with the
    // difficulty's "game paused in inventory", so the same is done while the
    // game reports its inventory open.
    bool gamePaused = CameraGamePaused();
    bool inventoryKeys = gamePaused || CameraInventoryOpen();
    if (inventoryKeys)
    {
        SetExtraKey(kKeyHolster, false);
        SetExtraKey(kKeyCombine, ReadButton(g_gunAction));
        SetExtraKey(kKeyDrop, ReadButton(g_aimAction2));
    }
    else
    {
        SetExtraKey(kKeyCombine, false);
        SetExtraKey(kKeyDrop, false);
        UpdateGunButton(gunMode);
    }
    // --- Putting away what is in the hand: X on the left controller, one
    // --- press, one clear job. The revolver goes back as with the holster at
    // --- the hip (not the shotgun: it goes behind the shoulder only); any
    // --- other item in the right hand is put away the game's way (script
    // --- command 12); with the right hand empty, the flashlight in the left
    // --- hand goes. X is not the game's right mouse button during play: what
    // --- that does is done by hand in VR (things are thrown by a swing, put
    // --- down by a still let-go, let go of by opening the hand; the gun and
    // --- items are kept ready by the mod; a number lock and a dogtag are left
    // --- by moving away or by the menu button). While paused or in the
    // --- inventory it is the game's X key (drop), above.
    // (The right mouse button, held by "kept aiming / kept ready" below, would
    // raise the gun or item again as it goes, so the thing put away would jump
    // up first. So the gun goes the holster's way, which lets go of aiming for
    // a moment, and the right mouse button is left up for half a second and
    // until X is released.)
    static double putAwayQuietUntil = 0.0;
    static bool xHeldAfterPutAway = false;
    {
        static bool xWasDown = false;
        bool xDown = ReadButton(g_aimAction2);
        if (!xDown) xHeldAfterPutAway = false;
        if (xDown && !xWasDown && !inventoryKeys && !g_grabHeld)
        {
            double now = NowMs();
            const char* what = nullptr;
            // (Not the shotgun: it goes behind the right shoulder.)
            if (gunMode && CameraShotgunHeld())                    { what = nullptr; }
            else if (gunMode)                                      { g_virtualGunTapUntil = now + 120.0; g_virtualGunTapHip = false; what = "the gun"; }
            else if (CameraRightItemKind() != kItemNone)           { SendScriptCommand(12); what = "the item in the right hand"; }
            else if (CameraLeftItemKind() == kItemFlashlight)      { SendScriptCommand(5);  what = "the flashlight"; }
            if (what)
            {
                putAwayQuietUntil = now + 500.0;
                xHeldAfterPutAway = true;
            }
            static int logsLeft = 8;
            if (logsLeft > 0)
            {
                --logsLeft;
                if (what) Log("VR: X pressed -> %s put away.", what);
                else Log("VR: X pressed -> nothing to put away (%s).", gunMode ? "the shotgun goes behind the right shoulder" : "both hands empty");
            }
        }
        xWasDown = xDown;
    }
    // (And let go of for the shotgun's put-away behind the shoulder, and for the
    // inventory while B waits, its Tab is down or the inventory is open.)
    bool putAwayQuiet = xHeldAfterPutAway || NowMs() < putAwayQuietUntil || NowMs() < g_shoulderQuietUntilMs ||
                        NowMs() < g_inventoryAimFreeUntilMs || CameraInventoryOpen();

    bool keepAiming = gunMode && !g_gunButtonBusy && !g_sprinting && !g_aimPaused && !gamePaused && !putAwayQuiet;

    // Any other item in the right hand (the fuel can, a flare, a bottle...) is
    // kept "ready" in the same way, so that squeezing the right hand uses it
    // at once. The game wants its ready button (the right mouse button) held
    // first, which would need the other hand.
    // (Not the lighter: it is lit by its trigger, by hand. A bottle is, for
    // the game's uses of it on things; it is thrown by hand.)
    int readyKind = CameraRightItemKind();
    bool keepReady = !gunMode && (readyKind == kItemOther || readyKind == kItemBottle) && !g_sprinting && !g_aimPaused && !putAwayQuiet;
    static bool loggedReady = false;
    if (keepReady && !loggedReady)
    {
        loggedReady = true;
        Log("VR: an item is held in the right hand: it is kept ready (no need to hold X); squeeze the right hand to use it.");
    }
    SetRightMouse(keepAiming || keepReady);

    static bool logged = false;
    if (!logged && (wantKey[0] || wantKey[1] || wantKey[2] || wantKey[3]))
    {
        logged = true;
        Log("VR: thumbstick walking is working (first key pressed).");
    }
}

// ---------------------------------------------------------------------------
// Using the menus by pointing a controller at the panel
// ---------------------------------------------------------------------------
// The game's menus are worked with the mouse. While a menu is open, pointing a
// controller at the floating panel moves the mouse pointer to the spot being
// pointed at, and squeezing the trigger or grip clicks (as it does everywhere).

// Two questions for the SDL library the game uses for its window and mouse.
typedef int (*SdlGetRelativeMouseMode)(void);
typedef unsigned int (*SdlGetMouseState)(int* x, int* y);

static bool GameMouseIsInLookMode(int* pointerX, int* pointerY)
{
    static bool looked = false;
    static SdlGetRelativeMouseMode getMode = nullptr;
    static SdlGetMouseState getState = nullptr;
    if (!looked)
    {
        looked = true;
        HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (sdl != nullptr)
        {
            getMode = (SdlGetRelativeMouseMode)(void*)GetProcAddress(sdl, "SDL_GetRelativeMouseMode");
            getState = (SdlGetMouseState)(void*)GetProcAddress(sdl, "SDL_GetMouseState");
        }
    }
    if (getState != nullptr && pointerX != nullptr && pointerY != nullptr) getState(pointerX, pointerY);
    return getMode != nullptr && getMode() != 0;
}

// Moves the game's mouse pointer to a spot on its picture: `u` runs from 0 at
// the left edge to 1 at the right, `v` from 0 at the top to 1 at the bottom.
static void MoveGamePointer(float u, float v, bool lookMode, int pointerX, int pointerY)
{
    if (g_steamOverlayOpen) return; // (the pointer is the PC's own over Steam's overlay)
    HWND gameWindow = WindowFromDC(wglGetCurrentDC());
    RECT area;
    if (gameWindow == nullptr || !GetClientRect(gameWindow, &area) || area.right < 2 || area.bottom < 2) return;

    int x = (int)(u * (float)(area.right - 1) + 0.5f);
    int y = (int)(v * (float)(area.bottom - 1) + 0.5f);

    if (lookMode)
    {
        // The game is reading mouse movement rather than the pointer's place
        // on screen: nudge its own pointer towards the spot, a step per frame.
        int dx = x - pointerX, dy = y - pointerY;
        if (dx > 300) dx = 300;
        if (dx < -300) dx = -300;
        if (dy > 300) dy = 300;
        if (dy < -300) dy = -300;
        if (dx != 0 || dy != 0)
        {
            INPUT input = {};
            input.type = INPUT_MOUSE;
            input.mi.dx = dx;
            input.mi.dy = dy;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendModInput(input);
        }
    }
    else
    {
        // Moved by a pointer move "sent by a program", like the mod's clicks.
        // (SetCursorPos is only a fallback: Windows does not mark it as sent
        // by a program, so with the PC's mouse kept out of the game (setting
        // pc_input) the blocker takes it for the real mouse and throws it
        // away, and the pointer never moves in the game's menus.)
        POINT wanted = { x, y };
        POINT current;
        if (ClientToScreen(gameWindow, &wanted) && GetCursorPos(&current) &&
            (current.x != wanted.x || current.y != wanted.y))
        {
            int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int across = GetSystemMetrics(SM_CXVIRTUALSCREEN), down = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (across > 1 && down > 1)
            {
                // Windows counts the whole desktop as 0 to 65535 each way.
                INPUT input = {};
                input.type = INPUT_MOUSE;
                input.mi.dx = (LONG)(((long long)(wanted.x - left) * 65536 + across / 2) / across);
                input.mi.dy = (LONG)(((long long)(wanted.y - top) * 65536 + down / 2) / down);
                input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                SendModInput(input);
            }
            else
            {
                SetCursorPos(wanted.x, wanted.y);
            }
        }
    }
}

// Steadies the spot a hand points at on a menu against the hand's tremble.
// `state` remembers the steadied spot (two numbers and, third, 1 once it holds
// one). Small moves are followed slowly, so the pointer does not shiver on a
// button; a quick move across the menu is followed at once.
static void SteadyPointer(float& u, float& v, float state[3])
{
    float amount = GetSettings().pointerSmooth;
    if (amount < 0.01f || state[2] < 0.5f)
    {
        state[0] = u;
        state[1] = v;
        state[2] = 1.0f;
        return;
    }
    if (amount > 0.95f) amount = 0.95f;

    float du = u - state[0], dv = v - state[1];
    float away = sqrtf(du * du + dv * dv) / 0.05f; // 1 = a twentieth of the menu's width away
    if (away > 1.0f) away = 1.0f;
    float follow = (1.0f - amount) + amount * away * away;
    state[0] += du * follow;
    state[1] += dv * follow;
    u = state[0];
    v = state[1];
}

// Returns true (and steers the mouse pointer) while a menu is open and a hand
// is pointing at the panel. Call after the hands were located this frame.
static bool UpdateMenuPointer()
{
    static float steady[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };

    g_pointerActive = false;
    if (!g_panelShown || !GameWindowIsInUse())
    {
        g_pointerHit[0] = false;
        g_pointerHit[1] = false;
        steady[0][2] = steady[1][2] = 0.0f;
        return false;
    }

    int pointerX = 0, pointerY = 0;
    bool lookMode = GameMouseIsInLookMode(&pointerX, &pointerY);

    // The panel: its center, the direction it faces and its sideways direction.
    float c = cosf(g_panelYaw), s = sinf(g_panelYaw);
    float normal[3] = { s, 0, c };
    float across[3] = { c, 0, -s };

    // Where does each hand's pointing line meet the panel, if at all?
    float hitU[2] = { 0, 0 }, hitV[2] = { 0, 0 };
    bool steadied[2] = { false, false };
    for (int hand = 0; hand < 2; ++hand)
    {
        g_pointerHit[hand] = false;
        if (!g_aimValid[hand]) continue;

        const XrQuaternionf& q = g_aimRoom[hand].orientation;
        const XrVector3f& o = g_aimRoom[hand].position;
        // The way the controller points: its own "forwards" (negative z), turned into the room.
        float d[3] = { -2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y)) };

        float towards = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2];
        if (towards > -0.05f) continue; // pointing away from, or along, the panel

        float toPanel[3] = { g_panelPosition[0] - o.x, g_panelPosition[1] - o.y, g_panelPosition[2] - o.z };
        float distance = (toPanel[0] * normal[0] + toPanel[1] * normal[1] + toPanel[2] * normal[2]) / towards;
        if (distance < 0.05f || distance > 10.0f) continue;

        // Where the pointing line meets the panel, measured from the panel's center.
        float hit[3] = { o.x + d[0] * distance - g_panelPosition[0], o.y + d[1] * distance - g_panelPosition[1],
                         o.z + d[2] * distance - g_panelPosition[2] };
        float u = (hit[0] * across[0] + hit[2] * across[2]) / g_panelWidth + 0.5f;
        float v = 0.5f - hit[1] / g_panelHeight;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;

        SteadyPointer(u, v, steady[hand]);
        steadied[hand] = true;

        g_pointerHit[hand] = true;
        g_pointerHitLength[hand] = distance;
        hitU[hand] = u;
        hitV[hand] = v;
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        if (!steadied[hand]) steady[hand][2] = 0.0f; // not on the panel: start afresh next time
    }
    if (!g_pointerHit[0] && !g_pointerHit[1]) return false;

    // A hand is pointing at the panel. Only take over the mouse in a menu.
    //
    // While the game is being played, its scripts ask this mod where the
    // head and hands are many times a second. In a menu (the game is
    // paused, or no game is loaded) they stop asking. That silence is how
    // a menu is recognized. Before any script has ever asked, a menu is
    // recognized by the game's mouse not being in its "movement" mode.
    // The inventory is open while the game goes on: then the game's mouse is
    // in "pointer" mode, which also counts as a menu.
    bool playing = CameraScriptsBusy(300) && lookMode;
    bool unsure = !playing && lookMode && !CameraScriptsSeen();
    if (playing || unsure)
    {
        static bool loggedPlaying = false, loggedUnsure = false;
        if (playing && !loggedPlaying)
        {
            loggedPlaying = true;
            Log("VR: a hand pointed at the panel while the game was being played, so the mouse was left alone "
                "(the game's mouse is in %s mode).", lookMode ? "movement" : "pointer");
        }
        if (unsure && !loggedUnsure)
        {
            loggedUnsure = true;
            Log("VR: a hand pointed at the panel, but it is unclear whether a menu is open "
                "(no script has asked anything yet and the game's mouse is in movement mode), so the mouse was left alone.");
        }
        g_pointerHit[0] = false;
        g_pointerHit[1] = false;
        return false;
    }

    // The mouse follows the hand that pressed last, if it is pointing at the
    // panel; otherwise the other one.
    int hand = g_pointerHit[g_menuHand] ? g_menuHand : 1 - g_menuHand;

    // During a trigger click the pointer holds still (see UpdateGrabInput).
    if (NowMs() >= g_pointerFrozenUntil) MoveGamePointer(hitU[hand], hitV[hand], lookMode, pointerX, pointerY);
    g_pointerActive = true;
    g_pointerHand = hand;
    g_pointerLength = g_pointerHitLength[hand];

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("VR: menu pointer in use (%s hand; the game's mouse is in %s mode).", hand == 0 ? "left" : "right",
            lookMode ? "movement" : "pointer");
    }
    return true;
}

// ---------------------------------------------------------------------------
// The mod's own menu (vrmenu.cpp draws it; here it is shown and pointed at)
// ---------------------------------------------------------------------------

static XrSwapchain g_menuSwapchain = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLKHR g_menuSwapchainImages[16];
static uint32_t g_menuSwapchainImageCount = 0;
static int g_menuSwapchainWidth = 0, g_menuSwapchainHeight = 0;
static bool g_menuHasPicture = false;
static bool g_menuPlaced = false;
static float g_menuPosition[3] = { 0, 0, 0 }; // its center, in the room
static float g_menuYaw = 0.0f;
static const float kMenuWidthMetres = 0.90f;
static float g_menuHeightMetres = 0.75f;

// The way you face while the mod's menu is up: the way it faces you (for the
// Body page's spots).
static bool MenuFacingYaw(float& yaw)
{
    if (!VrMenuIsOpen() || !g_menuPlaced) return false;
    yaw = g_menuYaw;
    return true;
}

static void OnVrMenuAction(int action);

// ---------------------------------------------------------------------------
// The start-up page, so the game knows your height the same way all through
// the game: your height is saved and never measured again by itself, your
// size comes from standing up or is set on the page, and the body's spots are
// set too. In the main menu's room, before the game's own menu, the mod's menu
// comes up on a page of its own (VrStartupView) and goes through one thing at
// a time:
//   how you play; a controller rested on the floor (kept: SaveFloor); your
//   size, standing up straight for a moment or your height set on the page;
//   seated, your eyes as you sit to play (from then on SavedHeightFromFloor
//   puts the view there at every centering); the holster, the pouch and the
//   flashlight (the Body page's own steps); and what came out, to keep or to
//   go through again.
// The first time, all of it; from then on each start only asks whether to keep
// what is saved ("Welcome Back") and waits for a press. Y skips it, keeping
// what is saved. Not for the test maps or in stand-in mode (the Player page's
// "Run Player Setup Again" brings it up anywhere: menu action 89).
// ---------------------------------------------------------------------------

enum StartupStep
{
    kStartupOff,
    kStartupSame,      // "Welcome Back": keep what is saved? (waits for a press)
    kStartupHow,       // seated or standing
    kStartupFloor,     // a controller resting on the floor (the Body page's floor step)
    kStartupSizeWay,   // stand up, or set your height
    kStartupStand,     // standing up straight (StartMeasure, the Player page's height measuring)
    kStartupSetHeight, // your height on the picker
    kStartupSit,       // seated: your eyes as you sit to play
    kStartupHolster,   // the Body page's steps for the spots
    kStartupPouch,
    kStartupLight,
    kStartupDone,      // what came out
};
static const char* const kStartupStepNames[] = { "off", "same as last time?", "how you play", "the floor", "your size: which way",
                                                 "your size: standing up", "your size: set by hand", "your seated height",
                                                 "the holster", "the pouch", "the flashlight", "all set" };
static const double kStartupRoomFirstMs = 1000.0;   // the main menu all in view this long (cVR_MenuInView) before the page comes up
static const double kStartupNoWordMs = 30000.0;     // the room up this long with no word from the menu script: up anyway
static double g_menuInViewMs = -1000000.0;        // when the menu script last said the main menu is all in view

void VrMenuInView()
{
    g_menuInViewMs = NowMs();
}
static const float kEyesOfHeight = 0.936f;        // standing eyes as a share of how tall you are (an adult's, on average)
static int g_startupStep = kStartupOff;
static bool g_startupWanted = false;       // the game's VR settings asked for it (RECALIBRATE)

static void StartupWantedFromSettings()
{
    g_startupWanted = true;
}
static bool g_startupSitOnly = false;      // the Player page's "Calibrate Seated Height", alone
static bool g_startupOpenedMenu = false;   // the page opened the menu (and closes it at the end)
static bool g_startupRetry = false;        // the step's last try failed: its buttons are "Try again" and a way on
static VrStartupView g_startupView;
static char g_startupTitle[64], g_startupText[256], g_startupNote[128];
// The steps come through, for Back: each step's page has it as its last choice
// once there is a step to go back to.
static int g_startupHistory[16];
static int g_startupHistoryCount = 0;
static bool g_startupGoingBack = false;
static const char* const kStartupBack = "Back";
static float g_sitStretchFirst = 0.0f;     // the seated eyes' steady stretch (as UpdateMeasure's)
static double g_sitStretchSum = 0.0;
static int g_sitStretchCount = 0;

static bool StartupRunning()
{
    return g_startupStep != kStartupOff;
}

bool VrStartupShown()
{
    return StartupRunning();
}

// How dark the room is behind the start-up page (the scripts also blur it,
// cVR_RoomBlurred): it eases to kStartupDim over kStartupDimSeconds as the page
// comes, and back as it goes. The page itself is a layer of its own, not
// darkened. Drawn by DrawFade.
static const float kStartupDim = 0.55f, kStartupDimSeconds = 0.5f;
static float StartupDim()
{
    static float dim = 0.0f;
    static double lastMs = 0.0;
    double now = NowMs();
    float seconds = lastMs > 0.0 ? (float)((now - lastMs) / 1000.0) : 0.0f;
    lastMs = now;
    if (seconds > 0.1f) seconds = 0.1f;
    float target = StartupRunning() ? kStartupDim : 0.0f;
    float step = seconds / kStartupDimSeconds * kStartupDim;
    dim = dim < target ? fminf(target, dim + step) : fmaxf(target, dim - step);
    return dim;
}

// Is there a saved body to keep ("Same as last time?" rather than the whole page)?
static bool StartupHaveBody()
{
    const Settings& s = GetSettings();
    return s.startupDone && s.eyeHeight > 0.0f && (!s.seated || s.seatedEyes > 0.0f);
}

// What is saved, in a few words.
static void StartupSummary(char* text, size_t size)
{
    const Settings& s = GetSettings();
    char tall[48];
    if (s.bodyHeight > 0.0f) snprintf(tall, sizeof(tall), "Height: %.0f cm", s.bodyHeight * 100.0f);
    else if (s.eyeHeight > 0.0f) snprintf(tall, sizeof(tall), "Height: about %.0f cm", s.eyeHeight / kEyesOfHeight * 100.0f);
    else snprintf(tall, sizeof(tall), "Height: not calibrated");
    if (!s.seated) snprintf(text, size, "Standing. %s.", tall);
    else if (s.seatedEyes > 0.0f) snprintf(text, size, "Seated. %s. Eye height: %.0f cm.", tall, s.seatedEyes * 100.0f);
    else snprintf(text, size, "Seated. %s.", tall);
}

static void StartupShow(const char* title, const char* text, const char* note, const char* button0, const char* button1 = "",
                        const char* button2 = "", bool heightPicker = false)
{
    snprintf(g_startupTitle, sizeof(g_startupTitle), "%s", title);
    snprintf(g_startupText, sizeof(g_startupText), "%s", text);
    snprintf(g_startupNote, sizeof(g_startupNote), "%s", note);
    g_startupView.title = g_startupTitle;
    g_startupView.text = g_startupText;
    g_startupView.note = g_startupNote;
    g_startupView.buttons[0] = button0;
    g_startupView.buttons[1] = button1;
    g_startupView.buttons[2] = button2;
    g_startupView.heightPicker = heightPicker;
    if (g_startupHistoryCount > 0) // (Back, in the first free place)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (g_startupView.buttons[i] && g_startupView.buttons[i][0]) continue;
            g_startupView.buttons[i] = kStartupBack;
            break;
        }
    }
    VrMenuSetStartup(&g_startupView);
}

static void StartupGo(int step)
{
    Settings& s = GetSettings();
    // The step left is kept for Back (not when going back, nor for a step tried again).
    if (!g_startupGoingBack && g_startupStep != kStartupOff && step != g_startupStep)
    {
        if (g_startupHistoryCount == (int)(sizeof(g_startupHistory) / sizeof(g_startupHistory[0])))
        {
            for (int i = 1; i < g_startupHistoryCount; ++i) g_startupHistory[i - 1] = g_startupHistory[i];
            --g_startupHistoryCount;
        }
        g_startupHistory[g_startupHistoryCount++] = g_startupStep;
    }
    g_startupStep = step;
    g_startupRetry = false;
    Log("VR: the start-up page: %s.", kStartupStepNames[step]);
    char summary[160];
    switch (step)
    {
    case kStartupSame:
        // (Waits for a press: it never goes on by itself.)
        StartupSummary(summary, sizeof(summary));
        // (A seated height guessed on another headset is not used on this one,
        // SavedHeightApplies; the page says so.)
        StartupShow("Welcome Back", summary,
                    SavedHeightApplies() ? "Your saved calibration is ready." : "New headset: choose Recalibrate to measure your seated height.",
                    "Continue", "Recalibrate");
        break;
    case kStartupHow:
        StartupShow("Player Setup", "How will you play?", "This takes about a minute and is only needed once.", "Seated", "Standing");
        break;
    case kStartupFloor:
        StartBodyStep(kBodyFloor, true);
        StartupShow("Floor Calibration", "Rest a controller on the floor.", "Then pull its trigger. Can't reach it? Choose Skip.", "Skip");
        break;
    case kStartupSizeWay:
        StartupShow("Player Height", "Stand up to measure your height, or enter it yourself.", "", "Measure", "Enter Height");
        break;
    case kStartupStand:
        StartMeasure(true);
        StartupShow("Height Calibration", "Stand up straight and look straight ahead.", "Then pull either trigger.", "Enter Height");
        break;
    case kStartupSetHeight:
    {
        float& height = VrMenuStartupHeight();
        height = s.bodyHeight > 0.0f ? s.bodyHeight : (s.eyeHeight > 0.0f ? roundf(s.eyeHeight / kEyesOfHeight * 100.0f) / 100.0f : 1.75f);
        if (height < 1.20f) height = 1.20f;
        if (height > 2.20f) height = 2.20f;
        StartupShow("Player Height", "", "", "Confirm", "", "", true);
        break;
    }
    case kStartupSit:
        g_sitStretchCount = 0;
        StartupShow("Seated Height", "Sit as you will play and look straight ahead.", "Then pull either trigger.", "Skip");
        break;
    case kStartupHolster:
        StartBodyStep(kBodyHolster, true);
        StartupShow("Holster Position", "Place your right hand where you want the holster.", "Then pull the right trigger.", "Skip");
        break;
    case kStartupPouch:
        StartBodyStep(kBodyPouch, true);
        StartupShow("Ammo Pouch Position", "Place your left hand where you want the ammo pouch.", "Then pull the left trigger.", "Skip");
        break;
    case kStartupLight:
        StartBodyStep(kBodyLight, true);
        StartupShow("Flashlight Position", "Place your left hand where you want the flashlight.", "Then pull the left trigger.", "Skip");
        break;
    case kStartupDone:
        StartupSummary(summary, sizeof(summary));
        StartupShow("Setup Complete", summary,
                    kPlayerBuild ? "You can change this when the game next starts." : "You can change this at any time: press Y and open the Player tab.",
                    "Finish", "Start Over");
        break;
    }
}

// The page is put away. `done`: gone through to the end (or kept as it was),
// so the next start only asks whether to keep it (kStartupSame).
static void StartupEnd(const char* why, bool done)
{
    Settings& s = GetSettings();
    g_bodyStep = kBodyNone;  // (a step still under way is let go)
    g_measureStartMs = -1.0;
    g_startupStep = kStartupOff;
    g_startupHistoryCount = 0;
    VrMenuSetStartup(nullptr);
    if (g_startupOpenedMenu && VrMenuIsOpen())
    {
        VrMenuSetOpen(false);
        g_menuPlaced = false;
    }
    g_startupOpenedMenu = false;
    if (done && !g_startupSitOnly) s.startupDone = true;
    SaveSettings();
    char summary[160];
    StartupSummary(summary, sizeof(summary));
    Log("VR: the start-up page is put away (%s). %s", why, summary);
    // In a level, the view is centered so that the saved height counts now (in
    // the main menu's room it does as the room is left).
    if (g_haveReference && !MenuRoomOn()) VrRecenter();
}

static void StartupOpen(int step, bool sitOnly)
{
    g_startupSitOnly = sitOnly;
    BodyKeepBefore();
    VrMenuSetComfortInfo(""); // (the line below shows what each step finds: not what was found before)
    g_startupHistoryCount = 0;
    StartupGo(step); // (the page first, then the menu opened on it)
    if (!VrMenuIsOpen())
    {
        VrMenuSetActionHandler(OnVrMenuAction);
        VrMenuSetOpen(true);
        g_menuPlaced = false; // (in front of you)
        g_startupOpenedMenu = true;
    }
}

static void StartupNext(int from)
{
    const Settings& s = GetSettings();
    if (g_startupSitOnly)
    {
        StartupEnd("your seated height alone", true);
        return;
    }
    switch (from)
    {
    case kStartupFloor: StartupGo(kStartupSizeWay); break;
    case kStartupStand:
    case kStartupSetHeight: StartupGo(s.seated ? kStartupSit : kStartupHolster); break;
    case kStartupSit: StartupGo(kStartupHolster); break;
    case kStartupHolster: StartupGo(kStartupPouch); break;
    case kStartupPouch: StartupGo(kStartupLight); break;
    default: StartupGo(kStartupDone); break;
    }
}

// Your height set on the page: your eyes taken as kEyesOfHeight of it, and the
// world sized from them as the Player page's height measuring sizes it.
static void StartupSetHeight()
{
    Settings& s = GetSettings();
    float height = VrMenuStartupHeight();
    float was = s.worldScale;
    s.bodyHeight = height;
    s.eyeHeight = height * kEyesOfHeight;
    SetWorldScaleCalibrated(kGameEyeHeight / s.eyeHeight); // (0.89 to 1.64; the player's adjustment kept)
    if (!s.seated) s.heightOffset = 0.0f;
    SaveSettings();
    Log("VR: the start-up page: your height set to %.0f cm, your eyes taken as %.1f cm up. World scale %.3f (was %.3f).",
        height * 100.0f, s.eyeHeight * 100.0f, s.worldScale, was);
    char text[96];
    snprintf(text, sizeof(text), "Height set to %.0f cm. World scale: %.0f%% (was %.0f%%).", height * 100.0f, s.worldScale * 100.0f, was * 100.0f);
    VrMenuSetComfortInfo(text);
}

// Back to the step before: what is under way in this one is let go;
// what an earlier step kept stays kept until that step is done again.
static void StartupBack()
{
    if (g_startupHistoryCount <= 0) return;
    int to = g_startupHistory[--g_startupHistoryCount];
    g_bodyStep = kBodyNone;
    g_measureStartMs = -1.0;
    Log("VR: the start-up page: Back, from %s.", kStartupStepNames[g_startupStep]);
    g_startupGoingBack = true;
    StartupGo(to);
    g_startupGoingBack = false;
}

// From the start, with nothing to go back to ("Recalibrate", "Start Over").
static void StartupStartAgain()
{
    g_startupHistoryCount = 0;
    g_startupGoingBack = true;
    StartupGo(kStartupHow);
    g_startupGoingBack = false;
}

static void StartupButton(int button)
{
    Settings& s = GetSettings();
    if (button >= 0 && button < 3 && g_startupView.buttons[button] == kStartupBack)
    {
        StartupBack();
        return;
    }
    switch (g_startupStep)
    {
    case kStartupSame:
        if (button == 0) StartupEnd("kept as last time", true);
        else StartupStartAgain();
        break;
    case kStartupHow:
        if ((button == 0) != s.seated)
        {
            s.seated = button == 0;
            OnVrMenuAction(kMenuActionSeatedChanged); // (as the Comfort page's switch: the holster's height, saved)
        }
        StartupGo(kStartupFloor);
        break;
    case kStartupFloor:
        if (g_startupRetry && button == 0) StartupGo(kStartupFloor);
        else
        {
            g_bodyStep = kBodyNone;
            Log("VR: the start-up page: the floor not touched; %s.", g_stageFloorKnown ? "the play area's floor is used" : "no floor is known");
            StartupNext(kStartupFloor);
        }
        break;
    case kStartupSizeWay: StartupGo(button == 0 ? kStartupStand : kStartupSetHeight); break;
    case kStartupStand:
        if (g_startupRetry && button == 0) StartupGo(kStartupStand);
        else
        {
            g_measureStartMs = -1.0;
            StartupGo(kStartupSetHeight);
        }
        break;
    case kStartupSetHeight:
        StartupSetHeight();
        StartupNext(kStartupSetHeight);
        break;
    case kStartupSit:
        if (g_startupRetry && button == 0) StartupGo(kStartupSit);
        else StartupNext(kStartupSit);
        break;
    case kStartupHolster:
    case kStartupPouch:
    case kStartupLight:
        g_bodyStep = kBodyNone;
        StartupNext(g_startupStep);
        break;
    case kStartupDone:
        if (button == 0) StartupEnd("kept", true);
        else StartupStartAgain();
        break;
    }
}

// Seated: your eyes held steady and level as you sit, taken as a trigger is
// pulled; kept as how far they are above the floor. With no floor a
// controller was rested on (it can be out of reach when seated), the play
// area's floor is not trusted: a Rift's can be about 160 cm below the seated
// eyes, more than a short person's standing eyes, so nothing would ever be
// taken and the step could only be skipped. Instead your seated eyes are
// taken as kSeatedEyesOfHeight of your height, and the floor put that far
// below them (kept as a guess: SaveFloor), so the view's height is your
// sitting eyes' own, every run. (A play area's floor that the eyes stand a
// seated height above is used: see fromPlayArea below.)
static const float kSeatedEyesOfHeight = 0.71f; // (a seat about 0.26 of your height up, the eyes about 0.45 of it above the seat: 120 cm for 169 cm)

static void StartupMeasureSitting(const XrPosef& head, double now)
{
    Settings& s = GetSettings();
    float eyes = NeckHeight(head) + 0.075f;
    const XrQuaternionf& q = head.orientation;
    bool level = fabsf(2 * (q.y * q.z - q.w * q.x)) < kMeasureLevel;
    if (!level || g_sitStretchCount == 0 || fabsf(eyes - g_sitStretchFirst) > kMeasureSteady)
    {
        g_sitStretchFirst = eyes;
        g_sitStretchSum = 0.0;
        g_sitStretchCount = 0;
    }
    if (level)
    {
        g_sitStretchSum += eyes;
        ++g_sitStretchCount;
    }
    if (!g_takePulled[0] && !g_takePulled[1]) return;
    if (!level || g_sitStretchCount < kTakeSteadyFrames)
    {
        TakeRefused("Hold your head level and still, then pull the trigger again.");
        return;
    }

    float eyesY = (float)(g_sitStretchSum / g_sitStretchCount);
    g_sitStretchCount = 0;
    float floorY = 0.0f, above = 0.0f;
    bool real = false;
    // Sitting: well below your standing eyes (else you are still standing).
    auto seatedHeight = [&](float eyesAbove) {
        return eyesAbove >= 0.60f && eyesAbove <= 1.70f && !(s.eyeHeight > 0.0f && eyesAbove > s.eyeHeight - 0.10f);
    };
    // (With no floor of this headset's own, its play area's floor is used when
    // the eyes stand a seated height above it: SteamVR's for the Quest 3, from
    // its guardian, is the real floor. The Rift's, about 160 cm below a seated
    // head, is not, and the height is guessed.)
    bool fromPlayArea = !s.floorSaved && !g_touchFloorKnown && g_stageFloorKnown && seatedHeight(eyesY - g_stageFloorY);
    if (FloorY(floorY, &real) && real)
    {
        above = eyesY - floorY;
        if (!seatedHeight(above))
        {
            Log("VR: the start-up page: your eyes %.1f cm above the floor you rested a controller on: not taken as sitting (standing eyes %.1f cm).",
                above * 100.0f, s.eyeHeight * 100.0f);
            TakeRefused("You appear to be standing. Sit as you will play, then pull again.");
            return;
        }
        s.seatedEyesGuessed = false;
    }
    else if (fromPlayArea)
    {
        above = eyesY - g_stageFloorY;
        s.seatedEyesGuessed = false;
        Log("VR: the start-up page: your seated eyes measured above the play area's floor of \"%s\": %.1f cm.", g_headsetKey, above * 100.0f);
    }
    else
    {
        float height = s.bodyHeight > 0.0f ? s.bodyHeight : (s.eyeHeight > 0.0f ? s.eyeHeight / kEyesOfHeight : 1.75f);
        above = kSeatedEyesOfHeight * height;
        Log("VR: the start-up page: no floor rested on, so your seated eyes are taken from your height (%.0f cm): %.1f cm above the floor.",
            height * 100.0f, above * 100.0f);
        if (g_stageFloorKnown) Log("VR: the start-up page: (the play area's own floor is %.1f cm below your seated eyes)", (eyesY - g_stageFloorY) * 100.0f);
        SaveFloor(eyesY - above, "guessed from your height, seated", true);
        s.seatedEyesGuessed = true;
    }
    float was = s.seatedEyes;
    bool heightWas = s.heightOffset != 0.0f;
    s.seatedEyes = above;
    s.heightOffset = 0.0f; // (your height is measured now: the Comfort page's Height back to 0)
    SaveSettings();
    Log("VR: the start-up page: your seated eyes are %.1f cm above the floor (were %.1f).%s", above * 100.0f, was * 100.0f,
        heightWas ? " The Height setting is back to 0." : "");
    char text[96];
    snprintf(text, sizeof(text), "Seated height calibrated. Eye height: %.0f cm.", above * 100.0f);
    VrMenuSetComfortInfo(text);
    Buzz(0, 0.08f, 0.6f);
    Buzz(1, 0.08f, 0.6f);
    g_bodyBuzzAtMs = now + 200.0; // (the second of two short buzzes: UpdateBodyPage)
    StartupNext(kStartupSit);
}

// Once per headset frame, after the Body page.
static void UpdateStartup(const XrPosef& head)
{
    double now = NowMs();

    // Asked for from the game's VR settings, RECALIBRATE: the whole page, as on the
    // first start. (Asked for there by a script, opened here, in the headset frame.)
    if (g_startupWanted)
    {
        g_startupWanted = false;
        if (g_startupStep == kStartupOff)
        {
            Log("VR: the start-up page: opened from the game's VR settings (Measure again).");
            StartupOpen(kStartupHow, false);
        }
    }

    // Brought up once a run, in the main menu's room, once the game's own start
    // is over: the dedication, the loading screen, and the main menu's 5 s fade
    // from black (main_menu.hps), its title shown. The mod can't see that fade,
    // so the menu script says when the main menu is all in view
    // (cVR_MenuInView), and the page comes kStartupRoomFirstMs after. Should
    // the scripts never say so, after kStartupNoWordMs of the room.
    static bool tried = false;
    static double roomSinceMs = -1.0, inViewSinceMs = -1.0;
    bool roomUp = MenuRoomOn() && !CameraLoadingScreen() && g_frameShouldShow;
    if (roomUp) { if (roomSinceMs < 0.0) roomSinceMs = now; }
    else roomSinceMs = -1.0;
    if (roomUp && now - g_menuInViewMs < 500.0) { if (inViewSinceMs < 0.0) inViewSinceMs = now; }
    else inViewSinceMs = -1.0;
    bool due = (inViewSinceMs >= 0.0 && now - inViewSinceMs >= kStartupRoomFirstMs) ||
               (roomSinceMs >= 0.0 && now - roomSinceMs >= kStartupNoWordMs);
    if (!tried && g_startupStep == kStartupOff && due &&
        !g_dedicationHolding && g_frameShouldShow && g_framePoseValid && g_recentreStartMs < 0.0)
    {
        tried = true;
        Log("VR: the start-up page is due: %s.", inViewSinceMs >= 0.0 ? "the main menu has been all in view for a second"
                                                                       : "the menu script never said the main menu was all in view (after 30 s of the room)");
        if (StartedForTestMap() != 0 || g_standIn) Log("VR: the start-up page: not for a test run.");
        else if (VrMenuIsOpen()) Log("VR: the start-up page: not now, the menu is open.");
        else StartupOpen(StartupHaveBody() ? kStartupSame : kStartupHow, false);
    }

    if (g_startupStep == kStartupOff) return;
    if (!VrMenuIsOpen())
    {
        StartupEnd("Y pressed: skipped, what was saved stays", false);
        return;
    }

    switch (g_startupStep)
    {
    case kStartupFloor:
        if (g_bodyStep == kBodyNone && !g_startupRetry)
        {
            if (g_bodyLastDone) StartupNext(kStartupFloor);
            else
            {
                StartupShow("Floor Calibration", "Floor not detected.", "Rest the controller on the floor, then pull its trigger.", "Try Again", "Skip");
                g_startupRetry = true;
            }
        }
        break;
    case kStartupStand:
        if (g_measureStartMs < 0.0 && !g_startupRetry)
        {
            if (g_measureLastDone) StartupNext(kStartupStand);
            else
            {
                StartupShow("Height Calibration", "Height not measured.", "Stand up straight, look straight ahead, then pull either trigger.", "Try Again", "Enter Height");
                g_startupRetry = true;
            }
        }
        break;
    case kStartupSit:
        if (g_framePoseValid) StartupMeasureSitting(head, now); // (it waits for a trigger, or Skip)
        break;
    case kStartupHolster:
    case kStartupPouch:
    case kStartupLight:
        if (g_bodyStep == kBodyNone) StartupNext(g_startupStep);
        break;
    default:
        break;
    }
}

// Your height watched during play. The saved height is never re-measured by
// itself; but if your eyes stay more than 15 cm above your saved height for
// 20 seconds, or more than 30 cm below it for 30 (not while the game crouches:
// ducking to stay low), the game shows a hint, once a run, to recalibrate
// (script command 74). In seated play the head stays within 5 cm of where it
// was centered most of the time and dips about 20 cm at most, so the limits
// leave room for that.
static void UpdateHeightWatch(const XrPosef& head)
{
    static bool told = false;
    static double offSinceMs = -1.0;
    static int offWay = 0;
    const Settings& s = GetSettings();
    float saved = s.seated ? s.seatedEyes : s.eyeHeight;
    float floorY = 0.0f;
    bool watching = !told && saved > 0.0f && SavedHeightApplies() && g_haveReference && g_framePoseValid && g_frameShouldShow && FloorY(floorY) &&
                    CameraBodyNewsSeen() && CameraPlayerStateNormal() && !CameraGamePaused() && !CameraInventoryOpen() &&
                    !CameraTheatre() && !CameraLoadingScreen() && !MenuRoomOn() && !VrMenuIsOpen() && !StartupRunning() &&
                    g_recentreStartMs < 0.0;
    if (!watching)
    {
        offWay = 0;
        offSinceMs = -1.0;
        return;
    }
    float off = NeckHeight(head) + 0.075f - (floorY + saved);
    int way = off > 0.15f ? 1 : ((off < -0.30f && !CameraGameCrouched()) ? -1 : 0);
    double now = NowMs();
    if (way != offWay)
    {
        offWay = way;
        offSinceMs = now;
        return;
    }
    if (way == 0 || now - offSinceMs < (way > 0 ? 20000.0 : 30000.0)) return;
    told = true;
    SendScriptCommand(74); // (the hint: VRShowHeightHint in PlayerBodyHandler.hps)
    Log("VR: your eyes have been %.0f cm %s your saved height for %.0f s: the hint to measure again is shown (once a run).",
        fabsf(off) * 100.0f, way > 0 ? "above" : "below", (now - offSinceMs) / 1000.0);
}

static void OnVrMenuAction(int action)
{
    if (action == kMenuActionRecenter)
    {
        VrRecenter();
    }
    else if (action == kMenuActionCapture)
    {
        CaptureRequest();
        VrRequestArmNotes();
    }
    else if (action == kMenuActionSeatedChanged)
    {
        // The hip is nearer the head when sitting.
        GetSettings().holsterDown = GetSettings().seated ? 0.50f : 0.70f;
        SaveSettings();
        Log("VR: playing %s; the holster is now %.0f cm below the head.", GetSettings().seated ? "seated" : "standing",
            GetSettings().holsterDown * 100.0f);
        // The view's height from the floor at once, with that way's measured eyes (standing or seated).
        if (g_haveReference) SavedHeightFromFloor(g_frameHeadPose, GetSettings().seated ? "switched to seated" : "switched to standing");
    }
    else if (action == kMenuActionBodyFloor) StartBodyStep(kBodyFloor); // (the Body page)
    else if (action == kMenuActionBodyArms) StartBodyStep(kBodyArms);
    else if (action == kMenuActionBodyHolster) StartBodyStep(kBodyHolster);
    else if (action == kMenuActionBodyPouch) StartBodyStep(kBodyPouch);
    else if (action == kMenuActionBodyLight) StartBodyStep(kBodyLight);
    else if (action == kMenuActionBodySlot) StartBodyStep(kBodySlot);
    else if (action == kMenuActionBodyUndo) UndoBody();
    else if (action == kMenuActionHintsReset) HintsForget(); // (the Accessibility page)
    else if (action == kMenuActionGameMenu)
    {
        // (The Player page: SteamVR gives the Touch controllers' left
        // menu button to itself, so the game's menu is opened from here too: the
        // mod's menu shut, then the game's Escape a moment later.)
        VrMenuSetOpen(false);
        g_gameMenuEscapeFromMs = NowMs() + 200.0;
        Log("VR: \"Open Game Menu\" pressed: the game's Escape is sent.");
    }
    else if (action == kMenuActionWorldScaleMoved)
    {
        // (The Player page's number is the calibrated scale; the scale in use follows it, with the
        // player's adjustment.)
        Settings& s = GetSettings();
        SetWorldScaleCalibrated(s.worldScaleCalibrated);
        SaveSettings();
    }
    else if (action == kMenuActionPouchMoved || action == kMenuActionHolsterMoved) // (the Body page's numbers)
    {
        Settings& s = GetSettings();
        if (action == kMenuActionPouchMoved && !s.pouchSet)
        {
            s.pouchSet = true; // (its own spot from now on, not the holster's mirrored)
            SaveSettings();
        }
        static int logsLeft = 30;
        if (logsLeft > 0)
        {
            --logsLeft;
            if (action == kMenuActionHolsterMoved)
                Log("VR: the Body page: the holster moved by hand to %.0f cm right, %.0f cm forwards, %.0f cm below the eyes.",
                    s.holsterRight * 100.0f, s.holsterForward * 100.0f, s.holsterDown * 100.0f);
            else
                Log("VR: the Body page: the pouch moved by hand to %.0f cm left, %.0f cm forwards, %.0f cm below the eyes.",
                    -s.pouchRight * 100.0f, s.pouchForward * 100.0f, s.pouchDown * 100.0f);
        }
    }
    // The start-up page.
    else if (action >= kMenuActionStartupButton && action < kMenuActionStartupButton + 3)
    {
        if (StartupRunning()) StartupButton(action - kMenuActionStartupButton);
    }
    else if (action == kMenuActionStartupRun)
    {
        if (!StartupRunning()) StartupOpen(kStartupHow, false);
    }
    else if (action == kMenuActionStartupSit)
    {
        if (!GetSettings().seated) VrMenuSetComfortInfo("Seated height applies only when Play Mode is set to Seated.");
        else if (!StartupRunning()) StartupOpen(kStartupSit, true);
    }
    else if (action == kMenuActionMeasureSize)
    {
        StartMeasure();
    }
    // The Checks page (testlist.h).
    else if (action == kMenuActionCheckPass || action == kMenuActionCheckSkip)
    {
        TestListAnswer(action == kMenuActionCheckPass ? kCheckPassed : kCheckSkipped, 0, "");
        Buzz(g_grabHand, 0.05f, 0.4f);
    }
    else if (action == kMenuActionCheckFail)
    {
        // A mark (and the last two minutes with it), both eyes, and the answer
        // saying where they are.
        const TestCheck* check = TestListCurrent();
        if (check)
        {
            char why[64], prefix[128];
            snprintf(why, sizeof(why), "check %s failed", check->id);
            int mark = MakeMark(why, prefix, sizeof(prefix));
            CaptureRequestNamed(prefix);
            VrRequestArmNotes();
            TestListAnswer(kCheckFailed, mark, prefix);
        }
    }
    else if (action >= kMenuActionCheckWhyFirst && action < kMenuActionCheckWhyFirst + kTestListWhyButtons)
    {
        TestListGiveWhy(action - kMenuActionCheckWhyFirst);
        Buzz(g_grabHand, 0.05f, 0.4f);
    }
    else if (action == kMenuActionCheckBack || action == kMenuActionCheckNext)
    {
        TestListMove(action == kMenuActionCheckNext ? 1 : -1);
    }
    else if (action == kMenuActionCheckJump)
    {
        // The menu closes itself and it starts at once, as the Beast page's moves do.
        if (TestJumpStart() && VrMenuIsOpen()) VrToggleMenu();
    }
    else if (action >= kMenuActionPlaceFirst && action <= kMenuActionPlaceLast)
    {
        // The Places page: the same jump, to a place in the game's levels.
        int place = action - kMenuActionPlaceFirst;
        if (place < VrPlaceCount() && TestJumpToPlace(VrPlaceName(place), VrPlaceWords(place)) && VrMenuIsOpen()) VrToggleMenu();
    }
    else if (action == kMenuActionFeelDefaults)
    {
        // The Feel page: the numbers last tested (see Settings).
        const Settings tested;
        Settings& s = GetSettings();
        s.feelWeight = tested.feelWeight;
        s.feelHandStiffness = tested.feelHandStiffness;
        s.feelWristStiffness = tested.feelWristStiffness;
        s.feelMoveQuickest = tested.feelMoveQuickest;
        s.feelTurnQuickest = tested.feelTurnQuickest;
        s.feelWeightShown = tested.feelWeightShown;
        s.feelWristStrength = tested.feelWristStrength;
        s.feelSteadyBelow = tested.feelSteadyBelow;
        s.feelHandOnThing = tested.feelHandOnThing;
        s.feelEvenTurn = tested.feelEvenTurn;
        s.feelThrowSpeed = tested.feelThrowSpeed;
        SaveSettings();
        Log("VR: the Feel page is back to the tested numbers.");
    }
    // The Grips page (see grips.h).
    else if (action == kMenuActionGripChanged)
    {
        GripsMenuEdited();
    }
    else if (action == kMenuActionGripReset)
    {
        GripsMenuReset();
    }
    else if (action == kMenuActionGripEditing)
    {
        GripsMenuEditing();
    }
    else if (action == kMenuActionGripHand)
    {
        GripsMenuHand();
    }
    else if (action == kMenuActionGripWindow)
    {
        PoseToolSetOpen(GripsView().onPc); // (the hand poses tool on the PC's screen)
    }
    else if (action >= kMenuActionTestFirst && action <= kMenuActionTestLast)
    {
        // The Testing page: the body script does these (see VRDoTestCommand
        // in PlayerBodyHandler.hps).
        SendScriptCommand(action);
        Log("VR: testing button pressed; script command %d sent.", action);

        // The Beast page's moves: the menu closes itself and the move starts
        // at once.
        if (action >= kMenuActionBeastThrown && action <= kMenuActionBeastKillLadder && VrMenuIsOpen()) VrToggleMenu();
    }
}

void VrToggleMenu()
{
    VrMenuSetActionHandler(OnVrMenuAction);
    VrMenuSetOpen(!VrMenuIsOpen());
    g_menuPlaced = false; // put it in front of the head again when it opens
    Log("VR: the BunkerVR menu was %s.", VrMenuIsOpen() ? "opened" : "closed");
}

// Opens and closes the menu with its button, and while it is open passes on
// where a hand points on it and whether that hand squeezes. Returns true
// while a hand points at it (so that the beam is drawn).
static bool UpdateVrMenu()
{
    static bool buttonWasDown = false;
    static double lastToggleMs = 0;
    bool buttonDown = g_vrMenuReady && ReadButton(g_vrMenuAction);
    // A press only counts if the button had been up for a moment first, so a
    // press that flickers cannot open the menu and shut it again at once.
    // A short press opens or closes the menu as it is let go; held for
    // kRecentreHoldMs it recenters the view instead (VrRecenter: the eyes dim,
    // the view is centered, both controllers buzz), and the menu is left as it
    // was. (On SteamVR the left menu button, held for this on the Oculus
    // software, is SteamVR's own, so Y does it too.)
    static const double kRecentreHoldMs = 600.0;
    static double upSinceMs = 0, downSinceMs = 0;
    static bool pressCounts = false, heldLong = false;
    double nowMs = NowMs();
    if (!buttonDown && buttonWasDown) upSinceMs = nowMs;
    if (buttonDown && !buttonWasDown)
    {
        downSinceMs = nowMs;
        heldLong = false;
        pressCounts = nowMs - lastToggleMs > 300.0 && nowMs - upSinceMs > 120.0;
    }
    if (buttonDown && pressCounts && !heldLong && nowMs - downSinceMs >= kRecentreHoldMs)
    {
        heldLong = true;
        VrRecenter();
        Log("VR: Y held -> the view is re-centred.");
    }
    if (!buttonDown && buttonWasDown && pressCounts && !heldLong)
    {
        lastToggleMs = nowMs;
        // (With kPlayerBuild the mod's menu is a development tool that Y never
        // opens. Y shuts it while the start-up page is on it, to skip that page,
        // and otherwise opens and shuts the game's own menu, as "Open Game
        // Menu" does: on SteamVR the left menu button is SteamVR's, so this is
        // the way to pause there.)
        if (!kPlayerBuild || (VrMenuIsOpen() && StartupRunning())) VrToggleMenu();
        else if (!VrMenuIsOpen())
        {
            g_gameMenuEscapeFromMs = nowMs;
            Log("VR: Y pressed: the game's menu (the testers' build).");
        }
    }
    if (!buttonDown) pressCounts = false;
    buttonWasDown = buttonDown;

    if (!VrMenuIsOpen() || !g_menuPlaced)
    {
        return false;
    }

    // The menu's panel: the direction it faces and its sideways direction.
    float c = cosf(g_menuYaw), s = sinf(g_menuYaw);
    float normal[3] = { s, 0, c };
    float across[3] = { c, 0, -s };

    // Where each hand's pointing line meets the menu, both first, and which hand
    // has just begun to squeeze: the pointer stays with the hand that points,
    // but goes to the other hand at once when that one squeezes on the menu,
    // and that squeeze presses.
    static bool squeezing[2] = { false, false };
    bool squeezeBegan[2] = { false, false };
    bool hits[2] = { false, false };
    float hitU[2] = { 0, 0 }, hitV[2] = { 0, 0 }, hitDistance[2] = { 0, 0 };
    for (int hand = 0; hand < 2; ++hand)
    {
        float squeezed = g_gripValue[hand] > g_triggerValue[hand] ? g_gripValue[hand] : g_triggerValue[hand];
        bool now = squeezed > (squeezing[hand] ? 0.35f : 0.6f);
        squeezeBegan[hand] = now && !squeezing[hand];
        squeezing[hand] = now;
        if (!g_aimValid[hand]) continue;

        const XrQuaternionf& q = g_aimRoom[hand].orientation;
        const XrVector3f& o = g_aimRoom[hand].position;
        float d[3] = { -2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y)) };

        float towards = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2];
        if (towards > -0.05f) continue;

        float toPanel[3] = { g_menuPosition[0] - o.x, g_menuPosition[1] - o.y, g_menuPosition[2] - o.z };
        float distance = (toPanel[0] * normal[0] + toPanel[1] * normal[1] + toPanel[2] * normal[2]) / towards;
        if (distance < 0.05f || distance > 10.0f) continue;

        float hit[3] = { o.x + d[0] * distance - g_menuPosition[0], o.y + d[1] * distance - g_menuPosition[1],
                         o.z + d[2] * distance - g_menuPosition[2] };
        float u = (hit[0] * across[0] + hit[2] * across[2]) / kMenuWidthMetres + 0.5f;
        float v = 0.5f - hit[1] / g_menuHeightMetres;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        hits[hand] = true;
        hitU[hand] = u;
        hitV[hand] = v;
        hitDistance[hand] = distance;
    }

    int hand = g_grabHand;
    bool taken = false; // (the other hand took the pointer with a squeeze)
    if (hits[1 - hand] && (!hits[hand] || squeezeBegan[1 - hand]))
    {
        taken = hits[hand];
        hand = 1 - hand;
    }
    if (!hits[hand])
    {
        VrMenuPointer(false, 0, 0, false, NowMs());
        return false;
    }
    float u = hitU[hand], v = hitV[hand];

    // Steadied against the hand's tremble (started afresh when another
    // hand takes over or the pointer has been off the menu).
    static float steady[3] = { 0, 0, 0 };
    static int steadyHand = -1;
    static double steadyMs = 0;
    if (steadyHand != hand || NowMs() - steadyMs > 150.0) steady[2] = 0.0f;
    steadyHand = hand;
    steadyMs = NowMs();
    SteadyPointer(u, v, steady);

    if (taken) VrMenuPointer(false, 0, 0, false, NowMs()); // (so that the squeeze that took it counts as a new press)
    VrMenuPointer(true, u, v, squeezing[hand], NowMs());
    static int hoverCountSeen = 0; // (a light tick as it comes onto something to press)
    if (VrMenuHoverCount() != hoverCountSeen)
    {
        hoverCountSeen = VrMenuHoverCount();
        if (VrMenuPointerOnButton()) HoverBuzz(hand);
    }
    g_grabHand = hand; // the hand that points stays the pointing hand
    g_pointerHand = hand;
    g_pointerLength = hitDistance[hand];
    return true;
}

static int64_t ChooseSwapchainFormat();

// Makes sure the menu's picture is in the headset's hands, and decides where
// the menu floats when it has just been opened. Call with OpenGL active.
static void PrepareVrMenuPicture()
{
    if (!VrMenuIsOpen())
    {
        g_menuPlaced = false;
        return;
    }

    // The start-up page in the main menu's room stands where the game's menu
    // does: straight ahead in the room, whichever way the head looked as it came,
    // and again there after each recenter.
    static unsigned placedAtRecentre = 0;
    bool roomPage = StartupRunning() && MenuRoomOn();
    if (roomPage && g_menuPlaced && placedAtRecentre != g_recentreCount) g_menuPlaced = false;
    if (!g_menuPlaced && roomPage)
    {
        float yaw = g_referenceYaw;
        g_menuPosition[0] = g_referencePosition[0] - sinf(yaw) * 0.95f;
        g_menuPosition[1] = g_menuRoomEyes - 0.12f;
        g_menuPosition[2] = g_referencePosition[2] - cosf(yaw) * 0.95f;
        g_menuYaw = yaw;
        g_menuPlaced = true;
        placedAtRecentre = g_recentreCount;
    }

    // Just opened: put it a little under a meter in front of the head, facing it.
    if (!g_menuPlaced && g_framePoseValid)
    {
        const XrQuaternionf& q = g_frameHeadPose.orientation;
        float dx = -2 * (q.x * q.z + q.w * q.y), dz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        float length = sqrtf(dx * dx + dz * dz);
        if (length > 0.05f)
        {
            dx /= length;
            dz /= length;
            g_menuPosition[0] = g_frameHeadPose.position.x + dx * 0.95f;
            g_menuPosition[1] = g_frameHeadPose.position.y - 0.12f;
            g_menuPosition[2] = g_frameHeadPose.position.z + dz * 0.95f;
            g_menuYaw = atan2f(-dx, -dz);
            g_menuPlaced = true;
        }
    }

    int width = 0, height = 0;
    bool changed = false;
    const unsigned char* pixels = VrMenuPicture(width, height, changed);
    g_menuHeightMetres = kMenuWidthMetres * (float)height / (float)width;

    if (g_menuSwapchain == XR_NULL_HANDLE || width != g_menuSwapchainWidth || height != g_menuSwapchainHeight)
    {
        if (g_menuSwapchain != XR_NULL_HANDLE) XrDestroySwapchainGl(g_menuSwapchain);
        g_menuSwapchain = XR_NULL_HANDLE;
        g_menuHasPicture = false;

        // (Failed: tried again a second later, not every frame: on SteamVR it
        // can fail every frame.)
        static double retryAtMs = 0.0;
        if (NowMs() < retryAtMs) return;

        int64_t format = ChooseSwapchainFormat();
        if (format == 0) return;

        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = (uint32_t)width;
        info.height = (uint32_t)height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(XrCreateSwapchainGl(g_session, &info, &g_menuSwapchain), "Creating the BunkerVR menu pictures"))
        {
            g_menuSwapchain = XR_NULL_HANDLE;
            retryAtMs = NowMs() + 1000.0;
            return;
        }
        for (int i = 0; i < 16; ++i)
        {
            g_menuSwapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            g_menuSwapchainImages[i].next = nullptr;
            g_menuSwapchainImages[i].image = 0;
        }
        if (!Check(XrEnumerateSwapchainImagesGl(g_menuSwapchain, 16, &g_menuSwapchainImageCount,
                                              (XrSwapchainImageBaseHeader*)g_menuSwapchainImages),
                   "Getting the BunkerVR menu pictures"))
        {
            XrDestroySwapchainGl(g_menuSwapchain);
            g_menuSwapchain = XR_NULL_HANDLE;
            return;
        }
        g_menuSwapchainWidth = width;
        g_menuSwapchainHeight = height;
        Log("VR: created the BunkerVR menu pictures: %u of %d x %d", (unsigned)g_menuSwapchainImageCount, width, height);
    }

    // Hand over a new picture only when the menu looks different.
    if (!changed && g_menuHasPicture) return;

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(g_menuSwapchain, nullptr, &index), "Taking the next menu picture")) return;

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(g_menuSwapchain, &waitInfo), "Waiting for the menu picture");

    if (ready && index < g_menuSwapchainImageCount)
        GlUploadPicture(g_menuSwapchainImages[index].image, width, height, pixels);
    KeepForMonitor(pixels, width, height, g_monitorMenuTexture, g_monitorMenuFramebuffer, g_monitorMenuWidth, g_monitorMenuHeight);

    if (Check(XrReleaseSwapchainImageGl(g_menuSwapchain, nullptr), "Handing the menu picture back") && ready)
        g_menuHasPicture = true;
}

// ---------------------------------------------------------------------------
// A small picture in the room turned to face the head (the bullet counter, the
// hints' labels): its face towards the eyes, its rows along the head's own left
// and right, kept level (the head's roll left out), so it reads as a page held
// up to them from wherever it is. Don't take its rows square to where it lies
// round the head seen from above: for one below the head, beside the body (the
// pouch, the flashlight at the chest), that is the small sideways gap between
// it and the head, and it would be turned on its side. False if it is nearer
// the head than `nearest` or further than 3 m.
// ---------------------------------------------------------------------------

static bool PoseFacingHead(const float at[3], XrPosef& pose, float nearest)
{
    float z[3] = { g_frameHeadPose.position.x - at[0], g_frameHeadPose.position.y - at[1], g_frameHeadPose.position.z - at[2] };
    float length = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (length < nearest || length > 3.0f) return false;
    for (int i = 0; i < 3; ++i) z[i] /= length;

    // The head's right, level, made square to the way to the eyes.
    const XrQuaternionf& h = g_frameHeadPose.orientation;
    float rx = 1 - 2 * (h.y * h.y + h.z * h.z), rz = 2 * (h.x * h.z - h.w * h.y);
    float x[3] = { 0.0f, 0.0f, 0.0f };
    length = sqrtf(rx * rx + rz * rz);
    if (length > 0.25f)
    {
        rx /= length;
        rz /= length;
        float along = rx * z[0] + rz * z[2];
        x[0] = rx - along * z[0];
        x[1] = -along * z[1];
        x[2] = rz - along * z[2];
        length = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    }
    if (length > 0.2f)
    {
        for (int i = 0; i < 3; ++i) x[i] /= length;
    }
    else
    {
        // (The head's right points at it, straight to one side of the head, or
        // the head is on its side: level and square to where it lies.)
        x[0] = z[2];
        x[1] = 0.0f;
        x[2] = -z[0];
        length = sqrtf(x[0] * x[0] + x[2] * x[2]);
        if (length < 0.01f) return false; // seen from straight above or below
        x[0] /= length;
        x[2] /= length;
    }
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };

    // Those three directions as a quaternion.
    XrQuaternionf& q = pose.orientation;
    float trace = x[0] + y[1] + z[2];
    if (trace > 0.0f)
    {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y[2] - z[1]) / s;
        q.y = (z[0] - x[2]) / s;
        q.z = (x[1] - y[0]) / s;
    }
    else if (x[0] > y[1] && x[0] > z[2])
    {
        float s = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
        q.w = (y[2] - z[1]) / s;
        q.x = 0.25f * s;
        q.y = (y[0] + x[1]) / s;
        q.z = (z[0] + x[2]) / s;
    }
    else if (y[1] > z[2])
    {
        float s = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
        q.w = (z[0] - x[2]) / s;
        q.x = (y[0] + x[1]) / s;
        q.y = 0.25f * s;
        q.z = (z[1] + y[2]) / s;
    }
    else
    {
        float s = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
        q.w = (x[1] - y[0]) / s;
        q.x = (z[0] + x[2]) / s;
        q.y = (z[1] + y[2]) / s;
        q.z = 0.25f * s;
    }
    pose.position.x = at[0];
    pose.position.y = at[1];
    pose.position.z = at[2];
    return true;
}

// ---------------------------------------------------------------------------
// The bullet counter: a small tag with a number, floating at the ammo pouch on
// the left hip or above the open revolver (the "Bullet count shown" setting).
// ---------------------------------------------------------------------------

static XrSwapchain g_counterSwapchain = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLKHR g_counterSwapchainImages[16];
static uint32_t g_counterSwapchainImageCount = 0;
static int g_counterSwapchainWidth = 0, g_counterSwapchainHeight = 0;
static char g_counterShownText[24] = ""; // what the picture in the headset's hands says
static XrPosef g_counterPose = {};
static float g_counterWidthMetres = 0.07f;

// Decides whether the counter shows this frame, where, and what it says, and
// hands the headset a new picture when the number has changed. Call with
// OpenGL active. Returns true if it is to be shown.
static bool PrepareAmmoCounter()
{
    int loaded = 0, spare = 0;
    if (!CameraRevolverCounts(loaded, spare) || !g_framePoseValid || VrMenuIsOpen() || CameraGamePaused()) return false;

    char text[24];
    float at[3];
    bool shotgunCheck = CameraShotgunHeld() && g_shotgunCheckHeld && CameraShotgunCheckShown() && g_aimValid[1];
    if (shotgunCheck)
    {
        // The shotgun's press-check (see g_shotgunCheckHeld): above its receiver.
        // The game's own picture of a shell with "x" and how many are loaded, in
        // the game's lettering, as at the pouch, so it matches the game's look.
        const XrQuaternionf& q = g_aimRoom[1].orientation;
        float d[3] = { -2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y)) };
        at[0] = g_aimRoom[1].position.x + d[0] * 0.12f;
        at[1] = g_aimRoom[1].position.y + d[1] * 0.12f + 0.08f;
        at[2] = g_aimRoom[1].position.z + d[2] * 0.12f;
        wsprintfA(text, "loaded %d", loaded);
        g_counterWidthMetres = 0.08f;
    }
    else
    {
        // At the pouch: the spare bullets, while the revolver is open or the
        // left hand is near the pouch. The game's own picture of them with its
        // "x3" (VrPouchPicture); the game's own count on the screen is left out
        // in the headset (DoCounterGui in ItemHandler.hps). Its middle is the
        // pouch's spot itself, where the hand takes them.
        if (!g_pouchValid || !(g_reloadKeyDown || g_leftNearPouch)) return false;
        at[0] = g_pouchRoom[0];
        at[1] = g_pouchRoom[1];
        at[2] = g_pouchRoom[2];
        wsprintfA(text, CameraShotgunHeld() ? "shells %d" : "pouch %d", spare); // (the shotgun's: its shell's picture)
        g_counterWidthMetres = 0.08f;
    }

    // Turned to face the head.
    if (!PoseFacingHead(at, g_counterPose, 0.05f)) return false;

    int width = 0, height = 0;
    bool textChanged = strcmp(text, g_counterShownText) != 0;
    if (g_counterSwapchain != XR_NULL_HANDLE && !textChanged) return true;

    // (The game's picture, square; should the game's files not be read, the tag with the number.)
    const unsigned char* pixels = shotgunCheck ? VrPouchPicture(loaded, true, width, height)
                                               : VrPouchPicture(spare, strncmp(text, "shells", 6) == 0, width, height);
    if (!pixels)
    {
        char number[24];
        wsprintfA(number, "%d", shotgunCheck ? loaded : spare);
        pixels = VrCounterPicture(number, width, height);
    }
    if (g_counterSwapchain != XR_NULL_HANDLE && (width != g_counterSwapchainWidth || height != g_counterSwapchainHeight))
    {
        // (Another size: the game's picture and the tag differ.)
        XrDestroySwapchainGl(g_counterSwapchain);
        g_counterSwapchain = XR_NULL_HANDLE;
    }
    if (g_counterSwapchain == XR_NULL_HANDLE)
    {
        int64_t format = ChooseSwapchainFormat();
        if (format == 0) return false;

        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = (uint32_t)width;
        info.height = (uint32_t)height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(XrCreateSwapchainGl(g_session, &info, &g_counterSwapchain), "Creating the bullet counter's pictures"))
        {
            g_counterSwapchain = XR_NULL_HANDLE;
            return false;
        }
        for (int i = 0; i < 16; ++i)
        {
            g_counterSwapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            g_counterSwapchainImages[i].next = nullptr;
            g_counterSwapchainImages[i].image = 0;
        }
        if (!Check(XrEnumerateSwapchainImagesGl(g_counterSwapchain, 16, &g_counterSwapchainImageCount,
                                              (XrSwapchainImageBaseHeader*)g_counterSwapchainImages),
                   "Getting the bullet counter's pictures"))
        {
            XrDestroySwapchainGl(g_counterSwapchain);
            g_counterSwapchain = XR_NULL_HANDLE;
            return false;
        }
        g_counterSwapchainWidth = width;
        g_counterSwapchainHeight = height;
        g_counterShownText[0] = 0;
        Log("VR: created the bullet counter's pictures: %u of %d x %d", (unsigned)g_counterSwapchainImageCount, width, height);
    }

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(g_counterSwapchain, nullptr, &index), "Taking the next bullet counter picture"))
        return g_counterShownText[0] != 0;

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(g_counterSwapchain, &waitInfo), "Waiting for the bullet counter picture");

    if (ready && index < g_counterSwapchainImageCount)
        GlUploadPicture(g_counterSwapchainImages[index].image, width, height, pixels);
    KeepForMonitor(pixels, width, height, g_monitorCounterTexture, g_monitorCounterFramebuffer, g_monitorCounterWidth, g_monitorCounterHeight);

    if (Check(XrReleaseSwapchainImageGl(g_counterSwapchain, nullptr), "Handing the bullet counter picture back") && ready)
    {
        strcpy(g_counterShownText, text);
        static int logsLeft = 6;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: the bullet counter shows \"%s\" (%s).", text, shotgunCheck ? "above the shotgun, its press-check" : "at the pouch");
        }
    }
    return g_counterShownText[0] != 0;
}

// ---------------------------------------------------------------------------
// The hints on the objects (hints.h): how to use each mechanic, shown on the
// object itself. One label for each hand: a small picture in the game's
// lettering (VrHintPicture) a little above the spot the hand is at, facing the
// head, faded in once the hand has been at it a moment and out as it leaves.
// Which hint: the places on the body first (the holster, the right chest's
// item slot, the pouch, the flashlight at the chest; HintAtBody), then what
// the scripts say the hand is at (cVR_Hint), then the mod's own for what the
// hand holds (the revolver, the bandage, the grenade, the lighter, a thing to
// throw). Used while it shows (its button pressed, or told: HintUsed), a label
// goes until the hand has left that hint a moment; used a few times, it stops
// coming (Until Learned, Settings::interactionHints). As layers of their own
// over the world, as the bullet counter is.
// ---------------------------------------------------------------------------

struct HintLabel
{
    const HintInfo* shown = nullptr;    // what it says (null: nothing yet)
    float alpha = 0.0f;                 // how far it is faded in, 0 to 1
    const HintInfo* wanted = nullptr;   // what it should say now
    double wantedSinceMs = 0.0;         // ...since when
    const HintInfo* lastWant = nullptr; // what the hand was at in the frame before (before a use left it out)
    float at[3] = { 0, 0, 0 };          // where, in the room (the spot, raised by the hint's lift)
    bool atKnown = false;
    const HintInfo* doneWith = nullptr; // used while it showed: left out until the hand is away from it a moment
    double doneAwaySinceMs = -1.0;
    double toldSeenMs = -100000.0;      // the last HintUsed looked at
    bool gripWas = false, triggerWas = false;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSwapchainImageOpenGLKHR images[16] = {};
    uint32_t imageCount = 0;
    bool failed = false;
    double retryAtMs = 0.0;             // (failed: when to try again)
    unsigned char* picture = nullptr;   // the words at full strength (its own copy)
    unsigned char* faded = nullptr;     // as handed to the headset
    int width = 0, height = 0;
    int inkFrom = 0, inkTo = 0;         // the picture's columns with anything in them (the rest stays clear)
    const HintInfo* pictureOf = nullptr;
    int uploadedLevel = -1;
    const HintInfo* uploadedOf = nullptr;
    XrPosef pose = {};
    int logsLeft = 60;
    // The faded picture again, for the monitor view (so a recording of the PC
    // window shows the labels too), and whether the headset was given the
    // label this frame (so the monitor draws it too).
    GLuint monitorTexture = 0, monitorFramebuffer = 0;
    int monitorWidth = 0, monitorHeight = 0;
    bool layeredNow = false;
};
static HintLabel g_hintLabels[2];
static const float kHintLabelWidth = 0.40f;   // meters, for the picture's whole width (1024 pixels: the letters about 1.3 cm a line)
// The labels' size as the Accessibility page's Hint Size has it (Settings::hintSize): their width, and how
// far they are lifted above (or put below) the spot, by the same factor, so a bigger label keeps its gap to the thing.
static float HintSizeScale()
{
    static const float kScales[4] = { 0.8f, 1.0f, 1.25f, 1.5f };
    int size = GetSettings().hintSize;
    return kScales[size < 0 ? 0 : (size > 3 ? 3 : size)];
}
static float HintLabelWidth() { return kHintLabelWidth * HintSizeScale(); }
static const double kHintSettleMs = 150.0;    // a hint is to be wanted this long before it shows (no flicker while the hand sweeps past things)
static const float kHintFadeIn = 0.20f, kHintFadeOut = 0.15f; // seconds
static const int kHintFadeSteps = 5;          // the fade in steps, each a new picture for the headset

// A place measured as the head and hands are handed to the game (from the
// game's camera, level: CameraWorldToPlayer) back in the room: ToRelativePose
// the other way.
static void PlayerToRoom(const float player[3], float room[3])
{
    float scale = GetSettings().worldScale > 0.01f ? GetSettings().worldScale : 1.0f;
    float c = cosf(g_referenceYaw), s = sinf(g_referenceYaw);
    float dx = c * player[0] + s * player[2];
    float dz = -s * player[0] + c * player[2];
    room[0] = g_referencePosition[0] + dx / scale;
    room[1] = g_referencePosition[1] + player[1] / scale - GetSettings().heightOffset;
    room[2] = g_referencePosition[2] + dz / scale;
}

// The hint the mod finds itself for a hand at a place on the body (true), with
// the place in the room.
static bool HintAtBody(int hand, int carried, const HintInfo*& info, float room[3])
{
    info = nullptr;
    bool gunMode = CameraGunHeld(), shotgun = CameraShotgunHeld();
    int rightKind = CameraRightItemKind(), leftKind = CameraLeftItemKind();
    if (hand == 1)
    {
        if (g_rightInHolster && g_holsterRoomKnown && !g_reloadKeyDown)
        {
            if (gunMode && !shotgun) info = HintFind("holster_put");
            else if (!gunMode && rightKind == kItemNone && (carried & kCarryRevolver)) info = HintFind("holster_draw");
            for (int i = 0; i < 3; ++i) room[i] = g_holsterRoomNow[i];
        }
        else if (g_rightAtChestSlot && !g_slotDone && !gunMode && !g_reloadKeyDown && !RightFittedNow() && !CameraInventoryOpen())
        {
            // The right chest's item slot (as UpdateGrabInput decides it).
            if (rightKind == kItemNone && !g_handDown[1] && (carried & kCarryChestItem) && CameraTouchKind(1) == kTouchNothing) info = HintFind("slot_take");
            else if (rightKind != kItemNone && (carried & kCarryChestTakes) && !(rightKind == kItemGrenade && g_grenadeArmed) &&
                     !(rightKind == kItemHealth && g_bandagePhase == 1))
                info = HintFind(g_throwHeld || (rightKind == kItemGrenade && g_handUse[1] == kHandGrenade) ? "slot_let_go" : "slot_put");
            for (int i = 0; i < 3; ++i) room[i] = g_chestSlotRoom[i];
        }
    }
    else
    {
        if (g_leftAtLightHolster && g_pouchValid && leftKind == kItemNone && g_handUse[0] != kHandBullet)
        {
            if (g_reloadKeyDown && (carried & kCarryBullets)) info = HintFind("pouch_bullet");
            else if (gunMode && shotgun && (carried & kCarryShells)) info = HintFind("pouch_shell");
            for (int i = 0; i < 3; ++i) room[i] = g_pouchRoom[i];
        }
        else if (g_leftAtChestLight)
        {
            if (leftKind == kItemFlashlight) info = HintFind("light_put");
            else if (leftKind == kItemNone && (carried & kCarryFlashlight)) info = HintFind("light_take");
            for (int i = 0; i < 3; ++i) room[i] = g_chestLightRoom[i];
        }
    }
    return info != nullptr;
}

// The hint the mod finds itself for what a hand holds (true), with the hand's
// place in the room (or the left forearm's, for the bandage's wrapping).
static bool HintForHeld(int hand, const HintInfo*& info, float room[3])
{
    info = nullptr;
    if (!g_hintGripKnown[hand]) return false;
    const XrVector3f& p = g_hintGrip[hand].position;
    room[0] = p.x;
    room[1] = p.y;
    room[2] = p.z;
    bool gunMode = CameraGunHeld(), shotgun = CameraShotgunHeld();
    int rightKind = CameraRightItemKind(), leftKind = CameraLeftItemKind();
    if (hand == 0)
    {
        if (leftKind == kItemLighter) info = HintFind("lighter");
        return info != nullptr;
    }
    if (gunMode)
    {
        if (shotgun) return false;
        int loaded = 0, spare = 0;
        bool counts = CameraRevolverCounts(loaded, spare);
        if (g_reloadKeyDown)
        {
            if (CameraRevolverHasSpent()) info = HintFind("gun_tip");
            else if (counts && (loaded >= 6 || spare <= 0) && g_handUse[0] != kHandBullet) info = HintFind("gun_close");
        }
        else if (!g_gunButtonBusy)
        {
            if (counts && loaded == 0) info = HintFind("gun_open");
            else info = HintFind("gun_fire");
        }
        return info != nullptr;
    }
    if (rightKind == kItemHealth)
    {
        if (g_bandagePhase == 0) info = HintFind("bandage_open");
        else if (g_bandagePhase == 1 && g_hintGripKnown[0])
        {
            // At the left forearm: 14 cm back from the left palm along the
            // controller (as the winding measures it).
            const XrQuaternionf& q = g_hintGrip[0].orientation;
            float back[3] = { 2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y) };
            const XrVector3f& left = g_hintGrip[0].position;
            room[0] = left.x + back[0] * 0.14f;
            room[1] = left.y + back[1] * 0.14f;
            room[2] = left.z + back[2] * 0.14f;
            info = HintFind("bandage_wind");
        }
        return info != nullptr;
    }
    if (rightKind == kItemGrenade)
    {
        info = HintFind(g_grenadeArmed ? "throw" : "pull_pin");
        return true;
    }
    if (rightKind == kItemLighter)
    {
        info = HintFind("lighter");
        return true;
    }
    if ((CameraRightItemMoves(nullptr) & kMoveThrow) != 0)
    {
        info = HintFind("throw");
        return true;
    }
    return false;
}

static bool EnsureHintSwapchain(HintLabel& label, int width, int height)
{
    if (label.swapchain != XR_NULL_HANDLE) return true;
    // (Failed: tried again a second later, not every frame. Never give up for the
    // run: on SteamVR it can fail for a while, and both hands' labels would be gone.)
    if (label.failed && NowMs() < label.retryAtMs) return false;
    label.failed = true;
    label.retryAtMs = NowMs() + 1000.0;
    int64_t format = ChooseSwapchainFormat();
    if (format == 0) return false;
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(XrCreateSwapchainGl(g_session, &info, &label.swapchain), "Creating a hint's pictures"))
    {
        label.swapchain = XR_NULL_HANDLE;
        return false;
    }
    for (int i = 0; i < 16; ++i)
    {
        label.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        label.images[i].next = nullptr;
        label.images[i].image = 0;
    }
    if (!Check(XrEnumerateSwapchainImagesGl(label.swapchain, 16, &label.imageCount, (XrSwapchainImageBaseHeader*)label.images),
               "Getting a hint's pictures"))
    {
        XrDestroySwapchainGl(label.swapchain);
        label.swapchain = XR_NULL_HANDLE;
        return false;
    }
    label.width = width;
    label.height = height;
    label.failed = false;
    Log("HINTS: made a label's pictures: %u of %d x %d.", (unsigned)label.imageCount, width, height);
    return true;
}

// Hands the headset the label's picture at its present fade, if that has
// changed. False if there is nothing to show.
static bool SendHintPicture(HintLabel& label)
{
    int level = (int)(label.alpha * kHintFadeSteps + 0.5f);
    if (level <= 0 || !label.shown) return false;
    if (label.pictureOf != label.shown)
    {
        int width = 0, height = 0;
        const unsigned char* pixels = VrHintPicture(label.shown->words, width, height);
        if (!pixels || width <= 0 || height <= 0) return false;
        if (!label.picture)
        {
            label.picture = new unsigned char[(size_t)width * height * 4];
            label.faded = new unsigned char[(size_t)width * height * 4];
        }
        if (!EnsureHintSwapchain(label, width, height) || width != label.width || height != label.height) return false;
        memcpy(label.picture, pixels, (size_t)width * height * 4);
        memset(label.faded, 0, (size_t)width * height * 4);
        // The columns with anything in them.
        label.inkFrom = width;
        label.inkTo = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (pixels[((size_t)y * width + x) * 4 + 3] != 0)
                {
                    if (x < label.inkFrom) label.inkFrom = x;
                    if (x + 1 > label.inkTo) label.inkTo = x + 1;
                }
        label.pictureOf = label.shown;
        label.uploadedLevel = -1;
    }
    if (label.swapchain == XR_NULL_HANDLE) return false;
    if (level == label.uploadedLevel && label.uploadedOf == label.shown) return true;

    // (The colors are stored multiplied by how solid they are: all four fade
    // together, by a table, over the columns with anything in them.)
    static unsigned char fadeTable[kHintFadeSteps + 1][256];
    static bool tableMade = false;
    if (!tableMade)
    {
        tableMade = true;
        for (int l = 0; l <= kHintFadeSteps; ++l)
            for (int v = 0; v < 256; ++v) fadeTable[l][v] = (unsigned char)(v * l / kHintFadeSteps);
    }
    const unsigned char* table = fadeTable[level > kHintFadeSteps ? kHintFadeSteps : level];
    if (label.inkTo > label.inkFrom)
    {
        size_t rowBytes = (size_t)(label.inkTo - label.inkFrom) * 4;
        for (int y = 0; y < label.height; ++y)
        {
            size_t at = ((size_t)y * label.width + label.inkFrom) * 4;
            const unsigned char* from = label.picture + at;
            unsigned char* to = label.faded + at;
            for (size_t i = 0; i < rowBytes; ++i) to[i] = table[from[i]];
        }
    }
    KeepForMonitor(label.faded, label.width, label.height, label.monitorTexture, label.monitorFramebuffer,
                   label.monitorWidth, label.monitorHeight); // (the same, for the monitor)

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(label.swapchain, nullptr, &index), "Taking a hint's next picture")) return label.uploadedLevel > 0;
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(label.swapchain, &waitInfo), "Waiting for a hint's picture");
    if (ready && index < label.imageCount) GlUploadPicture(label.images[index].image, label.width, label.height, label.faded);
    if (Check(XrReleaseSwapchainImageGl(label.swapchain, nullptr), "Handing a hint's picture back") && ready)
    {
        label.uploadedLevel = level;
        label.uploadedOf = label.shown;
    }
    return label.uploadedLevel > 0 && label.uploadedOf == label.shown;
}

// Each frame (from the frame's end, before its layers): what each hand's label
// says, where and how strongly. `shown`: the world is shown this frame.
static void UpdateHints(bool shown, double nowMs)
{
    static double lastMs = 0.0;
    float step = lastMs > 0.0 ? (float)((nowMs - lastMs) / 1000.0) : 0.0f;
    if (step < 0.0f || step > 0.1f) step = 0.0f;
    lastMs = nowMs;

    bool allowed = shown && GetSettings().interactionHints != 2 && !VrMenuIsOpen() && !CameraGamePaused() && !CameraInventoryOpen() &&
                   !CameraTheatre() && !StartupRunning() && g_framePoseValid;
    int carried = HintCarriedFlags(nowMs);
    double toldMs = 0.0;
    const HintInfo* told = HintLastUsed(toldMs);

    for (int hand = 0; hand < 2; ++hand)
    {
        HintLabel& label = g_hintLabels[hand];

        // What the hand is at.
        const HintInfo* want = nullptr;
        float room[3] = { 0, 0, 0 };
        bool placed = false;
        if (allowed)
        {
            float world[3];
            if (HintAtBody(hand, carried, want, room)) placed = true;
            else if (HintScriptSays(hand, nowMs, want, world))
            {
                float player[3];
                placed = CameraWorldToPlayer(world, player);
                if (placed) PlayerToRoom(player, room);
                else want = nullptr;
            }
            else if (HintForHeld(hand, want, room)) placed = true;
        }

        // Used: its button pressed while the hand is at what it is for, its
        // label up yet or not ("until used": a player who knows does not wait
        // for it, and a quick squeeze before the label is half in still
        // counts), or told (HintUsed). The press is looked at before what it
        // changes is (a door taken leaves the touch).
        bool gripDown = g_gripValue[hand] > 0.6f, triggerDown = g_triggerValue[hand] > 0.6f;
        bool gripPressed = gripDown && !label.gripWas, triggerPressed = triggerDown && !label.triggerWas;
        label.gripWas = g_gripValue[hand] > 0.35f && (label.gripWas || gripDown);
        label.triggerWas = g_triggerValue[hand] > 0.35f && (label.triggerWas || triggerDown);
        // (What the hand was at in the frame before comes first: the squeeze that
        // takes hold of a loose thing has the scripts say "Throw" for that hand
        // in the very update the press is first seen, so "Pick up" would never
        // count.)
        const HintInfo* atNow[3] = { label.lastWant, want, label.shown && label.alpha > 0.0f ? label.shown : nullptr };
        bool pressedAny = false;
        for (int c = 0; c < 3 && !pressedAny; ++c)
        {
            const HintInfo* candidate = atNow[c];
            if (!candidate || candidate == label.doneWith || !HintWanted(candidate)) continue;
            int use = candidate->use;
            bool pressed = (use == kHintUseSqueeze && (gripPressed || triggerPressed)) || (use == kHintUseGrip && gripPressed) ||
                           (use == kHintUseTrigger && triggerPressed);
            if (!pressed) continue;
            pressedAny = true;
            HintUsed(candidate, nowMs);
            label.doneWith = candidate;
            label.doneAwaySinceMs = -1.0;
        }
        for (int c = 1; c < 3 && !pressedAny; ++c)
        {
            if (!atNow[c] || told != atNow[c] || toldMs <= label.toldSeenMs) continue;
            label.doneWith = atNow[c];
            label.doneAwaySinceMs = -1.0;
            break;
        }
        if (toldMs > label.toldSeenMs) label.toldSeenMs = toldMs;
        label.lastWant = want;

        // A hint used: left out until the hand has been away from it 0.6 s.
        if (label.doneWith)
        {
            if (want == label.doneWith) label.doneAwaySinceMs = -1.0;
            else if (label.doneAwaySinceMs < 0.0) label.doneAwaySinceMs = nowMs;
            else if (nowMs - label.doneAwaySinceMs > 600.0) label.doneWith = nullptr;
        }
        if (want && (want == label.doneWith || !HintWanted(want))) want = nullptr;

        if (want != label.wanted)
        {
            label.wanted = want;
            label.wantedSinceMs = nowMs;
        }
        if (want && placed)
        {
            float lift = want->lift * HintSizeScale(); // (with the label's size)
            label.at[0] = room[0];
            label.at[1] = room[1] + lift;
            label.at[2] = room[2];
            label.atKnown = true;
        }

        // Faded out before another comes; in once wanted a moment.
        if (label.shown && label.shown != label.wanted)
        {
            label.alpha -= step / kHintFadeOut;
            if (label.alpha <= 0.0f)
            {
                label.alpha = 0.0f;
                label.shown = nullptr;
            }
        }
        else if (!label.shown && label.wanted && nowMs - label.wantedSinceMs >= kHintSettleMs)
        {
            label.shown = label.wanted;
            label.alpha = 0.0f;
            if (label.logsLeft > 0)
            {
                --label.logsLeft;
                Log("HINTS: the %s hand's label shows \"%s\" (%s; used %d time%s).", hand == 0 ? "left" : "right", label.shown->key,
                    label.shown->words, HintTimesUsed(label.shown), HintTimesUsed(label.shown) == 1 ? "" : "s");
            }
        }
        else if (label.shown)
        {
            label.alpha += step / kHintFadeIn;
            if (label.alpha > 1.0f) label.alpha = 1.0f;
        }
        if (label.shown && (!label.atKnown || !PoseFacingHead(label.at, label.pose, 0.12f))) label.alpha = 0.0f; // (in the face: not shown)
    }
}

// The labels' layers, after the bullet counter's.
static void AddHintLayers(XrCompositionLayerQuad quads[2], const XrCompositionLayerBaseHeader** layers, uint32_t& layerCount, uint32_t room)
{
    for (int hand = 0; hand < 2 && room > 0; ++hand)
    {
        HintLabel& label = g_hintLabels[hand];
        if (!label.shown || label.alpha <= 0.0f || !SendHintPicture(label)) continue;
        label.layeredNow = true;
        XrCompositionLayerQuad& quad = quads[hand];
        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = g_floorSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = label.swapchain;
        quad.subImage.imageRect.offset.x = 0;
        quad.subImage.imageRect.offset.y = 0;
        quad.subImage.imageRect.extent.width = label.width;
        quad.subImage.imageRect.extent.height = label.height;
        quad.subImage.imageArrayIndex = 0;
        quad.pose = label.pose;
        quad.size.width = HintLabelWidth();
        quad.size.height = HintLabelWidth() * (float)label.height / (float)label.width;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
        --room;
    }
}

// ---------------------------------------------------------------------------
// The pointer in menus (pointer.h), in the style of native VR games: a glowing
// orb at each hand, and for the hand that points a beam to the panel and a
// cursor where it lands, with soft edges and a buzz on hover. Each is a small
// layer of its own, laid over everything. (Drawn into the eyes' pictures they
// would be behind the panels, and a solid panel would hide the cursor.) The
// cursor swells while over something to press (on the game's menus, for a
// moment as a button comes into focus: the mod cannot tell how long the
// pointer stays on it) and pulses as it is pressed. Nothing here moves the
// game's mouse (UpdateMenuPointer, UpdateVrMenu).
// ---------------------------------------------------------------------------

struct PointerSwapchain
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int width = 0, height = 0;
    bool ready = false, failed = false;
    double retryAtMs = 0.0; // (failed: when to try again)
    GLuint monitorTexture = 0, monitorFramebuffer = 0; // (the same picture, for the monitor)
    int monitorWidth = 0, monitorHeight = 0;
};
static PointerSwapchain g_pointerPictures[kPointerPictureCount];

// The picture made and handed to the headset, once: it never changes. Each
// picture of the swapchain's set gets it, so whichever is shown is right.
static bool EnsurePointerPicture(int kind)
{
    PointerSwapchain& p = g_pointerPictures[kind];
    if (p.ready) return true;
    // (Failed: tried again a second later, not every frame. Never give up for the
    // run: on SteamVR it can fail for a while, and a piece of the pointer would be gone.)
    if (p.failed && NowMs() < p.retryAtMs) return false;
    p.failed = true;
    p.retryAtMs = NowMs() + 1000.0;
    int width = 0, height = 0;
    const unsigned char* pixels = PointerPicture(kind, width, height);
    int64_t format = ChooseSwapchainFormat();
    if (format == 0) return false;

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(XrCreateSwapchainGl(g_session, &info, &p.swapchain), "Creating a picture of the pointer"))
    {
        p.swapchain = XR_NULL_HANDLE;
        return false;
    }
    XrSwapchainImageOpenGLKHR images[16];
    for (int i = 0; i < 16; ++i)
    {
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        images[i].next = nullptr;
        images[i].image = 0;
    }
    uint32_t count = 0;
    bool all = Check(XrEnumerateSwapchainImagesGl(p.swapchain, 16, &count, (XrSwapchainImageBaseHeader*)images),
                     "Getting the pointer's pictures");
    for (uint32_t n = 0; all && n < count; ++n)
    {
        uint32_t index = 0;
        if (!Check(XrAcquireSwapchainImageGl(p.swapchain, nullptr, &index), "Taking a picture of the pointer"))
        {
            all = false;
            break;
        }
        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (Check(XrWaitSwapchainImageGl(p.swapchain, &waitInfo), "Waiting for a picture of the pointer") && index < count)
            GlUploadPicture(images[index].image, width, height, pixels);
        else
            all = false;
        if (!Check(XrReleaseSwapchainImageGl(p.swapchain, nullptr), "Handing a picture of the pointer back")) all = false;
    }
    if (!all || count == 0)
    {
        XrDestroySwapchainGl(p.swapchain);
        p.swapchain = XR_NULL_HANDLE;
        return false;
    }
    KeepForMonitor(pixels, width, height, p.monitorTexture, p.monitorFramebuffer, p.monitorWidth, p.monitorHeight);
    p.width = width;
    p.height = height;
    p.ready = true;
    p.failed = false;
    Log("VR: created a picture of the pointer in menus (number %d: %d x %d, %u in the set).", kind, width, height, (unsigned)count);
    return true;
}

// A turn from three directions, each of length 1 and at right angles to the
// others: the layer's across, up and towards-you.
static XrQuaternionf TurnFromAxes(const float x[3], const float y[3], const float z[3])
{
    XrQuaternionf q;
    float trace = x[0] + y[1] + z[2];
    if (trace > 0.0f)
    {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y[2] - z[1]) / s;
        q.y = (z[0] - x[2]) / s;
        q.z = (x[1] - y[0]) / s;
    }
    else if (x[0] > y[1] && x[0] > z[2])
    {
        float s = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;
        q.w = (y[2] - z[1]) / s;
        q.x = 0.25f * s;
        q.y = (y[0] + x[1]) / s;
        q.z = (z[0] + x[2]) / s;
    }
    else if (y[1] > z[2])
    {
        float s = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;
        q.w = (z[0] - x[2]) / s;
        q.x = (y[0] + x[1]) / s;
        q.y = 0.25f * s;
        q.z = (z[1] + y[2]) / s;
    }
    else
    {
        float s = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;
        q.w = (x[1] - y[0]) / s;
        q.x = (z[0] + x[2]) / s;
        q.y = (z[1] + y[2]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// What is shown this frame, in the order it is laid on: the beams, the
// cursors, then the orbs, the further first. (Kept for the monitor too.)
struct PointerPiece
{
    int kind;
    XrPosef pose;
    float width, height;
};
static PointerPiece g_pointerPieces[6];
static int g_pointerPieceCount = 0;
static XrCompositionLayerQuad g_pointerQuads[6];

// The pieces for this frame (g_pointerPieces), from the hands as last read.
static void PlacePointerPieces()
{
    g_pointerPieceCount = 0;
    double now = NowMs();
    static double lastMs = 0.0;
    float seconds = lastMs > 0.0 ? (float)((now - lastMs) / 1000.0) : 0.0f;
    if (seconds > 0.1f) seconds = 0.1f;
    lastMs = now;

    const XrVector3f& head = g_frameHeadPose.position;
    bool modMenu = VrMenuIsOpen() && g_menuPlaced;
    float panelYaw = modMenu ? g_menuYaw : g_panelYaw;
    float normal[3] = { sinf(panelYaw), 0.0f, cosf(panelYaw) }; // (the panel's face, towards you)

    // The cursor's size: swelling over something to press, pulsing as it is pressed.
    static float grow = 1.0f;
    bool over = modMenu ? VrMenuPointerOnButton() : now - g_gameMenuFocusMs < 400.0;
    float target = over ? 1.3f : 1.0f;
    grow += (target - grow) * fminf(1.0f, seconds * 12.0f);
    double pressMs = modMenu ? VrMenuLastPressMs() : g_gameMenuPressMs;
    float pulse = 1.0f + 0.35f * fmaxf(0.0f, 1.0f - (float)(now - pressMs) / 220.0f);

    PointerPiece beams[2], cursors[2], orbs[2];
    float orbDistance[2] = { 0, 0 };
    int beamCount = 0, cursorCount = 0, orbCount = 0;
    for (int hand = 0; hand < 2; ++hand)
    {
        if (!g_aimValid[hand]) continue;
        const XrPosef& aim = g_aimRoom[hand];
        const XrQuaternionf& q = aim.orientation;
        float d[3] = { -2 * (q.x * q.z + q.w * q.y), -2 * (q.y * q.z - q.w * q.x), -(1 - 2 * (q.x * q.x + q.y * q.y)) };
        float o[3] = { aim.position.x, aim.position.y, aim.position.z };
        bool steering = hand == g_pointerHand;
        bool pointing = g_menuPointerNow && (steering || (g_pointerActive && g_pointerHit[hand]));
        float length = steering ? g_pointerLength : g_pointerHitLength[hand];

        if (pointing && length > 0.05f)
        {
            float hit[3] = { o[0] + d[0] * length, o[1] + d[1] * length, o[2] + d[2] * length };

            // The beam: from just past the orb's core to the panel, its first rows
            // at the hand, turned about its own line to face the head.
            float from[3] = { o[0] + d[0] * 0.012f, o[1] + d[1] * 0.012f, o[2] + d[2] * 0.012f };
            float y[3] = { from[0] - hit[0], from[1] - hit[1], from[2] - hit[2] };
            float beamLength = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
            float mid[3] = { (from[0] + hit[0]) * 0.5f, (from[1] + hit[1]) * 0.5f, (from[2] + hit[2]) * 0.5f };
            if (beamLength > 0.02f)
            {
                for (int i = 0; i < 3; ++i) y[i] /= beamLength;
                float toHead[3] = { head.x - mid[0], head.y - mid[1], head.z - mid[2] };
                float along = toHead[0] * y[0] + toHead[1] * y[1] + toHead[2] * y[2];
                float z[3] = { toHead[0] - y[0] * along, toHead[1] - y[1] * along, toHead[2] - y[2] * along };
                float zLength = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
                if (zLength > 0.001f)
                {
                    for (int i = 0; i < 3; ++i) z[i] /= zLength;
                    float x[3] = { y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0] };
                    PointerPiece& beam = beams[beamCount++];
                    beam.kind = steering ? kPointerBeam : kPointerBeamDim;
                    beam.pose.orientation = TurnFromAxes(x, y, z);
                    beam.pose.position = { mid[0], mid[1], mid[2] };
                    beam.width = steering ? 0.012f : 0.009f;
                    beam.height = beamLength;
                }
            }

            // The cursor: lying on the panel, a few millimeters in front of it.
            PointerPiece& cursor = cursors[cursorCount++];
            cursor.kind = steering ? kPointerCursor : kPointerCursorDim;
            cursor.pose.orientation = { 0.0f, sinf(panelYaw * 0.5f), 0.0f, cosf(panelYaw * 0.5f) };
            cursor.pose.position = { hit[0] + normal[0] * 0.004f, hit[1], hit[2] + normal[2] * 0.004f };
            cursor.width = cursor.height = steering ? 0.022f * grow * pulse : 0.014f;
        }

        // The orb, at the controller's tip, facing the head.
        if (g_handsInMenuNow || pointing)
        {
            float z[3] = { head.x - o[0], head.y - o[1], head.z - o[2] };
            float zLength = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
            if (zLength < 0.03f) continue;
            for (int i = 0; i < 3; ++i) z[i] /= zLength;
            float x[3] = { z[2], 0.0f, -z[0] }; // level, to the right as seen
            float xLength = sqrtf(x[0] * x[0] + x[2] * x[2]);
            if (xLength < 0.01f) { x[0] = 1.0f; x[2] = 0.0f; xLength = 1.0f; }
            x[0] /= xLength;
            x[2] /= xLength;
            float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
            PointerPiece& orb = orbs[orbCount];
            orbDistance[orbCount++] = zLength;
            bool bright = pointing && steering;
            orb.kind = bright ? kPointerOrb : kPointerOrbDim;
            orb.pose.orientation = TurnFromAxes(x, y, z);
            orb.pose.position = aim.position;
            orb.width = orb.height = bright ? 0.045f : 0.035f;
        }
    }
    for (int i = 0; i < beamCount; ++i) g_pointerPieces[g_pointerPieceCount++] = beams[i];
    for (int i = 0; i < cursorCount; ++i) g_pointerPieces[g_pointerPieceCount++] = cursors[i];
    if (orbCount == 2 && orbDistance[1] > orbDistance[0]) // (the nearer laid on last)
    {
        g_pointerPieces[g_pointerPieceCount++] = orbs[1];
        g_pointerPieces[g_pointerPieceCount++] = orbs[0];
    }
    else
        for (int i = 0; i < orbCount; ++i) g_pointerPieces[g_pointerPieceCount++] = orbs[i];
}

// Adds this frame's pointer layers (as many as `room` allows).
static void AddPointerLayers(const XrCompositionLayerBaseHeader** layers, uint32_t& layerCount, uint32_t room)
{
    PlacePointerPieces();
    int kept = 0;
    for (int i = 0; i < g_pointerPieceCount; ++i)
    {
        const PointerPiece& piece = g_pointerPieces[i];
        if (room == 0 || !EnsurePointerPicture(piece.kind)) continue;
        const PointerSwapchain& picture = g_pointerPictures[piece.kind];
        XrCompositionLayerQuad& quad = g_pointerQuads[kept];
        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = g_floorSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = picture.swapchain;
        quad.subImage.imageRect.offset.x = 0;
        quad.subImage.imageRect.offset.y = 0;
        quad.subImage.imageRect.extent.width = picture.width;
        quad.subImage.imageRect.extent.height = picture.height;
        quad.subImage.imageArrayIndex = 0;
        quad.pose = piece.pose;
        quad.size.width = piece.width;
        quad.size.height = piece.height;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&quad;
        g_pointerPieces[kept++] = piece; // (what was shown, for the monitor)
        --room;
    }
    g_pointerPieceCount = kept;
}

// Draws a simple shape for each tracked hand into the game's finished picture,
// from the point of view of the eye that picture was drawn for. Also reads the
// controllers' buttons and sticks, since this runs once per frame.
// With both eyes drawn every frame this runs twice per headset frame. The
// controllers are only read, and the buttons only worked through, the first
// time (g_secondOfPair is false); the second time only draws for the other eye.
static bool g_secondOfPair = false;

// Both eyes every frame: the left eye's picture has been taken and the game
// is now drawing the right eye's for the same headset frame.
static bool g_midPair = false;

// Stereo in one game frame (scene.cpp): true while the left eye's picture is
// being taken from the middle of the game's frame, and while the headset
// frame is then moved on to the right eye.
static bool g_stereoMidFrame = false;

// kPairInOneFrame: draw the left and the right picture inside one headset
// frame. It is off: a graphics card that cannot draw two pictures in the time
// of one makes the pace jump between full and half speed, which shows as a
// constant flicker. Instead the eyes take turns (one picture per headset
// frame, a steady pace), and the "both eyes" option (Settings::bothEyes) only
// makes the game's clock stand still from the left eye's picture to the right
// eye's, so that both show the same moment of the game's world.
//
// Both pictures in one frame is a setting to try (pair_frames), because it
// suits the headset software's Spacewarp: with that forced to 45 frames a
// second the headset asks for a frame only every other refresh, which leaves
// time for two pictures, and it makes up the frames in between itself.
static const bool kPairInOneFrame = false;
static XrView g_pairEyes[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; // where both eyes are for this headset frame

bool VrMidPair()
{
    return g_midPair;
}

bool VrCordHeld()
{
    return g_handUse[1] == kHandCord;
}

bool VrBothEyesInStep()
{
    // Either way of showing both eyes one moment needs the game to draw a
    // picture while its clock stands still.
    return GetSettings().bothEyes || GetSettings().pairFrames;
}

void VrToggleBothEyes()
{
    Settings& settings = GetSettings();
    settings.bothEyes = !settings.bothEyes;
    SaveSettings();
    Log("VR: both eyes in step is now %s.", settings.bothEyes ? "ON (the game's clock stands still between the two eyes' pictures)" : "OFF");
}

// The commands of a stand-in run (see g_standIn), from remote.txt in the
// stand-in runtime's run folder (the path is below), one to a line, each with a
// number only ever higher:
//     <number> jump <where to go, as TestJumpStartWords takes it>
//     <number> script <a script command>
//     <number> mark <why>
//     <number> say <words for the log>
//     <number> menu <a menu action's number, as if its button was pressed>
//     <number> page <a menu page's number: the menu opens on it>
//     <number> front <1 or 0: the game's window kept in front, as with a player in the headset>
//     <number> set <a drawing switch for this run, "right_eye_shadows=0", or "beast_away=1">
//     <number> timings <words for the log: the timing lines now and every 10 s for a minute>
//     <number> sample <an engine mark> <eye: 0 any, 1 left, 2 right> <seconds: the sampling profiler>
//     <number> startup (the start-up page, as an ordinary run brings it up)
//     <number> card (the studio's card from the start, as an ordinary run has it)
static void DedicationAgainForTest(); // (below, with the card)
static void UpdateStandInCommands()
{
    if (!g_standIn) return;
    static double nextLook = 0.0;
    static FILETIME written = {};
    static int lastNumber = 0;
    double now = NowMs();
    if (now < nextLook) return;
    nextLook = now + 100.0;

    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    if (length == 0 || length + 40 >= MAX_PATH) return;
    lstrcatW(path, L"BunkerVR\\tools\\fakexr\\run\\remote.txt");
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || CompareFileTime(&data.ftLastWriteTime, &written) == 0) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return;
    written = data.ftLastWriteTime;

    char line[300];
    while (fgets(line, sizeof(line), file))
    {
        int number = 0, used = 0;
        char word[16] = "";
        if (sscanf_s(line, "%d %15s %n", &number, word, (unsigned)sizeof(word), &used) < 2 || number <= lastNumber) continue;
        lastNumber = number;
        char* rest = line + used;
        rest[strcspn(rest, "\r\n")] = '\0';
        if (strcmp(word, "jump") == 0)
        {
            if (TestJumpStartWords(rest) && VrMenuIsOpen()) VrToggleMenu();
        }
        else if (strcmp(word, "script") == 0)
        {
            SendScriptCommand(atoi(rest));
        }
        else if (strcmp(word, "mark") == 0)
        {
            char prefix[128];
            MakeMark(rest, prefix, sizeof(prefix));
        }
        else if (strcmp(word, "say") == 0)
        {
            Log("PRETEND HEADSET: %s", rest);
        }
        else if (strcmp(word, "menu") == 0)
        {
            OnVrMenuAction(atoi(rest)); // (as if its button was pressed)
        }
        else if (strcmp(word, "page") == 0)
        {
            if (!VrMenuIsOpen()) VrToggleMenu();
            VrMenuShowPage(atoi(rest));
        }
        else if (strcmp(word, "set") == 0)
        {
            // For a timed run: one of the drawing's speed switches for this run, not saved
            // ("right_eye_shadows=0"), or the Places page's Beast ("beast_away=1").
            bool done = false;
            if (strncmp(rest, "beast_away=", 11) == 0 && (rest[11] == '0' || rest[11] == '1'))
            {
                VrSetBeastAway(rest[11] == '1');
                done = true;
            }
            else if (strncmp(rest, "move_vignette=", 14) == 0 && rest[14] >= '0' && rest[14] <= '3' && rest[15] == '\0')
            {
                GetSettings().moveVignette = rest[14] - '0'; // (the test runner puts the player's settings file back afterwards)
                done = true;
            }
            else if (strncmp(rest, "hint_size=", 10) == 0 && rest[10] >= '0' && rest[10] <= '3' && rest[11] == '\0')
            {
                GetSettings().hintSize = rest[10] - '0'; // (the same)
                done = true;
            }
            else done = SetDrawingSwitchForRun(rest);
            Log("PRETEND HEADSET: set %s: %s.", rest, done ? "done" : "not a switch that can be set this way");
        }
        else if (strcmp(word, "sample") == 0)
        {
            // The sampling profiler (sampler.cpp): "<mark> <eye: 0 any, 1 left, 2 right> <seconds>".
            char mark[64] = "";
            int eye = 0;
            double seconds = 0.0;
            if (sscanf_s(rest, "%63s %d %lf", mark, (unsigned)sizeof(mark), &eye, &seconds) == 3) SamplerArm(mark, eye, seconds);
            else Log("PRETEND HEADSET: sample needs a mark, an eye and seconds: \"%s\".", rest);
        }
        else if (strcmp(word, "timings") == 0)
        {
            // The timing lines now and every 10 seconds for a minute, as after a mark, without a mark's
            // recording, pictures or test results.
            Log("PRETEND HEADSET: timing lines again: %s", rest);
            FrameTimingsAgain();
        }
        else if (strcmp(word, "front") == 0)
        {
            // Keep the game's window in front as with a player in the headset (UpdateGameFront).
            g_standInFrontTest = atoi(rest) != 0;
            Log("PRETEND HEADSET: the game's window %s kept in front.", g_standInFrontTest ? "is" : "is not");
        }
        else if (strcmp(word, "startup") == 0)
        {
            // The start-up page as an ordinary run brings it up (a stand-in
            // run leaves it out by itself).
            if (!StartupRunning()) StartupOpen(StartupHaveBody() ? kStartupSame : kStartupHow, false);
        }
        else if (strcmp(word, "card") == 0)
        {
            // The studio's card as at an ordinary start, from waiting for the
            // headset to be put on (also left out by itself in a stand-in run).
            DedicationAgainForTest();
        }
    }
    fclose(file);
}

static void CopyMotionPose(const XrPosef& from, MotionPose& to)
{
    to.position[0] = from.position.x;
    to.position[1] = from.position.y;
    to.position[2] = from.position.z;
    to.orientation[0] = from.orientation.x;
    to.orientation[1] = from.orientation.y;
    to.orientation[2] = from.orientation.z;
    to.orientation[3] = from.orientation.w;
}

static bool ReadHandButton(XrAction action, int hand)
{
    if (action == XR_NULL_HANDLE) return false;
    XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = action;
    getInfo.subactionPath = g_handPaths[hand];
    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &getInfo, &state)) && state.isActive &&
           state.currentState == XR_TRUE;
}

// The motion recorder (motion.h): what the headset said this frame, kept for
// the last two minutes. Once per headset frame, after the controllers'
// readings were fetched and the squeezes read.
static void RecordMotionFrame(const XrPosef grip[2], const XrPosef aim[2], unsigned handBits)
{
    MotionFrame frame = {};
    frame.timeMs = NowMs();
    frame.displayTime = (long long)g_frameDisplayTime;
    frame.frameNumber = (unsigned)g_frameNumber;
    CopyMotionPose(g_frameHeadPose, frame.head);
    for (int hand = 0; hand < 2; ++hand)
    {
        CopyMotionPose(grip[hand], frame.grip[hand]);
        CopyMotionPose(aim[hand], frame.aim[hand]);
        frame.gripValue[hand] = g_gripValue[hand];
        frame.triggerValue[hand] = g_triggerValue[hand];
        if (ReadHandButton(g_thumbTouchBound ? g_thumbTouchAction : XR_NULL_HANDLE, hand)) frame.buttons |= (unsigned)kMotionThumbTouch << hand;
        if (ReadHandButton(g_triggerTouchBound ? g_triggerTouchAction : XR_NULL_HANDLE, hand)) frame.buttons |= (unsigned)kMotionFingerTouch << hand;
    }
    if (g_sticksReady)
    {
        ReadStick(g_moveAction, frame.moveStick[0], frame.moveStick[1]);
        ReadStick(g_turnAction, frame.turnStick[0], frame.turnStick[1]);
    }
    frame.straightAhead[0] = g_referencePosition[0];
    frame.straightAhead[1] = g_referencePosition[1];
    frame.straightAhead[2] = g_referencePosition[2];
    frame.straightAhead[3] = g_referenceYaw;
    if (ReadButton(g_gunAction)) frame.buttons |= kMotionButtonA;
    if (ReadButton(g_crouchAction)) frame.buttons |= kMotionButtonB;
    if (ReadButton(g_aimAction2)) frame.buttons |= kMotionButtonX;
    if (ReadButton(g_vrMenuAction)) frame.buttons |= kMotionButtonY;
    if (ReadButton(g_menuAction)) frame.buttons |= kMotionButtonMenu;
    if (ReadButton(g_sprintAction)) frame.buttons |= kMotionButtonLeftStick;
    if (ReadButton(g_inventoryAction)) frame.buttons |= kMotionButtonRightStick;
    frame.flags = (g_framePoseValid ? (unsigned)kMotionHeadKnown : 0u) | handBits * (unsigned)kMotionHandBits;
    if (CameraGunHeld()) frame.flags |= kMotionGunHeld;
    if (CameraGamePaused()) frame.flags |= kMotionGamePaused;
    if (CameraInventoryOpen()) frame.flags |= kMotionInventoryOpen;
    if (CameraTheatre()) frame.flags |= kMotionCutsceneScreen;
    if (VrMenuIsOpen()) frame.flags |= kMotionVrMenuOpen;
    frame.rightItemKind = (short)CameraRightItemKind();
    frame.leftItemKind = (short)CameraLeftItemKind();
    MotionAdd(frame);
}

static void DrawHands(float tanHalfWidth, float tanHalfHeight, int width, int height, bool draw)
{
    if (!g_handsReady) return;

    static XrPosef grip[2], aim[2];
    static bool haveGrip[2] = { false, false }, haveAim[2] = { false, false };
    static float holsterRoom[3] = { 0, 0, 0 };
    static bool haveHolster = false;
    static bool menuPointer = false;

    if (!g_secondOfPair)
    {
    // Fetch the latest controller readings.
    XrActiveActionSet activeSet = { g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    if (XR_FAILED(xrSyncActions(g_session, &syncInfo)))
    {
        SetInteractHeld(false); // never leave a button or key stuck down
        ReleaseMoveKeys();
        g_aimValid[0] = g_aimValid[1] = false;
        return;
    }

    // Where is each hand?
    static bool handSeen[2] = { false, false };
    unsigned motionHandBits = 0;
    for (int hand = 0; hand < 2; ++hand)
    {
        // (Each part's state too, for a controller that loses tracking: see
        // LostHandPoses.)
        bool gripPosition = false, gripTurn = false, gripTracked = false;
        bool aimPosition = false, aimTurn = false, aimTracked = false;
        haveGrip[hand] = LocateHandParts(g_gripSpaces[hand], g_frameDisplayTime, grip[hand], gripPosition, gripTurn, gripTracked);
        g_gripTrackedNow[hand] = haveGrip[hand] && gripTracked; // (for the Body page's log)
        haveAim[hand] = LocateHandParts(g_aimSpaces[hand], g_frameDisplayTime, aim[hand], aimPosition, aimTurn, aimTracked);
        // (For the recorder, below: the four bits of this hand.)
        motionHandBits |= (unsigned)((haveGrip[hand] ? 1 : 0) | (gripTracked ? 2 : 0) | (haveAim[hand] ? 4 : 0) | (aimTracked ? 8 : 0)) << (hand * 4);
        g_aimValid[hand] = haveAim[hand];
        if (haveAim[hand]) g_aimRoom[hand] = aim[hand];
        if (haveGrip[hand] && gripTracked) MeasureNoteHand(grip[hand].position.y); // (a floor touched while measuring)

        // For the log: when the headset gives a controller's
        // position without tracking it (its own guess), and when it tracks it
        // again: how a lost controller is reported.
        {
            static bool guessed[2] = { false, false };
            static int logsLeft = 20;
            bool guessNow = aimPosition && !aimTracked;
            if (guessNow != guessed[hand] && logsLeft > 0)
            {
                --logsLeft;
                Log("VR: the %s controller's position is %s.", hand == 0 ? "left" : "right",
                    guessNow ? "given but not tracked (the headset's own guess)" : "tracked again");
            }
            guessed[hand] = guessNow;
        }

        if (haveGrip[hand] && !handSeen[hand])
        {
            handSeen[hand] = true;
            Log("VR: %s hand controller is being tracked (at %.2f %.2f %.2f).", hand == 0 ? "left" : "right",
                grip[hand].position.x, grip[hand].position.y, grip[hand].position.z);
        }

        // Share both poses with the camera hook, which passes them on to the
        // game's scripts (pointing and holding use "aim", the arms use "grip").
        if (g_haveReference)
        {
            // For the game's arms: the palm's position, with the pointing
            // pose's rotation. The pointing pose has a clear meaning (it
            // looks the way the controller points, with "up" towards the
            // thumb), while headsets differ in how the palm pose is turned.
            XrPosef shareAim = aim[hand];
            XrPosef shareGrip = grip[hand];
            if (haveAim[hand]) shareGrip.orientation = aim[hand].orientation;
            // A controller that has lost tracking: its hand held where it was
            // (see LostHandPoses). Not while the poses are frozen anyway
            // (g_handsFrozen).
            // (A guessed position counts as lost too, once the headset has
            // said this controller is tracked: see LostHand.)
            bool frozenNow = g_handsFrozen;
            bool share = false;
            bool givenNow = haveGrip[hand] && haveAim[hand];
            if (givenNow && gripTracked && aimTracked) g_trackBitSeen[hand] = true;
            bool trackedNow = givenNow && ((gripTracked && aimTracked) || !g_trackBitSeen[hand]);
            if (!frozenNow)
                share = LostHandPoses(hand, trackedNow, aimTurn ? &aim[hand].orientation : nullptr, shareAim, shareGrip);
            HeadPose relativeAim = {}, relativeGrip = {};
            if (share)
            {
                ToRelativePose(shareAim, relativeAim);
                ToRelativePose(shareGrip, relativeGrip);
            }

            // On request (F9), note how the palm pose is turned compared with
            // the pointing pose: the palm pose's three axes as the pointing pose sees them.
            if (g_handAxesLogRequested && haveAim[hand] && haveGrip[hand])
            {
                HeadPose a = {}, g = {};
                ToRelativePose(aim[hand], a);
                ToRelativePose(grip[hand], g);
                float m[9];
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        m[row * 3 + column] = a.rotation[0 + row] * g.rotation[0 + column] +
                                              a.rotation[3 + row] * g.rotation[3 + column] +
                                              a.rotation[6 + row] * g.rotation[6 + column];
                Log("VR: %s palm pose, seen from its pointing pose: x axis (%.2f %.2f %.2f), y axis (%.2f %.2f %.2f), "
                    "z axis (%.2f %.2f %.2f); palm is %.3f %.3f %.3f m from the pointing pose's origin (room axes).",
                    hand == 0 ? "left" : "right", m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8],
                    grip[hand].position.x - aim[hand].position.x, grip[hand].position.y - aim[hand].position.y,
                    grip[hand].position.z - aim[hand].position.z);
                if (hand == 1) g_handAxesLogRequested = false;
            }

            unsigned long long now = GetTickCount64();
            AcquireSRWLockExclusive(&g_poseLock);
            // (frozen: the poses stay as they are; the hand's pose and its
            // palm's, together)
            if (share && !frozenNow)
            {
                g_sharedHandPose[hand] = relativeAim;
                g_sharedHandRoom[hand] = shareAim;
                g_sharedHandTime[hand] = now;
                g_sharedGripPose[hand] = relativeGrip;
                g_sharedGripRoom[hand] = shareGrip;
                g_sharedGripTime[hand] = now;
            }
            ReleaseSRWLockExclusive(&g_poseLock);
        }
    }

    // The holster: a spot at the right hip, measured from the head in real
    // meters along the head's own sideways and forwards directions (kept
    // level, so looking down at it does not move it). Measured from the neck,
    // as the flashlight's place is: the eyes are 8 cm in front of the neck
    // and swing another 10 cm or so forwards and down as the head nods to
    // look at the hip, and a spot measured from them would go with them. It is
    // holsterDown below the eyes while looking ahead (the neck is 7.5 cm below
    // the eyes), and it does not drop as the head nods.
    haveHolster = false;
    {
        const Settings& settings = GetSettings();
        const XrQuaternionf& q = g_frameHeadPose.orientation;
        float rx = 1 - 2 * (q.y * q.y + q.z * q.z), rz = 2 * (q.x * q.z - q.w * q.y); // the head's "right"
        float length = sqrtf(rx * rx + rz * rz);
        if (g_framePoseValid && length > 0.25f)
        {
            rx /= length;
            rz /= length;
            float fx = rz, fz = -rx; // level "forwards"
            XrVector3f neck = NeckPoint(g_frameHeadPose);
            holsterRoom[0] = neck.x + rx * settings.holsterRight + fx * settings.holsterForward;
            holsterRoom[1] = neck.y + 0.075f - settings.holsterDown;
            holsterRoom[2] = neck.z + rz * settings.holsterRight + fz * settings.holsterForward;
            haveHolster = true;
        }
        g_holsterRoomKnown = haveHolster;
        for (int i = 0; i < 3; ++i) g_holsterRoomNow[i] = holsterRoom[i];
        // The hand is "at the holster" when it is within the holster's radius
        // of that spot seen from above, and at about its height: from 15 cm
        // above it to 30 cm below. (Not anywhere below it, down to the floor:
        // reaching down to a table on your right would take the squeeze for
        // the gun instead of grabbing.)
        g_rightInHolster = false;
        if (haveHolster && haveGrip[1])
        {
            float dx = grip[1].position.x - holsterRoom[0], dz = grip[1].position.z - holsterRoom[2];
            g_rightInHolster = sqrtf(dx * dx + dz * dz) < settings.holsterRadius &&
                               grip[1].position.y < holsterRoom[1] + 0.15f && grip[1].position.y > holsterRoom[1] - 0.30f;
        }

        // The flashlight's place: the same spot mirrored to the left hip.
        // (Also the ammo pouch; the bullet counter can be shown there.)
        g_leftAtLightHolster = false;
        g_pouchValid = false;
        g_leftNearPouch = false;
        if (haveHolster)
        {
            g_pouchRoom[0] = holsterRoom[0] - 2.0f * rx * settings.holsterRight;
            g_pouchRoom[1] = holsterRoom[1];
            g_pouchRoom[2] = holsterRoom[2] - 2.0f * rz * settings.holsterRight;
            if (settings.pouchSet) // (put there on the Body page)
            {
                float fx = rz, fz = -rx; // level "forwards"
                XrVector3f neck = NeckPoint(g_frameHeadPose);
                g_pouchRoom[0] = neck.x + rx * settings.pouchRight + fx * settings.pouchForward;
                g_pouchRoom[1] = neck.y + 0.075f - settings.pouchDown;
                g_pouchRoom[2] = neck.z + rz * settings.pouchRight + fz * settings.pouchForward;
            }
            g_pouchValid = true;
        }
        if (haveHolster && haveGrip[0])
        {
            float lightX = g_pouchRoom[0];
            float lightZ = g_pouchRoom[2];
            float dx = grip[0].position.x - lightX, dz = grip[0].position.z - lightZ;
            float dy = grip[0].position.y - g_pouchRoom[1];
            // (From 15 cm above it to 30 cm below, as the holster: the pouch
            // can be in front of the waist, and the shotgun's loading port,
            // held low, would come within its reach down to the floor.)
            g_leftAtLightHolster = sqrtf(dx * dx + dz * dz) < settings.holsterRadius &&
                                   dy < 0.15f && dy > -0.30f;
            g_leftToPouch = sqrtf(dx * dx + dy * dy + dz * dz); // (for LeftTakesThingNotBullet)
            g_leftNearPouch = g_leftAtLightHolster || sqrtf(dx * dx + dy * dy + dz * dz) < 0.35f;
        }

        // The flashlight's place: the left side of the chest. A ball of 19 cm
        // around a point 22.5 cm below the neck, 14 cm to the left and 10 cm
        // in front of it. Squeezing the left grip there takes it out or puts
        // it away. (Measured from the neck, not the eyes: the eyes are 8 cm in
        // front of the neck and go further forwards as the head looks down at
        // it, which would carry the spot further off.)
        g_leftAtChestLight = false;
        if (haveHolster && haveGrip[0])
        {
            float fx = rz, fz = -rx; // level "forwards"
            XrVector3f neck = NeckPoint(g_frameHeadPose);
            float cx = neck.x - rx * settings.lightLeft + fx * settings.lightForward; // (14, 22.5 and 10 cm, unless put elsewhere on the Body page)
            float cy = neck.y - settings.lightDown;
            float cz = neck.z - rz * settings.lightLeft + fz * settings.lightForward;
            float dx = grip[0].position.x - cx, dy = grip[0].position.y - cy, dz = grip[0].position.z - cz;
            // (Bigger while the flashlight is in the hand, so putting it back
            // is easy: the hand holding it does not come as near.)
            float reach = CameraLeftItemKind() == kItemFlashlight ? 0.27f : 0.19f;
            g_leftAtChestLight = sqrtf(dx * dx + dy * dy + dz * dz) < reach;
            const float light[3] = { cx, cy, cz };
            OutOfTheChest(light, neck, fx, fz, g_chestLightRoom);
        }

        // The shotgun's place: behind the right shoulder, where it is put away
        // and taken out. A ball of 25 cm around a point 3 cm above the neck,
        // 17 cm to its right and 6 cm behind it, the hand no more than 4 cm in
        // front of the neck: not in front of the shoulder, where the gun is
        // aimed (the trigger hand some 20 cm or more in front); and no lower
        // than 12 cm below the neck (not at the chest, where the right hand may
        // rest with the gun out).
        g_rightAtShoulder = false;
        if (haveHolster && haveGrip[1])
        {
            float fx = rz, fz = -rx; // level "forwards"
            XrVector3f neck = NeckPoint(g_frameHeadPose);
            float cx = neck.x + rx * 0.17f - fx * 0.06f;
            float cy = neck.y + 0.03f;
            float cz = neck.z + rz * 0.17f - fz * 0.06f;
            float dx = grip[1].position.x - cx, dy = grip[1].position.y - cy, dz = grip[1].position.z - cz;
            float ahead = (grip[1].position.x - neck.x) * fx + (grip[1].position.z - neck.z) * fz;
            g_rightAtShoulder = sqrtf(dx * dx + dy * dy + dz * dz) < 0.25f && ahead < 0.04f && grip[1].position.y > neck.y - 0.12f;
        }

        // The right chest's item slot (the flat game's hotkey 1): a ball of 15 cm round a point
        // 20 cm below the neck, 13 cm to its right and 9 cm in front of it (unless put elsewhere
        // on the Holsters page), 19 cm with an item in the right hand (as the flashlight's place
        // grows with the flashlight in the hand: the hand holding a thing does not come as
        // near). Never where the holster or the shotgun's place
        // behind the shoulder has the hand: their squeezes come first. (Not the flashlight's
        // place mirrored: a holster can reach up to 27.5 cm below the neck and in to 7 cm right
        // of it, and a flashlight set 32 cm below the neck would mirror to inside that reach.)
        g_rightAtChestSlot = false;
        if (haveHolster && haveGrip[1])
        {
            float fx = rz, fz = -rx; // level "forwards"
            XrVector3f neck = NeckPoint(g_frameHeadPose);
            float cx = neck.x + rx * settings.slotRight + fx * settings.slotForward;
            float cy = neck.y - settings.slotDown;
            float cz = neck.z + rz * settings.slotRight + fz * settings.slotForward;
            float dx = grip[1].position.x - cx, dy = grip[1].position.y - cy, dz = grip[1].position.z - cz;
            float reach = CameraRightItemKind() != kItemNone ? 0.19f : 0.15f;
            g_rightAtChestSlot = sqrtf(dx * dx + dy * dy + dz * dz) < reach && !g_rightInHolster && !g_rightAtShoulder;
            const float slot[3] = { cx, cy, cz };
            OutOfTheChest(slot, neck, fx, fz, g_chestSlotRoom);
        }
    }
    // The palms in the room, for the hints' labels at the hands.
    for (int hand = 0; hand < 2; ++hand)
    {
        g_hintGripKnown[hand] = haveGrip[hand];
        if (haveGrip[hand]) g_hintGrip[hand] = grip[hand];
    }

    // How fast the right hand is moving, for throwing a grenade: the fastest
    // of the last dozen frames, because people let go a little late, when the
    // hand is already slowing.
    {
        static XrVector3f was = {};
        static bool haveWas = false;
        static double wasMs = 0;
        static float recent[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        static int next = 0;
        double nowMs = NowMs();
        float speed = 0.0f;
        if (haveGrip[1])
        {
            const XrVector3f& p = grip[1].position;
            if (haveWas && nowMs - wasMs > 1.0 && nowMs - wasMs < 100.0)
            {
                float dx = p.x - was.x, dy = p.y - was.y, dz = p.z - was.z;
                speed = sqrtf(dx * dx + dy * dy + dz * dz) / (float)((nowMs - wasMs) / 1000.0);
            }
            was = p;
            wasMs = nowMs;
            haveWas = true;
        }
        else
        {
            haveWas = false;
        }
        g_rightHandSpeedNow = speed;
        recent[next] = speed;
        next = (next + 1) % 12;
        g_rightHandSpeed = 0.0f;
        for (int i = 0; i < 12; ++i)
            if (recent[i] > g_rightHandSpeed) g_rightHandSpeed = recent[i];
    }

    // How fast the right wrist is turning, for the "swing the revolver open" move.
    {
        static XrQuaternionf was = {};
        static bool haveWas = false;
        static double wasMs = 0;
        static int fastFrames = 0;
        double nowMs = NowMs();
        if (haveAim[1])
        {
            const XrQuaternionf& q = aim[1].orientation;
            if (haveWas && nowMs - wasMs > 1.0 && nowMs - wasMs < 100.0)
            {
                float dot = fabsf(q.x * was.x + q.y * was.y + q.z * was.z + q.w * was.w);
                if (dot > 1.0f) dot = 1.0f;
                g_rightSwingSpeed = 2.0f * acosf(dot) / (float)((nowMs - wasMs) / 1000.0);

                // Which way it is turning, as the controller itself feels it
                // (x = about its "right", y = about its "up", z = about its
                // "back"): the turn from last frame to this one, taken apart.
                float w = was.w * q.w + was.x * q.x + was.y * q.y + was.z * q.z;
                float ax = was.w * q.x - q.w * was.x - (was.y * q.z - was.z * q.y);
                float ay = was.w * q.y - q.w * was.y - (was.z * q.x - was.x * q.z);
                float az = was.w * q.z - q.w * was.z - (was.x * q.y - was.y * q.x);
                if (w < 0.0f) { ax = -ax; ay = -ay; az = -az; }
                float length = sqrtf(ax * ax + ay * ay + az * az);
                if (length > 0.00001f)
                {
                    g_rightSwingAxis[0] = ax / length;
                    g_rightSwingAxis[1] = ay / length;
                    g_rightSwingAxis[2] = az / length;
                }
            }
            else
            {
                g_rightSwingSpeed = 0.0f;
            }
            was = q;
            wasMs = nowMs;
            haveWas = true;
        }
        else
        {
            haveWas = false;
            g_rightSwingSpeed = 0.0f;
        }
        // Two fast frames in a row, so one jumpy reading cannot count as a swing.
        fastFrames = g_rightSwingSpeed > 5.5f ? fastFrames + 1 : 0;
        g_rightSwing = fastFrames >= 2;
        // Opening the revolver is also guarded by the A button, so there the
        // very first fast frame counts and nothing is waited for.
        g_rightSwingAtOnce = fastFrames >= 1;

        // How far the gun's barrel points upwards: 1 = straight up, 0 = level.
        // (The barrel lies along the controller's pointing direction.)
        if (haveAim[1])
        {
            const XrQuaternionf& q = aim[1].orientation;
            g_gunTipUp = 2.0f * (q.w * q.x - q.y * q.z);
        }
        else
        {
            g_gunTipUp = 0.0f;
        }
    }

    UpdateGrabInput(); // (GripsTick is on the game's own frame, in frame.cpp: also while the headset is off)
    bool vrMenuPointer = UpdateVrMenu();
    if (VrMenuIsOpen())
    {
        // The mod's own menu is up: the game's menus are not pointed at.
        g_pointerActive = false;
        g_pointerHit[0] = false;
        g_pointerHit[1] = false;
    }
    bool gameMenuPointer = !VrMenuIsOpen() && UpdateMenuPointer();
    menuPointer = vrMenuPointer || gameMenuPointer;
    UpdateMoveInput(gameMenuPointer);
    UpdateLoadingContinue(VrMenuIsOpen());
    RecordMotionFrame(grip, aim, motionHandBits);
    UpdateStandInCommands();
    UpdateTakePulls(vrMenuPointer && VrMenuPointerOnButton()); // (the measurings below are taken with a trigger)
    UpdateBodyPage(g_frameHeadPose, grip, haveGrip);
    UpdateStartup(g_frameHeadPose);
    UpdateHeightWatch(g_frameHeadPose);
    } // end of "the first time this headset frame"

    static const int kMaxBoxes = 16;
    float boxes[kMaxBoxes * 16];
    float colours[kMaxBoxes * 3];
    int boxCount = 0;
    float scale = GetSettings().worldScale;

    // While the game is being played, nothing extra is drawn on the hands: the
    // game's own arms are there. (If those are switched off with F4, a plain
    // block stands in for each fist.)
    // In a menu the game's arms do not move: each hand has a glowing orb, and
    // the hand that points a beam to the panel and a cursor where it lands,
    // each a layer of its own in front of the panels (AddPointerLayers): drawn
    // here, they would be behind the panels.
    bool gameArmsMoving = GetSettings().gameHands && CameraHandScriptBusy(300);
    bool inMenu = !CameraScriptsBusy(300) || (GetSettings().gameHands && !gameArmsMoving);
    g_handsInMenuNow = inMenu;
    g_menuPointerNow = menuPointer;

    for (int hand = 0; hand < 2; ++hand)
    {
        if (!inMenu && haveGrip[hand] && !GetSettings().gameHands)
        {
            const float offset[3] = { 0.0f, 0.0f, 0.0f };
            const float size[3] = { 0.055f, 0.085f, 0.10f };
            BoxSeenFromEye(g_frameEyePose, grip[hand], offset, size, scale, boxes + boxCount * 16);
            colours[boxCount * 3 + 0] = 0.80f;
            colours[boxCount * 3 + 1] = 0.80f;
            colours[boxCount * 3 + 2] = 0.80f;
            ++boxCount;
        }
    }
    // The flashlight's cord, while the right hand has hold of it: a thin line
    // from the flashlight (in the left hand) to the right hand.
    // (Off: the game's scripts move the flashlight's own string to the hand,
    // which looks solid and is hidden behind things.)
    static const bool kDrawCordLine = false;
    if (kDrawCordLine && g_handUse[1] == kHandCord && haveGrip[0] && haveGrip[1] && boxCount < kMaxBoxes)
    {
        const XrVector3f& from = grip[0].position;
        const XrVector3f& to = grip[1].position;
        float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        float length = sqrtf(dx * dx + dy * dy + dz * dz);
        if (length > 0.03f)
        {
            dx /= length; dy /= length; dz /= length;

            // A turn that takes "forwards" (0, 0, -1) onto the line's direction.
            XrPosef line = {};
            line.position = from;
            XrQuaternionf q = { dy, -dx, 0.0f, 1.0f - dz };
            float size2 = q.x * q.x + q.y * q.y + q.w * q.w;
            if (size2 < 0.000001f)
            {
                q.x = 0.0f; q.y = 1.0f; q.z = 0.0f; q.w = 0.0f; // straight backwards: half a turn
            }
            else
            {
                float inverse = 1.0f / sqrtf(size2);
                q.x *= inverse; q.y *= inverse; q.w *= inverse;
            }
            line.orientation = q;

            const float offset[3] = { 0.0f, 0.0f, -length * 0.5f };
            const float size[3] = { 0.003f, 0.003f, length };
            BoxSeenFromEye(g_frameEyePose, line, offset, size, scale, boxes + boxCount * 16);
            colours[boxCount * 3 + 0] = 0.78f;
            colours[boxCount * 3 + 1] = 0.72f;
            colours[boxCount * 3 + 2] = 0.60f;
            ++boxCount;
        }
    }

    // A bullet between the left hand's fingers while it carries one to the gun.
    // It sits in the curled fingers: at the palm, a little forwards and up,
    // turned the way the hand points.
    if (g_handUse[0] == kHandBullet && haveAim[0] && haveGrip[0] && !CameraBulletModelShown())
    {
        XrPosef inHand = grip[0];
        inHand.orientation = aim[0].orientation;
        const float offset[3] = { 0.0f, 0.03f, -0.04f };
        const float size[3] = { 0.011f, 0.011f, 0.034f };
        BoxSeenFromEye(g_frameEyePose, inHand, offset, size, scale, boxes + boxCount * 16);
        colours[boxCount * 3 + 0] = 0.80f;
        colours[boxCount * 3 + 1] = 0.60f;
        colours[boxCount * 3 + 2] = 0.22f;
        ++boxCount;
    }

    // A marker where the holster is (green while the right hand is in it): a
    // developer aid, and while the Body page is up, with one at the pouch
    // (green while the left hand is at it), to be moved with its numbers.
    bool markSpots = VrMenuShowHolster() || VrMenuOnBodyPage();
    if (markSpots && haveHolster)
    {
        XrPosef spot = {};
        spot.orientation.w = 1.0f;
        spot.position.x = holsterRoom[0];
        spot.position.y = holsterRoom[1];
        spot.position.z = holsterRoom[2];
        const float offset[3] = { 0.0f, 0.0f, 0.0f };
        const float size[3] = { 0.06f, 0.06f, 0.06f };
        BoxSeenFromEye(g_frameEyePose, spot, offset, size, scale, boxes + boxCount * 16);
        colours[boxCount * 3 + 0] = g_rightInHolster ? 0.25f : 0.85f;
        colours[boxCount * 3 + 1] = g_rightInHolster ? 0.85f : 0.45f;
        colours[boxCount * 3 + 2] = 0.25f;
        ++boxCount;
    }
    if (markSpots && g_pouchValid && boxCount < kMaxBoxes)
    {
        XrPosef spot = {};
        spot.orientation.w = 1.0f;
        spot.position.x = g_pouchRoom[0];
        spot.position.y = g_pouchRoom[1];
        spot.position.z = g_pouchRoom[2];
        const float offset[3] = { 0.0f, 0.0f, 0.0f };
        const float size[3] = { 0.05f, 0.05f, 0.05f };
        BoxSeenFromEye(g_frameEyePose, spot, offset, size, scale, boxes + boxCount * 16);
        colours[boxCount * 3 + 0] = g_leftAtLightHolster ? 0.25f : 0.80f;
        colours[boxCount * 3 + 1] = g_leftAtLightHolster ? 0.85f : 0.78f;
        colours[boxCount * 3 + 2] = g_leftAtLightHolster ? 0.25f : 0.70f;
        ++boxCount;
    }

    if (boxCount == 0 || !draw) return;

    // Furthest first, so that a nearer box is drawn over a further one: they are
    // drawn with no depth of their own, so in a fixed order one hand's box would
    // always cover the other's.
    int order[kMaxBoxes], orderCount = 0;
    for (int i = 0; i < boxCount; ++i)
    {
        const float* b = boxes + i * 16;
        float d = b[3] * b[3] + b[7] * b[7] + b[11] * b[11];
        int at = orderCount++;
        while (at > 0)
        {
            const float* a = boxes + order[at - 1] * 16;
            if (a[3] * a[3] + a[7] * a[7] + a[11] * a[11] >= d) break;
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
    }
    float sortedBoxes[kMaxBoxes * 16], sortedColours[kMaxBoxes * 3];
    for (int i = 0; i < boxCount; ++i)
    {
        memcpy(sortedBoxes + i * 16, boxes + order[i] * 16, 16 * sizeof(float));
        memcpy(sortedColours + i * 3, colours + order[i] * 3, 3 * sizeof(float));
    }

    // Flatten 3D the same way the game did for this picture: the same angles
    // across and up/down. Near and far limits are in game meters.
    const float nearLimit = 0.02f, farLimit = 100.0f;
    float projection[16] = {
        1.0f / tanHalfWidth, 0, 0, 0,
        0, 1.0f / tanHalfHeight, 0, 0,
        0, 0, -(farLimit + nearLimit) / (farLimit - nearLimit), -2.0f * farLimit * nearLimit / (farLimit - nearLimit),
        0, 0, -1, 0,
    };

    GlDrawBoxes(g_gamePicture, width, height, projection, sortedBoxes, sortedColours, boxCount);
}

// ---------------------------------------------------------------------------
// Blood, dark edges and the gas mask across the eyes
// ---------------------------------------------------------------------------
// On a monitor the game lays flat pictures over the screen when the player is
// hurt: blood rising from the bottom edge, red at the edges, dark edges. Drawn
// the game's way in a headset they sit at a different spot for each eye, which
// reads as a board hanging in the air and as double vision. So the game is
// told not to draw them (Effect_Screen.hps), reports how strong each should be
// (CameraGetScreenEffects), and they are drawn here instead, with the game's
// own picture files, at the same place for both eyes: stuck to the face.
//
// The game places each picture on an imagined screen 1 unit high and 1.78
// wide. Here that screen is wrapped across most of what the headset shows; the
// edge of each picture carries on to the edge of the view.
static void DrawScreenEffects(float tanHalfWidth, float tanHalfHeight, int width, int height)
{
    if (!GetSettings().faceEffects) return;

    float strengths[kScreenEffectCount];
    if (!CameraGetScreenEffects(strengths)) return;

    struct Effect
    {
        const char* file;             // in the game's folder
        float centreX, centreY;       // on the imagined screen: 0..1 across, 0..1 from the bottom
        float sizeX, sizeY;           // in screen heights
        float gain;                   // some pictures are very faint by themselves
        bool multiply;                // darkens instead of being laid over
        bool glass;                   // darkens as the game's shader does it (GlDrawScreenGlass)
        float tint[3];                // the picture's color is multiplied by this
        GLuint texture;
        bool tried;
    };
    // The last two are not from the game's screen-effect script: the game
    // draws them as part of its interface, so left to the game they would show
    // on the flat interface panel. They are a white picture that is clear in
    // the middle, tinted red when hurt and pale green when healed.
    static Effect effects[kScreenEffectCount] = {
        { "textures\\screeneffects\\player_damage_screen_major.dds", 0.5f, 0.45f, 1.75f, 0.875f, 1.0f, false, false, { 1, 1, 1 }, 0, false },
        { "textures\\screeneffects\\player_damage_screen_minor.dds", 0.5f, 0.32f, 1.75f, 0.5f, 1.0f, false, false, { 1, 1, 1 }, 0, false },
        { "textures\\screeneffects\\player_damage_screen_diffuse.dds", 0.5f, 0.5f, 1.8f, 1.8f, 2.0f, false, false, { 1, 1, 1 }, 0, false },
        { "textures\\screeneffects\\red_overlay.dds", 0.5f, 0.5f, 1.25f, 1.0f, 2.0f, false, false, { 1, 1, 1 }, 0, false },
        { "textures\\screeneffects\\vignette_screen_diffuse.dds", 0.5f, 0.5f, 1.78f, 1.0f, 1.0f, true, false, { 1, 1, 1 }, 0, false },
        // The gas mask. On a monitor the game stretches this picture to the
        // whole width and 1.42 times the height of the screen, which leaves
        // only the middle half of the height clear. Here it is taller than that,
        // so the rims show without hiding the player's hands: clear to about
        // 40 degrees either side and 33 up and down.
        { "textures\\screeneffects\\gasmask_screen.dds", 0.5f, 0.54f, 1.35f, 2.25f, 1.0f, false, true, { 1, 1, 1 }, 0, false },
        { "textures\\screeneffects\\vignette_white_alpha.dds", 0.5f, 0.5f, 1.78f, 1.0f, 1.0f, false, false, { 0.5f, 0.15f, 0.15f }, 0, false },
        { "textures\\screeneffects\\vignette_white_alpha.dds", 0.5f, 0.5f, 1.78f, 1.0f, 1.0f, false, false, { 0.785f, 1.0f, 0.831f }, 0, false },
        { nullptr, 0, 0, 1, 1, 1, false, false, { 1, 1, 1 }, 0, false },
    };

    // The imagined screen, as tangents of the angles from straight ahead: its
    // half width and half height, and how far its middle sits below center.
    const float kHalfWidth = 1.42f, kHalfHeight = 0.80f, kCentreY = -0.12f;
    const float kScreenWide = 1.78f;

    // The pictures are placed about 0.8 m in front of the face: each eye sees
    // them shifted a little towards the nose, as it would a real thing there.
    float shift = (g_frameEye == 0 ? 1.0f : -1.0f) * 0.032f / 0.8f;

    for (int i = 0; i < kScreenEffectCount; ++i)
    {
        Effect& e = effects[i];
        if (e.file == nullptr || strengths[i] <= 0.003f) continue;

        if (!e.tried)
        {
            e.tried = true;

            // "<folder of the exe>\<file>"
            char path[MAX_PATH * 2];
            DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            while (length > 0 && path[length - 1] != '\\') --length;
            path[length] = '\0';
            lstrcatA(path, e.file);

            e.texture = GlLoadDdsTexture(path);
            Log("VR: screen effect picture %s %s.", e.file, e.texture != 0 ? "loaded" : "could NOT be loaded");
        }
        if (e.texture == 0) continue;

        // From a point of the eye's picture (x, y from -1 to +1) to a point of
        // the effect's picture (0, 0 = its top-left corner):
        //   on the screen:  across = (x * tanHalfWidth - shift) / kHalfWidth * 0.89 + 0.89   (in heights)
        //                   up     = (y * tanHalfHeight - kCentreY) / (2 * kHalfHeight) + 0.5
        //   in the picture: u = (across - (centreX * 1.78 - sizeX / 2)) / sizeX
        //                   v = 1 - (up - (centreY - sizeY / 2)) / sizeY
        float half = kScreenWide * 0.5f;
        float scale[2], offset[2];
        scale[0] = tanHalfWidth * half / kHalfWidth / e.sizeX;
        offset[0] = (-shift * half / kHalfWidth + half - e.centreX * kScreenWide + e.sizeX * 0.5f) / e.sizeX;
        scale[1] = -tanHalfHeight / (2.0f * kHalfHeight) / e.sizeY;
        offset[1] = 1.0f - (-kCentreY / (2.0f * kHalfHeight) + 0.5f - e.centreY + e.sizeY * 0.5f) / e.sizeY;

        float strength = strengths[i] * e.gain;
        if (strength > 1.0f) strength = 1.0f;
        if (e.glass) GlDrawScreenGlass(e.texture, g_gamePicture, width, height, scale, offset, strength);
        else GlDrawScreenEffect(e.texture, g_gamePicture, width, height, scale, offset, strength, e.multiply, e.tint);
    }
}

// The comfort ring: while a knockdown, a kill or a death carries the view with
// the knockdown view set to the vignette, the body script (VRVignetteView, also
// VRGentleView) closes a dark ring in from the edges of the view as the view
// moves faster, through cVR_ComfortRing (scriptfuncs.cpp) every update.
// How far it is closed: 0 none (clear to beyond the edges), 1 clear only
// within about 24 degrees of straight ahead, black from about 39 degrees out.
// When no value arrives for 0.25 s (the game paused, the move over, the
// scripts stopped), it is not drawn: it never stays on by itself.
static float g_comfortRing = 0.0f;
static double g_comfortRingMs = -100000.0;

void VrSetComfortRing(float closed)
{
    if (!(closed >= 0.0f)) closed = 0.0f; // (also catches NaN)
    if (closed > 1.0f) closed = 1.0f;
    g_comfortRing = closed;
    g_comfortRingMs = NowMs();
}

// How far the ring may close at each of the menu's vignette strengths (Settings::vignetteStrength,
// Settings::moveVignette): none, weak (to about 49 degrees off the middle of the view), medium (38) and strong (24).
static const float kVignetteStrengths[4] = { 0.0f, 0.4f, 0.7f, 1.0f };

static void DrawComfortRing(float tanHalfWidth, float tanHalfHeight, int width, int height)
{
    // The knockdown's ring (with its own strength) and the movement vignette's (Settings::moveVignette,
    // UpdateMoveInput): the more closed of the two is drawn.
    double now = NowMs();
    float knockdown = 0.0f, moving = 0.0f;
    if (now - g_comfortRingMs <= 250.0)
    {
        int strength = GetSettings().vignetteStrength;
        knockdown = g_comfortRing * kVignetteStrengths[strength < 0 ? 0 : (strength > 3 ? 3 : strength)];
    }
    if (now - g_moveRingMs <= 250.0)
    {
        int strength = GetSettings().moveVignette;
        moving = g_moveRing * kVignetteStrengths[strength < 0 ? 0 : (strength > 3 ? 3 : strength)];
    }
    float closed = knockdown > moving ? knockdown : moving;
    if (closed < 0.004f) return;

    static bool logged = false, loggedMoving = false;
    if (!logged && knockdown >= moving)
    {
        logged = true;
        Log("VR: the comfort ring is drawn (closed %.2f).", closed);
    }
    if (!loggedMoving && moving > knockdown)
    {
        loggedMoving = true;
        Log("VR: the movement vignette is drawn (closed %.2f; strength %d of 3).", closed, GetSettings().moveVignette);
    }
    // Clear within: from tan 1.6 (58 degrees, beyond the edges) to tan 0.45
    // (24 degrees); black from 0.35 further out.
    float clearWithin = 1.6f + (0.45f - 1.6f) * closed;
    GlDrawRing(g_gamePicture, width, height, tanHalfWidth, tanHalfHeight, clearWithin, clearWithin + 0.35f, 1.0f);
}

// ---------------------------------------------------------------------------
// The hit indicator. VR games show a hit through the view's edges and the hands, never by moving the view (Meta's
// comfort guidance): a red edge strongest on the side the hit came from, bigger and longer for a harder hit (like an
// FPS damage indicator's direction and size); an impact buzz in the controllers, harder in the hand on the hit's side,
// then for a hard hit a fading heartbeat. Reported by the game's OnDamageTaken (Player_Custom.hps, cVR_Hurt): how hard,
// and where from, as a direction from the camera at that moment (x right, y up, z ahead), or all round, or from below
// for a fall. The edge keeps pointing where the hit came from as the head turns (about the vertical axis). It is drawn
// into each eye's picture, so it shows on the monitor too.
// ---------------------------------------------------------------------------
static double g_hurtMs = -1.0e9;    // when the last hit came
static float g_hurtStrength = 0.0f; // its edge's strength at its peak (0 to 1)...
static float g_hurtLastsMs = 0.0f;  // ...and how long it takes to fade
static int g_hurtKind = 0;          // 0 all round, 1 from a direction, 2 from below (a fall)
static float g_hurtFrom[3] = {};    // the direction it came from, from the camera as it came (x right, y up, z ahead)
static float g_hurtHeadYaw = 0.0f;  // the head's turn about the upright as it came (radians, right-handed)
static int g_hurtBeats = 0, g_hurtBeatsDone = 0; // the heartbeat after it: how many beats, how many felt so far...
static bool g_hurtSecondDue = false; // ...and the second thump of the beat under way
static const double kHurtPeakMs = 120.0, kHurtBeatFirstMs = 450.0, kHurtBeatGapMs = 800.0, kHurtBeatSecondMs = 180.0;
// How long the impact lasts (its buzz is 0.12 to 0.15 s). A buzz a script requests meanwhile waits for it to end:
// the even buzz in both hands from a knockdown or a death, requested a frame after the hit, would otherwise cut short
// the impact that tells which side the hit came from (a new buzz replaces the running one). The latest request for
// each hand is kept.
static const double kHurtImpactMs = 150.0;
struct WaitingBuzz
{
    double at; // when it is due (NowMs), or below 0: none
    float seconds, strength;
};
static WaitingBuzz g_buzzWaiting[2] = { { -1.0, 0.0f, 0.0f }, { -1.0, 0.0f, 0.0f } };

static float HeadYawNow()
{
    const XrQuaternionf& h = g_frameHeadPose.orientation;
    return atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));
}

// The edge's strength now (0 when none shows).
static float HurtStrengthAt(double now)
{
    double t = now - g_hurtMs;
    if (t < 0.0 || t >= g_hurtLastsMs) return 0.0f;
    if (t <= kHurtPeakMs) return g_hurtStrength;
    float f = (float)((t - kHurtPeakMs) / (g_hurtLastsMs - kHurtPeakMs));
    return g_hurtStrength * (1.0f - f * f * (3.0f - 2.0f * f)); // (a smooth fade)
}

void VrHurt(float amount, int kind, float x, float y, float z)
{
    if (amount <= 0.0f) return;
    double now = NowMs();
    float hard = amount > 1.0f ? 1.0f : amount;
    float strength = 0.45f + 0.9f * hard;
    if (strength > 1.0f) strength = 1.0f;
    float showing = HurtStrengthAt(now); // (a second hit while one shows: the stronger kept, the new place and time)
    g_hurtStrength = showing > strength ? showing : strength;
    g_hurtLastsMs = 900.0f + 1600.0f * hard;
    g_hurtKind = kind == 1 || kind == 2 ? kind : 0;
    float length = sqrtf(x * x + y * y + z * z);
    if (g_hurtKind == 1 && length < 0.0001f) g_hurtKind = 0; // (no direction given: all round)
    g_hurtFrom[0] = g_hurtKind == 1 ? x / length : 0.0f;
    g_hurtFrom[1] = g_hurtKind == 1 ? y / length : 0.0f;
    g_hurtFrom[2] = g_hurtKind == 1 ? z / length : 0.0f;
    g_hurtHeadYaw = HeadYawNow();
    g_hurtBeats = hard >= 0.5f ? 4 : (hard >= 0.25f ? 2 : 0);
    g_hurtBeatsDone = 0;
    g_hurtSecondDue = false;
    g_hurtMs = now;
    for (int hand = 0; hand < 2; ++hand) // (a buzz still waiting from a hit just before waits for this one's impact too)
        if (g_buzzWaiting[hand].at >= 0.0) g_buzzWaiting[hand].at = now + kHurtImpactMs;

    // The impact: harder in the hand on the hit's side; both alike for one all round or from below.
    float strong = 0.6f + 0.4f * hard;
    // (From well to one side: that side's hand harder; from ahead or behind: both alike.)
    bool aside = g_hurtKind == 1 && fabsf(g_hurtFrom[0]) >= 0.35f;
    if (aside)
    {
        int nearHand = g_hurtFrom[0] >= 0.0f ? 1 : 0;
        Buzz(nearHand, 0.12f, strong);
        Buzz(1 - nearHand, 0.12f, strong * 0.55f);
    }
    else
    {
        Buzz(0, 0.15f, strong * 0.9f);
        Buzz(1, 0.15f, strong * 0.9f);
    }
    static int logsLeft = 40;
    if (logsLeft > 0)
    {
        --logsLeft;
        const char* from = g_hurtKind == 2 ? "from below (a fall)" : "all round";
        if (g_hurtKind == 1) from = aside ? (g_hurtFrom[0] >= 0.0f ? "from the right" : "from the left") : (g_hurtFrom[2] >= 0.0f ? "from ahead" : "from behind");
        Log("HURT: a hit of %.2f %s (%.2f %.2f %.2f): the view's edge %.0f%% for %.1f s, %d heartbeat%s after it.", amount, from,
            g_hurtFrom[0], g_hurtFrom[1], g_hurtFrom[2], g_hurtStrength * 100.0f, g_hurtLastsMs / 1000.0f, g_hurtBeats,
            g_hurtBeats == 1 ? "" : "s");
    }
}

// A script's buzz (VrScriptBuzz) requested during the hit's impact is kept, to be sent when the impact ends. Returns
// true when it is kept.
static bool HurtHoldsBuzz(int hand, float seconds, float strength)
{
    if (hand < 0 || hand > 1) return false;
    double now = NowMs();
    if (now - g_hurtMs < 0.0 || now - g_hurtMs >= kHurtImpactMs) return false;
    g_buzzWaiting[hand].at = g_hurtMs + kHurtImpactMs;
    g_buzzWaiting[hand].seconds = seconds;
    g_buzzWaiting[hand].strength = strength;
    static int logsLeft = 10;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("HURT: a buzz of %.2f s asked for during the hit's impact (in the %s hand) waits for the impact to end.",
            seconds, hand == 0 ? "left" : "right");
    }
    return true;
}

// Once a headset frame: sends a script's buzz held through the hit's impact, and plays the heartbeat after a hard hit,
// each beat two thumps in both hands, fading (a thump is skipped in a hand still running a longer buzz, such as a
// knockdown's or a death's).
static void UpdateHurtFeel()
{
    double now = NowMs();
    for (int hand = 0; hand < 2; ++hand)
    {
        if (g_buzzWaiting[hand].at < 0.0 || now < g_buzzWaiting[hand].at) continue;
        Buzz(hand, g_buzzWaiting[hand].seconds, g_buzzWaiting[hand].strength);
        g_buzzWaiting[hand].at = -1.0;
    }

    if (g_hurtBeatsDone >= g_hurtBeats) return;
    double t = now - g_hurtMs;
    double beatAt = kHurtBeatFirstMs + g_hurtBeatsDone * kHurtBeatGapMs;
    float fade = 1.0f - (float)g_hurtBeatsDone / (float)(g_hurtBeats + 1);
    bool second = g_hurtSecondDue;
    if (t < beatAt + (second ? kHurtBeatSecondMs : 0.0)) return;
    for (int hand = 0; hand < 2; ++hand)
        if (now >= g_buzzEndsMs[hand]) Buzz(hand, second ? 0.04f : 0.05f, (second ? 0.4f : 0.6f) * fade);
    g_hurtSecondDue = !second;
    if (second) ++g_hurtBeatsDone;
}

// Into this eye's picture: the hit's edge, on the side it came from as the head faces now.
static void DrawHurtEdge(float tanHalfWidth, float tanHalfHeight, int width, int height)
{
    float strength = HurtStrengthAt(NowMs());
    if (strength < 0.004f) return;
    float sideX = 0.0f, sideY = 0.0f;
    if (g_hurtKind == 2) sideY = -1.0f;
    else if (g_hurtKind == 1)
    {
        // The direction it came from in the head's current frame: turned back by the head's turn since (right-handed
        // about the vertical axis: after the head turns left, a hit from ahead is on the right).
        float turned = HeadYawNow() - g_hurtHeadYaw;
        float x = g_hurtFrom[0] * cosf(turned) + g_hurtFrom[2] * sinf(turned);
        float y = g_hurtFrom[1];
        float across = sqrtf(x * x + y * y);
        if (across > 0.15f) // (from straight ahead or behind: all round)
        {
            sideX = x / across;
            sideY = y / across;
        }
    }
    GlDrawHurtEdge(g_gamePicture, width, height, tanHalfWidth, tanHalfHeight, sideX, sideY, strength);
}

// ---------------------------------------------------------------------------
// The change to and from the cutscene screen, through black, so the view never
// jumps between the eyes in the world and the screen in the dark. When the
// body script's news asks for the screen or lets it go (CameraTheatreWanted),
// the view darkens over kTheatreFadeOutMs (the eyes' pictures, or the screen's
// own), the change is made while it is black (CameraSetTheatreShown:
// everything that draws or moves the camera goes by that), it stays black for
// kTheatreBlackMs, and clears over kTheatreFadeInMs. Going back to the eyes,
// the view is re-centered behind the black so that the way the player faced
// the screen is the way the scene left the character facing (FaceAfterScene).
// Asked to change back while darkening, it clears again from where it was;
// asked again while clearing, it darkens again from there. The interface
// (panel, subtitles, hints) is left out while the view is more than half dark.
//
// Going into a scene skips the darkening: the screen comes at once, black, and
// clears over kTheatreFadeInMs. The game starts its scenes by fading in from
// black 0.1 to 0.2 s after the scene begins; a darkening would only be about
// half done by then, so the eyes would show the world at half brightness for a
// moment before the scene.
// ---------------------------------------------------------------------------

static const double kTheatreFadeOutMs = 250.0, kTheatreBlackMs = 150.0, kTheatreFadeInMs = 350.0;
enum TheatrePhase { kTheatreSteady, kTheatreDarkening, kTheatreBlack, kTheatreClearing };
static TheatrePhase g_theatrePhase = kTheatreSteady;
static double g_theatrePhaseMs = 0.0;    // when the present phase began
static float g_theatrePhaseDark = 0.0f;  // how dark it was then
static float g_theatreDark = 0.0f;       // how dark it is now, 0 to 1

// Back to the eyes from the screen: the view is re-centered with the way the
// screen faced (placed in front of the player as it came, and brought back in
// front when they turned away from it) as straight ahead, so that the way the
// player faced it is the way the character faces now. Not the head's own
// facing at that moment: a glance aside as the scene ended would turn the
// whole world.
static void FaceAfterScene()
{
    g_recentreYaw = g_panelYaw;
    g_recentreYawGiven = true;
    g_recenterRequested = true; // (counted like any re-center, so the scripts fit the body to the player again: VrGetRecentreCount)
}

static void SwitchTheatre(bool shown)
{
    CameraSetTheatreShown(shown);
    if (!shown) FaceAfterScene();
    static int logsLeft = 40;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: %s.", shown ? "the cutscene screen comes at once, out of black"
                             : "back to your eyes from the cutscene screen, facing the way the scene left you, behind black");
    }
}

// Is the game paused (its menu open), or has the mod just sent the key that opens it? The paused news comes from the
// hint script's drawing (roll 7021) a frame or two after the game has paused and blurred its world; the key is known
// on this side before that.
static const double kPauseAskedMs = 400.0;
static bool GamePausedOrAsked()
{
    return CameraGamePaused() || NowMs() - g_menuKeySentMs < kPauseAskedMs;
}

// Once per headset frame, before the frame decides what it shows.
static void UpdateTheatreChange()
{
    bool shown = CameraTheatre();
    // The screen is kept while the game is paused during a scene. The body script's news that a scene plays stops
    // while the game is paused (its update does not run), so otherwise the screen would be let go 0.4 s into a pause,
    // the paused scene shown through the eyes, blurred by the game's menu, and the screen brought back as the menu
    // closes. A pause never starts the screen, and it is let go for the main menu or a loading screen, as quitting
    // from the menu brings.
    bool wanted = CameraTheatreWanted() || (shown && GamePausedOrAsked() && !CameraLoadingScreen() && !MenuRoomOn());
    double now = NowMs();
    // Into a scene, from whatever phase: the screen at once, black, clearing
    // (see above).
    if (wanted && !shown)
    {
        SwitchTheatre(true);
        g_theatreDark = 1.0f;
        g_theatrePhase = kTheatreClearing;
        g_theatrePhaseMs = now;
        g_theatrePhaseDark = 1.0f;
        return;
    }
    double t = now - g_theatrePhaseMs;
    switch (g_theatrePhase)
    {
    case kTheatreSteady:
        g_theatreDark = 0.0f;
        if (wanted != shown)
        {
            g_theatrePhase = kTheatreDarkening;
            g_theatrePhaseMs = now;
            g_theatrePhaseDark = 0.0f;
        }
        break;
    case kTheatreDarkening:
        if (wanted == shown)
        {
            // Called off before the change: clear again from here.
            g_theatrePhase = kTheatreClearing;
            g_theatrePhaseMs = now;
            g_theatrePhaseDark = g_theatreDark;
            break;
        }
        g_theatreDark = g_theatrePhaseDark + (float)(t / kTheatreFadeOutMs);
        if (g_theatreDark >= 1.0f)
        {
            g_theatreDark = 1.0f;
            SwitchTheatre(wanted);
            g_theatrePhase = kTheatreBlack;
            g_theatrePhaseMs = now;
        }
        break;
    case kTheatreBlack:
        g_theatreDark = 1.0f;
        if (wanted != shown)
        {
            // Asked to change back while black: done at once, and the black held again.
            SwitchTheatre(wanted);
            g_theatrePhaseMs = now;
        }
        else if (t >= kTheatreBlackMs)
        {
            g_theatrePhase = kTheatreClearing;
            g_theatrePhaseMs = now;
            g_theatrePhaseDark = 1.0f;
        }
        break;
    case kTheatreClearing:
        if (wanted != shown)
        {
            g_theatrePhase = kTheatreDarkening;
            g_theatrePhaseMs = now;
            g_theatrePhaseDark = g_theatreDark;
            break;
        }
        g_theatreDark = g_theatrePhaseDark - (float)(t / kTheatreFadeInMs);
        if (g_theatreDark <= 0.0f)
        {
            g_theatreDark = 0.0f;
            g_theatrePhase = kTheatreSteady;
        }
        break;
    }
}

// The game's fades to black (before a cutscene, when a level starts, when the
// player is moved somewhere). The game draws a fade as part of its interface,
// as a darkening of what is already under it (a full-screen picture multiplied
// in), so it leaves nothing in the mod's picture of the interface and the
// world would stay in view. The body script reports how far the fade has gone
// and the eyes' pictures are darkened to match, as is the cutscene screen's
// own picture.
static void DrawFade(int width, int height)
{
    float fade = CameraFadeAlpha();
    // (And the mod's own darkening for a re-center: see VrRecenter. And for a
    // change to or from the cutscene screen: see UpdateTheatreChange.)
    float recentre = RecentreFade();
    if (recentre > fade) fade = recentre;
    if (g_theatreDark > fade) fade = g_theatreDark;
    float startup = StartupDim(); // (and behind the start-up page)
    if (startup > fade) fade = startup;
    float card = DedicationDark(); // (and behind the studio's card, then the room fading in after it)
    if (card > fade) fade = card;
    if (fade < 0.004f) return;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("VR: the game is fading to or from black; the eyes' pictures are darkened to match.");
    }
    const float black[3] = { 0.0f, 0.0f, 0.0f };
    GlDrawSolid(g_gamePicture, width, height, black, fade);
}

// ---------------------------------------------------------------------------
// Setup (runs once)
// ---------------------------------------------------------------------------

static bool OpenGlIsSupported()
{
    uint32_t count = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
               "Asking the VR software what it supports"))
        return false;

    XrExtensionProperties* extensions = new XrExtensionProperties[count];
    for (uint32_t i = 0; i < count; ++i)
    {
        extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        extensions[i].next = nullptr;
    }

    bool found = false;
    if (Check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions),
              "Listing what the VR software supports"))
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (strcmp(extensions[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0)
                found = true;
        }
    }

    delete[] extensions;
    return found;
}

static bool TestMapNoteIsNew(const wchar_t* noteName, bool& seen);

static bool Setup()
{
    Log("VR: starting setup.");

    // A test run with a stand-in OpenXR runtime instead of a headset? (See g_standIn.) The OpenXR
    // loader reads XR_RUNTIME_JSON the first time it is called, just below; only this process sees it.
    bool standInSeen = false;
    if (TestMapNoteIsNew(L"BunkerVR_start_fake_headset.txt", standInSeen))
    {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        while (length > 0 && path[length - 1] != L'\\') --length;
        path[length] = L'\0';
        if (length > 0 && length + 40 < MAX_PATH)
        {
            lstrcatW(path, L"BunkerVR\\tools\\fakexr\\fakexr.json");
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && SetEnvironmentVariableW(L"XR_RUNTIME_JSON", path))
            {
                g_standIn = true;
                Log("VR: a run with the pretend headset: no dedication.");
                CameraWatchSoundListener();
                StandInWatchStart();
            }
        }
    }

    // 1. Check the VR software can work with OpenGL, which is what the game draws with.
    if (!OpenGlIsSupported())
    {
        Log("VR ERROR: the active VR software does not support OpenGL games (or no VR software was found).");
        return false;
    }

    // 2. Connect to the VR software.
    XrInstanceCreateInfo instanceInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    lstrcpynA(instanceInfo.applicationInfo.applicationName, "BunkerVR", XR_MAX_APPLICATION_NAME_SIZE);
    instanceInfo.applicationInfo.applicationVersion = 1;
    lstrcpynA(instanceInfo.applicationInfo.engineName, "HPL3", XR_MAX_ENGINE_NAME_SIZE);
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    const char* extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;

    if (!Check(xrCreateInstance(&instanceInfo, &g_instance), "Connecting to the VR software"))
        return false;

    XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
    if (Check(xrGetInstanceProperties(g_instance, &instanceProperties), "Reading the VR software's name"))
    {
        Log("VR: connected to \"%s\" version %u.%u.%u", instanceProperties.runtimeName,
            (unsigned)XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
            (unsigned)XR_VERSION_MINOR(instanceProperties.runtimeVersion),
            (unsigned)XR_VERSION_PATCH(instanceProperties.runtimeVersion));
        // (SteamVR shows panels in overlays it hands out by their place in each frame's list, and
        // mixes up their pictures: the mod draws them into the eyes' pictures itself.)
        g_runtimeSteamVr = strncmp(instanceProperties.runtimeName, "SteamVR", 7) == 0;
        g_panelsInEyes = g_runtimeSteamVr;
        if (g_panelsInEyes)
            Log("VR: SteamVR: the panels (the game's interface, the menus, the pointer, the labels) are drawn into the eyes' "
                "pictures by the mod, and SteamVR is sent those alone.");
    }

    // 3. Find the headset.
    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(g_instance, &systemInfo, &g_systemId), "Finding the headset (is it plugged in?)"))
        return false;

    XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
    if (Check(xrGetSystemProperties(g_instance, g_systemId, &systemProperties), "Reading the headset's name"))
        Log("VR: headset found: \"%s\"", systemProperties.systemName);
    // (Before the view is first centered, so that the saved floor used is this headset's.)
    ChooseHeadsetFloor(instanceProperties.runtimeName[0] ? instanceProperties.runtimeName : "unknown software",
                       systemProperties.systemName[0] ? systemProperties.systemName : "unknown headset");

    // 4. Ask how big each eye's picture should be (used with the panels drawn into the
    // eyes' pictures, on SteamVR: EyePictureSizeForPanels).
    XrViewConfigurationView eyeViews[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t eyeCount = 0;
    if (Check(xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                2, &eyeCount, eyeViews),
              "Reading the recommended picture size"))
    {
        Log("VR: recommended picture size per eye: %u x %u", (unsigned)eyeViews[0].recommendedImageRectWidth,
            (unsigned)eyeViews[0].recommendedImageRectHeight);
        g_recommendedWidth = (int)eyeViews[0].recommendedImageRectWidth;
        g_recommendedHeight = (int)eyeViews[0].recommendedImageRectHeight;
    }

    XrViewConfigurationProperties viewProperties = { XR_TYPE_VIEW_CONFIGURATION_PROPERTIES };
    if (XR_SUCCEEDED(xrGetViewConfigurationProperties(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      &viewProperties)))
    {
        Log("VR: the headset software %s pictures with a different field of view from its own.",
            viewProperties.fovMutable ? "says it accepts" : "does NOT accept");
    }

    // 5. The VR software requires the app to ask about its OpenGL needs before going further.
    PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
    if (!Check(xrGetInstanceProcAddr(g_instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                     (PFN_xrVoidFunction*)&getRequirements),
               "Looking up the OpenGL requirements function"))
        return false;

    XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!Check(getRequirements(g_instance, g_systemId, &requirements), "Reading the OpenGL requirements"))
        return false;

    // 6. Link the game's OpenGL drawing to the headset.
    XrGraphicsBindingOpenGLWin32KHR graphics = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    graphics.hDC = wglGetCurrentDC();
    graphics.hGLRC = wglGetCurrentContext();
    if (graphics.hDC == nullptr || graphics.hGLRC == nullptr)
    {
        Log("VR ERROR: the game's OpenGL context is not active on this thread.");
        return false;
    }

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &graphics;
    sessionInfo.systemId = g_systemId;
    if (!Check(xrCreateSession(g_instance, &sessionInfo, &g_session), "Linking the game to the headset"))
        return false;

    // 7. Create the two reference points used to measure where the head is.
    XrPosef noOffset = {};
    noOffset.orientation.w = 1.0f;

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace = noOffset;

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(g_session, &spaceInfo, &g_floorSpace), "Creating the room reference point"))
        return false;

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(g_session, &spaceInfo, &g_headSpace), "Creating the head reference point"))
        return false;

    // The play area, for where the floor is (see "Measure my size").
    // Optional: without it the floor is found by touching it.
    XrReferenceSpaceType spaceTypes[16];
    uint32_t spaceTypeCount = 0;
    bool haveStage = false;
    if (XR_SUCCEEDED(xrEnumerateReferenceSpaces(g_session, 16, &spaceTypeCount, spaceTypes)))
        for (uint32_t i = 0; i < spaceTypeCount; ++i)
            if (spaceTypes[i] == XR_REFERENCE_SPACE_TYPE_STAGE) haveStage = true;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (haveStage && !Check(xrCreateReferenceSpace(g_session, &spaceInfo, &g_stageSpace), "Creating the play area's reference point"))
        g_stageSpace = XR_NULL_HANDLE;
    if (g_stageSpace != XR_NULL_HANDLE)
        Log("VR: the headset has a play area: the floor is known from it.");
    else
        Log("VR: the headset has no play area: the floor is known only once touched with a controller (Measure my size).");

    g_handsReady = SetupHands();
    if (!g_handsReady) Log("VR: carrying on without hand controllers.");

    Log("VR: setup finished. Waiting for the headset to become ready.");
    return true;
}

// ---------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------

// The headset sets the pace at 90 frames per second. If the game also waits
// for the monitor before showing each frame, the two waits fight each other
// and the headset stutters. So ask the game's window library (SDL) not to wait.
// While the headset is off it is switched back on, so that the game runs at
// the monitor's rate rather than as fast as it can (close to 300 frames a
// second, for nothing).
static void SetMonitorSync(bool on)
{
    HMODULE sdl = GetModuleHandleA("SDL2.dll");
    if (!sdl) return;

    typedef int (*SetSwapIntervalFunction)(int interval);
    SetSwapIntervalFunction setSwapInterval = (SetSwapIntervalFunction)GetProcAddress(sdl, "SDL_GL_SetSwapInterval");
    if (setSwapInterval && setSwapInterval(on ? 1 : 0) == 0)
        Log(on ? "VR: monitor sync (vsync) switched back on while the headset is off."
               : "VR: monitor sync (vsync) switched off so the headset can set the pace.");

    // Logs what the window library says it waits for, and the PC screen the game's window is on. A graphics
    // driver's own forced vertical sync, or a 60 Hz screen, can hold the game to 60 frames a second against the
    // headset's 90; it shows in the MONITOR lines of the timings.
    typedef int (*GetSwapIntervalFunction)();
    GetSwapIntervalFunction getSwapInterval = (GetSwapIntervalFunction)GetProcAddress(sdl, "SDL_GL_GetSwapInterval");
    HMONITOR monitor = MonitorFromWindow((HWND)VrGameWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo) && EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        Log("MONITOR: the game's window is on a PC screen of %lu x %lu at %lu Hz%s (%d screen%s in all); the window library's swap interval is now %d.",
            mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) ? ", the main one" : "",
            GetSystemMetrics(SM_CMONITORS), GetSystemMetrics(SM_CMONITORS) == 1 ? "" : "s", getSwapInterval ? getSwapInterval() : -1);
}

// Logs which of the mod's controls the headset software gives no button for,
// with the controllers it currently reads (for example, SteamVR gives games no
// left menu button on Touch controllers and logs "Invalid input type
// button::click for path (/user/hand/left/input/menu)"). Logged each time the
// controllers change, at most 6 times a run.
static void LogUnboundControls()
{
    static int logsLeft = 6;
    if (g_session == XR_NULL_HANDLE) return;
    // (The Controls page says how to pause where the left menu button is not
    // the game's, as on SteamVR.)
    if (g_menuAction != XR_NULL_HANDLE)
    {
        XrBoundSourcesForActionEnumerateInfo info = { XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
        info.action = g_menuAction;
        uint32_t sources = 0;
        if (XR_SUCCEEDED(xrEnumerateBoundSourcesForAction(g_session, &info, 0, &sources, nullptr)))
            VrMenuSetGameMenuButton(sources > 0);
    }
    if (logsLeft <= 0) return;
    struct { XrAction action; const char* name; } controls[] = {
        { g_menuAction, "the left menu button (the game's menu: use Y, Player, Open Game Menu; hold Y to recenter)" },
        { g_vrMenuAction, "Y (the BunkerVR menu)" },
        { g_aimAction2, "X (put away)" },
        { g_gunAction, "A (the revolver)" },
        { g_crouchAction, "B (the inventory)" },
        { g_sprintAction, "the left stick's click (sprint)" },
        { g_inventoryAction, "the right stick's click (the flashlight)" },
        { g_squeezeAction, "the grips" },
        { g_triggerAction, "the triggers" },
        { g_moveAction, "the left stick (walk)" },
        { g_turnAction, "the right stick (turn)" },
    };
    char missing[600] = "";
    int count = 0;
    for (const auto& control : controls)
    {
        if (control.action == XR_NULL_HANDLE) continue;
        XrBoundSourcesForActionEnumerateInfo info = { XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
        info.action = control.action;
        uint32_t sources = 0;
        if (!XR_SUCCEEDED(xrEnumerateBoundSourcesForAction(g_session, &info, 0, &sources, nullptr)) || sources > 0) continue;
        if (count++ > 0) strncat_s(missing, sizeof(missing), "; ", _TRUNCATE);
        strncat_s(missing, sizeof(missing), control.name, _TRUNCATE);
    }
    --logsLeft;
    if (count == 0) Log("VR: every control has a button on these controllers.");
    else Log("VR: no button for %d control%s on these controllers: %s.", count, count == 1 ? "" : "s", missing);
}

// The VR software sends messages, for example "the headset is ready" or "the
// headset was taken off". This reads and reacts to them.
static void HandleEvents()
{
    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED && g_session != XR_NULL_HANDLE)
        {
            // Which kind of controller the headset software now reads each
            // hand's inputs from.
            for (int hand = 0; hand < 2; ++hand)
            {
                XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char name[XR_MAX_PATH_LENGTH] = "";
                uint32_t length = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_session, ToPath(hand == 0 ? "/user/hand/left" : "/user/hand/right"), &state)) &&
                    state.interactionProfile != XR_NULL_PATH)
                    xrPathToString(g_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &length, name);
                g_indexHand[hand] = strcmp(name, "/interaction_profiles/valve/index_controller") == 0;
                g_indexGrabbing[hand] = false;
                Log("VR: the %s hand's controller is now read as %s.", hand == 0 ? "left" : "right", name[0] ? name : "nothing (none in use)");
            }
            LogUnboundControls();
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const XrEventDataSessionStateChanged* change = (const XrEventDataSessionStateChanged*)&event;
            Log("VR: headset state is now %s", SessionStateName(change->state));
            g_sessionFocused = change->state == XR_SESSION_STATE_FOCUSED;

            if (change->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(g_session, &beginInfo), "Starting the headset session"))
                {
                    g_sessionRunning = true;
                    if (g_headsetOff) Log("VR: the headset is back on.");
                    g_headsetOff = false;
                    SetMonitorSync(false);
                    ResetLogicHalf();
                }
            }
            else if (change->state == XR_SESSION_STATE_STOPPING)
            {
                g_sessionRunning = false;
                g_frameOpen = false;
                // (The view keeps its VR shape: see g_headsetOff.)
                g_headsetOff = true;
                Log("VR: the headset was taken off: the game stays in VR, head and hands held where they were.");
                SetInteractHeld(false);
                ReleaseMoveKeys();
                Check(xrEndSession(g_session), "Stopping the headset session");
                SetMonitorSync(true);
                ResetLogicHalf();
            }
            else if (change->state == XR_SESSION_STATE_EXITING || change->state == XR_SESSION_STATE_LOSS_PENDING)
            {
                g_sessionRunning = false;
                g_state = VrState::Failed;
                if (change->state == XR_SESSION_STATE_EXITING)
                {
                    // (The headset software's own Quit, or the software closing: the game
                    // closes too, by its own exit, as VR games do (cVR_QuitAsked, asked by
                    // the menu script each frame).)
                    g_quitAsked = true;
                    Log("VR: the headset software asks the game to close (its own Quit, or it is closing): the game closes.");
                }
                else Log("VR: the headset software closed the session. VR is off until the game restarts.");
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            // The headset's own "Reset view" (holding the Oculus button)
            // moves the room the head is measured in; the mod's center, kept
            // in the old room, would be off by as much. So it is re-centered
            // too, behind a moment of black.
            const XrEventDataReferenceSpaceChangePending* change = (const XrEventDataReferenceSpaceChangePending*)&event;
            if (change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
            {
                Log("VR: the headset's own \"Reset view\" was used: the view is re-centred to match.");
                RecenterBlackNow();
                MeasureRoomMoved();
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            g_sessionRunning = false;
            g_sessionFocused = false;
            g_state = VrState::Failed;
            Log("VR: the headset software is shutting down. VR is off until the game restarts.");
        }

        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// Logs the field of view of each eye, once.
static void LogEyes(XrTime time)
{
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = time;
    locateInfo.space = g_headSpace;

    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    XrView eyes[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t eyeCount = 0;
    if (!Check(xrLocateViews(g_session, &locateInfo, &viewState, 2, &eyeCount, eyes), "Reading the eye positions"))
        return;
    if (eyeCount < 2) return;

    const char* names[2] = { "left eye ", "right eye" };
    for (int i = 0; i < 2; ++i)
    {
        Log("VR: %s is %.1f mm from the centre; sees %.1f deg left, %.1f right, %.1f up, %.1f down",
            names[i], eyes[i].pose.position.x * 1000.0f,
            -ToDegrees(eyes[i].fov.angleLeft), ToDegrees(eyes[i].fov.angleRight),
            ToDegrees(eyes[i].fov.angleUp), -ToDegrees(eyes[i].fov.angleDown));
    }
    g_loggedEyes = true;
}

// ---------------------------------------------------------------------------
// The pictures shared with the headset
// ---------------------------------------------------------------------------

static int64_t ChooseSwapchainFormat();

static void DestroySwapchains()
{
    for (int eye = 0; eye < 2; ++eye)
    {
        if (g_swapchains[eye] != XR_NULL_HANDLE) XrDestroySwapchainGl(g_swapchains[eye]);
        g_swapchains[eye] = XR_NULL_HANDLE;
        g_swapchainImageCounts[eye] = 0;
        g_eyes[eye].hasPicture = false;
    }
}

// Makes sure the shared pictures exist (one set per eye) and match the game
// window's size.
static bool EnsureSwapchains(int width, int height)
{
    bool exist = g_swapchains[0] != XR_NULL_HANDLE && g_swapchains[1] != XR_NULL_HANDLE;
    if (exist && width == g_swapchainWidth && height == g_swapchainHeight) return true;

    if (exist) Log("VR: the game window changed size; recreating the headset pictures.");
    DestroySwapchains();

    int64_t chosen = ChooseSwapchainFormat();
    if (chosen == 0)
    {
        Log("VR ERROR: the headset software offers no picture format we can use.");
        return false;
    }

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = chosen;
        info.sampleCount = 1;
        info.width = (uint32_t)width;
        info.height = (uint32_t)height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(XrCreateSwapchainGl(g_session, &info, &g_swapchains[eye], true), "Creating the headset pictures"))
        {
            g_swapchains[eye] = XR_NULL_HANDLE;
            DestroySwapchains();
            return false;
        }

        for (int i = 0; i < 16; ++i)
        {
            g_swapchainImages[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            g_swapchainImages[eye][i].next = nullptr;
            g_swapchainImages[eye][i].image = 0;
        }
        if (!Check(XrEnumerateSwapchainImagesGl(g_swapchains[eye], 16, &g_swapchainImageCounts[eye],
                                              (XrSwapchainImageBaseHeader*)g_swapchainImages[eye]),
                   "Getting the headset pictures"))
        {
            DestroySwapchains();
            return false;
        }
    }

    g_swapchainWidth = width;
    g_swapchainHeight = height;
    Log("VR: created headset pictures for both eyes: %u each, %d x %d, format 0x%llX (%s)",
        (unsigned)g_swapchainImageCounts[0], width, height, (unsigned long long)chosen,
        chosen == kGlSrgb8Alpha8 ? "standard colour" : "plain");
    return true;
}

// The eyes' pictures with the panels drawn into them (SteamVR): the size the
// headset software asks for, whatever the game's picture size (an eye's
// picture remade at another size is unreliable there: its copies into the
// compositor keep their first size, see PanelsIntoEyes), so that the panels'
// writing is as sharp as the headset can show it. The game's own cut-out is
// stretched to it, and can be much smaller (for example 1795 x 1043 against a
// Quest 3's 1868 x 1996, half the vertical detail).
static void EyePictureSizeForPanels(int& width, int& height)
{
    width = g_recommendedWidth >= 256 ? g_recommendedWidth : 2048;
    height = g_recommendedHeight >= 256 ? g_recommendedHeight : 2048;
    if (width > 4096) width = 4096;
    if (height > 4096) height = 4096;
}

// The pictures each eye's latest view of the world is kept in (SteamVR), the
// size of the headset's.
static bool EnsureEyeKeep()
{
    if (g_eyeKeepTexture[0] != 0 && g_eyeKeepTexture[1] != 0 && g_eyeKeepWidth == g_swapchainWidth &&
        g_eyeKeepHeight == g_swapchainHeight)
        return true;
    for (int eye = 0; eye < 2; ++eye)
    {
        GlDeleteTarget(g_eyeKeepTexture[eye], g_eyeKeepFramebuffer[eye]);
        g_eyeKeepSerial[eye] = 0;
        g_eyes[eye].hasPicture = false;
    }
    g_eyeKeepWidth = g_eyeKeepHeight = 0;
    if (g_swapchainWidth <= 0 || g_swapchainHeight <= 0) return false;
    for (int eye = 0; eye < 2; ++eye)
    {
        if (!GlCreateTarget(g_swapchainWidth, g_swapchainHeight, g_eyeKeepTexture[eye], g_eyeKeepFramebuffer[eye]))
        {
            GlDeleteTarget(g_eyeKeepTexture[0], g_eyeKeepFramebuffer[0]);
            GlDeleteTarget(g_eyeKeepTexture[1], g_eyeKeepFramebuffer[1]);
            return false;
        }
    }
    g_eyeKeepWidth = g_swapchainWidth;
    g_eyeKeepHeight = g_swapchainHeight;
    return true;
}

// The game's finished picture (`source`: left, bottom, right, top in window
// pixels) into a framebuffer or a texture of the mod's, stretched to fill
// `width` x `height`. Taken from the engine's own drawing target (stereo in
// one frame), the picture has not had the engine's image correction yet (the
// player's gamma: CameraPictureCorrection), so it gets it on the way. Taken
// from the window, at its swap, it already has it.
static void CopyGamePicture(const int source[4], GLuint targetFramebuffer, int width, int height)
{
    float gamma = 1.0f, brightness = 1.0f, contrast = 1.0f;
    if (g_gamePicture != 0 && CameraPictureCorrection(gamma, brightness, contrast) &&
        GlCopyCorrected(g_gamePicture, source[0], source[1], source[2], source[3], targetFramebuffer, 0, 0, width, height, gamma,
                        brightness, contrast))
        return;
    GlCopy(g_gamePicture, source[0], source[1], source[2], source[3], targetFramebuffer, 0, 0, width, height);
}

static void CopyGamePictureToTexture(const int source[4], GLuint texture, int width, int height)
{
    float gamma = 1.0f, brightness = 1.0f, contrast = 1.0f;
    if (g_gamePicture != 0 && CameraPictureCorrection(gamma, brightness, contrast) &&
        GlCopyToTextureCorrected(g_gamePicture, source[0], source[1], source[2], source[3], texture, width, height, gamma,
                                 brightness, contrast))
        return;
    GlCopyToTexture(g_gamePicture, source[0], source[1], source[2], source[3], texture, width, height);
}

// Copies part of the game's finished picture into the next shared picture of
// one eye. `source` is left, bottom, right, top in window pixels. (On SteamVR
// into the eye's kept picture instead: PanelsIntoEyes hands it on.)
static bool CopyGameImageToHeadset(int eye, const int source[4])
{
    if (g_panelsInEyes)
    {
        if (!EnsureEyeKeep()) return false;
        CopyGamePicture(source, g_eyeKeepFramebuffer[eye], g_eyeKeepWidth, g_eyeKeepHeight);
        g_eyeKeepSerial[eye] = g_frameSerialAtBegin;
        return true;
    }

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(g_swapchains[eye], nullptr, &index), "Taking the next headset picture"))
        return false;

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(g_swapchains[eye], &waitInfo), "Waiting for the headset picture");

    if (ready && index < g_swapchainImageCounts[eye])
        CopyGamePictureToTexture(source, g_swapchainImages[eye][index].image, g_swapchainWidth, g_swapchainHeight);

    return Check(XrReleaseSwapchainImageGl(g_swapchains[eye], nullptr), "Handing the headset picture back") && ready;
}

// Keeps the left eye's cut-out in a picture of the mod's own until the right
// eye's picture has been drawn too.
static bool HoldLeftPicture(const int source[4], const XrFovf& covered)
{
    if (g_holdTexture == 0 || g_holdWidth != g_swapchainWidth || g_holdHeight != g_swapchainHeight)
    {
        GlDeleteTarget(g_holdTexture, g_holdFramebuffer);
        g_holdValid = false;
        if (!GlCreateTarget(g_swapchainWidth, g_swapchainHeight, g_holdTexture, g_holdFramebuffer)) return false;
        g_holdWidth = g_swapchainWidth;
        g_holdHeight = g_swapchainHeight;
    }

    CopyGamePicture(source, g_holdFramebuffer, g_holdWidth, g_holdHeight);
    g_holdValid = true;
    g_holdPose = g_frameEyePose;
    g_holdFov = covered;
    return true;
}

// Hands the held left-eye picture to the headset.
static bool SendHeldLeftPicture()
{
    if (!g_holdValid || g_holdWidth != g_swapchainWidth || g_holdHeight != g_swapchainHeight) return false;
    g_holdValid = false;

    if (g_panelsInEyes)
    {
        // (On SteamVR into the left eye's kept picture: PanelsIntoEyes hands it on.)
        if (!EnsureEyeKeep()) return false;
        GlCopy(g_holdFramebuffer, 0, 0, g_holdWidth, g_holdHeight, g_eyeKeepFramebuffer[0], 0, 0, g_eyeKeepWidth, g_eyeKeepHeight);
        g_eyeKeepSerial[0] = g_frameSerialAtBegin;
    }
    else
    {
        uint32_t index = 0;
        if (!Check(XrAcquireSwapchainImageGl(g_swapchains[0], nullptr, &index), "Taking the next headset picture"))
            return false;

        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        bool ready = Check(XrWaitSwapchainImageGl(g_swapchains[0], &waitInfo), "Waiting for the headset picture");

        if (ready && index < g_swapchainImageCounts[0])
            GlCopyToTexture(g_holdFramebuffer, 0, 0, g_holdWidth, g_holdHeight,
                            g_swapchainImages[0][index].image, g_swapchainWidth, g_swapchainHeight);

        if (!Check(XrReleaseSwapchainImageGl(g_swapchains[0], nullptr), "Handing the headset picture back") || !ready)
            return false;
    }

    g_eyes[0].hasPicture = true;
    g_eyes[0].pose = g_holdPose;
    g_eyes[0].fov = g_holdFov;
    return true;
}

// The game's inventory book (inventory_background.dds, drawn across the whole
// interface picture) is solid from 34% of the picture's width to its right
// edge: its middle is 17% of the width right of the picture's. Its left part
// shows the flat game's raised hand, and is made see-through in VR.
static const float kInventoryBookMiddle = 0.17f;
// A point of the interface's picture to be put in front instead (shares of it across and down), as the scripts last
// reported it (cVR_PanelFocus, SketchbookUIHandler.hps): a photo opened in the inventory is drawn left of the book's
// middle, and would otherwise sit some 40% of the panel's width off to the left. Stale after 0.25 s.
static float g_panelFocusX = 0.5f;
static double g_panelFocusMs = -100000.0;

void VrSetPanelFocus(float across, float down)
{
    (void)down; // (the photo stands near enough the middle up and down: only across is used)
    g_panelFocusX = across < 0.0f ? 0.0f : (across > 1.0f ? 1.0f : across);
    g_panelFocusMs = NowMs();
}

// The cutscene screen: this wide, this far away (meters). The words the game
// writes on black and its loading screen are shown on it too, at its size and
// in its place.
static const float kScreenWidth = 3.4f, kScreenDistance = 2.6f;
// The screen holds still in the room (see UpdateScreenPlacement); it comes
// back in front, at once, only after the head has looked more than this far
// away from it for this long.
static const float kScreenAwayAngle = 1.05f; // radians (60 degrees)
static const double kScreenAwayMs = 1500.0;

// Decides where the interface panel floats. It stays put in the room; when it
// ends up well off to one side (the player turned or walked away), it glides
// back in front. It never moves while a hand is pointing at it. `sideways`:
// the panel's middle is put that far to the right of straight ahead (meters;
// negative: to the left), so that some other part of it is in front.
static void UpdatePanelPlacement(float distance, bool pointerOnPanel, float sideways)
{
    const XrQuaternionf& h = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;

    // Which way the head faces, left/right only.
    float headYaw = atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));
    float target[3] = { p.x - sinf(headYaw) * distance + cosf(headYaw) * sideways, p.y,
                        p.z - cosf(headYaw) * distance - sinf(headYaw) * sideways };

    bool appeared = !g_panelPlaced || g_panelSnapRequested || g_frameSerialAtBegin - g_panelLastShownSerial > 45;
    g_panelLastShownSerial = g_frameSerialAtBegin;

    if (appeared)
    {
        // Newly shown: put it straight in front.
        for (int i = 0; i < 3; ++i) g_panelPosition[i] = target[i];
        g_panelYaw = headYaw;
        g_panelPlaced = true;
        g_panelSnapRequested = false;
        g_panelMoving = false;
        return;
    }

    // How far off to the side, how much nearer or further, how much higher or
    // lower is it? (Than where it is wanted: off to the side by `sideways`.)
    float dx = g_panelPosition[0] - p.x, dz = g_panelPosition[2] - p.z;
    float flatDistance = sqrtf(dx * dx + dz * dz);
    float angle = atan2f(-dx, -dz) - atan2f(-(target[0] - p.x), -(target[2] - p.z));
    while (angle > 3.14159265f) angle -= 6.28318531f;
    while (angle < -3.14159265f) angle += 6.28318531f;
    float height = g_panelPosition[1] - p.y;
    distance = sqrtf(distance * distance + sideways * sideways); // (the wanted place's own distance)

    if (!pointerOnPanel &&
        (fabsf(angle) > 0.61f || fabsf(flatDistance - distance) > 0.45f || fabsf(height) > 0.40f))
        g_panelMoving = true;

    if (g_panelMoving)
    {
        const float step = 0.07f; // the share of the remaining way covered each frame
        for (int i = 0; i < 3; ++i) g_panelPosition[i] += (target[i] - g_panelPosition[i]) * step;

        float turn = headYaw - g_panelYaw;
        while (turn > 3.14159265f) turn -= 6.28318531f;
        while (turn < -3.14159265f) turn += 6.28318531f;
        g_panelYaw += turn * step;

        if (fabsf(angle) < 0.05f && fabsf(flatDistance - distance) < 0.05f && fabsf(height) < 0.05f)
            g_panelMoving = false;
    }
}

// The main menu in its room (see UpdateMenuRoom): in front of the
// room's middle, the way the room faces, whichever way the head looks or moves.
static void PlaceMenuInRoom(float distance)
{
    float yaw = g_referenceYaw;
    g_panelPosition[0] = g_referencePosition[0] - sinf(yaw) * distance;
    g_panelPosition[1] = g_menuRoomEyes + kMenuRoomLift;
    g_panelPosition[2] = g_referencePosition[2] - cosf(yaw) * distance;
    g_panelYaw = yaw;
    g_panelPlaced = true;
    g_panelSnapRequested = false;
    g_panelMoving = false;
    g_panelLastShownSerial = g_frameSerialAtBegin;
}

// Where the cutscene screen stands (with the words on black and the loading
// screen on it): put straight in front as it comes, then held still in the
// room. It doesn't glide after the head: moving it a step each game frame, as
// UpdatePanelPlacement does, shows as a jitter in a scene's slow frames. One
// of them giving way to another (a scene, its words on black, the next scene)
// keeps it where it is: the caller asks for it to be put in front only as it
// comes after the ordinary panel. Brought back in front at once when the head
// has looked more than kScreenAwayAngle away from it for kScreenAwayMs.
static void UpdateScreenPlacement(float distance)
{
    const XrQuaternionf& h = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;
    float headYaw = atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));

    static double awaySinceMs = -1.0;
    bool appeared = !g_panelPlaced || g_panelSnapRequested || g_frameSerialAtBegin - g_panelLastShownSerial > 45;
    g_panelLastShownSerial = g_frameSerialAtBegin;
    if (!appeared)
    {
        float dx = g_panelPosition[0] - p.x, dz = g_panelPosition[2] - p.z;
        float angle = atan2f(-dx, -dz) - headYaw;
        while (angle > 3.14159265f) angle -= 6.28318531f;
        while (angle < -3.14159265f) angle += 6.28318531f;
        if (fabsf(angle) > kScreenAwayAngle) { if (awaySinceMs < 0.0) awaySinceMs = NowMs(); }
        else awaySinceMs = -1.0;
        if (awaySinceMs >= 0.0 && NowMs() - awaySinceMs >= kScreenAwayMs)
        {
            appeared = true;
            static int logsLeft = 10;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: the head looked away from the cutscene screen for %.1f s: it is put in front again.", kScreenAwayMs / 1000.0);
            }
        }
    }
    if (!appeared) return;

    g_panelPosition[0] = p.x - sinf(headYaw) * distance;
    g_panelPosition[1] = p.y;
    g_panelPosition[2] = p.z - cosf(headYaw) * distance;
    g_panelYaw = headYaw;
    g_panelPlaced = true;
    g_panelSnapRequested = false;
    g_panelMoving = false;
    awaySinceMs = -1.0;
}

// ---------------------------------------------------------------------------
// The interface in pieces during play. Instead of a flat panel that follows
// the head, two parts of the interface picture are shown, each a layer of its
// own, cut from the same picture (the game's own drawing):
// - the strip: the bottom band, where the engine writes subtitles and
//   captions (and also the item counter and the new note and objective icons
//   in its right-hand corner), below eye level;
// - the card: the band near the top where the hint box is (its "HINT" header
//   17.5% down the screen, its lines below), above the strip.
// Like Half-Life: Alyx's subtitles, they stand still at a readable place and,
// when they leave the view, glide to a new one and stay there.
// Only during play: the body script says the player is in play every update
// (cVR_InterfaceInPieces), the game is looking with the mouse (no pointer
// shown: no menu, inventory or map), not paused, not loading, not on the
// cutscene screen. Otherwise the whole interface goes on the big panel.
// Logged as it changes.
// ---------------------------------------------------------------------------

// The parts, as fractions of the picture from its bottom-left corner (the
// OpenGL picture's own rows; the panel's whole picture shows upright so):
// left, bottom, right, top. The strip: 0% to 30% up (the screen's bottom
// band, 70% down to the bottom edge); the card: 10% to 90% across, 55% to
// 90% up (10% to 45% down).
static const float kStripPart[4] = { 0.0f, 0.0f, 1.0f, 0.30f };
static const float kCardPart[4] = { 0.10f, 0.55f, 0.90f, 0.90f };
// How they move: standing still in the room, they start following once the
// look has moved more than kPiecesLetGo off them (or the head has moved
// kPiecesLetGoDistance nearer or further); following, they glide towards the
// place along the look, kPiecesFollowRate of the way each second, round the
// head; they stand still again once the look has turned slower than
// kPiecesStillSpeed for kPiecesStillMs and they are nearly in front.
//
// They are placed round the way the head looks, up and down as well as left
// and right, and tilted to face it. The strip's middle is kPiecesBelow under
// the line of the look; the card is kCardBigger as big to the eye as the
// panel's picture, with its hint (the "HINT" header 17.5% down the screen,
// 21% down the card, and its lines under it: kCardTextDown of the card from
// its top) kCardTextAbove over the line of the look, so the hint sits above
// eye level as in the flat game. The look followed is kept within
// kPiecesMostPitch of level (looking straight up or down, which way the head
// faces means nothing).
// The distances below and above are given for 1.5 m away (kPiecesWere: the
// strip 0.40 m below the eyes, 15 degrees) and scaled to kPiecesDistance, so
// each piece keeps its size to the eye and its angle below or above the look.
static const float kPiecesDistance = 2.2f, kPiecesWere = 1.5f;
static const float kPiecesBelow = 0.40f * kPiecesDistance / kPiecesWere;
// The card is 0.9 times as big to the eye as the panel's picture.
static const float kCardBigger = 0.9f, kCardTextAbove = 0.35f * kPiecesDistance / kPiecesWere, kCardTextDown = 0.30f;
// Never deeper than the room in front: writing that floats beyond a wall a
// meter away is hard on the eyes. The body script says every update how far
// the view is clear (cVR_InterfaceRoom: along the look, where the strip is
// below it and to each side); the pieces go kPiecesWallGap in front of what
// it met, no nearer than kPiecesNearest, at most kPiecesDistance. Wherever they are,
// each is as big to the eye and as far below or above the look (in degrees)
// as at kPiecesDistance: the distance they were put at (g_piecesAt) sets
// their size. Standing still in the room, they let go of their place
// kPiecesTooDeep behind the room (or kPiecesLetGoDistance in front of it).
static const float kPiecesNearest = 0.9f, kPiecesWallGap = 0.15f, kPiecesTooDeep = 0.1f;
static const float kPiecesLetGo = 0.17f, kPiecesLetGoDistance = 0.3f; // (10 degrees)
static const float kPiecesMostPitch = 1.05f; // radians (60 degrees)
static const float kPiecesFollowRate = 5.0f;
static const float kPiecesStillSpeed = 0.26f; // radians a second (15 degrees)
static const double kPiecesStillMs = 300.0;

static bool g_piecesAllowed = false;          // the body script's word: in play
static double g_piecesAllowedMs = -100000.0;  // ...when last said (stale after 0.25 s)
static float g_piecesRoom = -1.0f;            // the body script's word: how far the view is clear (the game's units; -1: all the way)
static double g_piecesRoomMs = -100000.0;     // ...when last said (stale after 0.25 s)
static float g_piecesAt = kPiecesDistance;    // how far away they were put (real meters; their size and spacing go by it)
static bool g_piecesShown = false;            // were the pieces shown in the last frame (not the big panel)?
static bool g_piecesPlaced = false;
static float g_piecesBase[3] = { 0, 0, 0 };   // the point they are placed round, on the line of the look, in the room (floor space)
static float g_piecesYaw = 0.0f;              // which way they face: the way back to the head, left and right...
static float g_piecesPitch = 0.0f;            // ...and up and down
static unsigned long long g_piecesLastShownSerial = 0;
static XrPosef g_stripPose = {}, g_cardPose = {}; // as placed this frame (for the monitor too)
static float g_stripSize[2] = { 0, 0 }, g_cardSize[2] = { 0, 0 }; // meters

// The pieces' own pictures: each piece's band copied out of the interface
// picture on the game's side (GlCopyToTexture keeps its rows as the panel's
// whole picture has them, which shows upright), so that each layer shows a
// whole picture, as the big panel does. Don't show a cut-out of the panel's
// picture instead: its corner is given the OpenGL way, from the bottom, and
// the subtitles don't show (whether the headset software reads that corner
// from the top is not known; this way does not depend on it).
struct PieceSwapchain
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int width = 0, height = 0;
    uint32_t imageCount = 0;
    XrSwapchainImageOpenGLKHR images[16];
    int sizes = 1; // how many sizes each picture has: its own and its smaller copies
    bool hasPicture = false; // one handed over at least (a layer may show it)
};
static PieceSwapchain g_pieceSwapchains[2]; // 0 the strip, 1 the card

// The cutscene screen's own pictures: the game's window picture of a scene, with its fade and interface laid over it.
// Kept apart from the interface panel's pictures so the two can be shown together: paused during a scene, the screen
// holds its last picture where it stood (g_pausedScreen*), and the game's menu comes on the ordinary panel in front of
// it, at its usual size.
static PieceSwapchain g_screenPicture;
static unsigned long long g_screenSentSerial = 0; // the frame its last picture was handed over
static const float kWholePicture[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
static float g_pausedScreenPosition[3] = {}, g_pausedScreenYaw = 0.0f, g_pausedScreenWidth = 0.0f, g_pausedScreenHeight = 0.0f;
static bool g_pausedScreenKept = false; // the screen's place kept as a pause took the panel away from it
static double g_lastPausedMs = -1000000.0; // when the game was last paused (or its menu requested)

void VrSetInterfaceInPieces(bool inPlay)
{
    g_piecesAllowed = inPlay;
    g_piecesAllowedMs = NowMs();
}

void VrSetInterfaceRoom(float distance)
{
    g_piecesRoom = distance;
    g_piecesRoomMs = NowMs();
}

// How far away the pieces are to be (real meters; see kPiecesNearest).
static float PiecesWantedDistance()
{
    if (g_piecesRoom < 0.0f || NowMs() - g_piecesRoomMs > 250.0) return kPiecesDistance;
    float scale = GetSettings().worldScale > 0.01f ? GetSettings().worldScale : 1.0f;
    float wanted = g_piecesRoom / scale - kPiecesWallGap;
    return wanted < kPiecesNearest ? kPiecesNearest : (wanted > kPiecesDistance ? kPiecesDistance : wanted);
}

// The storage box's book (see VrSetStorageBoxAt): the script's last word on
// where it stands, and when.
static float g_storageAngle = 0.0f, g_storageDistance = 0.0f, g_storageHeight = 0.0f;
static double g_storageMs = -100000.0;
// It is kept at least this near and at most this far (real meters), at most
// this far below the eyes and no higher than them; kStorageBigger times as
// big to the eye as the inventory's book.
static const float kStorageNearest = 1.0f, kStorageFurthest = 1.6f, kStorageLowest = 0.5f;
static const float kStorageBigger = 1.35f;

void VrSetStorageBoxAt(float angle, float distance, float height)
{
    g_storageAngle = angle;
    g_storageDistance = distance;
    g_storageHeight = height;
    g_storageMs = NowMs();
}

// Is the panel to be put at the storage box this frame: it has just been
// shown again (or a snap is requested)? (Call before PlacePanelAtBox, which
// notes that it is shown.)
static bool PanelAtBoxAppears()
{
    return !g_panelPlaced || g_panelSnapRequested || g_frameSerialAtBegin - g_panelLastShownSerial > 45;
}

// The panel standing at the storage box: put there as it is shown (or on a
// snap request), facing the head; then held still, as the box does not move.
// `distance` and `height` (real meters) from the head; `sideways` as in
// UpdatePanelPlacement.
static void PlacePanelAtBox(float angle, float distance, float height, float sideways)
{
    bool appeared = PanelAtBoxAppears();
    g_panelLastShownSerial = g_frameSerialAtBegin;
    if (!appeared) return;

    const XrQuaternionf& h = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;
    float headYaw = atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));
    float yaw = headYaw + angle;
    g_panelPosition[0] = p.x - sinf(yaw) * distance + cosf(yaw) * sideways;
    g_panelPosition[1] = p.y + height;
    g_panelPosition[2] = p.z - cosf(yaw) * distance - sinf(yaw) * sideways;
    g_panelYaw = yaw;
    g_panelPlaced = true;
    g_panelSnapRequested = false;
    g_panelMoving = false;
    static int logsLeft = 20;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("VR: the storage box's book stands at the box: %.0f degrees round from the head's facing, %.2f m off, %.2f m %s the eyes.",
            angle * 57.29578f, distance, fabsf(height), height < 0.0f ? "below" : "above");
    }
}

static bool GameMouseIsInLookMode(int* pointerX, int* pointerY);

// Is the interface to be shown in pieces this frame (see above)?
static bool InterfaceInPiecesNow(bool theatre)
{
    bool inPieces = !theatre && g_piecesAllowed && NowMs() - g_piecesAllowedMs < 250.0 &&
                    GameMouseIsInLookMode(nullptr, nullptr) && !CameraGamePaused() && !CameraInventoryOpen() &&
                    !CameraLoadingScreen();
    static int last = -1;
    static int logsLeft = 40;
    if ((inPieces ? 1 : 0) != last && logsLeft > 0)
    {
        --logsLeft;
        Log("VR: the game's interface %s.", inPieces ? "in pieces (subtitles below the eyes, the hint card above them)" : "on the big panel");
    }
    last = inPieces ? 1 : 0;
    return inPieces;
}

// One piece as a layer: `part` of the interface picture (in its own
// pictures, `piece`), its middle at `centre` in the room, turned to `turn`
// (up and down too); the whole picture would be `pictureWidth` meters wide.
static void FillPieceLayer(XrCompositionLayerQuad& quad, const float part[4], const float centre[3], const XrQuaternionf& turn,
                           float pictureWidth, XrPosef& pose, float size[2], const PieceSwapchain& piece)
{
    pose = {};
    pose.orientation = turn;
    pose.position.x = centre[0];
    pose.position.y = centre[1];
    pose.position.z = centre[2];
    size[0] = pictureWidth * (part[2] - part[0]);
    size[1] = pictureWidth * (float)g_guiSwapchainHeight / (float)g_guiSwapchainWidth * (part[3] - part[1]);

    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = g_floorSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = piece.swapchain;
    quad.subImage.imageRect.offset.x = 0;
    quad.subImage.imageRect.offset.y = 0;
    quad.subImage.imageRect.extent.width = piece.width;
    quad.subImage.imageRect.extent.height = piece.height;
    quad.subImage.imageArrayIndex = 0;
    quad.pose = pose;
    quad.size.width = size[0];
    quad.size.height = size[1];
}

// Places the strip and the card (Half-Life: Alyx's way: still in the room,
// brought back in front when they leave the view, gliding round the head;
// placed round the look, up and down too) and fills their layers.
static void PlacePieces(XrCompositionLayerQuad& strip, XrCompositionLayerQuad& card)
{
    const XrQuaternionf& h = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;
    // Which way the head looks: left and right, and up (+) and down; the
    // look followed is kept within kPiecesMostPitch of level.
    float headYaw = atan2f(2 * (h.x * h.z + h.w * h.y), 1 - 2 * (h.x * h.x + h.y * h.y));
    float sinPitch = 2 * (h.w * h.x - h.y * h.z);
    if (sinPitch > 1.0f) sinPitch = 1.0f;
    if (sinPitch < -1.0f) sinPitch = -1.0f;
    float headPitch = asinf(sinPitch);
    float lookPitch = headPitch > kPiecesMostPitch ? kPiecesMostPitch : (headPitch < -kPiecesMostPitch ? -kPiecesMostPitch : headPitch);

    // How fast the look turns (for when they stand still), and the time
    // since the last frame.
    static float lastYaw = 0.0f, lastPitch = 0.0f;
    static double lastMs = -1.0, stillSinceMs = -1.0;
    double now = NowMs();
    float step = 0.0f, turnSpeed = 0.0f;
    if (lastMs >= 0.0 && now > lastMs)
    {
        step = (float)((now - lastMs) / 1000.0);
        float turned = headYaw - lastYaw;
        while (turned > 3.14159265f) turned -= 6.28318531f;
        while (turned < -3.14159265f) turned += 6.28318531f;
        turned *= cosf(headPitch); // (a turn left or right, looking up or down, is a smaller one of the look)
        float tilted = headPitch - lastPitch;
        turnSpeed = sqrtf(turned * turned + tilted * tilted) / step;
        if (step > 0.1f) step = 0.1f;
    }
    lastYaw = headYaw;
    lastPitch = headPitch;
    lastMs = now;
    if (turnSpeed < kPiecesStillSpeed) { if (stillSinceMs < 0.0) stillSinceMs = now; }
    else stillSinceMs = -1.0;
    bool headStill = stillSinceMs >= 0.0 && now - stillSinceMs >= kPiecesStillMs;

    // Following the look, or standing still in the room (see kPiecesLetGo).
    // (Gliding round the head as they follow, facing it all the way.) Where
    // they stand from the head: `around` left and right, `rise` up and down,
    // `distance` away.
    static bool following = false;
    bool appeared = !g_piecesPlaced || g_frameSerialAtBegin - g_piecesLastShownSerial > 45;
    g_piecesLastShownSerial = g_frameSerialAtBegin;
    float dx = g_piecesBase[0] - p.x, dy = g_piecesBase[1] - p.y, dz = g_piecesBase[2] - p.z;
    float flatDistance = sqrtf(dx * dx + dz * dz);
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    float around = atan2f(-dx, -dz);
    float rise = atan2f(dy, flatDistance);
    float angle = around - headYaw; // how far off the look, left and right...
    while (angle > 3.14159265f) angle -= 6.28318531f;
    while (angle < -3.14159265f) angle += 6.28318531f;
    float tilt = rise - lookPitch;  // ...and up and down
    float off = sqrtf(angle * cosf(lookPitch) * angle * cosf(lookPitch) + tilt * tilt);
    // How far away they are to be: kPiecesDistance, or nearer where the room
    // in front is less (see kPiecesNearest). Logged as that starts and ends.
    float wanted = PiecesWantedDistance();
    {
        static bool walled = false;
        static int logsLeft = 20;
        bool walledNow = wanted < kPiecesDistance - 0.01f;
        if (walledNow != walled && logsLeft > 0)
        {
            --logsLeft;
            if (walledNow) Log("VR: the interface's pieces come nearer: something stands in front (they go %.2f m away).", wanted);
            else Log("VR: the interface's pieces may go back to %.1f m away (nothing stands in front).", kPiecesDistance);
        }
        walled = walledNow;
    }
    if (appeared)
    {
        // Newly shown: straight along the look at once.
        around = headYaw;
        rise = lookPitch;
        distance = wanted;
        g_piecesAt = wanted;
        following = false;
        g_piecesPlaced = true;
    }
    else
    {
        static int logsLeft = 20;
        float deeper = wanted < kPiecesDistance - 0.01f ? kPiecesTooDeep : kPiecesLetGoDistance;
        if (!following && (off > kPiecesLetGo || distance - wanted > deeper || wanted - distance > kPiecesLetGoDistance))
        {
            following = true;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("VR: the interface's pieces follow the look (they were %.0f degrees off it, %.2f m away, to be %.2f m).", off * 57.2958f,
                    distance, wanted);
            }
        }
        if (following)
        {
            float share = 1.0f - expf(-kPiecesFollowRate * step);
            around -= angle * share;
            rise -= tilt * share;
            distance += (wanted - distance) * share;
            // (Their size goes the same way as smoothly: no jump as they let
            // go of a place the head had come nearer to or gone from.)
            g_piecesAt += (wanted - g_piecesAt) * share;
            if (headStill && off < 0.035f && fabsf(distance - wanted) < 0.05f)
            {
                following = false;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: the interface's pieces stand still in the room (the head is still).");
                }
            }
        }
    }
    if (appeared || following)
    {
        g_piecesBase[0] = p.x - sinf(around) * cosf(rise) * distance;
        g_piecesBase[1] = p.y + sinf(rise) * distance;
        g_piecesBase[2] = p.z - cosf(around) * cosf(rise) * distance;
        g_piecesYaw = around; // facing the head all the way
        g_piecesPitch = rise;
    }

    // Turned to face the head: left and right, then tilted up or down (the
    // turn about the upright line, then about the piece's own across line).
    XrQuaternionf turn;
    float cy = cosf(g_piecesYaw * 0.5f), sy = sinf(g_piecesYaw * 0.5f);
    float cx = cosf(g_piecesPitch * 0.5f), sx = sinf(g_piecesPitch * 0.5f);
    turn.w = cy * cx;
    turn.x = cy * sx;
    turn.y = sy * cx;
    turn.z = -sy * sx;
    // The pieces' own "up", in the room.
    float up[3] = { sinf(g_piecesPitch) * sinf(g_piecesYaw), cosf(g_piecesPitch), sinf(g_piecesPitch) * cosf(g_piecesYaw) };

    // The strip as big to the eye as on the big panel (its width at its
    // distance), its middle kPiecesBelow under the line of the look; the
    // card kCardBigger as big, with its hint kCardTextAbove over that line;
    // all at kPiecesDistance, and scaled with g_piecesAt.
    const Settings& settings = GetSettings();
    float nearer = g_piecesAt / kPiecesDistance;
    float pictureWidth = settings.hudWidth * g_piecesAt / (settings.hudDistance > 0.1f ? settings.hudDistance : 0.1f);
    float below = kPiecesBelow * nearer;
    float stripCentre[3] = { g_piecesBase[0] - up[0] * below, g_piecesBase[1] - up[1] * below, g_piecesBase[2] - up[2] * below };
    FillPieceLayer(strip, kStripPart, stripCentre, turn, pictureWidth, g_stripPose, g_stripSize, g_pieceSwapchains[0]);
    float cardWidth = pictureWidth * kCardBigger;
    float cardHeight = cardWidth * (float)g_guiSwapchainHeight / (float)g_guiSwapchainWidth * (kCardPart[3] - kCardPart[1]);
    float cardRaise = kCardTextAbove * nearer + (kCardTextDown - 0.5f) * cardHeight; // (its middle, from its hint's place)
    float cardCentre[3] = { g_piecesBase[0] + up[0] * cardRaise, g_piecesBase[1] + up[1] * cardRaise, g_piecesBase[2] + up[2] * cardRaise };
    FillPieceLayer(card, kCardPart, cardCentre, turn, cardWidth, g_cardPose, g_cardSize, g_pieceSwapchains[1]);
}

// Picks the pixel format for shared pictures. Returns 0 if none is usable.
static int64_t ChooseSwapchainFormat()
{
    int64_t formats[64];
    uint32_t formatCount = 0;
    if (!Check(xrEnumerateSwapchainFormats(g_session, 64, &formatCount, formats), "Listing the headset's picture formats"))
        return 0;

    for (uint32_t i = 0; i < formatCount; ++i)
        if (formats[i] == kGlSrgb8Alpha8) return formats[i];
    for (uint32_t i = 0; i < formatCount; ++i)
        if (formats[i] == kGlRgba8) return formats[i];
    return 0;
}

// Makes sure the shared pictures for the interface panel exist at this size.
static bool EnsureGuiSwapchain(int width, int height)
{
    if (g_guiSwapchain != XR_NULL_HANDLE && width == g_guiSwapchainWidth && height == g_guiSwapchainHeight)
        return true;

    if (g_guiSwapchain != XR_NULL_HANDLE) XrDestroySwapchainGl(g_guiSwapchain);
    g_guiSwapchain = XR_NULL_HANDLE;
    g_guiSwapchainImageCount = 0;
    g_guiLastSentSerial = 0;
    g_guiPanelCopiedSerial = 0;

    int64_t format = ChooseSwapchainFormat();
    if (format == 0) return false;

    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!Check(XrCreateSwapchainGl(g_session, &info, &g_guiSwapchain), "Creating the interface panel pictures"))
    {
        g_guiSwapchain = XR_NULL_HANDLE;
        return false;
    }

    for (int i = 0; i < 16; ++i)
    {
        g_guiSwapchainImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        g_guiSwapchainImages[i].next = nullptr;
        g_guiSwapchainImages[i].image = 0;
    }
    if (!Check(XrEnumerateSwapchainImagesGl(g_guiSwapchain, 16, &g_guiSwapchainImageCount,
                                          (XrSwapchainImageBaseHeader*)g_guiSwapchainImages),
               "Getting the interface panel pictures"))
    {
        XrDestroySwapchainGl(g_guiSwapchain);
        g_guiSwapchain = XR_NULL_HANDLE;
        return false;
    }

    g_guiSwapchainWidth = width;
    g_guiSwapchainHeight = height;
    Log("VR: created the interface panel pictures: %u of %d x %d", (unsigned)g_guiSwapchainImageCount, width, height);
    return true;
}

// A part of the captured interface (left, bottom, right, top) into a panel's
// texture, stretched to fill `width` x `height`. It is captured before the
// engine's image correction, which the window's interface gets with the rest
// of the frame (the player's gamma: CameraPictureCorrection), so it gets it on
// the way. Its colours are multiplied by how solid they are. The window's own
// picture (framebuffer 0, the cutscene screen's) has had the correction
// already, and its alpha means nothing: it is copied as it is.
static void CopyGuiPicture(GLuint guiFramebuffer, int left, int bottom, int right, int top, GLuint texture, int width, int height)
{
    float gamma = 1.0f, brightness = 1.0f, contrast = 1.0f;
    if (guiFramebuffer != 0 && CameraPictureCorrection(gamma, brightness, contrast) &&
        GlCopyToTextureCorrected(guiFramebuffer, left, bottom, right, top, texture, width, height, gamma, brightness, contrast, true))
        return;
    GlCopyToTexture(guiFramebuffer, left, bottom, right, top, texture, width, height);
}

// Copies the captured interface into the next shared panel picture.
static bool CopyGuiToHeadset(GLuint guiFramebuffer, int width, int height)
{
    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(g_guiSwapchain, nullptr, &index), "Taking the next panel picture"))
        return false;

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(g_guiSwapchain, &waitInfo), "Waiting for the panel picture");

    if (ready && index < g_guiSwapchainImageCount)
        CopyGuiPicture(guiFramebuffer, 0, 0, width, height, g_guiSwapchainImages[index].image, width, height);

    return Check(XrReleaseSwapchainImageGl(g_guiSwapchain, nullptr), "Handing the panel picture back") && ready;
}

// The pieces' own pictures (see PieceSwapchain).
static bool EnsurePieceSwapchain(PieceSwapchain& piece, int width, int height, const char* what)
{
    if (piece.swapchain != XR_NULL_HANDLE && width == piece.width && height == piece.height) return true;
    if (piece.swapchain != XR_NULL_HANDLE) XrDestroySwapchainGl(piece.swapchain);
    piece.swapchain = XR_NULL_HANDLE;
    piece.imageCount = 0;
    piece.hasPicture = false;
    if (width <= 0 || height <= 0) return false;

    int64_t format = ChooseSwapchainFormat();
    if (format == 0) return false;
    XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    // With its smaller copies (mipmaps): the headset draws the pieces at
    // about a third of their pictures' size, and without them thin writing
    // shimmers as the head moves. If the headset software refuses them, the
    // pictures are made without. (Not on SteamVR: there the mod draws the
    // pieces into the eyes itself, from their full size; see MakeOwnSet.)
    int sizes = 1;
    if (!g_panelsInEyes)
        for (int longest = width > height ? width : height; longest > 1; longest /= 2) ++sizes;
    info.mipCount = (uint32_t)sizes;
    XrResult created = XrCreateSwapchainGl(g_session, &info, &piece.swapchain);
    if (!XR_SUCCEEDED(created))
    {
        Log("VR: the pictures of the interface's %s could not have smaller copies (code %d); made without them.", what, (int)created);
        piece.swapchain = XR_NULL_HANDLE;
        sizes = 1;
        info.mipCount = 1;
        if (!Check(XrCreateSwapchainGl(g_session, &info, &piece.swapchain), "Creating a piece of the interface's pictures"))
        {
            piece.swapchain = XR_NULL_HANDLE;
            return false;
        }
    }
    for (int i = 0; i < 16; ++i)
    {
        piece.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        piece.images[i].next = nullptr;
        piece.images[i].image = 0;
    }
    if (!Check(XrEnumerateSwapchainImagesGl(piece.swapchain, 16, &piece.imageCount, (XrSwapchainImageBaseHeader*)piece.images),
               "Getting a piece of the interface's pictures"))
    {
        XrDestroySwapchainGl(piece.swapchain);
        piece.swapchain = XR_NULL_HANDLE;
        return false;
    }
    piece.width = width;
    piece.height = height;
    piece.sizes = sizes;
    Log("VR: created the pictures of the interface's %s: %u of %d x %d (%d size%s)", what, (unsigned)piece.imageCount, width, height,
        sizes, sizes == 1 ? "" : "s");
    return true;
}

// Copies `part` (left, bottom, right, top, as fractions from the picture's
// bottom-left corner) of the captured interface into the piece's next
// picture, then makes its smaller copies.
static bool CopyPieceToHeadset(PieceSwapchain& piece, const float part[4], GLuint guiFramebuffer, int guiWidth, int guiHeight,
                               const char* what)
{
    int left = (int)(part[0] * guiWidth + 0.5f), bottom = (int)(part[1] * guiHeight + 0.5f);
    int right = (int)(part[2] * guiWidth + 0.5f), top = (int)(part[3] * guiHeight + 0.5f);
    if (!EnsurePieceSwapchain(piece, right - left, top - bottom, what)) return false;

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(piece.swapchain, nullptr, &index), "Taking the next piece picture")) return false;
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(piece.swapchain, &waitInfo), "Waiting for the piece picture");
    if (ready && index < piece.imageCount)
    {
        CopyGuiPicture(guiFramebuffer, left, bottom, right, top, piece.images[index].image, piece.width, piece.height);
        if (piece.sizes > 1) GlMakeSmallerCopies(piece.images[index].image);
    }
    bool released = Check(XrReleaseSwapchainImageGl(piece.swapchain, nullptr), "Handing the piece picture back");
    if (released && ready) piece.hasPicture = true;
    return released && ready;
}

// The turn `q` as 9 numbers, row after row (a direction is turned by
// multiplying it by this).
static void TurnToMatrix(const XrQuaternionf& q, float m[9])
{
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[1] = 2 * (xy - wz);     m[2] = 2 * (xz + wy);
    m[3] = 2 * (xy + wz);     m[4] = 1 - 2 * (xx + zz); m[5] = 2 * (yz - wx);
    m[6] = 2 * (xz - wy);     m[7] = 2 * (yz + wx);     m[8] = 1 - 2 * (xx + yy);
}

// The steady view for the monitor (for recording videos of play): a camera at
// the left eye that follows where the eye looks, but slowly (an easing with
// `followSeconds` as its time), and always level (no tilt of the head). It is
// worked out in the headset's room, where the eye's picture was taken: turning
// with the stick turns the world in the picture itself, so it shows on the
// monitor straight away.
// Kept within `yawRoom` and `pitchRoom` (radians) of where the eye looks, so
// that the view never runs off the edge of the picture. Returns its turn.
static XrQuaternionf SteadyMonitorView(const XrQuaternionf& eye, float followSeconds, float yawRoom, float pitchRoom)
{
    static bool known = false;
    static float yaw = 0.0f, pitch = 0.0f;
    static double lastMs = 0.0;

    // Where the eye looks: the heading (also when looking almost straight
    // down or up, from the top of the head) and how far up or down.
    float m[9];
    TurnToMatrix(eye, m);
    float forward[3] = { -m[2], -m[5], -m[8] }; // the eye's -z
    float up[3] = { m[1], m[4], m[7] };         // the eye's +y
    // (The top of the head counts only when looking more than some 50
    // degrees up or down: otherwise a tilted head would shift the heading.)
    float steep = (fabsf(forward[1]) - 0.8f) / 0.2f;
    if (steep < 0.0f) steep = 0.0f;
    if (steep > 1.0f) steep = 1.0f;
    float headingX = forward[0] - forward[1] * steep * up[0];
    float headingZ = forward[2] - forward[1] * steep * up[2];
    float wantedYaw = atan2f(-headingX, -headingZ);
    float wantedPitch = asinf(forward[1] < -1.0f ? -1.0f : (forward[1] > 1.0f ? 1.0f : forward[1]));

    double nowMs = NowMs();
    float seconds = known ? (float)((nowMs - lastMs) / 1000.0) : 0.0f;
    lastMs = nowMs;
    if (seconds < 0.0f) seconds = 0.0f;
    if (seconds > 0.1f) seconds = 0.1f;
    if (!known)
    {
        yaw = wantedYaw;
        pitch = wantedPitch;
        known = true;
    }

    const float pi = 3.14159265f;
    auto wrap = [pi](float angle) {
        while (angle > pi) angle -= 2 * pi;
        while (angle < -pi) angle += 2 * pi;
        return angle;
    };
    float share = followSeconds > 0.001f ? 1.0f - expf(-seconds / followSeconds) : 1.0f;
    float yawBehind = wrap(wantedYaw - yaw) * (1.0f - share);
    float pitchBehind = (wantedPitch - pitch) * (1.0f - share);
    if (yawBehind > yawRoom) yawBehind = yawRoom;
    if (yawBehind < -yawRoom) yawBehind = -yawRoom;
    if (pitchBehind > pitchRoom) pitchBehind = pitchRoom;
    if (pitchBehind < -pitchRoom) pitchBehind = -pitchRoom;
    yaw = wrap(wantedYaw - yawBehind);
    pitch = wantedPitch - pitchBehind;
    const float pitchLimit = 1.53f; // (88 degrees)
    if (pitch > pitchLimit) pitch = pitchLimit;
    if (pitch < -pitchLimit) pitch = -pitchLimit;

    // Turned by the heading about the room's up, after tipping up or down
    // about its own sideways axis.
    float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
    float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
    XrQuaternionf view;
    view.x = cy * sp;
    view.y = cp * sy;
    view.z = -sy * sp;
    view.w = cy * cp;
    return view;
}

// ---------------------------------------------------------------------------
// The dedication: the studio's card, "Mina STUDIO" with Mina in the Mongolian
// script beside it (dedication.cpp), at the very start of the game, before any
// of its menus, in the middle of the view. The name fades in first, STUDIO and
// the mark after it. The eyes are kept dark behind it and the main menu's room
// fades in after it (DedicationDark); the main menu's room showing doesn't end
// it, only another world does (kDedicationOtherWorldMs). A card that cannot be
// made is left out. The game is usually at its menu long before the headset is
// put on, so it waits for that (DedicationHeadsetOn): once the headset is on
// and has come to rest, the view stays black for a moment, the words fade in
// straight ahead at eye level, 2 m away, held still in the room (not stuck to
// the head; if the head turns well away they glide back in front, like the
// game's panel), stay a few seconds and fade out, and only then does the
// game's panel (its menu) appear. Until then the game's panel is held back
// and the controllers do not reach the game (g_dedicationHolding). The monitor
// shows the same: black, with the words in the middle (ComposeMonitorPicture).
// Once a run of the game. Skipped if the game's world is shown first.
// ---------------------------------------------------------------------------

static XrSwapchain g_dedicationSwapchain = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLKHR g_dedicationImages[16];
static uint32_t g_dedicationImageCount = 0;
static int g_dedicationWidth = 0, g_dedicationHeight = 0;
static unsigned char g_dedicationFaded[1024 * 256 * 4]; // the picture with the fade in it
static int g_dedicationStage = 0;          // 0 = waiting for the headset to be put on, 1 = being shown, 2 = over
static float g_dedicationSeconds = 0.0f;   // time into the showing
static float g_dedicationFade = 0.0f;      // how solid it is now, 0 to 1
static float g_dedicationSentFade = -1.0f; // how solid the headset's picture of it is
static XrPosef g_dedicationPose = {};
static float g_dedicationCentre[3] = { 0, 0, 0 }; // where it is in the room (its middle)
static float g_dedicationYaw = 0.0f;              // which way it faces, radians
static bool g_dedicationGliding = false;          // gliding back in front of the head?
static GLuint g_monitorDedicationTexture = 0, g_monitorDedicationFramebuffer = 0; // the faded picture, for the monitor
static int g_monitorDedicationWidth = 0, g_monitorDedicationHeight = 0;
static double g_dedicationLastMs = 0.0;
static float g_dedicationFadeRest = 0.0f;  // how solid STUDIO and the mark are now (they come after the name)
static float g_dedicationSentRest = -1.0f; // ...and in the headset's picture
static double g_dedicationOverMs = -1.0;   // when it ran its course (the room behind fades in after it), else below 0
static double g_dedicationRoomFromMs = -1.0; // when the room behind it began fading in (after the game's own start), else below 0
static double g_dedicationOtherSinceMs = -1.0; // since when a world other than the main menu's room has been shown (else below 0)
static bool g_dedicationTest = false;      // shown again for a test run's "card" command (DedicationAgainForTest)
static const float kDedicationWidthMetres = 0.6f; // (the studio's name and its mark fill 0.57 of it, some 34 cm across; the card is half as tall as wide, 30 cm)
static const float kDedicationDistance = 2.0f;    // meters in front
static const float kDedicationBlack = 0.6f;       // seconds of black first,
static const float kDedicationIn = 1.5f;          // the name fading in,
static const float kDedicationRestAfter = 0.6f;   // STUDIO and the mark starting to fade in this long after it,
static const float kDedicationStay = 3.4f;        // all fully there,
static const float kDedicationOut = 1.5f;         // all fading out together,
static const float kDedicationAfter = 0.4f;       // and black again before the game's menu
static const float kDedicationRoomIn = 1.5f;      // then the main menu's room fades in from black
static const double kDedicationOtherWorldMs = 1000.0; // a world other than the main menu's room shown this long lets it go

// Has the headset been put on? Measured from where it was and which way it
// faced once its tracking had settled (2 s after it was first tracked: a
// headset's tracking can jump a little as it starts): it has been "moved"
// once it is 4 cm from there or faces 4 degrees away, or the view has been
// centered by hand, or a controller squeezed while it waits; and it is "on"
// once, after that, it has stayed within 3 cm and 6 degrees for 1.2 seconds.
// (A headset lying on the desk does not move at all; one picked up and put on
// moves a long way; one worn from the start, by a player sitting still, sways
// only a few degrees, perhaps 6 degrees in 30 s. So "moved" needs only a small
// change: 25 cm or 12 degrees would never take such a headset as on.)
struct DedicationWatch
{
    bool have = false, moved = false;
    XrVector3f first = {}, firstForward = {}, rest = {}, restForward = {};
    double trackedSinceMs = -1.0, restSinceMs = 0.0;
    char why[96] = "";
};
static DedicationWatch g_dedicationWatch;
static const double kDedicationSettleMs = 2000.0;

static bool DedicationHeadsetOn(double nowMs)
{
    DedicationWatch& w = g_dedicationWatch;
    if (!g_framePoseValid)
    {
        w.trackedSinceMs = -1.0;
        w.restSinceMs = nowMs; // a fresh rest is needed once it is tracked again
        return false;
    }
    const XrQuaternionf& q = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;
    XrVector3f forward; // the way the head faces (its own -z)
    forward.x = -2.0f * (q.x * q.z + q.w * q.y);
    forward.y = -2.0f * (q.y * q.z - q.w * q.x);
    forward.z = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    if (w.trackedSinceMs < 0.0) w.trackedSinceMs = nowMs;
    if (!w.have)
    {
        w.rest = p;
        w.restForward = forward;
        w.restSinceMs = nowMs;
        if (nowMs - w.trackedSinceMs < kDedicationSettleMs && !g_dedicationSqueezed) return false;
        w.first = p;
        w.firstForward = forward;
        w.have = true;
        return false;
    }
    float rx = p.x - w.rest.x, ry = p.y - w.rest.y, rz = p.z - w.rest.z;
    float restDot = forward.x * w.restForward.x + forward.y * w.restForward.y + forward.z * w.restForward.z;
    if (sqrtf(rx * rx + ry * ry + rz * rz) > 0.03f || restDot < 0.9945f) // (cos 6 degrees)
    {
        w.rest = p;
        w.restForward = forward;
        w.restSinceMs = nowMs;
    }
    if (!w.moved)
    {
        float fx = p.x - w.first.x, fy = p.y - w.first.y, fz = p.z - w.first.z;
        float away = sqrtf(fx * fx + fy * fy + fz * fz);
        float firstDot = forward.x * w.firstForward.x + forward.y * w.firstForward.y + forward.z * w.firstForward.z;
        float turned = acosf(fmaxf(-1.0f, fminf(1.0f, firstDot))) * 57.29578f;
        if (away > 0.04f) snprintf(w.why, sizeof(w.why), "it moved %.0f cm", away * 100.0f);
        else if (turned > 4.0f) snprintf(w.why, sizeof(w.why), "it turned %.0f degrees", turned);
        else if (g_recentreCount > 1) snprintf(w.why, sizeof(w.why), "the view was centred");
        else if (g_dedicationSqueezed) snprintf(w.why, sizeof(w.why), "a controller was squeezed");
        w.moved = w.why[0] != '\0';
        if (w.moved)
        {
            // (Its rest is counted from here: the card is placed once the head
            // has come to rest after the move, not partway through it, which
            // would leave it off to one side.)
            w.rest = p;
            w.restForward = forward;
            w.restSinceMs = nowMs;
        }
    }
    return w.moved && nowMs - w.restSinceMs >= 1200.0;
}

// How dark the eyes are for the studio's card (DrawFade): black while it waits
// and while it is shown, so that the main menu's room (drawn by then, more
// often than not) is not seen around it; after it, still black while the
// game's own start plays its logos (on black, as the game has them), until the
// menu script says the main menu is all in view (cVR_MenuInView: the logos
// over, its title up), and then the room fades in from black (eased). (Letting
// the room in as the card ends would show it for a moment, then go black again
// for the logos.) At most 60 s after the card, should the script never say so.
// Nothing if it was left out or let go.
static float DedicationDark()
{
    if (g_dedicationStage != 2) return 1.0f;
    if (g_dedicationOverMs < 0.0) return 0.0f;
    double now = NowMs();
    if (g_dedicationRoomFromMs < 0.0)
    {
        if (g_menuInViewMs <= g_dedicationOverMs && now - g_dedicationOverMs < 60000.0) return 1.0f;
        g_dedicationRoomFromMs = now;
        Log("VR: the room behind the studio's card fades in: %s.", g_menuInViewMs > g_dedicationOverMs ? "the main menu is all in view, the game's own start over"
                                                                                                   : "the menu script never said the main menu was in view (60 s after the card)");
    }
    float t = (float)((now - g_dedicationRoomFromMs) / 1000.0) / kDedicationRoomIn;
    if (t >= 1.0f) return 0.0f;
    if (t < 0.0f) t = 0.0f;
    float shown = t * t * (3.0f - 2.0f * t);
    return 1.0f - shown;
}

bool VrStudioCardHolds()
{
    return g_dedicationHolding;
}

// For a test run's "card" command: the card as at an ordinary start, from
// waiting for the headset to be put on (a test run with the stand-in runtime,
// g_standIn, otherwise leaves it out).
static void DedicationAgainForTest()
{
    g_dedicationTest = true;
    g_dedicationStage = 0;
    g_dedicationSeconds = 0.0f;
    g_dedicationFade = 0.0f;
    g_dedicationFadeRest = 0.0f;
    g_dedicationOverMs = -1.0;
    g_dedicationRoomFromMs = -1.0;
    g_dedicationOtherSinceMs = -1.0;
    g_dedicationSqueezed = false;
    g_dedicationWatch = DedicationWatch();
    Log("PRETEND HEADSET: the studio's card from the start: it waits for the headset to be put on.");
}

// Puts the dedication straight in front of the head, at eye level, upright,
// facing the head (`glide` false: at once). With `glide` it stays where it is
// while the head looks about it, and only if the head turns more than 25
// degrees away from it, or moves 40 cm nearer or further or 30 cm up or down,
// does it glide back in front (as the game's panel does, UpdatePanelPlacement).
// (It ignores which way the head looks up or down: placed while the head
// happened to look up for a moment, it would hang well above eye level.)
static void PlaceDedication(bool glide)
{
    const XrQuaternionf& q = g_frameHeadPose.orientation;
    const XrVector3f& p = g_frameHeadPose.position;
    float headYaw = atan2f(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y));
    float target[3] = { p.x - sinf(headYaw) * kDedicationDistance, p.y, p.z - cosf(headYaw) * kDedicationDistance };

    if (!glide)
    {
        for (int i = 0; i < 3; ++i) g_dedicationCentre[i] = target[i];
        g_dedicationYaw = headYaw;
        g_dedicationGliding = false;
    }
    else
    {
        float dx = g_dedicationCentre[0] - p.x, dz = g_dedicationCentre[2] - p.z;
        float flatDistance = sqrtf(dx * dx + dz * dz);
        float angle = atan2f(-dx, -dz) - headYaw;
        while (angle > 3.14159265f) angle -= 6.28318531f;
        while (angle < -3.14159265f) angle += 6.28318531f;
        float height = g_dedicationCentre[1] - p.y;
        if (fabsf(angle) > 0.44f || fabsf(flatDistance - kDedicationDistance) > 0.4f || fabsf(height) > 0.3f)
            g_dedicationGliding = true;
        if (g_dedicationGliding)
        {
            const float step = 0.07f; // the share of the remaining way covered each frame
            for (int i = 0; i < 3; ++i) g_dedicationCentre[i] += (target[i] - g_dedicationCentre[i]) * step;
            float turn = headYaw - g_dedicationYaw;
            while (turn > 3.14159265f) turn -= 6.28318531f;
            while (turn < -3.14159265f) turn += 6.28318531f;
            g_dedicationYaw += turn * step;
            if (fabsf(angle) < 0.05f && fabsf(flatDistance - kDedicationDistance) < 0.05f && fabsf(height) < 0.05f)
                g_dedicationGliding = false;
        }
    }

    g_dedicationPose.position.x = g_dedicationCentre[0];
    g_dedicationPose.position.y = g_dedicationCentre[1];
    g_dedicationPose.position.z = g_dedicationCentre[2];
    g_dedicationPose.orientation.x = 0.0f; // turned about the upright axis only
    g_dedicationPose.orientation.y = sinf(g_dedicationYaw * 0.5f);
    g_dedicationPose.orientation.z = 0.0f;
    g_dedicationPose.orientation.w = cosf(g_dedicationYaw * 0.5f);
}

// Was the game started for one of the mod's test maps? Each test map's desktop
// shortcut (start_grab_test.bat and the like) leaves a note next to the game's
// exe and then has Steam start the game as usual. If the note is there and new
// (written in the last 3 minutes), this start is for that test map: the note
// is removed (so that the next start is an ordinary one), the dedication is
// not shown (it is for playing the game, not for testing), and the game's main
// menu opens the test map's mod at once (it asks: VR option 90; see
// VRGrabTestLaunch in MenuHandler.hps). The mod's folder named on the command
// line, as when the game starts itself again for a mod, counts too. Don't have
// the shortcut pass the mod to the game ("-mod mods/BunkerVR_GrabTest/entry.hpc"):
// the game stops straight after starting. Worked out once, on the first call.
//   grab test:   "BunkerVR Grab Test" (start_grab_test.bat), BunkerVR_start_grab_test.txt, mods\BunkerVR_GrabTest
//   item test:   "BunkerVR Item Test" (start_item_test.bat), BunkerVR_start_item_test.txt, mods\BunkerVR_ItemTest
//   drawer test: "BunkerVR Drawer Test" (start_drawer_test.bat), BunkerVR_start_drawer_test.txt, mods\BunkerVR_DrawerTest
//   sight test:  "BunkerVR Sight Test" (start_sight_test.bat), BunkerVR_start_sight_test.txt, mods\BunkerVR_SightTest
// 0: an ordinary start; 1: the grab test map; 2: the item test map; 3: the
// drawer test map; 4: the sight test map.
static SRWLOCK g_grabTestLock = SRWLOCK_INIT;

// Was this shortcut's note there and new (written in the last 3 minutes)? It
// is removed either way. `seen` says whether there was one at all.
static bool TestMapNoteIsNew(const wchar_t* noteName, bool& seen)
{
    seen = false;
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    if (length == 0 || length + 40 >= MAX_PATH) return false;
    lstrcatW(path, noteName);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    seen = true;
    FILETIME nowFile;
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER written, now;
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    now.LowPart = nowFile.dwLowDateTime;
    now.HighPart = nowFile.dwHighDateTime;
    double ageSeconds = now.QuadPart > written.QuadPart ? (double)(now.QuadPart - written.QuadPart) / 1.0e7 : 0.0;
    DeleteFileW(path);
    return ageSeconds < 180.0;
}

static int StartedForTestMap()
{
    static int answer = -1;
    AcquireSRWLockExclusive(&g_grabTestLock);
    if (answer < 0)
    {
        answer = 0;
        const char* why = "no";

        // The shortcuts' notes (all looked at, so that none is left behind). (Never in a tester build,
        // kPlayerBuild: the test maps are not in it, and nothing opens one, whatever note or command line is there.)
        bool grabSeen = false, itemSeen = false, drawerSeen = false, sightSeen = false;
        bool grabNew = !kPlayerBuild && TestMapNoteIsNew(L"BunkerVR_start_grab_test.txt", grabSeen);
        bool itemNew = !kPlayerBuild && TestMapNoteIsNew(L"BunkerVR_start_item_test.txt", itemSeen);
        bool drawerNew = !kPlayerBuild && TestMapNoteIsNew(L"BunkerVR_start_drawer_test.txt", drawerSeen);
        bool sightNew = !kPlayerBuild && TestMapNoteIsNew(L"BunkerVR_start_sight_test.txt", sightSeen);
        if (sightNew)
        {
            answer = 4;
            why = "the sight test map, from its shortcut's note (no dedication; the menu opens it)";
        }
        else if (drawerNew)
        {
            answer = 3;
            why = "the drawer test map, from its shortcut's note (no dedication; the menu opens it)";
        }
        else if (itemNew)
        {
            answer = 2;
            why = "the item test map, from its shortcut's note (no dedication; the menu opens it)";
        }
        else if (grabNew)
        {
            answer = 1;
            why = "the grab test map, from its shortcut's note (no dedication; the menu opens it)";
        }
        else if (grabSeen || itemSeen || drawerSeen || sightSeen)
        {
            why = "no (an old note from a shortcut was found and removed)";
        }

        // The mod's folder on the command line.
        const wchar_t* line = GetCommandLineW();
        if (answer == 0 && line && !kPlayerBuild)
        {
            // The command line in small letters (up to 4095 of them), then looked through.
            static wchar_t lower[4096];
            size_t n = 0;
            for (; line[n] && n < 4095; ++n)
                lower[n] = (line[n] >= L'A' && line[n] <= L'Z') ? (wchar_t)(line[n] - L'A' + L'a') : line[n];
            lower[n] = 0;
            if (wcsstr(lower, L"bunkervr_grabtest"))
            {
                answer = 1;
                why = "the grab test map, named on the command line (no dedication)";
            }
            else if (wcsstr(lower, L"bunkervr_itemtest"))
            {
                answer = 2;
                why = "the item test map, named on the command line (no dedication)";
            }
            else if (wcsstr(lower, L"bunkervr_drawertest"))
            {
                answer = 3;
                why = "the drawer test map, named on the command line (no dedication)";
            }
            else if (wcsstr(lower, L"bunkervr_sighttest"))
            {
                answer = 4;
                why = "the sight test map, named on the command line (no dedication)";
            }
        }
        if (kPlayerBuild) why = "no (the player build opens no test map)";
        Log("VR: started for a test map: %s.", why);
    }
    int result = answer;
    ReleaseSRWLockExclusive(&g_grabTestLock);
    return result;
}

// How solid a part of the card is: its fading in (`in`, 0 to 1 over its
// time) or the card's fading out (`out`, 1 to 0), whichever is less, eased at
// both ends.
static float DedicationEase(float in, float out)
{
    float v = in < out ? in : out;
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v * v * (3.0f - 2.0f * v);
}

// Once a headset frame, at its end, before the game's panel is added.
// `otherWorld`: the eyes show a world other than the main menu's room this
// frame, or the cutscene screen is up. The main menu's room is drawn behind
// the card, darkened to black by DedicationDark, and doesn't end it: the room
// comes some 25 s after the game starts, often before the headset is put on
// or while the card waits. Sets g_dedicationHolding. Returns true if the card
// is to be shown this frame (at g_dedicationPose, the name g_dedicationFade
// solid, STUDIO and the mark g_dedicationFadeRest).
static bool UpdateDedication(bool otherWorld)
{
    double nowMs = NowMs();
    float seconds = g_dedicationLastMs > 0.0 ? (float)((nowMs - g_dedicationLastMs) / 1000.0) : 0.0f;
    g_dedicationLastMs = nowMs;
    if (seconds < 0.0f) seconds = 0.0f;
    if (seconds > 0.1f) seconds = 0.1f; // (a frame that stood still holds it up rather than skipping on)
    if (!g_frameShouldShow) seconds = 0.0f; // (nothing is shown in the headset: it waits)

    // (Another world for a second lets it go: not a frame or two of one as
    // the game changes over, before the menu's script has said its room.)
    if (otherWorld) { if (g_dedicationOtherSinceMs < 0.0) g_dedicationOtherSinceMs = nowMs; }
    else g_dedicationOtherSinceMs = -1.0;
    bool otherWorldShown = g_dedicationOtherSinceMs >= 0.0 && nowMs - g_dedicationOtherSinceMs >= kDedicationOtherWorldMs;

    if (g_dedicationStage == 0)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("VR: the studio's card waits for the headset to be put on (the game's menu is held back, the eyes kept dark, until it has been shown).");
        }
        int width = 0, height = 0;
        const unsigned char* nameLayer = nullptr;
        const unsigned char* restLayer = nullptr;
        if ((StartedForTestMap() != 0 || g_standIn) && !g_dedicationTest)
        {
            g_dedicationStage = 2; // (testing: straight on; see StartedForTestMap)
        }
        else if (otherWorldShown)
        {
            g_dedicationStage = 2;
            Log("VR: a world other than the main menu's room was shown before the headset was put on: no studio card this time.");
        }
        else if (!DedicationLayers(width, height, nameLayer, restLayer)) // (made now, while nobody is looking)
        {
            g_dedicationStage = 2; // (an empty card would only hold the menu back)
            Log("VR: the studio's card could not be made: it is left out.");
        }
        else if (g_frameShouldShow && DedicationHeadsetOn(nowMs))
        {
            PlaceDedication(false); // straight ahead, at eye level
            g_dedicationStage = 1;
            g_dedicationSeconds = 0.0f;
            const XrQuaternionf& q = g_frameHeadPose.orientation;
            float lookUp = asinf(fmaxf(-1.0f, fminf(1.0f, -2.0f * (q.y * q.z - q.w * q.x)))); // (for the log)
            Log("VR: the headset is on (%s): the studio's card is shown, at eye level straight ahead (the head looking %+.0f degrees up).",
                g_dedicationWatch.why, lookUp * 57.29578f);
        }
    }

    if (g_dedicationStage == 1)
    {
        if (g_framePoseValid) PlaceDedication(true);
        g_dedicationSeconds += seconds;
        // The name fades in, STUDIO and the mark a moment after it, all stay,
        // then all fade out together.
        float t = g_dedicationSeconds - kDedicationBlack; // time since the name began fading in
        const float outStart = kDedicationRestAfter + kDedicationIn + kDedicationStay;
        float out = t <= outStart ? 1.0f : 1.0f - (t - outStart) / kDedicationOut;
        g_dedicationFade = DedicationEase(t / kDedicationIn, out);
        g_dedicationFadeRest = DedicationEase((t - kDedicationRestAfter) / kDedicationIn, out);
        if (g_dedicationSeconds >= kDedicationBlack + outStart + kDedicationOut + kDedicationAfter)
        {
            g_dedicationStage = 2;
            g_dedicationFade = 0.0f;
            g_dedicationFadeRest = 0.0f;
            g_dedicationOverMs = nowMs; // (the room behind fades in now: DedicationDark)
            Log("VR: the studio's card has been shown: the game's menu comes now, the room fading in.");
        }
        else if (otherWorldShown)
        {
            g_dedicationStage = 2; // (the game went on by itself: it is let through at once)
            Log("VR: a world other than the main menu's room was shown during the studio's card: it is cut short.");
        }
    }
    else if (g_dedicationStage == 2 && (g_dedicationFade > 0.0f || g_dedicationFadeRest > 0.0f))
    {
        float step = seconds / 0.5f; // (only when cut short)
        g_dedicationFade = g_dedicationFade > step ? g_dedicationFade - step : 0.0f;
        g_dedicationFadeRest = g_dedicationFadeRest > step ? g_dedicationFadeRest - step : 0.0f;
    }

    g_dedicationHolding = g_dedicationStage != 2;
    return g_dedicationStage != 0 && (g_dedicationFade > 0.0f || g_dedicationFadeRest > 0.0f);
}

// Hands the headset the dedication's picture with the present fade in it,
// when that has changed. Call with OpenGL active. Returns false if it cannot
// be shown.
static bool SendDedicationPicture()
{
    int width = 0, height = 0;
    const unsigned char* nameLayer = nullptr;
    const unsigned char* restLayer = nullptr;
    if (!DedicationLayers(width, height, nameLayer, restLayer) || width * height * 4 > (int)sizeof(g_dedicationFaded)) return false;

    if (g_dedicationSwapchain == XR_NULL_HANDLE)
    {
        int64_t format = ChooseSwapchainFormat();
        if (format == 0) return false;
        XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = (uint32_t)width;
        info.height = (uint32_t)height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(XrCreateSwapchainGl(g_session, &info, &g_dedicationSwapchain), "Creating the dedication's pictures"))
        {
            g_dedicationSwapchain = XR_NULL_HANDLE;
            return false;
        }
        for (int i = 0; i < 16; ++i)
        {
            g_dedicationImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            g_dedicationImages[i].next = nullptr;
            g_dedicationImages[i].image = 0;
        }
        if (!Check(XrEnumerateSwapchainImagesGl(g_dedicationSwapchain, 16, &g_dedicationImageCount,
                                              (XrSwapchainImageBaseHeader*)g_dedicationImages),
                   "Getting the dedication's pictures"))
        {
            XrDestroySwapchainGl(g_dedicationSwapchain);
            g_dedicationSwapchain = XR_NULL_HANDLE;
            return false;
        }
        g_dedicationWidth = width;
        g_dedicationHeight = height;
        g_dedicationSentFade = -1.0f;
        g_dedicationSentRest = -1.0f;
    }

    // A new picture whenever either fade has moved on by a step of 256 (at
    // most once a headset frame). Coarser steps, such as 0.02 (some 50 a
    // fade), show as visible steps in the dark.
    int nameScale = (int)(g_dedicationFade * 256.0f + 0.5f);
    int restScale = (int)(g_dedicationFadeRest * 256.0f + 0.5f);
    if (g_dedicationSentFade >= 0.0f && (int)g_dedicationSentFade == nameScale && (int)g_dedicationSentRest == restScale) return true;

    // (The colors are already multiplied by how solid they are, so all four
    // numbers of each pixel are faded alike; the two layers do not overlap, so
    // they are added.)
    int bytes = width * height * 4;
    for (int i = 0; i < bytes; ++i)
    {
        int value = (nameLayer[i] * nameScale + restLayer[i] * restScale) >> 8;
        g_dedicationFaded[i] = (unsigned char)(value > 255 ? 255 : value);
    }
    KeepForMonitor(g_dedicationFaded, width, height, g_monitorDedicationTexture, g_monitorDedicationFramebuffer,
                   g_monitorDedicationWidth, g_monitorDedicationHeight); // (the same, for the monitor)

    uint32_t index = 0;
    if (!Check(XrAcquireSwapchainImageGl(g_dedicationSwapchain, nullptr, &index), "Taking the next dedication picture"))
        return false;
    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    bool ready = Check(XrWaitSwapchainImageGl(g_dedicationSwapchain, &waitInfo), "Waiting for the dedication picture");
    if (ready && index < g_dedicationImageCount)
        GlUploadPicture(g_dedicationImages[index].image, width, height, g_dedicationFaded);
    if (Check(XrReleaseSwapchainImageGl(g_dedicationSwapchain, nullptr), "Handing the dedication picture back") && ready)
    {
        g_dedicationSentFade = (float)nameScale;
        g_dedicationSentRest = (float)restScale;
    }
    return g_dedicationSentFade >= 0.0f;
}

// Rebuilds the picture shown in the game's window on the monitor.
//
// Left alone, the window would show the headset's raw material: a view
// squashed sideways that flickers between the two eyes, with no interface.
// Instead it shows the latest left-eye picture, cut to the window's shape so
// nothing is squashed, with the interface laid back on top.
static void ComposeMonitorPicture(bool newLeftEyePicture, bool haveGui, GLuint guiTexture, bool drawPanels = true)
{
    int width = 0, height = 0;
    if (!GlGetWindowSize(width, height)) return;

    if (g_monitorTexture == 0 || width != g_monitorWidth || height != g_monitorHeight)
    {
        GlDeleteTarget(g_monitorTexture, g_monitorFramebuffer);
        g_monitorHasPicture = false;
        if (!GlCreateTarget(width, height, g_monitorTexture, g_monitorFramebuffer)) return;
        g_monitorWidth = width;
        g_monitorHeight = height;
    }

    // Keep a copy of each new left-eye picture (the window currently holds it).
    if (newLeftEyePicture)
    {
        const int whole[4] = { 0, 0, width, height };
        CopyGamePicture(whole, g_monitorFramebuffer, width, height);
        g_monitorHasPicture = true;
    }

    // The dedication at the start (see UpdateDedication): the window shows
    // what the headset does, black with the words in the middle as they fade
    // in and out, and not the game's menu yet. The words take the share of the
    // window's width that they take of a view as wide as the "monitor width"
    // setting (70 degrees: some 270 pixels of 1920), as in the headset with
    // the steady view on.
    if (g_dedicationHolding)
    {
        const float black[3] = { 0.0f, 0.0f, 0.0f };
        GlDrawSolid(0, width, height, black, 1.0f);
        if ((g_dedicationFade > 0.0f || g_dedicationFadeRest > 0.0f) && g_monitorDedicationTexture != 0 && g_monitorDedicationWidth > 0)
        {
            float viewDegrees = GetSettings().monitorWidth;
            if (viewDegrees < 30.0f) viewDegrees = 30.0f;
            if (viewDegrees > 120.0f) viewDegrees = 120.0f;
            float share = (kDedicationWidthMetres * 0.5f / kDedicationDistance) / tanf(viewDegrees * 0.5f * 3.14159265f / 180.0f);
            int shownWidth = (int)(width * share + 0.5f);
            int shownHeight = shownWidth * g_monitorDedicationHeight / g_monitorDedicationWidth;
            GlDrawOverRect(g_monitorDedicationTexture, 0, (width - shownWidth) / 2, (height - shownHeight) / 2, shownWidth,
                           shownHeight);
        }
        return;
    }

    // While the game's loading screen is up, the window shows it as a flat
    // game would: the loading screen over the whole window, on black. (The
    // headset's view is black or stands still while the game loads, with the
    // loading screen on its panel somewhere in it, which would look poor on
    // the monitor.)
    if (CameraLoadingScreen() && haveGui)
    {
        const float black[3] = { 0.0f, 0.0f, 0.0f };
        GlDrawSolid(0, width, height, black, 1.0f);
        GlDrawOver(guiTexture, 0, width, height);
        return;
    }

    // Where the left eye was, and how it was turned, when its picture was
    // taken (in the headset's room).
    static XrPosef monitorEye = {};
    static bool haveMonitorEye = false;
    if (newLeftEyePicture && g_framePoseValid)
    {
        monitorEye = g_frameEyePose;
        haveMonitorEye = true;
    }
    // The way the window looks: the eye's own turn, or the steady view's.
    XrQuaternionf windowTurn = monitorEye.orientation;

    float tanHalfWidth = 0, tanHalfHeight = 0;
    float shownHalfWidth = 0, shownHalfHeight = 0; // how much of the view the window shows (tangents)
    const Settings& settings = GetSettings();
    bool steady = settings.monitorSmooth > 0.001f && haveMonitorEye;
    if (steady && g_monitorHasPicture && CameraGetFieldOfView(tanHalfWidth, tanHalfHeight))
    {
        // The steady view (see SteadyMonitorView): as wide as the setting, with
        // the window's shape, but never wider than the picture itself.
        const float degree = 3.14159265f / 180.0f;
        float viewHalfWidth = tanf(settings.monitorWidth * 0.5f * degree);
        if (viewHalfWidth > tanHalfWidth * 0.95f) viewHalfWidth = tanHalfWidth * 0.95f;
        float viewHalfHeight = viewHalfWidth * height / width;
        if (viewHalfHeight > tanHalfHeight * 0.95f)
        {
            viewHalfHeight = tanHalfHeight * 0.95f;
            viewHalfWidth = viewHalfHeight * width / height;
        }
        // How far the view may turn away from where the eye looks before it
        // would run off the picture (less two and a half degrees: the eye's
        // picture is tilted with the head, and the view's corners reach
        // further out then).
        float yawRoom = atanf(tanHalfWidth) - atanf(viewHalfWidth) - 2.5f * degree;
        float pitchRoom = atanf(tanHalfHeight) - atanf(viewHalfHeight) - 2.5f * degree;
        if (yawRoom < 0.0f) yawRoom = 0.0f;
        if (pitchRoom < 0.0f) pitchRoom = 0.0f;
        windowTurn = SteadyMonitorView(monitorEye.orientation, settings.monitorSmooth, yawRoom, pitchRoom);

        // From the steady view's way of looking to the picture's: the eye's
        // turn undone after the view's.
        float eyeTurn[9], viewTurn[9], fromView[9];
        TurnToMatrix(monitorEye.orientation, eyeTurn);
        TurnToMatrix(windowTurn, viewTurn);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                fromView[row * 3 + column] = eyeTurn[0 * 3 + row] * viewTurn[0 * 3 + column] +
                                             eyeTurn[1 * 3 + row] * viewTurn[1 * 3 + column] +
                                             eyeTurn[2 * 3 + row] * viewTurn[2 * 3 + column];
        const float pictureHalf[2] = { tanHalfWidth, tanHalfHeight };
        const float viewHalf[2] = { viewHalfWidth, viewHalfHeight };
        if (GlDrawTurnedView(g_monitorTexture, 0, width, height, fromView, pictureHalf, viewHalf))
        {
            shownHalfWidth = viewHalfWidth;
            shownHalfHeight = viewHalfHeight;
        }
        else
        {
            steady = false; // (could not be drawn: the plain way below)
            windowTurn = monitorEye.orientation;
        }
    }
    else
    {
        steady = false;
    }
    if (!steady && g_monitorHasPicture && CameraGetFieldOfView(tanHalfWidth, tanHalfHeight))
    {
        // The game's picture covers a view that is taller than it is wide,
        // squeezed into a wide window. Pick the largest part of it that has
        // the window's shape, and stretch that part over the whole window.
        int left = 0, bottom = 0, right = width, top = height;
        float neededHalfHeight = tanHalfWidth * height / width;
        shownHalfWidth = tanHalfWidth;
        shownHalfHeight = neededHalfHeight;
        if (neededHalfHeight <= tanHalfHeight)
        {
            bottom = (int)(height * (tanHalfHeight - neededHalfHeight) / (2 * tanHalfHeight) + 0.5f);
            top = height - bottom;
        }
        else
        {
            float neededHalfWidth = tanHalfHeight * width / height;
            left = (int)(width * (tanHalfWidth - neededHalfWidth) / (2 * tanHalfWidth) + 0.5f);
            right = width - left;
            shownHalfWidth = neededHalfWidth;
            shownHalfHeight = tanHalfHeight;
        }
        if (right - left >= 16 && top - bottom >= 16)
            GlCopy(g_monitorFramebuffer, left, bottom, right, top, 0, 0, 0, width, height);
    }

    // The panels are drawn into the picture where they stand in the room, in
    // perspective, seen from where the left eye was when its picture was
    // taken: the game's interface panel, the BunkerVR menu and the bullet
    // counter, as the headset shows them, so the monitor shows what the
    // player sees. They stay put in the room, so they move across the window
    // as the head turns, exactly as the room behind them does. (Placed from
    // where the head was a moment later than the picture, they would slide
    // about and swell.) With the steady view they are seen the way it looks
    // (windowTurn).
    bool canPlace = drawPanels && shownHalfWidth > 0.01f && shownHalfHeight > 0.01f && haveMonitorEye;
    // Turns `v` by the turn `q`.
    auto turn = [](const XrQuaternionf& q, const float v[3], float out[3]) {
        float t[3] = { 2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0]) };
        out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
        out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
        out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
    };
    // A panel `panelWidth` x `panelHeight` meters, its middle at `pose`
    // (it lies across the pose's own x and y, facing along its z).
    // (`part`: only that part of the picture, for the interface's pieces; null, all of it.)
    auto drawPanel = [&](GLuint texture, const XrPosef& pose, float panelWidth, float panelHeight, const float* part) {
        if (texture == 0 || !canPlace) return;
        const float nearLimit = 0.05f, farLimit = 100.0f;
        const float xAxis[3] = { 1, 0, 0 }, yAxis[3] = { 0, 1, 0 };
        float across[3], up[3];
        turn(pose.orientation, xAxis, across);
        turn(pose.orientation, yAxis, up);
        // The window's turn (the eye's, or the steady view's) undone: the
        // conjugate of its quaternion.
        XrQuaternionf undo = windowTurn;
        undo.x = -undo.x;
        undo.y = -undo.y;
        undo.z = -undo.z;
        float corners[16];
        int behind = 0;
        for (int corner = 0; corner < 4; ++corner) // bottom left, bottom right, top left, top right
        {
            float sx = (corner & 1) ? 0.5f : -0.5f, sy = (corner & 2) ? 0.5f : -0.5f;
            float p[3] = { pose.position.x + across[0] * panelWidth * sx + up[0] * panelHeight * sy - monitorEye.position.x,
                           pose.position.y + across[1] * panelWidth * sx + up[1] * panelHeight * sy - monitorEye.position.y,
                           pose.position.z + across[2] * panelWidth * sx + up[2] * panelHeight * sy - monitorEye.position.z };
            float seen[3];
            turn(undo, p, seen); // as the eye sees it: x right, y up, looking along -z
            corners[corner * 4 + 0] = seen[0] / shownHalfWidth;
            corners[corner * 4 + 1] = seen[1] / shownHalfHeight;
            corners[corner * 4 + 2] = -seen[2] * (farLimit + nearLimit) / (farLimit - nearLimit) - 2.0f * farLimit * nearLimit / (farLimit - nearLimit);
            corners[corner * 4 + 3] = -seen[2];
            if (-seen[2] < nearLimit) ++behind;
        }
        if (behind == 4) return; // all of it behind the eye
        if (part != nullptr) GlDrawPanelPart(texture, 0, width, height, corners, part);
        else GlDrawPanel(texture, 0, width, height, corners);
    };
    auto uprightPose = [](const float centre[3], float yaw) {
        XrPosef pose = {};
        pose.orientation.y = sinf(yaw * 0.5f);
        pose.orientation.w = cosf(yaw * 0.5f);
        pose.position.x = centre[0];
        pose.position.y = centre[1];
        pose.position.z = centre[2];
        return pose;
    };

    // Not while the start-up page is up: the headset hides the game's panel
    // then, and the monitor would fall back to laying it over the whole view,
    // so the game's menu would fill it behind the page.
    if (haveGui && !StartupRunning())
    {
        if (g_piecesShown && canPlace)
        {
            // The interface in pieces, as the headset shows them.
            drawPanel(guiTexture, g_stripPose, g_stripSize[0], g_stripSize[1], kStripPart);
            drawPanel(guiTexture, g_cardPose, g_cardSize[0], g_cardSize[1], kCardPart);
        }
        else if (g_panelShown && canPlace)
            drawPanel(guiTexture, uprightPose(g_panelPosition, g_panelYaw), g_panelWidth, g_panelHeight, nullptr);
        else
            GlDrawOver(guiTexture, 0, width, height);
    }
    if (canPlace && VrMenuIsOpen() && g_menuPlaced && g_monitorMenuTexture != 0)
        drawPanel(g_monitorMenuTexture, uprightPose(g_menuPosition, g_menuYaw), kMenuWidthMetres, g_menuHeightMetres, nullptr);
    if (canPlace && g_counterShownNow && g_monitorCounterTexture != 0 && g_monitorCounterWidth > 0)
        drawPanel(g_monitorCounterTexture, g_counterPose, g_counterWidthMetres,
                  g_counterWidthMetres * (float)g_monitorCounterHeight / (float)g_monitorCounterWidth, nullptr);
    // The hints' labels at the hands, as the headset was given them.
    for (int hand = 0; canPlace && hand < 2; ++hand)
    {
        const HintLabel& label = g_hintLabels[hand];
        if (label.layeredNow && label.monitorTexture != 0 && label.monitorWidth > 0)
            drawPanel(label.monitorTexture, label.pose, HintLabelWidth(),
                      HintLabelWidth() * (float)label.monitorHeight / (float)label.monitorWidth, nullptr);
    }
    // The pointer in menus, as the headset was given it.
    for (int i = 0; canPlace && i < g_pointerPieceCount; ++i)
    {
        const PointerPiece& piece = g_pointerPieces[i];
        drawPanel(g_pointerPictures[piece.kind].monitorTexture, piece.pose, piece.width, piece.height, nullptr);
    }
}

// ---------------------------------------------------------------------------
// SteamVR: the panels drawn into the eyes' pictures
// ---------------------------------------------------------------------------
// SteamVR's OpenXR runtime (vrclient_x64.dll, CSxrCompositorOpenVR::
// ComposeLayerQuad) shows each quad layer in an overlay of its own,
// "openxr_quadlayer_<n>", chosen by the layer's place in that frame's list,
// not by its set of pictures. On OpenGL it copies each picture into a picture
// shared with its compositor that keeps the size of the first picture that
// overlay was given ("Scene create OpenGL", never resized: its D3D and Vulkan
// paths have a "Scene resize", OpenGL none), the copy as big as that shared
// picture. So as panels come and go, the n-th overlay is handed another
// panel's picture: the game's panel (1920 x 1080) in an overlay first given
// the dedication's 384 x 96 shows a corner of it; the pointer's 64 x 64 in one
// first given the menu's 840 x 744 cannot be copied at all (GL_INVALID_VALUE,
// 0x501, on which SteamVR then fails new sets of pictures) and the menu stays
// on show in its place; the start-up page, lost that way, leaves the room
// dimmed behind it (StartupDim). So on SteamVR no panels go to it: every frame
// the mod takes a fresh picture for each eye, puts the eye's latest picture of
// the world in it (or black), and draws every panel over it where it stands in
// the room, seen from that eye, in the order of the frame's layers, as a
// headset's own compositor does. SteamVR gets the eyes' pictures alone, as
// from any other game. (Other runtimes, such as Oculus, keep the quad layers:
// they show the panels sharper, from their own pictures.)

static bool LocateEyes(XrTime time, XrView eyes[2]);

// Turns `v` by the turn `q`.
static void TurnByQuaternion(const XrQuaternionf& q, const float v[3], float out[3])
{
    float t[3] = { 2 * (q.y * v[2] - q.z * v[1]), 2 * (q.z * v[0] - q.x * v[2]), 2 * (q.x * v[1] - q.y * v[0]) };
    out[0] = v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]);
    out[1] = v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]);
    out[2] = v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0]);
}

// Draws one panel into an eye's picture `target` (width x height), seen from
// the eye at `eye` through `fov` (the panel and the eye both in the room).
static bool DrawQuadIntoEye(const XrCompositionLayerQuad& quad, const XrPosef& eye, const XrFovf& fov, GLuint target,
                            int width, int height)
{
    const KnownSet* known = FindKnownSet(quad.subImage.swapchain);
    if (!known || known->released < 0 || (uint32_t)known->released >= known->count || known->width <= 0 || known->height <= 0)
        return false; // (nothing handed back in it yet)
    GLuint picture = known->pictures[known->released];

    // The part of its picture the panel shows (from the bottom, as OpenGL counts).
    const XrRect2Di& rect = quad.subImage.imageRect;
    const float part[4] = { (float)rect.offset.x / (float)known->width, (float)rect.offset.y / (float)known->height,
                            (float)(rect.offset.x + rect.extent.width) / (float)known->width,
                            (float)(rect.offset.y + rect.extent.height) / (float)known->height };

    // The eye's view is lopsided (it sees further outwards): its edges as tangents.
    const float tanLeft = tanf(fov.angleLeft), tanRight = tanf(fov.angleRight);
    const float tanDown = tanf(fov.angleDown), tanUp = tanf(fov.angleUp);
    if (tanRight - tanLeft < 0.0001f || tanUp - tanDown < 0.0001f) return false;

    const float nearLimit = 0.05f, farLimit = 100.0f;
    const float xAxis[3] = { 1, 0, 0 }, yAxis[3] = { 0, 1, 0 };
    float across[3], up[3];
    TurnByQuaternion(quad.pose.orientation, xAxis, across);
    TurnByQuaternion(quad.pose.orientation, yAxis, up);
    XrQuaternionf undo = eye.orientation; // the eye's turn undone
    undo.x = -undo.x;
    undo.y = -undo.y;
    undo.z = -undo.z;
    float corners[16];
    int behind = 0;
    for (int corner = 0; corner < 4; ++corner) // bottom left, bottom right, top left, top right
    {
        float sx = (corner & 1) ? 0.5f : -0.5f, sy = (corner & 2) ? 0.5f : -0.5f;
        float p[3] = { quad.pose.position.x + across[0] * quad.size.width * sx + up[0] * quad.size.height * sy - eye.position.x,
                       quad.pose.position.y + across[1] * quad.size.width * sx + up[1] * quad.size.height * sy - eye.position.y,
                       quad.pose.position.z + across[2] * quad.size.width * sx + up[2] * quad.size.height * sy - eye.position.z };
        float seen[3];
        TurnByQuaternion(undo, p, seen); // as the eye sees it: x right, y up, looking along -z
        // OpenGL "clip" positions: x / w runs from -1 at the view's left edge (tangent tanLeft) to +1 at its right.
        corners[corner * 4 + 0] = (2.0f * seen[0] + (tanRight + tanLeft) * seen[2]) / (tanRight - tanLeft);
        corners[corner * 4 + 1] = (2.0f * seen[1] + (tanUp + tanDown) * seen[2]) / (tanUp - tanDown);
        corners[corner * 4 + 2] = -seen[2] * (farLimit + nearLimit) / (farLimit - nearLimit) - 2.0f * farLimit * nearLimit / (farLimit - nearLimit);
        corners[corner * 4 + 3] = -seen[2];
        if (-seen[2] < nearLimit) ++behind;
    }
    if (behind == 4) return false; // all of it behind the eye

    // (The mod's panels have their colors already multiplied by how solid they are, OpenXR's own way.)
    bool blend = (quad.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
    return GlDrawPanelIntoTexture(target, width, height, picture, corners, part, blend);
}

// Takes the frame's panels (its quad layers) out of `layers` and draws them
// into fresh pictures for the eyes; `layers` is left with the eyes' pictures
// alone (`projection`, its views in `views`), and their count is returned: 1,
// or 0 for a black frame. `eyesShown`: the eyes have pictures of the world to
// show (see EndFrameWork).
static uint32_t PanelsIntoEyes(const XrCompositionLayerBaseHeader** layers, uint32_t layerCount, bool eyesShown,
                               XrCompositionLayerProjection& projection, XrCompositionLayerProjectionView views[2])
{
    const XrCompositionLayerQuad* quads[16];
    uint32_t quadCount = 0;
    for (uint32_t i = 0; i < layerCount; ++i)
        if (layers[i] && layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quadCount < 16)
            quads[quadCount++] = (const XrCompositionLayerQuad*)layers[i];
    if (!eyesShown && quadCount == 0) return 0; // (nothing to show: black, as on any headset)

    // The eyes' pictures of the world: from where they were taken when both came
    // this game frame; otherwise (an earlier pair kept up, as while the game is
    // paused with its menu) seen again from where the eyes are now, so that the
    // panels, placed for the head as it is now, are not cut off at the edges of
    // the old pictures.
    bool fresh = eyesShown && g_eyeKeepSerial[0] == g_frameSerialAtBegin && g_eyeKeepSerial[1] == g_frameSerialAtBegin;
    XrView now[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    bool nowKnown = !fresh && LocateEyes(g_frameDisplayTime, now);
    if (!eyesShown && !nowKnown) return 0; // (where the eyes are is not known: black)

    int width = 0, height = 0;
    EyePictureSizeForPanels(width, height);
    if (!EnsureSwapchains(width, height)) return 0;
    bool keepUsable = eyesShown && g_eyeKeepTexture[0] != 0 && g_eyeKeepTexture[1] != 0 &&
                      g_eyeKeepWidth == g_swapchainWidth && g_eyeKeepHeight == g_swapchainHeight;
    bool asTaken = fresh || !nowKnown; // (the eyes where their pictures were taken)

    for (int eye = 0; eye < 2; ++eye)
    {
        XrPosef pose = asTaken ? g_eyes[eye].pose : now[eye].pose;
        XrFovf fov = asTaken ? g_eyes[eye].fov : now[eye].fov;

        uint32_t index = 0;
        if (!Check(XrAcquireSwapchainImageGl(g_swapchains[eye], nullptr, &index), "Taking the next headset picture"))
            return 0;
        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        bool ready = Check(XrWaitSwapchainImageGl(g_swapchains[eye], &waitInfo), "Waiting for the headset picture");
        if (ready && index < g_swapchainImageCounts[eye])
        {
            GLuint target = g_swapchainImages[eye][index].image;
            bool world = false;
            if (keepUsable && asTaken)
            {
                GlCopyToTexture(g_eyeKeepFramebuffer[eye], 0, 0, g_eyeKeepWidth, g_eyeKeepHeight, target, g_swapchainWidth,
                                g_swapchainHeight);
                world = true;
            }
            else if (keepUsable)
            {
                // From the eye's way of looking now to its way then: now's turn, then's undone.
                float thenTurn[9], nowTurn[9], fromNow[9];
                TurnToMatrix(g_eyes[eye].pose.orientation, thenTurn);
                TurnToMatrix(pose.orientation, nowTurn);
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        fromNow[row * 3 + column] = thenTurn[0 * 3 + row] * nowTurn[0 * 3 + column] +
                                                    thenTurn[1 * 3 + row] * nowTurn[1 * 3 + column] +
                                                    thenTurn[2 * 3 + row] * nowTurn[2 * 3 + column];
                const XrFovf& then = g_eyes[eye].fov;
                const float pictureEdges[4] = { tanf(then.angleLeft), tanf(then.angleDown), tanf(then.angleRight), tanf(then.angleUp) };
                const float viewEdges[4] = { tanf(fov.angleLeft), tanf(fov.angleDown), tanf(fov.angleRight), tanf(fov.angleUp) };
                world = GlDrawEyeSeenAgain(target, g_swapchainWidth, g_swapchainHeight, g_eyeKeepTexture[eye], fromNow,
                                           pictureEdges, viewEdges);
            }
            if (!world)
            {
                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                GlFillTexture(target, g_swapchainWidth, g_swapchainHeight, black);
            }
            for (uint32_t i = 0; i < quadCount; ++i)
                DrawQuadIntoEye(*quads[i], pose, fov, target, g_swapchainWidth, g_swapchainHeight);
        }
        bool released = Check(XrReleaseSwapchainImageGl(g_swapchains[eye], nullptr), "Handing the headset picture back");
        if (!released || !ready) return 0;

        views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        views[eye].pose = pose;
        views[eye].fov = fov;
        views[eye].subImage.swapchain = g_swapchains[eye];
        views[eye].subImage.imageRect.offset.x = 0;
        views[eye].subImage.imageRect.offset.y = 0;
        views[eye].subImage.imageRect.extent.width = g_swapchainWidth;
        views[eye].subImage.imageRect.extent.height = g_swapchainHeight;
        views[eye].subImage.imageArrayIndex = 0;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("VR: SteamVR: the eyes' pictures are %d x %d; %u panel%s drawn into them this frame (the world %s).", g_swapchainWidth,
            g_swapchainHeight, quadCount, quadCount == 1 ? "" : "s",
            !keepUsable ? "black" : (asTaken ? "as taken" : "seen again from where the eyes are now"));
    }

    projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    projection.layerFlags = 0;
    projection.space = g_floorSpace;
    projection.viewCount = 2;
    projection.views = views;
    layers[0] = (const XrCompositionLayerBaseHeader*)&projection;
    return 1;
}

// ---------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------

// The time a part of the sending takes, added to its count as it ends (see kSendPartHands).
struct SendPartTimer
{
    int part;
    double started;
    explicit SendPartTimer(int which) : part(which), started(NowMs()) {}
    ~SendPartTimer() { g_sendPartMs[part] += NowMs() - started; }
};

// The interface's picture as the window shows it: the window's own pictures
// have the game's correction (gamma, brightness, contrast) from the engine, so
// the interface laid over them (on the monitor, and on the cutscene screen)
// gets the same, in a picture of the mod's own. With no correction set, or if
// that picture cannot be made, the interface's own picture.
static GLuint g_windowGuiTexture = 0, g_windowGuiFramebuffer = 0;
static int g_windowGuiWidth = 0, g_windowGuiHeight = 0;
static bool g_windowGuiFailed = false;

static GLuint WindowInterfacePicture(GLuint guiTexture, GLuint guiFramebuffer, int width, int height)
{
    float gamma = 1.0f, brightness = 1.0f, contrast = 1.0f;
    if (g_windowGuiFailed || width <= 0 || height <= 0 || !CameraPictureCorrection(gamma, brightness, contrast)) return guiTexture;
    if (width != g_windowGuiWidth || height != g_windowGuiHeight)
    {
        GlDeleteTarget(g_windowGuiTexture, g_windowGuiFramebuffer);
        g_windowGuiWidth = g_windowGuiHeight = 0;
        if (!GlCreateTarget(width, height, g_windowGuiTexture, g_windowGuiFramebuffer))
        {
            g_windowGuiFailed = true;
            Log("VR: the interface's corrected picture for the window could not be made: the monitor shows it uncorrected.");
            return guiTexture;
        }
        g_windowGuiWidth = width;
        g_windowGuiHeight = height;
    }
    if (!GlCopyToTextureCorrected(guiFramebuffer, 0, 0, width, height, g_windowGuiTexture, width, height, gamma, brightness,
                                  contrast, true))
        return guiTexture;
    return g_windowGuiTexture;
}

// Second half of a headset frame: the game has finished drawing, so send the
// picture to the headset.
static void EndFrameWork()
{
    if (!g_frameOpen) return;
    g_frameOpen = false;
    UpdateHurtFeel(); // (the heartbeat after a hard hit)

    // Stereo in one game frame: this call comes from the middle of the game's
    // frame (see VrStereoMidFrame), to take the left eye's picture. If the
    // world was not drawn after all, nothing is taken and the headset frame
    // stays as it was, to be finished the usual way when the game's frame ends.
    if (g_stereoMidFrame && CameraLastWorldSerial() != g_frameSerialAtBegin)
    {
        g_frameOpen = true;
        return;
    }
    if (!g_stereoMidFrame) WatchSnapFrame(); // (for the log, after a snap turn)

    // The game has just drawn a picture for one eye.
    //
    // The game draws a view that is the same size on both sides of center,
    // because that is the only kind its camera settings can describe. A
    // headset lens is lopsided: each eye sees further outwards than inwards.
    // The headset software expects pictures that cover exactly the lens's
    // angles, so the matching rectangle is cut out of the game's picture.
    float tanHalfWidth = 0, tanHalfHeight = 0;
    int width = 0, height = 0;
    bool eyePictureCopied = false;
    bool leftHeld = false; // the left eye's picture was taken this frame and is waiting for the right eye's

    // While the game is loading (and in a few other moments) it draws no 3D
    // world at all, only its interface. The picture it hands over then still
    // holds whatever was drawn last, which would hang frozen in front of the
    // eyes. So when no camera view was built for this frame, the eyes are left
    // black and only the interface panel is shown.
    //
    // The exception is when the game is paused with something to read (a hint
    // that waits for a button, the pause menu): its scripts say so (see
    // CameraGamePaused), and then the last pictures of the world stay up
    // behind the panel instead of everything going black.
    bool worldDrawn = CameraLastWorldSerial() == g_frameSerialAtBegin;

    // A frame without a fresh picture of the world now and then while a level
    // is being played: the last pictures stay up for it, as when the game is
    // paused, instead of a black frame (the headset software turns them with
    // the head). Up to three frames in a row; a longer spell is taken for
    // loading and goes black. (Otherwise single black frames flash while
    // walking and looking around.) Each such frame is noted for the log.
    static int missedInRow = 0;
    bool briefMiss = false;
    if (!g_stereoMidFrame)
    {
        static unsigned long long updatesAtLastEnd = 0;
        unsigned long long updatesNow = CameraPlayerUpdates();
        int updatesThisFrame = (int)(updatesNow - updatesAtLastEnd);
        updatesAtLastEnd = updatesNow;

        if (!worldDrawn && !CameraLoadingScreen() && !CameraGamePaused() && g_framesShown > 0)
        {
            ++missedInRow;
            briefMiss = missedInRow <= 3;
            if (briefMiss)
            {
                ++g_statMissedFrames;
                if (!g_framePoseValid) ++g_statMissedNoPose;
                static int logsLeft = 20;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: no fresh picture of the world this frame while playing (%s; head pose %s; "
                        "game logic updates this frame %d, at %s rate; the view was last built for frame %llu, this is %llu): "
                        "the last pictures are shown again.",
                        g_midFailedSerial == g_frameSerialAtBegin ? "the left eye's picture could not be taken" : "at the end of the frame",
                        g_framePoseValid ? "known" : "NOT known", updatesThisFrame, g_logicHalf ? "half" : "full",
                        CameraLastWorldSerial(), g_frameSerialAtBegin);
                }
            }
        }
        else if (worldDrawn)
        {
            missedInRow = 0;
        }
    }
    bool pausedPicture = !worldDrawn && (CameraGamePaused() || briefMiss);
    if (!worldDrawn) g_holdValid = false;

    // "Black" frames only hide the eye pictures; they are kept, in case the
    // game turns out to be paused a moment later. After a longer black spell
    // (loading) they are thrown away once the world is back, so that the first
    // pictures shown are a fresh pair.
    static int blackFrames = 0;
    // A loading screen that waits for a button already has the new level
    // drawn behind it; the eyes stay black until the game says it is gone.
    bool loadingScreen = CameraLoadingScreen();
    bool blackout = (!worldDrawn && !pausedPicture) || loadingScreen;
    if (blackout)
    {
        ++blackFrames;
    }
    else
    {
        if (worldDrawn && blackFrames > 3)
        {
            Log("VR: the world is shown again after %d black frames.", blackFrames);
            g_eyes[0].hasPicture = false;
            g_eyes[1].hasPicture = false;
            if (blackFrames >= kWorldBackBlackFrames) g_worldBack = true; // (for the frame timings: VrTakeWorldBack)
        }
        if (worldDrawn) blackFrames = 0;
    }
    if (blackout)
    {
        static bool logged = false;
        if (!logged && g_framesShown > 0)
        {
            logged = true;
            Log("VR: the game drew no 3D world this frame (loading?); the eyes are shown black until it does.");
        }

    }

    // A cutscene shown on a screen: the game has drawn its picture as for a
    // monitor (see CameraTheatre). It goes on a large flat panel; the eyes get
    // no pictures of their own, so everything around the screen is black.
    // (Changed to and from behind black: UpdateTheatreChange.)
    UpdateTheatreChange();
    bool theatre = worldDrawn && CameraTheatre();
    static bool wasTheatre = false;
    if (theatre != wasTheatre)
    {
        wasTheatre = theatre;
        // (Put in front as the screen comes after the ordinary panel, or the
        // panel comes back after it: see onScreen below.)
        g_holdValid = false;
        g_eyes[0].hasPicture = false; // start from a fresh pair afterwards
        g_eyes[1].hasPicture = false;
        Log("VR: cutscene screen %s.", theatre ? "on" : "off");
    }
    // Paused during a scene: the screen holds its last picture, the game's menu on the ordinary panel.
    bool pausedNow = GamePausedOrAsked();
    if (pausedNow) g_lastPausedMs = NowMs();
    bool pausedOnScreen = theatre && pausedNow;
    static bool wasPausedOnScreen = false;
    if (pausedOnScreen != wasPausedOnScreen)
    {
        wasPausedOnScreen = pausedOnScreen;
        static int logsLeft = 20;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log(pausedOnScreen ? "VR: paused during a scene: the cutscene screen holds its last picture, the game's menu on the panel in front."
                               : "VR: the pause is over: the scene goes on on the cutscene screen, where it stood.");
        }
    }

    // The controllers are still read (buttons, the menu pointer); only the
    // drawing is left out, as there is no new picture to draw into.
    if ((!worldDrawn || theatre) && g_framePoseValid)
    {
        SendPartTimer timer(kSendPartHands);
        DrawHands(1.0f, 1.0f, 0, 0, false);
    }

    // (The game's picture's own size: the window's, unless the game's
    // "Resolution scale" makes it smaller. See GlGetPictureSize.)
    if (worldDrawn && !theatre && g_frameShouldShow && g_framePoseValid && CameraGetFieldOfView(tanHalfWidth, tanHalfHeight) &&
        GlLoadFunctions() && GlGetPictureSize(g_gamePicture, width, height))
    {
        // Add the hands to the picture before it is sent anywhere.
        {
            SendPartTimer timer(kSendPartHands);
            DrawHands(tanHalfWidth, tanHalfHeight, width, height, true);
            DrawScreenEffects(tanHalfWidth, tanHalfHeight, width, height);
            DrawComfortRing(tanHalfWidth, tanHalfHeight, width, height);
            DrawHurtEdge(tanHalfWidth, tanHalfHeight, width, height);
            DrawFade(width, height);
        }

        // Edges of the lens's view, as tangents (left and down are negative).
        float wanted[4] = { tanf(g_frameEyeFov.angleLeft), tanf(g_frameEyeFov.angleDown),
                            tanf(g_frameEyeFov.angleRight), tanf(g_frameEyeFov.angleUp) };

        // The same edges as pixel positions in the game's picture, counted
        // from its bottom-left corner. Kept inside the picture.
        int source[4];
        source[0] = (int)floorf((wanted[0] + tanHalfWidth) / (2 * tanHalfWidth) * width + 0.5f);
        source[1] = (int)floorf((wanted[1] + tanHalfHeight) / (2 * tanHalfHeight) * height + 0.5f);
        source[2] = (int)floorf((wanted[2] + tanHalfWidth) / (2 * tanHalfWidth) * width + 0.5f);
        source[3] = (int)floorf((wanted[3] + tanHalfHeight) / (2 * tanHalfHeight) * height + 0.5f);
        if (source[0] < 0) source[0] = 0;
        if (source[1] < 0) source[1] = 0;
        if (source[2] > width) source[2] = width;
        if (source[3] > height) source[3] = height;

        if (source[2] - source[0] >= 16 && source[3] - source[1] >= 16)
        {
            // The angles the cut-out really covers (identical to the lens's
            // angles unless the game's picture was too small to cover them).
            XrFovf covered;
            covered.angleLeft = atanf((float)source[0] / width * 2 * tanHalfWidth - tanHalfWidth);
            covered.angleRight = atanf((float)source[2] / width * 2 * tanHalfWidth - tanHalfWidth);
            covered.angleDown = atanf((float)source[1] / height * 2 * tanHalfHeight - tanHalfHeight);
            covered.angleUp = atanf((float)source[3] / height * 2 * tanHalfHeight - tanHalfHeight);

            // Both eyes use pictures of the same size: the size of the left eye's cut-out.
            static int cutWidth = 0, cutHeight = 0, cutForWidth = 0, cutForHeight = 0;
            if (cutWidth == 0 || cutForWidth != width || cutForHeight != height)
            {
                cutWidth = source[2] - source[0];
                cutHeight = source[3] - source[1];
                cutForWidth = width;
                cutForHeight = height;
            }

            // The eyes are drawn on alternate frames but shown as a pair: the
            // left eye's picture is held until the right eye's is done, then
            // both go to the headset together. If each were sent as soon as it
            // was drawn, the two eyes would disagree for a moment whenever the
            // game's world moves (turning with the stick, walking), which the
            // eyes read as shimmering depth.
            bool stored = false;
            int pictureWidth = cutWidth, pictureHeight = cutHeight;
            if (g_panelsInEyes) EyePictureSizeForPanels(pictureWidth, pictureHeight); // (SteamVR)
            SendPartTimer eyesTimer(kSendPartEyes);
            if (EnsureSwapchains(pictureWidth, pictureHeight))
            {
                if (g_frameEye == 0)
                {
                    stored = HoldLeftPicture(source, covered);
                    if (stored) ++g_statLeftPictures;
                    leftHeld = stored;
                }
                else if (CopyGameImageToHeadset(1, source) && ++g_statRightPictures)
                {
                    stored = true;
                    g_eyes[1].hasPicture = true;
                    g_eyes[1].pose = g_frameEyePose;
                    g_eyes[1].fov = covered;
                    SendHeldLeftPicture();
                }
            }

            if (stored)
            {
                eyePictureCopied = true;

                static int logsLeft = 2;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: %s eye: game picture %d x %d covers %.1f deg across, %.1f up and down. "
                        "Cut-out is pixels %d..%d across, %d..%d up (from the bottom), covering %.1f left, %.1f right, %.1f up, %.1f down.",
                        g_frameEye == 0 ? "left" : "right", width, height,
                        2 * ToDegrees(atanf(tanHalfWidth)), 2 * ToDegrees(atanf(tanHalfHeight)),
                        source[0], source[2], source[1], source[3],
                        -ToDegrees(covered.angleLeft), ToDegrees(covered.angleRight),
                        ToDegrees(covered.angleUp), -ToDegrees(covered.angleDown));
                }
            }
        }
    }

    // Testing aid: save pictures / measure timing if a capture was requested. It is given the eye's own
    // picture: with both eyes drawn in one game frame, the window holds only the desktop's picture, so both
    // saved eyes would be the same. The second picture of a game frame is the right eye's.
    if (g_framePoseValid && GlLoadFunctions() && GlGetPictureSize(g_gamePicture, width, height))
        CaptureOnFrameEnd(g_secondOfPair ? 1 : g_frameEye, g_frameSerialAtBegin, width, height, g_gamePicture);

    // The window's shape, for cutscenes shown on the screen.
    {
        int windowWidth = 0, windowHeight = 0;
        if (GlLoadFunctions() && GlGetWindowSize(windowWidth, windowHeight) && windowHeight > 0)
            CameraSetWindowAspect((float)windowWidth / (float)windowHeight);
    }

    // Both eyes every frame: this was the left eye's half. Nothing is sent to
    // the headset yet. The game now draws again for the right eye (see the
    // top of BeginFrameWork), with its clock held so that its world stays put,
    // and both pictures go to the headset together after that.
    if ((kPairInOneFrame || GetSettings().pairFrames || g_stereoMidFrame) && leftHeld && !g_secondOfPair)
    {
        {
            SendPartTimer timer(kSendPartMonitor);
            ComposeMonitorPicture(true, false, 0, false); // keeps a copy of the left eye's picture for the monitor
        }
        g_midPair = true;
        g_frameOpen = true; // the headset frame is still in progress
        return;
    }
    if (g_stereoMidFrame)
    {
        // Called from the middle of the game's frame, but the left eye's
        // picture could not be kept: leave the headset frame open and let the
        // frame be finished the usual way.
        g_frameOpen = true;
        return;
    }

    // Send both eyes to the headset. With the eyes taking turns, the eye that
    // was not drawn this frame keeps showing its picture from the previous
    // frame; the headset shifts it to match where the head is now.
    XrCompositionLayerProjectionView eyeViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                     { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad panel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad menuPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad counterPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad dedicationPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad stripPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD }; // the interface in pieces (PlacePieces)
    XrCompositionLayerQuad cardPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad pausedScreen = { XR_TYPE_COMPOSITION_LAYER_QUAD }; // the scene's last picture while paused
    // (Up to 16: any OpenXR runtime takes that many, XR_MIN_COMPOSITION_LAYERS_SUPPORTED. At most
    // 7 before the pointer's, the hints' two labels, and up to 6 of the pointer's: 15 at most.)
    static const uint32_t kMaxLayers = 16;
    const XrCompositionLayerBaseHeader* layers[kMaxLayers] = {};
    bool addPanelLayer = false; // (added after the dedication, so that it is drawn over it)
    bool addPieceLayers = false; // (the same)
    uint32_t layerCount = 0;

    // The game's interface, captured into its own picture this frame (if it drew any).
    GLuint guiTexture = 0, guiFramebuffer = 0;
    int guiWidth = 0, guiHeight = 0;
    bool haveGui = GlLoadFunctions() && GuiGetCaptured(g_frameSerialAtBegin, guiTexture, guiFramebuffer, guiWidth, guiHeight);
    double interfaceStarted = NowMs(); // (the interface's part of the sending, to the end of the cutscene screen's)
    if (haveGui) CaptureInterfaceOnFrameEnd(guiFramebuffer, guiWidth, guiHeight); // (after a capture: the interface's picture too)
    GLuint windowGuiTexture = haveGui ? WindowInterfacePicture(guiTexture, guiFramebuffer, guiWidth, guiHeight) : 0;
    if (theatre)
    {
        // Lay the interface (subtitles and so on) over the game's picture (as
        // the monitor shows it too), then hand the whole window picture to the
        // screen's own pictures, but not while paused: the screen keeps its
        // last picture, and the game's menu goes on the panel (below).
        if (GlLoadFunctions() && GlGetWindowSize(width, height))
        {
            // The game's own fade to black first, under the interface. The
            // engine draws its fade as a darkening of whatever is already in
            // the window, so it leaves nothing in the mod's picture of the
            // interface, and the screen would show parts of scenes the game
            // still keeps black (the wake-up in the sickroom, for one, is
            // black for its first 5.5 s, then two short glimpses, then black
            // again until about 20 s). The interface's own parts under the
            // fade were darkened in its picture already, so this is as the
            // game's window has it.
            const float black[3] = { 0.0f, 0.0f, 0.0f };
            float gameFade = CameraFadeAlpha();
            if (gameFade > 0.004f) GlDrawSolid(0, width, height, black, gameFade);
            if (haveGui) GlDrawOver(windowGuiTexture, 0, width, height);
            // (Darkened while changing to or from the screen.)
            if (g_theatreDark > 0.004f) GlDrawSolid(0, width, height, black, g_theatreDark);
            if (!pausedOnScreen && g_frameShouldShow && CopyPieceToHeadset(g_screenPicture, kWholePicture, 0, width, height, "cutscene screen"))
                g_screenSentSerial = g_frameSerialAtBegin;
        }
    }
    if ((!theatre || pausedOnScreen) && haveGui && g_frameShouldShow && EnsureGuiSwapchain(guiWidth, guiHeight))
    {
        // (Copied into the panel's own pictures further down, and only if the big panel is shown
        // this frame. In play the interface is shown in pieces, with pictures of their own, and
        // copying the whole interface to the panel every frame would cost a picture about the
        // window's size, taken from the headset software, filled and handed back, for nothing.)
        g_guiLastSentSerial = g_frameSerialAtBegin;
    }
    g_sendPartMs[kSendPartInterface] += NowMs() - interfaceStarted;

    bool panelShown = false;
    bool piecesShown = false; // (the interface in pieces instead)
    bool eyesShown = g_frameShouldShow && !blackout && !theatre && g_eyes[0].hasPicture && g_eyes[1].hasPicture;
    if (eyesShown)
    {
        for (int i = 0; i < 2; ++i)
        {
            const EyeImage& eye = g_eyes[i];
            eyeViews[i].pose = eye.pose;
            eyeViews[i].fov = eye.fov;
            eyeViews[i].subImage.swapchain = g_swapchains[i];
            eyeViews[i].subImage.imageRect.offset.x = 0;
            eyeViews[i].subImage.imageRect.offset.y = 0;
            eyeViews[i].subImage.imageRect.extent.width = g_swapchainWidth;
            eyeViews[i].subImage.imageRect.extent.height = g_swapchainHeight;
            eyeViews[i].subImage.imageArrayIndex = 0;
        }
        layer.space = g_floorSpace;
        layer.viewCount = 2;
        layer.views = eyeViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&layer;
    }

    // The dedication at the start (see UpdateDedication). While it waits for
    // the headset to be put on, and while it is shown, the game's panel is
    // held back, so that it comes first.
    bool dedicationShown = UpdateDedication((eyesShown && !MenuRoomOn()) || theatre); // (the main menu's room does not let it go)

    if (g_frameShouldShow && !g_dedicationHolding && !StartupRunning()) // (nor while the start-up page is up)
    {
        // The interface panel: a flat rectangle that stays in front of the
        // face, a fixed distance away. Shown while the interface is being
        // drawn (kept for a moment if a frame or two goes missing). A scene on
        // the cutscene screen is shown on it from the screen's own pictures.
        bool sceneOnPanel = theatre && !pausedOnScreen;
        bool panelHasPicture = sceneOnPanel ? (g_screenPicture.hasPicture && g_frameSerialAtBegin - g_screenSentSerial < 30)
                                            : (g_guiSwapchain != XR_NULL_HANDLE && g_guiLastSentSerial != 0 &&
                                               g_frameSerialAtBegin - g_guiLastSentSerial < 30);
        if (panelHasPicture)
        {
            const Settings& settings = GetSettings();
            // While the world has been faded to black for half a second (the
            // game's fade at 95% or more), the text the game writes on the
            // black (the trenches' opening words, the words after carrying
            // Lambert: the game's size 20, about a degree tall at 1.5 m) is
            // shown on the cutscene screen, at its size and in its place (see
            // kScreenWidth). The pieces can't show it: they leave out the
            // middle of the picture where those words are. It stays until the
            // fade has been under half for kBlackLetGoMs: the black can come
            // and go within half a second, and the words would jump between
            // the two sizes each time.
            static const double kBlackLetGoMs = 600.0;
            static double blackSinceMs = -1.0, clearSinceMs = -1.0;
            float gameFade = CameraFadeAlpha();
            if (gameFade >= 0.95f) { if (blackSinceMs < 0.0) blackSinceMs = NowMs(); }
            else blackSinceMs = -1.0;
            if (gameFade < 0.5f) { if (clearSinceMs < 0.0) clearSinceMs = NowMs(); }
            else clearSinceMs = -1.0;
            static bool blackForReading = false;
            bool wasBlack = blackForReading;
            if (theatre) blackForReading = false;
            else if (!blackForReading) blackForReading = blackSinceMs >= 0.0 && NowMs() - blackSinceMs >= 500.0;
            else blackForReading = !(clearSinceMs >= 0.0 && NowMs() - clearSinceMs >= kBlackLetGoMs);
            if (blackForReading != wasBlack && !theatre) // (a scene coming is logged as that)
            {
                static int logsLeft = 20;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: the world is %s: the game's words %s.", blackForReading ? "black" : "back",
                        blackForReading ? "are shown on the cutscene screen" : "go back to the panel");
                }
            }
            // The cutscene screen's size and place: the scene itself, the words
            // on black, the loading screen. Put in front as it comes
            // after the ordinary panel, and the panel as it comes back; kept in
            // its place from one of them to another.
            bool loadingOnScreen = !theatre && CameraLoadingScreen();
            bool menuRoom = !theatre && !loadingOnScreen && MenuRoomOn(); // (its fade from black is no reading matter)
            // (Never while paused: the game's menu goes on the panel, at its own size and place. Paused on the words
            // on black it would otherwise come at the screen's size, 3.4 m wide at 2.6 m against the panel's 1.8 at
            // 1.5; paused in a scene, the screen holds its picture behind the panel.)
            bool onScreen = sceneOnPanel || (blackForReading && !menuRoom && !pausedNow) || loadingOnScreen;
            static bool wasOnScreen = false;
            if (onScreen != wasOnScreen)
            {
                wasOnScreen = onScreen;
                // A pause takes the panel from the screen: the screen's place kept, to stand at during the pause
                // (holding its picture, in a scene) and to come back to after it, rather than put in front again.
                if (!onScreen && pausedNow && g_panelPlaced)
                {
                    for (int i = 0; i < 3; ++i) g_pausedScreenPosition[i] = g_panelPosition[i];
                    g_pausedScreenYaw = g_panelYaw;
                    g_pausedScreenWidth = g_panelWidth;
                    g_pausedScreenHeight = g_panelHeight;
                    g_pausedScreenKept = true;
                }
                if (onScreen && g_pausedScreenKept && NowMs() - g_lastPausedMs < 1000.0)
                {
                    for (int i = 0; i < 3; ++i) g_panelPosition[i] = g_pausedScreenPosition[i];
                    g_panelYaw = g_pausedScreenYaw;
                    g_panelPlaced = true;
                    g_panelMoving = false;
                    g_panelSnapRequested = false;
                    g_panelLastShownSerial = g_frameSerialAtBegin;
                }
                else g_panelSnapRequested = true;
                if (onScreen) g_pausedScreenKept = false;
            }
            // During play, the interface in pieces instead (PlacePieces), each
            // in its own pictures, copied when the game drew its interface this
            // frame; until both have one, the big panel.
            bool inPieces = eyesShown && !blackForReading && InterfaceInPiecesNow(theatre);
            if (inPieces && haveGui)
            {
                SendPartTimer timer(kSendPartInterface);
                CopyPieceToHeadset(g_pieceSwapchains[0], kStripPart, guiFramebuffer, guiWidth, guiHeight, "subtitle strip");
                CopyPieceToHeadset(g_pieceSwapchains[1], kCardPart, guiFramebuffer, guiWidth, guiHeight, "hint card");
            }
            inPieces = inPieces && g_pieceSwapchains[0].hasPicture && g_pieceSwapchains[1].hasPicture;
            if (inPieces)
            {
                PlacePieces(stripPanel, cardPanel);
                piecesShown = true;
                addPieceLayers = true;
            }
            // The cutscene screen is bigger and further away than the interface
            // panel (and solid for the scene itself).
            g_panelWidth = onScreen ? kScreenWidth : settings.hudWidth;
            g_panelHeight = sceneOnPanel ? g_panelWidth * (float)g_screenPicture.height / (float)g_screenPicture.width
                                         : g_panelWidth * (float)g_guiSwapchainHeight / (float)g_guiSwapchainWidth;
            float panelDistance = onScreen ? kScreenDistance : settings.hudDistance;
            // The inventory open: the book's middle in front, not the picture's
            // (the picture's left part is see-through, so the book would sit
            // off to the right). Put there at once as it opens and closes.
            bool bookShown = !theatre && CameraInventoryOpen();
            static bool wasBookShown = false;
            if (bookShown != wasBookShown)
            {
                wasBookShown = bookShown;
                g_panelSnapRequested = true;
                static int logsLeft = 20;
                if (logsLeft > 0)
                {
                    --logsLeft;
                    Log("VR: the inventory %s: the panel %s.", bookShown ? "is open" : "is shut",
                        bookShown ? "is put with the book's middle in front" : "is put with its own middle in front again");
                }
            }
            // The storage box's book at the box (see VrSetStorageBoxAt):
            // kStorageBigger times as big to the eye as the inventory's book,
            // its middle where the script says, kept within reach and below
            // the eyes; put there as the box opens.
            bool atBox = bookShown && !onScreen && NowMs() - g_storageMs < 250.0;
            static bool wasAtBox = false;
            if (atBox != wasAtBox)
            {
                wasAtBox = atBox;
                g_panelSnapRequested = true;
            }
            float boxDistance = 0.0f, boxHeight = 0.0f;
            if (atBox)
            {
                float scale = settings.worldScale > 0.01f ? settings.worldScale : 1.0f;
                boxDistance = g_storageDistance / scale;
                boxDistance = boxDistance < kStorageNearest ? kStorageNearest : (boxDistance > kStorageFurthest ? kStorageFurthest : boxDistance);
                boxHeight = g_storageHeight / scale;
                boxHeight = boxHeight < -kStorageLowest ? -kStorageLowest : (boxHeight > 0.0f ? 0.0f : boxHeight);
                // Its size set as it is put there and then kept, as its place
                // is (set from the head's distance every frame, it would change
                // size as the head moves back and forth).
                static float boxWidth = 0.0f;
                if (PanelAtBoxAppears() || boxWidth <= 0.0f)
                    boxWidth = settings.hudDistance > 0.01f ? settings.hudWidth * boxDistance / settings.hudDistance * kStorageBigger : g_panelWidth;
                g_panelWidth = boxWidth;
                g_panelHeight = g_panelWidth * (float)g_guiSwapchainHeight / (float)g_guiSwapchainWidth;
            }
            float panelSideways = bookShown ? -kInventoryBookMiddle * g_panelWidth : 0.0f;
            // (An opened photo's middle in front instead, while the scripts say so; the panel glides there and back,
            // as it follows the look, rather than jumping.)
            {
                bool focused = bookShown && NowMs() - g_panelFocusMs < 250.0;
                if (focused) panelSideways = -(g_panelFocusX - 0.5f) * g_panelWidth;
                static bool wasFocused = false;
                if (focused != wasFocused)
                {
                    wasFocused = focused;
                    if (g_panelPlaced && !onScreen) g_panelMoving = true;
                    static int logsLeft = 20;
                    if (logsLeft > 0)
                    {
                        --logsLeft;
                        if (focused) Log("VR: a photo is open in the inventory: the panel glides to put its middle in front (%.0f%% across).", g_panelFocusX * 100.0f);
                        else Log("VR: the photo is shut: the panel glides back to the book's middle.");
                    }
                }
            }
            if (onScreen) UpdateScreenPlacement(panelDistance);
            else if (menuRoom) PlaceMenuInRoom(panelDistance);
            else if (atBox) PlacePanelAtBox(g_storageAngle, boxDistance, boxHeight, panelSideways);
            else if (!inPieces) UpdatePanelPlacement(panelDistance, g_pointerActive, panelSideways);
            // The panel's own pictures brought up to date now that it is shown (see above):
            // from this frame's interface, or, when the game drew none this frame, the last one
            // captured (its picture keeps it until the next capture starts).
            if (!inPieces && !sceneOnPanel && g_guiPanelCopiedSerial != g_guiLastSentSerial)
            {
                GLuint lastTexture = 0, lastFramebuffer = 0;
                int lastWidth = 0, lastHeight = 0;
                bool have = haveGui;
                if (have)
                {
                    lastFramebuffer = guiFramebuffer;
                    lastWidth = guiWidth;
                    lastHeight = guiHeight;
                }
                else have = GuiGetLastCaptured(lastTexture, lastFramebuffer, lastWidth, lastHeight);
                double copyStarted = NowMs();
                if (have && EnsureGuiSwapchain(lastWidth, lastHeight) && CopyGuiToHeadset(lastFramebuffer, lastWidth, lastHeight))
                    g_guiPanelCopiedSerial = g_guiLastSentSerial;
                g_sendPartMs[kSendPartInterface] += NowMs() - copyStarted;
                ++g_statPanelCopies;
            }
            panelShown = !inPieces;

            panel.layerFlags = sceneOnPanel ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            panel.space = g_floorSpace;
            panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            panel.subImage.swapchain = sceneOnPanel ? g_screenPicture.swapchain : g_guiSwapchain;
            panel.subImage.imageRect.offset.x = 0;
            panel.subImage.imageRect.offset.y = 0;
            panel.subImage.imageRect.extent.width = sceneOnPanel ? g_screenPicture.width : g_guiSwapchainWidth;
            panel.subImage.imageRect.extent.height = sceneOnPanel ? g_screenPicture.height : g_guiSwapchainHeight;
            panel.subImage.imageArrayIndex = 0;
            panel.pose.orientation.x = 0.0f; // turned about the upright axis only
            panel.pose.orientation.y = sinf(g_panelYaw * 0.5f);
            panel.pose.orientation.z = 0.0f;
            panel.pose.orientation.w = cosf(g_panelYaw * 0.5f);
            panel.pose.position.x = g_panelPosition[0];
            panel.pose.position.y = g_panelPosition[1];
            panel.pose.position.z = g_panelPosition[2];
            panel.size.width = g_panelWidth;
            panel.size.height = g_panelHeight;
            addPanelLayer = !inPieces;
        }
    }

    // The dedication (drawn under the game's panel, were that ever up with it).
    if (dedicationShown && g_frameShouldShow && GlLoadFunctions() && SendDedicationPicture())
    {
        dedicationPanel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        dedicationPanel.space = g_floorSpace;
        dedicationPanel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        dedicationPanel.subImage.swapchain = g_dedicationSwapchain;
        dedicationPanel.subImage.imageRect.offset.x = 0;
        dedicationPanel.subImage.imageRect.offset.y = 0;
        dedicationPanel.subImage.imageRect.extent.width = g_dedicationWidth;
        dedicationPanel.subImage.imageRect.extent.height = g_dedicationHeight;
        dedicationPanel.subImage.imageArrayIndex = 0;
        dedicationPanel.pose = g_dedicationPose;
        dedicationPanel.size.width = kDedicationWidthMetres;
        dedicationPanel.size.height = kDedicationWidthMetres * (float)g_dedicationHeight / (float)g_dedicationWidth;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&dedicationPanel;
    }
    // (While changing to or from the cutscene screen and more than half dark,
    // the interface is left out: it would float in the black. The screen
    // itself is darkened in its picture.)
    bool interfaceInDark = !theatre && g_theatreDark > 0.5f;
    // Paused during a scene: the cutscene screen where it stood, holding the scene's last picture, under
    // the panel with the game's menu.
    if (pausedOnScreen && g_pausedScreenKept && g_screenPicture.hasPicture && g_frameShouldShow)
    {
        pausedScreen.layerFlags = 0;
        pausedScreen.space = g_floorSpace;
        pausedScreen.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        pausedScreen.subImage.swapchain = g_screenPicture.swapchain;
        pausedScreen.subImage.imageRect.offset.x = 0;
        pausedScreen.subImage.imageRect.offset.y = 0;
        pausedScreen.subImage.imageRect.extent.width = g_screenPicture.width;
        pausedScreen.subImage.imageRect.extent.height = g_screenPicture.height;
        pausedScreen.subImage.imageArrayIndex = 0;
        pausedScreen.pose.orientation.x = 0.0f;
        pausedScreen.pose.orientation.y = sinf(g_pausedScreenYaw * 0.5f);
        pausedScreen.pose.orientation.z = 0.0f;
        pausedScreen.pose.orientation.w = cosf(g_pausedScreenYaw * 0.5f);
        pausedScreen.pose.position.x = g_pausedScreenPosition[0];
        pausedScreen.pose.position.y = g_pausedScreenPosition[1];
        pausedScreen.pose.position.z = g_pausedScreenPosition[2];
        pausedScreen.size.width = g_pausedScreenWidth;
        pausedScreen.size.height = g_pausedScreenHeight;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&pausedScreen;
    }
    if (addPanelLayer && !interfaceInDark) layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&panel;
    if (addPieceLayers && !interfaceInDark)
    {
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&stripPanel;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&cardPanel;
    }

    // The mod's own menu, on a smaller panel of its own in front of everything.
    double overlaysStarted = NowMs(); // (timed to the end of the pointer's layers)
    if (GlLoadFunctions()) PrepareVrMenuPicture();
    if (g_frameShouldShow && VrMenuIsOpen() && g_menuPlaced && g_menuHasPicture && g_menuSwapchain != XR_NULL_HANDLE)
    {
        // Solid, nothing showing through it; but for the start-up page in the game's own look, a
        // dark patch fading into the room at its edges (VrMenuSeeThrough).
        menuPanel.layerFlags = VrMenuSeeThrough() ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        menuPanel.space = g_floorSpace;
        menuPanel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        menuPanel.subImage.swapchain = g_menuSwapchain;
        menuPanel.subImage.imageRect.offset.x = 0;
        menuPanel.subImage.imageRect.offset.y = 0;
        menuPanel.subImage.imageRect.extent.width = g_menuSwapchainWidth;
        menuPanel.subImage.imageRect.extent.height = g_menuSwapchainHeight;
        menuPanel.subImage.imageArrayIndex = 0;
        menuPanel.pose.orientation.x = 0.0f;
        menuPanel.pose.orientation.y = sinf(g_menuYaw * 0.5f);
        menuPanel.pose.orientation.z = 0.0f;
        menuPanel.pose.orientation.w = cosf(g_menuYaw * 0.5f);
        menuPanel.pose.position.x = g_menuPosition[0];
        menuPanel.pose.position.y = g_menuPosition[1];
        menuPanel.pose.position.z = g_menuPosition[2];
        menuPanel.size.width = kMenuWidthMetres;
        menuPanel.size.height = g_menuHeightMetres;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&menuPanel;
    }

    // The bullet counter, a small tag of its own.
    g_counterShownNow = false;
    if (GlLoadFunctions() && !g_dedicationHolding && PrepareAmmoCounter() && eyesShown && g_counterSwapchain != XR_NULL_HANDLE) // (not behind the studio's card)
    {
        counterPanel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        counterPanel.space = g_floorSpace;
        counterPanel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        counterPanel.subImage.swapchain = g_counterSwapchain;
        counterPanel.subImage.imageRect.offset.x = 0;
        counterPanel.subImage.imageRect.offset.y = 0;
        counterPanel.subImage.imageRect.extent.width = g_counterSwapchainWidth;
        counterPanel.subImage.imageRect.extent.height = g_counterSwapchainHeight;
        counterPanel.subImage.imageArrayIndex = 0;
        counterPanel.pose = g_counterPose;
        counterPanel.size.width = g_counterWidthMetres;
        counterPanel.size.height = g_counterWidthMetres * (float)g_counterSwapchainHeight / (float)g_counterSwapchainWidth;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&counterPanel;
        g_counterShownNow = true;
    }

    // The hints' labels at the hands: at most two.
    XrCompositionLayerQuad hintQuads[2] = { { XR_TYPE_COMPOSITION_LAYER_QUAD }, { XR_TYPE_COMPOSITION_LAYER_QUAD } };
    g_hintLabels[0].layeredNow = g_hintLabels[1].layeredNow = false;
    UpdateHints(eyesShown, NowMs());
    if (GlLoadFunctions() && eyesShown && !g_dedicationHolding) AddHintLayers(hintQuads, layers, layerCount, kMaxLayers - layerCount > 8 ? 2 : 0); // (nor these behind the studio's card)

    // The pointer in menus, over everything. (Not while the studio's card holds
    // the game back, unless the mod's own menu is opened meanwhile: the hands
    // are in the game's main menu then, and the pointer's two orbs would glow at
    // the controllers in the black around the card.)
    if (GlLoadFunctions() && (eyesShown || pausedOnScreen) && (!g_dedicationHolding || VrMenuIsOpen())) // (and paused during a scene, for the game's menu)
        AddPointerLayers(layers, layerCount, kMaxLayers - layerCount);
    else g_pointerPieceCount = 0;
    g_sendPartMs[kSendPartOverlays] += NowMs() - overlaysStarted;

    // A line for the menu's developer page: how fast the game is running.
    {
        static unsigned long long countStart = 0;
        static int counted = 0;
        unsigned long long tick = GetTickCount64();
        ++counted;
        if (countStart == 0) countStart = tick;
        if (tick - countStart >= 1000)
        {
            char info[96];
            wsprintfA(info, "%d frames per second (each eye gets half of them)", (int)(counted * 1000 / (tick - countStart)));
            VrMenuSetInfo(info);
            countStart = tick;
            counted = 0;
        }
    }

    if (eyesShown)
    {
        if (g_framesShown == 0) Log("VR: sending 3D pictures to the headset.");
        ++g_framesShown;
        ++g_statEyesShown;
    }

    g_panelShown = panelShown;
    g_piecesShown = piecesShown;

    {
        SendPartTimer timer(kSendPartEndFrame);
        // SteamVR: the panels drawn into the eyes' pictures, and those alone sent.
        if (g_panelsInEyes)
            layerCount = GlLoadFunctions() ? PanelsIntoEyes(layers, layerCount, eyesShown, layer, eyeViews) : 0;

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = g_frameDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = layerCount;
        endInfo.layers = layerCount ? layers : nullptr;
        if (!Check(XrEndFrameGl(g_session, &endInfo), "Sending the frame to the headset"))
            g_state = VrState::Failed;
    }

    // Last of all, tidy up what the monitor will show.
    // (On the cutscene screen the window already holds the finished picture.)
    if (GlLoadFunctions() && !theatre)
    {
        SendPartTimer timer(kSendPartMonitor);
        ComposeMonitorPicture(eyePictureCopied && g_frameEye == 0, haveGui, windowGuiTexture);
    }
}

// Reads where both eyes will be at `time`. Returns false if not tracked.
static bool LocateEyes(XrTime time, XrView eyes[2])
{
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = time;
    locateInfo.space = g_floorSpace;

    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    eyes[0] = { XR_TYPE_VIEW };
    eyes[1] = { XR_TYPE_VIEW };
    uint32_t eyeCount = 0;
    if (XR_FAILED(xrLocateViews(g_session, &locateInfo, &viewState, 2, &eyeCount, eyes))) return false;
    if (eyeCount < 2) return false;

    return (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
           (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

// ---------------------------------------------------------------------------
// The PC's own keyboard and mouse are kept out of the game while it is played
// in the headset (setting "pc_input", off unless switched on): a knock of the
// mouse doesn't swing the view or move the menu pointer. Everything the
// mod itself sends (the controllers' keys, the menu pointer) is marked by
// Windows as sent by a program, and still gets through. So do Alt (and
// anything pressed with it, such as Alt+Tab), the Windows keys and F1 to F12.
// Only while the game's window is in front. Ctrl+F12 switches the PC's
// keyboard and mouse back on (and off again).
// ---------------------------------------------------------------------------

static HWND volatile g_inputGameWindow = nullptr;
static volatile bool g_blockPcInput = false;
static volatile bool g_pcInputToggleAsked = false;
static volatile unsigned long long g_blockDecidedAt = 0; // (if the frame stops deciding, for example VR stopped, nothing is kept out)

static bool PcInputKeptOut()
{
    return g_blockPcInput && GetTickCount64() - g_blockDecidedAt < 1000 && g_inputGameWindow != nullptr && GetForegroundWindow() == g_inputGameWindow;
}

// What other programs send (marked by Windows as sent by a program, but
// without the mod's mark, kModInputMark: SteamVR's laser mouse, Steam's
// controller emulation and the like) is kept out of the game too while the
// headset has the game in focus and its window is in front, whatever pc_input
// says: the controllers drive the game then, through the mod alone. (While
// SteamVR's dashboard is up the headset does not have the game in focus, and
// its laser mouse works normally.) Counted for the log (UpdatePcInputBlock).
static volatile bool g_keepOutOtherPrograms = false;
static volatile LONG g_otherProgramMouse = 0, g_otherProgramKeys = 0, g_otherProgramKeptOut = 0;

static bool OtherProgramsKeptOut()
{
    return g_keepOutOtherPrograms && GetTickCount64() - g_blockDecidedAt < 1000 && g_inputGameWindow != nullptr &&
           GetForegroundWindow() == g_inputGameWindow;
}

static LRESULT CALLBACK BlockPcKeyboard(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* key = (const KBDLLHOOKSTRUCT*)lParam;
        bool fromProgram = (key->flags & LLKHF_INJECTED) != 0;
        bool fromMod = fromProgram && key->dwExtraInfo == kModInputMark;
        bool pressed = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        if (!fromProgram) g_lastPcInputMs = GetTickCount64();
        if (!fromProgram && pressed && key->vkCode == VK_F12 && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
            g_pcInputToggleAsked = true;

        bool letThrough = fromMod || (key->flags & LLKHF_ALTDOWN) != 0 || key->vkCode == VK_LMENU || key->vkCode == VK_RMENU ||
                          key->vkCode == VK_LWIN || key->vkCode == VK_RWIN || (key->vkCode >= VK_F1 && key->vkCode <= VK_F12) ||
                          key->vkCode == VK_LCONTROL || key->vkCode == VK_RCONTROL;
        if (fromProgram && !fromMod) InterlockedIncrement(&g_otherProgramKeys);
        if (!letThrough && !fromProgram && PcInputKeptOut())
            return 1;
        if (!letThrough && fromProgram && OtherProgramsKeptOut())
        {
            InterlockedIncrement(&g_otherProgramKeptOut);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static LRESULT CALLBACK BlockPcMouse(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const MSLLHOOKSTRUCT* mouse = (const MSLLHOOKSTRUCT*)lParam;
        bool fromProgram = (mouse->flags & LLMHF_INJECTED) != 0;
        bool fromMod = fromProgram && mouse->dwExtraInfo == kModInputMark;
        if (!fromProgram) g_lastPcInputMs = GetTickCount64();
        if (!fromProgram && PcInputKeptOut())
            return 1;
        if (fromProgram && !fromMod)
        {
            InterlockedIncrement(&g_otherProgramMouse);
            if (OtherProgramsKeptOut())
            {
                InterlockedIncrement(&g_otherProgramKeptOut);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static DWORD WINAPI PcInputThread(LPVOID)
{
    // Windows holds up every key press and mouse move on the whole PC until
    // this thread has looked at it, so it must never wait for anything: it
    // runs ahead of other work. (For the same reason it is started only once
    // the headset is showing pictures, after the heavy start: see
    // UpdatePcInputBlock.)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HHOOK keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, BlockPcKeyboard, GetModuleHandleW(nullptr), 0);
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, BlockPcMouse, GetModuleHandleW(nullptr), 0);
    Log("VR: watching the PC's keyboard and mouse (%s, %s), to keep them out of the game while in the headset.",
        keyboard ? "keyboard ok" : "keyboard FAILED", mouse ? "mouse ok" : "mouse FAILED");
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {}
    return 0;
}

// The game's window kept in front of the PC while the headset has the game in
// focus. The controllers reach the game through the PC's keyboard and mouse,
// which Windows gives to the window in front, and the mod points and clicks
// only into the game's window, never into another program; with another
// window in front, everything the controllers do goes nowhere until the game
// is clicked at the PC. SteamVR causes this: its status window, set to stay on
// top, can take a click over the game's (see ClickLandsOnGame), and its
// dashboard, with a laser mouse that clicks with the PC's own pointer, can be
// up at the start, as the main menu comes, and after the controllers drop out
// ("SystemBehaviorFlag_LaserMouse" in vrclient_vrcompositor.txt). So while the
// headset has the game in focus (not while the dashboard or the headset's own
// menu is up), when another window has been in front for 0.4 s the game's is
// brought back (every 2 s at most), unless the PC's own mouse or keyboard was
// used in the last 5 s (someone at the PC put that window there), the other
// window is the game's own (the hand poses tool), Steam's overlay is open, or
// this is a test run with the stand-in runtime (the PC may be in use then; its
// "front 1" command asks for it anyway). The game's window put on top of the
// windows that stay on top (ClickLandsOnGame) is let go as soon as the headset
// does not have the game in focus or the PC's own mouse or keyboard is used.
static void UpdateGameFront()
{
    HWND game = g_inputGameWindow;
    if (game == nullptr) return;
    unsigned long long now = GetTickCount64();
    unsigned long long lastPc = g_lastPcInputMs;
    bool someoneAtPc = lastPc != 0 && now - lastPc < 5000;

    if (g_gameMadeTopmost && (!g_sessionFocused || someoneAtPc || g_state != VrState::Ready))
    {
        SetWindowPos(game, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        g_gameMadeTopmost = false;
        Log("VR: the game's window no longer stays on top of all others (%s).",
            someoneAtPc ? "the PC's own mouse or keyboard was used" : "the headset does not have the game in focus");
    }

    HWND front = GetForegroundWindow();
    bool inFront = front == game;
    static bool wasInFront = true, wasFocused = false;
    static unsigned long long lostAt = 0, lastTry = 0, focusedSince = 0;
    static int changeLogsLeft = 30, tryLogsLeft = 20;
    if (g_sessionFocused && !wasFocused) focusedSince = now;
    wasFocused = g_sessionFocused;

    if (inFront != wasInFront)
    {
        wasInFront = inFront;
        if (!inFront) lostAt = now;
        if (changeLogsLeft > 0)
        {
            --changeLogsLeft;
            char frontText[400];
            DescribeWindow(front, frontText, sizeof(frontText));
            if (inFront)
                Log("VR: the game's window is in front of the PC again.");
            else
                Log("VR: the game's window is no longer in front of the PC: %s is (the headset %s the game in focus; the PC's own mouse "
                    "or keyboard %s).", frontText, g_sessionFocused ? "has" : "does not have", someoneAtPc ? "was just used" : "not used just now");
        }
    }
    if (inFront || !g_sessionFocused || (g_standIn && !g_standInFrontTest) || g_state != VrState::Ready || g_steamOverlayOpen || someoneAtPc)
        return;
    if (now - lostAt < 400 || now - focusedSince < 400 || now - lastTry < 2000) return;
    if (front != nullptr)
    {
        DWORD process = 0;
        GetWindowThreadProcessId(front, &process);
        if (process == GetCurrentProcessId()) return; // (the game's own: the hand poses tool)
    }
    lastTry = now;

    // Windows lets a program bring its window in front only in some cases;
    // joined for a moment to the input of the window in front, it may.
    DWORD ours = GetCurrentThreadId();
    DWORD theirs = front != nullptr ? GetWindowThreadProcessId(front, nullptr) : 0;
    bool joined = theirs != 0 && theirs != ours && AttachThreadInput(ours, theirs, TRUE) != FALSE;
    if (IsIconic(game)) ShowWindow(game, SW_RESTORE);
    BringWindowToTop(game);
    SetForegroundWindow(game);
    if (joined) AttachThreadInput(ours, theirs, FALSE);
    bool done = GetForegroundWindow() == game;
    if (tryLogsLeft > 0)
    {
        --tryLogsLeft;
        char frontText[400];
        DescribeWindow(front, frontText, sizeof(frontText));
        Log("VR: the game's window brought back in front of the PC (from %s), so that the controllers reach the game: %s.", frontText,
            done ? "done" : "Windows did not allow it (tried again in 2 s)");
    }
}

// The mod's watcher (watch.cpp, watch_shared.h): a game that stops while the
// mod holds a button down (the right mouse button, say) leaves it held for the
// whole PC, and the watcher lets it go. Started once: the shared memory of
// what the mod holds (kept for the whole run), and BunkerVR_watch.exe beside
// the game, out of the game's job where Windows allows, so that it outlives
// the game however it ends.
static void StartWatcher()
{
    DWORD processId = GetCurrentProcessId();
    wchar_t name[64];
    WatchSharedName(processId, name, 64);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(WatchShared), name);
    WatchShared* shared = mapping != nullptr ? (WatchShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WatchShared)) : nullptr;
    if (shared == nullptr)
    {
        Log("WATCH: no watcher: its shared memory could not be made (error %lu). Should the game crash while a button or key is held, "
            "one press of it at the PC lets it go.", GetLastError());
        if (mapping != nullptr) CloseHandle(mapping);
        return;
    }
    shared->version = kWatchVersion;
    shared->mouseHeld = g_modMouseHeld;
    for (int index = 0; index < 512; ++index)
        if (g_modKeyHeld[index]) shared->keyHeld[index >> 5] |= (LONG)(1u << (index & 31));
    shared->beatMs = (LONG64)GetTickCount64();
    g_watchShared = shared; // (and its handle kept open for the run)

    wchar_t exe[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    while (length > 0 && exe[length - 1] != L'\\') --length;
    exe[length] = L'\0';
    if (length + 24 >= MAX_PATH) return;
    lstrcatW(exe, L"BunkerVR_watch.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
    {
        Log("WATCH: no watcher: BunkerVR_watch.exe is not beside the game. Should the game crash while a button or key is held, "
            "one press of it at the PC lets it go.");
        return;
    }
    wchar_t commandLine[MAX_PATH + 32];
    wsprintfW(commandLine, L"\"%s\" %lu", exe, processId);
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    BOOL started = CreateProcessW(exe, commandLine, nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW,
                                  nullptr, nullptr, &startup, &process);
    bool brokeAway = started != FALSE;
    if (!started)
        started = CreateProcessW(exe, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!started)
    {
        Log("WATCH: no watcher: BunkerVR_watch.exe could not be started (error %lu).", GetLastError());
        return;
    }
    Log("WATCH: the watcher started (BunkerVR_watch.exe, process %lu%s): anything the mod holds down is let go if the game "
        "crashes, is stopped from outside, or draws no frame for %llu ms (its notes: BunkerVR_watch.log).",
        process.dwProcessId, brokeAway ? "" : ", in the game's job", kWatchStallMs);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

// The watcher let go of everything while the game stood still (watch.cpp):
// nothing is held now. Forgotten here too, so that what is still wanted (a
// squeeze, the stick pushed) is pressed again by the next update.
static void ForgetHeldAfterWatcher()
{
    g_grabHeld = false;
    g_rightMouseDown = false;
    for (int key = 0; key < 4; ++key) g_moveKeyDown[key] = false;
    for (int key = 0; key < kExtraKeyCount; ++key) g_extraKeyDown[key] = false;
    g_modMouseHeld = 0;
    for (int index = 0; index < 512; ++index) g_modKeyHeld[index] = false;
    static int logsLeft = 10;
    if (logsLeft > 0)
    {
        --logsLeft;
        Log("WATCH: the game stood still with buttons or keys held, and the watcher let go of them (BunkerVR_watch.log says which); "
            "what is still wanted is pressed again.");
    }
}

// Called every frame: starts the watcher of the PC's keyboard and mouse once,
// and decides whether they are kept out right now. (And the mod's watcher,
// above: started once, told each frame that the game runs.)
static void UpdatePcInputBlock()
{
    static bool watcherStarted = false;
    if (!watcherStarted)
    {
        watcherStarted = true;
        StartWatcher();
    }
    if (g_watchShared)
    {
        InterlockedExchange64(&g_watchShared->beatMs, (LONG64)GetTickCount64());
        if (InterlockedExchange(&g_watchShared->letGo, 0) != 0) ForgetHeldAfterWatcher();
    }

    // Started once the headset has been showing pictures for about a second,
    // not during the heavy start of the game and the headset.
    static bool started = false;
    if (!started && g_framesShown > 90)
    {
        started = true;
        HANDLE thread = CreateThread(nullptr, 0, PcInputThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    if (g_pcInputToggleAsked)
    {
        g_pcInputToggleAsked = false;
        GetSettings().pcInput = !GetSettings().pcInput;
        SaveSettings();
        Log("VR: Ctrl+F12: the PC's keyboard and mouse are now %s.", GetSettings().pcInput ? "ON in the game" : "kept out of the game");
    }
    HWND window = WindowFromDC(wglGetCurrentDC());
    if (window != nullptr) g_inputGameWindow = window;

    // Steam's overlay open (see SteamOverlayInUse): the PC's
    // keyboard and mouse work, so it can be closed at the PC.
    bool overlay = SteamOverlayInUse();
    if (overlay != g_steamOverlayOpen)
    {
        g_steamOverlayOpen = overlay;
        Log("VR: Steam's overlay is %s.", overlay ? "open over the game: the controllers' keys are held back, the PC's keyboard and mouse work, "
                                                     "and B (or Shift+Tab at the PC) closes it"
                                                   : "closed: the controllers drive the game again");
    }
    bool block = !GetSettings().pcInput && g_state == VrState::Ready && !g_steamOverlayOpen;
    g_keepOutOtherPrograms = g_sessionFocused && g_state == VrState::Ready && !g_steamOverlayOpen; // (see OtherProgramsKeptOut)
    g_blockDecidedAt = GetTickCount64();
    if (block != g_blockPcInput)
    {
        g_blockPcInput = block;
        Log("VR: the PC's keyboard and mouse are %s.", block ? "kept out of the game (Ctrl+F12 lets them in)" : "working in the game");
    }

    // What other programs sent (see OtherProgramsKeptOut), for the log: every 10 s at most.
    static LONG seenMouse = 0, seenKeys = 0, seenKeptOut = 0;
    static unsigned long long lastSaid = 0;
    static int saysLeft = 20;
    LONG mouseNow = g_otherProgramMouse, keysNow = g_otherProgramKeys, keptOutNow = g_otherProgramKeptOut;
    if ((mouseNow != seenMouse || keysNow != seenKeys || keptOutNow != seenKeptOut) && saysLeft > 0 &&
        GetTickCount64() - lastSaid >= 10000)
    {
        --saysLeft;
        lastSaid = GetTickCount64();
        HWND front = GetForegroundWindow();
        char frontText[400];
        DescribeWindow(front, frontText, sizeof(frontText));
        Log("VR: another program sent the PC %ld mouse events and %ld keys (not the mod's, not the PC's own); %ld of them kept out of "
            "the game (the headset %s the game in focus; in front: %s).",
            (long)(mouseNow - seenMouse), (long)(keysNow - seenKeys), (long)(keptOutNow - seenKeptOut),
            g_sessionFocused ? "has" : "does not have", frontText);
        seenMouse = mouseNow;
        seenKeys = keysNow;
        seenKeptOut = keptOutNow;
    }

    UpdateGameFront();
}

void* VrGameWindow()
{
    return (void*)g_inputGameWindow;
}

bool VrHeadsetIsOff()
{
    return g_headsetOff;
}

// Is the headset away from the game for more than a moment: taken off (its
// session stopped), or showing the headset software's own menu (SteamVR's
// dashboard, the Meta menu: the game still shown, without its input)? The menu
// script then opens the game's menu (VRPauseWhileAway in MenuHandler.hps), as
// VR games pause there. Not before the game first had the headset's input.
bool VrHeadsetAway()
{
    static const double kAwayAfterMs = 250.0;
    static double awaySinceMs = -1.0;
    static bool everFocused = false, said = false;
    if (g_sessionFocused) everFocused = true;
    bool away = everFocused && g_state == VrState::Ready && (g_headsetOff || !g_sessionFocused);
    if (!away)
    {
        if (said) Log("VR: the headset is back in use, with its input.");
        awaySinceMs = -1.0;
        said = false;
        return false;
    }
    double nowMs = NowMs();
    if (awaySinceMs < 0.0) awaySinceMs = nowMs;
    if (nowMs - awaySinceMs < kAwayAfterMs) return false;
    if (!said)
    {
        said = true;
        Log("VR: the headset is away (%s): the game's menu is asked for.",
            g_headsetOff ? "taken off" : "its own menu is open, the game has no input");
    }
    return true;
}

bool VrQuitAsked()
{
    return g_quitAsked;
}

bool VrHandsFrozen()
{
    return g_handsFrozen;
}

void VrSetHandInFront(bool on, int hand, int facing)
{
    HeadPose pose = {};
    bool placed = false;
    HeadPose head;
    if (on && hand >= 0 && hand <= 1 && VrGetHeadCentrePose(head))
    {
        // The head's own directions are its rotation's columns (x right, y
        // up, z backwards): its forward and right, kept level.
        float forward[3] = { -head.rotation[2], 0.0f, -head.rotation[8] };
        float length = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
        if (length > 0.01f)
        {
            forward[0] /= length;
            forward[2] /= length;
            float right[3] = { -forward[2], 0.0f, forward[0] };
            float side = hand == 1 ? 1.0f : -1.0f;
            float scale = GetSettings().worldScale; // (distances measured as the head's are real meters times the world scale)
            // 40 cm ahead of the eyes, 15 cm below them, 8 cm to its own side.
            for (int i = 0; i < 3; ++i) pose.position[i] = head.position[i] + (forward[i] * 0.40f + right[i] * side * 0.08f) * scale;
            pose.position[1] = head.position[1] - 0.15f * scale;

            // The controller's own directions (x right, y up, z backwards).
            // Held out: pointing ahead, as an item is held out. Palm (or the
            // back of the hand) towards the eyes: turned a quarter about the
            // upright. (A right palm faces the controller's -x, a left one
            // its +x.)
            float palmSide = hand == 1 ? -1.0f : 1.0f;
            float x[3] = { right[0], 0.0f, right[2] };
            if (facing == 1)
            {
                x[0] = -forward[0] * palmSide;
                x[2] = -forward[2] * palmSide;
            }
            else if (facing == 2)
            {
                x[0] = forward[0] * palmSide;
                x[2] = forward[2] * palmSide;
            }
            float y[3] = { 0.0f, 1.0f, 0.0f };
            float z[3] = { -x[2], 0.0f, x[0] }; // (x crossed with y)
            for (int i = 0; i < 3; ++i)
            {
                pose.rotation[i * 3 + 0] = x[i];
                pose.rotation[i * 3 + 1] = y[i];
                pose.rotation[i * 3 + 2] = z[i];
            }
            placed = true;
        }
    }

    AcquireSRWLockExclusive(&g_inFrontLock);
    bool wasOn = g_inFrontOn;
    g_inFrontOn = placed;
    if (placed)
    {
        g_inFrontHand = hand;
        g_inFrontPose = pose;
    }
    ReleaseSRWLockExclusive(&g_inFrontLock);
    if (placed)
        Log("VR: the %s hand floats in front of the eyes (%s; the hand poses tool's \"Hand in front of me\").", hand == 0 ? "left" : "right",
            facing == 1 ? "palm towards them" : (facing == 2 ? "back of the hand towards them" : "held out"));
    else if (wasOn)
        Log("VR: the floating hand is back with its controller.");
    else if (on)
        Log("VR: \"Hand in front of me\": not placed (where the head is not known yet).");
}

void VrSetHandsFrozen(bool frozen)
{
    if (frozen == g_handsFrozen) return;
    g_handsFrozen = frozen;
    Log("VR: the hands are %s.", frozen ? "frozen where they are (the hand poses tool's \"Freeze my hands\")" : "following the controllers again");
}

// Are the game's scripts the mod's? BunkerVR Setup patches them, and a Steam
// update or Steam's "Verify integrity of game files" puts the game's own back.
// Without its scripts the mod cannot work (no hands, body or items), so VR is
// not started then: the game runs as it is, and the player is told to run
// Setup again. One file is enough to tell: Steam puts back every changed file
// at once. A file that cannot be read counts as the mod's (the game itself
// then says what is wrong).
static bool GameScriptsAreTheMods()
{
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    static const wchar_t kScript[] = L"script\\custom\\modules\\MenuHandler.hps";
    if (length + (DWORD)wcslen(kScript) + 1 >= MAX_PATH) return true;
    lstrcatW(path, kScript);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    LARGE_INTEGER size = {};
    bool theMods = true;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024)
    {
        char* text = new char[(size_t)size.QuadPart + 1];
        DWORD read = 0;
        if (ReadFile(file, text, (DWORD)size.QuadPart, &read, nullptr) && read > 0)
        {
            text[read] = '\0';
            theMods = strstr(text, "cVR_") != nullptr;
        }
        delete[] text;
    }
    CloseHandle(file);
    return theMods;
}

static DWORD WINAPI ScriptsNotTheModsMessage(LPVOID)
{
    MessageBoxW(nullptr,
                L"The game's own files have been put back, by a Steam update or by Steam's \"Verify integrity of game "
                L"files\", so BunkerVR can't run and the game starts without VR.\n\n"
                L"Run BunkerVR Setup again, then start the game. If Setup says the game was updated, BunkerVR needs an "
                L"update too.\n\nTo play without VR, uninstall BunkerVR in Windows' Installed apps.",
                L"BunkerVR", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

// First half of a headset frame: wait for the headset's timing and read where
// the head will be, so the game can draw its next picture from there.
static void BeginFrameWork()
{
    if (g_state == VrState::NotStarted)
    {
#ifdef BUNKERVR_COPY_LOCK
        // A private test build plays only for the Steam accounts it was made for: not before licence.cpp has
        // confirmed the account.
        if (LicenceState() != 1) return;
#endif
        if (!GameScriptsAreTheMods())
        {
            g_state = VrState::Failed;
            Log("VR: the game's scripts are its own, not BunkerVR's (a Steam update or a check of the game's files put "
                "them back): VR is not started; the player is told to run BunkerVR Setup again.");
            HANDLE thread = CreateThread(nullptr, 0, ScriptsNotTheModsMessage, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            return;
        }
        if (Setup())
        {
            g_state = VrState::Ready;
        }
        else
        {
            g_state = VrState::Failed;
            Log("VR: setup failed. The game will keep running normally without VR.");
            return;
        }
    }
    UpdatePcInputBlock();

    // Both eyes every frame, second half: the same headset frame goes on, now
    // for the right eye. No waiting for the headset here. The game's clock is
    // held, so its world does not move on between the two pictures.
    if (g_midPair)
    {
        g_midPair = false;
        g_secondOfPair = true;
        g_frameEye = 1;
        ++g_frameNumber;
        g_frameSerialAtBegin = g_frameNumber;
        if (g_framePoseValid)
        {
            g_frameEyePose = g_pairEyes[g_swapEyes ? 0 : 1].pose;
            g_frameEyeFov = g_pairEyes[1].fov;
            PublishHeadPose(g_frameHeadPose, g_frameEyePose, false); // the camera hook picks this up (the second picture of the same game frame: see "turning with the stick" in PublishHeadPose)
        }
        // Drawing the pair as two whole game frames needs the game's clock
        // held. Stereo in one game frame does not: it is the same frame.
        if (!g_stereoMidFrame) ClockHold(true);
        return;
    }
    g_secondOfPair = false;
    g_gamePicture = 0; // a new headset frame: pictures come from the window unless stereo in one frame says otherwise

    HandleEvents();
    if (g_state != VrState::Ready || !g_sessionRunning) return;

    // Waits until the headset wants the next frame. While VR is running this
    // is what sets the game's speed (90 frames per second on a Rift).
    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    double waitStarted = NowMs();
    bool waited = Check(xrWaitFrame(g_session, nullptr, &frameState), "Waiting for the headset's next frame");
    g_statLastWaitMs = NowMs() - waitStarted;
    if (!waited)
    {
        g_state = VrState::Failed;
        return;
    }

    // For the log: how evenly the frames come. A frame is "late" when it
    // starts more than one and a half of the headset's frame times after the
    // one before (it then missed its turn and the last picture was shown again).
    // The game's logic rate follows it too (UpdateLogicHalf).
    {
        double nowMs = NowMs();
        g_statPeriodMs = (double)frameState.predictedDisplayPeriod / 1000000.0;
        g_displayPeriodMs = g_statPeriodMs;
        if (g_statLastFrameMs != 0)
        {
            double gap = nowMs - g_statLastFrameMs;
            ++g_statHeadsetFrames;
            if (gap > g_statWorstGapMs) g_statWorstGapMs = gap;
            bool late = g_statPeriodMs > 1.0 && gap > g_statPeriodMs * 1.5;
            if (g_standIn && gap > 100.0) Log("STOPWATCH: a stop of %.0f ms.", gap);
            if (late) ++g_statLateFrames;
            NoteLateFrame(late, gap, waitStarted - g_statLastFrameMs); // (before the rate follows it: the frame ran at the rate it had)
            UpdateLogicHalf(late);
        }
        g_statLastFrameMs = nowMs;
        if (g_standIn) StandInWatchFrame();
    }
    if (!Check(XrBeginFrameGl(g_session, nullptr), "Beginning the headset frame"))
    {
        g_state = VrState::Failed;
        return;
    }

    g_frameOpen = true;
    g_frameShouldShow = frameState.shouldRender == XR_TRUE;
    g_frameDisplayTime = frameState.predictedDisplayTime;

    // The eyes take turns: even frames are the left eye, odd frames the right.
    g_frameEye = (int)(g_frameNumber & 1);
    ++g_frameNumber;
    g_frameSerialAtBegin = g_frameNumber;

    // Both eyes in step: the game's clock is set once, as the left eye's
    // picture begins, and then stands still until the next left eye's picture.
    // The game draws the right eye's picture without having moved its world.
    // Only while the world is being drawn (not in loading screens, where the
    // game waits for time to pass).
    if (GetSettings().pairFrames || GetSettings().stereoFrame)
    {
        // Both eyes drawn per headset frame: every headset frame starts with
        // the left eye and a running clock (with pair_frames it is held for
        // the right eye's picture, further up).
        ClockHold(false);
        g_frameEye = 0;
    }
    else if (GetSettings().bothEyes)
    {
        if (g_frameEye == 0)
        {
            ClockHold(false);
            if (CameraLastWorldSerial() + 1 >= g_frameSerialAtBegin && !CameraTheatre()) ClockHold(true);
        }
    }
    else
    {
        ClockHold(false);
    }

    // Where will the head and each eye be when this frame is shown?
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    XrResult located = xrLocateSpace(g_headSpace, g_floorSpace, frameState.predictedDisplayTime, &head);
    bool headValid = XR_SUCCEEDED(located) &&
                     (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                     (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;

    XrView eyes[2];
    g_framePoseValid = headValid && LocateEyes(frameState.predictedDisplayTime, eyes);
    if (headValid) g_frameHeadPose = head.pose;
    UpdateStageFloor(frameState.predictedDisplayTime, headValid, head.pose.position.y); // (before the re-center below, which may use it)
    if (headValid) UpdateMeasure(head.pose);
    if (headValid && g_haveReference) UpdateDuck(head.pose);

    if (g_framePoseValid)
    {
        g_pairEyes[0] = eyes[0];
        g_pairEyes[1] = eyes[1];
        g_frameEyePose = eyes[g_swapEyes ? 1 - g_frameEye : g_frameEye].pose;
        g_frameEyeFov = eyes[g_frameEye].fov;

        // Testing aid: the capture tool can ask for one frame to be tilted upwards.
        float tilt = CaptureGetTestTilt(g_frameNumber);
        if (tilt != 0.0f)
        {
            // Combine the eye's rotation with a rotation about its own left-right axis.
            XrQuaternionf q = g_frameEyePose.orientation;
            float sx = sinf(tilt * 0.5f), cx = cosf(tilt * 0.5f);
            XrQuaternionf tilted;
            tilted.x = q.x * cx + q.w * sx;
            tilted.y = q.y * cx + q.z * sx;
            tilted.z = q.z * cx - q.y * sx;
            tilted.w = q.w * cx - q.x * sx;
            g_frameEyePose.orientation = tilted;
        }
        CheckPickedUp();
        UpdateMenuRoom(head.pose);
        UpdateRecentre();
        PublishHeadPose(head.pose, g_frameEyePose); // the camera hook picks this up

        // Ask the game's camera for a view big enough to cover everything
        // either eye can see through the headset's lenses.
        float tanAcross = 0, tanUpDown = 0;
        for (int i = 0; i < 2; ++i)
        {
            float l = tanf(-eyes[i].fov.angleLeft), r = tanf(eyes[i].fov.angleRight);
            float u = tanf(eyes[i].fov.angleUp), d = tanf(-eyes[i].fov.angleDown);
            if (l > tanAcross) tanAcross = l;
            if (r > tanAcross) tanAcross = r;
            if (u > tanUpDown) tanUpDown = u;
            if (d > tanUpDown) tanUpDown = d;
        }
        if (tanAcross > 0.1f && tanUpDown > 0.1f)
            CameraSetFieldOfViewOverride(2.0f * atanf(tanUpDown), tanAcross / tanUpDown);
    }
    else
    {
        CameraSetFieldOfViewOverride(0, 0);
    }

    unsigned long long now = GetTickCount64();
    if (g_poseLogsLeft > 0 && now - g_lastPoseLogTime >= 1000)
    {
        g_lastPoseLogTime = now;
        --g_poseLogsLeft;

        if (g_framePoseValid)
        {
            float yaw = 0, pitch = 0, roll = 0;
            ToYawPitchRoll(head.pose.orientation, yaw, pitch, roll);
            Log("HEAD  position (metres) x %+.2f  y %+.2f  z %+.2f   |   turn %+6.1f  up/down %+6.1f  tilt %+6.1f (degrees)",
                head.pose.position.x, head.pose.position.y, head.pose.position.z, yaw, pitch, roll);

            if (!g_loggedEyes) LogEyes(frameState.predictedDisplayTime);
        }
        else
        {
            Log("HEAD  not tracked right now (is the headset on and in view of the sensors?)");
        }
    }
}

// Runs `work` while protecting the game's OpenGL settings from any changes
// made by the mod or by the VR software.
static void RunProtected(void (*work)(), const char* stage, bool isBegin)
{
    if (g_state == VrState::Failed) return;

    double started = NowMs();
    g_statLastWaitMs = 0;

    HDC gameDC = wglGetCurrentDC();
    HGLRC gameContext = wglGetCurrentContext();

    bool canGuard = GlLoadFunctions();
    GlStateGuard guard;
    if (canGuard) guard.Capture();

    // (Errors left by the game's own drawing, or the mod's elsewhere: taken
    // and named apart from those the mod's work then leaves.)
    char where[96];
    snprintf(where, sizeof(where), "\"%s\" (left from before it: the game's drawing)", stage);
    TakeGlErrorsBefore(where);

    work();

    if (g_state == VrState::Failed) CameraSetFieldOfViewOverride(0, 0);

    if (wglGetCurrentContext() != gameContext || wglGetCurrentDC() != gameDC)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("VR: note: the VR software changed the active OpenGL context; restoring the game's one each frame.");
        }
        wglMakeCurrent(gameDC, gameContext);
    }

    if (canGuard && guard.Restore())
    {
        static int logsLeft = 1;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("VR: note: some OpenGL settings were changed during \"%s\" and have been put back.", stage);
        }
    }

    // Keep count of how long this took (the wait for the headset is counted apart).
    // Frames are counted at the start of a headset frame only, not in the
    // middle of one (stereo in one frame), so the log's figures are per frame.
    // (Counting the middle too would halve every figure with stereo in one
    // frame, the wait for the headset as well.)
    double took = NowMs() - started - g_statLastWaitMs;
    if (isBegin)
    {
        g_statBeginMs += took;
        g_statWaitMs += g_statLastWaitMs;
        if (!g_stereoMidFrame) ++g_statFrames;
        g_lastBeginTookMs = took; // (for the late frames: NoteLateFrame)
    }
    else
    {
        g_statEndMs += took;
        g_lastEndTookMs = took;
    }
}

void VrEndFrame()
{
    // (Timing marks 5 and 6 around the mod's own end-of-frame work, and 0 for
    // the start of the next frame: see GlTimerMark. Not for the left eye's
    // picture in the middle of a frame, which scene.cpp marks.)
    if (!g_stereoMidFrame) GlTimerMark(5);
    RunProtected(EndFrameWork, "sending the picture", false);
    if (!g_stereoMidFrame) GlTimerMark(6);
}

void VrBeginFrame()
{
    if (!g_stereoMidFrame) GlTimerMark(0);
    RunProtected(BeginFrameWork, "starting the frame", true);
}

// ---------------------------------------------------------------------------
// Stereo in one game frame
// ---------------------------------------------------------------------------
// scene.cpp makes the game draw its world twice inside one frame. Before the
// first drawing it asks whether this headset frame can be done that way; after
// it, VrStereoMidFrame takes the left eye's picture (hands and all, exactly as
// at the end of a frame) and moves the headset frame on to the right eye.
// The second drawing's picture is taken when the game's frame ends, as usual.

void VrFollowTurn(float gameYaw, bool haveYaw)
{
    // (Kept for measuring the game's own turning speed; see g_turnMeasuring.)
    if (haveYaw)
    {
        g_gameYawSeen = gameYaw;
        g_gameYawSeenKnown = true;
    }

    // For the first picture of a headset frame: from the game's logic, as soon
    // as its camera has turned (the camera hook asks on every use of the
    // camera, the arms' questions too), and as the world's drawing starts.
    if (g_state != VrState::Ready || !g_sessionRunning || !g_frameOpen) return;
    if (!g_framePoseValid || !g_haveReference || g_midPair || g_secondOfPair) return;
    if (!haveYaw || CameraTheatre())
    {
        g_turnHaveYaw = false;
        return;
    }
    g_turnAtDrawingMs = NowMs();
    if (g_turnHaveYaw && gameYaw == g_turnLastYaw) return; // (no turn since)
    if (g_snapWatchFrames > 0 && g_turnHaveYaw)
    {
        float turned = gameYaw - g_turnLastYaw;
        if (turned > 3.14159265f) turned -= 6.28318531f;
        if (turned < -3.14159265f) turned += 6.28318531f;
        Log("SNAP: the game's camera turned %+.1f degrees; followed about the head now (headset frame %llu).",
            turned * 57.29578f, g_frameNumber);
    }
    TurnAboutHead(gameYaw, g_frameHeadPose);
    // Shared again, from the middle as it is now.
    PublishHeadPose(g_frameHeadPose, g_frameEyePose, false);
}

bool VrStereoCanStart()
{
    if (!GetSettings().stereoFrame) return false;
    if (g_state != VrState::Ready || !g_sessionRunning || !g_frameOpen) return false;
    if (g_midPair || g_secondOfPair || g_frameEye != 0) return false;
    if (!g_frameShouldShow || !g_framePoseValid) return false;
    if (CameraTheatre() || CameraLoadingScreen()) return false; // cutscene screen, loading: one flat picture
    return true;
}

bool VrStereoMidFrame()
{
    // Where the engine has its finished picture right now: the target it is
    // aimed at as it comes out of drawing the viewport.
    g_gamePicture = GlLoadFunctions() ? GlGetDrawFramebuffer() : 0;
    static GLuint loggedTarget = 0xFFFFFFFF;
    if (g_gamePicture != loggedTarget)
    {
        loggedTarget = g_gamePicture;
        Log("VR: stereo in one frame: the eyes' pictures are taken from the game's drawing target %u (0 = the window).",
            g_gamePicture);
    }

    g_stereoMidFrame = true;
    VrEndFrame(); // takes and keeps the left eye's picture; sets g_midPair if that worked
    bool taken = g_midPair;
    if (taken) VrBeginFrame(); // moves on to the right eye (no waiting for the headset)
    g_stereoMidFrame = false;

    bool ok = taken && g_secondOfPair;
    if (!ok)
    {
        g_gamePicture = 0; // this frame is finished the usual way, from the window
        g_midFailedSerial = g_frameSerialAtBegin; // (for the log: see EndFrameWork)
    }
    return ok;
}
