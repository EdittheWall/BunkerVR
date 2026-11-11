#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "motion.h"

// Two minutes at the headset's 90 frames a second: 2.4 MB, made the first time
// a frame comes.
static const int kFramesKept = 120 * 90;
static MotionFrame* g_frames = nullptr;
static int g_nextFrame = 0;  // where the next one goes
static int g_frameCount = 0; // how many are kept

static volatile LONG g_savesRunning = 0;

struct SaveJob
{
    wchar_t path[MAX_PATH];
    char name[128];
    MotionFileHeader header;
    MotionFrame* frames;
};

void MotionAdd(const MotionFrame& frame)
{
    if (!g_frames)
    {
        g_frames = (MotionFrame*)malloc(sizeof(MotionFrame) * kFramesKept);
        if (!g_frames) return;
    }
    g_frames[g_nextFrame] = frame;
    g_nextFrame = (g_nextFrame + 1) % kFramesKept;
    if (g_frameCount < kFramesKept) ++g_frameCount;
}

// The folder of the game's exe, with its last "\".
static DWORD ExeFolder(wchar_t* path)
{
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    return length;
}

void MotionFilePrefix(char* out, int outSize, int mark)
{
    wchar_t folder[MAX_PATH];
    ExeFolder(folder);
    if (lstrlenW(folder) + 32 < MAX_PATH)
    {
        lstrcatW(folder, L"BunkerVR");
        CreateDirectoryW(folder, nullptr); // harmless if it already exists
        lstrcatW(folder, L"\\recordings");
        CreateDirectoryW(folder, nullptr);
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    snprintf(out, (size_t)outSize, "BunkerVR\\recordings\\%04d%02d%02d_%02d%02d%02d_mark%d", now.wYear, now.wMonth, now.wDay,
             now.wHour, now.wMinute, now.wSecond, mark);
}

static DWORD WINAPI SaveThread(void* parameter)
{
    SaveJob* job = (SaveJob*)parameter;
    HANDLE file = CreateFileW(job->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Log("MOTION ERROR: could not create %s", job->name);
    }
    else
    {
        DWORD written = 0, bytes = job->header.frameCount * (DWORD)sizeof(MotionFrame);
        BOOL good = WriteFile(file, &job->header, sizeof(job->header), &written, nullptr) && written == sizeof(job->header);
        good = good && WriteFile(file, job->frames, bytes, &written, nullptr) && written == bytes;
        CloseHandle(file);
        if (good) Log("MOTION: saved %s (%u frames).", job->name, job->header.frameCount);
        else Log("MOTION ERROR: could not write all of %s", job->name);
    }
    free(job->frames);
    delete job;
    InterlockedDecrement(&g_savesRunning);
    return 0;
}

void MotionSave(const char* prefix, const char* why, int mark, int modVersion)
{
    if (!g_frames || g_frameCount == 0)
    {
        Log("MOTION: nothing to save yet (no frame has been kept).");
        return;
    }
    SaveJob* job = new SaveJob();
    job->frames = (MotionFrame*)malloc(sizeof(MotionFrame) * (size_t)g_frameCount);
    if (!job->frames)
    {
        delete job;
        return;
    }
    // Oldest first: the kept frames start at the next one to be overwritten
    // once the store is full, and at the beginning until then.
    int first = g_frameCount == kFramesKept ? g_nextFrame : 0;
    int toEnd = kFramesKept - first;
    if (toEnd > g_frameCount) toEnd = g_frameCount;
    memcpy(job->frames, g_frames + first, sizeof(MotionFrame) * (size_t)toEnd);
    if (toEnd < g_frameCount) memcpy(job->frames + toEnd, g_frames, sizeof(MotionFrame) * (size_t)(g_frameCount - toEnd));

    snprintf(job->name, sizeof(job->name), "%s_motion.bin", prefix);
    DWORD length = ExeFolder(job->path);
    MultiByteToWideChar(CP_ACP, 0, job->name, -1, job->path + length, (int)(MAX_PATH - length));
    job->path[MAX_PATH - 1] = L'\0';

    MotionFileHeader& header = job->header;
    memcpy(header.magic, "BVRMOT1", 8);
    header.frameSize = (unsigned)sizeof(MotionFrame);
    header.frameCount = (unsigned)g_frameCount;
    header.mark = mark;
    header.modVersion = modVersion;
    header.savedAtMs = job->frames[g_frameCount - 1].timeMs;
    SYSTEMTIME now;
    GetLocalTime(&now);
    header.year = now.wYear;
    header.month = now.wMonth;
    header.day = now.wDay;
    header.hour = now.wHour;
    header.minute = now.wMinute;
    header.second = now.wSecond;
    header.millisecond = now.wMilliseconds;
    snprintf(header.why, sizeof(header.why), "%s", why);

    InterlockedIncrement(&g_savesRunning);
    HANDLE thread = CreateThread(nullptr, 0, SaveThread, job, 0, nullptr);
    if (thread)
    {
        CloseHandle(thread);
    }
    else
    {
        // No thread could be started: write it here instead, a short hold-up rather than nothing.
        SaveThread(job);
    }
}

int MotionSavesRunning()
{
    return (int)g_savesRunning;
}
