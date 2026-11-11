#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#include <MinHook.h>

#include "camera.h"
#include "enginetime.h"
#include "gl_helpers.h"
#include "log.h"
#include "pattern.h"
#include "scene.h"
#include "settings.h"
#include "vr.h"

// ---------------------------------------------------------------------------
// How the engine draws a frame
// ---------------------------------------------------------------------------
// Once per frame the engine's scene object runs its "render" function. That
// function adds one to the engine's frame counter and then, for every visible
// viewport, calls one function that draws that viewport from start to finish.
// The stages inside it carry the engine's own names:
//
//   RenderWorld            - the 3D world, through the viewport's renderer
//   Render3DGui            - interface pieces that live in the world
//   RenderPostEffects      - screen-wide effects
//   CopyBackBufferToScreen - the finished picture is copied to the window
//   RenderGUI              - the flat interface, on top (gui.cpp hooks this)
//
// Its arguments: the scene, the viewport, the frame's length in seconds, and
// which parts to do (1 = world, 2 = flat interface, 4 = screen effects).
//
// We hook that per-viewport function for two things:
//
//   1. Timing: how long drawing the world takes out of each frame.
//   2. Stereo inside one game frame (a setting, off unless switched on): the
//      world is drawn once for the left eye, the picture is taken, the camera
//      is moved to the right eye, and the world is drawn again. The game's
//      own logic, scripts and flat interface run once, and both eyes show
//      exactly the same moment. This is how games with VR built in do it.

static const char* kRenderViewportPattern =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 "
    "48 8B FA 0F 29 74 24 70 48 8B 92 80 00 00 00 48 8B E9 32 DB";

typedef void (*RenderViewportFunction)(void* scene, void* viewport, float frameTime, int flags);
static RenderViewportFunction g_realRenderViewport = nullptr;

static const int kPartWorld = 1;
static const int kPartInterface = 2;
static const int kPartEffects = 4;

// Places inside the viewport object.
static const int kViewportCamera = 0x18;
static const int kViewportWorld = 0x20;
static const int kViewportRenderer = 0x70;
static const int kViewportTarget = 0x90; // the picture it draws into; 0 = the window
static const int kWorldActive = 0x91;    // inside the world object: 1 = to be drawn

static double NowMs()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

// Is this the viewport that draws the game's 3D world to the window?
// (The same checks the engine itself makes before its RenderWorld stage.)
static bool IsWorldViewport(void* viewport, int flags)
{
    if ((flags & kPartWorld) == 0 || viewport == nullptr) return false;
    char* v = (char*)viewport;
    void* camera = *(void**)(v + kViewportCamera);
    void* world = *(void**)(v + kViewportWorld);
    void* renderer = *(void**)(v + kViewportRenderer);
    void* target = *(void**)(v + kViewportTarget);
    if (camera == nullptr || world == nullptr || renderer == nullptr || target != nullptr) return false;
    return *((unsigned char*)world + kWorldActive) != 0;
}

// ---- The right eye without waiting (settings: right_eye_no_wait) ----
// The engine asks the graphics card whether things are hidden behind walls
// (big lights, the glow and flares around lamps, ...) and uses the answers
// at its next drawing. Its "answers" object keeps two sets of them (two
// std::multimap at +0x3D8 and +0x3E8; which of them is the newest at
// +0x3F8). At the start of each drawing the renderer swaps them: it clears
// the older set to take this drawing's questions, and reads from the other,
// filled at the drawing before. An answer not in yet is waited for, in a
// loop. Later in the drawing every question in the newest set is sent to
// the graphics card.
//
// In the normal game the drawing before is last frame's, long finished.
// With stereo in one frame the drawing before the right eye's is the left
// eye's, still on the graphics card, so the processor would sit waiting for
// it.
//
// So the swap is left out for the right eye: it reads the left eye's
// answers from the frame before. But while the right eye is drawn, the
// newest set in the answers object is one of the right eye's own (put in
// place of the left eye's newest, and taken out straight after). So its
// questions go there, and only they are sent to the card. They are never
// read: before the next right eye's drawing that set is cleared by the
// game's own routine, which hands the entries and the query objects back as
// the game always does.
//
// Two simpler ways fail:
// - Leaving out the swap alone puts the right eye's questions into the left
//   eye's newest set, and the right eye's drawing sends the left eye's
//   questions to the card a second time, before their answers have been
//   read: the game logs "Failed to start occlusion query ..." over a
//   thousand times a second.
// - Giving the right eye a whole pair of sets of its own makes the lights
//   go wrong (lit patches with straight edges, moving about).
//
// The answers object is the one the game's own call takes: the world
// viewport's renderer (+0x70), its settings (+0x440), their answers (+0x18).
// The right eye's set holds the game's own entries, which point at the
// renderer's query objects. It is given up (left as it is, never touched
// again) when the game deletes that answers object (hooked), or when the
// viewport has another one.

// The swap's first instructions (it reads the newest set's number at +0x3F8).
static const char* kSwapAnswersPattern =
    "44 88 44 24 18 41 55 48 83 EC 50 8B 81 F8 03 00 00 4C 8B E9 48 89 5C 24 68 "
    "48 89 7C 24 38 4C 89 64 24 30 4C 89 7C 24 20 4C 8B FA 45 84 C0";

// The renderer's call to it at the start of each drawing, up to the call
// instruction (E8, then the distance to the swap). The swap is also called
// from elsewhere (when everything is cleared), which is always let through.
static const char* kSwapAnswersCallPattern =
    "48 8B 81 40 04 00 00 48 8B 48 18 48 85 C9 74 10 44 0F B6 80 D7 00 00 00 48 8B D3 E8";
static const int kSwapAnswersCallLength = 28; // the bytes above

// What the game does just before deleting an answers object (it frees the
// sets and the spare entries).
static const char* kDeleteAnswersPattern =
    "48 89 5C 24 20 41 54 41 56 41 57 48 83 EC 40 33 D2 4C 8B F9 E8 ?? ?? ?? ?? "
    "4D 8D B7 C0 03 00 00 45 33 E4 49 8B 1E 49 8B 46 08 48 3B D8";

// The game's routine that clears one set (the newest, or with its third
// argument the other one), handing back its entries and query objects.
static const char* kClearAnswerSetPattern =
    "40 53 57 41 54 41 57 48 83 EC 28 8B 81 F8 03 00 00 4C 8B FA 45 84 C0 74 10 "
    "FF C0 25 01 00 00 80 7D 07 FF C8 83 C8 FE FF C0 4C 63 E0 49 C1 E4 04 4C 03 E1";

