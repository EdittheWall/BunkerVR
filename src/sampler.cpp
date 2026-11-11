#include "sampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "log.h"

// A sampling profiler of the game's main thread (see sampler.h). The main thread is stopped only
// for GetThreadContext and a copy of the top of its stack: no allocation, lock or log meanwhile, so it can
// never be left waiting on something the stopped thread holds. Everything else is done after it goes on.

static volatile bool g_armed = false;
static volatile LONG g_generation = 0;   // a newer arming ends an older sampler thread
static char g_mark[64] = "";
static int g_wantedEye = 0;
static double g_seconds = 0.0;

// The main thread, and the top of its stack (the highest address: its stack grows down from there).
static volatile DWORD g_mainId = 0;
static volatile ULONG_PTR g_stackTop = 0;

// The window: the mark open in the wanted eye's drawing (written by the main thread only).
static volatile LONG g_windowDepth = 0;
static LARGE_INTEGER g_windowStart = {};
static unsigned g_windows = 0;
static double g_windowMs = 0.0;

// The exe's code, as loaded.
static ULONG_PTR g_exeBase = 0, g_codeStart = 0, g_codeEnd = 0;

// Counts by place.
struct PlaceCount
{
    ULONG_PTR place;
    unsigned count;
};
static const int kPlaces = 8192;           // (a power of two)
static PlaceCount g_atInExe[kPlaces];       // where it was, in the exe
static PlaceCount g_onStack[kPlaces];       // the exe's return addresses near the top of the stack (once a sample each)
static const int kModules = 64;
static PlaceCount g_atInModule[kModules];   // where it was outside the exe, by module (its base; 0 not known)
static unsigned g_samples = 0, g_outsideExe = 0, g_droppedPlaces = 0;

static const size_t kStackCopy = 8192;
static unsigned char g_stackCopy[kStackCopy];

static void CountPlace(PlaceCount* table, int slots, ULONG_PTR place)
{
    unsigned hash = (unsigned)((place >> 2) * 2654435761u);
    for (int k = 0; k < slots; ++k)
    {
        PlaceCount& slot = table[(hash + (unsigned)k) & (unsigned)(slots - 1)];
        if (slot.place == place)
        {
            ++slot.count;
            return;
        }
        if (slot.place == 0)
        {
            slot.place = place;
            slot.count = 1;
            return;
        }
    }
    ++g_droppedPlaces;
}

static void CountModule(ULONG_PTR base)
{
    for (int i = 0; i < kModules; ++i)
    {
        if (g_atInModule[i].count != 0 && g_atInModule[i].place == base)
        {
            ++g_atInModule[i].count;
            return;
        }
        if (g_atInModule[i].count == 0)
        {
            g_atInModule[i].place = base;
            g_atInModule[i].count = 1;
            return;
        }
    }
}

static bool FindExeCode()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    if (exe == nullptr) return false;
    const unsigned char* base = (const unsigned char*)exe;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    ULONG_PTR start = 0, end = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        ULONG_PTR from = (ULONG_PTR)base + section->VirtualAddress;
        ULONG_PTR to = from + section->Misc.VirtualSize;
        if (start == 0 || from < start) start = from;
        if (to > end) end = to;
    }
    g_exeBase = (ULONG_PTR)base;
    g_codeStart = start;
    g_codeEnd = end;
    return start != 0 && end > start;
}

// An address as disassemblers number it (at the exe's preferred base, 0x140000000).
static unsigned long long PreferredBaseAddress(ULONG_PTR place)
{
    return 0x140000000ull + (unsigned long long)(place - g_exeBase);
}

static int ByCount(const void* a, const void* b)
{
    unsigned ca = ((const PlaceCount*)a)->count, cb = ((const PlaceCount*)b)->count;
    return ca < cb ? 1 : (ca > cb ? -1 : 0);
}

