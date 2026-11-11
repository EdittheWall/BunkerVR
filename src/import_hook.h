#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Redirects one function that `module` imports from another DLL.
//
// Every exe keeps a table of addresses for the functions it uses from DLLs.
// This swaps one entry in that table for `replacement`, so the next time the
// exe calls that function it lands in our code instead. `original` receives
// the address that was there before, so we can still call the real function.
//
// Returns false if the module does not import that function.
bool HookImport(HMODULE module, const char* dllName, const char* functionName,
                void* replacement, void** original);
