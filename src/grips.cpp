#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "grips.h"
#include "log.h"
#include "posetool.h"
#include "settings.h"
#include "vr.h"
#include "vrmenu.h"

// See grips.h.

static const char* const kSlotNames[kGripSlotCount] = {
    "Fuel", "EmptyMolotovCocktail", "EmptyBottle", "ClothPiece", "FuelBottle", "MolotovCocktail", "Gun", "Shotgun",
    "Ammunition", "ShotgunAmmunition", "DynamoLantern", "Stick", "Torch", "PocketWatch", "Grenade", "GasGrenade",
    "Cutters", "Wrench", "GasMask", "Flare", "Lighter", "PocketBag", "Medkit", "Bandage", "Dynamite", "DynamiteBundle",
    "Meat", "LockdownWheel", "DetonatorHandle", "RabbitDoll", "CommunicationsKey", "PillboxKey", "Fuse",
    // (hands at work)
    "GunSupport", "Bullet", "Cord", "Door", "Loose", "BandageRoll", "Dogtag", "ShotgunPump" };

// The same, as people call them (shown in the menu and the hand poses tool;
// the file keeps the names above).
static const char* const kSlotTitles[kGripSlotCount] = {
    "Fuel can", "Empty molotov", "Empty bottle", "Piece of cloth", "Bottle of fuel", "Molotov cocktail", "Revolver", "Shotgun",
    "Revolver bullets", "Shotgun shells", "Flashlight", "Stick", "Torch", "Pocket watch", "Grenade", "Gas grenade",
    "Chain cutters", "Wrench", "Gas mask", "Flare", "Lighter", "Pocket bag", "Medkit", "Bandage", "Dynamite", "Dynamite bundle",
    "Meat", "Lockdown wheel", "Detonator handle", "Rabbit doll", "Communications key", "Pillbox key", "Fuse",
    "Steadying the gun", "Carrying a bullet", "Pulling the flashlight's cord", "Holding a door handle", "Holding a loose thing",
    "Winding the bandage (the roll)", "Holding a dogtag", "Holding the shotgun's pump" };

const char* GripItemName(int slot)
{
    return (slot >= 0 && slot < kGripSlotCount) ? kSlotNames[slot] : "";
}

const char* GripItemTitle(int slot)
{
    return (slot >= 0 && slot < kGripSlotCount) ? kSlotTitles[slot] : "";
}

bool GripSlotHasPlace(int slot)
{
    return (slot >= 0 && slot < kGripItemCount) || slot == kSlotBandageRoll || slot == kSlotDogtag || slot == kSlotShotgunPump;
}

bool GripSlotHasHand(int slot, int hand)
{
    if (slot < 0 || slot >= kGripSlotCount || hand < 0 || hand > 1) return false;
    if (slot == kSlotGunSupport || slot == kSlotBullet || slot == kSlotShotgunPump) return hand == 0;
    if (slot == kSlotCord || slot == kSlotBandageRoll) return hand == 1;
    return true;
}

// A pose with nothing of its own: as the game has it. For the shotgun's pump,
// its fingers, when switched on, start from the hand the body script draws
// there without them (gvBunkerVR_PumpCurlsA... in helper_bunkervr_shotgun.hps:
// keep the two the same).
static ItemGrip FreshGrip(int slot)
{
    ItemGrip g;
    if (slot == kSlotShotgunPump)
    {
        const float curl[5] = { 0.68f, 0.86f, 0.90f, 0.92f, 0.94f }; // curled round the fore-end, gripping it
        const float spread[5] = { 30.0f, -4.0f, 0.0f, 3.0f, 6.0f };
        for (int i = 0; i < 5; ++i)
        {
            g.pose.curl[i] = curl[i];
            g.pose.spread[i] = spread[i];
        }
        g.pose.cup = 0.60f;
    }
    return g;
}

// Each pose, for each hand (0 left, 1 right), and the named ones.
static ItemGrip g_grips[kGripSlotCount][2];
static NamedPose g_library[kPoseLibraryMost];
static int g_libraryCount = 0;
static bool g_dirty = false;       // changed since last saved (only the tool leaves changes unsaved)
static unsigned g_version = 1;     // goes up with every change

// Which item each hand holds (as the body script reports it), and when it last reported.
static int g_held[2] = { -1, -1 };
static unsigned long long g_heldTime = 0;

// The game's own placing of the item in each hand (the news): its middle
// point, its z and y directions, in the palm's terms; and when each came.
static float g_gamePlace[2][3][3] = {};
static unsigned long long g_gameTime[2][3] = {};

// The hand at work each hand is now (the body script asks for that pose).
static int g_atWork[2] = { -1, -1 };
static unsigned long long g_atWorkTime[2] = { 0, 0 };

static GripView g_view;
static GripView g_viewShown; // what ViewFromHeld last put on the page (to tell what was changed there)
static bool g_viewWasValid = false;

static volatile LONG g_showGameFingers = 0;

static void Changed()
{
    ++g_version;
}

static int Held(int hand)
{
    if (hand < 0 || hand > 1) return -1;
    if (g_heldTime == 0 || GetTickCount64() - g_heldTime > 500) return -1; // the scripts have gone quiet (a menu, a loading screen)
    return g_held[hand];
}

static int ViewHand()
{
    return g_view.hand == 0 ? 1 : 0; // the menu's choice: 0 "Right", 1 "Left"
}

