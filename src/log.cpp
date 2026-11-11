#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "log.h"
#include "player_build.h"

static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;

void LogInit()
{
    if (g_logReady) return;

    // Build "<folder of the exe>\BunkerVR.log".
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    wchar_t folder[MAX_PATH];
    lstrcpynW(folder, path, MAX_PATH);
    lstrcatW(path, L"BunkerVR.log");

    // The last three runs' logs are kept, so starting the game again does not lose the last run's:
    // BunkerVR_before1.log is the run before this one, then before2 and before3. BunkerVR.log is
    // always this run's. The tester build keeps the last 20, so that the logs BunkerVR Setup
    // gathers cover days of play (a run writes about 3 MB an hour).
    for (int keep = kPlayerBuild ? 20 : 3; keep >= 1; --keep)
    {
        wchar_t from[MAX_PATH], to[MAX_PATH];
        if (keep == 1) lstrcpynW(from, path, MAX_PATH);
        else wsprintfW(from, L"%sBunkerVR_before%d.log", folder, keep - 1);
        wsprintfW(to, L"%sBunkerVR_before%d.log", folder, keep);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING); // fails harmlessly when there is nothing to keep yet
    }

    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
}

void Log(const char* format, ...)
{
    if (!g_logReady || g_logFile == INVALID_HANDLE_VALUE) return;

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    char line[1100];
    int lineLength = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n",
                              now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, message);
    if (lineLength < 0) return;
    if (lineLength > (int)sizeof(line) - 1) lineLength = (int)sizeof(line) - 1;

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_logFile, line, (DWORD)lineLength, &written, nullptr);
    LeaveCriticalSection(&g_logLock);
}
