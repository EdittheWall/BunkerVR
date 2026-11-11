#pragma once

// Tiny logger. Everything goes to BunkerVR.log next to the game's exe.
// The file is started fresh each time the game launches; earlier runs'
// logs are kept as BunkerVR_before1.log and up (see LogInit).

void LogInit();
void Log(const char* format, ...);
