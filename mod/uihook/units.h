#pragma once
#include <windows.h>
#include <stdint.h>
#include "game.h"
#include "cursor.h"

// Who is where on the tactical map, and whom the squad may be told about.
//
// The table is built from the overhead flags (UIUnitFlag.SetNames, one entry
// per flag) and read from the game when asked. units.c has the reasoning;
// this is what the readouts use.

// ---- the table -------------------------------------------------------------
#define UNIT_MAX 64

// The flag also draws a cover shield and hit points, which is what the shot
// readout says about a target; they are kept as the flag last drew them.
// UIUnitFlag.OnInit sends SetHitPoints before SetNames, so an entry can exist
// for a moment with no name, and unit_seen passes over it until it has one.
typedef struct {
    void* flag;
    char  name[64];
    char  nick[64];
    char  cover[16];    // RealizeCover's shield: "_highCover" ... "" unknown
    int   flanked;      // the shield's flanked state, -1 unknown
    int   hp, hp_max;   // as displayed, -1 when the flag shows none
    int   strip_flanked; // the target strip's mark: flanked by the soldier, -1 unknown
    int   moves;        // RealizeMoves: action pips, friendly units only; -1 unknown
    int   buff, debuff; // ShowBuff / ShowDebuff: the flag's markers; -1 unknown
    int   panicked;     // RealizeEKG: 1, 0, -1 unknown
    int   wounded;      // RealizeCriticallyWounded: SOLDIER_WOUND_* (soldier.h)
    int   bleed_turns;  // turns left while bleeding out
    int   number;       // the mod's "Sectoid 2" (known_number); 0 for none
} UnitName;

// Walked by index; an entry whose flag is NULL is an emptied slot.
extern UnitName g_units[UNIT_MAX];
extern int      g_nunits;

// The entry for a flag, made if there is none (reusing an emptied slot).
// NULL for no flag, or when the table is full.
UnitName* unit_entry(void* flag);

// A flag's names, from UIUnitFlag.SetNames.
void unit_note(void* flag, const char* name, const char* nick);

// Empties a flag's slot, logging whose it was.
void unit_forget(UnitName* u);

// The flag whose unit is `unit`, or NULL. Compares pointers only.
UnitName* unit_by_unit(const void* unit);

// "Surname, Nickname", or the name alone; an enemy the squad has seen with
// its number, "Sectoid 2".
void unit_label(const UnitName* u, char* out, size_t out_sz);

// The label with what the screen shows: "Sectoid, 3 of 4 HP, on overwatch".
// Overwatch only when `enemy`.
void unit_label_state(const UnitName* u, void* unit, int enemy,
                      char* out, size_t out_sz);

// ---- asking the game about a unit ------------------------------------------
#define TEAM_NEUTRAL 1          // Object.ETeam.eTeam_Neutral -- a civilian

// A flag's unit, alive and in sight (IsAliveAndVisible), with its pawn,
// where the pawn stands (its middle, NAVH_LIFT above the feet), and
// whether it is on the squad's side.
typedef struct {
    const UnitName* who;
    void*  unit;
    void*  pawn;
    float  loc[3];
    int    friendly;
} UnitSeen;

int   unit_seen(UnitName* u, void* squad, UnitSeen* out);
void* unit_player(void* unit);          // XGUnit.m_kPlayer
void* squad_player(void);               // the player of the soldier being moved
void* unit_pawn(void* unit);            // XGUnit.m_kPawn, not checked for life
int   unit_team(void* unit);            // m_eTeam; 0 when unreadable
int   unit_overwatch(void* unit);       // IsInOverwatch, for a unit unit_seen vouched for

// Whether the flag's unit is gone: flag reused, unit freed, or no longer
// alive and visible.
int   unit_gone(const UnitName* u, void* flag);

// Shared with readers of the same fields elsewhere: a pawn's Location, and a
// unit's m_arrVisibleEnemies.
extern FieldSlot g_pawn_loc, g_visen;

