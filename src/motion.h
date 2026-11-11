#pragma once

// The motion recorder, a developer tool. Everything the headset tells the mod,
// frame by frame, is kept for the last two minutes (at 90 frames a second;
// longer at lower frame rates): the head, both hands' two poses, the squeezes,
// buttons and sticks. A mark or a failed check (testlist.h) saves those two
// minutes to a file in BunkerVR\recordings, so every reported problem comes
// with what the hands really did.
//
// No OpenXR in here, so this can be unit-tested on its own, outside the game.

struct MotionPose
{
    float position[3];    // meters, in the room (the mod's floor space)
    float orientation[4]; // x, y, z, w
};

// MotionFrame::buttons
enum
{
    kMotionButtonA = 1 << 0,
    kMotionButtonB = 1 << 1,
    kMotionButtonX = 1 << 2,
    kMotionButtonY = 1 << 3,
    kMotionButtonMenu = 1 << 4,
    kMotionButtonLeftStick = 1 << 5,
    kMotionButtonRightStick = 1 << 6,
    kMotionThumbTouch = 1 << 7,    // and << 8 for the right hand
    kMotionFingerTouch = 1 << 9,   // and << 10 for the right hand
};

// MotionFrame::flags. The hands' four bits start at kMotionHandBits (the left
// hand) and kMotionHandBits << 4 (the right).
enum
{
    kMotionHeadKnown = 1 << 0,
    kMotionHandBits = 1 << 1,  // the palm's pose is known...
    // << 1: the palm's position is tracked (not estimated by the runtime);
    // << 2: the pointing pose is known; << 3: its position is tracked.
    kMotionGunHeld = 1 << 9,
    kMotionGamePaused = 1 << 10,
    kMotionInventoryOpen = 1 << 11,
    kMotionCutsceneScreen = 1 << 12,
    kMotionVrMenuOpen = 1 << 13,
};

// One frame (224 bytes). Laid out so that nothing is padded: the file holds
// these as they are, and readers read them by offset.
struct MotionFrame
{
    double timeMs;            // the mod's clock (QueryPerformanceCounter)
    long long displayTime;    // when the headset will show this frame, its own clock (ns)
    MotionPose head;
    MotionPose grip[2];       // the palm: 0 left, 1 right
    MotionPose aim[2];        // where the controller points
    float gripValue[2];       // the grip buttons, 0 to 1
    float triggerValue[2];    // the triggers, 0 to 1
    float moveStick[2];       // the left stick, x and y
    float turnStick[2];       // the right stick, x and y
    float straightAhead[4];   // "straight ahead" as the last recenter set it: x, y, z, yaw
    unsigned buttons;
    unsigned flags;
    short rightItemKind;      // the kinds CameraRightItemKind gives
    short leftItemKind;
    unsigned frameNumber;
    float spare;
};
static_assert(sizeof(MotionFrame) == 224, "the recordings' frame layout: see tools\\motion\\read_motion.ps1");

// The start of a recording's file (128 bytes). The frames follow, oldest first.
struct MotionFileHeader
{
    char magic[8];            // "BVRMOT1"
    unsigned frameSize;
    unsigned frameCount;
    int mark;                 // the mark's number, as in both logs
    int modVersion;           // in hundredths (1.24 is 124)
    double savedAtMs;         // the mod's clock when it was saved: the moment of the mark
    unsigned short year, month, day, hour, minute, second, millisecond, unused;
    char why[80];
};
static_assert(sizeof(MotionFileHeader) == 128, "the recordings' header layout: see tools\\motion\\read_motion.ps1");

// Adds this frame to what is kept. The oldest is dropped once there are two minutes' worth.
void MotionAdd(const MotionFrame& frame);

// The name that this moment's files start with, next to the game's exe:
// "BunkerVR\recordings\20261101_201105_mark7". The folder is made if need be.
void MotionFilePrefix(char* out, int outSize, int mark);

// Saves what is kept to "<prefix>_motion.bin", on a thread of its own, so the
// current frame isn't held up by the disk.
void MotionSave(const char* prefix, const char* why, int mark, int modVersion);

// How many saves are still being written (for the tests).
int MotionSavesRunning();
