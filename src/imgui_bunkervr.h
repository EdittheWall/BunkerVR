#pragma once

// Dear ImGui's settings for this mod (third_party\imgui; this file is named
// by IMGUI_USER_CONFIG in CMakeLists.txt, so every ImGui file sees it).

// In a debug build, one of ImGui's own checks failing would stop the whole
// game with a message box. Here it is written to the log instead (once for
// each place in the code), and the hand poses tool carries on (posetool.cpp).
void PoseToolCheckFailed(const char* what, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : PoseToolCheckFailed(#_EXPR, __FILE__, __LINE__))

// No game controller in the tool (the controllers are the headset's), and
// ImGui's own demo windows left out; so are its opening of web links
// (Windows' shell32) and its support for typing in East Asian languages
// (imm32), which the tool doesn't use, so the mod links no extra Windows
// libraries for them.
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
