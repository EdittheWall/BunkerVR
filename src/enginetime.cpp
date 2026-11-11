#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>
#include "enginetime.h"
#include "log.h"
#include "pattern.h"
#include "sampler.h"

// The game's own update timed part by part. The scripts' own timing marks
// cover only the mod's scripts; this shows where the rest of the game's
// update goes.
//
// The engine brackets many pieces of its work with two calls: one at the
// start (name, source file, line) and one at the end (name), for example
// "UpdatePhysics" and "NewtonUpdate" in PhysicsWorldNewton.cpp. Both do
// nothing unless the engine's profiler is on (a global pointer, empty in the
// shipped game). Here each pair is timed on the game's main thread; the
// engine's own calls still run unchanged. A part's time includes whatever it
// calls inside it. Each pattern is the function's first bytes, the addresses
// it reads left out; each is found once in the game.
static const char* kMarkBeginPattern =
    "48 8B C4 44 89 40 18 55 53 56 48 8D 68 A1 48 81 EC E0 00 00 00 48 83 3D ?? ?? ?? ?? 00 48 8B DA 48 8B F1 0F 84 ?? ?? ?? ??";
static const char* kMarkEndPattern =
    "48 83 EC 48 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 0F 57 C0 0F 57 C9 0F 11 44 24 20 49 C7 C0 FF FF FF FF F3 0F 7F 4C 24 30";

typedef void (*MarkBeginFunction)(const char* name, const char* file, int line);
typedef void (*MarkEndFunction)(const char* name);
static MarkBeginFunction g_realMarkBegin = nullptr;
static MarkEndFunction g_realMarkEnd = nullptr;

static volatile DWORD g_mainThread = 0;
static double g_ticksToMs = 0.0;

// Each part met so far, by its name, in a table with linear probing on the
// name's text: some names are the engine's own fixed words, many are text it
// makes afresh each time (a module's name and "_Update"), so the start and the
// end of one part come with different pointers, and the text is what they
// share (going by the pointer pairs parts wrongly). A part begun again before
// it ended starts again.
static const int kNameMost = 48;
struct EnginePart
{
    char name[kNameMost];
    unsigned hash;
    bool used;
    bool open;
    long long start;
    double ms;
    unsigned long long count;
    int openEye;            // the eye being drawn as it began (EngineTimeEye)
    double eyeMs[2];        // ...its time within the left eye's drawing, and within the right's
    unsigned long long eyeCount[2];
};
static const int kPartsMost = 1024; // (a power of two)
static EnginePart g_parts[kPartsMost];
static int g_partsUsed = 0;
static unsigned long long g_partsLeftOut = 0;

// Timed only while the timing lines come every 10 seconds (a level's first
// minute, see frame.cpp): each mark costs a little (some 900 a frame).
static volatile bool g_active = false;

// The eye whose world is being drawn: 0 none, 1 the left, 2 the right. Set and read on the main thread
// only.
static int g_eye = 0;

// When the engine's last loop ended: the time from there to the next loop's start is timed as a part of its own,
// "(between the engine's loops)". Without it, time spent outside the engine's loop would show in no part at all
// (a frame of 16.7 ms can have only about 5 ms inside the loop).
static const char* kLoopName = "EngineLoop";
static const char* kBetweenLoopsName = "(between the engine's loops)";
static long long g_loopEnded = 0;
static bool g_loopEndedKnown = false;

static unsigned NameHash(const char* name)
{
    unsigned hash = 2166136261u;
    for (int i = 0; i < kNameMost - 1 && name[i] != '\0'; ++i) hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    return hash;
}

// The part of that name, made if new; -1 if the table is full.
static int PartFor(const char* name)
{
    unsigned hash = NameHash(name);
    for (int k = 0; k < kPartsMost; ++k)
    {
        int i = (int)((hash + (unsigned)k) & (kPartsMost - 1));
        EnginePart& part = g_parts[i];
        if (!part.used)
        {
            if (g_partsUsed >= kPartsMost * 3 / 4) break; // kept well short of full, so probing stays short
            strncpy_s(part.name, kNameMost, name, _TRUNCATE);
            part.hash = hash;
            part.used = true;
            part.open = false;
            part.ms = 0.0;
            part.count = 0;
            part.openEye = 0;
            part.eyeMs[0] = part.eyeMs[1] = 0.0;
            part.eyeCount[0] = part.eyeCount[1] = 0;
            ++g_partsUsed;
            return i;
        }
        if (part.hash == hash && strncmp(part.name, name, kNameMost - 1) == 0) return i;
    }
    ++g_partsLeftOut;
    return -1;
}