static const int kRendererSettings = 0x440; // in the renderer: its settings
static const int kSettingsAnswers = 0x18;   // in those: the answers object
static const int kAnswerSetsAt = 0x3D8;     // the two sets (16 bytes each)
static const int kAnswerNewestAt = 0x3F8;   // which of them is the newest (0 or 1)

typedef void (*SwapAnswersFunction)(void* answers, void* renderer, bool delayed);
typedef void (*DeleteAnswersFunction)(void* answers);
typedef void (*ClearAnswerSetFunction)(void* answers, void* renderer, bool other);
static SwapAnswersFunction g_realSwapAnswers = nullptr;
static DeleteAnswersFunction g_realDeleteAnswers = nullptr;
static ClearAnswerSetFunction g_clearAnswerSet = nullptr;
static void* g_swapAnswersReturn = nullptr; // where the renderer's call returns to
static bool g_answersHooked = false;

// The answers object the right eye's set belongs to, and that set (16 bytes,
// as the game's: its head and its size).
static void* g_answers = nullptr;
static unsigned char g_rightSet[16];
static bool g_rightSetReady = false;
static bool g_rightSetIn = false; // in the answers object right now (the right eye is being drawn)
static unsigned char* g_rightSetSlot = nullptr; // where it is put while in

// The empty start of the right eye's set: a std::multimap's head (in the
// game's build: left, parent, right, color, "is the head", then room for a
// value; 0x30 bytes, the same as its other entries).
struct AnswerSetHead
{
    void* left;
    void* parent;
    void* right;
    char colour;
    char isHead;
    char unused[6];
    unsigned char value[16];
};
static_assert(sizeof(AnswerSetHead) == 0x30, "a set's head is 0x30 bytes, as the game's");
static AnswerSetHead g_rightHead;

static unsigned long long g_statSwapsDone = 0, g_statSwapsLeftOut = 0, g_statRightSetUsed = 0;

// An empty set for the right eye. Whatever the old one held is given up.
static void StartRightSet()
{
    memset(&g_rightHead, 0, sizeof(g_rightHead));
    g_rightHead.left = g_rightHead.parent = g_rightHead.right = &g_rightHead;
    g_rightHead.colour = 1; // black
    g_rightHead.isHead = 1;
    void* headAddress = &g_rightHead;
    unsigned long long size = 0;
    memcpy(g_rightSet, &headAddress, 8);
    memcpy(g_rightSet + 8, &size, 8);
    g_rightSetReady = true;
}

// Does the answers object's own pair of sets look as it should?
static bool AnswerSetsLookRight(const void* answers)
{
    const unsigned char* sets = (const unsigned char*)answers + kAnswerSetsAt;
    for (int i = 0; i < 2; ++i)
    {
        const AnswerSetHead* head = *(AnswerSetHead* const*)(sets + i * 16);
        if (head == nullptr || head->isHead != 1) return false;
    }
    int newest = *(const int*)((const unsigned char*)answers + kAnswerNewestAt);
    return newest == 0 || newest == 1;
}

// Puts the right eye's set in the place of the answers object's newest set,
// or the left eye's back.
static void ExchangeRightSet()
{
    unsigned char kept[16];
    memcpy(kept, g_rightSetSlot, 16);
    memcpy(g_rightSetSlot, g_rightSet, 16);
    memcpy(g_rightSet, kept, 16);
    g_rightSetIn = !g_rightSetIn;
}

static void HookedSwapAnswers(void* answers, void* renderer, bool delayed)
{
    if (_ReturnAddress() == g_swapAnswersReturn)
    {
        // The right eye's drawing: no swap, so that it reads the left eye's
        // answers from the frame before (and its own set stays the newest).
        if (g_rightSetIn && answers == g_answers)
        {
            ++g_statSwapsLeftOut;
            return;
        }
        ++g_statSwapsDone;
    }
    g_realSwapAnswers(answers, renderer, delayed);
}

static void HookedDeleteAnswers(void* answers)
{
    // The occlusion queries held for the right eye go back to the object in its clearing, which the
    // deleting calls first (HookedClearAllAnswers).
    if (answers == g_answers && answers != nullptr)
    {
        // (Never while the right eye is drawn; if it were, the left eye's
        // set goes back first, so that the game frees its own.)
        if (g_rightSetIn) ExchangeRightSet();
        g_answers = nullptr;
        g_rightSetReady = false;
        static int logsLeft = 3;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("SCENE: the game deleted its hidden-or-not answers object; the right eye's set is given up with it.");
        }
    }
    g_realDeleteAnswers(answers);
}

// Around the right eye's drawing (see HookedRenderViewport).
static void RightEyeAnswersIn(void* viewport)
{
    if (!g_answersHooked || !GetSettings().rightEyeNoWait || g_rightSetIn) return;
    char* renderer = *(char**)((char*)viewport + kViewportRenderer);
    char* settings = renderer != nullptr ? *(char**)(renderer + kRendererSettings) : nullptr;
    void* answers = settings != nullptr ? *(void**)(settings + kSettingsAnswers) : nullptr;
    if (answers == nullptr) return;
    if (answers != g_answers)
    {
        if (g_answers != nullptr) Log("SCENE: the world's hidden-or-not answers object changed; the right eye starts afresh.");
        g_answers = answers;
        g_rightSetReady = false;
    }
    if (!AnswerSetsLookRight(g_answers))
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("SCENE ERROR: the game's hidden-or-not answers do not look as expected; the right eye uses them as the game does.");
        }
        return;
    }
    if (!g_rightSetReady) StartRightSet();

    int newest = *(const int*)((const unsigned char*)g_answers + kAnswerNewestAt);
    g_rightSetSlot = (unsigned char*)g_answers + kAnswerSetsAt + newest * 16;
    ExchangeRightSet();

    // Last frame's questions of the right eye, never read: cleared by the
    // game's own routine (its newest set is the right eye's now).
    unsigned long long size = 0;
    memcpy(&size, g_rightSetSlot + 8, 8);
    if (size > 0) g_clearAnswerSet(g_answers, renderer, false);
    ++g_statRightSetUsed;
}

static void RightEyeAnswersOut()
{
    if (g_rightSetIn) ExchangeRightSet();
}

