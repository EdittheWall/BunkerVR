#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>

#include <MinHook.h>

#include "camera.h"
#include "grips.h"
#include "log.h"
#include "pattern.h"
#include "settings.h"
#include "vr.h"

// ---------------------------------------------------------------------------
// What we know about the game's camera
// ---------------------------------------------------------------------------
// The engine (HPL3) has a camera class. Whenever the camera has moved or
// turned, the engine calls one internal function to rebuild the camera's
// "view matrix": a grid of 16 numbers that describes where the camera is and
// which way it faces. Everything that gets drawn goes through that matrix.
//
// We let the game build its matrix as usual, then combine it with where the
// headset is. The game still controls where the player stands and which way
// the body faces; the headset adds the head movement on top.
//
// Offsets of the fields this mod uses inside the engine's camera object:
static const int kCameraPosition = 0x10;    // 3 numbers: x, y, z
static const int kCameraFieldOfView = 0x28; // full up-and-down viewing angle, in radians
static const int kCameraAspect = 0x2C;      // width of the view divided by its height
static const int kCameraProjectionType = 0x40; // 0 = normal perspective view
static const int kCameraRotateMode = 0x70;  // 0 = the camera is aimed using the pitch/yaw/roll angles
static const int kCameraPitch = 0x48;       // looking up/down, in radians
static const int kCameraRoll = 0x50;        // tilting sideways, in radians
static const int kCameraYaw = 0x4C;         // turning left/right, in radians
static const int kCameraViewMatrix = 0x78;  // 16 numbers: world -> camera
static const int kCameraWorldMatrix = 0xB8; // 16 numbers: camera -> world (the opposite direction)
static const int kCameraProjection = 0xF8;  // 16 numbers: how the camera's view is flattened onto the screen
static const int kCameraViewDirty = 0x6F1;       // 1 = the view matrix needs rebuilding
static const int kCameraProjectionDirty = 0x6F2; // 1 = the projection needs rebuilding
static const int kCameraOtherDirty = 0x6F3;      // 3 bytes, 1 = things built from the view need rebuilding
static const int kCameraFrustumDirty = 0x6F4;    // (the 2nd of those: "what is visible", used for drawing)

// The first instructions of the engine's "rebuild the view matrix" function.
static const char* kUpdateViewMatrixPattern =
    "4C 8B DC 49 89 5B 08 49 89 73 10 49 89 7B 18 55 49 8D 6B E8 "
    "48 81 EC 10 01 00 00 0F 10 05 ?? ?? ?? ?? 8B 41 70 48 8B F9 0F 11 41 78";

typedef void (*UpdateViewMatrixFunction)(void* camera);
static UpdateViewMatrixFunction g_realUpdateViewMatrix = nullptr;

// The first instructions of the engine's "give me the projection matrix"
// function. It rebuilds the matrix from the field of view when needed.
static const char* kGetProjectionMatrixPattern =
    "48 89 5C 24 08 57 48 81 EC 80 00 00 00 80 B9 F2 06 00 00 00 "
    "48 8D B9 F8 00 00 00 48 8B D9 74 75 83 79 40 00";

typedef const float* (*GetProjectionMatrixFunction)(void* camera);
static GetProjectionMatrixFunction g_realGetProjectionMatrix = nullptr;

// From the camera's projection matrix: how wide and tall its view is.
static volatile float g_projectionAcross = 0; // 0 = not known yet
static volatile float g_projectionUpDown = 0;

bool CameraGetFieldOfView(float& tanHalfWidth, float& tanHalfHeight)
{
    float across = g_projectionAcross;
    float upDown = g_projectionUpDown;
    if (across < 0.01f || upDown < 0.01f) return false;
    tanHalfWidth = 1.0f / across;
    tanHalfHeight = 1.0f / upDown;
    return true;
}

// ---------------------------------------------------------------------------
// Field of view override
// ---------------------------------------------------------------------------
// A headset's lenses show a much taller, squarer view than a monitor does. So
// while VR is running we swap the camera's field of view and shape for ones
// that cover what the headset can show. We change the camera's own settings
// (rather than editing the finished matrix) so that everything in the engine
// that depends on them, such as deciding what is visible, stays consistent.

static volatile float g_overrideFieldOfView = 0; // 0 = no override
static volatile float g_overrideAspect = 0;

void CameraSetFieldOfViewOverride(float verticalRadians, float aspect)
{
    g_overrideFieldOfView = verticalRadians;
    g_overrideAspect = aspect;
}

// ---- Cutscenes on a screen ----
// When the body script reports that a cutscene is playing (third number of
// its roll 7020 news), the game is left to draw its picture exactly as it
// would on a monitor: its own field of view, no head movement, no hands.
// vr.cpp then shows that picture on a large flat screen in a dark room.
static bool g_cutscenePlaying = false;
static unsigned long long g_cutsceneNewsTime = 0;

// The shape of the game's window (width divided by height): the shape the
// cutscene picture must be drawn in.
static float g_windowAspect = 0.0f;

void CameraSetWindowAspect(float aspect)
{
    g_windowAspect = aspect;
}

bool CameraTheatreWanted()
{
    return g_cutscenePlaying && g_cutsceneNewsTime != 0 && GetTickCount64() - g_cutsceneNewsTime < 400;
}

// The screen as shown: switched by vr.cpp while the view is black, so that
// the change to and from the screen is never seen
// (UpdateTheatreChange). Everything that draws or moves the camera goes by
// this one.
static volatile bool g_theatreShown = false;

bool CameraTheatre()
{
    return g_theatreShown;
}

void CameraSetTheatreShown(bool shown)
{
    g_theatreShown = shown;
}

// What each camera's field of view and shape were before the override, so
// they can be put back for the screen.
struct FovMemory
{
    const void* camera;
    float fieldOfView, aspect;               // the game's own values
    float appliedFieldOfView, appliedAspect; // what this mod last put in their place
    bool known;
};
static FovMemory g_fovMemory[8];
static int g_nextFovMemory = 0;

static FovMemory& FovMemoryFor(const void* camera)
{
    for (int i = 0; i < 8; ++i)
    {
        if (g_fovMemory[i].camera == camera) return g_fovMemory[i];
    }
    FovMemory& slot = g_fovMemory[g_nextFovMemory];
    g_nextFovMemory = (g_nextFovMemory + 1) % 8;
    slot.camera = camera;
    slot.known = false;
    slot.appliedFieldOfView = 0.0f;
    slot.appliedAspect = 0.0f;
    return slot;
}

// ---- The hand poses tool's camera on a hand ----
// See CameraSetHandView. While it is shown, the view is built from it
// instead of from the head (HookedUpdateViewMatrix), with its own field of
// view in the window's shape (ApplyFieldOfViewOverride).
struct HandCamera
{
    bool on;
    int hand;
    float direction[3];
    float distance;
    float fieldOfView; // radians, up and down
};
static HandCamera g_handView = {};
static SRWLOCK g_handViewLock = SRWLOCK_INIT;
static volatile LONG g_handViewChanges = 0; // goes up with each change (the view is then built again: see EnsureFreshView)

void CameraSetHandView(bool on, int hand, const float direction[3], float distance, float fieldOfViewDegrees)
{
    HandCamera view = {};
    view.on = on && hand >= 0 && hand <= 1;
    view.hand = hand;
    float length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length < 0.001f) view.on = false;
    else
        for (int i = 0; i < 3; ++i) view.direction[i] = direction[i] / length;
    view.distance = distance < 0.05f ? 0.05f : (distance > 5.0f ? 5.0f : distance);
    float degrees = fieldOfViewDegrees < 10.0f ? 10.0f : (fieldOfViewDegrees > 120.0f ? 120.0f : fieldOfViewDegrees);
    view.fieldOfView = degrees * 3.14159265f / 180.0f;
    if (!view.on) view = HandCamera{}; // every "off" state compares equal

    AcquireSRWLockExclusive(&g_handViewLock);
    bool changed = memcmp(&g_handView, &view, sizeof(HandCamera)) != 0;
    bool wasOn = g_handView.on;
    g_handView = view;
    ReleaseSRWLockExclusive(&g_handViewLock);
    if (!changed) return;
    InterlockedIncrement(&g_handViewChanges);
    if (wasOn != view.on)
        Log("CAMERA: the hand poses tool's camera on the hand is %s.", view.on ? "on (the PC's screen shows it while the headset is off)" : "off");
}

// Where the camera on the hand is now, measured as the head's pose is, and
// its field of view (radians). False if it is not shown: switched off, the
// headset on, or that hand not known.
static bool HandViewPose(HeadPose& pose, float& fieldOfView)
{
    if (!VrHeadsetIsOff()) return false;
    AcquireSRWLockShared(&g_handViewLock);
    HandCamera view = g_handView;
    ReleaseSRWLockShared(&g_handViewLock);
    if (!view.on) return false;
    HeadPose palm;
    if (!VrGetGripHandPose(view.hand, palm)) return false;

    // The camera's own backwards direction: from the palm towards the camera
    // (the palm's own directions are its rotation's columns).
    const float* r = palm.rotation;
    float z[3];
    for (int i = 0; i < 3; ++i) z[i] = r[i * 3 + 0] * view.direction[0] + r[i * 3 + 1] * view.direction[1] + r[i * 3 + 2] * view.direction[2];
    // Its right: level (up crossed with backwards); looking almost straight
    // up or down, the palm's own right instead.
    float x[3] = { z[2], 0.0f, -z[0] };
    float length = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (length < 0.1f)
    {
        float along = r[0] * z[0] + r[3] * z[1] + r[6] * z[2];
        for (int i = 0; i < 3; ++i) x[i] = r[i * 3 + 0] - along * z[i];
        length = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (length < 0.001f) return false;
    }
    for (int i = 0; i < 3; ++i) x[i] /= length;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] }; // (backwards crossed with right)

    // In the palm's units: real meters times the world scale.
    float away = view.distance * GetSettings().worldScale;
    for (int i = 0; i < 3; ++i)
    {
        pose.rotation[i * 3 + 0] = x[i];
        pose.rotation[i * 3 + 1] = y[i];
        pose.rotation[i * 3 + 2] = z[i];
        pose.position[i] = palm.position[i] + z[i] * away;
    }
    fieldOfView = view.fieldOfView;
    return true;
}

static void ApplyFieldOfViewOverride(void* camera)
{
    float wantedFieldOfView = g_overrideFieldOfView;
    float wantedAspect = g_overrideAspect;
    // The camera on a hand: its own field of view, in the window's shape
    // (the PC's screen shows it as it is).
    HeadPose handPose;
    float handFieldOfView = 0.0f;
    if (HandViewPose(handPose, handFieldOfView))
    {
        wantedFieldOfView = handFieldOfView;
        wantedAspect = g_windowAspect > 0.2f ? g_windowAspect : 16.0f / 9.0f;
    }
    if (wantedFieldOfView <= 0 || wantedAspect <= 0) return;

    // Leave special cameras (flat, non-perspective ones) alone.
    if (*(int*)((char*)camera + kCameraProjectionType) != 0) return;

    float* fieldOfView = (float*)((char*)camera + kCameraFieldOfView);
    float* aspect = (float*)((char*)camera + kCameraAspect);
    FovMemory& memory = FovMemoryFor(camera);
    bool overridden = *fieldOfView == wantedFieldOfView && *aspect == wantedAspect;

    // Are the values in the camera ones this mod put there (now or earlier,
    // when the headset reported slightly different angles)? Those must never
    // be taken for the game's own, or cutscenes on the screen come out
    // stretched.
    bool ours = overridden || (memory.appliedAspect > 0.0f && *fieldOfView == memory.appliedFieldOfView &&
                               *aspect == memory.appliedAspect);

    if (CameraTheatre())
    {
        // The game's own up-and-down angle (if this mod's is in place), and
        // the window's shape, so that the picture is not stretched.
        float theatreFieldOfView = *fieldOfView;
        if (ours) theatreFieldOfView = memory.known ? memory.fieldOfView : 1.0f;
        float theatreAspect = g_windowAspect > 0.2f ? g_windowAspect : (memory.known ? memory.aspect : 16.0f / 9.0f);
        if (*fieldOfView == theatreFieldOfView && *aspect == theatreAspect) return;
        *fieldOfView = theatreFieldOfView;
        *aspect = theatreAspect;
    }
    else
    {
        if (overridden) return;
        if (!ours)
        {
            memory.fieldOfView = *fieldOfView; // whatever the game has set
            memory.aspect = *aspect;
            memory.known = true;
        }
        *fieldOfView = wantedFieldOfView;
        *aspect = wantedAspect;
        memory.appliedFieldOfView = wantedFieldOfView;
        memory.appliedAspect = wantedAspect;
    }

    // Tell the engine these things are out of date, the same way its own
    // "set field of view" function does.
    *((unsigned char*)camera + kCameraProjectionDirty) = 1;
    *((unsigned char*)camera + kCameraFrustumDirty) = 1;
    *((unsigned char*)camera + kCameraFrustumDirty + 1) = 1;
}

