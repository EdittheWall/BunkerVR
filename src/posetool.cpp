#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "camera.h"
#include "grips.h"
#include "log.h"
#include "posetool.h"
#include "vr.h"

// See posetool.h. Everything here runs on the tool's own thread, apart from
// PoseToolSetOpen and the questions below it. It never touches the poses
// itself: it works on its own copy and asks grips.cpp for every change
// (GripsDeskSetSlot...), which the game's thread makes on its next frame.

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifdef POSETOOL_TEST
// Only in a standalone test build of the tool, never in the mod: the test
// program supplies the mouse and keys for each frame and receives a
// picture of each frame as drawn.
void PoseToolTestFrame(unsigned frame);
void PoseToolTestDrawn(unsigned frame, ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain);
#endif

// ---------------------------------------------------------------------------
// Opening and closing (any thread)
// ---------------------------------------------------------------------------

static volatile LONG g_running = 0;       // the thread exists
static volatile LONG g_closing = 0;       // closing (the window is going away)
static volatile LONG g_inFrontOnOpen = 0; // opened by F11: comes to the front
static HWND volatile g_window = nullptr;
static const UINT kMessageToFront = WM_APP + 1;
static const UINT kMessageQuietClose = WM_APP + 2; // closed from the headset's menu: no asking (changes are kept in the game, unsaved)

// ImGui's own checks (see imgui_bunkervr.h): written to the log, once for
// each place, instead of stopping the game.
void PoseToolCheckFailed(const char* what, const char* file, int line)
{
    static const char* seen[32];
    static int seenLines[32];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (seen[i] == file && seenLines[i] == line) return;
    if (count >= 32) return; // enough logged
    seen[count] = file;
    seenLines[count] = line;
    ++count;
    const char* name = strrchr(file, '\\');
    Log("HAND POSES TOOL: ImGui's check failed (%s, line %d): %s", name ? name + 1 : file, line, what);
}

// ---------------------------------------------------------------------------
// DirectX 11
// ---------------------------------------------------------------------------

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_target = nullptr;
static UINT g_resizeWidth = 0, g_resizeHeight = 0;

static void MakeTarget()
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
    {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

static void DropTarget()
{
    if (g_target)
    {
        g_target->Release();
        g_target = nullptr;
    }
}

static bool MakeDevice(HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc,
                                                   &g_swapChain, &g_device, &got, &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED) // no graphics card support: WARP draws on the processor instead
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &g_swapChain,
                                               &g_device, &got, &g_context);
    if (FAILED(result))
    {
        Log("HAND POSES TOOL: DirectX 11 could not be started (0x%08lx).", (unsigned long)result);
        return false;
    }
    MakeTarget();
    return true;
}

