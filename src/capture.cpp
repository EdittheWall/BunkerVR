#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "capture.h"
#include "gl_helpers.h"
#include "log.h"

enum class Stage
{
    Idle,
    FirstEye,   // waiting to grab the first eye's picture
    SecondEye,  // waiting to grab the other eye's picture
    Timing,     // measuring the delay
};

static Stage g_stage = Stage::Idle;
static bool g_requested = false;
static int g_captureNumber = 0;

static int g_width = 0, g_height = 0;
static unsigned char* g_pictures[2] = { nullptr, nullptr }; // full pictures, one per eye
static unsigned char* g_scratch = nullptr;
// The eye whose picture was grabbed first. On the cutscene screen and while loading, every frame is
// one flat picture handed over as the left eye's, so the same eye comes twice; without this the right
// eye's file would keep the picture of the previous capture.
static int g_firstEye = -1;

// Timing measurement
static unsigned long long g_tiltSerial = 0; // the frame that gets tilted
static const int kSampleCount = 4096;
static unsigned char g_samples[3][kSampleCount]; // brightness samples of the last 3 frames
static bool g_sampleValid[3] = { false, false, false };

// The interface's picture, saved after the eyes': the number of the capture
// it belongs to, or 0 for none wanted.
static int g_interfaceWanted = 0;

// What a named capture's pictures are called: "" for the numbered
// BunkerVR_shotN files. The one requested, then the one under way.
static char g_prefixRequested[128] = "";
static char g_prefixNow[128] = "";

static void SavePicture(const unsigned char* bgra, int width, int height, const char* name);

void CaptureRequest()
{
    g_prefixRequested[0] = '\0';
    g_requested = true;
}

void CaptureRequestNamed(const char* prefix)
{
    snprintf(g_prefixRequested, sizeof(g_prefixRequested), "%s", prefix);
    g_requested = true;
}

void CaptureInterfaceOnFrameEnd(unsigned int framebuffer, int width, int height)
{
    if (g_interfaceWanted == 0 || width <= 0 || height <= 0) return;
    int number = g_interfaceWanted;
    g_interfaceWanted = 0;
    unsigned char* picture = (unsigned char*)malloc((size_t)width * (size_t)height * 4);
    if (!picture) return;
    if (GlReadGameImage(picture, width, height, framebuffer))
    {
        // Where nothing is drawn the picture is see-through, which shows as black here.
        char name[160];
        if (g_prefixNow[0]) snprintf(name, sizeof(name), "%s_interface.bmp", g_prefixNow);
        else snprintf(name, sizeof(name), "BunkerVR_shot%d_interface.bmp", number);
        SavePicture(picture, width, height, name);
    }
    else
    {
        Log("CAPTURE ERROR: could not read the interface's picture.");
    }
    free(picture);
}

float CaptureGetTestTilt(unsigned long long serial)
{
    if (g_stage == Stage::Timing && serial == g_tiltSerial) return 0.45f; // about 26 degrees upwards
    return 0.0f;
}

static bool EnsureBuffers(int width, int height)
{
    if (width == g_width && height == g_height && g_pictures[0]) return true;

    free(g_pictures[0]);
    free(g_pictures[1]);
    free(g_scratch);
    size_t bytes = (size_t)width * (size_t)height * 4;
    g_pictures[0] = (unsigned char*)malloc(bytes);
    g_pictures[1] = (unsigned char*)malloc(bytes);
    g_scratch = (unsigned char*)malloc(bytes);
    g_width = width;
    g_height = height;
    return g_pictures[0] && g_pictures[1] && g_scratch;
}

// Saves a picture as a .bmp file next to the game's exe.
static void SavePicture(const unsigned char* bgra, int width, int height, const char* name)
{
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';

    wchar_t wideName[160];
    MultiByteToWideChar(CP_ACP, 0, name, -1, wideName, 160);
    if (lstrlenW(path) + lstrlenW(wideName) >= MAX_PATH) return;
    lstrcatW(path, wideName);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Log("CAPTURE ERROR: could not create %s", name);
        return;
    }

    DWORD pixelBytes = (DWORD)width * (DWORD)height * 4;

    BITMAPFILEHEADER fileHeader = {};
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;

    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(BITMAPINFOHEADER);
    info.biWidth = width;
    info.biHeight = height; // positive = rows stored bottom to top, which is how OpenGL hands them over
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = pixelBytes;

    DWORD written = 0;
    WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
    WriteFile(file, &info, sizeof(info), &written, nullptr);
    WriteFile(file, bgra, pixelBytes, &written, nullptr);
    CloseHandle(file);
    Log("CAPTURE: saved %s (%d x %d)", name, width, height);
}

