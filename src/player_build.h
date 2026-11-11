#pragma once

// Is this the build made for players (CMake option BUNKERVR_PLAYER_BUILD)?
// The same code as the developer build, with Microsoft's C runtime linked in
// statically and its developer-only parts left out: no developer menu and no
// developer keys. A press of Y opens the game's own menu instead of the mod's
// menu (held, it still re-centers; the start-up page, shown on the mod's menu
// panel, still appears and Y still skips it), and F2, F3, F4, F5, F6, F7, F8,
// F11 and End do nothing. Home (re-center) and F9 (pictures of the eyes) still
// work.
#ifdef BUNKERVR_PLAYER_BUILD
constexpr bool kPlayerBuild = true;
#else
constexpr bool kPlayerBuild = false;
#endif