static float Clamp(float value, float lowest, float highest)
{
    return value < lowest ? lowest : (value > highest ? highest : value);
}

static HandPose SensiblePose(const HandPose& wanted)
{
    HandPose p = wanted;
    for (int i = 0; i < 5; ++i)
    {
        p.curl[i] = Clamp(p.curl[i], 0.0f, 1.0f);
        p.spread[i] = i == 0 ? Clamp(p.spread[i], -60.0f, 60.0f) : Clamp(p.spread[i], -40.0f, 40.0f);
    }
    p.cup = Clamp(p.cup, 0.0f, 1.0f);
    return p;
}

// How far an item's center may be from the center of the palm, along each of
// the three axes (meters). Long items need more than 30 cm: the chain cutters
// are held halfway up their handles. The Grips page and the hand poses tool
// allow the same range.
static const float kGripPlaceMost = 0.5f;

// A pose as it may be kept (within the page's, the tool's and the file's limits).
static ItemGrip Sensible(const ItemGrip& wanted, int slot)
{
    ItemGrip g = wanted;
    if (!GripSlotHasPlace(slot)) g.set = false;
    g.forward = Clamp(g.forward, -kGripPlaceMost, kGripPlaceMost);
    g.up = Clamp(g.up, -kGripPlaceMost, kGripPlaceMost);
    g.right = Clamp(g.right, -kGripPlaceMost, kGripPlaceMost);
    g.turnRight = Clamp(g.turnRight, -180.0f, 180.0f);
    g.tipUp = Clamp(g.tipUp, -89.0f, 89.0f);
    g.leanRight = Clamp(g.leanRight, -180.0f, 180.0f);
    g.pose = SensiblePose(g.pose);
    return g;
}

// ---- Turns (3x3, row after row, as HeadPose) ----

static const float kDegrees = 57.2957795f;

// A grip's turn from its three angles: Ry(a) Rx(b) Rz(c) about the palm's
// own y (up), x (right) and z (backwards), with a = -turned right, b =
// tipped up, c = -leaning right.
static void TurnFromAngles(float turnRight, float tipUp, float leanRight, float r[9])
{
    float a = -turnRight / kDegrees, b = tipUp / kDegrees, c = -leanRight / kDegrees;
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    r[0] = ca * cc + sa * sb * sc;  r[1] = -ca * sc + sa * sb * cc; r[2] = sa * cb;
    r[3] = cb * sc;                 r[4] = cb * cc;                 r[5] = -sb;
    r[6] = -sa * cc + ca * sb * sc; r[7] = sa * sc + ca * sb * cc;  r[8] = ca * cb;
}

static void AnglesFromTurn(const float r[9], float& turnRight, float& tipUp, float& leanRight)
{
    float sb = -r[5];
    if (sb > 1.0f) sb = 1.0f;
    if (sb < -1.0f) sb = -1.0f;
    tipUp = asinf(sb) * kDegrees;
    turnRight = -atan2f(r[2], r[8]) * kDegrees;
    leanRight = -atan2f(r[3], r[4]) * kDegrees;
}

// A turn from where its own z and y directions point (x = y cross z).
static bool TurnFromZY(const float z[3], const float y[3], float r[9])
{
    float lz = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (lz < 0.5f) return false;
    float Z[3] = { z[0] / lz, z[1] / lz, z[2] / lz };
    float X[3] = { y[1] * Z[2] - y[2] * Z[1], y[2] * Z[0] - y[0] * Z[2], y[0] * Z[1] - y[1] * Z[0] };
    float lx = sqrtf(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
    if (lx < 0.5f) return false; // y nowhere near square to z
    X[0] /= lx; X[1] /= lx; X[2] /= lx;
    float Y[3] = { Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0] };
    r[0] = X[0]; r[1] = Y[0]; r[2] = Z[0];
    r[3] = X[1]; r[4] = Y[1]; r[5] = Z[1];
    r[6] = X[2]; r[7] = Y[2]; r[8] = Z[2];
    return true;
}

// A rigid place: turned by r, then moved by t.
struct Place
{
    float r[9];
    float t[3];
};

static void Combine(const Place& a, const Place& b, Place& out)
{
    Place p;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            p.r[i * 3 + j] = a.r[i * 3 + 0] * b.r[0 * 3 + j] + a.r[i * 3 + 1] * b.r[1 * 3 + j] + a.r[i * 3 + 2] * b.r[2 * 3 + j];
        p.t[i] = a.t[i] + a.r[i * 3 + 0] * b.t[0] + a.r[i * 3 + 1] * b.t[1] + a.r[i * 3 + 2] * b.t[2];
    }
    out = p;
}

static void Inverse(const Place& a, Place& out)
{
    Place p;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.r[i * 3 + j] = a.r[j * 3 + i];
    for (int i = 0; i < 3; ++i) p.t[i] = -(p.r[i * 3 + 0] * a.t[0] + p.r[i * 3 + 1] * a.t[1] + p.r[i * 3 + 2] * a.t[2]);
    out = p;
}

static void PlaceFromPose(const HeadPose& pose, Place& out)
{
    for (int i = 0; i < 9; ++i) out.r[i] = pose.rotation[i];
    for (int i = 0; i < 3; ++i) out.t[i] = pose.position[i];
}

// A grip's placement in the palm's terms (x right, y up, z backwards).
static void PlaceFromGrip(const ItemGrip& grip, Place& out)
{
    TurnFromAngles(grip.turnRight, grip.tipUp, grip.leanRight, out.r);
    out.t[0] = grip.right;
    out.t[1] = grip.up;
    out.t[2] = -grip.forward;
}

