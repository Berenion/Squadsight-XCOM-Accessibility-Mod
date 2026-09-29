#pragma once
#include <windows.h>
#include "units.h"

// The tile report: what a numpad step says about the tile it lands on -- who
// stands there, cover, smoke and poison, how far the move goes, who would see
// or be flanked from it, the ability rings, the evac zone. The game is asked
// here (report.c); the words are tile.c's, where the offline checks reach
// them.

// Describes tile (tx, ty) with its floor at `floor`. Returns 0 when the game
// could not be asked, leaving `say` empty. `with_dash` is off where the last
// path is not this tile's; `with_who` off where the units were said already.
int tile_report(int tx, int ty, float floor, int with_dash, int with_who,
                char* say, size_t say_sz);

// How far the path just built goes, against how far the soldier may go:
//   0 a standard move, 1 a dash, 2 past this turn's reach, -1 unreadable.
int tile_dash(int* cost_out, int* std_out, int* max_out, int* moves_out, int* turns_out);

// A pathing pawn's bOutOfRange: 1 when the path it last built stops short of
// its destination for want of allowance, 0 when not, -1 unreadable.
int path_out_of_range(void* ppawn);

// Whether the path just built ran out of a one-move allowance while a dash is
// still open this turn: its cost is not the tile's until the game builds it
// again at the dash allowance.
int tile_dash_pending(void);

// The pathing pawn that built the last path, set by hook_computepath.
extern void* g_path_pawn;

// ---- who is on the tile ----------------------------------------------------
// One unit in a tile's column, on any storey, and where its feet are
// (units_in_column).
typedef struct {
    char  label[160];
    float feet;
    int   mine;         // the soldier being moved
} ColumnUnit;
#define COLUMN_UNITS 8

// Everyone the squad may be told about in the column of (tx, ty), flagged or
// not. Returns how many.
int  units_in_column(int tx, int ty, ColumnUnit* out, int max);

// "Wright, Disco. Sectoid." -- those on (tx, ty); with a floor known, only
// those on that storey. *mine is set when one is the soldier being moved.
void units_on_tile(int tx, int ty, int have_floor, float floor,
                   char* out, size_t out_sz, int* mine);
