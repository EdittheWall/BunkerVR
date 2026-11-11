#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "camera.h"
#include "clock.h"
#include "enginetime.h"
#include "frame.h"
#include "grips.h"
#include "gui.h"
#ifdef BUNKERVR_COPY_LOCK
#include "licence.h"
#endif
#include "log.h"
#include "scene.h"
#include "scriptfuncs.h"
#include "settings.h"
#include "sight.h"
#include "player_build.h"
#include "vr.h"

// Runs on its own thread shortly after the game starts loading.
// Setup work goes here rather than in DllMain, because Windows holds an
// internal lock while DllMain runs and very little is safe to do there.
static DWORD WINAPI SetupThread(LPVOID)
{
    LoadGrips(); // how items sit in the hand (loaded long before the game's scripts ask)

    if (InstallFrameHook())
        Log("Frame hook installed.");
    else
        Log("ERROR: could not install the frame hook (SDL_GL_SwapWindow not found).");

    InstallClockHooks(); // writes its own line to the log

    if (InstallCameraHook())
        Log("Camera hook installed.");
    else
        Log("ERROR: could not install the camera hook. Head tracking will not affect the game.");

    if (!GuiInstallHook())
        Log("ERROR: could not install the interface hook. The HUD and menus will stay drawn on the 3D picture.");

    CameraInstallFadeHook(); // the game's fades to black as the engine has them; writes its own line to the log
    EngineTimeInstall(); // the game's own update timed by part; writes its own line to the log

    if (!InstallSceneHook())
        Log("ERROR: could not install the scene hook. Drawing times will not be logged and stereo in one frame is not available.");

    SightInstall(); // the creatures see the player at the real head's height; writes its own line to the log
    return 0;
}

// Windows calls DllMain when the game loads this DLL.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LogInit();

        char exePath[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);

        Log("BunkerVR loaded%s (version %d.%02d).", kPlayerBuild ? ", the player build" : "", BUNKERVR_VERSION_HUNDREDTHS / 100, BUNKERVR_VERSION_HUNDREDTHS % 100);
        LoadSettings();
        Log("Running inside: %s (process id %lu)", exePath, GetCurrentProcessId());

        // The mod's own script functions: here, not on the setup thread, so
        // they are added before the game sets up its scripts (only one
        // address in the import table is swapped at this point).
        InstallScriptFunctions();

#ifdef BUNKERVR_COPY_LOCK
        // A private test build's license check: which Steam account this copy runs for, asked of Steam once
        // the game has started it (licence.h).
        LicenceStart();
#endif

        HANDLE thread = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Nothing the mod pressed stays held for the PC's other programs.
        VrLetGoAtExit();
    }
    return TRUE;
}