static void InstallSwapAnswersHook(HMODULE game)
{
    int matches = 0;
    void* target = FindPattern(game, kSwapAnswersPattern, &matches);
    int callMatches = 0;
    unsigned char* call = (unsigned char*)FindPattern(game, kSwapAnswersCallPattern, &callMatches);
    int deleteMatches = 0;
    void* deleteTarget = FindPattern(game, kDeleteAnswersPattern, &deleteMatches);
    int clearMatches = 0;
    void* clearTarget = FindPattern(game, kClearAnswerSetPattern, &clearMatches);
    if (matches != 1 || callMatches != 1 || deleteMatches != 1 || clearMatches != 1)
    {
        Log("SCENE ERROR: expected to find the engine's swap of hidden-or-not answers, its call, its deleting and its clearing "
            "exactly once, found them %d, %d, %d and %d times. The right eye will wait as before.",
            matches, callMatches, deleteMatches, clearMatches);
        return;
    }
    g_clearAnswerSet = (ClearAnswerSetFunction)clearTarget;

    // The call must lead to the swap found above.
    unsigned char* returnTo = call + kSwapAnswersCallLength + 4;
    int distance = *(int*)(call + kSwapAnswersCallLength);
    if (returnTo + distance != (unsigned char*)target)
    {
        Log("SCENE ERROR: the renderer's call does not lead to the swap of hidden-or-not answers. The right eye will wait as before.");
        return;
    }
    g_swapAnswersReturn = returnTo;

    MH_STATUS status = MH_CreateHook(deleteTarget, (LPVOID)HookedDeleteAnswers, (LPVOID*)&g_realDeleteAnswers);
    if (status == MH_OK) status = MH_EnableHook(deleteTarget);
    if (status != MH_OK)
    {
        Log("SCENE ERROR: could not hook the deleting of hidden-or-not answers: %s. The right eye will wait as before.",
            MH_StatusToString(status));
        return;
    }
    status = MH_CreateHook(target, (LPVOID)HookedSwapAnswers, (LPVOID*)&g_realSwapAnswers);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("SCENE ERROR: could not hook the swap of hidden-or-not answers: %s. The right eye will wait as before.",
            MH_StatusToString(status));
        return;
    }
    g_answersHooked = true;
    Log("SCENE: hooked the engine's swap of hidden-or-not answers at exe+0x%llX (called from exe+0x%llX) and their deleting "
        "at exe+0x%llX; their clearing is at exe+0x%llX.",
        (unsigned long long)((char*)target - (char*)game),
        (unsigned long long)((char*)call + kSwapAnswersCallLength - 1 - (char*)game),
        (unsigned long long)((char*)deleteTarget - (char*)game),
        (unsigned long long)((char*)clearTarget - (char*)game));
}

// ---- The right eye reads the left eye's occlusion queries of the frame before ----
// The game culls with its "CHC_Culling_Delayed" function. Its "Queries" step first reads the answers to
// the occlusion queries the drawing before sent (a std::list in the answers object at +0x3A8, its size
// at +0x3B0; portals and things are read separately), waiting in a loop for each answer to come back
// from the graphics card, and writes them into the object's result sets, which the drawing's culling
// then reads with the drawing before's own marks (double-buffered, swapped at each drawing). Later in
// the drawing it sends its own queries ("DelayedCHCQueries") for the next drawing to read. The left eye
// reads the right eye's of the frame before, long done. Left to the game, the right eye would read the
// left eye's of moments before, still on the card, and wait for the card to get through the left eye's
// world (about 1.3 ms of the processor's frame; the same wait as for the lights' answers, the multimaps
// above).
// Don't give each eye its own list instead: that ends the wait, but the right eye then reads its own
// answers with the left eye's marks, and its culling grows cautious in places: it keeps rooms and
// lights in view the left eye does not, and draws their shadow maps every frame (0.4 to 1.4 ms).
// So the right eye reads the left eye's queries of the frame before: the left eye's marks with the left
// eye's answers, as the game always pairs them, one frame older (long done: no wait).
// Just before the right eye is drawn, the left eye's queries just sent are moved from the answers
// object's list to one the mod holds, and the left eye's of the frame before, held there since, take
// their place. The right eye's own then go to the left eye's next drawing, as usual. The left eye is
// drawn exactly as the game draws it. Only the engine's own entries move, by their links (the list's
// head stays the engine's; the mod's heads never go into its object): the engine reads, sends and
// hands back every query itself. Whenever the engine clears the object entirely (a world changed, or
// the object deleted, which calls it first), the entries held are put into its list first (hooked
// here), so it hands them back too. Switched off, the held entries go back into the engine's list.
static const int kAnswerQueriesAt = 0x3A8;     // in the answers object: the pending occlusion queries (the list's head)
static const int kAnswerQueriesSizeAt = 0x3B0; // ...and how many
// The engine's clearing of a whole answers object (it clears the list too, handing each query back).
static const char* kClearAllAnswersPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 90 00 00 00 45 33 F6 "
    "48 8D 99 10 01 00 00 48 8B EA 4C 89 B1 B8 03 00 00";
typedef void (*ClearAllAnswersFunction)(void* answers, void* renderer);
static ClearAllAnswersFunction g_realClearAllAnswers = nullptr;
static bool g_queriesHooked = false;

// A node of the list as the engine makes them (0x78 bytes: next, previous, then the entry). The mod's
// two heads are only ever heads of its own lists.
struct QueryListNode
{
    QueryListNode* next;
    QueryListNode* prev;
    unsigned char entry[0x68];
};
static_assert(sizeof(QueryListNode) == 0x78, "a query list node is 0x78 bytes, as the engine's");
static QueryListNode g_heldQueries = {};  // the left eye's queries of the frame before, held for the right eye
static unsigned long long g_heldQueriesSize = 0;
static QueryListNode g_movingQueries = {}; // used only while the two lists change places
static unsigned long long g_movingQueriesSize = 0;
static bool g_queryListsReady = false;
static void* g_queriesOwner = nullptr;   // the answers object the held entries belong to
static unsigned long long g_statEarlierQueries = 0, g_statEarlierQueryEntries = 0, g_statQueriesGivenBack = 0;

static void EmptyQueryList(QueryListNode* head)
{
    head->next = head;
    head->prev = head;
}

static bool QueryListLooksRight(const QueryListNode* head, unsigned long long size)
{
    return head != nullptr && head->next != nullptr && head->prev != nullptr && size < 1000000;
}

// Every entry of one list moved to the end of another (by their links only: the entries stay the
// engine's, in the order they were).
static void MoveQueries(QueryListNode* toHead, unsigned long long* toSize, QueryListNode* fromHead, unsigned long long* fromSize)
{
    if (fromHead->next != fromHead)
    {
        QueryListNode* first = fromHead->next;
        QueryListNode* last = fromHead->prev;
        QueryListNode* tail = toHead->prev;
        tail->next = first;
        first->prev = tail;
        last->next = toHead;
        toHead->prev = last;
        *toSize += *fromSize;
        EmptyQueryList(fromHead);
    }
    *fromSize = 0;
}