static void DropDevice()
{
    DropTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ---------------------------------------------------------------------------
// The tool's state (its own thread)
// ---------------------------------------------------------------------------

static GripDeskState g_state;                      // the game's latest copy
static ItemGrip g_table[kGripSlotCount][2];        // the tool's working copy
static NamedPose g_library[kPoseLibraryMost];
static int g_libraryCount = 0;
static unsigned g_docVersion = 0;                  // the game's version the working copy was taken from
static unsigned long long g_lastEditMs = 0;        // while things are being changed here, the working copy isn't replaced

static int g_slot = 0;       // the pose chosen on the left
static int g_hand = 1;       // ...and the hand (0 left, 1 right)
static int g_finger = 1;     // the finger chosen (0 thumb .. 4 little)
static int g_stepChoice = 1; // - and + : 0 fine, 1 normal, 2 big
static char g_filter[64] = "";
static int g_librarySel = -1;
static bool g_showGame = false;
static bool g_freeze = false;
static float g_ui = 1.0f;    // the screen's scaling

// "Hand in front of me": the hand being posed floats in front of
// the eyes (VrSetHandInFront), facing: 0 held out, 1 palm to the eyes, 2
// back of the hand to them. It follows the hand chosen at the top.
static bool g_inFront = false;
static int g_inFrontFacing = 0;
static int g_inFrontHandShown = -1; // the hand last put there (-1 none)

// The camera on the hand.
static bool g_camOn = false;
static float g_camDir[3] = { -0.6f, 0.5f, -0.6f }; // from the palm towards the camera, in the hand's own directions
static float g_camDistance = 0.45f;
static float g_camFov = 50.0f;

// Asking before closing with unsaved changes.
static bool g_askClose = false;
static bool g_closeNow = false;

// Undo and redo. A step is one change to the poses of your own, or changes
// to one or more poses (several for "Reset all", undone in one go).
// This file's own types are in an anonymous namespace: another file's type of
// the same name would otherwise be mixed up with them when linked.
namespace
{
struct Change
{
    int slot = 0, hand = 0;
    ItemGrip before, after;
};
struct Step
{
    bool library = false;
    std::vector<Change> changes;
    std::vector<NamedPose> libraryBefore, libraryAfter;
    unsigned long long when = 0;
};
}
static std::vector<Step> g_undo, g_redo;
static bool g_grouping = false; // changes go into g_group, which becomes one step at the end
static Step g_group;

// Naming a pose of your own.
static char g_nameBuffer[kPoseNameMost] = "";
static int g_renaming = -1; // the one being renamed, or -1 for a new one

// ---------------------------------------------------------------------------
// Changes (every one goes to the game and onto the Undo list)
// ---------------------------------------------------------------------------

static bool SameGrip(const ItemGrip& a, const ItemGrip& b)
{
    return memcmp(&a, &b, sizeof(ItemGrip)) == 0;
}

static void SetSlot(int slot, int hand, const ItemGrip& wanted, bool record)
{
    if (!GripSlotHasHand(slot, hand)) return;
    ItemGrip before = g_table[slot][hand];
    if (SameGrip(before, wanted)) return;
    g_table[slot][hand] = wanted;
    GripsDeskSetSlot(slot, hand, wanted);
    unsigned long long now = GetTickCount64();
    g_lastEditMs = now;
    if (!record) return;
    Change change;
    change.slot = slot;
    change.hand = hand;
    change.before = before;
    change.after = wanted;
    if (g_grouping)
    {
        g_group.changes.push_back(change);
        return;
    }
    // Changes to the same pose close together in time (one drag) are one step.
    Step* last = g_undo.empty() ? nullptr : &g_undo.back();
    if (last && !last->library && last->changes.size() == 1 && last->changes[0].slot == slot && last->changes[0].hand == hand && now - last->when < 600)
    {
        last->changes[0].after = wanted;
        last->when = now;
    }
    else
    {
        Step step;
        step.changes.push_back(change);
        step.when = now;
        g_undo.push_back(step);
        if (g_undo.size() > 300) g_undo.erase(g_undo.begin());
    }
    g_redo.clear();
}

// Several changes as one step (see g_grouping): GroupStart(), then
// SetSlot... with record, then GroupEnd().
static void GroupStart()
{
    g_grouping = true;
    g_group = Step();
}

static void GroupEnd()
{
    g_grouping = false;
    if (g_group.changes.empty()) return;
    g_group.when = 0; // never merged with what comes next
    g_undo.push_back(g_group);
    if (g_undo.size() > 300) g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_group = Step();
}

static void SetLibrary(const std::vector<NamedPose>& wanted)
{
    Step step;
    step.library = true;
    step.libraryBefore.assign(g_library, g_library + g_libraryCount);
    step.libraryAfter = wanted;
    step.when = GetTickCount64();
    g_libraryCount = (int)wanted.size() < kPoseLibraryMost ? (int)wanted.size() : kPoseLibraryMost;
    for (int i = 0; i < g_libraryCount; ++i) g_library[i] = wanted[i];
    GripsDeskSetLibrary(g_library, g_libraryCount);
    g_lastEditMs = step.when;
    g_undo.push_back(step);
    g_redo.clear();
}

static void ApplyLibrary(const std::vector<NamedPose>& poses)
{
    g_libraryCount = (int)poses.size() < kPoseLibraryMost ? (int)poses.size() : kPoseLibraryMost;
    for (int i = 0; i < g_libraryCount; ++i) g_library[i] = poses[i];
    GripsDeskSetLibrary(g_library, g_libraryCount);
    g_lastEditMs = GetTickCount64();
}

// A step's changes put back (undo) or made again (redo). A step of one
// change shows that pose.
static void ApplyStep(const Step& step, bool undo)
{
    if (step.library)
    {
        ApplyLibrary(undo ? step.libraryBefore : step.libraryAfter);
        return;
    }
    for (size_t i = 0; i < step.changes.size(); ++i)
    {
        const Change& c = step.changes[undo ? step.changes.size() - 1 - i : i];
        const ItemGrip& grip = undo ? c.before : c.after;
        g_table[c.slot][c.hand] = grip;
        GripsDeskSetSlot(c.slot, c.hand, grip);
    }
    if (step.changes.size() == 1)
    {
        g_slot = step.changes[0].slot;
        g_hand = step.changes[0].hand;
    }
    g_lastEditMs = GetTickCount64();
}

static void Undo()
{
    if (g_undo.empty()) return;
    Step step = g_undo.back();
    g_undo.pop_back();
    ApplyStep(step, true);
    g_redo.push_back(step);
}

static void Redo()
{
    if (g_redo.empty()) return;
    Step step = g_redo.back();
    g_redo.pop_back();
    ApplyStep(step, false);
    step.when = 0; // never merged with what comes next
    g_undo.push_back(step);
}

// The hand being posed put in front of the eyes (or, with g_inFront off,
// back with its controller).
static void PlaceHandInFront()
{
    VrSetHandInFront(g_inFront, g_hand, g_inFrontFacing);
    g_inFrontHandShown = g_inFront ? g_hand : -1;
}

// The chosen pose as it is now, and a change to it.
static ItemGrip Current()
{
    return g_table[g_slot][g_hand];
}

static void Commit(const ItemGrip& wanted)
{
    SetSlot(g_slot, g_hand, wanted, true);
}

// Where the chosen item sits: its own placement, or the game's (known only
// while that hand holds it). False if neither is known.
static bool PlacementShown(ItemGrip& out)
{
    out = Current();
    if (out.set) return true;
    const GripDeskHand& h = g_state.hand[g_hand];
    if ((h.item == g_slot || h.atWork == g_slot) && h.known) // or held in a way the game doesn't report: the bandage's tin while winding
    {
        out.forward = h.game.forward;
        out.up = h.game.up;
        out.right = h.game.right;
        out.turnRight = h.game.turnRight;
        out.tipUp = h.game.tipUp;
        out.leanRight = h.game.leanRight;
        return true;
    }
    return false;
}

static bool InHandNow(int slot, int hand)
{
    const GripDeskHand& h = g_state.hand[hand];
    return g_state.fresh && (h.item == slot || h.atWork == slot);
}

// ---------------------------------------------------------------------------
// Starting points for the fingers
// ---------------------------------------------------------------------------

namespace
{
struct StartingPoint
{
    const char* name;
    const char* help;
    HandPose pose;
};
}

static HandPose MakePose(float t, float i, float m, float r, float l, float st, float si, float sm, float sr, float sl, float cup)
{
    HandPose p;
    p.curl[0] = t; p.curl[1] = i; p.curl[2] = m; p.curl[3] = r; p.curl[4] = l;
    p.spread[0] = st; p.spread[1] = si; p.spread[2] = sm; p.spread[3] = sr; p.spread[4] = sl;
    p.cup = cup;
    return p;
}

static const StartingPoint* StartingPoints(int& count)
{
    static StartingPoint points[] = {
        { "Relaxed", "A hand at rest: fingers a little bent.", MakePose(0.35f, 0.30f, 0.35f, 0.40f, 0.45f, 0, -3, 0, 3, 6, 0.10f) },
        { "Open", "Flat and spread.", MakePose(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 12, -6, 0, 6, 12, 0.0f) },
        { "Fist", "Every finger closed, the thumb over them.", MakePose(0.90f, 1.0f, 1.0f, 1.0f, 1.0f, -10, 0, 0, 0, 0, 0.30f) },
        { "Point", "The first finger straight, the rest closed.", MakePose(0.75f, 0.0f, 1.0f, 1.0f, 1.0f, -10, 0, 0, 0, 0, 0.20f) },
        { "Thumbs up", "The thumb straight up, the rest closed.", MakePose(0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 20, 0, 0, 0, 0, 0.20f) },
        { "Pinch", "Thumb and first finger meeting.", MakePose(0.55f, 0.60f, 0.25f, 0.30f, 0.35f, -15, 0, 0, 3, 6, 0.10f) },
        { "Grip", "Round a handle or a bar.", MakePose(0.60f, 0.75f, 0.80f, 0.85f, 0.90f, -8, 0, 0, 0, 0, 0.20f) },
    };
    count = (int)(sizeof(points) / sizeof(points[0]));
    return points;
}

// ---------------------------------------------------------------------------
// Small pieces of the look
// ---------------------------------------------------------------------------

static const ImVec4 kGreen(0.36f, 0.80f, 0.45f, 1.0f);
static const ImVec4 kOrange(0.95f, 0.68f, 0.25f, 1.0f);
static const ImVec4 kDim(0.58f, 0.61f, 0.66f, 1.0f);

static void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A heading across the column. With help, "(?)" after its name, and the
// help shown while the mouse is over the heading.
static void Section(const char* title, const char* help)
{
    char label[96];
    if (help) snprintf(label, sizeof(label), "%s  (?)", title);
    else snprintf(label, sizeof(label), "%s", title);
    ImGui::SeparatorText(label);
    if (help && ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Buttons in a row that goes on to the next line when it is full:
// lineEnd = FlowStart() before the first, FlowNext(label, lineEnd) before
// each of the others.
static float FlowStart()
{
    return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
}

static void FlowNext(const char* nextLabel, float lineEnd)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = ImGui::CalcTextSize(nextLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= lineEnd) ImGui::SameLine();
}

static void Chip(const char* text, ImU32 colour)
{
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 pad(8.0f * g_ui, 3.0f * g_ui);
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size.x + pad.x * 2, at.y + size.y + pad.y * 2), colour, 9.0f * g_ui);
    draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), IM_COL32(12, 16, 14, 255), text);
    ImGui::Dummy(ImVec2(size.x + pad.x * 2, size.y + pad.y * 2));
}

static float StepSize(bool angle)
{
    if (g_stepChoice == 0) return angle ? 1.0f : 0.001f;
    if (g_stepChoice == 2) return angle ? 20.0f : 0.02f;
    return angle ? 5.0f : 0.005f;
}

// One adjustable row: its name on one line, then
//   [-] [+] less  [==== value ====]  more.
// `shown` turns the number into what is shown (meters into millimeters, 0 to
// 1 into percent).
static bool StepRow(const char* id, const char* name, const char* less, const char* more, float* value, float lowest, float highest,
                    float step, float shown, const char* format)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted(name);
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (ImGui::Button("-")) { *value -= step; changed = true; }
    ImGui::SetItemTooltip("Less (hold to keep going)");
    ImGui::SameLine(0.0f, 4.0f * g_ui);
    if (ImGui::Button("+")) { *value += step; changed = true; }
    ImGui::SetItemTooltip("More (hold to keep going)");
    ImGui::PopItemFlag();
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%s", less);
    ImGui::SameLine();
    float moreWidth = ImGui::CalcTextSize(more).x + ImGui::GetStyle().ItemSpacing.x;
    float width = ImGui::GetContentRegionAvail().x - moreWidth;
    if (width < 60.0f * g_ui) width = 60.0f * g_ui;
    ImGui::SetNextItemWidth(width);
    float seen = *value * shown;
    if (seen > -0.05f && seen < 0.05f) seen = 0.0f; // never shown as "-0"
    if (ImGui::SliderFloat("##v", &seen, lowest * shown, highest * shown, format))
    {
        *value = seen / shown;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%s", more);
    ImGui::PopID();
    if (*value < lowest) *value = lowest;
    if (*value > highest) *value = highest;
    return changed;
}

// ---------------------------------------------------------------------------
// The top
// ---------------------------------------------------------------------------

static void SaveNow()
{
    GripsDeskSave();
}

static const char* HandWord(int hand)
{
    return hand == 0 ? "left" : "right";
}

