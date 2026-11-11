#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <MinHook.h>

#include "log.h"
#include "standin_watch.h"

// Only in a run with the stand-in OpenXR runtime, to find the cause of single stalls of 200
// to 430 ms. Nothing here runs when the game is played in a real headset.
//
// When no frame has been finished for a while, a thread of its own suspends the drawing
// thread for a moment, copies the top of its stack, resumes it, and only then logs the
// return addresses it found there. Nothing that could wait for the suspended thread is done
// while it is suspended (no log, no memory allocated). Scanning the stack for values that
// point into the game's code, instead of unwinding it properly, can also show stale values:
// trust the ones that repeat.

static volatile LONGLONG g_lastFrameTicks = 0;
static volatile DWORD g_frameThread = 0;
static LONGLONG g_ticksPerMs = 1;

typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileWFn g_realCreateFileW = nullptr;
static volatile LONG g_fileLinesLeft = 40000;

static HANDLE WINAPI HookedCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    // Leaves out the stand-in runtime's own files, which are read many times a second.
    if (name && (access & GENERIC_READ) && !wcsstr(name, L"\\fakexr\\") && !wcsstr(name, L"\\\\.\\") && InterlockedDecrement(&g_fileLinesLeft) > 0)
        Log("STOPWATCH: opens %ls%s", name, GetCurrentThreadId() == g_frameThread ? " (on the drawing thread)" : "");
    return g_realCreateFileW(name, access, share, security, disposition, flags, templateFile);
}

static DWORD WINAPI WatchThread(void*)
{
    const char* image = (const char*)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* headers = (const IMAGE_NT_HEADERS*)(image + ((const IMAGE_DOS_HEADER*)image)->e_lfanew);
    const SIZE_T imageSize = headers->OptionalHeader.SizeOfImage;

    static unsigned long long stack[2048]; // 16 KB
    HANDLE thread = nullptr;
    DWORD threadId = 0;
    LONGLONG stopSeen = 0;
    double nextSampleMs = 50.0;
    int samplesLeft = 2000;

    for (;;)
    {
        Sleep(5);
        DWORD wanted = g_frameThread;
        LONGLONG last = g_lastFrameTicks;
        if (wanted == 0 || last == 0) continue;
        if (wanted != threadId)
        {
            if (thread) CloseHandle(thread);
            thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, wanted);
            threadId = wanted;
            if (!thread) { Log("STOPWATCH: the drawing thread could not be opened."); continue; }
        }
        if (!thread) continue;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double overdueMs = (double)(now.QuadPart - last) / (double)g_ticksPerMs;
        if (last != stopSeen) { stopSeen = last; nextSampleMs = 50.0; }
        if (overdueMs < nextSampleMs || samplesLeft <= 0) continue;
        nextSampleMs *= 2.0; // samples at 50, 100, 200, 400 ms and so on into one stall
        --samplesLeft;

        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        int count = 0;
        if (SuspendThread(thread) == (DWORD)-1) continue;
        if (GetThreadContext(thread, &context))
        {
            MEMORY_BASIC_INFORMATION region = {};
            if (VirtualQuery((const void*)context.Rsp, &region, sizeof(region)) == sizeof(region))
            {
                SIZE_T room = (SIZE_T)((const char*)region.BaseAddress + region.RegionSize - (const char*)context.Rsp);
                SIZE_T bytes = room < sizeof(stack) ? room : sizeof(stack);
                memcpy(stack, (const void*)context.Rsp, bytes);
                count = (int)(bytes / sizeof(stack[0]));
            }
        }
        ResumeThread(thread);

        // The thread is running again, so the log may wait for whatever it needs.
        char text[900];
        int used = snprintf(text, sizeof(text), "STOPWATCH: overdue %.0f ms: at %llx;", overdueMs,
                            (unsigned long long)context.Rip - (unsigned long long)image);
        int found = 0;
        for (int i = 0; i < count && found < 24 && used < (int)sizeof(text) - 24; ++i)
        {
            unsigned long long value = stack[i];
            if (value < (unsigned long long)image || value >= (unsigned long long)image + imageSize) continue;
            used += snprintf(text + used, sizeof(text) - used, " %llx", value - (unsigned long long)image);
            ++found;
        }
        Log("%s", text);
    }
}

void StandInWatchStart()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_ticksPerMs = frequency.QuadPart / 1000;

    // MinHook is already initialized by the camera's hooks.
    void* target = (void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW");
    MH_STATUS status = target ? MH_CreateHook(target, (LPVOID)HookedCreateFileW, (LPVOID*)&g_realCreateFileW) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (status == MH_OK) status = MH_EnableHook(target);
    HANDLE thread = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    Log("STOPWATCH: watching %s; the files %s.", thread ? "for long stops" : "nothing (no thread)", status == MH_OK ? "too" : "not (the hook failed)");
}

void StandInWatchFrame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_frameThread = GetCurrentThreadId();
    g_lastFrameTicks = now.QuadPart;
}