static void* AnswersOfViewport(void* viewport)
{
    char* renderer = *(char**)((char*)viewport + kViewportRenderer);
    char* settings = renderer != nullptr ? *(char**)(renderer + kRendererSettings) : nullptr;
    return settings != nullptr ? *(void**)(settings + kSettingsAnswers) : nullptr;
}

// The answers object's own list (its head and size), if it looks right.
static bool AnswersQueryList(void* answers, QueryListNode** head, unsigned long long** size)
{
    unsigned char* a = (unsigned char*)answers;
    *head = *(QueryListNode**)(a + kAnswerQueriesAt);
    *size = (unsigned long long*)(a + kAnswerQueriesSizeAt);
    return QueryListLooksRight(*head, **size);
}

// The held entries back into the answers object's list, for its next drawing to read.
static bool GiveBackHeldQueries(void* answers)
{
    if (!g_queryListsReady || g_heldQueriesSize == 0 || answers == nullptr || answers != g_queriesOwner) return false;
    QueryListNode* head = nullptr;
    unsigned long long* size = nullptr;
    if (!AnswersQueryList(answers, &head, &size)) return false;
    MoveQueries(head, size, &g_heldQueries, &g_heldQueriesSize);
    return true;
}

static void HookedClearAllAnswers(void* answers, void* renderer)
{
    // The held entries go into the list first, so the engine hands them back with its own.
    if (GiveBackHeldQueries(answers))
    {
        static int logsLeft = 5;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("SCENE: the game clears its hidden-or-not answers: the left eye's occlusion queries held for the right eye are given to it to hand back.");
        }
    }
    g_realClearAllAnswers(answers, renderer);
}

// Just before the right eye's drawing (see HookedRenderViewport).
static void RightEyeQueriesIn(void* viewport)
{
    if (!g_queriesHooked) return;
    void* answers = AnswersOfViewport(viewport);
    if (answers == nullptr) return;
    if (!g_queryListsReady)
    {
        EmptyQueryList(&g_heldQueries);
        EmptyQueryList(&g_movingQueries);
        g_heldQueriesSize = g_movingQueriesSize = 0;
        g_queryListsReady = true;
    }
    if (answers != g_queriesOwner)
    {
        // The engine clears an object before deleting it, which gives the held entries back: none
        // should be left here for another.
        if (g_heldQueriesSize > 0)
        {
            static int logsLeft = 3;
            if (logsLeft > 0)
            {
                --logsLeft;
                Log("SCENE ERROR: %llu occlusion queries were held for a hidden-or-not answers object no longer in use; let go.", g_heldQueriesSize);
            }
        }
        EmptyQueryList(&g_heldQueries);
        g_heldQueriesSize = 0;
        g_queriesOwner = answers;
    }
    if (!GetSettings().rightEyeEarlierQueries)
    {
        if (GiveBackHeldQueries(answers)) ++g_statQueriesGivenBack; // switched off: the right eye reads them now, as the game would
        return;
    }
    QueryListNode* head = nullptr;
    unsigned long long* size = nullptr;
    if (!AnswersQueryList(answers, &head, &size))
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log("SCENE ERROR: the game's pending occlusion queries do not look as expected; the right eye reads them as the game does.");
        }
        return;
    }
    g_statEarlierQueryEntries += g_heldQueriesSize;
    // The left eye's just sent aside, its of the frame before in their place, the just sent held.
    MoveQueries(&g_movingQueries, &g_movingQueriesSize, head, size);
    MoveQueries(head, size, &g_heldQueries, &g_heldQueriesSize);
    MoveQueries(&g_heldQueries, &g_heldQueriesSize, &g_movingQueries, &g_movingQueriesSize);
    ++g_statEarlierQueries;
}

// ---- The right eye's occlusion answers: the left eye's of this frame ----
// Even without the query wait above, the right eye would wait about 1 ms where lights with shadows are
// in view: in its light setup the main thread sits in the engine's reading of a thing's occlusion
// answer (the function that logs "Retrieved %d samples from occlusion object with source %d and custom
// ID: %d"; its reader waits in a loop for the card), called from the shadow step. The right eye reads
// answers to the questions the left eye has just asked, still on the card. The same reading also
// serves the lights' glows and halos and the dynamic textures.
// With the same switch (right_eye_earlier_queries): while the left eye is drawn, each answer it reads
// is noted (the thing, the custom number, the answer); while the right eye is drawn, an answer the left
// eye read this frame is given again, the card not asked. The left eye's answer is to its question of
// the drawing before, as the game always reads them (one drawing old). The answers the right eye does
// not read are read by the left eye's next drawing, as usual. Only on the main thread; the things are
// compared, never used (nothing is kept from one frame to the next: each left eye's drawing starts the
// notes afresh). Anything the left eye did not read is asked of the card, as usual.
static const char* kRetrieveSamplesPattern =
    "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 30 48 8B 81 40 04 00 00 41 8B F0 48 89 7C 24 40 48 8B EA "
    "48 8B F9 48 8B 48 18";
typedef int (*RetrieveSamplesFunction)(void* renderer, void* source, int customId);
static RetrieveSamplesFunction g_realRetrieveSamples = nullptr;
static int g_drawingEye = 0;            // 0 none, 1 the left eye's world being drawn, 2 the right eye's
static DWORD g_drawingThread = 0;       // the main thread (the one drawing the eyes)

struct LeftAnswer
{
    void* renderer;
    void* source;
    int customId;
    int samples;
    unsigned frame;
};
static const int kLeftAnswers = 4096;   // (a power of two)
static LeftAnswer g_leftAnswers[kLeftAnswers];
static unsigned g_leftAnswersFrame = 1; // the notes of other frames count as empty
static unsigned long long g_statAnswersGiven = 0, g_statAnswersAsked = 0, g_statAnswersNoted = 0, g_statAnswersNoRoom = 0;

static unsigned AnswerSlot(void* renderer, void* source, int customId)
{
    unsigned long long key = (unsigned long long)(ULONG_PTR)source * 31ull + (unsigned long long)(ULONG_PTR)renderer * 7ull +
                             (unsigned long long)(unsigned)customId;
    key ^= key >> 29;
    key *= 0x9E3779B97F4A7C15ull;
    return (unsigned)(key >> 40) & (unsigned)(kLeftAnswers - 1);
}