// ---------------------------------------------------------------------------
// Keeping every frame fresh
// ---------------------------------------------------------------------------
// The engine only rebuilds a camera's view when the game has moved the camera.
// But in VR the view must change on EVERY frame, because the eyes take turns
// and the head never stops moving. If a frame reuses the previous view, the
// wrong eye's picture ends up in front of an eye and the two pictures cannot
// merge into 3D.
//
// So the first time each frame that the engine asks a camera for anything,
// we mark its view as out of date, which makes the engine rebuild it (and our
// view hook then adds the current eye's position).

struct CameraFrame
{
    const void* camera;        // only compared, never used to reach into memory
    unsigned long long serial; // the headset frame this camera's view was last rebuilt for

    // Where the game had this camera when the left eye's picture was drawn.
    // The right eye's picture, one frame later, is drawn from the same place.
    unsigned long long pairSerial; // the left-eye frame these were noted in (0 = none)
    float pairPosition[3];
    float pairYaw;

    LONG handViewChanges; // the camera on a hand as this camera's view was last rebuilt for it (see g_handViewChanges)
};
static CameraFrame g_cameraFrames[8];
static int g_nextCameraFrame = 0;

static unsigned long long g_statForcedRebuilds = 0;
static unsigned long long g_statNaturalRebuilds = 0;

// Returns the entry for this camera, creating one if needed.
static CameraFrame& CameraFrameFor(const void* camera)
{
    for (int i = 0; i < 8; ++i)
    {
        if (g_cameraFrames[i].camera == camera) return g_cameraFrames[i];
    }
    CameraFrame& slot = g_cameraFrames[g_nextCameraFrame];
    g_nextCameraFrame = (g_nextCameraFrame + 1) % 8;
    slot.camera = camera;
    slot.serial = 0;
    slot.pairSerial = 0;
    slot.handViewChanges = 0;
    return slot;
}

static void EnsureFreshView(void* camera)
{
    unsigned long long serial = VrGetFrameSerial();
    if (serial == 0)
    {
        // VR not active, or the headset off. The camera on a hand is followed
        // all the same: the view is built again when it changes (the game
        // builds it again only when it has moved its camera itself).
        LONG changes = g_handViewChanges;
        if (changes == 0 || *(int*)((char*)camera + kCameraProjectionType) != 0) return;
        CameraFrame& entry = CameraFrameFor(camera);
        if (entry.handViewChanges == changes) return;
        entry.handViewChanges = changes;
        unsigned char* flags = (unsigned char*)camera;
        flags[kCameraViewDirty] = 1;
        flags[kCameraOtherDirty + 0] = 1;
        flags[kCameraOtherDirty + 1] = 1;
        flags[kCameraOtherDirty + 2] = 1;
        return;
    }

    CameraFrame& entry = CameraFrameFor(camera);
    if (entry.serial == serial) return; // already rebuilt for this frame

    // Mark the view, and the things built from it, as out of date. This is
    // what the engine's own "set pitch" function does.
    unsigned char* flags = (unsigned char*)camera;
    flags[kCameraViewDirty] = 1;
    flags[kCameraOtherDirty + 0] = 1;
    flags[kCameraOtherDirty + 1] = 1;
    flags[kCameraOtherDirty + 2] = 1;

    entry.serial = serial;
    ++g_statForcedRebuilds;
}

// The engine's "give me the view matrix" and "give me what is visible" functions.
static const char* kGetViewMatrixPattern =
    "40 53 48 83 EC 20 80 B9 F1 06 00 00 00 48 8B D9 74 0F E8 ?? ?? ?? ?? 48 8D 43 78";
static const char* kGetFrustumPattern =
    "40 55 48 83 EC 70 80 B9 F4 06 00 00 00 48 8B E9 0F 84";

typedef const float* (*GetViewMatrixFunction)(void* camera);
typedef void* (*GetFrustumFunction)(void* camera);
static GetViewMatrixFunction g_realGetViewMatrix = nullptr;
static GetFrustumFunction g_realGetFrustum = nullptr;

static const float* HookedGetViewMatrix(void* camera)
{
    EnsureFreshView(camera);
    return g_realGetViewMatrix(camera);
}

static void* HookedGetFrustum(void* camera)
{
    EnsureFreshView(camera);
    return g_realGetFrustum(camera);
}

static void EnsureFreshView(void* camera);

// Runs every time the engine asks a camera for its projection matrix.
static const float* HookedGetProjectionMatrix(void* camera)
{
    EnsureFreshView(camera);
    ApplyFieldOfViewOverride(camera);
    const float* projection = g_realGetProjectionMatrix(camera);

    // Note how wide this camera really sees now (numbers 0 and 5 of the
    // matrix), so the headset can be told exactly what the picture covers.
    if (*(int*)((char*)camera + kCameraProjectionType) == 0)
    {
        g_projectionAcross = projection[0];
        g_projectionUpDown = projection[5];
    }
    return projection;
}

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------
// The engine stores a matrix as 16 numbers, row after row. The last number of
// each of the first three rows is the position part.

static void Multiply(const float* a, const float* b, float* result)
{
    float temp[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            temp[row * 4 + column] =
                a[row * 4 + 0] * b[0 * 4 + column] +
                a[row * 4 + 1] * b[1 * 4 + column] +
                a[row * 4 + 2] * b[2 * 4 + column] +
                a[row * 4 + 3] * b[3 * 4 + column];
        }
    }
    for (int i = 0; i < 16; ++i) result[i] = temp[i];
}

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------

static const void* g_seenCameras[8]; // only compared, never used to reach into memory
static int g_seenCameraCount = 0;
static unsigned long long g_lastCameraLogTime = 0;
static int g_cameraLogsLeft = 4;

static void LogCamera(void* camera, const float* viewBefore, bool headApplied)
{
    const float* position = (const float*)((char*)camera + kCameraPosition);
    float pitch = *(const float*)((char*)camera + kCameraPitch);
    float yaw = *(const float*)((char*)camera + kCameraYaw);

    // Note each different camera the first time we see it.
    bool isNew = true;
    for (int i = 0; i < g_seenCameraCount; ++i)
    {
        if (g_seenCameras[i] == camera) isNew = false;
    }
    if (isNew && g_seenCameraCount < 8)
    {
        g_seenCameras[g_seenCameraCount++] = camera;
        Log("CAMERA: new camera #%d seen at %p, position %.2f %.2f %.2f",
            g_seenCameraCount, camera, position[0], position[1], position[2]);
    }

    // Every 5 seconds (six times), report how often the game rebuilt the view
    // by itself and how often we had to make it.
    static unsigned long long lastStatsTime = 0;
    static int statsLeft = 6;
    unsigned long long statsNow = GetTickCount64();
    if (lastStatsTime == 0) lastStatsTime = statsNow;
    if (statsLeft > 0 && statsNow - lastStatsTime >= 5000)
    {
        lastStatsTime = statsNow;
        --statsLeft;
        Log("CAMERA: in the last 5 seconds the game rebuilt the view by itself for %llu frames; we forced it for %llu frames.",
            g_statNaturalRebuilds, g_statForcedRebuilds);
        g_statNaturalRebuilds = 0;
        g_statForcedRebuilds = 0;
    }

    // Every 2 seconds (a few times), log one camera in detail for checking.
    unsigned long long now = GetTickCount64();
    if (g_cameraLogsLeft > 0 && now - g_lastCameraLogTime >= 2000)
    {
        g_lastCameraLogTime = now;
        --g_cameraLogsLeft;
        Log("CAMERA %p: position %.2f %.2f %.2f | game pitch %+.1f yaw %+.1f deg | head %s",
            camera, position[0], position[1], position[2],
            pitch * 57.29578f, yaw * 57.29578f, headApplied ? "applied" : "not applied");
        const float* projection = (const float*)((char*)camera + kCameraProjection);
        Log("        projection: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] | fov setting %.3f, aspect %.3f",
            projection[0], projection[1], projection[2], projection[3],
            projection[4], projection[5], projection[6], projection[7],
            *(const float*)((char*)camera + 0x28), *(const float*)((char*)camera + 0x2C));
    }
}

// Details of the most recent view that was built, kept for the capture tool (capture.cpp).
struct LastView
{
    bool valid;
    bool headApplied;
    unsigned long long serial;
    const void* camera;
    float gamePosition[3];
    float gamePitch, gameYaw;
    float fieldOfView, aspect;
    HeadPose head;
    float gameView[16];  // the game's own view matrix
    float finalView[16]; // after adding the head/eye
    unsigned long long rebuildsThisSerial;
};
static LastView g_lastView = {};

bool CameraLastViewNumbers(float gamePosition[3], float eyePosition[3], float& gameYaw, unsigned long long& serial)
{
    const LastView& v = g_lastView;
    if (!v.valid || !v.headApplied) return false;
    for (int i = 0; i < 3; ++i) gamePosition[i] = v.gamePosition[i];
    // The eye's place in the world: the final view (world -> eye) undone,
    // -(its turn, flipped) times its position part.
    const float* m = v.finalView;
    for (int i = 0; i < 3; ++i)
        eyePosition[i] = -(m[0 * 4 + i] * m[3] + m[1 * 4 + i] * m[7] + m[2 * 4 + i] * m[11]);
    gameYaw = v.gameYaw;
    serial = v.serial;
    return true;
}

bool CameraWorldToPlayer(const float world[3], float player[3])
{
    const LastView& v = g_lastView;
    if (!v.valid || !v.headApplied || CameraTheatre()) return false;
    unsigned long long now = VrGetFrameSerial();
    if (now > v.serial + 3) return false; // (no world drawn lately)
    // The game's own view before the head was put in (world -> the game's
    // camera, level): the same space the head and hands are measured in
    // (ApplyPose puts "world = that camera's place x the pose").
    const float* m = v.gameView;
    for (int i = 0; i < 3; ++i)
        player[i] = m[i * 4 + 0] * world[0] + m[i * 4 + 1] * world[1] + m[i * 4 + 2] * world[2] + m[i * 4 + 3];
    return true;
}

bool CameraLastEyeFacing(float& degrees)
{
    const LastView& v = g_lastView;
    if (!v.valid || !v.headApplied) return false;
    // The final view's third row is the eye's backwards in the world.
    const float* m = v.finalView;
    if (m[8] * m[8] + m[10] * m[10] < 0.01f) return false; // (looking nearly straight up or down)
    degrees = atan2f(-m[8], -m[10]) * 57.29578f;
    return true;
}

void CameraLogLastView(const char* label)
{
    const LastView& v = g_lastView;
    if (!v.valid)
    {
        Log("%s: no camera view has been built yet.", label);
        return;
    }
    Log("%s", label);
    Log("    camera %p, view built for headset frame %llu (%llu rebuilds in that frame), head %s",
        v.camera, v.serial, v.rebuildsThisSerial, v.headApplied ? "applied" : "NOT applied");
    Log("    game camera: position %.3f %.3f %.3f | pitch %+.1f yaw %+.1f deg | fov %.3f aspect %.3f",
        v.gamePosition[0], v.gamePosition[1], v.gamePosition[2],
        v.gamePitch * 57.29578f, v.gameYaw * 57.29578f, v.fieldOfView, v.aspect);
    Log("    eye relative to centre point (metres): x %+.4f  y %+.4f  z %+.4f",
        v.head.position[0], v.head.position[1], v.head.position[2]);
    Log("    eye rotation rows: [%.3f %.3f %.3f] [%.3f %.3f %.3f] [%.3f %.3f %.3f]",
        v.head.rotation[0], v.head.rotation[1], v.head.rotation[2],
        v.head.rotation[3], v.head.rotation[4], v.head.rotation[5],
        v.head.rotation[6], v.head.rotation[7], v.head.rotation[8]);
    Log("    game view:  [%.3f %.3f %.3f %.4f] [%.3f %.3f %.3f %.4f] [%.3f %.3f %.3f %.4f]",
        v.gameView[0], v.gameView[1], v.gameView[2], v.gameView[3],
        v.gameView[4], v.gameView[5], v.gameView[6], v.gameView[7],
        v.gameView[8], v.gameView[9], v.gameView[10], v.gameView[11]);
    Log("    final view: [%.3f %.3f %.3f %.4f] [%.3f %.3f %.3f %.4f] [%.3f %.3f %.3f %.4f]",
        v.finalView[0], v.finalView[1], v.finalView[2], v.finalView[3],
        v.finalView[4], v.finalView[5], v.finalView[6], v.finalView[7],
        v.finalView[8], v.finalView[9], v.finalView[10], v.finalView[11]);
}

