#pragma once

// A direct line between the game's scripts and the mod: the mod adds
// functions of its own to the game's script language, which the scripts call
// like any of the game's (cVR_Version(), cVR_Active()), instead of passing
// numbers through the camera's roll.
//
// Called once from DllMain: it only swaps one address in the game's import
// table (SteamAPI_Init, which the game's main calls first), which is safe
// there. The rest happens on the game's own thread when it calls that:
// before the game sets up its scripts, so there is no race with the mod's
// other hooks, which are installed on a thread of their own.
void InstallScriptFunctions();