static int HookedRetrieveSamples(void* renderer, void* source, int customId)
{
    if (g_drawingEye == 0 || GetCurrentThreadId() != g_drawingThread) return g_realRetrieveSamples(renderer, source, customId);
    unsigned slot = AnswerSlot(renderer, source, customId);
    if (g_drawingEye == 2 && GetSettings().rightEyeEarlierQueries)
    {
        for (int k = 0; k < 64; ++k)
        {
            const LeftAnswer& note = g_leftAnswers[(slot + (unsigned)k) & (unsigned)(kLeftAnswers - 1)];
            if (note.frame != g_leftAnswersFrame) break;
            if (note.source == source && note.renderer == renderer && note.customId == customId)
            {
                ++g_statAnswersGiven;
                return note.samples;
            }
        }
        ++g_statAnswersAsked;
        return g_realRetrieveSamples(renderer, source, customId);
    }
    int samples = g_realRetrieveSamples(renderer, source, customId);
    if (g_drawingEye == 1)
    {
        for (int k = 0; k < 64; ++k)
        {
            LeftAnswer& note = g_leftAnswers[(slot + (unsigned)k) & (unsigned)(kLeftAnswers - 1)];
            if (note.frame != g_leftAnswersFrame || (note.source == source && note.renderer == renderer && note.customId == customId))
            {
                note.renderer = renderer;
                note.source = source;
                note.customId = customId;
                note.samples = samples;
                note.frame = g_leftAnswersFrame;
                ++g_statAnswersNoted;
                return samples;
            }
        }
        ++g_statAnswersNoRoom;
    }
    return samples;
}

// The eye whose world is being drawn (HookedRenderViewport): the left eye's starts its notes afresh.
static void SetDrawingEye(int eye)
{
    if (eye == 1 && g_drawingEye != 1)
    {
        ++g_leftAnswersFrame;
        if (g_leftAnswersFrame == 0) // (never 0: an empty note has 0)
        {
            memset(g_leftAnswers, 0, sizeof(g_leftAnswers));
            g_leftAnswersFrame = 1;
        }
    }
    g_drawingEye = eye;
    g_drawingThread = GetCurrentThreadId();
}

static void InstallRightAnswersHook(HMODULE game)
{
    int matches = 0;
    void* target = FindPattern(game, kRetrieveSamplesPattern, &matches);
    if (matches != 1 || target == nullptr)
    {
        Log("SCENE ERROR: expected to find the engine's reading of occlusion answers exactly once, found it %d times. "
            "The right eye waits for the lights' answers as before.", matches);
        return;
    }
    MH_STATUS status = MH_CreateHook(target, (LPVOID)HookedRetrieveSamples, (LPVOID*)&g_realRetrieveSamples);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("SCENE ERROR: could not hook the reading of occlusion answers: %s. The right eye waits for the lights' answers as before.",
            MH_StatusToString(status));
        return;
    }
    Log("SCENE: hooked the engine's reading of occlusion answers at exe+0x%llX: the right eye can take the left eye's of this frame "
        "(settings: right_eye_earlier_queries).", (unsigned long long)((char*)target - (char*)game));
}

static void InstallRightQueriesHook(HMODULE game)
{
    int matches = 0;
    void* target = FindPattern(game, kClearAllAnswersPattern, &matches);
    if (matches != 1 || target == nullptr)
    {
        Log("SCENE ERROR: expected to find the engine's clearing of hidden-or-not answers exactly once, found it %d times. "
            "The right eye waits for the left eye's occlusion queries as before.", matches);
        return;
    }
    MH_STATUS status = MH_CreateHook(target, (LPVOID)HookedClearAllAnswers, (LPVOID*)&g_realClearAllAnswers);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("SCENE ERROR: could not hook the clearing of hidden-or-not answers: %s. The right eye waits for the left eye's occlusion queries as before.",
            MH_StatusToString(status));
        return;
    }
    g_queriesHooked = true;
    Log("SCENE: hooked the engine's clearing of hidden-or-not answers at exe+0x%llX: the right eye can read the left eye's occlusion "
        "queries of the frame before, without waiting (settings: right_eye_earlier_queries).", (unsigned long long)((char*)target - (char*)game));
}

// ---- Don't give the right eye the left eye's list of what to draw ----
// Noting the left eye's additions to the render list (the render list's AddObject and the second
// list's) and making them again for the right eye, in place of its own Culling, would save about
// 1.1 ms a frame (CHC_Culling once a frame, not twice), but it breaks two things:
// (1) the engine's occlusion questions go out of step: "Failed to start occlusion query, the result
// for the previous query has not been fetched" floods hpl.log and OpenGL reports INVALID_OPERATION
// every frame (the Culling also fetches the answers its next drawing needs); and
// (2) a thing noted for the left eye can be destroyed before the right eye's drawing in the same
// frame: AddObject's first call on it is then a pure virtual call, which aborts the game
// (0xC0000409). Things can go between the eyes, so pointers kept from one drawing to the next are
// not safe.

// ---- The right eye's shadows: the left eye's, drawn moments before ----
// Each drawing of the world first brings the shadow maps of the lights in view up to date (the
// engine's light setup, its "ShadowMap" step). The maps live in a cache of the renderer's, and both
// eyes are drawn by the same viewport and renderer, in the same engine frame (one frame count). For
// each light in view a map is drawn again:
//   - always when the light has changed since its map was last checked (moved, its size or cone
//     changed, or its cache place given to another light);
//   - otherwise by a priority, highest first, up to a budget a drawing (3 unless the graphics
//     settings set it): how many frames since its map was last drawn times its nearness, plus
//     5,000,000 when the camera frustum's bounding volume (its +0x48) is inside the light's frustum.
// So with both eyes drawn each frame the budget would be spent twice: the right eye would draw again
// maps the left eye had just drawn (boosted ones, with nothing moved between: the game's logic runs
// once a frame), or more of the lights waiting their turn, whose shadows would then differ between
// the eyes for that frame (the right eye's fresher). In soldier_quarters "ShadowMap" takes 1.6 to
// 2.1 ms a frame, the largest part of the drawing left.
// While the right eye is drawn the budget is 0 here: only lights that have changed since the left
// eye's drawing are drawn again, all the rest keep the maps drawn moments before. The engine's own
// path for lights not brought up to date in a drawing (the budget used up) is taken: such a light's
// job is dropped from the shadow drawing ("FinalShadowMap") and it is lit from its cached map. The
// left eye's drawing is left exactly as the game does it. A light the left eye does not see at all
// (wholly outside its view or hidden for it) is brought up to date for the right eye only if it has
// changed; it lights nothing the left eye sees. Nothing of the engine's is kept across drawings:
// one number, written before the right eye's drawing and put back after it.

