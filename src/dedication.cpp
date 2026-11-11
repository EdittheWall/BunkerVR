#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

#include "dedication.h"
#include "log.h"

#pragma comment(lib, "gdi32.lib")

// The studio's card, shown in the headset at the very start of the game: a
// name in a high-contrast serif, full white, and under its right end the word
// STUDIO, small, thin, letter-spaced and grayer.
//
// The studio's name: change it here. DedicationMake draws any name, for a
// preview.
static const wchar_t* const kStudioName = L"Mina";
static const wchar_t* const kStudioWord = L"STUDIO";

// The card's mark: beside the name, the same word in the traditional
// Mongolian script (m, i, n, a), read top to bottom as that script is, as
// tall as the name and as gray as STUDIO. Fonts hold the script lying on its
// side (each word's stem horizontal, read left to right); it is drawn that
// way, joined by Windows' own shaping, and turned a quarter turn clockwise.
// It says Mina whatever name the preview is given.
static const wchar_t kStudioMark[] = { 0x182E, 0x1822, 0x1828, 0x1820, 0 }; // U+182E m, U+1822 i, U+1828 n, U+1820 a: as numbers, so the source stays plain ASCII for the compiler

// The picture is about as sharp as a headset can show it at its size there
// (vr.cpp shows it 0.6 m wide, 2 m away: some 22 of its pixels to a degree,
// where an Oculus Rift shows about 15), so the headset doesn't have to
// downsample it, which would make thin strokes flicker. The letters are drawn
// four times as big first and shrunk, which smooths their edges.
static const int kScale = 4;            // drawn this many times bigger, then shrunk
static const float kRowShare = 0.57f;   // how much of the picture's width the name and the mark beside it take (some 34 cm in the headset)
static const float kNameTallest = 0.5f; // ...the name at most this much of its height
static const float kMarkGap = 0.3f;     // the gap between the name and the mark, against the name's height
static const float kWordHeight = 0.2f;  // STUDIO's capitals, against the name's height
static const float kWordGap = 0.25f;    // the gap between the name and STUDIO, against the name's height
static const float kWordSpacing = 0.5f; // the space after each of STUDIO's letters, against their height
static const float kWordGrey = 0.7f;    // STUDIO this bright (the name at full)

static unsigned char g_picture[kDedicationWidth * kDedicationHeight * 4];     // the name
static unsigned char g_restPicture[kDedicationWidth * kDedicationHeight * 4]; // STUDIO and the mark
static bool g_made = false, g_madeWell = false;

// Converting between a color as stored in the picture (sRGB, as the headset
// takes it) and an amount of linear light.
static float ToLight(float stored)
{
    return stored <= 0.04045f ? stored / 12.92f : powf((stored + 0.055f) / 1.055f, 2.4f);
}
static float ToStored(float light)
{
    if (light <= 0.0f) return 0.0f;
    return light <= 0.0031308f ? light * 12.92f : 1.055f * powf(light, 1.0f / 2.4f) - 0.055f;
}

namespace
{
// Where letters drawn into the big picture really are (their ink, not their
// boxes), in its pixels; `any` false if nothing was drawn.
struct Ink
{
    int left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

// The big picture Windows draws into.
struct Canvas
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    unsigned char* bits = nullptr; // blue, green, red, unused
    int width = 0, height = 0;

