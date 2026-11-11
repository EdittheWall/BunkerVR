#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Searches a loaded module (the game's exe) for a sequence of bytes.
//
// Game updates move functions to different addresses, but the instructions
// inside a function usually stay the same. So instead of remembering an
// address, we remember what the start of the function looks like and search
// for it. "??" in the pattern means "any byte" (used for parts that do change).
//
// `matchCount` receives how many places matched. A pattern is only trustworthy
// if it matches exactly once. Returns the first match, or nullptr.
void* FindPattern(HMODULE module, const char* pattern, int* matchCount);