// The sort and the budget's reading: the sort's order (LEA R9, the comparator),
// MOV RCX,[R15], MOV R8,RDX, SUB R8,RCX, SAR R8,3, CALL the sort, MOV EAX,[the budget], MOV
// R12,[RSP+0x78], CMP [RSI+0x2818],BPL (every map at once, after a change of settings), JZ.
static const char* kShadowBudgetPattern =
    "4C 8D 0D ?? ?? ?? ?? 49 8B 0F 4C 8B C2 4C 2B C1 49 C1 F8 03 E8 ?? ?? ?? ?? "
    "8B 05 ?? ?? ?? ?? 4C 8B 64 24 78 40 38 AE 18 28 00 00 74 10";
static const int kShadowSortOrderAt = 3;   // the comparator's distance (from the end of the LEA, 7)
static const int kShadowBudgetAt = 27;     // the budget's distance (from the end of the MOV, 31)
// The comparator: MOV EAX,[RDX+0x28]; CMP [RCX+0x28],EAX; SETG AL; RET (the higher priority first).
static const unsigned char kShadowSortOrder[] = { 0x8B, 0x42, 0x28, 0x39, 0x41, 0x28, 0x0F, 0x9F, 0xC0, 0xC3 };

static volatile int* g_shadowBudget = nullptr; // the engine's budget of shadow maps a drawing
static int g_shadowBudgetKept = 0;
static bool g_shadowBudgetHeld = false;        // at 0 for the right eye's drawing right now
static unsigned long long g_statRightShadowsKept = 0, g_statRightEyes = 0;

static void FindShadowBudget(HMODULE game)
{
    int matches = 0;
    unsigned char* at = (unsigned char*)FindPattern(game, kShadowBudgetPattern, &matches);
    if (matches != 1 || at == nullptr)
    {
        Log("SCENE ERROR: expected to find the engine's shadow map budget exactly once, found it %d times. "
            "The right eye draws its own shadow maps as before.", matches);
        return;
    }
    unsigned char* order = at + 7 + *(int*)(at + kShadowSortOrderAt);
    unsigned char* budget = at + 31 + *(int*)(at + kShadowBudgetAt);
    // Both inside the game's exe as loaded.
    const unsigned char* base = (const unsigned char*)game;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const unsigned char* end = base + nt->OptionalHeader.SizeOfImage;
    bool inside = order >= base && order + sizeof(kShadowSortOrder) <= end && budget >= base && budget + 4 <= end;
    if (!inside || memcmp(order, kShadowSortOrder, sizeof(kShadowSortOrder)) != 0)
    {
        Log("SCENE ERROR: the engine's shadow map budget was found, but %s. The right eye draws its own shadow maps as before.",
            inside ? "its sort is not the one expected (the highest priority first)" : "it is not inside the game");
        return;
    }
    int value = *(volatile int*)budget;
    if (value < 0 || value > 1000)
    {
        Log("SCENE ERROR: the engine's shadow map budget reads %d, not a budget. The right eye draws its own shadow maps as before.", value);
        return;
    }
    g_shadowBudget = (volatile int*)budget;
    Log("SCENE: the engine's shadow map budget found at exe+0x%llX (read at exe+0x%llX): %d a drawing. While the right eye is "
        "drawn it is 0: only the lights that have changed since the left eye's drawing get new shadow maps (settings: right_eye_shadows).",
        (unsigned long long)(budget - base), (unsigned long long)(at + 25 - base), value);
}

// Around the right eye's drawing (see HookedRenderViewport).
static void RightEyeShadowsIn()
{
    ++g_statRightEyes;
    if (g_shadowBudget == nullptr || g_shadowBudgetHeld || !GetSettings().rightEyeShadows) return;
    g_shadowBudgetKept = *g_shadowBudget;
    *g_shadowBudget = 0;
    g_shadowBudgetHeld = true;
    ++g_statRightShadowsKept;
}

static void RightEyeShadowsOut()
{
    if (!g_shadowBudgetHeld) return;
    g_shadowBudgetHeld = false;
    // Put back unless the game has set it meanwhile, from its settings.
    if (*g_shadowBudget == 0) *g_shadowBudget = g_shadowBudgetKept;
}

// ---- Timing, for the log ----
// "Handing over" is how long the game's own code takes to work out what to
// draw and pass it to the graphics card. Every so often one frame is also
// measured "to the end": the graphics card is made to finish everything before
// the clock is stopped, which shows how long the drawing itself takes.
static double g_statHandOverMs = 0;      // one eye's world, all frames
static unsigned long long g_statWorlds = 0;
static double g_statFinishedMs = 0;      // one eye's world, measured to the end
static unsigned long long g_statFinished = 0;
static double g_statSecondMs = 0;        // the second eye's world (stereo in one frame)
static unsigned long long g_statSeconds = 0;
static double g_statSecondFinishedMs = 0;
static unsigned long long g_statSecondFinished = 0;
static unsigned long long g_statStereoRefused = 0;
static unsigned long long g_frameCounter = 0;
// The processor's time for the game's own update (its logic, physics, AI and
// scripts: from the start of the frame to the drawing of the world) and after
// the world (to the end of the frame).
static double g_frameStartedMs = 0, g_worldEndedMs = 0;
static double g_statUpdateMs = 0, g_statAfterMs = 0;
static unsigned long long g_statUpdates = 0, g_statAfters = 0;
// The same for this game frame alone (see SceneLastFrame): the
// update, the left eye's world, the rest of the drawing (the left eye's
// picture taken, the right eye's world and the interface), after the world;
// whether it was measured to the end, and how many worlds it drew.
static double g_framePartMs[4] = {};
static bool g_frameToTheEnd = false;
static int g_frameWorlds = 0;

void SceneFrameStarted()
{
    g_frameStartedMs = NowMs();
    g_worldEndedMs = 0;
    for (double& part : g_framePartMs) part = 0;
    g_frameToTheEnd = false;
    g_frameWorlds = 0;
}

void SceneFrameEnding()
{
    if (g_worldEndedMs > 0)
    {
        double after = NowMs() - g_worldEndedMs;
        g_statAfterMs += after;
        ++g_statAfters;
        g_framePartMs[3] += after;
    }
    g_worldEndedMs = 0;
}