static void LogTop(const char* title, PlaceCount* table, int slots, int most)
{
    static PlaceCount sorted[kPlaces];
    int used = 0;
    for (int i = 0; i < slots; ++i)
        if (table[i].count > 0) sorted[used++] = table[i];
    qsort(sorted, (size_t)used, sizeof(PlaceCount), ByCount);
    char text[1000];
    int length = snprintf(text, sizeof(text), "SAMPLER: %s:", title);
    for (int i = 0; i < used && i < most; ++i)
    {
        char item[64];
        int itemLength = snprintf(item, sizeof(item), " %llX %.1f%%", PreferredBaseAddress(sorted[i].place),
                                  g_samples > 0 ? 100.0 * sorted[i].count / g_samples : 0.0);
        if (length + itemLength >= 900)
        {
            Log("%s", text);
            length = snprintf(text, sizeof(text), "SAMPLER: (%s, more):", title);
        }
        memcpy(text + length, item, (size_t)itemLength + 1);
        length += itemLength;
    }
    Log("%s", text);
}

static void Report()
{
    Log("SAMPLER: \"%s\" in %s: %u samples in %u windows (%.3f ms a window on average, %.0f ms in all); places not counted: %u.",
        g_mark, g_wantedEye == 1 ? "the left eye's drawing" : (g_wantedEye == 2 ? "the right eye's drawing" : "any drawing"),
        g_samples, g_windows, g_windows > 0 ? g_windowMs / g_windows : 0.0, g_windowMs, g_droppedPlaces);
    if (g_samples == 0) return;
    char text[1000];
    int length = snprintf(text, sizeof(text), "SAMPLER: where it was: the exe %.1f%%", 100.0 * (g_samples - g_outsideExe) / g_samples);
    for (int i = 0; i < kModules && g_atInModule[i].count > 0; ++i)
    {
        wchar_t path[MAX_PATH] = L"";
        if (g_atInModule[i].place != 0) GetModuleFileNameW((HMODULE)g_atInModule[i].place, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name != nullptr ? name + 1 : (path[0] != L'\0' ? path : L"(no module)");
        char item[160];
        int itemLength = snprintf(item, sizeof(item), " | %ls %.1f%%", name, 100.0 * g_atInModule[i].count / g_samples);
        if (length + itemLength >= 900) break;
        memcpy(text + length, item, (size_t)itemLength + 1);
        length += itemLength;
    }
    Log("%s.", text);
    LogTop("the exe's instructions it was at most often (address, share of the samples)", g_atInExe, kPlaces, 24);
    LogTop("the exe's return addresses on its stack most often (address, share of the samples)", g_onStack, kPlaces, 48);
}

static DWORD WINAPI SamplerThread(void* parameter)
{
    LONG generation = (LONG)(LONG_PTR)parameter;
    HANDLE main = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainId);
    if (main == nullptr)
    {
        Log("SAMPLER: the main thread could not be opened (error %lu): nothing sampled.", GetLastError());
        g_armed = false;
        return 0;
    }
    LARGE_INTEGER frequency, now, until, next = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    until.QuadPart = now.QuadPart + (LONGLONG)(g_seconds * (double)frequency.QuadPart);
    LONGLONG step = frequency.QuadPart / 20000; // about every 50 microseconds while the window is open
    while (g_generation == generation)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= until.QuadPart) break;
        if (g_windowDepth <= 0 || now.QuadPart < next.QuadPart)
        {
            YieldProcessor();
            continue;
        }
        next.QuadPart = now.QuadPart + step;

        // Stopped only for this.
        if (SuspendThread(main) == (DWORD)-1) continue;
        alignas(16) CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool got = GetThreadContext(main, &context) != FALSE;
        bool inWindow = g_windowDepth > 0;
        size_t copied = 0;
        ULONG_PTR top = g_stackTop;
        if (got && inWindow && context.Rsp != 0 && top > context.Rsp)
        {
            copied = (size_t)(top - context.Rsp);
            if (copied > kStackCopy) copied = kStackCopy;
            memcpy(g_stackCopy, (const void*)context.Rsp, copied);
        }
        ResumeThread(main);
        if (!got || !inWindow) continue;

        ++g_samples;
        ULONG_PTR at = (ULONG_PTR)context.Rip;
        if (at >= g_codeStart && at < g_codeEnd) CountPlace(g_atInExe, kPlaces, at);
        else
        {
            ++g_outsideExe;
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &module);
            CountModule((ULONG_PTR)module);
        }
        // The exe's code addresses near the top of the stack: the calls it is in (and some stale values; the
        // ones seen in most samples are the calls).
        ULONG_PTR seen[96];
        int seenCount = 0;
        for (size_t offset = 0; offset + sizeof(ULONG_PTR) <= copied; offset += sizeof(ULONG_PTR))
        {
            ULONG_PTR value;
            memcpy(&value, g_stackCopy + offset, sizeof(value));
            if (value < g_codeStart || value >= g_codeEnd) continue;
            bool again = false;
            for (int i = 0; i < seenCount && !again; ++i) again = seen[i] == value;
            if (again) continue;
            if (seenCount < 96) seen[seenCount++] = value;
            CountPlace(g_onStack, kPlaces, value);
        }
    }
    CloseHandle(main);
    if (g_generation == generation)
    {
        g_armed = false;
        Report();
    }
    return 0;
}