// ---- what the squad sees ---------------------------------------------------
//
// Whether a readout that names units may name this one. Kept in one place
// because it was kept in three -- the scanner, who is on a tile, and the blast
// list -- and a survivor the squad could see was missed by each in turn, and
// had to be fixed in each in turn (2026-09-27).
//
//   - The squad's own: always.
//   - A civilian (the neutral team): civilian_seen -- someone's
//     m_arrVisibleCivilians, or a soldier's line to them.
//   - Anyone else: the union of the squad's m_arrVisibleEnemies, as the
//     radar and the targeting draw them. IsAliveAndVisible alone let
//     unrevealed pods through.
//
// The readouts about threats -- the radar, the alien heartbeats, "Seen by"
// and flanking, enemies coming into sight -- use `enemies` alone, on
// purpose: a civilian is no threat.
#define SEEN_MAX 128

typedef struct {
    void* unit[SEEN_MAX];
    int   n;
} SeenSet;

typedef struct {
    void*   squad;
    SeenSet enemies;
    SeenSet civilians;
} SquadSight;

void squad_sight(void* squad, SeenSet* set);            // m_arrVisibleEnemies, squad-wide
void squad_sight_civilians(void* squad, SeenSet* set);  // m_arrVisibleCivilians, squad-wide
int  seen_has(const SeenSet* set, const void* unit);
int  civilian_seen(void* squad, const SeenSet* civilians, void* unit,
                   const float* loc, const char* name);

// Taken once per readout, then asked per unit.
void squad_sight_take(void* squad, SquadSight* v);
int  squad_sees(const SquadSight* v, void* unit, const float* loc,
                int friendly, const char* name);

// ---- enemies the squad has seen: numbers and last known places --------------
//
// Two Sectoids used to be "Sectoid" and "Sectoid" everywhere, and an enemy
// that left sight left every list with it. Both are kept here, per enemy
// unit, for the mission (HANDOFF "Next", items 5 and 6):
//
//   - A number per kind, in the order the squad FIRST SEES each one -- never
//     the game's own order or count, which would say that an unseen third
//     exists. Given where every readout asks what the squad sees
//     (squad_sight), so the scanner, the target list, a sighting and the
//     tile all name the same alien the same way. Kept after death.
//   - Where it was last seen: the pawn's position at the last sight poll
//     that saw it, frozen once it is out of sight and never updated while
//     unseen, with the squad's turn (XGPlayer.m_iTurn) when sight was lost.
//     Dropped from the list when it is seen again or dies.
//
// Only a human player's sight counts (XGPlayer, XGPlayer_MP): in the aliens'
// turn the cursor's player is theirs, and their "enemies" are the squad. A
// different human player -- a new mission, a load -- starts again from 1.
#define KNOWN_MAX 128

// Forgets them all, as sight_reset does: a new squad, or the old one gone
// (a load frees it, and the next may be made at the same address).
void known_reset(void);

// Called from sight_poll with who the squad sees now and where (the pawns'
// Locations), so the places follow what was seen.
void known_seen(void* squad, void* const* units, const float (*locs)[3], int n);

typedef struct {
    char  label[80];            // "Sectoid 2"
    float loc[3];               // the pawn's Location when last seen
    int   turns_ago;            // squad turns since sight was lost; -1 unknown
} KnownLost;

// The enemies out of sight now whose last place is known, alive as far as
// the game says, and not in `now` (the sight a readout just took).
int known_lost(void* squad, const SeenSet* now, KnownLost* out, int max);

// ---- the soldier being moved -----------------------------------------------
void* soldier_unit(void);               // the cursor's chained unit, if live
// The soldier's tile, and the pawn's height (NAVH_LIFT above the feet).
int   soldier_tile(const CursorGrid* g, int* tx, int* ty, float* z);
int   soldier_out_of_moves(void);       // XGUnit.m_iMoves <= 0
int   soldier_aiming(void);             // m_kCurrAction is XGAction_Targeting / _Fire

// ---- units with no flag ----------------------------------------------------
// A civilian with no flag over them (flagless_units), whom the unit
// table, built from the flags, never holds.
typedef struct {
    void* unit;
    void* pawn;
    float loc[3];
    char  name[64];
} FlaglessUnit;
#define COLUMN_FLAGLESS 32

// Whether `unit` (an XGUnit from the object walk) is a living neutral with no
// flag, and if so, who and where. Sight is not asked.
int flagless_unit(void* unit, FlaglessUnit* out);