void SceneLastFrame(double parts[4], bool& toTheEnd, int& worlds)
{
    for (int i = 0; i < 4; ++i) parts[i] = g_framePartMs[i];
    toTheEnd = g_frameToTheEnd;
    worlds = g_frameWorlds;
}

void SceneLogTimings()
{
    if (g_statWorlds == 0) return;

    char finished[96] = "not measured";
    if (g_statFinished > 0)
        snprintf(finished, sizeof(finished), "%.1f ms", g_statFinishedMs / (double)g_statFinished);
    Log("SCENE: drawing the world for one eye: %.2f ms to hand over, %s until the graphics card had finished it (%llu frames).",
        g_statHandOverMs / (double)g_statWorlds, finished, g_statWorlds);

    if (g_statSeconds > 0)
    {
        char finishedSecond[96] = "not measured";
        if (g_statSecondFinished > 0)
            snprintf(finishedSecond, sizeof(finishedSecond), "%.1f ms", g_statSecondFinishedMs / (double)g_statSecondFinished);
        Log("SCENE: stereo in one frame: the second eye took %.2f ms to hand over, %s until finished (%llu frames; "
            "%llu frames were drawn the old way instead).",
            g_statSecondMs / (double)g_statSeconds, finishedSecond, g_statSeconds, g_statStereoRefused);
    }

    // The processor (see SceneFrameStarted): the game's own update, each eye
    // handed to the graphics card, and what is left after the world.
    if (g_statUpdates > 0)
    {
        Log("SCENE: the processor, per frame on average (ms): the game's own update (its logic, physics, AI and scripts) %.2f | "
            "handing the left eye over %.2f | the right eye and the interface %.2f | after the world %.2f.",
            g_statUpdateMs / (double)g_statUpdates, g_statHandOverMs / (double)g_statWorlds,
            g_statSeconds > 0 ? g_statSecondMs / (double)g_statSeconds : 0.0,
            g_statAfters > 0 ? g_statAfterMs / (double)g_statAfters : 0.0);
    }
    g_statUpdateMs = g_statAfterMs = 0;
    g_statUpdates = g_statAfters = 0;

    // The right eye without waiting (see the right eye's set): with stereo in
    // one frame, a swap done (the left eye's) and one left out (the right
    // eye's) a frame, and the right eye's set used every frame.
    if (g_answersHooked && g_statWorlds > 0)
    {
        Log("SCENE: hidden-or-not answers, per frame: %.2f swaps done, %.2f left out; the right eye's own set used %.2f times.",
            (double)g_statSwapsDone / (double)g_statWorlds, (double)g_statSwapsLeftOut / (double)g_statWorlds,
            (double)g_statRightSetUsed / (double)g_statWorlds);
    }
    g_statSwapsDone = g_statSwapsLeftOut = g_statRightSetUsed = 0;

    // The right eye's shadows: in how many right eyes' drawings the budget was 0.
    if (g_statRightEyes > 0)
    {
        Log("SCENE: the right eye's shadow maps: the left eye's kept (only changed lights drawn again) in %llu of %llu drawings; "
            "the engine's budget is %d a drawing.",
            g_statRightShadowsKept, g_statRightEyes, g_shadowBudget != nullptr ? (int)*g_shadowBudget : -1);
    }
    g_statRightShadowsKept = g_statRightEyes = 0;

    // The right eye's occlusion queries.
    if (g_queriesHooked && (g_statEarlierQueries > 0 || g_statQueriesGivenBack > 0))
    {
        Log("SCENE: the right eye read the left eye's occlusion queries of the frame before in %llu drawings (%.1f each time); "
            "the held ones given back %llu times.",
            g_statEarlierQueries, g_statEarlierQueries > 0 ? (double)g_statEarlierQueryEntries / (double)g_statEarlierQueries : 0.0,
            g_statQueriesGivenBack);
    }
    g_statEarlierQueries = g_statEarlierQueryEntries = g_statQueriesGivenBack = 0;

    // The right eye's occlusion answers.
    if (g_statAnswersNoted > 0 || g_statAnswersGiven > 0 || g_statAnswersAsked > 0)
    {
        Log("SCENE: occlusion answers: the left eye read %llu (%llu not noted, no room); the right eye was given %llu of them and "
            "asked the card %llu times.", g_statAnswersNoted, g_statAnswersNoRoom, g_statAnswersGiven, g_statAnswersAsked);
    }
    g_statAnswersGiven = g_statAnswersAsked = g_statAnswersNoted = g_statAnswersNoRoom = 0;

    g_statHandOverMs = g_statFinishedMs = g_statSecondMs = g_statSecondFinishedMs = 0;
    g_statWorlds = g_statFinished = g_statSeconds = g_statSecondFinished = g_statStereoRefused = 0;

    // Where the graphics card's time goes, frame by frame (timing marks, see
    // GlTimerMark): 0 the frame begins (vr.cpp), 1 the left eye's world
    // begins, 2 it is done, 3 the left eye's picture has been taken, 4 the
    // right eye's world and the interface are done, 5 the frame ends and the
    // mod's own end of frame begins (vr.cpp), 6 that is done. Each span also
    // holds any time the card sat idle waiting for the game, so the busy parts
    // are the worlds and the mod's spans; the whole frame is what it took.
    double gpu[kGlTimerMarks + 1];
    if (GlTimerTakeAverages(gpu))
    {
        char spans[kGlTimerMarks + 1][16];
        for (int m = 0; m <= kGlTimerMarks; ++m)
        {
            if (gpu[m] < 0.0) snprintf(spans[m], sizeof(spans[m]), "-");
            else snprintf(spans[m], sizeof(spans[m]), "%.2f", gpu[m]);
        }
        Log("SCENE: the graphics card, per frame on average (ms): before the world %s | the left eye's world %s | "
            "taking the left eye's picture %s | the right eye's world and the interface %s | after the world %s | "
            "the mod's end of frame %s | until the next frame %s | the whole frame %s.",
            spans[0], spans[1], spans[2], spans[3], spans[4], spans[5], spans[6], spans[kGlTimerMarks]);
    }
}

