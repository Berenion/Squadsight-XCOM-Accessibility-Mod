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

#define TILE_MAX_TEXT 512

// The vtable offset an exec thunk dispatches through, or -1 when the code does
// not have the shape above. `code` is the thunk's first bytes; the scan stops
// at its `ret 8`.
int tile_vtable_slot(const unsigned char* code, size_t n);

// The address an exec thunk calls OUTRIGHT, or NULL when it makes no such
// call. `lo`/`hi` bound the game's image, and a target outside them is not
// one.
//
// A script function declared `final` cannot be overridden, so the compiler
// has no reason to make it virtual and does not: its thunk carries no vtable
// load at all and tile_vtable_slot can never find it. IsFlankingCoverPoint is
// one, and the first run for it reported "vtable slot NOT FOUND" for a slot
// that was never going to exist. The call is picked out the same way the
// virtual one is -- by the saved `this` going back into ecx just before it --
// and the last one before the `ret 8` is the implementation, the calls ahead
// of it being the argument decoding.
const unsigned char* tile_direct_target(const unsigned char* code, size_t n,
                                        const unsigned char* lo,
                                        const unsigned char* hi);

// How many turns a path of `cost` takes: 1 while it is within `this_turn`
// (what is left to spend now), then one more for every full dash -- twice
// `standard` -- the rest needs. 0 when the numbers make no sense.
int tile_turns(int cost, int this_turn, int standard);

// Whether a path that stops (dx, dy) tiles short of its tile -- 0, 0 when it
// stops on the right tile but the wrong storey -- stopped for want of a route
// rather than of moves. The pathfinder searches only as far as MaxPathCost
// (ComputePath2 with bObeyUnitMaxCost), and a path cut by that ends with its
// cost at the allowance: 23 and 24 of 24 in the 2026-09-29 (10:47) log. The
// 2026-10-01 (11:46) one: Hagen on 60, 15, every path to the roof above him
// ending under it at a cost of 5 or 7 of 24 -- moves to spare, and a tile
// cost at least one per step, so whatever was left would have reached it if
// any route did. TILE_NO_ROUTE_SLACK is room for diagonals and climbs.
// Only "within reach": a route longer than this turn's allowance is never
// searched, so it cannot be told from none.
#define TILE_NO_ROUTE_SLACK 4
int tile_no_route(int dx, int dy, int cost, int max);

// What the player hears about a tile.
//
// The three exposure fields go together and are only ever set as a group.
// `enemies_known` is how many enemies the answer was measured against, and is
// what tells "nobody can see this tile" apart from "nobody has been seen yet"
// -- two silences a player must never have to guess between. It is 0 when the
// squad has met nobody, or when the game could not be asked at all, and the
// readout then says nothing about exposure.
typedef struct {
    int dash;           // the move there is a dash
    int turns;          // past this turn's reach: how many turns it takes (2+)
    int no_route;       // no route there within reach (tile_no_route): said instead
    int cover_flags;    // 0 when GetCoverPoint found none
    int smoke;
    int poison;
    int enemies_known;  // enemies in sight the exposure was measured against
    int seen_by;        // how many of them can see this tile
    int flanked;        // and at least one of those gets past its cover
    // The seen enemies a soldier here would flank, named, repeats counted
    // (tile_names_counted): "Sectoid", "2 Sectoids, Muton". Empty for none.
    // The game's own mark -- see tile_exposure in main.c.
    char flanks[256];
    // The rings the game draws round units while a move is hovered
    // (XGUnit.DrawRanges) that this tile lies inside, as sentences:
    // "Medikit reaches White. Arc Thrower reaches Sectoid." Empty for none.
    // See tile_rings in main.c.
    char reach[384];
    // Seen enemies with a line to this tile that a soldier here would stand a
    // storey or more above (height advantage, +20 aim), and ones that would
    // stand as far above the soldier: "Sectoid", "2 Sectoids, Muton". See
    // tile_exposure in main.c.
    char height_over[256];
    char height_under[256];
} TileReport;

// "Dash. High cover north. Low cover east and west. Smoke." -- or "No cover."
// Directions are the mod's own, the ones the numpad moves in: north +Y, east
// +X. The game's own names for its cover bits are not: it calls +Y north and
// -X east, which is a real compass in Unreal's left-handed world, and the
// mirror image of the numpad's. So the game's East is spoken as west.
void tile_describe(const TileReport* r, char* out, size_t out_sz);

