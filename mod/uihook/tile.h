#pragma once
#include <stddef.h>

// What is on a tile: the part that does not touch the game.
//
// A sighted player learns a tile's worth from the shields drawn around the
// cursor -- a tall one for high cover, a short one for low, one per side --
// and from the path turning yellow when the move is a dash. Those shields are
// drawn by a native (XComActionIconManager.UpdateCursorLocation) from the same
// data the script reads with
//
//     native function bool GetCoverPoint(const Vector InPoint, out XComCoverPoint CoverPoint);
//
// so main.c asks that question of the game itself and this file turns the
// answer into words.
//
// ---- asking the game ------------------------------------------------------
//
// Every UXComWorldData exec thunk decodes its arguments off the script stack
// and then calls the C++ implementation through the object's vtable:
//
//     mov edx, [eax + SLOT]   ; eax = the object's vtable
//     ...
//     mov ecx, ebx            ; this
//     call edx
//
// The slot differs between builds (GetCoverPoint is +0x1F4 in EW, +0x1EC in
// EU; the smoke and poison tests are +0x178 / +0x174 in both), so it is read
// out of the thunk at startup rather than written down. The implementation is
// then called with the thunk's own convention: __thiscall, arguments pushed
// in declaration order, a Vector by value as three floats, a UBOOL back in
// eax, callee cleans the stack.

// XComCoverPoint as the script declares it, and as the thunk zeroes it:
// three ints, two Vectors, one int -- 40 bytes.
typedef struct {
    int   x, y, z;
    float tile_location[3];
    float cover_location[3];
    int   flags;
} TileCoverPoint;

// XComWorldData's cover flags. A direction bit means cover on that side; the
// matching low bit, that the cover there is low. COVER_Diagonal turns all four
// directions 45 degrees.
#define TILE_COVER_N        0x0001
#define TILE_COVER_S        0x0002
#define TILE_COVER_E        0x0004
#define TILE_COVER_W        0x0008
#define TILE_COVER_NLOW     0x0010
#define TILE_COVER_SLOW     0x0020
#define TILE_COVER_ELOW     0x0040
#define TILE_COVER_WLOW     0x0080
#define TILE_COVER_DIAGONAL 0x10000

#define TILE_MAX_TEXT 192

// The vtable offset an exec thunk dispatches through, or -1 when the code does
// not have the shape above. `code` is the thunk's first bytes; the scan stops
// at its `ret 8`.
int tile_vtable_slot(const unsigned char* code, size_t n);

// How many turns a path of `cost` takes: 1 while it is within `this_turn`
// (what is left to spend now), then one more for every full dash -- twice
// `standard` -- the rest needs. 0 when the numbers make no sense.
int tile_turns(int cost, int this_turn, int standard);

// What the player hears about a tile.
typedef struct {
    int dash;           // the move there is a dash
    int turns;          // past this turn's reach: how many turns it takes (2+)
    int cover_flags;    // 0 when GetCoverPoint found none
    int smoke;
    int poison;
} TileReport;

// "Dash. High cover north. Low cover east and west. Smoke." -- or "No cover."
// Directions are the mod's own, the ones the numpad moves in: north +Y, east
// +X. The game's own names for its cover bits are not: it calls +Y north and
// -X east, which is a real compass in Unreal's left-handed world, and the
// mirror image of the numpad's. So the game's East is spoken as west.
void tile_describe(const TileReport* r, char* out, size_t out_sz);

// "6 north, 3 east" -- an offset in tiles, in the numpad's directions (dx
// east, dy north). "here" for none.
void tile_offset_text(int dx, int dy, char* out, size_t out_sz);

// One unit a list mentions, relative to the soldier.
typedef struct {
    const char* name;
    int dx, dy;
} TileContact;

// "Chryssalid, 2 north, 5 east. Zombie, 8 south." -- nearest first. `none`
// is said when the list is empty.
void tile_contacts(const TileContact* c, int n, const char* none,
                   char* out, size_t out_sz);
