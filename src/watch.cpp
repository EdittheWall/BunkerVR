// BunkerVR_watch.exe <the game's process id>: the mod's watcher.
//
// The mod presses mouse buttons and keys through Windows (the right mouse
// button while aiming the revolver, for example). If the game stops while one
// is held, it stays held for the whole PC. The mod lets go of what it holds
// when the game ends normally (VrLetGoAtExit), but a crash, or the game being
// stopped from outside, runs none of its code. So the mod starts this beside
// the game (no window; it reads and writes nothing of the player's). It waits
// on the game's process and reads what the mod holds down (watch_shared.h):
//   - the game ended, however: everything still held is let go at once, and
//     how the game ended is logged (its exit code: 0 is a normal end);
//   - the game has drawn no frame for kWatchStallMs (it hangs, or a long
//     load) with something held: that is let go, and the mod is told (letGo),
//     so that it presses again what it still wants once it runs on.
// Its log: BunkerVR_watch.log beside the game, written afresh each start.
// In private test builds only (BUNKERVR_COPY_LOCK), BunkerVR_watch.exe
// /message <n> shows why the mod refused to run (licence.cpp,
// licence_messages.h), once the game is closed.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef BUNKERVR_COPY_LOCK
#include "licence_messages.h"
#endif
#include "watch_shared.h"

static FILE* g_log = nullptr;

static void Say(const char* format, ...)
{
    if (!g_log) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static void Append(char* text, size_t size, const char* words)
{
    size_t length = strlen(text);
    if (length + strlen(words) + 3 >= size) return;
    if (length > 0) strcat_s(text, size, ", ");
    strcat_s(text, size, words);
}

// Lets go of everything the mod holds down; says what in `what`. Returns how many.
static int LetGo(WatchShared* shared, char* what, size_t size)
{
    what[0] = '\0';
    INPUT inputs[48] = {};
    UINT count = 0;
    LONG mouse = InterlockedExchange(&shared->mouseHeld, 0);
    const DWORD ups[3] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    const char* const names[3] = { "the left mouse button", "the right mouse button", "the middle mouse button" };
    for (int button = 0; button < 3; ++button)
    {
        if ((mouse & (1 << button)) == 0) continue;
        INPUT& input = inputs[count++];
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = ups[button];
        input.mi.dwExtraInfo = kBunkerVRInputMark;
        Append(what, size, names[button]);
    }
    for (int word = 0; word < 16; ++word)
    {
        LONG bits = InterlockedExchange(&shared->keyHeld[word], 0);
        for (int bit = 0; bit < 32 && bits != 0; ++bit)
        {
            if ((bits & (1 << bit)) == 0) continue;
            bits &= ~(1 << bit);
            if (count >= 48) continue;
            int index = word * 32 + bit;
            INPUT& input = inputs[count++];
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = (WORD)(index & 0xFF);
            input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (index >= 256 ? KEYEVENTF_EXTENDEDKEY : 0);
            input.ki.dwExtraInfo = kBunkerVRInputMark;
            char name[48];
            snprintf(name, sizeof(name), "the key of scan code 0x%02X%s", index & 0xFF, index >= 256 ? " (extended)" : "");
            Append(what, size, name);
        }
    }
    if (count > 0) SendInput(count, inputs, sizeof(INPUT));
    return (int)count;
}

static bool AnythingHeld(const WatchShared* shared)
{
    if (shared->mouseHeld != 0) return true;
    for (int word = 0; word < 16; ++word)
        if (shared->keyHeld[word] != 0) return true;
    return false;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
#ifdef BUNKERVR_COPY_LOCK
    // BunkerVR_watch.exe /message <n>: this copy of the mod refused to run (licence.cpp); the game has been
    // closed, and this says why (licence_messages.h), in its own window. It touches no log.
    if (commandLine != nullptr && wcsncmp(commandLine, L"/message ", 9) == 0)
    {
        MessageBoxW(nullptr, LicenceMessageText(_wtoi(commandLine + 9)), L"BunkerVR", MB_OK | MB_ICONSTOP | MB_TOPMOST | MB_SETFOREGROUND);
        return 0;
    }
#endif

    wchar_t logPath[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    while (length > 0 && logPath[length - 1] != L'\\') --length;
    logPath[length] = L'\0';
    if (length + 24 < MAX_PATH)
    {
        wcscat_s(logPath, MAX_PATH, L"BunkerVR_watch.log");
        g_log = _wfsopen(logPath, L"w", _SH_DENYNO);
    }

    DWORD processId = wcstoul(commandLine != nullptr ? commandLine : L"", nullptr, 10);
    if (processId == 0)
    {
        Say("no game's process id given: nothing to watch.");
        return 1;
    }
    HANDLE game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (game == nullptr)
    {
        Say("the game's process %lu could not be opened (error %lu): nothing to watch.", processId, GetLastError());
        return 2;
    }
    wchar_t name[64];
    WatchSharedName(processId, name, 64);
    HANDLE mapping = nullptr;
    for (int attempt = 0; attempt < 50 && mapping == nullptr; ++attempt)
    {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (mapping == nullptr) Sleep(100);
    }
    WatchShared* shared = mapping != nullptr ? (WatchShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WatchShared)) : nullptr;
    if (shared == nullptr || shared->version != kWatchVersion)
    {
        Say("what the mod holds could not be read (%s): nothing to watch.", shared == nullptr ? "no shared memory" : "another version");
        return 3;
    }
    Say("watching the game (process %lu): anything the mod holds down is let go if the game ends or stops drawing for %llu ms.",
        processId, kWatchStallMs);

    int stallsSaid = 0;
    for (;;)
    {
        DWORD waited = WaitForSingleObject(game, 200);
        if (waited == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(game, &code);
            char what[1024];
            int count = LetGo(shared, what, sizeof(what));
            Say("the game ended (exit code 0x%08lX: %s); %s%s.", code, code == 0 ? "a normal end" : "not a normal end, a crash or stopped from outside",
                count > 0 ? "let go of " : "nothing was held", what);
            break;
        }
        if (waited != WAIT_TIMEOUT)
        {
            Say("waiting on the game failed (error %lu): no longer watching.", GetLastError());
            break;
        }
        LONG64 beat = shared->beatMs;
        unsigned long long now = GetTickCount64();
        if (beat > 0 && now > (unsigned long long)beat && now - (unsigned long long)beat > kWatchStallMs && AnythingHeld(shared))
        {
            char what[1024];
            int count = LetGo(shared, what, sizeof(what));
            InterlockedExchange(&shared->letGo, 1);
            if (count > 0 && stallsSaid < 20)
            {
                ++stallsSaid;
                Say("the game has drawn no frame for %llu ms: let go of %s.", now - (unsigned long long)beat, what);
            }
        }
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(game);
    if (g_log) fclose(g_log);
    return 0;
}
