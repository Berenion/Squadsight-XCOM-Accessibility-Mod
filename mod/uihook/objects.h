#pragma once
#include <windows.h>
#include <stdint.h>

// Walking the game's own object table.
//
// The scanner needs things that never pass through the UI and are not units:
// doors, ladders, the Meld canisters, the radar array. None of them announce
// themselves, and there is no native that lists them -- GetInteractionPoints
// comes closest but covers only XComInteractiveLevelActor, which a ladder is
// not.
//
// UE3 keeps every live UObject in one global array, UObject::GObjObjects, and
// that array is found the same self-validating way names.c finds GNames: scan
// .data for a TArray of pointers, and accept the one where each entry's own
// UObject::Index equals its position in the array. That is not a heuristic --
// the engine maintains exactly that invariant, and it is what lets an object
// be freed by index -- so nothing else in .data can satisfy it by accident.
// The probe settles the Index offset at the same time, for the same reason
// names.c settles FNameEntry's text offset while it looks for "None".
//
// The array has holes: a freed object leaves a null. Walks skip them.
//
// ---- why this is fast enough to sit on a key press ------------------------
//
// A mission's table holds well over a hundred thousand entries, and the
// obvious walk -- readable() then object_is_a() on each -- costs a
// VirtualQuery and a run of string compares per object. That is tenths of a
// second on the game's own thread, which is a stutter the player would feel on
// every Page Down.
//
// So two things are different here. Reads go through a small cache of accepted
// memory regions, because a hundred thousand objects come out of a handful of
// heap regions and the second object in a region needs no syscall at all. And
// a class is matched by *pointer*: the UClass is found once by name, and the
// test is then a walk up SuperStruct comparing pointers, with no names read.

// Probes for the table. Called at startup, and again -- through
// objects_retry -- whenever the scanner wants it and has not got it.
//
// The first run of this probe reported "not found", and the reason is most
// likely that it only ever ran once, at injection: at that point the game is
// still in its shell and the table can be far smaller than the floor the probe
// insists on. The array's header never moves, so finding it late is as good as
// finding it early -- there is just no reason to give up after one look.
int objects_init(HMODULE mod, char* why, size_t why_sz);
int objects_ready(void);

// Tries again if the table is not known yet, at most once every few seconds so
// a failing probe cannot become a per-key-press scan of .data. Returns 1 once
// the table is known. `why` receives the probe's account on the call that
// changes the answer, and is left empty otherwise.
int objects_retry(char* why, size_t why_sz);

// The UClass objects called `names[0..n-1]`, written to `out`, found in one
// pass and kept. An entry is NULL when there is no such class -- which, for a
// class the game declares, is also how "no mission has loaded it yet" looks.
//
// Several at once on purpose: the pass is the cost, not the comparison, and
// resolving three classes one at a time would walk the whole table three
// times. The names are turned into FName indices first (names_find), so the
// walk compares integers rather than decoding a string per object.
void objects_classes(const char* const* names, const void** out, int n);

// One class, through the same cache.
const void* objects_class(const char* name);

// Called once per matching object; `which` is its index in the class list the
// walk was given, and `idx` its slot in the object table -- which is what
// objects_still needs to check on it later. Returning 0 stops the walk.
typedef int (*ObjectVisitFn)(void* obj, int which, int idx, void* ctx);

// Visits every live object whose class is one of `classes[0..n-1]`, or below
// one of them, in table order. Returns how many were visited, or -1 when the
// table is not known.
//
// Several classes in one walk on purpose. The table is 175,000 entries and a
// walk of it costs tens of milliseconds; doing that once per class froze the
// game for a quarter of a second on every scanner key press. A class that
// matches more than one entry goes to the first.
//
// Class default objects are skipped. They derive from the class like anything
// else, and their Location is the origin -- which put "Radar array" on tile
// 65, -12, off the side of the map, and sent the cursor there.
int objects_each(const void* const* classes, int n, ObjectVisitFn fn, void* ctx);

// The same walk over [from, end of table), with *next left at the end it
// reached -- to be handed back as `from` next time.
//
// Even at its cheapest a full walk is tens of milliseconds, and the game
// thread is stopped for all of it: one run measured 78 ms over 175,328
// entries, five dropped frames on a key press. But a mission's table only
// grows at the end, so after the first walk there is nothing to look at but
// what has been added since, which is usually nothing at all.
//
// What this does *not* see is an object placed into a slot freed earlier in
// the mission, below `from`. The classes the scanner walks for -- doors,
// ladders, Meld canisters -- are placed when the map is built and not spawned
// afterwards, so that gap costs nothing in practice; objects that come and go
// are projectiles and effects, which are nobody's business here. Objects that
// *leave* are the real case, and objects_still is how they are noticed.
int objects_each_from(const void* const* classes, int n, int from, int* next,
                      ObjectVisitFn fn, void* ctx);

// Whether `obj` is still a live object in the table at all, asked without
// knowing its index: the object is asked where it thinks it sits and the
// table is asked to agree.
//
// For a pointer that came from somewhere other than a walk -- a unit out of
// another unit's m_arrVisibleEnemies, say -- and is about to be handed to one
// of the game's own natives. A freed object's slot is nulled, so this catches
// it; a slot already handed to something else it does not, which is what the
// caller's own test of the thing (IsAliveAndVisible for a unit) is for.
int objects_live(void* obj);

// Whether `obj` is still the live object at table slot `idx`, and still of
// one of `classes[0..n-1]` or below one.
//
// The pointer alone would not do. A door blown off its hinges is freed, its
// slot is nulled, and the engine then hands that slot to the next object that
// wants one -- so a kept pointer can come back matching an object that was
// never a door. Asking the table settles both at once.
int objects_still(void* obj, int idx, const void* const* classes, int n);

// How many entries the table holds, live and dead -- for the log.
int objects_count(void);

// How long the last walk took, in milliseconds, and how many entries it
// looked at. For the log: if the scanner ever does feel slow, this says so.
void objects_last_walk(unsigned* ms, int* entries);