static void GripFromPlace(const Place& place, ItemGrip& grip)
{
    AnglesFromTurn(place.r, grip.turnRight, grip.tipUp, grip.leanRight);
    grip.right = place.t[0];
    grip.up = place.t[1];
    grip.forward = -place.t[2];
}

// The game's own placing of the item in a hand, as a grip's placement
// (false if it hasn't been reported lately).
static bool GamePlacing(int hand, ItemGrip& grip)
{
    unsigned long long now = GetTickCount64();
    for (int part = 0; part < 3; ++part)
        if (g_gameTime[hand][part] == 0 || now - g_gameTime[hand][part] > 500) return false;
    Place place;
    if (!TurnFromZY(g_gamePlace[hand][1], g_gamePlace[hand][2], place.r)) return false;
    for (int i = 0; i < 3; ++i) place.t[i] = g_gamePlace[hand][0][i];
    GripFromPlace(place, grip);
    return true;
}

// ---- The file ----

static bool GripsPath(wchar_t* path)
{
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') --length;
    path[length] = L'\0';
    if (length + 32 >= MAX_PATH) return false;
    lstrcatW(path, L"BunkerVR_grips.txt");
    return true;
}

// A number to be written with `decimals` places: one that rounds to nothing
// becomes 0, so it isn't written as "-0" (a mirrored angle of 0 would be).
static float Tidy(float value, int decimals)
{
    float half = decimals == 0 ? 0.5f : (decimals == 1 ? 0.05f : 0.005f);
    return (value > -half && value < half) ? 0.0f : value;
}

static int WritePose(char* text, int room, const HandPose& p)
{
    return snprintf(text, room, "fingers %.2f %.2f %.2f %.2f %.2f; spread %.0f %.0f %.0f %.0f %.0f; cup %.2f", Tidy(p.curl[0], 2), Tidy(p.curl[1], 2),
                    Tidy(p.curl[2], 2), Tidy(p.curl[3], 2), Tidy(p.curl[4], 2), Tidy(p.spread[0], 0), Tidy(p.spread[1], 0), Tidy(p.spread[2], 0),
                    Tidy(p.spread[3], 0), Tidy(p.spread[4], 0), Tidy(p.cup, 2));
}

static void SaveGrips()
{
    wchar_t path[MAX_PATH];
    if (!GripsPath(path)) return;
    static char text[65536];
    int length = snprintf(text, sizeof(text),
        "# BunkerVR: how the game's items sit in the hand, and how the hands are posed.\r\n"
        "# Made by the hand poses tool (F11 on the PC) and the \"Grips\" page of the mod's menu (Y); it can be\r\n"
        "# changed here too, with the game closed. One line per pose (anything with no line is as the game has it):\r\n"
        "#   name hand: place forwards up right (cm, from the middle of the palm) turned-right tipped-up leaning-right\r\n"
        "#              (degrees), or place game; fingers thumb first middle ring little (0 straight to 1 closed), or\r\n"
        "#              fingers game; spread the same five (degrees sideways: + towards the little finger; the thumb:\r\n"
        "#              + out, - across the palm); cup (0 flat to 1 cupped)\r\n"
        "#   pose \"name\": fingers ...; spread ...; cup ...   (poses of your own)\r\n");
    for (int slot = 0; slot < kGripSlotCount; ++slot)
    {
        for (int hand = 1; hand >= 0; --hand)
        {
            const ItemGrip& g = g_grips[slot][hand];
            if ((!g.set && !g.fingersSet) || length <= 0 || length >= (int)sizeof(text) - 512) continue;
            length += snprintf(text + length, sizeof(text) - length, "%s %s: ", kSlotNames[slot], hand == 0 ? "left" : "right");
            if (GripSlotHasPlace(slot))
            {
                if (g.set)
                    length += snprintf(text + length, sizeof(text) - length, "place %.1f %.1f %.1f %.0f %.0f %.0f; ", Tidy(g.forward * 100.0f, 1),
                                       Tidy(g.up * 100.0f, 1), Tidy(g.right * 100.0f, 1), Tidy(g.turnRight, 0), Tidy(g.tipUp, 0), Tidy(g.leanRight, 0));
                else
                    length += snprintf(text + length, sizeof(text) - length, "place game; ");
            }
            if (g.fingersSet) length += WritePose(text + length, (int)sizeof(text) - length, g.pose);
            else length += snprintf(text + length, sizeof(text) - length, "fingers game");
            length += snprintf(text + length, sizeof(text) - length, "\r\n");
        }
    }
    for (int i = 0; i < g_libraryCount && length > 0 && length < (int)sizeof(text) - 512; ++i)
    {
        length += snprintf(text + length, sizeof(text) - length, "pose \"%s\": ", g_library[i].name);
        length += WritePose(text + length, (int)sizeof(text) - length, g_library[i].pose);
        length += snprintf(text + length, sizeof(text) - length, "\r\n");
    }
    if (length <= 0) return;
    if (length >= (int)sizeof(text)) length = (int)sizeof(text) - 1;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Log("GRIPS: BunkerVR_grips.txt could not be written.");
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, (DWORD)length, &written, nullptr);
    CloseHandle(file);
    g_dirty = false;
}

