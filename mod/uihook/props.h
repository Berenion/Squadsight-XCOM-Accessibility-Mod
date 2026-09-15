#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>

// What kind of value a UProperty holds.
//
// The frame walk needs this because a plain `int` parameter is otherwise
// invisible: read_fstring() rejects it, read_array() rejects it, and nothing
// else looked at it.  So every setter that carries its index as an ordinary
// integer appeared to carry no index at all --
//
//     AS_SetCheckboxLabel(int Index, string strText)        (EW)
//     AS_SetCheckboxValue(int Index, bool bChecked)         (EW)
//     AS_SetCurrentDifficultyMarker(int Index)              (EU)
//     SetListSelection(int Index, int iSelection)           (both)
//
// -- and was filed by arrival order, or treated as an announcement that reset
// the screen's label list.  Strings and arrays reach the payload through their
// own readers, so only the scalars are named here.
typedef enum {
    PROP_UNKNOWN = 0,   // string, array, object, struct -- handled elsewhere
    PROP_INT,
    PROP_BOOL,
    PROP_FLOAT,
    PROP_BYTE
} PropKind;

// Locates UObject::Class by probing a function's own field chain: the right
// offset is the one where every child resolves to a name ending "Property".
// Self-validating in the same way as the GNames scan, so nothing is assumed.
// `node` is any UStruct with children.  `why` receives a readable result
// either way.  Safe to call repeatedly; only the first success counts.
int props_init(const void* node, char* why, size_t why_sz);
int props_ready(void);

// Classifies one UProperty.  After the first sighting of each type this is a
// pointer comparison against a short table, because it runs on the game's UI
// thread for every property of every UI call.
PropKind props_kind(const void* prop);

// Reads a BoolProperty out of `base` (the owner's storage).  UnrealScript
// packs bools into a bitfield, so the property carries a mask alongside its
// offset; the mask's location is probed the same way the class offset is.
// Returns 0 if the value could not be read.
int props_read_bool(const void* prop, const uint8_t* base, int* out);

// The two predicates a wrong offset has to get past. They are the whole of
// the probe's judgement, and they are pure, so they live here where the
// offline tests can reach them without a live game.

// A bitfield mask selects exactly one bit. Zero means nothing was found;
// several bits mean the offset is pointing at something that is not a mask.
static __inline int is_single_bit(uint32_t m)
{
    return m != 0 && (m & (m - 1)) == 0;
}

// Every UProperty subclass is named "<something>Property". A bare "Property"
// does not count -- there is no such class, so matching it would mean the
// offset found a string that merely ends the right way.
static __inline int ends_with_property(const char* s)
{
    size_t n = s ? strlen(s) : 0;
    return n > 8 && strcmp(s + n - 8, "Property") == 0;
}

// Implemented in main.c; shared so the probe can check a pointer before
// following it.
int readable(const void* p, size_t n);