static void HookedRenderViewport(void* scene, void* viewport, float frameTime, int flags)
{
    if (!IsWorldViewport(viewport, flags))
    {
        g_realRenderViewport(scene, viewport, frameTime, flags);
        return;
    }

    // One frame in every 450 (every five seconds) is measured to the end (see above). That frame
    // is a little slower for it, which is why it is not done more often.
    ++g_frameCounter;
    bool toTheEnd = (g_frameCounter % 450) == 225;

    bool stereo = GetSettings().stereoFrame && VrStereoCanStart();
    if (GetSettings().stereoFrame && !stereo) ++g_statStereoRefused;

    // The game's own update ends here (see SceneFrameStarted).
    if (g_frameStartedMs > 0)
    {
        double update = NowMs() - g_frameStartedMs;
        g_statUpdateMs += update;
        ++g_statUpdates;
        g_framePartMs[0] += update;
        g_frameStartedMs = 0;
    }
    ++g_frameWorlds;
    if (toTheEnd) g_frameToTheEnd = true;

    // (Timing marks 1 to 4 for the graphics card: see GlTimerMark and the
    // log every ten seconds. 0, 5 and 6 are made in vr.cpp.)
    GlTimerMark(1);
    if (toTheEnd) glFinish();
    double started = NowMs();

    // The left eye's picture (or, without stereo in one frame, the only
    // picture this frame). For stereo the flat interface is left for the
    // second pass.
    // With stereo the camera's view is rebuilt for the left eye too, as for
    // the right eye below, so that it is built for this frame even when the
    // game's logic did not run this frame and did not move its camera. A
    // frame whose view was not built for it is not shown, so with the logic
    // at half the headset's rate there would be single black frames.
    // While an eye's world is drawn the camera holds that eye's own position
    // (CameraUseEyePosition; the view is rebuilt in the drawing for it).
    void* camera = *(void**)((char*)viewport + kViewportCamera);
    // The turn the game's logic has made this frame is followed about the
    // head now, before anything is drawn with it (VrFollowTurn).
    {
        float yaw = 0.0f;
        bool haveYaw = CameraReadYaw(camera, yaw);
        VrFollowTurn(yaw, haveYaw);
    }
    bool eyePosition = GetSettings().eyePositionForDrawing && VrGetFrameSerial() != 0;
    if (stereo || eyePosition) CameraForceRebuild(camera);
    if (eyePosition) CameraUseEyePosition(camera, true);
    EngineTimeEye(1); // the engine's parts are timed per eye too
    SetDrawingEye(1); // its occlusion answers are noted for the right eye
    g_realRenderViewport(scene, viewport, frameTime, stereo ? (flags & ~kPartInterface) : flags);
    SetDrawingEye(0);
    EngineTimeEye(0);
    if (eyePosition) CameraUseEyePosition(camera, false);

    GlTimerMark(2);
    double handedOver = NowMs();
    g_statHandOverMs += handedOver - started;
    ++g_statWorlds;
    if (toTheEnd)
    {
        glFinish();
        g_statFinishedMs += NowMs() - started;
        ++g_statFinished;
    }
    g_worldEndedMs = NowMs(); // (moved on below, once the right eye is drawn too)
    g_framePartMs[1] += g_worldEndedMs - started;
    if (!stereo) return;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        Log("SCENE: stereo in one game frame is on: the world is drawn twice per frame, once for each eye.");
    }

    // Take the left eye's picture and move on to the right eye.
    if (!VrStereoMidFrame())
    {
        // The picture could not be taken. Finish the frame as a normal one:
        // only the flat interface is still missing.
        static int logsLeft = 5;
        if (logsLeft > 0)
        {
            --logsLeft;
            Log("SCENE: the left eye's picture could not be taken this frame; the frame was finished the old way.");
        }
        double leftDone = g_worldEndedMs;
        if (flags & kPartInterface) g_realRenderViewport(scene, viewport, frameTime, flags & kPartInterface);
        g_worldEndedMs = NowMs();
        g_framePartMs[2] += g_worldEndedMs - leftDone;
        return;
    }

    GlTimerMark(3);

    // The camera's view is rebuilt for the right eye (camera.cpp adds the
    // eye's position whenever the engine rebuilds it).
    CameraForceRebuild(camera);

    if (toTheEnd) glFinish();
    double secondStarted = NowMs();
    if (eyePosition) CameraUseEyePosition(camera, true);
    RightEyeAnswersIn(viewport); // the right eye's own hidden-or-not answers (see there)
    RightEyeQueriesIn(viewport); // the left eye's occlusion queries of the frame before (see there)
    RightEyeShadowsIn();         // the left eye's shadow maps kept (see there)
    EngineTimeEye(2);            // and the interface, drawn in this pass
    SetDrawingEye(2);            // the left eye's occlusion answers given again
    g_realRenderViewport(scene, viewport, frameTime, flags);
    SetDrawingEye(0);
    EngineTimeEye(0);
    RightEyeShadowsOut();
    RightEyeAnswersOut();
    if (eyePosition) CameraUseEyePosition(camera, false);
    GlTimerMark(4);
    double leftDone = g_worldEndedMs; // (the left eye's world was done then: all since is the rest of the drawing)
    g_worldEndedMs = NowMs();
    g_statSecondMs += g_worldEndedMs - secondStarted;
    ++g_statSeconds;
    if (toTheEnd)
    {
        glFinish();
        g_statSecondFinishedMs += NowMs() - secondStarted;
        ++g_statSecondFinished;
        g_worldEndedMs = NowMs(); // (the wait for the card belongs to the drawing, not to after the world)
    }
    g_framePartMs[2] += g_worldEndedMs - leftDone;
}

bool InstallSceneHook()
{
    HMODULE game = GetModuleHandleW(nullptr);

    int matches = 0;
    void* target = FindPattern(game, kRenderViewportPattern, &matches);
    if (matches != 1)
    {
        Log("SCENE ERROR: expected to find the engine's viewport-drawing function exactly once, found it %d times.", matches);
        return false;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("SCENE ERROR: could not start the hooking library: %s", MH_StatusToString(status));
        return false;
    }

    status = MH_CreateHook(target, (LPVOID)HookedRenderViewport, (LPVOID*)&g_realRenderViewport);
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        Log("SCENE ERROR: could not hook the viewport-drawing function: %s", MH_StatusToString(status));
        return false;
    }

    Log("SCENE: hooked the engine's viewport-drawing function at exe+0x%llX",
        (unsigned long long)((char*)target - (char*)game));

    // None of the following is required. Without this one the right eye
    // waits for the hidden-or-not answers.
    InstallSwapAnswersHook(game);
    // Without this one the right eye draws its own shadow maps.
    FindShadowBudget(game);
    // Without this one the right eye waits for the left eye's occlusion queries.
    InstallRightQueriesHook(game);
    // Without this one the right eye waits for the lights' answers.
    InstallRightAnswersHook(game);
    return true;
}