static bool OnMainThread()
{
    return g_active && g_mainThread != 0 && GetCurrentThreadId() == g_mainThread;
}

static void HookedMarkBegin(const char* name, const char* file, int line)
{
    // The sampling profiler's window: on the main thread whether or not the timing is on.
    if (SamplerArmed() && g_mainThread != 0 && GetCurrentThreadId() == g_mainThread) SamplerMarkBegin(name, g_eye);
    if (name != nullptr && OnMainThread())
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_loopEndedKnown && strcmp(name, kLoopName) == 0)
        {
            g_loopEndedKnown = false;
            int between = PartFor(kBetweenLoopsName);
            if (between >= 0)
            {
                g_parts[between].ms += (double)(now.QuadPart - g_loopEnded) * g_ticksToMs;
                ++g_parts[between].count;
            }
        }
        int part = PartFor(name);
        if (part >= 0)
        {
            g_parts[part].start = now.QuadPart;
            g_parts[part].open = true;
            g_parts[part].openEye = g_eye;
        }
    }
    g_realMarkBegin(name, file, line);
}

static void HookedMarkEnd(const char* name)
{
    g_realMarkEnd(name);
    if (SamplerArmed() && g_mainThread != 0 && GetCurrentThreadId() == g_mainThread) SamplerMarkEnd(name, g_eye);
    if (name == nullptr || !OnMainThread()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (strcmp(name, kLoopName) == 0)
    {
        g_loopEnded = now.QuadPart;
        g_loopEndedKnown = true;
    }
    int part = PartFor(name);
    if (part < 0 || !g_parts[part].open) return;
    double ms = (double)(now.QuadPart - g_parts[part].start) * g_ticksToMs;
    g_parts[part].ms += ms;
    ++g_parts[part].count;
    g_parts[part].open = false;
    // Within one eye's drawing from its start to its end: counted for that eye too.
    if (g_eye > 0 && g_parts[part].openEye == g_eye)
    {
        g_parts[part].eyeMs[g_eye - 1] += ms;
        ++g_parts[part].eyeCount[g_eye - 1];
    }
}

void EngineTimeEye(int eye)
{
    g_eye = eye == 1 || eye == 2 ? eye : 0;
}
bool EngineTimeInstall()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_ticksToMs = 1000.0 / (double)frequency.QuadPart;

    HMODULE game = GetModuleHandleW(nullptr);
    int beginMatches = 0;
    int endMatches = 0;
    void* begin = FindPattern(game, kMarkBeginPattern, &beginMatches);
    void* end = FindPattern(game, kMarkEndPattern, &endMatches);
    if (beginMatches != 1 || endMatches != 1)
    {
        Log("ENGINE ERROR: expected to find the engine's timing marks once each, found them %d and %d times (the game's own update is not timed by part).",
            beginMatches, endMatches);
        return false;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("ENGINE ERROR: could not start the hooking library for the timing marks: %s", MH_StatusToString(status));
        return false;
    }
    status = MH_CreateHook(begin, (LPVOID)HookedMarkBegin, (LPVOID*)&g_realMarkBegin);
    if (status == MH_OK) status = MH_CreateHook(end, (LPVOID)HookedMarkEnd, (LPVOID*)&g_realMarkEnd);
    if (status == MH_OK) status = MH_EnableHook(begin);
    if (status == MH_OK) status = MH_EnableHook(end);
    if (status != MH_OK)
    {
        Log("ENGINE ERROR: could not hook the engine's timing marks: %s", MH_StatusToString(status));
        return false;
    }
    Log("ENGINE: timing the game's own update by part (its timing marks at exe+0x%llX and exe+0x%llX).",
        (unsigned long long)((char*)begin - (char*)game), (unsigned long long)((char*)end - (char*)game));
    return true;
}

void EngineTimeFrame()
{
    g_mainThread = GetCurrentThreadId();
}