    bool Make(int w, int h)
    {
        width = w;
        height = h;
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top row first
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* drawn = nullptr;
        dc = CreateCompatibleDC(nullptr);
        bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &drawn, nullptr, 0) : nullptr;
        if (dc == nullptr || bitmap == nullptr || drawn == nullptr) return false;
        bits = (unsigned char*)drawn;
        oldBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        Clear();
        return true;
    }
    ~Canvas()
    {
        if (dc && oldBitmap) SelectObject(dc, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    void Clear() { memset(bits, 0, (size_t)width * height * 4); }

    // `text` drawn with its box's top left at x, y, `spacing` pixels after
    // each letter, in a gray of `bright` (0 to 255).
    void Draw(const wchar_t* text, HFONT font, int spacing, int x, int y, int bright)
    {
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextCharacterExtra(dc, spacing);
        SetTextColor(dc, RGB(bright, bright, bright));
        TextOutW(dc, x, y, text, (int)wcslen(text));
        GdiFlush();
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, oldFont);
    }

    Ink InkNow() const
    {
        Ink ink;
        ink.left = width;
        ink.top = height;
        for (int y = 0; y < height; ++y)
        {
            const unsigned char* row = bits + (size_t)y * width * 4;
            for (int x = 0; x < width; ++x)
            {
                if ((row[x * 4 + 0] | row[x * 4 + 1] | row[x * 4 + 2]) == 0) continue;
                ink.any = true;
                if (x < ink.left) ink.left = x;
                if (x + 1 > ink.right) ink.right = x + 1;
                if (y < ink.top) ink.top = y;
                if (y + 1 > ink.bottom) ink.bottom = y + 1;
            }
        }
        if (!ink.any) ink = Ink();
        return ink;
    }

    // Where `text` is inked when drawn at x, y (the picture cleared first and after).
    Ink Measure(const wchar_t* text, HFONT font, int spacing, int x, int y)
    {
        Clear();
        Draw(text, font, spacing, x, y, 255);
        Ink ink = InkNow();
        Clear();
        return ink;
    }

    // What `from` has inside `ink`, turned a quarter clockwise (its top to the
    // right, its left to the top), put here with its top left at x, y; each
    // pixel the brighter of the two. Nothing outside this picture is touched.
    void CopyTurned(const Canvas& from, const Ink& ink, int x, int y)
    {
        for (int sy = ink.top; sy < ink.bottom; ++sy)
        {
            int tx = x + (ink.bottom - 1 - sy);
            if (tx < 0 || tx >= width) continue;
            const unsigned char* row = from.bits + (size_t)sy * from.width * 4;
            for (int sx = ink.left; sx < ink.right; ++sx)
            {
                int ty = y + (sx - ink.left);
                if (ty < 0 || ty >= height) continue;
                const unsigned char* source = row + (size_t)sx * 4;
                unsigned char* target = bits + ((size_t)ty * width + tx) * 4;
                for (int c = 0; c < 3; ++c)
                    if (source[c] > target[c]) target[c] = source[c];
            }
        }
    }
};

// The first of these faces this PC has, at `size` pixels tall (its letters'
// box), upright; null if none. `used` is its name.
HFONT FirstFace(HDC dc, const wchar_t* const* faces, int count, int size, int weight, const wchar_t*& used)
{
    for (int i = 0; i < count; ++i)
    {
        HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faces[i]);
        if (font == nullptr) continue;
        HGDIOBJ before = SelectObject(dc, font);
        wchar_t actual[LF_FACESIZE] = L"";
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, before);
        if (lstrcmpiW(actual, faces[i]) == 0)
        {
            used = faces[i];
            return font;
        }
        DeleteObject(font);
    }
    used = L"(none)";
    return nullptr;
}
} // namespace

// The name: Bodoni MT (it comes with Microsoft Office: thick and hairline
// strokes), or else Modern No. 20 (Office too), Palatino Linotype or Times New
// Roman (they come with Windows), all with their figures standing on the line.
// STUDIO: Segoe UI Semilight (Windows: thin, but a step stronger than Segoe UI
// Light, whose strokes come out a single pixel wide in the headset's picture
// and flicker), or else Century Gothic (Office) or Arial.
// The mark: Mongolian Baiti (it comes with Windows); without it, no mark.
static const wchar_t* const kNameFaces[] = { L"Bodoni MT", L"Modern No. 20", L"Palatino Linotype", L"Times New Roman" };
static const wchar_t* const kWordFaces[] = { L"Segoe UI Semilight", L"Century Gothic", L"Arial" };
static const wchar_t* const kMarkFaces[] = { L"Mongolian Baiti" };

// The big picture shrunk into `out` (width * height, kScale times smaller):
// how much of each pixel the letters cover (0 to 1, the grays counting as
// less), the average over the big picture's pixels in it. Pure white, with no
// glow. The colors are stored premultiplied by alpha, worked out in linear
// light, so the edges are as bright as the share of the pixel the letters
// cover.
static void ShrinkInto(const Canvas& canvas, unsigned char* out, int width, int height)
{
    const float white = ToLight(1.0f);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int sum = 0;
            for (int sy = 0; sy < kScale; ++sy)
            {
                const unsigned char* row = canvas.bits + ((size_t)(y * kScale + sy) * canvas.width + (size_t)x * kScale) * 4;
                for (int sx = 0; sx < kScale; ++sx)
                {
                    unsigned char b = row[sx * 4 + 0], g = row[sx * 4 + 1], r = row[sx * 4 + 2];
                    sum += r > g ? (r > b ? r : b) : (g > b ? g : b);
                }
            }
            float alpha = (float)sum / (255.0f * kScale * kScale);
            unsigned char* pixel = out + ((size_t)y * width + x) * 4;
            unsigned char colour = (unsigned char)(ToStored(white * alpha) * 255.0f + 0.5f);
            pixel[0] = pixel[1] = pixel[2] = colour;
            pixel[3] = (unsigned char)(alpha * 255.0f + 0.5f);
        }
    }
}

