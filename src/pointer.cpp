#include <math.h>

#include "pointer.h"

// See pointer.h. The game's colors: its amber for what is chosen (gHoverColor
// 1, 0.75, 0.25) and the pale of its lettering.
namespace
{

struct Rgb
{
    float r, g, b;
};

const Rgb kAmber = { 1.00f, 0.72f, 0.28f };
const Rgb kWarmWhite = { 1.00f, 0.94f, 0.80f };
const Rgb kPale = { 0.80f, 0.77f, 0.72f }; // (the dim ones: grayer)

const int kOrbSize = 64, kBeamWidth = 16, kBeamLength = 128, kCursorSize = 64;

unsigned char g_pictures[kPointerPictureCount][kBeamLength * kBeamWidth > kOrbSize * kOrbSize ? kBeamLength * kBeamWidth * 4 : kOrbSize * kOrbSize * 4];
bool g_made[kPointerPictureCount] = {};

float Clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

float Smooth(float from, float to, float x)
{
    float t = Clamp01((x - from) / (to - from));
    return t * t * (3.0f - 2.0f * t);
}

// One pixel: `core` of `inner` over `outer`, `solid` in all (0 to 1 each).
void Put(unsigned char* p, const Rgb& inner, const Rgb& outer, float core, float solid)
{
    solid = Clamp01(solid);
    core = Clamp01(core);
    float r = (inner.r * core + outer.r * (1.0f - core)) * solid;
    float g = (inner.g * core + outer.g * (1.0f - core)) * solid;
    float b = (inner.b * core + outer.b * (1.0f - core)) * solid;
    p[0] = (unsigned char)(r * 255.0f + 0.5f);
    p[1] = (unsigned char)(g * 255.0f + 0.5f);
    p[2] = (unsigned char)(b * 255.0f + 0.5f);
    p[3] = (unsigned char)(solid * 255.0f + 0.5f);
}

// `r`: from the middle (0) to the edge (1) of a round picture of `size` pixels.
float Radius(int x, int y, int size)
{
    float half = size * 0.5f;
    float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
    return sqrtf(dx * dx + dy * dy);
}

// The orb: a small bright core in a warm glow that fades to nothing at the edge.
void MakeOrb(unsigned char* pixels, bool dim)
{
    for (int y = 0; y < kOrbSize; ++y)
        for (int x = 0; x < kOrbSize; ++x)
        {
            float r = Radius(x, y, kOrbSize);
            float core = 1.0f - Smooth(0.09f, 0.22f, r);
            float glow = powf(1.0f - Smooth(0.0f, 1.0f, r), 2.6f) * 0.8f;
            float solid = core + glow * (1.0f - core);
            if (dim) solid *= 0.5f;
            Put(pixels + (y * kOrbSize + x) * 4, dim ? kPale : kWarmWhite, kAmber, core, solid);
        }
}

// The beam: soft at its sides, a fine bright middle; strong at the hand's end
// (the first row, coming in over the first few rows so it does not start with
// a hard edge) and fading towards the menu's.
void MakeBeam(unsigned char* pixels, bool dim)
{
    for (int y = 0; y < kBeamLength; ++y)
    {
        float v = (y + 0.5f) / kBeamLength;
        float along = Smooth(0.0f, 0.05f, v) * (0.85f + (0.12f - 0.85f) * powf(v, 0.8f));
        for (int x = 0; x < kBeamWidth; ++x)
        {
            float u = ((x + 0.5f) / kBeamWidth) * 2.0f - 1.0f;
            float across = expf(-u * u / (0.35f * 0.35f) * 3.0f);
            float middle = expf(-u * u / (0.15f * 0.15f) * 3.0f);
            float solid = across * along * (dim ? 0.45f : 1.0f);
            Put(pixels + (y * kBeamWidth + x) * 4, dim ? kPale : kWarmWhite, dim ? kPale : kAmber, middle, solid);
        }
    }
}

// The cursor: a soft dot, a thin amber ring round it, and a faint glow.
void MakeCursor(unsigned char* pixels, bool dim)
{
    for (int y = 0; y < kCursorSize; ++y)
        for (int x = 0; x < kCursorSize; ++x)
        {
            float r = Radius(x, y, kCursorSize);
            float dot = 1.0f - Smooth(0.18f, 0.30f, r);
            float ring = expf(-((r - 0.70f) / 0.07f) * ((r - 0.70f) / 0.07f)) * 0.85f;
            float glow = powf(1.0f - Smooth(0.0f, 1.0f, r), 3.0f) * 0.3f;
            float solid = dot + (ring + glow) * (1.0f - dot);
            if (dim) solid *= 0.6f;
            Put(pixels + (y * kCursorSize + x) * 4, dim ? kPale : kWarmWhite, dim ? kPale : kAmber, dot, solid);
        }
}

} // namespace

const unsigned char* PointerPicture(int kind, int& width, int& height)
{
    if (kind < 0 || kind >= kPointerPictureCount) kind = kPointerOrb;
    bool orb = kind == kPointerOrb || kind == kPointerOrbDim;
    bool beam = kind == kPointerBeam || kind == kPointerBeamDim;
    bool dim = kind == kPointerOrbDim || kind == kPointerBeamDim || kind == kPointerCursorDim;
    width = orb ? kOrbSize : (beam ? kBeamWidth : kCursorSize);
    height = orb ? kOrbSize : (beam ? kBeamLength : kCursorSize);
    unsigned char* pixels = g_pictures[kind];
    if (!g_made[kind])
    {
        if (orb) MakeOrb(pixels, dim);
        else if (beam) MakeBeam(pixels, dim);
        else MakeCursor(pixels, dim);
        g_made[kind] = true;
    }
    return pixels;
}