// Reduces a picture to a small grid of brightness values for comparing frames.
static void TakeSamples(const unsigned char* bgra, int width, int height, unsigned char* samples)
{
    for (int i = 0; i < kSampleCount; ++i)
    {
        int column = (i % 64) * width / 64 + width / 128;
        int row = (i / 64) * height / 64 + height / 128;
        const unsigned char* pixel = bgra + ((size_t)row * (size_t)width + (size_t)column) * 4;
        samples[i] = (unsigned char)((pixel[0] + pixel[1] * 2 + pixel[2]) / 4);
    }
}

static double AverageDifference(const unsigned char* a, const unsigned char* b)
{
    unsigned long long total = 0;
    for (int i = 0; i < kSampleCount; ++i) total += (unsigned long long)abs((int)a[i] - (int)b[i]);
    return (double)total / kSampleCount;
}

void CaptureOnFrameEnd(int eye, unsigned long long serial, int width, int height, unsigned int source)
{
    if (g_stage == Stage::Idle)
    {
        if (!g_requested) return;
        g_requested = false;
        snprintf(g_prefixNow, sizeof(g_prefixNow), "%s", g_prefixRequested);
        if (!EnsureBuffers(width, height)) return;
        ++g_captureNumber;
        Log("CAPTURE %d: starting.", g_captureNumber);
        g_stage = Stage::FirstEye;
    }
    if (width != g_width || height != g_height)
    {
        g_stage = Stage::Idle;
        return;
    }

    if (g_stage == Stage::FirstEye || g_stage == Stage::SecondEye)
    {
        if (!GlReadGameImage(g_pictures[eye], width, height, source))
        {
            Log("CAPTURE ERROR: could not read the game's picture.");
            g_stage = Stage::Idle;
            return;
        }

        char label[96];
        snprintf(label, sizeof(label), "CAPTURE %d: %s eye picture, headset frame %llu", g_captureNumber,
                 eye == 0 ? "LEFT" : "RIGHT", serial);
        CameraLogLastView(label);

        if (g_stage == Stage::FirstEye)
        {
            g_firstEye = eye;
            g_stage = Stage::SecondEye;
            return;
        }

        // The same eye again: one flat picture for both (the cutscene screen, loading). The newer one
        // is saved for both eyes, never the other eye's picture of an earlier capture.
        if (eye == g_firstEye)
        {
            memcpy(g_pictures[1 - eye], g_pictures[eye], (size_t)width * (size_t)height * 4);
            Log("CAPTURE %d: one flat picture for both eyes (the cutscene screen or loading): saved as both.", g_captureNumber);
        }
        g_firstEye = -1;

        // Both eyes grabbed: save them.
        char name[160];
        if (g_prefixNow[0]) snprintf(name, sizeof(name), "%s_left.bmp", g_prefixNow);
        else snprintf(name, sizeof(name), "BunkerVR_shot%d_left.bmp", g_captureNumber);
        SavePicture(g_pictures[0], width, height, name);
        if (g_prefixNow[0]) snprintf(name, sizeof(name), "%s_right.bmp", g_prefixNow);
        else snprintf(name, sizeof(name), "BunkerVR_shot%d_right.bmp", g_captureNumber);
        SavePicture(g_pictures[1], width, height, name);
        g_interfaceWanted = g_captureNumber; // the interface's picture next: CaptureInterfaceOnFrameEnd

        // A named capture leaves out the timing test.
        if (g_prefixNow[0])
        {
            Log("CAPTURE %d: finished.", g_captureNumber);
            g_stage = Stage::Idle;
            return;
        }

        // Then measure the delay: tilt one frame, a few frames from now.
        g_tiltSerial = serial + 6;
        g_sampleValid[0] = g_sampleValid[1] = g_sampleValid[2] = false;
        g_stage = Stage::Timing;
        Log("CAPTURE %d: timing test: headset frame %llu will be tilted upwards.", g_captureNumber, g_tiltSerial);
        return;
    }

    if (g_stage == Stage::Timing)
    {
        if (serial + 3 < g_tiltSerial) return; // not yet near the tilted frame

        if (!GlReadGameImage(g_scratch, width, height, source))
        {
            g_stage = Stage::Idle;
            return;
        }

        // Compare with the picture from two frames ago, which was the same eye.
        int slot = (int)(serial % 3);
        int twoAgo = (int)((serial + 1) % 3); // (serial - 2) mod 3
        TakeSamples(g_scratch, width, height, g_samples[slot]);
        g_sampleValid[slot] = true;

        if (g_sampleValid[twoAgo])
        {
            double difference = AverageDifference(g_samples[slot], g_samples[twoAgo]);
            long long offset = (long long)serial - (long long)g_tiltSerial;
            Log("CAPTURE %d: timing: picture at frame %+lld (relative to the tilted frame) differs from 2 frames earlier by %.1f",
                g_captureNumber, offset, difference);
        }

        if (serial >= g_tiltSerial + 6)
        {
            Log("CAPTURE %d: finished.", g_captureNumber);
            g_stage = Stage::Idle;
        }
    }
}
