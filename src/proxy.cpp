// How our code gets into the game
// --------------------------------
// The game uses SDL2.dll, and SDL2.dll asks Windows for "version.dll".
// Windows looks in the game's own folder before the system folder, so a file
// called version.dll placed next to the exe gets loaded instead of the real one.
//
// That means we have to behave like the real version.dll as well. Each function
// below passes the call straight on to the real version.dll in System32.
// version.def gives these functions their real exported names.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const char* const kFunctionNames[] = {
    "GetFileVersionInfoA",        //  0
    "GetFileVersionInfoByHandle", //  1
    "GetFileVersionInfoExA",      //  2
    "GetFileVersionInfoExW",      //  3
    "GetFileVersionInfoSizeA",    //  4
    "GetFileVersionInfoSizeExA",  //  5
    "GetFileVersionInfoSizeExW",  //  6
    "GetFileVersionInfoSizeW",    //  7
    "GetFileVersionInfoW",        //  8
    "VerFindFileA",               //  9
    "VerFindFileW",               // 10
    "VerInstallFileA",            // 11
    "VerInstallFileW",            // 12
    "VerLanguageNameA",           // 13
    "VerLanguageNameW",           // 14
    "VerQueryValueA",             // 15
    "VerQueryValueW",             // 16
};
static const int kFunctionCount = sizeof(kFunctionNames) / sizeof(kFunctionNames[0]);

static FARPROC g_realFunctions[kFunctionCount];
static INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;

// Loads the real version.dll from the Windows system folder. Runs only once.
static BOOL CALLBACK LoadRealDll(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t path[MAX_PATH];
    UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length > MAX_PATH - 16) return TRUE;
    lstrcatW(path, L"\\version.dll");

    HMODULE real = LoadLibraryW(path);
    if (real)
    {
        for (int i = 0; i < kFunctionCount; ++i)
            g_realFunctions[i] = GetProcAddress(real, kFunctionNames[i]);
    }
    return TRUE;
}

// None of these functions takes more than 8 arguments, and on 64-bit Windows
// passing along 8 pointer-sized arguments works for any of them. That lets one
// pattern cover all 17 without spelling out each function's exact arguments.
typedef void* (*PassThrough)(void*, void*, void*, void*, void*, void*, void*, void*);

#define PROXY(index, name)                                                         \
    extern "C" void* Proxy_##name(void* a, void* b, void* c, void* d,              \
                                  void* e, void* f, void* g, void* h)              \
    {                                                                              \
        InitOnceExecuteOnce(&g_loadOnce, LoadRealDll, nullptr, nullptr);           \
        PassThrough real = (PassThrough)g_realFunctions[index];                    \
        return real ? real(a, b, c, d, e, f, g, h) : nullptr;                      \
    }

PROXY(0,  GetFileVersionInfoA)
PROXY(1,  GetFileVersionInfoByHandle)
PROXY(2,  GetFileVersionInfoExA)
PROXY(3,  GetFileVersionInfoExW)
PROXY(4,  GetFileVersionInfoSizeA)
PROXY(5,  GetFileVersionInfoSizeExA)
PROXY(6,  GetFileVersionInfoSizeExW)
PROXY(7,  GetFileVersionInfoSizeW)
PROXY(8,  GetFileVersionInfoW)
PROXY(9,  VerFindFileA)
PROXY(10, VerFindFileW)
PROXY(11, VerInstallFileA)
PROXY(12, VerInstallFileW)
PROXY(13, VerLanguageNameA)
PROXY(14, VerLanguageNameW)
PROXY(15, VerQueryValueA)
PROXY(16, VerQueryValueW)