bool DedicationMake(const wchar_t* name, unsigned char* picture, int width, int height, unsigned char* rest)
{
    memset(picture, 0, (size_t)width * height * 4);
    if (rest) memset(rest, 0, (size_t)width * height * 4);
    if (name == nullptr || name[0] == 0 || width <= 0 || height <= 0) return false;
    const int bigWidth = width * kScale, bigHeight = height * kScale;
    Canvas canvas;
    if (!canvas.Make(bigWidth, bigHeight))
    {
        Log("VR: the studio's card could not be made (no drawing surface).");
        return false;
    }

    // The name and the mark: measured at one size, then sized so the two side
    // by side take their share of the picture's width (smaller if the name
    // would be too tall), and measured again. The mark is measured as fonts
    // hold it, lying down: its width is its height once turned.
    const wchar_t* nameFace = L"(none)";
    const wchar_t* wordFace = L"(none)";
    const wchar_t* markFace = L"(none)";
    const int measureSize = bigHeight / 3;
    const int margin = bigHeight / 20;
    HFONT font = FirstFace(canvas.dc, kNameFaces, 4, measureSize, FW_NORMAL, nameFace);
    if (font == nullptr)
    {
        Log("VR: the studio's card could not be made (none of its fonts).");
        return false;
    }
    Ink first = canvas.Measure(name, font, 0, margin, margin);
    DeleteObject(font);
    if (!first.any)
    {
        Log("VR: the studio's card could not be made (its name drew nothing).");
        return false;
    }
    Canvas side; // the mark is drawn here lying down, then turned into the card
    bool haveSide = side.Make(bigWidth, bigHeight);
    Ink markFirst;
    HFONT markFont = haveSide ? FirstFace(side.dc, kMarkFaces, 1, measureSize, FW_NORMAL, markFace) : nullptr;
    if (markFont)
    {
        markFirst = side.Measure(kStudioMark, markFont, 0, margin, margin);
        DeleteObject(markFont);
        markFont = nullptr;
    }
    // The mark's width at the name's measured height (lying down, its width is that height).
    float markWideFirst = markFirst.any ? (float)markFirst.Height() * (float)first.Height() / (float)markFirst.Width() : 0.0f;
    float rowFirst = (float)first.Width() + (markFirst.any ? kMarkGap * (float)first.Height() + markWideFirst : 0.0f);
    float grow = kRowShare * (float)bigWidth / rowFirst;
    float tallest = kNameTallest * (float)bigHeight / (float)first.Height();
    if (grow > tallest) grow = tallest;
    int nameSize = (int)((float)measureSize * grow);
    HFONT nameFont = FirstFace(canvas.dc, kNameFaces, 4, nameSize, FW_NORMAL, nameFace);
    Ink nameInk = nameFont ? canvas.Measure(name, nameFont, 0, margin, margin) : Ink();
    if (!nameInk.any)
    {
        if (nameFont) DeleteObject(nameFont);
        Log("VR: the studio's card could not be made (its name at size %d drew nothing).", nameSize);
        return false;
    }

    // STUDIO: its capitals a share of the name's height, spaced out.
    int wordSize = 0, spacing = 0;
    HFONT wordFont = FirstFace(canvas.dc, kWordFaces, 3, measureSize, FW_NORMAL, wordFace);
    Ink wordInk;
    if (wordFont)
    {
        Ink measured = canvas.Measure(kStudioWord, wordFont, 0, margin, margin);
        DeleteObject(wordFont);
        wordFont = nullptr;
        if (measured.any)
        {
            float wanted = kWordHeight * (float)nameInk.Height();
            wordSize = (int)((float)measureSize * wanted / (float)measured.Height() + 0.5f);
            spacing = (int)(kWordSpacing * wanted + 0.5f);
            wordFont = FirstFace(canvas.dc, kWordFaces, 3, wordSize, FW_NORMAL, wordFace);
            if (wordFont) wordInk = canvas.Measure(kStudioWord, wordFont, spacing, margin, margin);
        }
    }

    // The mark at the name's height (lying down: as wide as the name is tall).
    int markSize = 0;
    Ink markInk;
    if (markFirst.any)
    {
        markSize = (int)((float)measureSize * (float)nameInk.Height() / (float)markFirst.Width() + 0.5f);
        markFont = FirstFace(side.dc, kMarkFaces, 1, markSize, FW_NORMAL, markFace);
        if (markFont) markInk = side.Measure(kStudioMark, markFont, 0, margin, margin);
    }

    // Laid out: the name and the mark side by side across the middle, their
    // feet on one line; STUDIO under the name, ending where the name ends; it
    // all in the middle from top to bottom.
    const int bright = (int)(255.0f * kWordGrey + 0.5f);
    int gap = wordInk.any ? (int)(kWordGap * (float)nameInk.Height() + 0.5f) : 0;
    int block = nameInk.Height() + (wordInk.any ? gap + wordInk.Height() : 0);
    int top = (bigHeight - block) / 2;
    int markGap = markInk.any ? (int)(kMarkGap * (float)nameInk.Height() + 0.5f) : 0;
    int markWide = markInk.any ? markInk.Height() : 0, markTall = markInk.any ? markInk.Width() : 0;
    int row = nameInk.Width() + markGap + markWide;
    int nameLeft = (bigWidth - row) / 2;
    canvas.Draw(name, nameFont, 0, margin + nameLeft - nameInk.left, margin + top - nameInk.top, 255);
    if (rest)
    {
        // Two layers: the name alone in `picture`, the rest in `rest`.
        ShrinkInto(canvas, picture, width, height);
        canvas.Clear();
    }
    if (wordInk.any)
    {
        int wordLeft = nameLeft + nameInk.Width() - wordInk.Width();
        int wordTop = top + nameInk.Height() + gap;
        canvas.Draw(kStudioWord, wordFont, spacing, margin + wordLeft - wordInk.left, margin + wordTop - wordInk.top, bright);
    }
    if (markInk.any)
    {
        // Drawn lying down where it was measured, then turned into place.
        side.Clear();
        side.Draw(kStudioMark, markFont, 0, margin, margin, bright);
        canvas.CopyTurned(side, markInk, nameLeft + nameInk.Width() + markGap, top + nameInk.Height() - markTall);
    }
    DeleteObject(nameFont);
    if (wordFont) DeleteObject(wordFont);
    if (markFont) DeleteObject(markFont);
    ShrinkInto(canvas, rest ? rest : picture, width, height);

    char nameText[64] = "", nameFaceText[64] = "", wordFaceText[64] = "", markFaceText[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, name, -1, nameText, sizeof(nameText), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, nameFace, -1, nameFaceText, sizeof(nameFaceText), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, wordFace, -1, wordFaceText, sizeof(wordFaceText), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, markFace, -1, markFaceText, sizeof(markFaceText), nullptr, nullptr);
    Log("VR: the studio's card is made (%d x %d): \"%s\" in %s at %d, STUDIO in %s at %d spaced %d, the Mongolian mark %s (%s at %d, %d x %d) (drawn %d times bigger and shrunk).",
        width, height, nameText, nameFaceText, nameSize / kScale, wordFaceText, wordSize / kScale, spacing / kScale,
        markInk.any ? "beside it" : "left out", markFaceText, markSize / kScale, markWide / kScale, markTall / kScale, kScale);
    return true;
}