// Up to `most` numbers from the start of text; how many were read.
static int ReadNumbers(const char* text, float* out, int most)
{
    int count = 0;
    const char* at = text;
    while (count < most)
    {
        int used = 0;
        if (sscanf(at, " %f%n", &out[count], &used) != 1 || used <= 0) break;
        at += used;
        ++count;
    }
    return count;
}

static const char* SkipSpaces(const char* text)
{
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

// "place ...; fingers ...; spread ...; cup ..." into a grip; false if a part
// is not understood.
static bool ReadParts(char* text, ItemGrip& g, bool allowPlace)
{
    char* part = text;
    while (part && *part)
    {
        char* next = strchr(part, ';');
        if (next) *next++ = '\0';
        const char* p = SkipSpaces(part);
        float v[6];
        if (*p == '\0')
        {
        }
        else if (strncmp(p, "place", 5) == 0 && allowPlace)
        {
            const char* rest = SkipSpaces(p + 5);
            if (strncmp(rest, "game", 4) == 0) g.set = false;
            else if (ReadNumbers(rest, v, 6) == 6)
            {
                g.set = true;
                g.forward = v[0] / 100.0f;
                g.up = v[1] / 100.0f;
                g.right = v[2] / 100.0f;
                g.turnRight = v[3];
                g.tipUp = v[4];
                g.leanRight = v[5];
            }
            else return false;
        }
        else if (strncmp(p, "fingers", 7) == 0)
        {
            const char* rest = SkipSpaces(p + 7);
            if (strncmp(rest, "game", 4) == 0) g.fingersSet = false;
            else if (ReadNumbers(rest, v, 5) == 5)
            {
                g.fingersSet = true;
                for (int i = 0; i < 5; ++i) g.pose.curl[i] = v[i];
            }
            else return false;
        }
        else if (strncmp(p, "spread", 6) == 0)
        {
            if (ReadNumbers(p + 6, v, 5) != 5) return false;
            for (int i = 0; i < 5; ++i) g.pose.spread[i] = v[i];
        }
        else if (strncmp(p, "cup", 3) == 0)
        {
            if (ReadNumbers(p + 3, v, 1) != 1) return false;
            g.pose.cup = v[0];
        }
        else return false;
        part = next;
    }
    return true;
}

static void ReadGripsFile(bool quiet)
{
    for (int slot = 0; slot < kGripSlotCount; ++slot) g_grips[slot][0] = g_grips[slot][1] = FreshGrip(slot);
    g_libraryCount = 0;
    g_dirty = false;
    Changed();

    wchar_t path[MAX_PATH];
    if (!GripsPath(path)) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (!quiet) Log("GRIPS: no BunkerVR_grips.txt yet: every item is held as the game holds it.");
        return;
    }
    static char text[65536];
    DWORD read = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    text[read] = '\0';

    int loaded = 0, refused = 0;
    char* line = text;
    while (line && *line)
    {
        char* next = strpbrk(line, "\r\n");
        if (next)
        {
            *next = '\0';
            ++next;
            while (*next == '\r' || *next == '\n') ++next;
        }
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '#' || *line == '\0')
        {
            line = next;
            continue;
        }
        bool ok = false;
        if (strncmp(line, "pose \"", 6) == 0)
        {
            // A pose of your own: pose "name": ...
            char* nameStart = line + 6;
            char* nameEnd = strchr(nameStart, '"');
            char* colon = nameEnd ? strchr(nameEnd, ':') : nullptr;
            if (nameEnd && colon && nameEnd > nameStart && g_libraryCount < kPoseLibraryMost)
            {
                *nameEnd = '\0';
                ItemGrip g;
                g.fingersSet = true;
                if (ReadParts(colon + 1, g, false))
                {
                    NamedPose& named = g_library[g_libraryCount++];
                    snprintf(named.name, sizeof(named.name), "%s", nameStart);
                    named.pose = SensiblePose(g.pose);
                    ok = true;
                }
            }
        }
        else
        {
            char name[64] = "", handWord[16] = "";
            int used = 0;
            if (sscanf(line, "%63s %15[^:]:%n", name, handWord, &used) == 2 && used > 0)
            {
                int slot = -1;
                for (int i = 0; i < kGripSlotCount; ++i)
                    if (strcmp(name, kSlotNames[i]) == 0) slot = i;
                int hand = strcmp(handWord, "left") == 0 ? 0 : (strcmp(handWord, "right") == 0 ? 1 : -1);
                char* rest = line + used;
                const char* first = SkipSpaces(rest);
                ItemGrip g = FreshGrip(slot);
                if (slot >= 0 && hand >= 0 && GripSlotHasHand(slot, hand))
                {
                    if (isdigit((unsigned char)*first) || *first == '-' || *first == '+' || *first == '.')
                    {
                        // The older line format, still read: forwards up right | turned tipped leaning | thumb first others (or game).
                        float v[6] = {};
                        int afterPlace = 0;
                        if (sscanf(rest, " %f %f %f | %f %f %f | %n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &afterPlace) == 6 && afterPlace > 0)
                        {
                            g.set = true;
                            g.forward = v[0] / 100.0f;
                            g.up = v[1] / 100.0f;
                            g.right = v[2] / 100.0f;
                            g.turnRight = v[3];
                            g.tipUp = v[4];
                            g.leanRight = v[5];
                            const char* fingers = rest + afterPlace;
                            float f[3];
                            if (strncmp(fingers, "game", 4) == 0) ok = true;
                            else if (ReadNumbers(fingers, f, 3) == 3)
                            {
                                g.fingersSet = true;
                                g.pose.curl[0] = f[0];
                                g.pose.curl[1] = f[1];
                                g.pose.curl[2] = g.pose.curl[3] = g.pose.curl[4] = f[2];
                                ok = true;
                            }
                        }
                    }
                    else
                    {
                        ok = ReadParts(rest, g, GripSlotHasPlace(slot));
                    }
                }
                if (ok)
                {
                    g_grips[slot][hand] = Sensible(g, slot);
                    ++loaded;
                }
            }
        }
        if (!ok)
        {
            ++refused;
            Log("GRIPS: a line in BunkerVR_grips.txt was not understood: %.80s", line);
        }
        line = next;
    }
    Log("GRIPS: %d poses and %d poses of your own read from BunkerVR_grips.txt%s.", loaded, g_libraryCount,
        refused > 0 ? " (some lines were not understood: see above)" : "");
}

void LoadGrips()
{
    // The list of items, checked against the game's own (in case a game
    // update changed it: the scripts would then name the wrong items).
    {
        int size = 0;
        unsigned char* inventory = ReadGameFile("config\\Inventory.cfg", size);
        if (inventory)
        {
            int found = 0, same = 0;
            const char* at = (const char*)inventory;
            while ((at = strstr(at, "<ItemType ")) != nullptr)
            {
                const char* id = strstr(at, " ID=\"");
                const char* end = strchr(at, '>');
                at += 10;
                if (!id || (end && id > end)) continue;
                id += 5;
                const char* quote = strchr(id, '"');
                if (!quote) break;
                if (found < kGripItemCount && (size_t)(quote - id) == strlen(kSlotNames[found]) && strncmp(id, kSlotNames[found], quote - id) == 0) ++same;
                ++found;
            }
            delete[] inventory;
            if (found == kGripItemCount && same == kGripItemCount)
                Log("GRIPS: the game's %d item types are as expected.", found);
            else
                Log("GRIPS: WARNING: the game's item list differs (%d items, %d of %d as expected): grips may be given to the wrong items.",
                    found, same, kGripItemCount);
        }
        else
        {
            Log("GRIPS: config\\Inventory.cfg could not be read to check the item list.");
        }
    }
    ReadGripsFile(false);
}

// ---- From the scripts ----

void GripsHeldNews(const float numbers[3])
{
    for (int hand = 0; hand < 2; ++hand)
    {
        int item = (int)floorf(numbers[hand] + 0.5f);
        int now = (item >= 0 && item < kGripItemCount) ? item : -1;
        if (now != g_held[hand] && now >= 0)
        {
            static int logsLeft = 30;
            if (logsLeft > 0)
            {
                --logsLeft;
                const ItemGrip& g = g_grips[now][hand];
                Log("GRIPS: the %s hand holds %s (%s placement, %s fingers).", hand == 0 ? "left" : "right", kSlotNames[now],
                    g.set ? "its own" : "the game's", g.fingersSet ? "its own" : "the game's");
            }
        }
        g_held[hand] = now;
    }
    g_heldTime = GetTickCount64();
}

void GripsGamePlacingNews(int hand, int part, const float numbers[3])
{
    if (hand < 0 || hand > 1 || part < 0 || part > 2) return;
    for (int i = 0; i < 3; ++i) g_gamePlace[hand][part][i] = numbers[i];
    g_gameTime[hand][part] = GetTickCount64();
}

static volatile LONG g_deskOpen = 0;

void GripsAnswer(int hand, int part, int which, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    int slot = -1;
    if (which == 0) slot = Held(hand);
    else if (which - 1 >= 0 && which - 1 < kGripSlotCount && GripSlotHasHand(which - 1, hand)) slot = which - 1; // a hand at work, or an item held in a way the game doesn't report
    if (part == 0)
    {
        if (which != 0 && slot >= 0)
        {
            g_atWork[hand] = slot; // asking means the hand is doing that right now
            g_atWorkTime[hand] = GetTickCount64();
        }
        bool forItem = which == 0 || GripSlotHasPlace(slot); // the game's placing is only wanted for things that can be placed
        bool wantGame = forItem && (g_view.editing || (VrMenuIsOpen() && VrMenuOnGripsPage()) || g_deskOpen != 0);
        int flags = wantGame ? 4 : 0;
        if (slot >= 0)
        {
            const ItemGrip& g = g_grips[slot][hand];
            if (g.set && GripSlotHasPlace(slot)) flags |= 1;
            if (g.fingersSet && g_showGameFingers == 0) flags |= 2;
        }
        out[0] = 12345.0f;
        out[1] = (float)flags;
        out[2] = (float)slot;
        return;
    }
    if (slot < 0) return;
    const ItemGrip& g = g_grips[slot][hand];
    const HandPose& p = g.pose;
    switch (part)
    {
    case 4: out[0] = p.curl[0]; out[1] = p.curl[1]; out[2] = p.curl[2]; return;
    case 5: out[0] = p.curl[3]; out[1] = p.curl[4]; out[2] = p.cup; return;
    case 6: out[0] = p.spread[0]; out[1] = p.spread[1]; out[2] = p.spread[2]; return;
    case 7: out[0] = p.spread[3]; out[1] = p.spread[4]; out[2] = 0.0f; return;
    default: break;
    }
    if (!g.set || !GripSlotHasPlace(slot)) return;
    Place place;
    PlaceFromGrip(g, place);
    if (part == 1)
    {
        for (int i = 0; i < 3; ++i) out[i] = place.t[i];
    }
    else if (part == 2 || part == 3)
    {
        int column = part == 2 ? 2 : 1; // its own z, its own y
        for (int i = 0; i < 3; ++i) out[i] = place.r[i * 3 + column];
    }
}

// ---- The hand poses tool's copy and its requests (see grips.h) ----

static SRWLOCK g_deskLock = SRWLOCK_INIT;
static GripDeskState g_desk;
struct DeskRequest
{
    int slot, hand;
    ItemGrip grip;
};
static const int kDeskQueueSize = kGripSlotCount * 2; // one waiting change per pose at most, so it never fills
static DeskRequest g_deskQueue[kDeskQueueSize];
static int g_deskQueueCount = 0;
static bool g_deskLibraryWaiting = false;
static NamedPose g_deskLibrary[kPoseLibraryMost];
static int g_deskLibraryCount = 0;
static bool g_deskSaveWaiting = false;
static bool g_deskRevertWaiting = false;

void GripsDeskSnapshot(GripDeskState& out)
{
    AcquireSRWLockShared(&g_deskLock);
    out = g_desk;
    ReleaseSRWLockShared(&g_deskLock);
}

void GripsDeskSetSlot(int slot, int hand, const ItemGrip& grip)
{
    if (!GripSlotHasHand(slot, hand)) return;
    AcquireSRWLockExclusive(&g_deskLock);
    // A change to a pose already waiting replaces it: only the latest counts.
    // "Reset all" sends every pose at once, possibly between two of the game's
    // frames.
    int at = 0;
    while (at < g_deskQueueCount && (g_deskQueue[at].slot != slot || g_deskQueue[at].hand != hand)) ++at;
    if (at == g_deskQueueCount)
    {
        if (g_deskQueueCount >= kDeskQueueSize) at = kDeskQueueSize - 1; // can't happen
        else ++g_deskQueueCount;
    }
    g_deskQueue[at].slot = slot;
    g_deskQueue[at].hand = hand;
    g_deskQueue[at].grip = grip;
    ReleaseSRWLockExclusive(&g_deskLock);
}

void GripsDeskSetLibrary(const NamedPose* poses, int count)
{
    if (count < 0) count = 0;
    if (count > kPoseLibraryMost) count = kPoseLibraryMost;
    AcquireSRWLockExclusive(&g_deskLock);
    for (int i = 0; i < count; ++i) g_deskLibrary[i] = poses[i];
    g_deskLibraryCount = count;
    g_deskLibraryWaiting = true;
    ReleaseSRWLockExclusive(&g_deskLock);
}

void GripsDeskSave()
{
    AcquireSRWLockExclusive(&g_deskLock);
    g_deskSaveWaiting = true;
    ReleaseSRWLockExclusive(&g_deskLock);
}

void GripsDeskRevert()
{
    AcquireSRWLockExclusive(&g_deskLock);
    g_deskRevertWaiting = true;
    g_deskQueueCount = 0;
    g_deskLibraryWaiting = false;
    ReleaseSRWLockExclusive(&g_deskLock);
}

void GripsSetDeskOpen(bool open)
{
    InterlockedExchange(&g_deskOpen, open ? 1 : 0);
}

void GripsSetShowGameFingers(bool show)
{
    InterlockedExchange(&g_showGameFingers, show ? 1 : 0);
}

// ---- Each frame ----

// The pose the page is about for a hand: the item it holds, or else, while the
// scripts say the hand is doing it, a hand at work that has a placement of its
// own (the shotgun's pump, the bandage's roll, a dogtag). -1 for none.
// Those are set by their numbers: "Move it with the other hand" is for items.
static int ViewSlot(int hand)
{
    int item = Held(hand);
    if (item >= 0 || hand < 0 || hand > 1) return item;
    if (g_atWorkTime[hand] == 0 || GetTickCount64() - g_atWorkTime[hand] > 300) return -1;
    int slot = g_atWork[hand];
    return (slot >= kGripItemCount && GripSlotHasPlace(slot)) ? slot : -1;
}

// The numbers the page shows: the held item's own grip, or where the game
// places it (so that the first change starts from there).
static bool ViewFromHeld(GripView& view)
{
    int hand = ViewHand();
    int item = ViewSlot(hand);
    if (item < 0) return false;
    const ItemGrip& g = g_grips[item][hand];
    ItemGrip place = g;
    if (!g.set && !GamePlacing(hand, place)) return false;
    view.forward = place.forward;
    view.up = place.up;
    view.right = place.right;
    view.turnRight = place.turnRight;
    view.tipUp = place.tipUp;
    view.leanRight = place.leanRight;
    view.fingersSet = g.fingersSet;
    view.thumb = g.pose.curl[0];
    view.first = g.pose.curl[1];
    view.others = (g.pose.curl[2] + g.pose.curl[3] + g.pose.curl[4]) / 3.0f;
    return true;
}

bool GripsTick()
{
    unsigned long long now = GetTickCount64();

    // The hand poses tool's changes, made here on the game's own thread.
    {
        static DeskRequest requests[kDeskQueueSize];
        static NamedPose library[kPoseLibraryMost];
        int count = 0, libraryCount = 0;
        bool libraryWaiting = false, saveWaiting = false, revertWaiting = false;
        AcquireSRWLockExclusive(&g_deskLock);
        count = g_deskQueueCount;
        for (int i = 0; i < count; ++i) requests[i] = g_deskQueue[i];
        g_deskQueueCount = 0;
        libraryWaiting = g_deskLibraryWaiting;
        if (libraryWaiting)
        {
            libraryCount = g_deskLibraryCount;
            for (int i = 0; i < libraryCount; ++i) library[i] = g_deskLibrary[i];
            g_deskLibraryWaiting = false;
        }
        saveWaiting = g_deskSaveWaiting;
        revertWaiting = g_deskRevertWaiting;
        g_deskSaveWaiting = g_deskRevertWaiting = false;
        ReleaseSRWLockExclusive(&g_deskLock);

        if (revertWaiting)
        {
            ReadGripsFile(true);
            Log("GRIPS: the hand poses tool: back to what was last saved.");
        }
        for (int i = 0; i < count; ++i)
        {
            const DeskRequest& request = requests[i];
            if (!GripSlotHasHand(request.slot, request.hand)) continue;
            g_grips[request.slot][request.hand] = Sensible(request.grip, request.slot);
            g_dirty = true;
            Changed();
        }
        if (libraryWaiting)
        {
            for (int i = 0; i < libraryCount; ++i)
            {
                g_library[i] = library[i];
                g_library[i].name[kPoseNameMost - 1] = '\0';
                g_library[i].pose = SensiblePose(library[i].pose);
            }
            g_libraryCount = libraryCount;
            g_dirty = true;
            Changed();
        }
        if (saveWaiting)
        {
            SaveGrips();
            Changed();
            Log("GRIPS: the hand poses tool: saved.");
        }
    }

    // Setting a grip by hand ends when that hand no longer holds the item.
    if (g_view.editing && Held(ViewHand()) < 0)
    {
        g_view.editing = false;
        Log("GRIPS: setting the grip by hand ends: the %s hand no longer holds an item.", ViewHand() == 0 ? "left" : "right");
    }
    g_view.onPc = PoseToolIsOpen(); // so the menu's switch also shows the tool being closed on the PC

    GripView before = g_view;
    bool valid = ViewFromHeld(g_view);
    if (valid) g_viewShown = g_view;
    bool changed = valid != g_viewWasValid || memcmp(&before, &g_view, sizeof(GripView)) != 0;
    g_viewWasValid = valid;

    // The copy for the hand poses tool.
    {
        static GripDeskState state;
        state.fresh = g_heldTime != 0 && now - g_heldTime <= 500;
        state.gunRight = CameraGunHeld();
        state.headsetOff = VrHeadsetIsOff();
        state.handsFrozen = VrHandsFrozen();
        float turns = 0.0f;
        state.bandageWinding = VrGetBandage(turns) == 1;
        state.dirty = g_dirty;
        state.version = g_version;
        state.tickMs = now;
        for (int hand = 0; hand < 2; ++hand)
        {
            GripDeskHand& h = state.hand[hand];
            h.item = Held(hand);
            h.atWork = (g_atWorkTime[hand] != 0 && now - g_atWorkTime[hand] < 300) ? g_atWork[hand] : -1;
            // Where the game places the item: the one the hand holds in a way
            // the game doesn't report, if any, otherwise the one it holds.
            int placed = GripSlotHasPlace(h.atWork) ? h.atWork : h.item;
            h.known = placed >= 0 && GamePlacing(hand, h.game);
        }
        for (int slot = 0; slot < kGripSlotCount; ++slot)
        {
            state.table[slot][0] = g_grips[slot][0];
            state.table[slot][1] = g_grips[slot][1];
        }
        state.libraryCount = g_libraryCount;
        for (int i = 0; i < g_libraryCount; ++i) state.library[i] = g_library[i];
        AcquireSRWLockExclusive(&g_deskLock);
        g_desk = state;
        ReleaseSRWLockExclusive(&g_deskLock);
    }
    return changed;
}

static bool g_nudging = false;
static Place g_nudgeOther0; // the other hand in the holding hand's terms, as it took hold
static Place g_nudgeGrip0;  // the grip's placement then

int GripsTakeSqueezes(const bool squeezed[2])
{
    if (!g_view.editing)
    {
        g_nudging = false;
        return 0;
    }
    int hand = ViewHand();
    int other = 1 - hand;
    int item = Held(hand);
    if (item < 0)
    {
        g_nudging = false;
        return 1;
    }

    HeadPose holdPose, otherPose;
    bool posesKnown = VrGetGripHandPose(hand, holdPose) && VrGetGripHandPose(other, otherPose);
    Place hold, otherPlace, otherInHand;
    if (posesKnown)
    {
        PlaceFromPose(holdPose, hold);
        PlaceFromPose(otherPose, otherPlace);
        Place holdInverse;
        Inverse(hold, holdInverse);
        Combine(holdInverse, otherPlace, otherInHand);
    }

    if (squeezed[other] && !g_nudging)
    {
        // Takes hold: from the item's own placement, or from where the game places it.
        ItemGrip start = g_grips[item][hand];
        if (!start.set)
        {
            ItemGrip game;
            if (!posesKnown || !GamePlacing(hand, game)) return 1; // not known yet: wait
            start.forward = game.forward;
            start.up = game.up;
            start.right = game.right;
            start.turnRight = game.turnRight;
            start.tipUp = game.tipUp;
            start.leanRight = game.leanRight;
        }
        if (!posesKnown) return 1;
        PlaceFromGrip(start, g_nudgeGrip0);
        g_nudgeOther0 = otherInHand;
        g_nudging = true;
        start.set = true;
        g_grips[item][hand] = start;
        Changed();
        Log("GRIPS: the %s hand takes hold of %s to set its grip.", other == 0 ? "left" : "right", kSlotNames[item]);
        return 2;
    }

    if (squeezed[other] && g_nudging && posesKnown)
    {
        // G = D x G0, D = how the other hand has moved in the holding hand's
        // terms: the item stays put in the other hand.
        Place other0Inverse, moved, place;
        Inverse(g_nudgeOther0, other0Inverse);
        Combine(otherInHand, other0Inverse, moved);
        Combine(moved, g_nudgeGrip0, place);
        ItemGrip& g = g_grips[item][hand];
        GripFromPlace(place, g);
        g.forward = Clamp(g.forward, -kGripPlaceMost, kGripPlaceMost);
        g.up = Clamp(g.up, -kGripPlaceMost, kGripPlaceMost);
        g.right = Clamp(g.right, -kGripPlaceMost, kGripPlaceMost);
        g.set = true;
        Changed();
        return 1;
    }

    if (!squeezed[other] && g_nudging)
    {
        g_nudging = false;
        SaveGrips();
        const ItemGrip& g = g_grips[item][hand];
        Log("GRIPS: %s in the %s hand set by hand: %.1f %.1f %.1f cm (forwards, up, right), turned %.0f, tipped %.0f, leaning %.0f degrees; saved.",
            kSlotNames[item], hand == 0 ? "left" : "right", g.forward * 100.0f, g.up * 100.0f, g.right * 100.0f, g.turnRight, g.tipUp, g.leanRight);
        return 3;
    }
    return 1;
}

// ---- The menu ----

GripView& GripsView()
{
    return g_view;
}

void GripsMenuEdited()
{
    int hand = ViewHand();
    int item = ViewSlot(hand);
    if (item < 0 || !g_viewWasValid) return; // nothing held, or where the game places it not known yet
    ItemGrip& g = g_grips[item][hand];
    const GripView& v = g_view;
    const GripView& was = g_viewShown;
    // The placement only if one of its numbers was changed (a change to the
    // fingers leaves it the game's way, if it was).
    if (v.forward != was.forward || v.up != was.up || v.right != was.right || v.turnRight != was.turnRight || v.tipUp != was.tipUp ||
        v.leanRight != was.leanRight)
    {
        g.set = true;
        g.forward = v.forward;
        g.up = v.up;
        g.right = v.right;
        g.turnRight = v.turnRight;
        g.tipUp = v.tipUp;
        g.leanRight = v.leanRight;
    }
    g.fingersSet = v.fingersSet;
    g.pose.curl[0] = v.thumb;
    g.pose.curl[1] = v.first;
    if (v.others != was.others) g.pose.curl[2] = g.pose.curl[3] = g.pose.curl[4] = v.others; // the three together, only when that number was changed
    g = Sensible(g, item);
    Changed();
    SaveGrips();
}

void GripsMenuReset()
{
    int hand = ViewHand();
    int item = ViewSlot(hand);
    if (item < 0) return;
    g_grips[item][hand] = FreshGrip(item);
    g_view.editing = false;
    g_nudging = false;
    Changed();
    SaveGrips();
    Log("GRIPS: %s in the %s hand is held as the game holds it again.", kSlotNames[item], hand == 0 ? "left" : "right");
}

void GripsMenuEditing()
{
    int hand = ViewHand();
    int item = Held(hand);
    if (g_view.editing && item < 0)
    {
        g_view.editing = false; // nothing to set (or a hand at work: set by its numbers)
        return;
    }
    g_nudging = false;
    Log("GRIPS: setting the grip of %s in the %s hand by hand: %s.", item >= 0 ? kSlotNames[item] : "nothing", hand == 0 ? "left" : "right",
        g_view.editing ? "on" : "off");
}

void GripsMenuHand()
{
    g_view.editing = false;
    g_nudging = false;
}

const char* GripsFooter()
{
    static char text[128];
    int hand = ViewHand();
    int item = ViewSlot(hand);
    const char* handName = hand == 0 ? "Left" : "Right";
    // At most about 790 pixels across: mind the longest item names.
    if (item < 0)
    {
        if (hand == 1 && CameraGunHeld())
            snprintf(text, sizeof(text), "Right hand: weapon (adjust its angle on the Hands tab).");
        else if (hand == 0 && CameraShotgunHeld())
            snprintf(text, sizeof(text), "Left hand: nothing held. Grip the shotgun's pump to adjust its grip.");
        else
            snprintf(text, sizeof(text), "%s hand: nothing held. Take out an item first.", handName);
    }
    else if (g_view.editing)
    {
        snprintf(text, sizeof(text), "Close the menu, then grip with the other hand to move the item.");
    }
    else if (!g_viewWasValid)
    {
        snprintf(text, sizeof(text), "%s hand: %s (locating...)", handName, kSlotTitles[item]);
    }
    else
    {
        const ItemGrip& g = g_grips[item][hand];
        snprintf(text, sizeof(text), "%s hand: %s (%s). Saved automatically.", handName, kSlotTitles[item],
                 (g.set || g.fingersSet) ? "custom grip" : "game default");
    }
    return text;
}
