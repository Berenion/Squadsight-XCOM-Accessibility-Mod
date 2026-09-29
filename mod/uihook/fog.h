#pragma once
#include "cursor.h"

// The fog of war, per tile: whether the squad has ever seen a tile. XCOM
// paints a tile nobody has seen solid black (XComFOWEffect.FogColor), so a
// level actor on one is not on the screen at all. fog.c has where the answer
// comes from and how far it is trusted.

#define FOG_UNKNOWN (-1)    // no buffer, or no soldier to settle what it means
#define FOG_NEVER     0     // never seen: black on the screen
#define FOG_SEEN      1     // seen now, or seen before and greyed

// Settles what the buffer's bytes mean from where the squad stands: a
// soldier's own tile is always in sight. `tiles` holds `n` soldiers' tiles
// (x, y, layer) on grid `g`. Called before asking, on every refresh; logs
// only when the verdict changes.
void fog_calibrate(const CursorGrid* g, const int (*tiles)[3], const char* const* names, int n);

// The fog on tile (x, y, layer) of grid `g`.
int fog_tile(const CursorGrid* g, int x, int y, int z);