// ---------------------------------------------------------------------------
// Answering questions from the game's scripts
// ---------------------------------------------------------------------------
// The game's grab, body and arm code are scripts. Besides the mod's own script
// functions (cVR_..., scriptfuncs.cpp), a script can reach this DLL through
// the camera: it can set the camera's roll angle and then read the camera's
// matrix, and both of those pass through code we have hooked.
//
// So the mod's scripts ask questions by setting an impossible roll angle
// (thousands of radians), and we answer by writing the reply into the matrix
// it is about to read. The script then puts the real roll angle back.
//
//   roll 7001: "where is the grabbing hand?"  -> the matrix becomes the hand's
//              position and rotation in the game's world
//   roll 7002: "is a hand grabbing?"          -> the matrix's position becomes
//              (12345, 1 or 0, which hand)
//   roll 7003: "where is the pointing hand?"  -> as 7001, but for the hand
//              used to point at things (works without squeezing)
//   roll 7004: "is a hand pointing?"          -> as 7002
//   roll 7005: "where is the head?"           -> as 7001, but for the middle
//              of the head (used to keep the character's body under it)
//   roll 7006: "is the head being tracked?"   -> as 7002, but the third number
//              is how many times bigger the body should be drawn
//   roll 7007: "where does the game itself have the camera?" -> the camera's
//              own matrix, level, without the head's movement added
//   roll 7010: "which palms can be followed?" -> the matrix's position becomes
//              (12345, 1 for left + 2 for right + 4 to ask for log notes,
//              how many times bigger the arms should be). The middle number is 0 when the game's arms
//              are switched off with the F4 key.
//   roll 7011, 7012: "where is the left / right palm?" -> as 7001
//   roll 7013: "how far are the fingers closed?" -> the matrix's position
//              becomes (left hand, right hand, 0), each from 0 (open) to 1
static const float kAskHeadMatrix = 7005.0f;
static const float kAskHeadStatus = 7006.0f;
static const float kAskLevelCamera = 7007.0f;
static const float kAskPalmStatus = 7010.0f;
static const float kAskLeftPalm = 7011.0f;
static const float kAskRightPalm = 7012.0f;
static const float kAskFingers = 7013.0f;
//   roll 7014: "what are the hands doing?" -> the matrix's position becomes
//              (12345, which hands hold a loose object by themselves: 1 left +
//              2 right, 1 while the ammo check is open + 2 while the left hand
//              steadies the gun)
static const float kAskHandUse = 7014.0f;

static unsigned long long g_lastWorldSerial = 0;
unsigned long long CameraLastWorldSerial()
{
    return g_lastWorldSerial;
}

// When scripts last asked anything, and when the arm script last asked.
static unsigned long long g_scriptAskedTime = 0;
static unsigned long long g_handScriptAskedTime = 0;

bool CameraScriptsBusy(unsigned milliseconds)
{
    unsigned long long asked = g_scriptAskedTime;
    return asked != 0 && GetTickCount64() - asked < milliseconds;
}

// What the body script last reported about the things in the hands and the
// game's fade to black (roll 7026).
static int g_rightItemKind = 0, g_leftItemKind = 0;
static float g_fadeAlpha = 0.0f;
static unsigned long long g_itemNewsTime = 0;

// Whether the game's loading screen is up, as the menu script last said
// (roll 7027). Forgotten after a second and a half without news.
static bool g_loadingScreen = false;
static unsigned long long g_loadingNewsTime = 0;

bool CameraLoadingScreen()
{
    return g_loadingScreen && g_loadingNewsTime != 0 && GetTickCount64() - g_loadingNewsTime < 1500;
}

int CameraRightItemKind()
{
    return (g_itemNewsTime != 0 && GetTickCount64() - g_itemNewsTime < 400) ? g_rightItemKind : kItemNone;
}

int CameraLeftItemKind()
{
    return (g_itemNewsTime != 0 && GetTickCount64() - g_itemNewsTime < 400) ? g_leftItemKind : kItemNone;
}

// What each hand touches (roll 7083; see CameraTouchKind in camera.h).
static int g_touchKind[2] = { kTouchNothing, kTouchNothing };
static unsigned long long g_touchNewsTime = 0;

int CameraTouchKind(int hand)
{
    if (hand < 0 || hand > 1 || g_touchNewsTime == 0 || GetTickCount64() - g_touchNewsTime >= 400) return kTouchNothing;
    return g_touchKind[hand];
}

// The game's fade as the engine itself has it, read each frame as the engine
// draws it: its fade-drawing function (EffectHandler.cpp) is given the fade's
// own fields: +0x08 whether it is on, +0x10 how dark it is now. The body
// script's news (above) pauses for a moment as a map starts (for most of a
// second, and there is none before the body script's first update in a new
// game). Going by the news alone, the eyes would briefly see the world the
// game has already faded out, before a map's opening cutscene. The pattern's
// first 28 bytes match once in the game; the two compares after them are the
// ones that read those two fields.
static const char* kFadeDrawPattern =
    "4C 8B DC 55 41 57 49 8D 6B C8 48 81 EC 38 01 00 00 41 0F 29 7B B8 4C 8B F9 0F 57 FF "
    "0F 2F 79 10 0F 83 ?? ?? ?? ?? 80 79 08 00 0F 84";
typedef void (*FadeDrawFunction)(void* fade, float frameTime);
static FadeDrawFunction g_realFadeDraw = nullptr;
static volatile float g_engineFade = 0.0f;
static volatile unsigned long long g_engineFadeTime = 0;

static void HookedFadeDraw(void* fade, float frameTime)
{
    const char* fields = (const char*)fade;
    float alpha = *(const float*)(fields + 0x10);
    bool on = fields[0x08] != 0;
    g_engineFade = (on && alpha > 0.0f) ? (alpha > 1.0f ? 1.0f : alpha) : 0.0f; // (also catches "not a number")
    g_engineFadeTime = GetTickCount64();
    g_realFadeDraw(fade, frameTime);
}