const wchar_t* DedicationName()
{
    return kStudioName;
}

#ifdef BUNKERVR_PLAYER_BUILD
// Player builds carry the card already drawn (assets/studio_card.bin, resource 201 of this DLL): its name's font,
// Bodoni MT, comes with Microsoft Office, not with Windows, and on a PC without it the card would come out in
// another font. False if the resource isn't there.
static bool LoadDrawnCard()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadDrawnCard, &self))
        return false;
    HRSRC found = FindResourceW(self, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10)); // 10: RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(self, found) : nullptr;
    const unsigned char* bytes = loaded ? (const unsigned char*)LockResource(loaded) : nullptr;
    const size_t layer = (size_t)kDedicationWidth * kDedicationHeight * 4;
    if (!bytes || SizeofResource(self, found) != 16 + 2 * layer || memcmp(bytes, "BVRCARD1", 8) != 0) return false;
    unsigned int size[2];
    memcpy(size, bytes + 8, sizeof(size));
    if (size[0] != (unsigned int)kDedicationWidth || size[1] != (unsigned int)kDedicationHeight) return false;
    memcpy(g_picture, bytes + 16, layer);
    memcpy(g_restPicture, bytes + 16 + layer, layer);
    return true;
}
#endif

bool DedicationLayers(int& width, int& height, const unsigned char*& nameLayer, const unsigned char*& restLayer)
{
    width = kDedicationWidth;
    height = kDedicationHeight;
    nameLayer = g_picture;
    restLayer = g_restPicture;
    if (!g_made)
    {
        g_made = true;
#ifdef BUNKERVR_PLAYER_BUILD
        if (LoadDrawnCard())
        {
            Log("VR: the studio's card as drawn when this copy was made (%d x %d; its fonts need not be on this PC).", kDedicationWidth, kDedicationHeight);
            g_madeWell = true;
            return true;
        }
        Log("VR: the studio's card is not inside this copy: drawn here, with this PC's fonts.");
#endif
        g_madeWell = DedicationMake(kStudioName, g_picture, kDedicationWidth, kDedicationHeight, g_restPicture);
    }
    return g_madeWell;
}
