#pragma once
#include <windows.h>

// Where a tile is: "Inside building, floor 2 of 3.", "On the roof.",
// "Outside.", the evac zone, and where a storey F / C passed over can be
// reached. Asked of the map's floor and building volumes (where.c has the
// rule, which is the game's CheckForFloorVolumeEvents); the words are tile.c's.

// The words for arriving at (tx, ty) with its floor at `floor`, said only on
// a crossing -- or always, with `force`. Empty when nothing changed or the
// game could not be asked.
void where_say(int tx, int ty, float floor, int force, char* out, size_t out_sz);

// A new navigation starts from nothing heard.
void where_forget(void);

// How many of a building's floors lie between heights `from` and `to` on
// (tx, ty), in *dz; 0 when the two are not in the same building.
int  where_levels_between(int tx, int ty, float from, float to, int* dz);

// After F / C inside a building: "Floor 2 of 3 does not reach this tile;
// nearest 3 north, 2 east." when the next storey that way (`dir` +1 / -1) was
// passed over or not found. Empty otherwise.
void floor_missed(int tx, int ty, float from, int found, float to, int dir,
                  char* out, size_t out_sz);

// Whether (tx, ty) at `floor` is in the evac zone, while an open objective
// mentions evac.
int  evac_at(int tx, int ty, float floor);

// The evac zone's tile nearest (ox, oy) with a floor, and that floor.
int  evac_nearest(int ox, int oy, int* nx, int* ny, float* nz);

// The exact floor inside the 64-unit layer from `bottom`, pos[0..1] its
// place (pos[2] is overwritten); `bottom` when there is none.
float aim_floor_exact(void* world, float* pos, float bottom);
