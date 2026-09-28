#pragma once
#include <windows.h>
#include <stdint.h>
#include <math.h>

// The battle cursor: finding it, and reading where it stands.
//
// This is the first piece of the mod that is not narration.  Everything until
// now has read text on its way out or relabelled a command on its way in; the
// grid cannot be navigated that way, because the cursor is not a widget with
// a selection to move.  XCom3DCursor extends XComPawn and the controller
// *flies* it, with acceleration and momentum (PlayerInput.aForward /
// aStrafe), so there is no "next tile" command to rename.
//
// What the game does give is a grid underneath: XComWorldData's tile maths is
// all native, and WORLD_StepSize is 96 world units, so one tile is a known
// distance along a world axis.  Moving by a tile therefore means placing the
// cursor, and placing it means writing to the object -- which is why this
// starts by *reading*: identify the cursor, resolve its fields, and report
// where it is, with no writes at all, so that the mechanism can be confirmed
// against a real mission before anything acts on it.
//
// Finding the cursor costs nothing: several of its functions are native
// (AXCom3DCursor::GetCursorMode among them) and a native's `self` is the
// object.  Hooking one hands the cursor over the first time it runs.
//
// Its position is found the way this mod finds everything -- by name.
// Actor declares
//
//     var(Movement) databinding const Vector Location;
//
// so the offset comes from walking the class chain for a UProperty called
// "Location", exactly as props.c walks a function's children.  Nothing is
// assumed about the layout of an Actor.

// Records the cursor object.  Called from the hook on one of its natives, so
// this runs on the game thread, often; it does no work after the first call
// beyond a pointer comparison.
void cursor_seen(void* self);

// The cursor object, or NULL if no mission has been entered.
void* cursor_object(void);

// Resolves the named fields against the cursor's class, once.  `why` receives
// a readable account either way -- what was found and where, or what failed.
// Returns 2 on the call that resolved them, so the offsets it chose can be put
// on record once, 1 on every later call, and 0 when the position cannot be
// read.
int cursor_fields(char* why, size_t why_sz);

// Whether cursor_fields has succeeded for the current cursor.  A plain check,
// for code that runs every frame and must not consume the call that reports.
int cursor_resolved(void);

// Reads the cursor's position.  Returns 0 when the fields are not resolved or
// the memory will not read.
int cursor_position(float* x, float* y, float* z);

// The offset of a property called `name` on any object's class, searched up
// the class chain.  Returns 0 when the object is unreadable or has no such
// property -- which also makes it a test of what kind of object this is.
int object_field_offset(const void* obj, const char* name, uint32_t* out);

// The same, and the class in the chain that declares the field (*owner):
// every class below that one has the field at the same offset.
int object_field_owner(const void* obj, const char* name, uint32_t* out, const void** owner);

// Whether class `cls` is `base` or below it: a climb up SuperStruct comparing
// pointers, no names read. 0 before the SuperStruct offset is known.
int class_derives(const void* cls, const void* base);

// The UProperty itself, for a field that needs its type to be read -- a bool
// is one bit of a dword (props_read_bool). NULL when there is no such field.
const void* object_field_prop(const void* obj, const char* name);

// Whether an object's class is `name`, or anything below it -- the class chain
// walked the same way object_field_offset walks it. The scanner asks this of
// every object in the game's table, so it is a name comparison per class in
// the chain and nothing more.
int object_is_a(const void* obj, const char* name);

// The name of an object's own class, for the log and for naming a thing the
// scanner has no better word for. Returns 0 when it cannot be read.
int object_class_name(const void* obj, char* out, size_t out_sz);

// Where UStruct::SuperStruct was found, or 0 before any class chain has been
// walked. Exposed because objects.c matches classes by pointer rather than by
// name, and needs to climb the chain itself -- once per object in the game's
// whole table, where a name comparison would be far too slow.
uint32_t object_super_offset(void);

// The soldier the cursor is leashed to (XCom3DCursor.ChainedPawn), which
// changes when the player switches soldier.  Returns 0 when it could not be
// resolved or read; *out may be NULL on success.
int cursor_chained_pawn(void** out);

// One tile, in world units: XComWorldData.WORLD_StepSize.
#define CURSOR_TILE 96.0f

