#pragma once
#include <stddef.h>

// Tile-by-tile navigation of the battle cursor, from the numpad.
//
// The cursor is flown in gamepad mode and follows the mouse in mouse mode;
// neither steps.  So navigation keeps a *target tile* of its own and the hook
// on GetClosestValidCursorPosition puts that tile in front of the game each
// frame, in place of the tile under the mouse -- which means the game's own
// floor snapping, validation and path preview all run on it.  This file holds
// the part that does not touch the game: which key means which way, the
// target, and what to say.  main.c does the reading and the writing.
//
// Directions are fixed to the map, not the camera: north is +Y, east is +X.
// The game has no compass, so the choice is a convention -- picked so that
// both spoken numbers grow as the player goes north and east.

#define NAV_MAX_TEXT 128

typedef struct {
    int num_x, num_y;       // grid size in tiles, from XComWorldData
} NavGrid;

// A numpad digit (0-9) as a step: 8 north, 2 south, 6 east, 4 west, and the
// diagonals 7 9 1 3.  Returns 0 for a digit that is not a direction (0, 5).
int nav_step_for_digit(int digit, int* dx, int* dy);

// Starts navigation from a tile, or re-anchors it there.
void nav_begin(int tx, int ty);

// Whether a target is being held.  Off until a direction key is pressed, and
// off again after nav_end -- the mouse moved, or the soldier changed.
int nav_active(void);

void nav_end(void);

// Moves the target one step, clamped to the grid.  Returns 1 when the target
// moved, 0 when the edge stopped it.  `say` receives the announcement either
// way: the new position, or that the edge was reached.
int nav_move(const NavGrid* g, int dx, int dy, char* say, size_t say_sz);

// The target tile.  Returns 0 when navigation is not active.
int nav_target(int* tx, int* ty);

// "12, 40" -- a tile as it is spoken.
void nav_describe(int tx, int ty, char* out, size_t out_sz);

// ---- finding the floor under a target tile --------------------------------
//
// The cursor has to be put at the floor's height, or the game builds no path
// to it and a confirm does nothing. Nothing tells the mod that height, and the
// game's own floor search (GetFloorZForPosition) is unreliable from outside:
// it looks down only a limited way, returns the height it was *given* when it
// finds nothing, and in the logs never found a floor below -129 -- the lowest
// layer of the grid on that map -- from any start at all.
//
// So a tile's height is settled in phases, one frame at a time:
//
//   SEARCH   the floor search, from starts around the current ground; the
//            first start that finds a floor settles it.
//   PROBE    no start found one: place the cursor at candidate heights and let
//            the pathfinder judge. The first height the game builds a path to
//            is the floor. This is what makes the bottom layer reachable.
//   SETTLED  a height is known. A path that fails there, and keeps failing
//            for NAVH_SETTLE_MS, means the tile cannot be reached.
//   NONE     every probe failed: the tile cannot be reached.
//
// The ground carries over from tile to tile, so on level ground every step
// settles on its first frame.

typedef enum {
    NAVH_SEARCH,
    NAVH_PROBE,
    NAVH_SETTLED,
    NAVH_NONE,
} NavHeightPhase;

// What a path result means for the player.
typedef enum {
    NAVH_WAIT,          // nothing to say yet
    NAVH_REACHABLE,     // decided: the soldier can get there (say nothing)
    NAVH_NO_PATH,       // decided: say "No path"
} NavVerdict;

// The cursor stands this far above the position it is placed at: every
// placement logged showed getValidLocation adding exactly 64.
#define NAVH_LIFT 64.0f
#define NAVH_SETTLE_MS 300

// A floor the search finds further below the ground than this is only a
// fallback. The game's search misses a floor asked about from inside its own
// grid layer and answers with the next one down: on a roof at 466.2 the first
// start, 32 above, came back 212.6, the floor below (2026-09-22). So the
// search goes on, and the far find is taken only if nothing nearer turns up
// -- which is what a step off a roof needs.
#define NAVH_FAR_BELOW 64.0f

// Sets the ground estimate outright -- at the start of navigation.
void navh_set_ground(float ground);

// Starts on a new target tile, from the ground as it stands.
void navh_begin_tile(void);

// The height to put the pick at this frame.
float navh_query_z(void);

// The floor search ran from `asked` and answered `got`.
void navh_floor_result(float asked, float got);

// The game built (ok) or failed to build a path to the target tile, with the
// cursor at `dest_z`. Returns a verdict once one is reached, NAVH_WAIT
// otherwise; each tile gets one verdict at most.
NavVerdict navh_path_result(float dest_z, int ok, unsigned long long now_ms);

// Called every frame: returns NAVH_NO_PATH once a failure on a settled floor
// has gone uncontradicted for NAVH_SETTLE_MS.
NavVerdict navh_poll(unsigned long long now_ms);

NavHeightPhase navh_phase(void);
float navh_ground(void);
