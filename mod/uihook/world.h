#pragma once
#include <windows.h>
#include "cursor.h"
#include "scan.h"
#include "units.h"

// The level actors: doors, windows, panels, ladders, Meld canisters, the
// radar array, what explodes, and units with no flag. None of them pass
// through the UI and no native lists them, so they come from the game's
// object table -- walked in full once per map, then only caught up on
// (world.c has the reasons and the costs).

// Brings the actors and their items up to date for the map on grid `g`.
// 0 when there is nothing to ask.
int world_refresh(const CursorGrid* g);

// The items the last world_refresh placed: the scanner's entries for them,
// with tile and floor on that grid.
const ScanItem* world_items(int* n);

// The door actors within a tile of (tx, ty) on storey `tz` (floor_of), as
// of the last world_refresh; how many.
int world_doors_near(int tx, int ty, int tz, void** out, int max);

// Whether a door is shut: 1 shut, 0 open (or broken), -1 unknown. `how`, if
// given, gets what decided it, for the log ("state _Inactive").
int world_door_shut(void* door, char* how, size_t how_sz);

// Whether `actor` is a destructible that blows up (as of the last refresh).
int world_explodes(const void* actor);

// Civilians with no flag over them: living neutral units from the walk.
// `refresh` brings the walk up to date first; otherwise it is walked only if
// it never has been.
int flagless_units(int refresh, FlaglessUnit* out, int max);

// The special-mission HUD's arrow panel (UISpecialMissionHUD_Arrows), handed
// over by main.c from its SetArrow calls: the scanner reads the actors the
// arrows point at as objectives.
void world_arrows_note(void* panel);
