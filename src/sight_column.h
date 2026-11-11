#pragma once

// The part of the player that the game's creatures look at, moved to the real
// head's height (used by sight.cpp). Only arithmetic, so that it can be
// unit-tested on its own.
//
// What the engine does: to see whether a creature sees the player it takes a
// column over the player's body, given as its center and its full size: 20 cm
// wide and deep, from 10 cm above the feet up to the game's eye (the body's top
// less 10 cm, the game's base camera offset: 1.85 m standing, 1.05 m
// crouched). It looks from the creature's eyes at up to 12 points on it: across
// it, and up it at -0.4, 0, +0.2 and +0.4 of its height from its center, so
// from 0.1 to 0.9 of the way up. Two points in clear view and the player is
// seen. GetPlayerHeadPos, the player's head for the creatures' other checks,
// is the same eye.
//
// Moved: the column's bottom stays (the feet), and its top moves by as much as
// the real head is above (+) or below (-) the game's eye, so that the column
// again runs from the feet to the eye, the real one. Never shorter than
// kSightShortestColumn (unless the game's own was).

#include <math.h>

// The shortest the moved column may be (game meters): a player lying flat is
// still a little of a target.
static const float kSightShortestColumn = 0.3f;
// The most the real head is taken to be below or above the game's eye (game
// meters; the body script keeps it nearer, these only guard against nonsense).
static const float kSightMostDown = 1.8f;
static const float kSightMostUp = 1.2f;
// The highest point the engine looks at, as a share of the column's height
// from its bottom (for the log).
static const float kSightHighestPoint = 0.9f;

// The real head's rise over the game's eye as it is used: within the limits
// above, and 0 for anything that is not a number.
inline float SightUsableRise(float up)
{
    if (!(up == up) || fabsf(up) > 1000.0f) return 0.0f; // (not a number, or no sensible number)
    if (up < -kSightMostDown) return -kSightMostDown;
    if (up > kSightMostUp) return kSightMostUp;
    return up;
}

// The column (its center and its full size, x y z) moved for a real head
// `up` game meters above the game's eye (below when negative): the bottom
// kept, the top moved by `up`. x and z, and the width and depth, as they were.
inline void SightMoveColumn(const float centre[3], const float size[3], float up, float outCentre[3], float outSize[3])
{
    const float bottom = centre[1] - size[1] * 0.5f;
    float height = size[1] + SightUsableRise(up);
    const float shortest = size[1] < kSightShortestColumn ? size[1] : kSightShortestColumn;
    if (!(height >= shortest)) height = shortest; // (also catches a height that is not a number)
    outCentre[0] = centre[0];
    outCentre[1] = bottom + height * 0.5f;
    outCentre[2] = centre[2];
    outSize[0] = size[0];
    outSize[1] = height;
    outSize[2] = size[2];
}

// The top of a column (the eye it was made for), and the highest point of it
// the engine looks at (heights above the world's zero).
inline float SightColumnTop(const float centre[3], const float size[3])
{
    return centre[1] + size[1] * 0.5f;
}
inline float SightHighestLookedAt(const float centre[3], const float size[3])
{
    return centre[1] - size[1] * 0.5f + size[1] * kSightHighestPoint;
}