void SamplerArm(const char* mark, int eye, double seconds)
{
    if (mark == nullptr || mark[0] == '\0' || seconds <= 0.0) return;
    if (g_mainId == 0 || g_stackTop == 0 || (g_codeEnd == 0 && !FindExeCode()))
    {
        Log("SAMPLER: not ready (the main thread or the exe's code not known yet): nothing sampled.");
        return;
    }
    InterlockedIncrement(&g_generation); // an older sampler thread ends
    g_armed = false;
    Sleep(5);
    strncpy_s(g_mark, sizeof(g_mark), mark, _TRUNCATE);
    g_wantedEye = eye == 1 || eye == 2 ? eye : 0;
    g_seconds = seconds > 120.0 ? 120.0 : seconds;
    memset(g_atInExe, 0, sizeof(g_atInExe));
    memset(g_onStack, 0, sizeof(g_onStack));
    memset(g_atInModule, 0, sizeof(g_atInModule));
    g_samples = g_outsideExe = g_droppedPlaces = 0;
    g_windows = 0;
    g_windowMs = 0.0;
    g_windowDepth = 0;
    g_armed = true;
    HANDLE thread = CreateThread(nullptr, 0, SamplerThread, (void*)(LONG_PTR)g_generation, 0, nullptr);
    if (thread == nullptr)
    {
        g_armed = false;
        Log("SAMPLER: its thread could not be started (error %lu).", GetLastError());
        return;
    }
    CloseHandle(thread);
    Log("SAMPLER: sampling \"%s\" in %s for %.0f s.", g_mark,
        g_wantedEye == 1 ? "the left eye's drawing" : (g_wantedEye == 2 ? "the right eye's drawing" : "any drawing"), g_seconds);
}

bool SamplerArmed()
{
    return g_armed;
}

void SamplerMarkBegin(const char* name, int eye)
{
    if (!g_armed || name == nullptr) return;
    if (g_windowDepth <= 0 && g_wantedEye != 0 && eye != g_wantedEye) return;
    if (strncmp(name, g_mark, sizeof(g_mark)) != 0) return;
    if (InterlockedIncrement(&g_windowDepth) == 1)
    {
        QueryPerformanceCounter(&g_windowStart);
        ++g_windows;
    }
}

void SamplerMarkEnd(const char* name, int eye)
{
    (void)eye;
    if (!g_armed || name == nullptr || g_windowDepth <= 0) return;
    if (strncmp(name, g_mark, sizeof(g_mark)) != 0) return;
    if (InterlockedDecrement(&g_windowDepth) == 0)
    {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        g_windowMs += (double)(now.QuadPart - g_windowStart.QuadPart) * 1000.0 / (double)frequency.QuadPart;
    }
}

void SamplerMainThreadHere()
{
    g_mainId = GetCurrentThreadId();
    g_stackTop = (ULONG_PTR)((NT_TIB*)NtCurrentTeb())->StackBase;
    // No mark of the wanted name is open between frames: a window left open by a missed end is closed.
    if (g_windowDepth > 0) g_windowDepth = 0;
}
