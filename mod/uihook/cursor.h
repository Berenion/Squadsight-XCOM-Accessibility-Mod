#pragma once
#include <windows.h>
#include <stdint.h>

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
// Returns 1 when the cursor's position can be read.
int cursor_fields(char* why, size_t why_sz);

// Reads the cursor's position.  Returns 0 when the fields are not resolved or
// the memory will not read.
int cursor_position(float* x, float* y, float* z);

// One tile, in world units: XComWorldData.WORLD_StepSize.
#define CURSOR_TILE 96.0f

// Forgets the cursor -- leaving a mission destroys it, and the next mission
// spawns another.
void cursor_forget(void);
