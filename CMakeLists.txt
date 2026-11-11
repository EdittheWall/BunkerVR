cmake_minimum_required(VERSION 3.21)
project(BunkerVR VERSION 2.08 LANGUAGES C CXX)

# The game is a 64-bit program, so the DLL must be 64-bit too.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BunkerVR must be built as 64-bit (x64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The player build is what BunkerVR Setup installs: the same code as the developer build, but without the
# developer menu and developer keys (src/player_build.h), and with Microsoft's C runtime linked in, so it runs on
# a PC without Visual Studio.
option(BUNKERVR_PLAYER_BUILD "Build for players: no developer menu or keys, C runtime linked in" OFF)

# Private test builds only: ties a build to listed Steam accounts. Needs src/licence.cpp, which the public
# repository does not include.
option(BUNKERVR_COPY_LOCK "Tie the build to listed Steam accounts (private test builds)" OFF)
if(BUNKERVR_COPY_LOCK AND NOT EXISTS "${CMAKE_SOURCE_DIR}/src/licence.cpp")
    message(FATAL_ERROR "BUNKERVR_COPY_LOCK needs src/licence.cpp, which is not in this copy of the source.")
endif()

if(BUNKERVR_PLAYER_BUILD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# OpenXR is the standard way to talk to VR headsets. The official SDK is fetched from GitHub the first time the
# project is configured, and its loader (which finds the installed runtime: Meta, SteamVR, ...) is linked in.
include(FetchContent)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.63
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openxr)
if(BUNKERVR_PLAYER_BUILD)
    # The loader picks its own C runtime; the player build links it in like everything else.
    set_target_properties(openxr_loader PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# MinHook redirects functions inside the running game (third_party/minhook, BSD 2-Clause license).
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# Dear ImGui draws the hand-pose tool's window on the PC's screen (third_party/imgui, MIT license, version
# 1.92.9b with its Win32 and DirectX 11 backends). Its settings for this mod are in src/imgui_bunkervr.h.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends src)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="imgui_bunkervr.h")

add_library(bunkervr SHARED
    src/camera.cpp
    src/capture.cpp
    src/clock.cpp
    src/dedication.cpp
    src/dllmain.cpp
    src/enginetime.cpp
    src/frame.cpp
    src/gl_helpers.cpp
    src/grips.cpp
    src/gui.cpp
    src/hints.cpp
    src/import_hook.cpp
    src/log.cpp
    src/motion.cpp
    src/pattern.cpp
    src/pointer.cpp
    src/posetool.cpp
    src/proxy.cpp
    src/sampler.cpp
    src/scene.cpp
    src/scriptfuncs.cpp
    src/settings.cpp
    src/sight.cpp
    src/standin_watch.cpp
    src/testlist.cpp
    src/version.def
    src/vr.cpp
    src/vrmenu.cpp
)

# The version above in hundredths, for the scripts' cVR_Version() (2.08 is 208). Worked out here because CMake
# keeps a leading zero ("2.08" gives "08"), which C would read as an octal number.
math(EXPR BUNKERVR_VERSION_HUNDREDTHS "${PROJECT_VERSION_MAJOR} * 100 + ${PROJECT_VERSION_MINOR}")
target_compile_definitions(bunkervr PRIVATE BUNKERVR_VERSION_HUNDREDTHS=${BUNKERVR_VERSION_HUNDREDTHS})

if(BUNKERVR_PLAYER_BUILD)
    target_compile_definitions(bunkervr PRIVATE BUNKERVR_PLAYER_BUILD=1)
    # Only the debug file's name goes into the DLL, not the folder it was built in.
    target_link_options(bunkervr PRIVATE "/PDBALTPATH:%_PDB%")
    # The studio card at the start, already drawn (src/dedication.cpp): its font, Bodoni MT, comes with Microsoft
    # Office, so a player's PC may not have it.
    set(BUNKERVR_STUDIO_CARD "${CMAKE_SOURCE_DIR}/assets/studio_card.bin")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/studio_card.rc" CONTENT "201 RCDATA \"${BUNKERVR_STUDIO_CARD}\"\n")
    target_sources(bunkervr PRIVATE "${CMAKE_BINARY_DIR}/studio_card.rc")
    set_source_files_properties("${CMAKE_BINARY_DIR}/studio_card.rc" PROPERTIES OBJECT_DEPENDS "${BUNKERVR_STUDIO_CARD}")
endif()

if(BUNKERVR_COPY_LOCK)
    target_sources(bunkervr PRIVATE src/licence.cpp)
    target_compile_definitions(bunkervr PRIVATE BUNKERVR_COPY_LOCK=1)
    target_link_libraries(bunkervr PRIVATE bcrypt wintrust crypt32)
endif()

# OpenGL is what the game draws with.
target_link_libraries(bunkervr PRIVATE opengl32 OpenXR::openxr_loader minhook)

# The hand-pose tool draws with DirectX 11. Those parts of Windows are delay-loaded, so they load only if the tool
# is ever opened, and the game starts exactly as before otherwise.
target_link_libraries(bunkervr PRIVATE imgui d3d11 d3dcompiler dwmapi delayimp)
target_link_options(bunkervr PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll)

# The game loads a file called version.dll from its own folder (see src/proxy.cpp).
set_target_properties(bunkervr PROPERTIES OUTPUT_NAME "version" PREFIX "")

# The watcher (src/watch.cpp): a small program the mod starts beside the game, which releases any key or button
# the mod holds down if the game crashes, is stopped from outside, or hangs. Its C runtime is always linked in, so
# it needs nothing else.
add_executable(bunkervr_watch WIN32 src/watch.cpp)
set_target_properties(bunkervr_watch PROPERTIES OUTPUT_NAME "BunkerVR_watch"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${BUNKERVR_PLAYER_BUILD}>>>:Debug>")
if(BUNKERVR_PLAYER_BUILD)
    target_link_options(bunkervr_watch PRIVATE "/PDBALTPATH:%_PDB%")
endif()
if(BUNKERVR_COPY_LOCK)
    target_compile_definitions(bunkervr_watch PRIVATE BUNKERVR_COPY_LOCK=1)
endif()
add_dependencies(bunkervr bunkervr_watch)

# Optionally copy each build into the game's folder (the one with AmnesiaTheBunker.exe). Empty: no copy; install
# with tools/install_dev.ps1 instead. When this project sits inside the game's folder, that folder is the default.
if(EXISTS "${CMAKE_SOURCE_DIR}/../AmnesiaTheBunker.exe")
    get_filename_component(BUNKERVR_DEFAULT_GAME_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
else()
    set(BUNKERVR_DEFAULT_GAME_DIR "")
endif()
set(GAME_DIR "${BUNKERVR_DEFAULT_GAME_DIR}" CACHE PATH "Folder with AmnesiaTheBunker.exe to copy each build into (empty: none)")
if(GAME_DIR)
    add_custom_command(TARGET bunkervr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bunkervr>" "${GAME_DIR}/version.dll"
        COMMENT "Copying version.dll into the game folder"
    )
    add_custom_command(TARGET bunkervr_watch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:bunkervr_watch>" "${GAME_DIR}/BunkerVR_watch.exe"
        COMMENT "Copying BunkerVR_watch.exe into the game folder"
    )
endif()