// A note under the bandage's and the medkit's poses on what they do while a
// bandage is wound: the mod's winding draws the roll in the right hand
// ("Winding the bandage (the roll)") and the bandage's tin in the left
// ("Bandage, left hand"); "Bandage, right hand" and "Medkit, right hand"
// show only when those items are held again.
static void WindingNote()
{
    bool roll = g_slot == kSlotBandageRoll;
    bool bandage = strcmp(GripItemName(g_slot), "Bandage") == 0;
    bool medkit = strcmp(GripItemName(g_slot), "Medkit") == 0;
    const char* text = nullptr;
    bool now = g_state.bandageWinding;
    if (roll && now)
        text = "The roll in your right hand follows this pose now.";
    else if (roll)
        text = "This is the roll in your right hand while a bandage is being wound: it shows then (pull the roll out of the tin).";
    else if (!now)
        return;
    else if (bandage && g_hand == 0)
        text = "The bandage is being wound: the tin in your left hand follows this pose now.";
    else if (bandage)
        text = "The bandage is being wound: its tin is put away from the right hand meanwhile, so this pose shows when the bandage is held "
               "again. The roll in your right hand is \"Winding the bandage (the roll)\" (Hands at work); the tin in your left hand is the "
               "left hand's bandage.";
    else if (medkit && g_hand == 1)
        text = "A bandage is being wound: the medkit is put away from the right hand meanwhile, so this pose shows when it is held again. "
               "The roll in your right hand is \"Winding the bandage (the roll)\" (Hands at work).";
    if (text == nullptr) return;
    ImGui::PushStyleColor(ImGuiCol_Text, now ? kOrange : kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void WhatHandDoes(int hand, char* text, size_t size)
{
    const GripDeskHand& h = g_state.hand[hand];
    if (!g_state.fresh) snprintf(text, size, "?");
    else if (h.atWork >= kGripItemCount) snprintf(text, size, "%s", GripItemTitle(h.atWork));
    else if (h.atWork >= 0) snprintf(text, size, "holding the %s's tin (winding)", GripItemTitle(h.atWork));
    else if (h.item >= 0) snprintf(text, size, "holding the %s", GripItemTitle(h.item));
    else if (hand == 1 && g_state.gunRight) snprintf(text, size, "holding the gun");
    else snprintf(text, size, "nothing");
}

static void TopBar()
{
    ImGui::AlignTextToFramePadding();
    bool ticking =g_state.tickMs != 0 && GetTickCount64() - g_state.tickMs < 1000;
    if (!ticking)
    {
        ImGui::TextColored(kOrange, "Game not updating");
        ImGui::SetItemTooltip("The game has not drawn a frame for a moment. It may pause while this window is in front (the game's own "
                              "setting SleepWhenOutOfFocus). Changes are kept, and show in the game when it runs again.");
    }
    else if (g_state.fresh)
    {
        ImGui::TextColored(kGreen, "Connected");
        ImGui::SetItemTooltip("The game is running and says what each hand holds: changes show in it at once.");
    }
    else
    {
        ImGui::TextColored(kOrange, "Waiting for the game");
        ImGui::SetItemTooltip("The game's scripts are not running just now: a menu, a loading screen, or the game is paused. Changes wait until they are.");
    }
    ImGui::SameLine(0.0f, 18.0f * g_ui);
    ImGui::TextUnformatted("Hand:");
    ImGui::SameLine();
    ImGui::BeginDisabled(!GripSlotHasHand(g_slot, 0));
    if (ImGui::RadioButton("Left", g_hand == 0)) g_hand = 0;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!GripSlotHasHand(g_slot, 1));
    if (ImGui::RadioButton("Right", g_hand == 1)) g_hand = 1;
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 18.0f * g_ui);
    ImGui::BeginDisabled(g_undo.empty());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Z");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_redo.empty());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Y");
    ImGui::SameLine(0.0f, 18.0f * g_ui);
    if (ImGui::Button("Save")) SaveNow();
    ImGui::SetItemTooltip("Ctrl+S. Writes BunkerVR_grips.txt in the game's folder.");
    ImGui::SameLine();
    if (g_state.dirty) ImGui::TextColored(kOrange, "Unsaved changes");
    else ImGui::TextColored(kDim, "All saved");

    // What is going on, as colored chips; and the hands.
    bool live = InHandNow(g_slot, g_hand);
    Chip(live ? "Fingers live" : "Not in that hand now", live ? IM_COL32(92, 205, 115, 255) : IM_COL32(150, 156, 166, 255));
    ImGui::SetItemTooltip(live ? "This pose is what that hand is doing right now: every change shows in the game at once."
                               : "That hand is not doing this just now. Changes are kept, and show when it does.");
    ImGui::SameLine();
    bool camShown = g_camOn && g_state.headsetOff;
    Chip(camShown ? "Camera on hand" : "Camera: your eyes", camShown ? IM_COL32(92, 205, 115, 255) : IM_COL32(150, 156, 166, 255));
    ImGui::SameLine();
    if (g_inFront)
    {
        Chip("Hand in front of you", IM_COL32(92, 205, 115, 255));
        ImGui::SetItemTooltip("The %s hand floats in front of your eyes (\"Hand in front of me\", on the right).", HandWord(g_hand));
        ImGui::SameLine();
    }
    if (g_state.headsetOff) Chip("Headset off: hands held", IM_COL32(242, 174, 64, 255));
    else if (g_state.handsFrozen) Chip("Hands frozen", IM_COL32(242, 174, 64, 255));
    else Chip("Hands follow the controllers", IM_COL32(150, 156, 166, 255));
    ImGui::SameLine(0.0f, 18.0f * g_ui);
    if (ImGui::Checkbox("Freeze my hands", &g_freeze)) VrSetHandsFrozen(g_freeze);
    Help("Holds both hands where they are now, so you can put the controllers down and pose at the PC. (With the headset off they are held anyway.)");

    char left[96], right[96];
    WhatHandDoes(0, left, sizeof(left));
    WhatHandDoes(1, right, sizeof(right));
    ImGui::TextColored(kDim, "Left hand: %s.  Right hand: %s.", left, right);
    ImGui::SameLine(0.0f, 18.0f * g_ui);
    ImGui::TextDisabled("F11: open or close this window");
}

// ---------------------------------------------------------------------------
// The left: every pose
// ---------------------------------------------------------------------------

namespace
{
struct Category
{
    const char* name;
    int slots[12];
    int count;
};
}

static const Category kCategories[] = {
    { "Light and fire", { 10, 20, 12, 19, 11 }, 5 },
    { "Throwing", { 14, 15, 5, 24, 25 }, 5 },
    { "Healing and food", { 23, 22, 26 }, 3 },
    { "Fuel and bottles", { 0, 4, 2, 1, 3 }, 5 },
    { "Tools, keys and parts", { 17, 16, 32, 27, 28, 30, 31 }, 7 },
    { "Other things", { 18, 13, 29, 21, 8, 9 }, 6 },
    { "Hands at work", { kSlotGunSupport, kSlotBullet, kSlotCord, kSlotDoor, kSlotLoose, kSlotBandageRoll, kSlotDogtag, kSlotShotgunPump }, 8 },
};

static bool Matches(int slot)
{
    if (g_filter[0] == '\0') return true;
    char a[64], b[64];
    snprintf(a, sizeof(a), "%s", GripItemTitle(slot));
    snprintf(b, sizeof(b), "%s", g_filter);
    for (char* c = a; *c; ++c) *c = (char)tolower((unsigned char)*c);
    for (char* c = b; *c; ++c) *c = (char)tolower((unsigned char)*c);
    return strstr(a, b) != nullptr;
}