// The grid's origin.
//
// A tile is not the position divided by 96.  The native that answers
// GetTileCoordinatesFromPosition does, in both builds,
//
//     TileX = appFloor((Position.X - Min.X) * (1/96))
//     TileY = appFloor((Position.Y - Min.Y) * (1/96))
//     TileZ = appFloor((Position.Z - Min.Z) * (1/64))    // WORLD_FloorHeight
//
// with appFloor spelled cvtss2si(2f - 0.5) >> 1 (EW .text 0x5d0730, the same
// bytes in EU).  So the origin is the world's own bounds, and the rounding is
// a floor, not the truncation a C cast gives -- which differs for every
// negative coordinate.  The origin lives on XComWorldData as
//
//     var Box WorldBounds;      // { Vector Min; Vector Max; byte IsValid; }
//     var int NumX, NumY, NumZ;
//
// and that object is handed over by the static native GetWorldData, whose
// return value is the object: the hook reads it out of Result.

// Records the world data object.  Called from the hook on GetWorldData, which
// half the tactical script calls, so after the first sighting this is one
// comparison.
void cursor_world_seen(void* world);

// Resolves WorldBounds / NumX / NumY / NumZ on the world object's class, once
// per object.  Same contract as cursor_fields: 2 on the call that resolved
// them, 1 afterwards, 0 on failure, with `why` saying which.
int cursor_world_fields(char* why, size_t why_sz);

typedef struct {
    float min_x, min_y, min_z;
    int   num_x, num_y, num_z;
} CursorGrid;

// Reads the grid as it stands now.  The values are read on every call rather
// than cached, because the world data is rebuilt when a map loads.
int cursor_grid(CursorGrid* g);

// The world data object, once its class has been confirmed as XComWorldData;
// NULL before then. tile queries call its natives through it.
void* cursor_world(void);

// The tile the cursor stands on, and its height in *z (may be NULL).
int cursor_tile(const CursorGrid* g, int* tx, int* ty, float* z);

// One axis of the native's arithmetic.  `step` is CURSOR_TILE for X and Y.
static __inline int cursor_tile_axis(float pos, float min, float step)
{
    return (int)floorf((pos - min) / step);
}

// The grid in named pieces (refactor step 6), so that the two ways of
// counting height are not mixed at a call site:
//   - a *layer* is one row of tiles, WORLD_FloorHeight (64) deep from Min.Z,
//     as the tile natives count them. Something standing on a floor is in
//     the layer 4 above that floor, as XGAction_EndMove asks
//     (grid_floor_layer); a pawn's Location is NAVH_LIFT above its feet.
//   - a *storey* is the cursor's floor, three layers (192) deep, asked of
//     the game through floor_of (where.h). Never worked out from a layer.
// X and Y are tiles, CURSOR_TILE wide, counted from Min.X / Min.Y.
#define CURSOR_LAYER       64.0f
#define CURSOR_ABOVE_FLOOR 4.0f

static __inline int grid_x(const CursorGrid* g, float x)
{
    return cursor_tile_axis(x, g->min_x, CURSOR_TILE);
}

static __inline int grid_y(const CursorGrid* g, float y)
{
    return cursor_tile_axis(y, g->min_y, CURSOR_TILE);
}

// The middle of a tile, in world units.
static __inline float grid_centre_x(const CursorGrid* g, int tx)
{
    return g->min_x + ((float)tx + 0.5f) * CURSOR_TILE;
}

static __inline float grid_centre_y(const CursorGrid* g, int ty)
{
    return g->min_y + ((float)ty + 0.5f) * CURSOR_TILE;
}

// The layer a height is in.
static __inline int grid_layer(const CursorGrid* g, float z)
{
    return cursor_tile_axis(z, g->min_z, CURSOR_LAYER);
}

// A layer's lowest height, and its middle, in world units.
static __inline float grid_layer_bottom(const CursorGrid* g, int tz)
{
    return g->min_z + (float)tz * CURSOR_LAYER;
}

static __inline float grid_layer_middle(const CursorGrid* g, int tz)
{
    return g->min_z + ((float)tz + 0.5f) * CURSOR_LAYER;
}

// The layer of whatever stands on a floor at height `floor`.
static __inline int grid_floor_layer(const CursorGrid* g, float floor)
{
    return grid_layer(g, floor + CURSOR_ABOVE_FLOOR);
}

// Forgets the cursor -- leaving a mission destroys it, and the next mission
// spawns another.
void cursor_forget(void);
