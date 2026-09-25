#pragma once
#include <stddef.h>

// The scanner: everything worth knowing about on the map, one item at a time.
//
// Ported from the Wasteland 2 accessibility mod, whose NavigationManager is
// the same idea -- a filtered, sorted list of what is around you, cycled from
// the keyboard, with the camera following the selection. The keys are the same
// ones, because a player who has both games should not have to learn two:
//
//   Page Down / Page Up          next / previous item
//   Ctrl + Page Down / Page Up   next / previous category
//   Alt  + Page Down / Page Up   next / previous storey
//   Home                         put the cursor on the selected item
//   Shift + Home                 put the cursor back on the soldier
//   End                          how far the selection is, and which way
//   Shift + End                  the same, back to the soldier
//
// Every offset the scanner speaks is measured from the tile being navigated
// to while there is one, and from the soldier otherwise -- so an offset is
// always the keys still to press. Step one north towards something two north
// and it becomes one north.
//
// Page Up and Page Down are unbound in [XComGame.XComTacticalInput] -- the
// global Camera bindings for them are removed with -Bindings -- so, like the
// numpad, they never become an InputEvent and nothing has to be swallowed.
// Home is bound, but it only raises InputEvent(570), which nothing in a
// mission handles. End is not so harmless: it is the secondary binding for
// Backspace_Key_Press, which is PerformEndTurn. main.c swallows that one.
//
// This file is the part that does not touch the game: the categories, the
// list, which item is selected, and what is said about it. main.c fills the
// list from the game each time the player asks.

#define SCAN_MAX        256     // items held in one scan
#define SCAN_NAME       64
#define SCAN_MAX_TEXT   192

typedef enum {
    SCAN_ALL,
    SCAN_SQUAD,
    SCAN_ENEMIES,
    SCAN_TARGETS,           // what the soldier can shoot, best shot first
    SCAN_EXPLOSIVES,        // cars, tanks, anything that blows up when destroyed
    SCAN_CIVILIANS,
    SCAN_DOORS,
    SCAN_OBJECTIVES,
    SCAN_MELD,              // EW's Meld canisters, one entry each
    SCAN_INTERACT,
    SCAN_CATEGORIES         // not a category: how many there are
} ScanCategory;

// One thing on the map. Tiles, not world units, because everything the player
// hears is in tiles and the cursor is placed in them.
//
// `tz` is a *floor*, not a row of the world grid. XComWorldData steps its Z
// axis by 64 units and a map 18 of those tall would otherwise be announced as
// having eighteen floors, on a building with two. main.c fills this in from
// XCom3DCursor.WorldZToCursorFloor, whose floor is 192 units -- three grid
// rows -- and which knows an indoor floor from the ground outside it.
typedef struct {
    char         name[SCAN_NAME];
    int          tx, ty, tz;        // tile x and y; tz is a floor, not a row
    float        world[3];          // where it actually is, for the cursor
    ScanCategory kind;              // the one category it belongs to
    // Said after the name: "45%, low cover, 8 of 8 HP". Kept apart from the
    // name because the name is what the selection is held by, and a hit
    // chance that moves with the soldier must not lose the selection.
    char         detail[96];
    // Sorts ahead of distance, higher first; 0 for everything sorted by
    // distance alone. The targets use it for best shot first.
    int          rank;
    // No tile to offer: a Meld canister nobody has seen yet, or one already
    // recovered or lost. Said as its name and detail alone, sorted after
    // everything that has a place, kept out of a storey filter, and Home
    // has nowhere to take the cursor.
    int          unplaced;
} ScanItem;

// "Everything", "Squad", "Enemies", ... -- as the category is spoken.
const char* scan_category_name(ScanCategory c);

// A static mesh's name as words for the scanner: "FlatBed" -> "Flat bed",
// "SedanA_Damaged" -> "Sedan", "ForkLift" -> "Fork lift". Everything from the
// first underscore goes, then trailing digits and a single trailing capital
// (the maps' variant letter), then the words are split where a capital
// follows a small letter. `fallback` when nothing is left.
void scan_mesh_words(const char* mesh, const char* fallback, char* out, size_t out_sz);

// ---- building a scan -------------------------------------------------------
//
// main.c calls scan_begin, then scan_add for everything it finds, then
// scan_end. The list is rebuilt on every key press rather than kept: units
// move, doors open, and a stale list would send the cursor somewhere empty.

void scan_begin(int from_tx, int from_ty, int from_tz);

// Adds one item. Ignored when the list is full, or when its category is not
// the one being shown. Returns 1 when it was kept.
int  scan_add(const ScanItem* item);

// Sorts nearest first -- by rank first, for items that carry one -- and restores the selection, by name and tile, to the
// item that was selected before -- so cycling does not jump because something
// moved a tile. Returns how many items the scan holds.
int  scan_end(void);

// ---- moving through it -----------------------------------------------------

// Steps the selection. `dir` is +1 or -1. Returns 0 when the list is empty.
int  scan_cycle(int dir);

// Steps the category, clearing the selection. Always succeeds.
void scan_cycle_category(int dir);

// Steps the storey filter: off, then each storey the map has, wrapping.
// `nz` is how many storeys there are. Returns the new filter, or
// SCAN_ALL_FLOORS.
#define SCAN_ALL_FLOORS (-1)
int  scan_cycle_floor(int dir, int nz);

ScanCategory scan_category(void);
int  scan_floor(void);
int  scan_count(void);

// Which item is selected, counting from 1 as it is spoken, or 0 for none.
int  scan_index(void);

// The selected item, or 0 when nothing is selected.
int  scan_selected(ScanItem* out);

// Drops the selection and the list -- the mission ended, or the scanner is
// being started fresh.
void scan_forget(void);

// ---- what is said ----------------------------------------------------------

// "Door, 4 north, 7 east, one floor up." -- or, with a detail, "Muton, 45%,
// low cover, 3 north." -- or, unplaced, "Meld canister, location unknown,
// 5 turns left." The offset is from where the scan
// began; the storey is left out when it is the same one.
void scan_describe(const ScanItem* item, int from_tx, int from_ty, int from_tz,
                   char* out, size_t out_sz);

// "Enemies, 3 found." -- what a category change says. With a storey filter on,
// "Enemies, floor 2, 1 found".
void scan_category_text(ScanCategory c, int floor, int count,
                        char* out, size_t out_sz);

// "Floor 3." / "All floors." -- what a storey change says.
void scan_floor_text(int floor, char* out, size_t out_sz);

// How many storeys a unit whose feet are at `feet` stands above (+) or below
// (-) a floor at `floor`. By height, 192 to a storey, rounded: within half a
// storey is the same floor. Pure arithmetic, because it is asked while a step
// is being spoken, which can be inside one of the game's own calls.
int scan_storey_diff(float feet, float floor);

// "Godongwana." on the same floor, "Godongwana, one floor up." otherwise.
void scan_unit_floor_text(const char* label, int dz, char* out, size_t out_sz);

// "Nothing here." -- said in place of an item when the list is empty. The
// category is named so the player knows which list was empty.
void scan_empty_text(ScanCategory c, char* out, size_t out_sz);