static void Marker(bool own, bool live)
{
    ImVec2 at = ImGui::GetCursorScreenPos();
    float r = 4.0f * g_ui;
    ImVec2 centre(at.x + r + 2.0f * g_ui, at.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (own) draw->AddCircleFilled(centre, r, live ? IM_COL32(92, 205, 115, 255) : IM_COL32(120, 170, 240, 255));
    else draw->AddCircle(centre, r, IM_COL32(120, 126, 136, 255), 0, 1.2f * g_ui);
    ImGui::Dummy(ImVec2(r * 2 + 6.0f * g_ui, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
}

static void PoseRow(int slot, int hand)
{
    const ItemGrip& g = g_table[slot][hand];
    bool own = g.set || g.fingersSet;
    bool live = InHandNow(slot, hand);
    ImGui::PushID(slot * 2 + hand);
    Marker(own, live);
    char label[96];
    snprintf(label, sizeof(label), "%s  %s%s", hand == 0 ? "L" : "R", slot >= kGripItemCount ? GripItemTitle(slot) : (hand == 0 ? "in the left hand" : "in the right hand"),
             live ? "  - now" : "");
    if (ImGui::Selectable(label, g_slot == slot && g_hand == hand))
    {
        g_slot = slot;
        g_hand = hand;
    }
    ImGui::PopID();
}

static void PoseList()
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##find", "Find an item...", g_filter, sizeof(g_filter));
    if (ImGui::Button("Go to what my hands do now"))
    {
        for (int hand = 1; hand >= 0; --hand)
        {
            const GripDeskHand& h = g_state.hand[hand];
            int slot = h.atWork >= 0 ? h.atWork : h.item;
            if (g_state.fresh && slot >= 0)
            {
                g_slot = slot;
                g_hand = hand;
                break;
            }
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("tree", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 9.5f), ImGuiChildFlags_None);
    for (const Category& category : kCategories)
    {
        bool any = false;
        for (int i = 0; i < category.count; ++i) any = any || Matches(category.slots[i]);
        if (!any) continue;
        if (g_filter[0] != '\0') ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (!ImGui::TreeNodeEx(category.name, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (int i = 0; i < category.count; ++i)
        {
            int slot = category.slots[i];
            if (!Matches(slot)) continue;
            if (slot >= kGripItemCount)
            {
                for (int hand = 1; hand >= 0; --hand)
                    if (GripSlotHasHand(slot, hand)) PoseRow(slot, hand);
                continue;
            }
            ImGui::PushID(slot);
            bool open = ImGui::TreeNodeEx(GripItemTitle(slot), (g_slot == slot ? ImGuiTreeNodeFlags_DefaultOpen : 0));
            if (open)
            {
                PoseRow(slot, 1);
                PoseRow(slot, 0);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();

    // Poses of your own.
    Section("My own poses", "Hand poses you name yourself, to start others from: \"Save as my own pose\" in the middle, then \"Use on this pose\" here.");
    ImGui::BeginChild("library", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 4.0f), ImGuiChildFlags_Borders);
    if (g_libraryCount == 0) ImGui::TextColored(kDim, "None yet.");
    for (int i = 0; i < g_libraryCount; ++i)
    {
        ImGui::PushID(1000 + i);
        if (ImGui::Selectable(g_library[i].name, g_librarySel == i)) g_librarySel = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    bool chosen = g_librarySel >= 0 && g_librarySel < g_libraryCount;
    ImGui::BeginDisabled(!chosen);
    if (ImGui::Button("Use on this pose"))
    {
        ItemGrip g = Current();
        g.fingersSet = true;
        g.pose = g_library[g_librarySel].pose;
        Commit(g);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename"))
    {
        g_renaming = g_librarySel;
        snprintf(g_nameBuffer, sizeof(g_nameBuffer), "%s", g_library[g_librarySel].name);
        ImGui::OpenPopup("Name your pose");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
    {
        std::vector<NamedPose> poses(g_library, g_library + g_libraryCount);
        poses.erase(poses.begin() + g_librarySel);
        SetLibrary(poses);
        g_librarySel = -1;
    }
    ImGui::EndDisabled();

    // The name, for a new pose or a renamed one.
    if (ImGui::BeginPopupModal("Name your pose", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(g_renaming >= 0 ? "A new name for this pose:" : "A name for your pose:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##name", g_nameBuffer, sizeof(g_nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        bool ok = (ImGui::Button("OK") || enter) && g_nameBuffer[0] != '\0';
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if (ok)
        {
            for (char* c = g_nameBuffer; *c; ++c)
                if (*c == '"') *c = '\''; // the file keeps names in double quotes
            std::vector<NamedPose> poses(g_library, g_library + g_libraryCount);
            if (g_renaming >= 0 && g_renaming < (int)poses.size())
            {
                snprintf(poses[g_renaming].name, sizeof(poses[g_renaming].name), "%s", g_nameBuffer);
            }
            else if ((int)poses.size() < kPoseLibraryMost)
            {
                NamedPose named;
                snprintf(named.name, sizeof(named.name), "%s", g_nameBuffer);
                named.pose = Current().pose;
                poses.push_back(named);
                g_librarySel = (int)poses.size() - 1;
            }
            SetLibrary(poses);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Saving.
    ImGui::Separator();
    if (ImGui::Button("Save")) SaveNow();
    ImGui::SameLine();
    if (ImGui::Button("Back to last saved")) ImGui::OpenPopup("Go back to what was saved?");
    ImGui::SameLine();
    if (g_state.dirty) ImGui::TextColored(kOrange, "Unsaved changes");
    else ImGui::TextColored(kDim, "All saved");
    if (ImGui::BeginPopupModal("Go back to what was saved?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Go back to what was last saved? The changes since are lost.");
        if (ImGui::Button("Go back"))
        {
            GripsDeskRevert();
            g_undo.clear();
            g_redo.clear();
            g_lastEditMs = 0;
            g_docVersion = 0; // the working copy is taken again from the game's
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("Kept in BunkerVR_grips.txt, in the game's folder.");
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// The middle: the fingers
// ---------------------------------------------------------------------------

static const char* const kFingerNames[5] = { "Thumb", "First finger", "Middle finger", "Ring finger", "Little finger" };
static const float kFingerLength[5] = { 1.25f, 1.60f, 1.75f, 1.60f, 1.25f };
static const float kSegment[3] = { 0.45f, 0.32f, 0.23f };

// Where each joint of a finger is in the drawing (hand units: the wrist at
// 0 0, the knuckles at 2 up; x to the right). palmView: looking at the palm
// (the bend comes towards you, so the finger looks shorter); otherwise from
// the thumb's side (the palm to the right).
static void FingerPoints(const HandPose& p, int finger, bool palmView, float thumbSide, ImVec2 points[4])
{
    float c = p.curl[finger];
    if (finger == 0)
    {
        // Bent as the game bends them (UpdateVRFingersFull in Prop_PlayerBody.hps): a little even when straight.
        float amount = 0.1f + 0.9f * c;
        float bend[3] = { 0.45f * amount, 0.5f * amount, 0.7f * amount };
        float spread = p.spread[0] / 57.2958f;
        if (palmView)
        {
            ImVec2 at(thumbSide * 0.95f, 0.55f);
            points[0] = at;
            float angle = 0.75f + spread; // out from the hand, from straight up
            for (int k = 0; k < 3; ++k)
            {
                angle -= bend[k] * 1.4f; // curling swings it across the palm
                float length = kFingerLength[0] * (k == 0 ? 0.42f : (k == 1 ? 0.32f : 0.26f)) * (1.0f - 0.25f * c);
                at = ImVec2(at.x + thumbSide * sinf(angle) * length, at.y + cosf(angle) * length);
                points[k + 1] = at;
            }
        }
        else
        {
            ImVec2 at(0.25f, 0.55f);
            points[0] = at;
            float angle = 0.35f - spread * 0.6f;
            for (int k = 0; k < 3; ++k)
            {
                angle += bend[k];
                float length = kFingerLength[0] * (k == 0 ? 0.42f : (k == 1 ? 0.32f : 0.26f));
                at = ImVec2(at.x + sinf(angle) * length, at.y + cosf(angle) * length);
                points[k + 1] = at;
            }
        }
        return;
    }
    float amount = 0.1f + 0.9f * c; // as the game does it (gfVRRestCurl)
    float bend[3] = { amount * 1.35f, amount * 1.5f, amount * 0.85f };
    float cupShrink = 1.0f - 0.12f * p.cup;
    float side = finger == 1 ? 0.75f : (finger == 2 ? 0.25f : (finger == 3 ? -0.25f : -0.75f));
    float knuckleY = finger == 4 ? 1.85f : (finger == 1 ? 1.95f : 2.0f);
    if (palmView)
    {
        ImVec2 at(thumbSide * side * cupShrink, knuckleY);
        points[0] = at;
        float spread = p.spread[finger] / 57.2958f;
        ImVec2 direction(-thumbSide * sinf(spread), cosf(spread)); // + towards the little finger's side
        float theta = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            theta += bend[k];
            float along = cosf(theta) * kFingerLength[finger] * kSegment[k];
            at = ImVec2(at.x + direction.x * along, at.y + direction.y * along);
            points[k + 1] = at;
        }
    }
    else
    {
        float behind = (finger - 1) * 0.06f; // the others a little behind the first finger
        ImVec2 at(-behind, knuckleY - behind * 0.5f);
        points[0] = at;
        float theta = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            theta += bend[k];
            float length = kFingerLength[finger] * kSegment[k];
            at = ImVec2(at.x + sinf(theta) * length, at.y + cosf(theta) * length);
            points[k + 1] = at;
        }
    }
}

namespace
{
struct DrawingFrame
{
    ImVec2 origin; // the wrist on the screen
    float scale;   // pixels per hand unit
    ImVec2 Map(ImVec2 p) const { return ImVec2(origin.x + p.x * scale, origin.y - p.y * scale); }
};
}

static int g_dragFinger = -1;
static bool g_dragPalm = true;

// One view of the hand, with fingers that can be dragged.
static void HandDrawing(const char* id, bool palmView, float width, float height, HandPose& pose, bool& changed)
{
    float thumbSide = g_hand == 1 ? 1.0f : -1.0f; // looking at a right palm, the thumb is to the right
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(22, 25, 31, 255), 4.0f * g_ui);
    draw->AddText(ImVec2(at.x + 8.0f * g_ui, at.y + 6.0f * g_ui), IM_COL32(150, 156, 166, 255), palmView ? "Palm" : "Side (thumb side)");

    DrawingFrame view;
    view.scale = height / 4.9f;
    view.origin = ImVec2(at.x + width * 0.5f + (palmView ? 0.0f : -0.4f * view.scale), at.y + height - 0.35f * view.scale);

    // The palm.
    if (palmView)
    {
        float cupShrink = 1.0f - 0.12f * pose.cup;
        ImVec2 a = view.Map(ImVec2(-0.85f, 0.0f)), b = view.Map(ImVec2(0.85f, 0.0f));
        ImVec2 c = view.Map(ImVec2(1.0f * cupShrink, 1.95f)), d = view.Map(ImVec2(-1.0f * cupShrink, 1.95f));
        draw->AddQuadFilled(a, b, c, d, IM_COL32(58, 52, 48, 255));
        draw->AddQuad(a, b, c, d, IM_COL32(110, 100, 92, 255), 1.5f * g_ui);
    }
    else
    {
        ImVec2 a = view.Map(ImVec2(-0.18f, 0.0f)), b = view.Map(ImVec2(0.22f + 0.1f * pose.cup, 0.0f));
        ImVec2 c = view.Map(ImVec2(0.15f, 2.0f)), d = view.Map(ImVec2(-0.18f, 2.0f));
        draw->AddQuadFilled(a, b, c, d, IM_COL32(58, 52, 48, 255));
        draw->AddQuad(a, b, c, d, IM_COL32(110, 100, 92, 255), 1.5f * g_ui);
    }

    // The fingers (the chosen one brighter; the thumb orange).
    ImVec2 tips[5];
    int order[5] = { 4, 3, 2, 1, 0 };
    for (int n = 0; n < 5; ++n)
    {
        int finger = order[n];
        ImVec2 points[4];
        FingerPoints(pose, finger, palmView, thumbSide, points);
        bool chosen = finger == g_finger;
        ImU32 colour = finger == 0 ? (chosen ? IM_COL32(255, 190, 70, 255) : IM_COL32(215, 150, 50, 255))
                                   : (chosen ? IM_COL32(245, 245, 240, 255) : IM_COL32(170, 172, 176, 255));
        if (!palmView && finger > 1 && !chosen) colour = IM_COL32(110, 112, 118, 255);
        float thickness = (finger == 0 ? 0.2f : 0.17f) * view.scale;
        for (int k = 0; k < 3; ++k) draw->AddLine(view.Map(points[k]), view.Map(points[k + 1]), colour, thickness);
        for (int k = 0; k < 4; ++k) draw->AddCircleFilled(view.Map(points[k]), thickness * 0.5f, colour);
        tips[finger] = view.Map(points[3]);
        if (chosen) draw->AddCircle(tips[finger], thickness * 0.9f, IM_COL32(255, 220, 120, 255), 0, 2.0f * g_ui);
    }

    // Dragging a finger: towards the palm curls it, sideways spreads it.
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        float best = 1e9f;
        for (int finger = 0; finger < 5; ++finger)
        {
            float dx = io.MousePos.x - tips[finger].x, dy = io.MousePos.y - tips[finger].y;
            float distance = dx * dx + dy * dy;
            if (!palmView && finger > 1 && finger != g_finger) distance *= 4.0f; // the first finger and the thumb are in front
            if (distance < best)
            {
                best = distance;
                g_dragFinger = finger;
            }
        }
        g_dragPalm = palmView;
        g_finger = g_dragFinger;
    }
    if (active && g_dragFinger >= 0 && g_dragPalm == palmView && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
    {
        int f = g_dragFinger;
        float length = kFingerLength[f] * view.scale;
        if (f == 0)
        {
            // The thumb: across towards the palm curls it; up and down swings it out or across.
            float across = palmView ? -thumbSide * io.MouseDelta.x : io.MouseDelta.x;
            pose.curl[0] += across / length;
            pose.spread[0] -= io.MouseDelta.y / length * 40.0f;
        }
        else
        {
            pose.curl[f] += (io.MouseDelta.y + (palmView ? 0.0f : io.MouseDelta.x)) / (length * 1.1f);
            if (palmView) pose.spread[f] += -thumbSide * io.MouseDelta.x / length * 57.3f;
        }
        changed = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_dragFinger = -1;
    if (hovered && g_dragFinger < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

static void FingersPanel()
{
    ItemGrip g = Current();
    ImGui::TextColored(kGreen, "%s", GripItemTitle(g_slot));
    ImGui::SameLine();
    ImGui::TextColored(kDim, "- %s hand", HandWord(g_hand));
    ImGui::TextWrapped("Drag a finger in the drawing, or use the sliders. Changes show in the game at once.");
    WindingNote();
    ImGui::Spacing();

    if (ImGui::RadioButton("The game's own fingers", !g.fingersSet))
    {
        g.fingersSet = false;
        Commit(g);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("My pose", g.fingersSet))
    {
        g.fingersSet = true;
        Commit(g);
    }
    if (ImGui::Checkbox("Show the game's own fingers", &g_showGame)) GripsSetShowGameFingers(g_showGame);
    Help("For comparing: the game's own fingers show in the game, everywhere, while this is ticked. Your poses are kept.");

    // The drawing.
    HandPose pose = g.pose;
    bool changed = false;
    float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    float height = 210.0f * g_ui;
    if (width < 80.0f * g_ui) width = 80.0f * g_ui;
    HandDrawing("##palm", true, width, height, pose, changed);
    ImGui::SameLine();
    HandDrawing("##side", false, width, height, pose, changed);
    if (!g.fingersSet)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("(The game's own fingers show in the game until you change one.)");
        ImGui::PopStyleColor();
    }

    // The finger chosen.
    Section("One finger", nullptr);
    for (int f = 0; f < 5; ++f)
    {
        if (f > 0) ImGui::SameLine();
        if (ImGui::RadioButton(f == 0 ? "Thumb" : (f == 1 ? "First" : (f == 2 ? "Middle" : (f == 3 ? "Ring" : "Little"))), g_finger == f)) g_finger = f;
    }
    {
        char name[48];
        snprintf(name, sizeof(name), "%s: curl", kFingerNames[g_finger]);
        if (StepRow("curl", name, "Straight", "Closed", &pose.curl[g_finger], 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%")) changed = true;
        snprintf(name, sizeof(name), "%s: spread", kFingerNames[g_finger]);
        if (g_finger == 0)
        {
            if (StepRow("spread", name, "Across", "Out", &pose.spread[0], -60.0f, 60.0f, 5.0f, 1.0f, "%+.0f deg")) changed = true;
        }
        else if (StepRow("spread", name, "Thumb", "Little", &pose.spread[g_finger], -40.0f, 40.0f, 2.0f, 1.0f, "%+.0f deg"))
        {
            changed = true;
        }
    }

    // The whole hand.
    Section("Whole hand", "These move every finger together, keeping how they differ.");
    {
        float before = (pose.curl[1] + pose.curl[2] + pose.curl[3] + pose.curl[4]) / 4.0f;
        float all = before;
        if (StepRow("curlall", "Curl all fingers", "Straight", "Fist", &all, 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%"))
        {
            for (int f = 1; f < 5; ++f) pose.curl[f] += all - before;
            changed = true;
        }
        float spreadBefore = (pose.spread[4] - pose.spread[1]) * 0.5f;
        float spreadAll = spreadBefore;
        if (StepRow("spreadall", "Spread fingers", "Together", "Apart", &spreadAll, -10.0f, 30.0f, 2.0f, 1.0f, "%+.0f deg"))
        {
            float d = spreadAll - spreadBefore;
            pose.spread[1] -= d;
            pose.spread[3] += d * 0.6f;
            pose.spread[4] += d;
            changed = true;
        }
        if (StepRow("cup", "Cup palm", "Flat", "Cupped", &pose.cup, 0.0f, 1.0f, 0.05f, 100.0f, "%.0f%%")) changed = true;
    }

    // Starting points.
    Section("Starting points", "A whole hand at once, to change from there.");
    int count = 0;
    const StartingPoint* points = StartingPoints(count);
    float lineEnd = FlowStart();
    for (int i = 0; i < count; ++i)
    {
        if (i > 0) FlowNext(points[i].name, lineEnd);
        if (ImGui::Button(points[i].name))
        {
            pose = points[i].pose;
            changed = true;
        }
        ImGui::SetItemTooltip("%s", points[i].help);
    }
    if (ImGui::Button("Save as my own pose..."))
    {
        g_renaming = -1;
        g_nameBuffer[0] = '\0';
        ImGui::OpenPopup("Name your pose");
    }
    // The same naming window as on the left.
    if (ImGui::BeginPopupModal("Name your pose", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        bool full = g_libraryCount >= kPoseLibraryMost;
        if (full) ImGui::TextColored(kOrange, "You have %d poses of your own, the most there can be: delete one first.", kPoseLibraryMost);
        ImGui::TextUnformatted("A name for your pose:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##name", g_nameBuffer, sizeof(g_nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(full || g_nameBuffer[0] == '\0');
        bool ok = (ImGui::Button("OK") || enter) && g_nameBuffer[0] != '\0' && !full;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if (ok)
        {
            for (char* c = g_nameBuffer; *c; ++c)
                if (*c == '"') *c = '\'';
            std::vector<NamedPose> poses(g_library, g_library + g_libraryCount);
            NamedPose named;
            snprintf(named.name, sizeof(named.name), "%s", g_nameBuffer);
            named.pose = pose;
            poses.push_back(named);
            SetLibrary(poses);
            g_librarySel = (int)poses.size() - 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    int other = 1 - g_hand;
    FlowNext("Mirror to the other hand", lineEnd);
    ImGui::BeginDisabled(!GripSlotHasHand(g_slot, other));
    if (ImGui::Button("Mirror to the other hand"))
    {
        ItemGrip mirrored = g;
        mirrored.pose = pose;
        mirrored.right = -g.right;
        mirrored.turnRight = -g.turnRight;
        mirrored.leanRight = -g.leanRight;
        SetSlot(g_slot, other, mirrored, true);
    }
    ImGui::SetItemTooltip("The %s hand gets this pose too (fingers, and where the item sits, mirrored).", HandWord(other));
    ImGui::EndDisabled();

    if (changed)
    {
        // Within the limits grips.cpp keeps, so that what is shown here is what the game gets.
        for (int f = 0; f < 5; ++f)
        {
            float most = f == 0 ? 60.0f : 40.0f;
            pose.curl[f] = pose.curl[f] < 0.0f ? 0.0f : (pose.curl[f] > 1.0f ? 1.0f : pose.curl[f]);
            pose.spread[f] = pose.spread[f] < -most ? -most : (pose.spread[f] > most ? most : pose.spread[f]);
        }
        pose.cup = pose.cup < 0.0f ? 0.0f : (pose.cup > 1.0f ? 1.0f : pose.cup);
        g.pose = pose;
        g.fingersSet = true;
        Commit(g);
    }
}

// ---------------------------------------------------------------------------
// The right: where the item sits, and the camera
// ---------------------------------------------------------------------------

static void Preset(const char* name, float x, float y, float z, const char* help)
{
    if (ImGui::Button(name))
    {
        float side = g_hand == 1 ? -1.0f : 1.0f; // the right palm faces left in the controller's own directions
        float d[3] = { x * side, y, z };
        float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int i = 0; i < 3; ++i) g_camDir[i] = d[i] / length;
        g_camOn = true;
    }
    ImGui::SetItemTooltip("%s", help);
}

static void MovePanel()
{
    Section("Move", "Where the item sits in the hand and how it is turned, in the controller's own directions: forwards is where it points.");
    ImGui::TextColored(kGreen, "%s", GripItemTitle(g_slot));
    ImGui::SameLine();
    ImGui::TextColored(kDim, "- %s hand", HandWord(g_hand));
    WindingNote();
    if (!GripSlotHasPlace(g_slot))
    {
        ImGui::TextWrapped("A hand at work: only its fingers are posed.");
    }
    else
    {
        ItemGrip g;
        bool known = PlacementShown(g);
        if (ImGui::RadioButton("The game's placement", !g.set))
        {
            g = Current();
            g.set = false;
            Commit(g);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!known);
        if (ImGui::RadioButton("My placement", g.set))
        {
            g.set = true;
            Commit(g);
        }
        ImGui::EndDisabled();
        if (!known && g_slot == kSlotBandageRoll)
        {
            ImGui::TextWrapped("Pull a bandage's roll out of its tin (start winding): then where the roll sits is known, and it can be moved from there.");
        }
        else if (!known && g_slot == kSlotDogtag)
        {
            ImGui::TextWrapped("Hold a dogtag in your %s hand (squeeze on one and keep squeezing): then where it sits is known, and it can be moved from there.",
                               HandWord(g_hand));
        }
        else if (!known && g_slot == kSlotShotgunPump)
        {
            ImGui::TextWrapped("Hold the shotgun's pump with your left hand (squeeze on the fore-end and keep squeezing): then where it sits is known, and it can be moved from there.");
        }
        else if (!known)
        {
            ImGui::TextWrapped("Take the %s out into your %s hand once: then where the game holds it is known, and it can be moved from there.",
                               GripItemTitle(g_slot), HandWord(g_hand));
        }
        ImGui::TextUnformatted("Step for - and +:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Fine", g_stepChoice == 0)) g_stepChoice = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Normal", g_stepChoice == 1)) g_stepChoice = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Big", g_stepChoice == 2)) g_stepChoice = 2;

        ImGui::BeginDisabled(!known);
        bool changed = false;
        float mm = StepSize(false), deg = StepSize(true);
        // As far as grips.cpp lets a pose go: 50 cm each way (kGripPlaceMost).
        changed |= StepRow("x", "Shift sideways", "Left", "Right", &g.right, -0.5f, 0.5f, mm, 1000.0f, "%+.0f mm");
        changed |= StepRow("y", "Shift up or down", "Down", "Up", &g.up, -0.5f, 0.5f, mm, 1000.0f, "%+.0f mm");
        changed |= StepRow("z", "Shift forwards or back", "Back", "Forwards", &g.forward, -0.5f, 0.5f, mm, 1000.0f, "%+.0f mm");
        changed |= StepRow("turn", "Turn", "Left", "Right", &g.turnRight, -180.0f, 180.0f, deg, 1.0f, "%+.0f deg");
        changed |= StepRow("tilt", "Tilt", "Down", "Up", &g.tipUp, -89.0f, 89.0f, deg, 1.0f, "%+.0f deg");
        changed |= StepRow("roll", "Roll", "Anticlockwise", "Clockwise", &g.leanRight, -180.0f, 180.0f, deg, 1.0f, "%+.0f deg");
        ImGui::EndDisabled();
        if (changed && known)
        {
            g.set = true;
            Commit(g);
        }
    }

    if (ImGui::Button("Reset this one"))
    {
        ItemGrip reset;
        Commit(reset);
    }
    ImGui::SetItemTooltip("This pose back to the game's own way (placement and fingers).");
    ImGui::SameLine();
    if (ImGui::Button("Reset all...")) ImGui::OpenPopup("Reset all poses");
    if (ImGui::BeginPopupModal("Reset all poses", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Every pose back to the game's own way? (One Undo brings them all back.)");
        if (ImGui::Button("Reset all"))
        {
            GroupStart();
            for (int slot = 0; slot < kGripSlotCount; ++slot)
                for (int hand = 0; hand < 2; ++hand)
                    if (GripSlotHasHand(slot, hand)) SetSlot(slot, hand, ItemGrip(), true);
            GroupEnd();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The hand in front of you, and the camera on the hand.
    Section("View", "Put the hand you are posing in the air in front of you, and (with the headset off) look at it from any side.");
    if (ImGui::Button(g_inFront ? "Put the hand back" : "Hand in front of me"))
    {
        g_inFront = !g_inFront;
        PlaceHandInFront();
    }
    Help("Puts the hand you are posing in the air in front of your eyes, where your head is now, so it can be posed without holding its "
         "controller (with the headset off the controllers cannot be moved). Choosing the other hand at the top moves it to that one. "
         "\"Put the hand back\" gives it back to its controller.");
    if (g_inFront)
    {
        ImGui::SameLine();
        if (ImGui::Button("Place it again")) PlaceHandInFront();
        ImGui::SetItemTooltip("In front of where your head is now.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Facing:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Held out", g_inFrontFacing == 0)) { g_inFrontFacing = 0; PlaceHandInFront(); }
        ImGui::SetItemTooltip("Pointing ahead, as an item is held out.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Palm to me", g_inFrontFacing == 1)) { g_inFrontFacing = 1; PlaceHandInFront(); }
        ImGui::SameLine();
        if (ImGui::RadioButton("Back to me", g_inFrontFacing == 2)) { g_inFrontFacing = 2; PlaceHandInFront(); }
    }
    ImGui::Spacing();
    bool usable = g_state.headsetOff;
    if (!usable) ImGui::TextWrapped("Take the headset off to use this (the view follows your head while it is on).");
    ImGui::BeginDisabled(!usable);
    ImGui::Checkbox("Camera on the hand", &g_camOn);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "(the %s hand)", HandWord(g_hand));
    Preset("Palm", 1.0f, 0.15f, -0.25f, "Looking at the palm.");
    ImGui::SameLine();
    Preset("Back of hand", -1.0f, 0.15f, -0.25f, "Looking at the back of the hand.");
    ImGui::SameLine();
    Preset("Thumb side", 0.0f, 1.0f, -0.2f, "From above, the thumb's side.");
    Preset("Little-finger side", 0.0f, -1.0f, -0.2f, "From below, the little finger's side.");
    ImGui::SameLine();
    Preset("Fingertips", 0.0f, 0.2f, -1.0f, "From in front of the fingers.");
    if (ImGui::Button("From my eyes")) g_camOn = false;
    ImGui::SetItemTooltip("The normal view again.");
    ImGui::SameLine();
    Preset("Recenter", 0.6f, 0.5f, -0.6f, "A view from the front and to the side.");

    // Dragging here turns the view round the hand; the wheel comes closer.
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 size(ImGui::GetContentRegionAvail().x, 90.0f * g_ui);
    ImGui::InvisibleButton("##orbit", size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(22, 25, 31, 255), 4.0f * g_ui);
    draw->AddText(ImVec2(at.x + 8.0f * g_ui, at.y + 6.0f * g_ui), IM_COL32(150, 156, 166, 255), "Drag here to turn the view.");
    draw->AddText(ImVec2(at.x + 8.0f * g_ui, at.y + 6.0f * g_ui + ImGui::GetTextLineHeightWithSpacing()), IM_COL32(150, 156, 166, 255),
                  "The mouse wheel comes closer.");
    draw->AddCircleFilled(ImVec2(at.x + size.x * 0.5f, at.y + size.y * 0.7f), 4.0f * g_ui, IM_COL32(242, 174, 64, 255));
    ImGuiIO& io = ImGui::GetIO();
    if (usable && ImGui::IsItemActive() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
    {
        float yaw = atan2f(g_camDir[0], g_camDir[2]) - io.MouseDelta.x * 0.01f;
        float pitch = asinf(g_camDir[1] < -1.0f ? -1.0f : (g_camDir[1] > 1.0f ? 1.0f : g_camDir[1])) + io.MouseDelta.y * 0.01f;
        if (pitch > 1.45f) pitch = 1.45f;
        if (pitch < -1.45f) pitch = -1.45f;
        g_camDir[0] = sinf(yaw) * cosf(pitch);
        g_camDir[1] = sinf(pitch);
        g_camDir[2] = cosf(yaw) * cosf(pitch);
        g_camOn = true;
    }
    if (usable && ImGui::IsItemHovered() && io.MouseWheel != 0.0f) g_camDistance *= powf(0.9f, io.MouseWheel);
    ImGui::SliderFloat("Distance", &g_camDistance, 0.15f, 1.5f, "%.2f m");
    ImGui::SliderFloat("Field of view", &g_camFov, 20.0f, 100.0f, "%.0f deg");
    if (g_camDistance < 0.15f) g_camDistance = 0.15f;
    if (g_camDistance > 1.5f) g_camDistance = 1.5f;
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// The whole window
// ---------------------------------------------------------------------------

static void BuildUi()
{
    ImGuiIO& io = ImGui::GetIO();

    // The game's state. The working copy is taken again when the game's has
    // changed and nothing is being changed here.
    GripsDeskSnapshot(g_state);
    unsigned long long now = GetTickCount64();
    if (g_state.version != g_docVersion && now - g_lastEditMs > 500)
    {
        for (int slot = 0; slot < kGripSlotCount; ++slot)
        {
            g_table[slot][0] = g_state.table[slot][0];
            g_table[slot][1] = g_state.table[slot][1];
        }
        g_libraryCount = g_state.libraryCount;
        for (int i = 0; i < g_libraryCount; ++i) g_library[i] = g_state.library[i];
        g_docVersion = g_state.version;
    }
    if (!GripSlotHasHand(g_slot, g_hand)) g_hand = 1 - g_hand;

    // The keys.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveNow();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) PostMessageW(g_window, WM_CLOSE, 0, 0); // F11 at the PC closes it, asking first if there are unsaved changes

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("BunkerVR hand poses", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    TopBar();
    ImGui::Separator();

    if (ImGui::BeginTable("columns", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0)))
    {
        ImGui::TableSetupColumn("poses", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("fingers", ImGuiTableColumnFlags_WidthStretch, 0.37f);
        ImGui::TableSetupColumn("move", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableNextRow();
        float height = ImGui::GetContentRegionAvail().y;
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("left", ImVec2(0, height));
        PoseList();
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("middle", ImVec2(0, height));
        FingersPanel();
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginChild("right", ImVec2(0, height));
        MovePanel();
        ImGui::EndChild();
        ImGui::EndTable();
    }

    // Closing with unsaved changes.
    if (g_askClose)
    {
        ImGui::OpenPopup("Unsaved changes");
        g_askClose = false;
    }
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("There are unsaved changes. Save them before closing?");
        if (ImGui::Button("Save and close"))
        {
            GripsDeskSave();
            g_closeNow = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close without saving"))
        {
            GripsDeskRevert(); // the game goes back to what was saved
            g_closeNow = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    // The camera, as set here (only used with the headset off); and the hand
    // in front of you, moved to the hand chosen now.
    CameraSetHandView(g_camOn, g_hand, g_camDir, g_camDistance, g_camFov);
    if (g_inFront && g_inFrontHandShown != g_hand) PlaceHandInFront();
}

// The look: dark, with blue controls.
static void SetStyle(float scale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.GrabMinSize = 12.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.078f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.23f, 0.28f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.88f, 0.90f, 0.92f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.53f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.20f, 0.33f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.27f, 0.44f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.32f, 0.52f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.56f, 0.92f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.68f, 1.0f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.32f, 0.55f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.42f, 0.70f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.50f, 0.82f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.33f, 0.55f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.65f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.46f, 0.75f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.70f, 1.0f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.25f, 0.30f, 1.0f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = 15.0f;
    style.FontScaleDpi = scale;
}

// ---------------------------------------------------------------------------
// The window and its thread
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return TRUE;
    switch (message)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // Alt alone doesn't open the window menu
        break;
    case WM_DPICHANGED:
    {
        const RECT* suggested = (const RECT*)lParam;
        g_ui = HIWORD(wParam) / 96.0f;
        if (ImGui::GetCurrentContext()) SetStyle(g_ui);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case kMessageToFront:
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return 0;
    case kMessageQuietClose:
        if (g_state.dirty) Log("HAND POSES TOOL: closed from the headset's menu with unsaved changes: they stay in the game (not saved yet).");
        g_closeNow = true;
        return 0;
    case WM_CLOSE:
        if (g_state.dirty && !g_closeNow)
        {
            g_askClose = true; // the window then offers: save, don't save, or cancel
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return 0;
        }
        g_closeNow = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static HINSTANCE OurModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OurModule, &module);
    return module;
}

typedef DPI_AWARENESS_CONTEXT(WINAPI* SetThreadDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);

static DWORD WINAPI ToolThread(LPVOID)
{
    // Sharp on scaled screens (this thread only; the game's window is left as it is).
    SetThreadDpiAwarenessContextFn setAwareness =
        (SetThreadDpiAwarenessContextFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (setAwareness) setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW windowClass = { sizeof(windowClass) };
        windowClass.style = CS_CLASSDC;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = OurModule();
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));     // IDI_APPLICATION
        windowClass.lpszClassName = L"BunkerVRHandPoses";
        registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    // On the game's screen, at its left, most of its height.
    HWND game = (HWND)VrGameWindow();
    HMONITOR monitor = game ? MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = { sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    int freeWidth = info.rcWork.right - info.rcWork.left, freeHeight = info.rcWork.bottom - info.rcWork.top;
    int width = (int)(freeWidth * 0.66f), height = (int)(freeHeight * 0.92f);
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
    if (scale < 0.5f || scale > 4.0f) scale = 1.0f;
    int wanted = (int)(1280.0f * scale); // room for the three columns at the screen's scaling
    if (width < wanted) width = wanted < freeWidth - 16 ? wanted : freeWidth - 16;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, L"BunkerVRHandPoses", L"BunkerVR Hand Poses", WS_OVERLAPPEDWINDOW, info.rcWork.left + 8,
                                  info.rcWork.top + (freeHeight - height) / 2, width, height, nullptr, nullptr, OurModule(), nullptr);
    if (!window || !MakeDevice(window))
    {
        if (window) DestroyWindow(window);
        Log("HAND POSES TOOL: could not be opened.");
        InterlockedExchange(&g_closing, 0);
        InterlockedExchange(&g_running, 0);
        return 0;
    }
    g_window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the game
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    g_ui = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (g_ui < 0.5f || g_ui > 4.0f) g_ui = 1.0f;
    SetStyle(g_ui);
    // Consolas (a font every Windows has); ImGui's built-in font if it is
    // missing.
    char fontPath[MAX_PATH] = "";
    UINT folder = GetWindowsDirectoryA(fontPath, MAX_PATH);
    if (folder > 0 && folder < MAX_PATH - 32) strcat_s(fontPath, "\\Fonts\\consola.ttf");
    else fontPath[0] = '\0';
    DWORD attributes = fontPath[0] ? GetFileAttributesA(fontPath) : INVALID_FILE_ATTRIBUTES;
    ImFont* font = nullptr;
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) font = io.Fonts->AddFontFromFileTTF(fontPath);
    if (!font) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    // The working copy and the choices, from the game as it is now.
    GripsSetDeskOpen(true);
    GripsDeskSnapshot(g_state);
    g_docVersion = 0;
    g_undo.clear();
    g_redo.clear();
    g_closeNow = false;
    g_askClose = false;
    g_freeze = g_state.handsFrozen;
    g_inFront = false;
    g_inFrontHandShown = -1;
    for (int hand = 1; hand >= 0; --hand)
    {
        int slot = g_state.hand[hand].atWork >= 0 ? g_state.hand[hand].atWork : g_state.hand[hand].item;
        if (g_state.fresh && slot >= 0)
        {
            g_slot = slot;
            g_hand = hand;
            break;
        }
    }

    ShowWindow(window, SW_SHOWNOACTIVATE); // the game keeps the keyboard...
    UpdateWindow(window);
    if (InterlockedExchange(&g_inFrontOnOpen, 0) != 0) SetForegroundWindow(window); // ...unless opened with F11 at the PC
    Log("HAND POSES TOOL: open on the PC's screen (%.0f%% size).", g_ui * 100.0f);

    bool done = false;
    unsigned frameCount = 0;
    while (!done)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done || g_closeNow) break;
        if (IsIconic(window))
        {
            Sleep(50);
            continue;
        }
        if (g_resizeWidth != 0 && g_resizeHeight != 0)
        {
            DropTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            MakeTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
#ifdef POSETOOL_TEST
        PoseToolTestFrame(frameCount);
#endif
        ImGui::NewFrame();
        BuildUi();
        ImGui::Render();
        const float clear[4] = { 0.055f, 0.063f, 0.078f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#ifdef POSETOOL_TEST
        PoseToolTestDrawn(frameCount, g_device, g_context, g_swapChain);
#endif
        g_swapChain->Present(1, 0);
        ++frameCount;
        // Behind the game: a slower pace that still keeps up.
        if (GetForegroundWindow() != window) Sleep(30);
    }

    // Tidying up: the tool leaves nothing changed in the game.
    InterlockedExchange(&g_closing, 1);
    CameraSetHandView(false, 1, g_camDir, g_camDistance, g_camFov);
    g_inFront = false;
    PlaceHandInFront(); // the hand goes back to its controller
    GripsSetShowGameFingers(false);
    g_showGame = false;
    if (g_freeze)
    {
        VrSetHandsFrozen(false);
        g_freeze = false;
    }
    GripsSetDeskOpen(false);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DropDevice();
    g_window = nullptr;
    bool wasInFront = GetForegroundWindow() == window;
    DestroyWindow(window);
    // Hand the keyboard back to the game, if the tool had it.
    game = (HWND)VrGameWindow();
    if (wasInFront && game) SetForegroundWindow(game);
    Log("HAND POSES TOOL: closed.");
    InterlockedExchange(&g_closing, 0);
    InterlockedExchange(&g_running, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// From the game's thread
// ---------------------------------------------------------------------------

void PoseToolSetOpen(bool open)
{
    if (open)
    {
        if (g_closing) return;
        if (InterlockedCompareExchange(&g_running, 1, 0) == 0)
        {
            HANDLE thread = CreateThread(nullptr, 0, ToolThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            else InterlockedExchange(&g_running, 0);
        }
    }
    else
    {
        HWND window = g_window;
        if (window && g_running && !g_closing) PostMessageW(window, kMessageQuietClose, 0, 0);
    }
}

void PoseToolFromKey()
{
    if (!PoseToolIsOpen())
    {
        InterlockedExchange(&g_inFrontOnOpen, 1);
        PoseToolSetOpen(true);
    }
    else if (!PoseToolIsForeground())
    {
        HWND window = g_window;
        if (window) PostMessageW(window, kMessageToFront, 0, 0);
    }
    else
    {
        HWND window = g_window;
        if (window) PostMessageW(window, WM_CLOSE, 0, 0); // at the PC: asks first if there are unsaved changes
    }
}

bool PoseToolIsOpen()
{
    return g_running != 0 && g_closing == 0;
}

bool PoseToolIsForeground()
{
    HWND window = g_window;
    return window != nullptr && GetForegroundWindow() == window;
}