bool CameraInstallFadeHook()
{
    HMODULE game = GetModuleHandleW(nullptr);
    int matches = 0;
    void* target = FindPattern(game, kFadeDrawPattern, &matches);
    if (matches != 1)
    {
        Log("CAMERA ERROR: expected to find the game's fade-drawing function exactly once, found it %d times (the fade goes by the scripts' news only).", matches);
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("CAMERA ERROR: could not start the hooking library for the fade: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(target, (LPVOID)HookedFadeDraw, (LPVOID*)&g_realFadeDraw);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("CAMERA ERROR: could not hook the game's fade-drawing function: %s", MH_StatusToString(status));
        return false;
    }
    Log("CAMERA: hooked the game's fade-drawing function at exe+0x%llX", (unsigned long long)((char*)target - (char*)game));
    return true;
}

float CameraFadeAlpha()
{
    float alpha = 0.0f;
    if (g_itemNewsTime != 0 && GetTickCount64() - g_itemNewsTime < 400)
    {
        alpha = g_fadeAlpha;
        if (!(alpha > 0.0f)) alpha = 0.0f; // also catches "not a number"
        if (alpha > 1.0f) alpha = 1.0f;
    }
    // And the engine's own fade (HookedFadeDraw), when it has drawn it in the
    // last quarter second. The news also carries the mod's own darkenings,
    // so the darker of the two wins.
    unsigned long long engineTime = g_engineFadeTime;
    if (engineTime != 0 && GetTickCount64() - engineTime < 250)
    {
        float engine = g_engineFade;
        if (engine > alpha) alpha = engine;
    }
    return alpha;
}

// The player's Gamma (the game's options), with the engine's Brightness and
// Contrast. The engine applies them in its very last step, as it draws the
// finished frame into the window through its image correction shader
// (base_image_correction_frag: the colour to the power 1 / gamma, then the
// contrast about the middle grey, then times the brightness). The headset's
// pictures are taken before that step, so the mod applies the same
// correction to them itself (CameraPictureCorrection). The values are read
// from the engine's graphics each time that step runs: brightness at +0x1F4,
// gamma at +0x1F8, contrast at +0x1FC. The pattern matches once in the game;
// its end is the division that turns the gamma into the shader's power.
static const char* kImageCorrectionPattern =
    "40 53 57 48 83 EC 78 48 8B 05 ?? ?? ?? ?? 48 8B F9 33 D2 B9 40 8D 00 00 FF 10 B9 B9 8D 00 00 FF 15 ?? ?? ?? ?? "
    "44 8B 47 10 48 8D 5F 10 44 8B 4F 14 33 D2 33 C9 FF 15 ?? ?? ?? ?? 48 8B 8F D8 00 00 00 48 85 C9 0F 84 ?? ?? ?? ?? "
    "F3 0F 10 15 ?? ?? ?? ?? 33 D2 F3 0F 5E 97 F8 01 00 00";
typedef void (*ImageCorrectionDrawFunction)(void* graphics);
static ImageCorrectionDrawFunction g_realImageCorrectionDraw = nullptr;
static volatile float g_pictureGamma = 1.0f, g_pictureBrightness = 1.0f, g_pictureContrast = 1.0f;

// A value the engine holds, if it is sensible (also catches "not a number").
static void TakeCorrection(const char* fields, int offset, volatile float& into)
{
    float value = *(const float*)(fields + offset);
    if (value >= 0.05f && value <= 10.0f) into = value;
}

static void HookedImageCorrectionDraw(void* graphics)
{
    const char* fields = (const char*)graphics;
    float gammaWas = g_pictureGamma;
    TakeCorrection(fields, 0x1F4, g_pictureBrightness);
    TakeCorrection(fields, 0x1F8, g_pictureGamma);
    TakeCorrection(fields, 0x1FC, g_pictureContrast);
    static int logsLeft = 20;
    if (g_pictureGamma != gammaWas && logsLeft > 0)
    {
        --logsLeft;
        Log("CAMERA: the game's gamma is %.2f (brightness %.2f, contrast %.2f): the headset's pictures get the same correction.",
            g_pictureGamma, g_pictureBrightness, g_pictureContrast);
    }
    g_realImageCorrectionDraw(graphics);
}

bool CameraInstallPictureCorrectionHook()
{
    HMODULE game = GetModuleHandleW(nullptr);
    int matches = 0;
    void* target = FindPattern(game, kImageCorrectionPattern, &matches);
    if (matches != 1)
    {
        Log("CAMERA ERROR: expected to find the game's image correction exactly once, found it %d times (the headset gets no gamma).", matches);
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("CAMERA ERROR: could not start the hooking library for the image correction: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(target, (LPVOID)HookedImageCorrectionDraw, (LPVOID*)&g_realImageCorrectionDraw);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("CAMERA ERROR: could not hook the game's image correction: %s", MH_StatusToString(status));
        return false;
    }
    Log("CAMERA: hooked the game's image correction (gamma) at exe+0x%llX", (unsigned long long)((char*)target - (char*)game));
    return true;
}

bool CameraPictureCorrection(float& gamma, float& brightness, float& contrast)
{
    gamma = g_pictureGamma;
    brightness = g_pictureBrightness;
    contrast = g_pictureContrast;
    return fabsf(gamma - 1.0f) > 0.001f || fabsf(brightness - 1.0f) > 0.001f || fabsf(contrast - 1.0f) > 0.001f;
}

// What the body script last reported about the gun (see roll 7020).
static bool g_gunHeld = false;
static bool g_shotgunHeld = false; // and whether it is the shotgun
static bool g_stateNormal = true;
static unsigned long long g_gunNewsTime = 0;

// Whether the game has the player crouched, as the body script last said
// (with its roll 7019 question).
static bool g_gameCrouched = false;
static unsigned long long g_gameCrouchedTime = 0;

bool CameraGameCrouched()
{
    return g_gameCrouched && g_gameCrouchedTime != 0 && GetTickCount64() - g_gameCrouchedTime < 400;
}

// The player camera's own left-right turn, as the game last had it (radians;
// the camera hook notes it while the headset is in use). vr.cpp follows its
// changes so that turning with the stick turns you about your own head.
static float g_lastGameYaw = 0.0f;
static unsigned long long g_lastGameYawTime = 0;

bool CameraGetGameYaw(float& yaw)
{
    yaw = g_lastGameYaw;
    return g_lastGameYawTime != 0 && GetTickCount64() - g_lastGameYawTime < 500;
}

bool CameraReadYaw(void* camera, float& yaw)
{
    if (camera == nullptr) return false;
    yaw = *(const float*)((const char*)camera + kCameraYaw);
    return yaw == yaw; // (not "not a number")
}

// What the revolver's script last reported (roll 7028).
static int g_revolverEvent = 0; // kept until vr.cpp takes it
static bool g_revolverHasSpent = false;
static int g_revolverLoaded = 0; // live bullets in the cylinder
static int g_revolverSpare = 0;  // bullets in the inventory
static unsigned long long g_revolverNewsTime = 0;
static bool g_shotgunCheckShown = false; // the shotgun's press-check shows its shells

int CameraTakeRevolverEvent()
{
    int happened = g_revolverEvent;
    g_revolverEvent = 0;
    return happened;
}

bool CameraRevolverScriptActive()
{
    return g_revolverNewsTime != 0 && GetTickCount64() - g_revolverNewsTime < 400;
}

bool CameraRevolverHasSpent()
{
    return CameraRevolverScriptActive() && g_revolverHasSpent;
}

bool CameraRevolverCounts(int& loaded, int& spare)
{
    loaded = g_revolverLoaded;
    spare = g_revolverSpare;
    return CameraRevolverScriptActive();
}

bool CameraShotgunCheckShown()
{
    return g_shotgunCheckShown && CameraRevolverScriptActive();
}

// When a script last reported that the game is paused (roll 7021).
static unsigned long long g_pausedNewsTime = 0;

bool CameraGamePaused()
{
    return g_pausedNewsTime != 0 && GetTickCount64() - g_pausedNewsTime < 400;
}

// When the body script last reported the game's inventory open (roll 7092);
// 0 once it reports it shut.
static unsigned long long g_inventoryNewsTime = 0;

bool CameraInventoryOpen()
{
    return g_inventoryNewsTime != 0 && GetTickCount64() - g_inventoryNewsTime < 400;
}

// The moves the item in the right hand takes part in, and that item's place
// in the game's item list, as the body script last reported them (roll 7093;
// see camera.h).
static int g_rightItemMoves = 0, g_rightMovesItem = -1;
static unsigned long long g_movesNewsTime = 0;

int CameraRightItemMoves(int* item)
{
    bool fresh = g_movesNewsTime != 0 && GetTickCount64() - g_movesNewsTime < 400;
    if (item != nullptr) *item = fresh ? g_rightMovesItem : -1;
    return fresh ? g_rightItemMoves : 0;
}

// When the gun script last reported that it is showing its own bullet model
// in the left hand (roll 7022).
static unsigned long long g_bulletModelTime = 0;

bool CameraBulletModelShown()
{
    return g_bulletModelTime != 0 && GetTickCount64() - g_bulletModelTime < 400;
}

// How strongly the game wants each of its screen effects shown (blood, red
// edges, dark edges), as the screen-effect script last reported (rolls 7023
// and 7024).
static float g_screenEffects[kScreenEffectCount] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static unsigned long long g_screenEffectsTime[3] = { 0, 0, 0 }; // one per message (three numbers each)

bool CameraGetScreenEffects(float strengths[kScreenEffectCount])
{
    unsigned long long now = GetTickCount64();
    bool any = false;
    for (int i = 0; i < kScreenEffectCount; ++i)
    {
        unsigned long long time = g_screenEffectsTime[i / 3];
        bool fresh = time != 0 && now - time < 400;
        float value = fresh ? g_screenEffects[i] : 0.0f;
        if (!(value > 0.0f)) value = 0.0f; // also catches "not a number"
        if (value > 1.0f) value = 1.0f;
        strengths[i] = value;
        any = any || value > 0.003f;
    }
    return any;
}

bool CameraGunHeld()
{
    return g_gunHeld && g_gunNewsTime != 0 && GetTickCount64() - g_gunNewsTime < 400;
}

bool CameraShotgunHeld()
{
    return CameraGunHeld() && g_shotgunHeld;
}

bool CameraGunNewsFresh()
{
    return g_gunNewsTime != 0 && GetTickCount64() - g_gunNewsTime < 400;
}

bool CameraPlayerStateNormal()
{
    // With no recent news, assume normal so that nothing waits forever.
    if (g_gunNewsTime == 0 || GetTickCount64() - g_gunNewsTime >= 400) return true;
    return g_stateNormal;
}

bool CameraScriptsSeen()
{
    return g_scriptAskedTime != 0;
}

bool CameraBodyNewsSeen()
{
    return g_gunNewsTime != 0;
}

bool CameraHandScriptBusy(unsigned milliseconds)
{
    unsigned long long asked = g_handScriptAskedTime;
    return asked != 0 && GetTickCount64() - asked < milliseconds;
}
static const float kAskHandMatrix = 7001.0f;
static const float kAskHandStatus = 7002.0f;
static const float kAskPointMatrix = 7003.0f;
static const float kAskPointStatus = 7004.0f;

// Combines the game's freshly built view and world matrices with a pose
// (head, eye or hand) measured from "straight ahead".
static void ApplyPose(void* camera, const HeadPose& pose)
{
    float* view = (float*)((char*)camera + kCameraViewMatrix);
    float* world = (float*)((char*)camera + kCameraWorldMatrix);

    const float* r = pose.rotation; // 3x3, row after row
    const float* t = pose.position;

    // "poseMatrix" places the pose relative to where the game's camera is.
    float poseMatrix[16] = {
        r[0], r[1], r[2], t[0],
        r[3], r[4], r[5], t[1],
        r[6], r[7], r[8], t[2],
        0,    0,    0,    1,
    };

    // Its opposite: for a pure rotation the opposite is the same grid
    // flipped along its diagonal, and the position part is turned back.
    float poseInverse[16] = {
        r[0], r[3], r[6], -(r[0] * t[0] + r[3] * t[1] + r[6] * t[2]),
        r[1], r[4], r[7], -(r[1] * t[0] + r[4] * t[1] + r[7] * t[2]),
        r[2], r[5], r[8], -(r[2] * t[0] + r[5] * t[1] + r[8] * t[2]),
        0,    0,    0,    1,
    };

    // world -> game camera -> pose
    Multiply(poseInverse, view, view);
    // pose -> game camera -> world
    Multiply(world, poseMatrix, world);
}

// Returns true if this rebuild was a question from a script (and answers it).
static bool AnswerScriptQuestion(void* camera)
{
    float* roll = (float*)((char*)camera + kCameraRoll);
    float question = *roll;
    // roll 7020: news from a script, in the camera's position (which the script
    // puts straight back): (1 if a gun is held ready in the right hand, 2 if it
    // is the shotgun; 1 if the player is in the game's normal state; 1 while a
    // cutscene plays).
    if (question >= 7019.5f && question < 7020.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        g_gunHeld = numbers[0] > 0.5f;
        g_shotgunHeld = numbers[0] > 1.5f;
        g_stateNormal = numbers[1] > 0.5f;
        g_gunNewsTime = GetTickCount64();
        if (g_cutscenePlaying != (numbers[2] > 0.5f))
            Log("CAMERA: the game reports that a cutscene %s.", numbers[2] > 0.5f ? "has started" : "is over");
        g_cutscenePlaying = numbers[2] > 0.5f;
        g_cutsceneNewsTime = g_gunNewsTime;

        static bool loggedGun = false;
        if (g_gunHeld && !loggedGun)
        {
            loggedGun = true;
            Log("CAMERA: the game reports a gun held in the right hand; gun controls are on.");
        }
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7021: news from the hint script, sent on every update while the
    // game is paused (a hint waiting for a button, the pause menu).
    if (question >= 7020.5f && question < 7021.5f)
    {
        if (g_pausedNewsTime == 0) Log("CAMERA: the game reports that it is paused; the last pictures stay up meanwhile.");
        g_pausedNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7022: news from the gun script: it is showing the game's own bullet
    // model in the left hand (so the mod's stand-in block is not needed).
    if (question >= 7021.5f && question < 7022.5f)
    {
        if (g_bulletModelTime == 0) Log("CAMERA: the gun script is showing the game's bullet model in the left hand.");
        g_bulletModelTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7023, 7024, 7025: news about the game's screen effects: how
    // strongly each should show, three numbers each, in the camera's position.
    //   7023 (Effect_Screen.hps): blood (major), blood (minor), blood (the older, even kind)
    //   7024 (Effect_Screen.hps): red edges (low health), dark edges, the gas mask's view
    //   7025 (Player_Custom.hps): red pulse at the edges when hurt, pale green
    //                             glow at the edges when healed, unused
    if (question >= 7022.5f && question < 7025.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int group = (int)(question + 0.5f) - 7023;
        for (int i = 0; i < 3; ++i) g_screenEffects[group * 3 + i] = numbers[i];
        if (g_screenEffectsTime[group] == 0)
            Log("CAMERA: screen effects are being reported (message %d); they are drawn across the eyes.", 7023 + group);
        g_screenEffectsTime[group] = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7026: news from the body script, in the camera's position: (what
    // the right hand holds, what the left hand holds, how far the game has
    // faded to black 0..1). The kinds are listed in camera.h.
    if (question >= 7025.5f && question < 7026.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int right = (int)(numbers[0] + 0.5f), left = (int)(numbers[1] + 0.5f);
        if (right != g_rightItemKind || left != g_leftItemKind)
        {
            static int logsLeft = 30;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("CAMERA: the game reports the hands hold: right %d, left %d (0 nothing special, 1 flashlight, 2 bandage or medkit, 3 grenade, "
                    "4 lighter, 5 bottle, 6 torch, 9 anything else).",
                    right, left);
            }
        }
        g_rightItemKind = right;
        g_leftItemKind = left;
        g_fadeAlpha = numbers[2];
        g_itemNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7092: news from the body script, in the camera's position: (is the
    // game's inventory open, 1 or 0; unused; unused). The inventory pauses the
    // game only with the difficulty's "game paused in inventory", so the
    // paused news alone cannot tell (without this, X would not drop the chosen
    // item).
    if (question >= 7091.5f && question < 7092.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        if (numbers[0] > 0.5f && g_inventoryNewsTime == 0) Log("CAMERA: the game reports its inventory open.");
        g_inventoryNewsTime = numbers[0] > 0.5f ? GetTickCount64() : 0;
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7093: news from the body script (the hands-on items' shared
    // moves), in the camera's position: (the moves the item in the
    // right hand takes part in, as flags (kMoveThrow...; camera.h), that
    // item's place in the game's item list or -1, unused). The flags come
    // from the scripts' item profiles (BunkerVR_ItemMoves).
    if (question >= 7092.5f && question < 7093.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int moves = (int)floorf(numbers[0] + 0.5f), item = (int)floorf(numbers[1] + 0.5f);
        if (moves < 0 || moves > 255) moves = 0;
        if (moves != g_rightItemMoves || item != g_rightMovesItem)
        {
            static int logsLeft = 30;
            if (logsLeft > 0 && moves != 0)
            {
                --logsLeft;
                Log("CAMERA: the game reports the right hand's item (%s) takes part in the moves %d (1 throw, 2 a still let-go puts it down).",
                    GripItemName(item), moves);
            }
        }
        g_rightItemMoves = moves;
        g_rightMovesItem = item;
        g_movesNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7094: "is the player walking with the stick?" -> (12345, 1 walking
    // or 0, unused). roll 7095: news, in the camera's position: (the turn the
    // body script is about to give the game's camera, radians, as the
    // camera's own turn counts it; unused; unused): the character turned under
    // the player, the view to stay as it is (VrAbsorbTurn). The script tells
    // before it turns, with nothing in between that rebuilds the view, so that
    // the turn is not first followed as one of the stick's (VrFollowTurn, at
    // the top of every rebuild). Both are used by VRDoWalkTurn in the body
    // script.
    if (question >= 7093.5f && question < 7095.5f)
    {
        int code = (int)(question + 0.5f);
        if (code == 7095)
        {
            const float* numbers = (const float*)((char*)camera + kCameraPosition);
            float turn = numbers[0];
            if (turn == turn && fabsf(turn) < 0.5f) VrAbsorbTurn(turn);
        }
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        if (code == 7094)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            world[7] = VrWalkingWithStick() ? 1.0f : 0.0f;
            world[11] = 0.0f;
        }
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7096: news from the body script (pulling things to the hand), in
    // the camera's position: (the left hand points at a thing it
    // may pull, 1 or 0; the right hand; unused). roll 7097: "how does each
    // hand stand with pulling?" -> (12345, left, right), each as VrPullState
    // (vr.h) has it.
    if (question >= 7095.5f && question < 7097.5f)
    {
        int code = (int)(question + 0.5f);
        if (code == 7096)
        {
            const float* numbers = (const float*)((char*)camera + kCameraPosition);
            VrPullTargetsNews(numbers[0] > 0.5f, numbers[1] > 0.5f);
        }
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        if (code == 7097)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            world[7] = (float)VrPullState(0);
            world[11] = (float)VrPullState(1);
        }
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7083: news from the body script, in the camera's position:
    // (what the left hand touches, what the right hand touches,
    // unused), each kTouchNothing, kTouchOwnHold, kTouchGameOnly or
    // kTouchHeldDoor (camera.h).
    if (question >= 7082.5f && question < 7083.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        for (int hand = 0; hand < 2; ++hand)
        {
            int kind = (int)(numbers[hand] + 0.5f);
            g_touchKind[hand] = (kind >= kTouchNothing && kind <= kTouchHeldDoor) ? kind : kTouchNothing;
        }
        g_touchNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7084: news from the body script, in the camera's position:
    // which item each hand holds (left, right, 0), as its place in
    // the game's item list, or -1. Rolls 7086 .. 7091: where the game places
    // the item in a hand, in the palm's terms (its middle point, its own z
    // direction, its own y direction; 7086 .. 7088 the left hand, 7089 ..
    // 7091 the right). See grips.h.
    if ((question >= 7083.5f && question < 7084.5f) || (question >= 7085.5f && question < 7091.5f))
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int code = (int)(question + 0.5f);
        if (code == 7084) GripsHeldNews(numbers);
        else GripsGamePlacingNews(code <= 7088 ? 0 : 1, (code - 7086) % 3, numbers);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7085: "how does this hand hold what it holds?":
    // in the camera's position (the hand: 0 left, 1 right; which part of the
    // answer: 0 to 7; which pose: 0 the item held, or 1 + a hand at work's
    // slot; see GripsAnswer in grips.h). The answer is in the matrix's
    // position.
    if (question >= 7084.5f && question < 7085.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int hand = (int)(numbers[0] + 0.5f);
        int part = (int)(numbers[1] + 0.5f);
        int which = (int)(numbers[2] + 0.5f);
        float answer[3] = { 0.0f, 0.0f, 0.0f };
        if (hand >= 0 && hand <= 1 && part >= 0 && part <= 7 && which >= 0 && which <= kGripSlotCount) GripsAnswer(hand, part, which, answer);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = answer[0];
        world[7] = answer[1];
        world[11] = answer[2];
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7027: news from the menu script, which also runs while a loading
    // screen is up: (1 if the game's loading screen is showing, unused,
    // unused). The game already draws the new level behind a loading screen
    // that waits for a button, and the eyes must stay black until it is gone.
    if (question >= 7026.5f && question < 7027.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        bool showing = numbers[0] > 0.5f;
        if (showing != g_loadingScreen)
            Log("CAMERA: the game reports that its loading screen is %s.", showing ? "up" : "gone");
        g_loadingScreen = showing;
        g_loadingNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7028: news from the revolver's script (Prop_Gun.hps), sent every
    // frame while the revolver is in the right hand, in the camera's position:
    // (something that just happened: 1 = a bullet was pushed into the cylinder,
    // 2 = the spent shells were tipped out, 3 = a bullet was refused because
    // spent shells are in the way or the cylinder is full; 1 if spent shells
    // are in the cylinder; how many live bullets are in it).
    if (question >= 7027.5f && question < 7028.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        // The shotgun's script adds 10 to the first number while its
        // press-check shows the shells (A held and the pump drawn back).
        int firstNumber = (int)(numbers[0] + 0.5f);
        g_shotgunCheckShown = firstNumber >= 10;
        int happened = firstNumber % 10;
        if (happened >= 1 && happened <= 3)
        {
            g_revolverEvent = happened;
            static int logsLeft = 12;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("CAMERA: the revolver's script reports: %s.",
                    happened == 1 ? "a bullet was pushed into the cylinder"
                                  : (happened == 2 ? "the spent shells were tipped out" : "a bullet was refused (spent shells in the way, or full)"));
            }
        }
        // Second number: 1 if spent shells are in the cylinder, plus ten
        // times the spare bullets in the inventory.
        int second = (int)(numbers[1] + 0.5f);
        g_revolverHasSpent = (second % 10) == 1;
        g_revolverSpare = second / 10;
        g_revolverLoaded = (int)(numbers[2] + 0.5f);
        if (g_revolverNewsTime == 0) Log("CAMERA: the revolver's script is looking after loading and emptying by hand.");
        g_revolverNewsTime = GetTickCount64();
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    // roll 7030: "where should the revolver's open and close animations
    //            start?" -> (12345, seconds into "open", seconds into "close").
    //            The game's reload state asks as it opens the revolver.
    // Roll 7029 is not used: like any roll the mod does not know, it is left
    // to the game.
    if (question >= 7029.5f && question < 7030.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = GetSettings().revolverOpenStart;
        world[11] = GetSettings().revolverCloseStart;
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7035: "how should the gun sit in the hand?" -> (12345, barrel turned
    //            to the right, barrel tipped up), in degrees.
    // roll 7036: the rest of that -> (12345, top of the gun leaning to the right, 0).
    //            The arm script asks while a gun is held.
    if (question >= 7034.5f && question < 7036.5f)
    {
        int code = (int)(question + 0.5f);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = code == 7035 ? GetSettings().gunTurn : GetSettings().gunLean;
        world[11] = code == 7035 ? GetSettings().gunTip : 0.0f;
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7037, 7038: "how are the left / right hand's fingers?" -> three
    // numbers. The first tells what to do:
    //   10 to 11: pose the hand from its rest shape; the fraction is how far
    //             the thumb is closed, the second number the first finger,
    //             the third the other three fingers (each 0 to 1);
    //   20:       the hand holds one of the game's things: leave its fingers
    //             as the game's animation has them;
    //   0:        no pose: the whole hand closes by one amount (roll 7013).
    // The arm script asks.
    if (question >= 7036.5f && question < 7038.5f)
    {
        int hand = (int)(question + 0.5f) - 7037;
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float curls[3];
        int mode = VrGetFingers(hand, curls);
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = mode == 1 ? 10.0f + curls[0] : (mode == 2 ? 20.0f : 0.0f);
        world[7] = curls[1];
        world[11] = curls[2];
        g_scriptAskedTime = GetTickCount64();
        g_handScriptAskedTime = g_scriptAskedTime;
        return true;
    }

    // roll 7039: "how is the healing by hand going?" -> (12345; 0 = the
    // bandage is still in its tin, 1 = the roll is out and being wound round
    // the left forearm, 2 = done; how many times round so far).
    if (question >= 7038.5f && question < 7039.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float turns = 0.0f;
        int phase = VrGetBandage(turns);
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = (float)phase;
        world[11] = turns;
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7017: "was the last press in a menu made with a grip button?"
    // -> (12345, 1 or 0, 0). The inventory asks, so that letting go of the
    // grip only ends a drag and never uses the thing. Does not count as "the
    // game is being played".
    if (question >= 7016.5f && question < 7017.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = VrMenuPressWasGrip() ? 1.0f : 0.0f;
        world[11] = 0.0f;
        return true;
    }

    // roll 7018: "how is the walking stick pushed?" -> (12345, to the body's
    // right, the way the body faces), each -1 to 1. The player script asks
    // while a walking key is down and walks by this instead of by the key.
    // The first number is 0 when it should go by the keys after all.
    // roll 7019: "is the real head ducked?" -> (12345; 0 = ducking is switched
    // off, 1 = head up, 2 = head ducked; the most the view may be raised again
    // during such a crouch, in game meters). The body script asks.
    if (question >= 7017.5f && question < 7019.5f)
    {
        int code = (int)(question + 0.5f);
        if (code == 7019)
        {
            // With its question the body script also says, in the camera's
            // position, whether the game has the player crouched (first number).
            const float* numbers = (const float*)((char*)camera + kCameraPosition);
            g_gameCrouched = numbers[0] > 0.5f && numbers[0] < 1.5f;
            g_gameCrouchedTime = GetTickCount64();
        }
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        if (code == 7018)
        {
            float sideways = 0.0f, forwards = 0.0f;
            world[3] = VrGetStickMove(sideways, forwards) ? 12345.0f : 0.0f;
            world[7] = sideways;
            world[11] = forwards;
        }
        else
        {
            float maxLift = 0.0f;
            int duck = VrGetDuck(maxLift);
            world[3] = 12345.0f;
            world[7] = (float)duck;
            world[11] = maxLift;
        }
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7040 .. 7049: "what is VR option N set to?" -> (12345, choice, 0)
    // roll 7050 .. 7059: "set VR option N", the choice is in the camera's
    // position (first number). Both come from the VR page that MenuHandler.hps
    // adds to the game's options menu. They do not count as "the game is being
    // played" (the menu pointer depends on that).
    if (question >= 7039.5f && question < 7059.5f)
    {
        int code = (int)(question + 0.5f);
        if (code >= 7050)
        {
            const float* numbers = (const float*)((char*)camera + kCameraPosition);
            VrSetMenuOption(code - 7050, (int)(numbers[0] + 0.5f));
        }
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        if (code < 7050)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            world[7] = (float)VrGetMenuOption(code - 7040);
            world[11] = 0.0f;
        }
        return true;
    }

    // roll 7060: "what is VR option N set to?", N in the camera's position
    //            (first number) -> (12345, choice, 0)
    // roll 7061: "set VR option N", N and the choice in the camera's position
    //            (first and second numbers).
    // The same as 7040 .. 7059, without their limit of ten options; the VR
    // pages of the game's main menu (MenuHandler.hps) use these.
    if (question >= 7059.5f && question < 7061.5f)
    {
        int code = (int)(question + 0.5f);
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        int index = (int)(numbers[0] + 0.5f);
        int choice = (int)(numbers[1] + 0.5f);
        if (code == 7061) VrSetMenuOption(index, choice);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        if (code == 7060)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            world[7] = (float)VrGetMenuOption(index);
            world[11] = 0.0f;
        }
        return true;
    }

    // roll 7062: "how thick is the left forearm taken to be, for the bandage
    //            wound round it?" -> (12345, meters from the arm's middle, 0).
    //            The body's script asks (VRUpdateBandage in Prop_PlayerBody.hps).
    if (question >= 7061.5f && question < 7062.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = GetSettings().bandageRadius;
        world[11] = 0.0f;
        return true;
    }

    // roll 7063: "how far is the bandage's wrap moved across the arm?"
    //            -> (12345, meters one way, meters the other way).
    if (question >= 7062.5f && question < 7063.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = GetSettings().bandageShiftA;
        world[11] = GetSettings().bandageShiftB;
        return true;
    }

    // roll 7064: "buzz a controller": in the camera's position, which hand
    //            (0 left, 1 right, 2 the one holding), how long (seconds) and
    //            how strongly (0 to 1). The door scripts ask (a handle clicking
    //            open, a locked handle rattling, a door at its stop).
    if (question >= 7063.5f && question < 7064.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        VrScriptBuzz((int)(numbers[0] + 0.5f), numbers[1], numbers[2]);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = 0.0f;
        world[11] = 0.0f;
        return true;
    }

    // roll 7066: "at what rate should the game's logic run?" -> (12345, updates
    //            a second, the slowest rate to keep up with). 0: leave the
    //            game's own. The body's script asks (VRMatchLogicRate in
    //            PlayerBodyHandler.hps).
    if (question >= 7065.5f && question < 7066.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = (float)VrGetLogicRate();
        world[11] = (float)VrGetLogicSlowest();
        return true;
    }

    // roll 7065: "how many times has the view been centered?" -> (12345, that
    //            many, 0). The body's script asks, to fit the body to the
    //            player again after each re-center (UpdateVRLegs in
    //            Prop_PlayerBody.hps).
    if (question >= 7064.5f && question < 7065.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = (float)VrGetRecentreCount();
        world[11] = 0.0f;
        return true;
    }

    // roll 7016: "is there something to do?" -> (12345, 1 = draw the gun,
    // 2 = put it away, 3 = open the revolver, 0)
    if (question >= 7015.5f && question < 7016.5f)
    {
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;
        world[7] = (float)VrTakeScriptCommand();
        world[11] = 0.0f;
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7015 and 7031 .. 7034: numbers a script asks for. They come back
    // in the matrix's position, like the status answers further down.
    //   7015: (12345, a turn to do now in radians, positive = left, 1 if a snap
    //         turn's step is in it, 0 if only a smooth turn's small steps)
    //   7031: left hand steadying the gun: (to the left, down, forwards), meters
    //   7032: (how far that hand closes 0..1, hand reach in meters, throw strength)
    //   7033: (12345, 0, 1 + how much the elbows move with the hands)
    //   7034: (12345, 1 if "both eyes in step" is on, 1 while the right hand holds the flashlight's cord)
    if ((question >= 7014.5f && question < 7015.5f) || (question >= 7030.5f && question < 7034.5f))
    {
        int code = (int)(question + 0.5f);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;

        const Settings& settings = GetSettings();
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        if (code == 7015)
        {
            world[3] = 12345.0f;
            world[7] = VrTakeSnapTurn();
            world[11] = VrLastTurnWasSnap() ? 1.0f : 0.0f;
        }
        else if (code == 7031)
        {
            world[3] = settings.supportLeft;
            world[7] = settings.supportDown;
            world[11] = settings.supportForward;
        }
        else if (code == 7032)
        {
            world[3] = settings.supportCurl;
            world[7] = settings.handReach;
            world[11] = settings.throwBoost;
        }
        else if (code == 7034)
        {
            world[3] = 12345.0f;
            world[7] = VrBothEyesInStep() ? 1.0f : 0.0f;
            world[11] = VrCordHeld() ? 1.0f : 0.0f;
        }
        else
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log("CAMERA: the arm script asked how much the elbows should move and was told %.1f.", settings.elbowMove);
            }
            world[3] = 12345.0f;
            world[7] = 0.0f; // (unused)
            world[11] = 1.0f + settings.elbowMove;
        }
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7080 .. 7082: the Feel page's numbers (see Settings),
    // which the game's scripts read on every update (BunkerVR_ReadFeel in
    // helper_bunkervr.hps):
    //   7080: (12345, switches: 1 weight + 2 hand fixed to the thing + 4 even
    //          turn, hand stiffness)
    //   7081: (wrist stiffness, light things follow moving, ...turning)
    //   7082: (the share of the weight that pulls, a still hand steadied below, wrist strength)
    if (question >= 7079.5f && question < 7082.5f)
    {
        int code = (int)(question + 0.5f);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;

        const Settings& settings = GetSettings();
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        if (code == 7080)
        {
            world[3] = 12345.0f;
            world[7] = (float)((settings.feelWeight ? 1 : 0) + (settings.feelHandOnThing ? 2 : 0) + (settings.feelEvenTurn ? 4 : 0));
            world[11] = settings.feelHandStiffness;
        }
        else if (code == 7081)
        {
            world[3] = settings.feelWristStiffness;
            world[7] = settings.feelMoveQuickest;
            world[11] = settings.feelTurnQuickest;
        }
        else
        {
            world[3] = settings.feelWeightShown;
            world[7] = settings.feelSteadyBelow;
            world[11] = settings.feelWristStrength;
        }
        g_scriptAskedTime = GetTickCount64();
        return true;
    }

    // roll 7100 .. 7199: a note from a script for the log. The three numbers
    // are in the camera's position (the script puts the real position back).
    // The scripts normally send their notes through cVR_Note
    // (scriptfuncs.cpp); this roll still works as well.
    if (question >= 7099.5f && question < 7199.5f)
    {
        const float* numbers = (const float*)((char*)camera + kCameraPosition);
        CameraScriptNote((int)(question + 0.5f) - 7100, numbers[0], numbers[1], numbers[2]);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return true;
    }

    if (question < 7000.0f || question > 7014.0f) return false;

    int code = (int)(question + 0.5f); // 7001 .. 7012
    bool aboutHead = code == (int)kAskHeadMatrix || code == (int)kAskHeadStatus;
    bool aboutPalms = code == (int)kAskPalmStatus || code == (int)kAskLeftPalm || code == (int)kAskRightPalm ||
                      code == (int)kAskFingers || code == (int)kAskHandUse;
    bool askMatrix = code == (int)kAskHandMatrix || code == (int)kAskPointMatrix || code == (int)kAskHeadMatrix ||
                     code == (int)kAskLeftPalm || code == (int)kAskRightPalm || code == (int)kAskLevelCamera;
    bool askStatus = code == (int)kAskHandStatus || code == (int)kAskPointStatus || code == (int)kAskHeadStatus ||
                     code == (int)kAskPalmStatus || code == (int)kAskFingers || code == (int)kAskHandUse;
    bool aboutPointing = code == (int)kAskPointMatrix || code == (int)kAskPointStatus;
    if (!askMatrix && !askStatus) return false;

    g_scriptAskedTime = GetTickCount64();

    // Build the game's own camera matrices, level (no up/down tilt, no roll),
    // the same base the head and eyes are measured from.
    float* pitch = (float*)((char*)camera + kCameraPitch);
    float savedPitch = *pitch;
    bool anglesInUse = *(int*)((char*)camera + kCameraRotateMode) == 0;
    if (anglesInUse) *pitch = 0.0f;
    *roll = 0.0f;

    // Hands and held things are placed in the world from this answer. While
    // the right eye's picture is being drawn, the view comes from where the
    // game's camera was for the left eye (see HookedUpdateViewMatrix), so
    // measure from that same place. Otherwise the hands would sit a little
    // differently for each eye whenever you walk or turn.
    float* askPosition = (float*)((char*)camera + kCameraPosition);
    float* askYaw = (float*)((char*)camera + kCameraYaw);
    float askSavedPosition[3] = { askPosition[0], askPosition[1], askPosition[2] };
    float askSavedYaw = *askYaw;
    bool placedInWorld = code == (int)kAskHandMatrix || code == (int)kAskLeftPalm || code == (int)kAskRightPalm;
    bool fromLeftEyePlace = false;
    if (placedInWorld && VrGetFrameEye() == 1)
    {
        unsigned long long serial = VrGetFrameSerial();
        CameraFrame& entry = CameraFrameFor(camera);
        if (serial != 0 && entry.pairSerial != 0 && entry.pairSerial + 1 == serial)
        {
            fromLeftEyePlace = true;
            askPosition[0] = entry.pairPosition[0];
            askPosition[1] = entry.pairPosition[1];
            askPosition[2] = entry.pairPosition[2];
            if (anglesInUse) *askYaw = entry.pairYaw;
        }
    }

    g_realUpdateViewMatrix(camera);

    if (anglesInUse) *pitch = savedPitch;
    *roll = question;
    if (fromLeftEyePlace)
    {
        askPosition[0] = askSavedPosition[0];
        askPosition[1] = askSavedPosition[1];
        askPosition[2] = askSavedPosition[2];
        *askYaw = askSavedYaw;
    }

    HeadPose hand;
    int handIndex = 0;

    // The game's own camera, level: that is what has just been built.
    if (code == (int)kAskLevelCamera) return true;

    // The palms, for the game's own arms.
    if (aboutPalms)
    {
        if (code == (int)kAskPalmStatus)
        {
            HeadPose unused;
            int tracked = 0;
            if (VrGameHandsEnabled() && !CameraTheatre())
            {
                if (VrGetGripHandPose(0, unused)) tracked |= 1;
                if (VrGetGripHandPose(1, unused)) tracked |= 2;
            }
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            // +4: "send your measurements to the log this time" (after an F9 capture).
            if (tracked != 0 && VrTakeArmNotesRequest()) tracked += 4;
            world[7] = (float)tracked;
            world[11] = VrGetArmScale();
        }
        else if (code == (int)kAskFingers)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = VrGetHandCurl(0);
            world[7] = VrGetHandCurl(1);
            world[11] = CameraGunHeld() ? 1.0f : 0.0f; // third number: is a gun held?
        }
        else if (code == (int)kAskHandUse)
        {
            float* world = (float*)((char*)camera + kCameraWorldMatrix);
            world[3] = 12345.0f;
            world[7] = (float)VrGetScriptHoldMask();
            world[11] = (float)VrGetGunHandFlags();
        }
        else if (VrGetGripHandPose(code == (int)kAskLeftPalm ? 0 : 1, hand))
        {
            ApplyPose(camera, hand);

            if (g_handScriptAskedTime == 0)
                Log("CAMERA: the game's arm code asked for a palm position and got it.");
            g_handScriptAskedTime = GetTickCount64();
        }
        return true;
    }

    bool grabbing = aboutHead       ? VrGetHeadCentrePose(hand)
                    : aboutPointing ? VrGetPointingHandPose(hand, handIndex)
                                    : VrGetGrabHandPose(hand, handIndex);

    if (askMatrix)
    {
        if (grabbing) ApplyPose(camera, hand);
    }
    else
    {
        float* world = (float*)((char*)camera + kCameraWorldMatrix);
        world[3] = 12345.0f;              // "yes, the VR mod is here"
        world[7] = (grabbing && !CameraTheatre()) ? 1.0f : 0.0f; // on the cutscene screen: head and hands left out
        world[11] = aboutHead ? VrGetBodyScale() : (float)handIndex; // head: how big to draw the body
    }

    static bool loggedGrab = false, loggedPoint = false, loggedHead = false;
    if (grabbing && aboutHead)
    {
        if (!loggedHead)
        {
            loggedHead = true;
            Log("CAMERA: the game's body code asked for the head position and got it.");
        }
        return true;
    }
    if (grabbing && !aboutPointing && !loggedGrab)
    {
        loggedGrab = true;
        Log("CAMERA: the game's grab code asked for the hand position and got it (hand %d).", handIndex);
    }
    if (grabbing && aboutPointing && !loggedPoint)
    {
        loggedPoint = true;
        Log("CAMERA: the game's pick code asked for the pointing hand and got it (hand %d).", handIndex);
    }
    return true;
}

// ---- Timing the scripts (for the log) ----
// How many questions the scripts ask the mod (special roll values) and how
// long answering them takes; and how long some of the mod's script work takes:
// a script marks the start of a piece of work with roll 7070 + 2 x its number
// and the end with 7071 + 2 x its number (see VRTimeMark in the scripts).
static const int kScriptSections = 45;
// Only the first five can also arrive as a camera roll (7070 to 7079): rolls 7080 and up are questions,
// the Feel page's numbers and more. The rest arrive through cVR_TimeMark only.
static const int kScriptSectionsByRoll = 5;
static const char* const kScriptSectionNames[kScriptSections] = {
    "the body script's update", "the arms", "the legs", "following the head", "the player script's whole update",
    // Parts of the arms.
    "arms: stopped by things", "arms: pressed by things", "arms: what they touch", "arms: buttons", "arms: the bones", "arms: second time",
    // Parts of the body script's update and the rest of the body's work (codes 7092 to 7105).
    "body: the game's own part", "body: hands, turning and pulls", "body: the items' own work", "body: the hints",
    "body: the rest", "held things placed", "body: the hands' own holds",
    // The arms' bones by part (UpdateVRArm, each arm in each pass; codes 7106 to 7113).
    "the pose kept and the reach", "upper arm and forearm", "the forearm's twist", "the hand and fingers",
    // What the hands touch by part (VRHandTouchFind, each hand once a frame; codes 7114 to 7127). The can-use
    // check, the balls and the doors' handles are marked once for each body that gets that far, so their
    // "times" count those bodies.
    "the bodies near found", "the game's can-use check", "the balls against each", "a door's handles",
    "all the bodies near tried", "the pick-ups by their look", "what was found said, and the ticks",
    // The body script's update by part (PlayerBodyHandler.hps Update, once a frame; codes 7128 to 7155).
    // The first five are part of "body: the game's own part", the rest of "body: hands, turning and pulls".
    "the logic rate matched", "the animation states", "the game's coverage", "the game's bones", "the body turned and placed",
    "the gun's news", "the throwing hand followed", "the Feel page read", "snap turns", "turning while walking",
    "pulls", "ducking", "the gun's commands", "waiting to put away",
    // Inside two of those: the room in front of the interface's pieces, in "the gun's news"; the pull's look for a
    // thing, in "pulls" (codes 7156 to 7159).
    "the interface's room", "pulls: looking for a thing" };
static const int kFirstArmBoneSection = 18;  // each group from here on gets a log line of its own
static const int kFirstTouchSection = 22;
static const int kFirstBodyPartSection = 29;
static double g_questionMs = 0;
static unsigned long long g_questions = 0;
static double g_sectionStartMs[kScriptSections] = {};
static double g_sectionMs[kScriptSections] = {};
static unsigned long long g_sectionCount[kScriptSections] = {};
// Every finished update of the player script (its last mark), never cleared:
// one for each update of the game's logic while a level is played.
static unsigned long long g_playerUpdatesEver = 0;

unsigned long long CameraPlayerUpdates()
{
    return g_playerUpdatesEver;
}

static double QuestionClockMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

void CameraLogScriptTimings(unsigned long long frames)
{
    if (frames == 0) return;
    char text[1000];
    int length = snprintf(text, sizeof(text), "SCRIPTS: per frame on average: %.1f questions to the mod (%.3f ms answering them)",
                          (double)g_questions / (double)frames, g_questionMs / (double)frames);
    // The sections from the body script's parts on go on lines of their own: the log's lines end at
    // 1024 characters.
    static const int kFirstLineSections = 11;
    for (int i = 0; i < kFirstLineSections && length > 0 && length < (int)sizeof(text); ++i)
    {
        if (g_sectionCount[i] == 0) continue;
        length += snprintf(text + length, sizeof(text) - length, " | %s %.3f ms (%.1f times)", kScriptSectionNames[i],
                           g_sectionMs[i] / (double)frames, (double)g_sectionCount[i] / (double)frames);
    }
    Log("%s.", text);
    length = snprintf(text, sizeof(text), "SCRIPTS, the body's parts, per frame on average:");
    int partsShown = 0;
    for (int i = kFirstLineSections; i < kFirstArmBoneSection && length > 0 && length < (int)sizeof(text); ++i)
    {
        if (g_sectionCount[i] == 0) continue;
        length += snprintf(text + length, sizeof(text) - length, "%s %s %.3f ms (%.1f times)", partsShown > 0 ? " |" : "",
                           kScriptSectionNames[i], g_sectionMs[i] / (double)frames, (double)g_sectionCount[i] / (double)frames);
        ++partsShown;
    }
    if (partsShown > 0) Log("%s.", text);
    // The arms' bones by part, each arm in each pass; their sum is about "arms: the bones".
    length = snprintf(text, sizeof(text), "SCRIPTS, the arms' bones by part, per frame on average:");
    partsShown = 0;
    for (int i = kFirstArmBoneSection; i < kFirstTouchSection && length > 0 && length < (int)sizeof(text); ++i)
    {
        if (g_sectionCount[i] == 0) continue;
        length += snprintf(text + length, sizeof(text) - length, "%s %s %.3f ms (%.1f times)", partsShown > 0 ? " |" : "",
                           kScriptSectionNames[i], g_sectionMs[i] / (double)frames, (double)g_sectionCount[i] / (double)frames);
        ++partsShown;
    }
    if (partsShown > 0) Log("%s.", text);
    // What the hands touch by part; their sum with the setting up is about "arms: what they touch".
    length = snprintf(text, sizeof(text), "SCRIPTS, what the hands touch by part, per frame on average:");
    partsShown = 0;
    for (int i = kFirstTouchSection; i < kFirstBodyPartSection && length > 0 && length < (int)sizeof(text); ++i)
    {
        if (g_sectionCount[i] == 0) continue;
        length += snprintf(text + length, sizeof(text) - length, "%s %s %.3f ms (%.1f times)", partsShown > 0 ? " |" : "",
                           kScriptSectionNames[i], g_sectionMs[i] / (double)frames, (double)g_sectionCount[i] / (double)frames);
        ++partsShown;
    }
    if (partsShown > 0) Log("%s.", text);
    // The body script's update by part.
    length = snprintf(text, sizeof(text), "SCRIPTS, the body's update by part, per frame on average:");
    partsShown = 0;
    for (int i = kFirstBodyPartSection; i < kScriptSections && length > 0 && length < (int)sizeof(text); ++i)
    {
        if (g_sectionCount[i] == 0) continue;
        length += snprintf(text + length, sizeof(text) - length, "%s %s %.3f ms (%.1f times)", partsShown > 0 ? " |" : "",
                           kScriptSectionNames[i], g_sectionMs[i] / (double)frames, (double)g_sectionCount[i] / (double)frames);
        ++partsShown;
    }
    if (partsShown > 0) Log("%s.", text);
    g_questionMs = 0;
    g_questions = 0;
    for (int i = 0; i < kScriptSections; ++i)
    {
        g_sectionMs[i] = 0;
        g_sectionCount[i] = 0;
    }
}

void CameraScriptTimeMark(int code)
{
    int mark = code - 7070;
    if (mark < 0 || mark >= 2 * kScriptSections) return;
    int section = mark / 2;
    double now = QuestionClockMs();
    if ((mark & 1) == 0)
    {
        g_sectionStartMs[section] = now;
    }
    else if (g_sectionStartMs[section] > 0)
    {
        g_sectionMs[section] += now - g_sectionStartMs[section];
        ++g_sectionCount[section];
        g_sectionStartMs[section] = 0;
    }
    if (mark == 2 * 4 + 1) ++g_playerUpdatesEver; // the player script's update is done
}

// ---- The sound's listener: at the real head ----
// The engine puts the ears (cLowLevelSoundFmod's function that stores what FMOD is told each
// frame: place, speed, ahead, up) where the game's camera is, turned as the view is, so they
// follow the head's turn, nod and tilt. But in the headset the game's camera stays at the
// middle of the play area while the real head walks about the room (the game's body follows
// the head, the camera does not: VRFollowHead in PlayerBodyHandler.hps). A step away from that
// middle left the ears behind, and the player's own sounds, made at the body and the hands
// (the flashlight, the gun), came from one side or the other as the head turned. So the ears
// are put at the middle of the real head in the game's world: the last view built undone, and
// the head's middle placed in it. Not on the cutscene screen, without a fresh view (menus,
// loading), or should that be implausibly far from the engine's place (more than
// kListenerFurthest: a view left over from before a jump).
static const char* kSetListenerPattern =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC E0 00 00 00 0F 29 70 E8 48 8B E9 "
    "0F 29 78 D8 48 8B F2 44 0F 29 40 C8";
static const float kListenerFurthest = 5.0f; // the game's units: a play area some 4 m across, at the world's scale
typedef void (*SetListenerFn)(void* self, const float* place, const float* speed, const float* ahead, const float* up);
static SetListenerFn g_realSetListener = nullptr;
static bool g_listenerLogged = false; // (CameraWatchSoundListener: where the ears are, in test runs)

// The middle of the real head in the game's world (false: not known now).
static bool HeadMiddleInWorld(float place[3])
{
    const LastView& v = g_lastView;
    if (!v.valid || !v.headApplied || CameraTheatre()) return false;
    unsigned long long serial = VrGetFrameSerial();
    if (serial == 0 || v.serial + 4 < serial) return false;
    HeadPose centre;
    if (!VrGetHeadCentrePose(centre)) return false;
    // The game's own view (world -> its camera: its turn, row after row, and its position part)
    // undone: the head's middle is placed from the game's camera, as ApplyPose places it.
    const float* m = v.gameView;
    float fromCamera[3] = { centre.position[0] - m[3], centre.position[1] - m[7], centre.position[2] - m[11] };
    for (int i = 0; i < 3; ++i)
    {
        place[i] = m[0 * 4 + i] * fromCamera[0] + m[1 * 4 + i] * fromCamera[1] + m[2 * 4 + i] * fromCamera[2];
        if (!(place[i] == place[i])) return false; // (not a number)
    }
    return true;
}

static void HookedSetListener(void* self, const float* place, const float* speed, const float* ahead, const float* up)
{
    float head[3];
    bool atHead = HeadMiddleInWorld(head);
    float apart = 0.0f; // (squared)
    if (atHead)
    {
        float dx = head[0] - place[0], dy = head[1] - place[1], dz = head[2] - place[2];
        apart = dx * dx + dy * dy + dz * dz;
        if (apart > kListenerFurthest * kListenerFurthest) atHead = false;
    }
    const float* used = atHead ? head : place;

    static bool toldAtHead = false;
    if (atHead && !toldAtHead && apart > 0.05f * 0.05f)
    {
        toldAtHead = true;
        Log("CAMERA: the sound's listener is put at the real head (%.2f %.2f %.2f; the engine had it at %.2f %.2f %.2f).",
            head[0], head[1], head[2], place[0], place[1], place[2]);
    }
    if (g_listenerLogged)
    {
        static double nextMs = 0.0;
        static int linesLeft = 3000;
        double now = QuestionClockMs();
        if (linesLeft > 0 && now >= nextMs)
        {
            nextMs = now + 200.0;
            --linesLeft;
            Log("LISTENER: at %.2f %.2f %.2f (%s; the engine's %.2f %.2f %.2f), ahead %.2f %.2f %.2f, up %.2f %.2f %.2f", used[0], used[1], used[2],
                atHead ? "the real head" : "the engine's place", place[0], place[1], place[2], ahead[0], ahead[1], ahead[2], up[0], up[1], up[2]);
        }
    }
    g_realSetListener(self, used, speed, ahead, up);
}

bool CameraInstallListenerHook()
{
    HMODULE game = GetModuleHandleW(nullptr);
    int matches = 0;
    void* target = FindPattern(game, kSetListenerPattern, &matches);
    if (matches != 1)
    {
        Log("CAMERA ERROR: expected to find the sound listener's function exactly once, found it %d times (the ears stay where the game's camera is).", matches);
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("CAMERA ERROR: could not start the hooking library for the sound listener: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(target, (LPVOID)HookedSetListener, (LPVOID*)&g_realSetListener);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("CAMERA ERROR: could not hook the sound listener's function: %s (the ears stay where the game's camera is).", MH_StatusToString(status));
        return false;
    }
    Log("CAMERA: the sound's listener is hooked at exe+0x%llX: the ears go with the real head.", (unsigned long long)((char*)target - (char*)game));
    return true;
}

void CameraWatchSoundListener()
{
    g_listenerLogged = true;
    Log("CAMERA: the sound's listener is %s.", g_realSetListener != nullptr ? "watched (LISTENER lines)" : "not watched: its hook is not in");
}

void CameraScriptNote(int note, float a, float b, float c)
{
    static int notesLeft = 6000;
    if (notesLeft <= 0) return;
    --notesLeft;
    Log("SCRIPT NOTE %3d: %10.4f %10.4f %10.4f", note, a, b, c);
}

// ---- The eye held still through a snap turn ----
// The game's camera hangs on a socket at the front of the drawn body's head,
// some 20 cm from the line the body turns about, so turning the body swings
// it round. Spread over a smooth turn that feels natural; in a snap turn it
// is a jump in one frame, and the eye would go with it (some 18 cm sideways
// at each 45 degree snap, then some 3 cm more over the next frame or two).
// So when the game's turn jumps by more than 10 degrees between two views,
// however far its camera point moved sideways is taken off the eye again,
// and the middle of the play area moved to match, so it stays off; the same
// for the next few frames while the stick is not walking.
static void HoldEyeThroughSnap(HeadPose& head, const float position[3], float yaw, const float* gameView)
{
    static bool known = false;
    static float lastPosition[3] = { 0, 0, 0 };
    static float lastYaw = 0.0f;
    static unsigned long long holdUntilSerial = 0;

    unsigned long long serial = VrGetFrameSerial();
    if (known)
    {
        float turned = yaw - lastYaw;
        if (turned > 3.14159265f) turned -= 6.28318531f;
        if (turned < -3.14159265f) turned += 6.28318531f;
        float moved[3] = { position[0] - lastPosition[0], 0.0f, position[2] - lastPosition[2] };
        float distance = sqrtf(moved[0] * moved[0] + moved[2] * moved[2]);
        // Only a snap the player asked for: not the game turning the player
        // itself, onto a ladder and the like, where the view must go with it.
        bool snap = fabsf(turned) > 0.17f && fabsf(turned) < 3.0f && VrSnapTurnJustTaken();
        if (snap) holdUntilSerial = serial + 6; // six pictures: about three frames when both eyes are drawn in one frame
        bool hold = distance > 0.0005f && distance < 0.5f &&
                    (snap || (serial <= holdUntilSerial && !VrWalkingWithStick()));
        if (hold)
        {
            // In the game camera's own directions (its view's turn, row
            // after row), the opposite of how far it moved.
            float shift[3];
            for (int row = 0; row < 3; ++row)
                shift[row] = -(gameView[row * 4 + 0] * moved[0] + gameView[row * 4 + 1] * moved[1] + gameView[row * 4 + 2] * moved[2]);
            for (int i = 0; i < 3; ++i) head.position[i] += shift[i];
            VrShiftCentre(shift);

            static int logsLeft = 10;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("CAMERA: %s: the game's camera point moved %.1f cm sideways; the eye is held where it was.",
                    snap ? "a snap turn" : "after a snap turn", distance * 100.0f);
            }
        }
    }
    for (int i = 0; i < 3; ++i) lastPosition[i] = position[i];
    lastYaw = yaw;
    known = true;
}

// ---- The eye's own position while the world is drawn ----
// The mod puts the head into the camera's view, not into its position, so
// that the game's logic keeps working from where its own camera is. But the
// drawing takes some decisions from the camera's position too: whether the
// camera is inside a light's reach decides how the light is drawn, and
// whether it is first tested for being hidden. With the real head away from
// the game's camera (leaning, crouching for real while the body stands, the
// body following a step behind), those go wrong: lights switch on and off
// with the head, drawn as patches with straight edges. So while the world is
// drawn (scene.cpp asks, per eye) the camera holds where the eye is, and
// afterwards its own position again.
struct EyePositionUse
{
    void* camera;
    bool on;
    bool written;  // the eye's position is in the camera now
    float game[3]; // the game's own position, while it is
    float eye[3];  // what was put in
};
static EyePositionUse g_eyeUse = {};

// The game's position back in the camera, if the eye's is (still) there.
static void PutGamePositionBack(void* camera)
{
    if (!g_eyeUse.written || g_eyeUse.camera != camera) return;
    float* position = (float*)((char*)camera + kCameraPosition);
    if (position[0] == g_eyeUse.eye[0] && position[1] == g_eyeUse.eye[1] && position[2] == g_eyeUse.eye[2])
    {
        position[0] = g_eyeUse.game[0];
        position[1] = g_eyeUse.game[1];
        position[2] = g_eyeUse.game[2];
    }
    g_eyeUse.written = false;
}

// The eye's position into the camera: the position part of its world
// matrix, which has the head in it once the view is built.
static void WriteEyePosition(void* camera)
{
    float* position = (float*)((char*)camera + kCameraPosition);
    const float* world = (const float*)((char*)camera + kCameraWorldMatrix);
    for (int i = 0; i < 3; ++i) g_eyeUse.game[i] = position[i];
    g_eyeUse.eye[0] = world[3];
    g_eyeUse.eye[1] = world[7];
    g_eyeUse.eye[2] = world[11];
    for (int i = 0; i < 3; ++i) position[i] = g_eyeUse.eye[i];
    g_eyeUse.written = true;

    static int logsLeft = 5;
    if (logsLeft > 0)
    {
        --logsLeft;
        float dx = g_eyeUse.eye[0] - g_eyeUse.game[0], dy = g_eyeUse.eye[1] - g_eyeUse.game[1], dz = g_eyeUse.eye[2] - g_eyeUse.game[2];
        Log("CAMERA: the world is drawn with the eye's own position, %.1f cm from the game camera's.",
            sqrtf(dx * dx + dy * dy + dz * dz) * 100.0f);
    }
}

void CameraUseEyePosition(void* camera, bool on)
{
    if (camera == nullptr) return;
    if (on)
    {
        if (g_eyeUse.on && g_eyeUse.camera != camera) PutGamePositionBack(g_eyeUse.camera);
        g_eyeUse.camera = camera;
        g_eyeUse.on = true;
        g_eyeUse.written = false;
        return;
    }
    if (g_eyeUse.camera == camera) PutGamePositionBack(camera);
    g_eyeUse.on = false;
}

// Runs every time the engine rebuilds a camera's view matrix.
static void HookedUpdateViewMatrix(void* camera)
{
    // A turn of the player's camera (the one the head is put into) is
    // followed about the head as soon as it is seen, before anything (the
    // arms' questions, the view) uses the head with it (see VrFollowTurn).
    if (camera == g_lastView.camera && *(int*)((char*)camera + kCameraRotateMode) == 0)
    {
        float yaw = *(const float*)((char*)camera + kCameraYaw);
        if (yaw == yaw) VrFollowTurn(yaw, true);
    }

    float question = *(float*)((char*)camera + kCameraRoll);
    if (question >= 7069.5f && question < 7069.5f + 2 * kScriptSectionsByRoll)
    {
        // A script's timing mark: nothing to answer. The scripts normally
        // send these through cVR_TimeMark (scriptfuncs.cpp); this roll still
        // works as well.
        CameraScriptTimeMark((int)(question + 0.5f));
        float* roll = (float*)((char*)camera + kCameraRoll);
        *roll = 0.0f;
        g_realUpdateViewMatrix(camera);
        *roll = question;
        return;
    }
    if (question > 6999.5f && question < 7199.5f)
    {
        double asked = QuestionClockMs();
        if (AnswerScriptQuestion(camera))
        {
            g_questionMs += QuestionClockMs() - asked;
            ++g_questions;
            return;
        }
    }
    else if (AnswerScriptQuestion(camera)) return;

    // The world's drawing, with the eye's own position in the camera (see
    // CameraUseEyePosition): the view is built from the game's position, so
    // that goes back in first.
    bool eyeUse = g_eyeUse.on && g_eyeUse.camera == camera;
    if (eyeUse) PutGamePositionBack(camera);

    ApplyFieldOfViewOverride(camera);

    // Note that this camera's view has now been rebuilt for the current frame.
    unsigned long long serial = VrGetFrameSerial();
    g_lastWorldSerial = serial;

    // A cutscene shown on a screen: the game's own view, untouched.
    if (CameraTheatre())
    {
        if (serial != 0) CameraFrameFor(camera).serial = serial;
        g_realUpdateViewMatrix(camera);
        return;
    }

    if (serial != 0)
    {
        CameraFrame& entry = CameraFrameFor(camera);
        if (entry.serial != serial)
        {
            entry.serial = serial;
            ++g_statNaturalRebuilds;
        }
    }

    HeadPose head;
    bool haveHead = VrGetHeadPose(head);
    // The hand poses tool's camera on a hand (only while the headset is off):
    // the view from there instead of from the head.
    HeadPose handView;
    float handFieldOfView = 0.0f;
    bool onHand = haveHead && HandViewPose(handView, handFieldOfView);

    // In VR, looking up and down is done with your head. If the game also
    // tilts the camera up or down (which the mouse normally does), the whole
    // world ends up tilted compared with the real floor. So while the headset
    // is tracked, build the game's view with its own up/down tilt set to zero,
    // then put the game's value back so nothing else in the game is disturbed.
    float* gamePitch = (float*)((char*)camera + kCameraPitch);
    float savedPitch = *gamePitch;
    bool removePitch = haveHead && *(int*)((char*)camera + kCameraRotateMode) == 0;
    if (removePitch) *gamePitch = 0.0f;

    // The eyes are drawn on alternate frames. If the game turned or moved its
    // camera between the left eye's frame and the right eye's, the two eyes
    // would show slightly different worlds, and depth would shimmer whenever
    // you turn with the stick or walk. So the right eye's picture is drawn
    // from exactly where the game's camera was for the left eye's picture.
    // (Your real head movement is separate and stays fresh for each eye.)
    float* gamePosition = (float*)((char*)camera + kCameraPosition);
    float* gameYaw = (float*)((char*)camera + kCameraYaw);
    float savedPosition[3] = { gamePosition[0], gamePosition[1], gamePosition[2] };
    float savedYaw = *gameYaw;
    if (haveHead) { g_lastGameYaw = savedYaw; g_lastGameYawTime = GetTickCount64(); } // (for turning about the head: see CameraGetGameYaw)
    bool samePlaceAsLeftEye = false;
    if (haveHead && serial != 0)
    {
        CameraFrame& entry = CameraFrameFor(camera);
        if (VrGetFrameEye() == 0)
        {
            entry.pairSerial = serial;
            entry.pairPosition[0] = gamePosition[0];
            entry.pairPosition[1] = gamePosition[1];
            entry.pairPosition[2] = gamePosition[2];
            entry.pairYaw = *gameYaw;
        }
        else if (entry.pairSerial != 0 && entry.pairSerial + 1 == serial)
        {
            samePlaceAsLeftEye = true;
            gamePosition[0] = entry.pairPosition[0];
            gamePosition[1] = entry.pairPosition[1];
            gamePosition[2] = entry.pairPosition[2];
            if (removePitch) *gameYaw = entry.pairYaw; // only when the camera is aimed by its angles
        }
    }

    // Let the game do its normal work.
    g_realUpdateViewMatrix(camera);

    if (removePitch) *gamePitch = savedPitch;
    if (samePlaceAsLeftEye)
    {
        gamePosition[0] = savedPosition[0];
        gamePosition[1] = savedPosition[1];
        gamePosition[2] = savedPosition[2];
        *gameYaw = savedYaw;
    }

    float* view = (float*)((char*)camera + kCameraViewMatrix);

    float viewBefore[16];
    for (int i = 0; i < 16; ++i) viewBefore[i] = view[i];

    // If the projection hook is not available, read the camera's stored
    // projection matrix here instead (it can be one frame out of date).
    if (g_realGetProjectionMatrix == nullptr)
    {
        const float* projection = (const float*)((char*)camera + kCameraProjection);
        g_projectionAcross = projection[0];
        g_projectionUpDown = projection[5];
    }

    // A snap turn swings the game's camera point (see HoldEyeThroughSnap).
    if (haveHead && removePitch && !onHand) HoldEyeThroughSnap(head, savedPosition, savedYaw, viewBefore);

    if (haveHead) ApplyPose(camera, onHand ? handView : head);

    // Remember this view for the capture tool (capture.cpp).
    {
        LastView& v = g_lastView;
        unsigned long long nowSerial = VrGetFrameSerial();
        v.rebuildsThisSerial = (v.valid && v.serial == nowSerial) ? v.rebuildsThisSerial + 1 : 1;
        v.valid = true;
        v.headApplied = haveHead;
        v.serial = nowSerial;
        v.camera = camera;
        const float* position = (const float*)((char*)camera + kCameraPosition);
        v.gamePosition[0] = position[0];
        v.gamePosition[1] = position[1];
        v.gamePosition[2] = position[2];
        v.gamePitch = *(const float*)((char*)camera + kCameraPitch);
        v.gameYaw = *(const float*)((char*)camera + kCameraYaw);
        v.fieldOfView = *(const float*)((char*)camera + kCameraFieldOfView);
        v.aspect = *(const float*)((char*)camera + kCameraAspect);
        v.head = head;
        for (int i = 0; i < 16; ++i)
        {
            v.gameView[i] = viewBefore[i];
            v.finalView[i] = view[i];
        }
    }

    LogCamera(camera, viewBefore, haveHead);

    // The world's drawing: from now until it is over the camera holds where
    // the eye is (see CameraUseEyePosition).
    if (eyeUse && haveHead) WriteEyePosition(camera);
}

bool InstallCameraHook()
{
    HMODULE game = GetModuleHandleW(nullptr);

    int matches = 0;
    void* target = FindPattern(game, kUpdateViewMatrixPattern, &matches);
    if (matches != 1)
    {
        Log("CAMERA ERROR: expected to find the camera function exactly once, found it %d times. "
            "The game may have been updated.", matches);
        return false;
    }
    Log("CAMERA: found the view matrix function at exe+0x%llX",
        (unsigned long long)((char*)target - (char*)game));

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("CAMERA ERROR: could not start the hooking library: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_CreateHook(target, (LPVOID)HookedUpdateViewMatrix, (LPVOID*)&g_realUpdateViewMatrix);
    if (status != MH_OK)
    {
        Log("CAMERA ERROR: could not create the camera hook: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("CAMERA ERROR: could not switch on the camera hook: %s", MH_StatusToString(status));
        return false;
    }

    // Second hook: the projection matrix, for the field of view override.
    // If this one fails, head tracking still works; the picture keeps
    // the game's own field of view.
    void* projectionTarget = FindPattern(game, kGetProjectionMatrixPattern, &matches);
    if (matches != 1)
    {
        Log("CAMERA ERROR: expected to find the projection function exactly once, found it %d times.", matches);
        return true;
    }
    status = MH_CreateHook(projectionTarget, (LPVOID)HookedGetProjectionMatrix, (LPVOID*)&g_realGetProjectionMatrix);
    if (status == MH_OK) status = MH_EnableHook(projectionTarget);
    if (status != MH_OK)
        Log("CAMERA ERROR: could not hook the projection function: %s", MH_StatusToString(status));
    else
        Log("CAMERA: found the projection function at exe+0x%llX",
            (unsigned long long)((char*)projectionTarget - (char*)game));

    // Third and fourth hooks: make sure the view is rebuilt on every frame.
    struct ExtraHook { const char* name; const char* pattern; LPVOID detour; LPVOID* original; };
    ExtraHook extraHooks[2] = {
        { "view matrix getter", kGetViewMatrixPattern, (LPVOID)HookedGetViewMatrix, (LPVOID*)&g_realGetViewMatrix },
        { "visibility getter", kGetFrustumPattern, (LPVOID)HookedGetFrustum, (LPVOID*)&g_realGetFrustum },
    };
    for (int i = 0; i < 2; ++i)
    {
        void* extraTarget = FindPattern(game, extraHooks[i].pattern, &matches);
        if (matches != 1)
        {
            Log("CAMERA ERROR: expected to find the %s exactly once, found it %d times.", extraHooks[i].name, matches);
            continue;
        }
        status = MH_CreateHook(extraTarget, extraHooks[i].detour, extraHooks[i].original);
        if (status == MH_OK) status = MH_EnableHook(extraTarget);
        if (status != MH_OK)
            Log("CAMERA ERROR: could not hook the %s: %s", extraHooks[i].name, MH_StatusToString(status));
        else
            Log("CAMERA: hooked the %s at exe+0x%llX", extraHooks[i].name,
                (unsigned long long)((char*)extraTarget - (char*)game));
    }
    return true;
}

void CameraForceRebuild(void* camera)
{
    if (camera == nullptr) return;

    // The same marks the engine's own "set pitch" function leaves (see
    // EnsureFreshView): the view, and the things built from it, are out of date.
    unsigned char* flags = (unsigned char*)camera;
    flags[kCameraViewDirty] = 1;
    flags[kCameraOtherDirty + 0] = 1;
    flags[kCameraOtherDirty + 1] = 1;
    flags[kCameraOtherDirty + 2] = 1;
}