void EngineTimeActive(bool active)
{
    if (active && !g_active)
    {
        // Starting again: nothing left open from before counts.
        for (int i = 0; i < kPartsMost; ++i) g_parts[i].open = false;
        g_loopEndedKnown = false;
    }
    g_active = active;
}

void EngineTimeLog(unsigned long long frames)
{
    if (frames == 0) return;
    // The parts that took the most, largest first (at most 18 of them).
    static int order[kPartsMost];
    int shown = 0;
    for (int i = 0; i < kPartsMost; ++i)
        if (g_parts[i].used && g_parts[i].count > 0) order[shown++] = i;
    for (int a = 1; a < shown; ++a)
    {
        int index = order[a];
        int b = a - 1;
        while (b >= 0 && g_parts[order[b]].ms < g_parts[index].ms)
        {
            order[b + 1] = order[b];
            --b;
        }
        order[b + 1] = index;
    }
    if (shown > 0)
    {
        // Up to 40 parts, on as many lines as they take: the log's lines end at 1024 characters,
        // which would cut a single line after about 18 parts.
        static const int kPartsShown = 40;
        static const int kLineMost = 900;
        char text[1100];
        int length = snprintf(text, sizeof(text), "ENGINE: the game's own work by part, per frame on average, in ms (each with what it calls inside it)");
        for (int i = 0; i < shown && i < kPartsShown && length > 0; ++i)
        {
            const EnginePart& part = g_parts[order[i]];
            char item[200];
            int itemLength = snprintf(item, sizeof(item), " | %s %.2f (%.1f times)", part.name, part.ms / (double)frames,
                                      (double)part.count / (double)frames);
            if (itemLength <= 0) continue;
            if (length + itemLength > kLineMost)
            {
                Log("%s", text);
                length = snprintf(text, sizeof(text), "ENGINE (more parts):");
            }
            if (length + itemLength < (int)sizeof(text))
            {
                memcpy(text + length, item, (size_t)itemLength + 1);
                length += itemLength;
            }
        }
        if (g_partsLeftOut > 0 && length > 0 && length < kLineMost)
            length += snprintf(text + length, sizeof(text) - length, " | (names left out, the table full: %llu)", g_partsLeftOut);
        Log("%s.", text);
    }

    // Each eye's drawing by part, the right eye's largest first: "name left / right" in ms a
    // frame. What the right eye's drawing costs beyond the left eye's shows as the larger right number.
    int eyeShown = 0;
    for (int i = 0; i < kPartsMost; ++i)
        if (g_parts[i].used && (g_parts[i].eyeCount[0] > 0 || g_parts[i].eyeCount[1] > 0)) order[eyeShown++] = i;
    for (int a = 1; a < eyeShown; ++a)
    {
        int index = order[a];
        int b = a - 1;
        while (b >= 0 && g_parts[order[b]].eyeMs[1] < g_parts[index].eyeMs[1])
        {
            order[b + 1] = order[b];
            --b;
        }
        order[b + 1] = index;
    }
    if (eyeShown > 0)
    {
        static const int kEyePartsShown = 30;
        static const int kLineMost = 900;
        char text[1100];
        int length = snprintf(text, sizeof(text), "ENGINE, each eye's drawing by part, ms a frame, the left eye's / the right eye's");
        for (int i = 0; i < eyeShown && i < kEyePartsShown && length > 0; ++i)
        {
            const EnginePart& part = g_parts[order[i]];
            char item[200];
            int itemLength = snprintf(item, sizeof(item), " | %s %.3f / %.3f", part.name, part.eyeMs[0] / (double)frames,
                                      part.eyeMs[1] / (double)frames);
            if (itemLength <= 0) continue;
            if (length + itemLength > kLineMost)
            {
                Log("%s", text);
                length = snprintf(text, sizeof(text), "ENGINE, each eye's drawing (more parts):");
            }
            if (length + itemLength < (int)sizeof(text))
            {
                memcpy(text + length, item, (size_t)itemLength + 1);
                length += itemLength;
            }
        }
        Log("%s.", text);
    }

    for (int i = 0; i < kPartsMost; ++i)
    {
        g_parts[i].ms = 0.0;
        g_parts[i].count = 0;
        g_parts[i].eyeMs[0] = g_parts[i].eyeMs[1] = 0.0;
        g_parts[i].eyeCount[0] = g_parts[i].eyeCount[1] = 0;
    }
    g_partsLeftOut = 0;
}