// ---- why a move is refused -----------------------------------------------
//
// "No path." was said for every tile the pathfinder refused, which is four
// different things to a player: a wall, a drop, a car roof, and a place that
// is fine but cannot be got to. The game keeps the difference in one flags
// word per tile, and three natives read it -- nothing else, no side effects,
// the same in EU and EW (checked in both exes):
//
//   IsPositionOnFloor                     flags & 0x2000             a floor
//   IsPositionOnFloorAndValidDestination  flags & 0x402000 == both   a floor a
//                                                                    move may end on
//   IsTileOccupied                        flags & 0x8000             solid stuff
//
// 0x400000 is the level designer's say: XComLevelActor and XComFracLevelActor
// carry `bIsValidDestination`, so a car roof can be walked over and never
// stopped on. The first two take a position and turn it into a tile
// themselves (GetTileCoordinatesFromPosition), so any height inside a layer
// asks about that layer; XComTacticalHUD.IsPositionInPathableTile is the
// game's own use of the second.
//
// A refused tile's floor height is not known -- that is why it was refused --
// so each layer the height probe tried is asked about, and the answer is the
// best thing any of them offers.
typedef struct {
    int floor;          // IsPositionOnFloor
    int destination;    // IsPositionOnFloorAndValidDestination
    int occupied;       // IsTileOccupied
    int below;          // under the ground's layer: solid there is not in the way
} TileLayerFlags;

typedef enum {
    TILE_REFUSE_NO_PATH,    // somewhere to stop is there: the route is what fails
    TILE_REFUSE_NO_STOP,    // a floor, but not one a move may end on
    TILE_REFUSE_BLOCKED,    // no floor, and solid stuff where one would stand
    TILE_REFUSE_NO_FLOOR,   // nothing to stand on at all
} TileRefusal;

TileRefusal tile_refusal(const TileLayerFlags* layers, int n);

// "No path." / "Cannot stop here." / "Blocked." / "No floor."
const char* tile_refusal_text(TileRefusal r);

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

// How far F / C moved the target, in the unit that decides height advantage:
// a storey, 192 (XGTacticalGameCoreNativeBase.RELATIVE_HEIGHT_BONUS_ZDIFF),
// to the nearest half. "Half a storey up.", "One storey down.", "A step up."
// for less than a quarter. The camera's floor number says nothing about cover
// or advantage, so it is not used.
void tile_height_step(float delta, char* out, size_t out_sz);

// The same inside a building, counted in its own floors rather than in
// height: "One floor up.", "2 floors down." -- a building's floors need not be
// 192 apart, and a tall room is one floor however high it is.
void tile_floor_step(int levels, char* out, size_t out_sz);

// Names as a list is spoken, in first-seen order with repeats counted:
// "2 Floaters, Sectoid". "Floater, Floater" read as two names, or as one said
// twice. `total` is how many there were, when only the first `n` were kept:
// the rest are ", and 3 more".
#define TILE_NAMES_MAX 16
void tile_names_counted(const char* const* names, int n, int total, char* out, size_t out_sz);

// Where a tile stands against the buildings, the way the game's own tracker
// (XComPawnIndoorOutdoorInfo.CheckForFloorVolumeEvents) works it out. The
// roof is the game's IsOnRoof: the top band of a building with more than one,
// so `storeys` counts the others. `building` only tells one building from the
// next; it is never followed.
typedef enum {
    TILE_WHERE_UNKNOWN = 0,     // nothing heard yet
    TILE_WHERE_OUTSIDE,
    TILE_WHERE_INSIDE,
    TILE_WHERE_ROOF
} TileWhereState;

typedef enum { TILE_BUILDING = 0, TILE_UFO, TILE_DROPSHIP } TileBuildingKind;

typedef struct {
    TileWhereState   state;
    int              floor;     // the game's FloorNumber, from 1; 0 when not inside
    int              storeys;   // the building's, roof left out; 0 when not known
    TileBuildingKind kind;
    const void*      building;
    int              no_above;  // the storey above has no floor on this tile
} TileWhere;

// The words for arriving at `now` from `before`, said on the crossing only:
// "Inside building, floor 2 of 3." on the way in, "Floor 3 of 3." up a floor
// of the same one, "On the roof.", "Outside." -- and "" when nothing changed.
// With nothing heard before, outside is what a player assumes and is not
// said. `force` says the state whatever it was (numpad 5).
void tile_where_text(const TileWhere* before, const TileWhere* now, int force,
                     char* out, size_t out_sz);
