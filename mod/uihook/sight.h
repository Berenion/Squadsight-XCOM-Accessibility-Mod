#pragma once
#include <stddef.h>

// Enemies coming into and going out of the squad's sight.
//
// A sighted player sees a pod revealed -- the camera pans to it, its icons
// join the enemy strip -- and sees an enemy drop out of view. The game marks
// the moment too: XGSightManager.AddVisibleEnemy runs as a soldier gains one
// in m_arrVisibleEnemies, and the first time the squad sees it
// (XGPlayer.HasSeenEnemy) XGUnit.OnSeeEnemy plays the "enemy spotted" voice.
// None of that says what, how many or where. This keeps the squad's sight,
// as main.c reads it on each poll, and says what changed:
//
//   "Sighted: Sectoid, 5 north, 3 east. Sectoid, 6 north, 3 east."
//   "In sight again: Muton, 2 north."
//   "Out of sight: Muton."
//
// A change is said only once it has held for SIGHT_SETTLE_MS, so an enemy
// flickering at the edge of view as a soldier steps is not read twice.

#define SIGHT_MAX        64
#define SIGHT_LABEL      160
#define SIGHT_SETTLE_MS  500
#define SIGHT_TEXT       2048

typedef struct {
    void* unit;                 // the XGUnit; identity only, never followed here
    char  label[SIGHT_LABEL];   // "Sectoid", "Thin Man"
    int   dx, dy;               // tiles from the soldier, east and north
    int   has_pos;              // dx, dy known
} SightUnit;

typedef enum {
    SIGHT_NEW,      // never seen before this mission
    SIGHT_AGAIN,    // seen before, out of sight since, back now
    SIGHT_GONE,     // was in sight, is not now
} SightKind;

typedef struct {
    SightUnit u;
    SightKind kind;
} SightEvent;

// Forgets everything: a new mission, a load.
void sight_reset(void);

// One poll. `cur` is who the squad can see now. Returns the events that have
// settled, at most `max` of them, into `ev`. A GONE event keeps the label the
// unit last had, since by then it may not be readable.
int sight_step(unsigned long long now_ms, const SightUnit* cur, int n,
               SightEvent* ev, int max);

// The events, as one announcement, in the order sighted, in sight again, out
// of sight. `out` is empty when there are none.
void sight_text(const SightEvent* ev, int n, char* out, size_t out_sz);
