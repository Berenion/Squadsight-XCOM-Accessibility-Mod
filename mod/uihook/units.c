// Who is where, and whom the squad may be told about: see units.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "units.h"
#include "log.h"
#include "cursor.h"
#include "nav.h"
#include "combat.h"
#include "names.h"
#include "ue3.h"
#include "props.h"

// Every unit has a flag over its head, and UIUnitFlag.SetNames(unitName,
// unitNickName) arrives through the text hooks once per flag: a soldier's
// surname and nickname, an alien's or civilian's name. The flag also holds
// its unit (UIUnitFlag.m_kUnit, an XGUnit), whose m_kPawn has the Location.
// So the table is kept by flag object, as focus.c keeps its lists, and the
// position is read when it is asked for -- units move, flags do not change.
//
// Only what a sighted player could see is ever said. The flag hides itself
// unless m_kUnit.IsVisible(), and the native IsAliveAndVisible is that test
// with the dead left out, asked of the unit through its vtable like the tile
// queries. A unit it cannot be asked about counts as unseen.

UnitName         g_units[UNIT_MAX];
int              g_nunits;
FieldSlot        g_pawn_loc;
static FieldSlot g_flag_unit, g_unit_pawn;

// A flag that has stopped being one. Its slot is left empty rather than
// closed up, because everything that walks this table walks it by index and
// an empty slot is skipped for nothing -- field_ptr answers a null object
// without reading anything.
void unit_forget(UnitName* u)
{
    logf_("units: the flag for %s is gone -- dropped\n",
          u->name[0] ? u->name : "someone");
    u->flag = NULL;
    u->name[0] = 0;
    u->nick[0] = 0;
}

// The entry for a flag, made if there is none.
UnitName* unit_entry(void* flag)
{
    if (!flag) return NULL;
    int i, free_slot = -1;
    for (i = 0; i < g_nunits && g_units[i].flag != flag; i++)
        if (!g_units[i].flag && free_slot < 0) free_slot = i;
    if (i < g_nunits) return &g_units[i];
    // A mission's worth of flags is dropped as its units die, so the emptied
    // slots are where the next mission's go. Without this a long session
    // would fill the table with the dead and stop noticing the living.
    if (free_slot >= 0) i = free_slot;
    else if (g_nunits < UNIT_MAX) g_nunits++;
    else return NULL;
    UnitName* u = &g_units[i];
    memset(u, 0, sizeof *u);
    u->flag = flag;
    u->flanked = -1;
    u->strip_flanked = -1;
    u->moves = -1;
    u->buff = u->debuff = -1;
    u->hp = u->hp_max = -1;
    u->panicked = -1;
    return u;
}

void unit_note(void* flag, const char* name, const char* nick)
{
    UnitName* u = unit_entry(flag);
    if (!u) return;
    strncpy_s(u->name, sizeof u->name, name, _TRUNCATE);
    strncpy_s(u->nick, sizeof u->nick, nick, _TRUNCATE);
}

// The player a unit belongs to (XGUnit.m_kPlayer).
static FieldSlot g_player;

void* unit_player(void* unit)
{
    const void* v;
    if (!field_ptr(unit, "m_kPlayer", &g_player, sizeof(void*), &v))
        return NULL;
    return *(void* const*)v;
}

// The player the soldier being moved belongs to: ChainedPawn.m_kGameUnit.
static FieldSlot g_squad_unit;

void* squad_player(void)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn ||
        !field_ptr(pawn, "m_kGameUnit", &g_squad_unit, sizeof(void*), &v))
        return NULL;
    return unit_player(*(void* const*)v);
}

// A flag's unit, if it is alive and in sight: its pawn, where it stands, and
// whether it is on the side of the soldier being moved.
//
// The side is the unit's player, compared with the soldier's. The flag's own
// m_bIsFriendly was the first try and put Chryssalids in the squad: this
// session never found UBoolProperty::BitMask ("no BitMask -- bools read as a
// whole dword"), and that bool shares its dword with m_bIsDead, m_bIsSelected
// and the rest, so any of them set read as friendly.
int unit_seen(UnitName* u, void* squad, UnitSeen* out)
{
    const void* v;
    if (!u->flag || !u->name[0]) return 0;
    if (!field_ptr(u->flag, "m_kUnit", &g_flag_unit, sizeof(void*), &v)) {
        // A flag whose class has no m_kUnit is not a flag any more. Flags
        // are destroyed with their units -- eleven Chryssalids and zombies
        // died over one mission, and loading a save replaced the squad's four
        // as well -- and the engine hands the memory straight on, so what is
        // left behind reads as an AudioComponent, or as a class pointer that
        // is not readable at all. The table held sixteen of them, and every
        // pass over it paid a failed class-chain walk for each. Dropping the
        // entry is the answer; the question does not get better with age.
        unit_forget(u);
        return 0;
    }
    void* unit = *(void* const*)v;

    // â›” A live flag is not a live unit, and this is a CALL into the game.
    // tile_vfn only proves the vtable entry points into the image, which a
    // RECYCLED object's does perfectly well -- so without this the mod can
    // call a real function of the wrong class on a wrong `this`. The
    // 2026-09-21 logs show it twice, as "tile: units faulted" one step after
    // a flag was dropped, with the game gone shortly after both times.
    // objects_live asks the object table instead of trusting the pointer.
    if (!unit_is_live(unit)) return 0;

    UnitTestFn visible = (UnitTestFn)tile_vfn(unit, g_unit_slot_visible);
    if (!visible || !visible(unit, NULL)) return 0;
    if (!field_ptr(unit, "m_kPawn", &g_unit_pawn, sizeof(void*), &v))
        return 0;
    void* pawn = *(void* const*)v;
    if (!unit_is_live(pawn)) return 0;
    if (!field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v))
        return 0;
    out->who = u;
    out->unit = unit;
    out->pawn = pawn;
    memcpy(out->loc, v, 3 * sizeof(float));
    out->friendly = squad && unit_player(unit) == squad;
    return 1;
}

// What the squad can see: every enemy in any living squad member's
// XGUnitNativeBase.m_arrVisibleEnemies.
//
// IsAliveAndVisible alone let unrevealed pods through -- the radar listed
// Chryssalids 29 tiles north that no one had met. The game's own minimap
// draws enemies from the active soldier's m_arrVisibleEnemies
// (UITacticalHUD_Radar.UpdateBlips), and targeting from the squad's; the
// union across the squad is what a sighted player could have on screen.
FieldSlot        g_visen;
static FieldSlot g_viciv;

static void squad_sight_of(void* squad, SeenSet* set, const char* field, FieldSlot* slot)
{
    set->n = 0;
    if (!squad) return;
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || !s.friendly) continue;
        const void* v;
        if (!field_ptr(s.unit, field, slot, sizeof(FArray), &v))
            continue;
        const FArray* a = (const FArray*)v;
        if (a->Num <= 0 || a->Num > SEEN_MAX ||
            !readable(a->Data, (size_t)a->Num * sizeof(void*)))
            continue;
        void* const* e = (void* const*)a->Data;
        for (int k = 0; k < a->Num; k++) {
            int j;
            for (j = 0; j < set->n && set->unit[j] != e[k]; j++) {}
            if (j == set->n && set->n < SEEN_MAX) set->unit[set->n++] = e[k];
        }
    }
}

static void known_number(void* squad, const SeenSet* set);

void squad_sight(void* squad, SeenSet* set)
{
    squad_sight_of(squad, set, "m_arrVisibleEnemies", &g_visen);
    known_number(squad, set);
}

// The civilians the squad can see, the same way: each squad member's
// m_arrVisibleCivilians. IsAliveAndVisible is not the player's sight for a
// civilian either -- the 2026-09-27 run's scanner gave "Survivor, 2 south,
// 43 west" before anyone had seen them. The sight manager's
// AddVisibleCivilian / RemoveVisibleCivilian events keep this array per
// viewer, as AddVisibleEnemy does m_arrVisibleEnemies.
void squad_sight_civilians(void* squad, SeenSet* set)
{
    squad_sight_of(squad, set, "m_arrVisibleCivilians", &g_viciv);
}

int seen_has(const SeenSet* set, const void* unit)
{
    for (int j = 0; j < set->n; j++)
        if (set->unit[j] == unit) return 1;
    return 0;
}

// Whether the squad sees this civilian: in someone's m_arrVisibleCivilians,
// or failing that, a living soldier with a line to the civilian's tile.
//
// The array alone missed a mission survivor. The 2026-09-27 (23:25) log has
// "Locate any survivors" complete and then "Civilians, 0 found" twice while
// the squad stood beside them. A survivor is not a civilian to the game: its
// behavior is XGAIBehavior_Survivor, not XGAIBehavior_Civilian, and
// XGAIPlayer_Animal keeps survivors in m_arrSurvivor and rescues them by
// distance, never through anyone's sight arrays. The line test is
// XComPresentationLayer.CanSquadSee's -- each living soldier's pawn to the
// place -- through CanSeeActorToTile, the native "Seen by" already trusts.
int civilian_seen(void* squad, const SeenSet* civilians, void* unit,
                  const float* loc, const char* name)
{
    if (seen_has(civilians, unit)) return 1;
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !squad || !cursor_grid(&g)) return 0;
    SeeTileFn see = (SeeTileFn)tile_vfn(world, g_world_slot_seetile);
    if (!see) return 0;
    // The tile the civilian stands in, as tile_report works one out: feet
    // plus 4, in 64-unit layers from Min.Z.
    int tx = grid_x(&g, loc[0]);
    int ty = grid_y(&g, loc[1]);
    int tz = grid_floor_layer(&g, loc[2] - NAVH_LIFT);
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || !s.friendly) continue;
        if (see(world, NULL, s.pawn, tx, ty, tz, 0)) {
            static void* told;
            if (told != unit) {
                told = unit;
                logf_("scan: %s seen by line from %s, not in anyone's "
                      "m_arrVisibleCivilians\n", name, s.who->name);
            }
            return 1;
        }
    }
    return 0;
}

// A unit's team, from XGUnitNativeBase.m_eTeam (Object.ETeam, one byte).
static FieldSlot g_team;

int unit_team(void* unit)
{
    const void* v;
    if (!field_ptr(unit, "m_eTeam", &g_team, 1, &v)) return 0;
    return *(const uint8_t*)v;
}

void* unit_pawn(void* unit)
{
    const void* v;
    if (!field_ptr(unit, "m_kPawn", &g_unit_pawn, sizeof(void*), &v)) return NULL;
    return *(void* const*)v;
}

// The flag whose unit is `unit`, or NULL. Pointers are compared and nothing
// is called, so a unit that has since gone costs a failed match, not a fault.
UnitName* unit_by_unit(const void* unit)
{
    for (int i = 0; i < g_nunits; i++) {
        UnitName* u = &g_units[i];
        const void* v;
        if (!u->flag || !u->name[0]) continue;
        if (!field_ptr(u->flag, "m_kUnit", &g_flag_unit, sizeof(void*), &v)) {
            unit_forget(u);     // see unit_seen: not a flag any more
            continue;
        }
        if (*(void* const*)v == unit) return u;
    }
    return NULL;
}

// Whether a unit the game was showing is gone: its flag has been destroyed or
// reused, the unit object is no longer live, or the game no longer counts it
// alive and visible (IsAliveAndVisible, asked through its vtable as unit_seen
// does).
int unit_gone(const UnitName* u, void* flag)
{
    const void* v;
    if (u->flag != flag || !flag || !unit_is_live(flag)) return 1;
    if (!field_ptr(flag, "m_kUnit", &g_flag_unit, sizeof(void*), &v)) return 1;
    void* unit = *(void* const*)v;
    if (!unit || !unit_is_live(unit)) return 1;
    UnitTestFn visible = (UnitTestFn)tile_vfn(unit, g_unit_slot_visible);
    return visible && !visible(unit, NULL);
}

// ---- enemies the squad has seen: numbers and last known places --------------
//
// See units.h. Keyed by the XGUnit, which is only ever compared here -- the
// entry outlives the unit's flag, and a dead unit's object may be gone.
//
// A load keeps the table (known_reset). The 2026-09-29 (12:53) log reloaded
// mid-mission and had "known: 4 enemies forgotten": every place the squad had
// seen an alien went with it, although a sighted player reloading still
// remembers them. A load makes every unit again, so an entry loses its
// XGUnit then and is matched to the new one by the object's name: the
// checkpoint records each actor's ActorName (Checkpoint.ActorRecord) and
// makes it again under that name. The kind must match too. Until matched,
// an entry is still listed where it was last seen -- nothing the squad saw
// is forgotten -- unless it was known dead.

typedef struct {
    void* unit;                 // NULL after a load, until known_relink
    char  obj[64];              // the XGUnit's object name, for known_relink
    char  name[64];             // the kind, as its flag names it
    int   number;
    int   dead;                 // found dead or stunned (known_gone)
    int   seen;                 // in sight at the last sight poll
    int   placed;               // loc holds a place the squad saw it
    float loc[3];
    int   lost_turn;            // the squad's m_iTurn when sight was lost; -1 unknown
} Known;

static Known g_known[KNOWN_MAX];
static int   g_nknown;
static void* g_known_squad;     // the human player the table belongs to
static int   g_known_carried;   // kept across a squad change, map not yet compared
static CursorGrid g_known_grid; // the map the places are on
static int   g_known_have_grid;
static int   g_known_turn = -1; // the squad's m_iTurn as last read

static void known_forget(const char* why)
{
    if (g_nknown) logf_("known: %d enemies forgotten -- %s\n", g_nknown, why);
    memset(g_known, 0, sizeof g_known);
    g_nknown = 0;
    g_known_squad = NULL;
    g_known_carried = 0;
    g_known_have_grid = 0;
    g_known_turn = -1;
    for (int i = 0; i < g_nunits; i++) g_units[i].number = 0;
}

// A different squad is a load or another mission, and which one is only
// known once the new map's grid can be read (known_settle). Until then the
// table is kept with its units let go, since after a load their addresses
// belong to other objects. One in sight at that moment was last seen then.
void known_reset(void)
{
    if (!g_nknown || !g_known_have_grid) { known_forget("a new squad"); return; }
    int kept = 0;
    char list[512];
    size_t used = 0;
    for (int i = 0; i < g_nknown; i++) {
        Known* kn = &g_known[i];
        if (kn->unit) kept++;
        kn->unit = NULL;
        if (kn->seen) { kn->seen = 0; kn->lost_turn = g_known_turn; }
        int w = _snprintf_s(list + used, sizeof list - used, _TRUNCATE, "%s%s %d (%s%s)",
                            used ? ", " : "", kn->name, kn->number, kn->obj,
                            kn->dead ? ", dead" : "");
        if (w > 0) used += (size_t)w;
    }
    // With the object names, so a load that matches nobody shows why.
    if (kept) logf_("known: %d enemies carried across a squad change, kept if the map is "
                    "the same: %s\n", g_nknown, used ? list : "");
    g_known_squad = NULL;
    g_known_carried = 1;
    for (int i = 0; i < g_nunits; i++) g_units[i].number = 0;
}

// After a squad change: the same grid is the same map, a load of this
// mission; any other is another mission, and its places mean nothing there.
// 0 while the grid cannot be read yet.
static int known_settle(void)
{
    CursorGrid g;
    if (!cursor_grid(&g)) return !g_known_carried;
    if (!g_known_carried) {
        g_known_grid = g;
        g_known_have_grid = 1;
        return 1;
    }
    if (memcmp(&g, &g_known_grid, sizeof g) != 0) {
        void* squad = g_known_squad;
        known_forget("another map");
        g_known_squad = squad;
        g_known_grid = g;
        g_known_have_grid = 1;
        return 1;
    }
    g_known_carried = 0;
    logf_("known: the same map after the squad change, %d enemies kept\n", g_nknown);
    return 1;
}

// Gives entries that lost their unit in a load the unit made again under the
// same object name, and its number back. Only while something is unmatched,
// and on every call then: known_number would otherwise give a unit coming
// back into sight a new number before it was matched.
static FieldSlot g_known_flag_unit;

static void known_relink(void)
{
    int want = 0;
    for (int i = 0; i < g_nknown; i++)
        if (!g_known[i].unit && g_known[i].obj[0] && !g_known[i].dead) want++;
    if (!want) return;
    for (int i = 0; i < g_nunits; i++) {
        UnitName* u = &g_units[i];
        const void* v;
        if (!u->flag || !u->name[0] ||
            !field_ptr(u->flag, "m_kUnit", &g_known_flag_unit, sizeof(void*), &v))
            continue;
        void* unit = *(void* const*)v;
        char obj[64];
        if (!unit || !unit_is_live(unit) || !object_name(unit, obj, sizeof obj)) continue;
        int taken = 0;
        for (int k = 0; k < g_nknown && !taken; k++) taken = g_known[k].unit == unit;
        if (taken) continue;
        for (int k = 0; k < g_nknown; k++) {
            Known* kn = &g_known[k];
            if (kn->unit || kn->dead || strcmp(kn->obj, obj) != 0) continue;
            if (strcmp(kn->name, u->name) != 0) {
                logf_("known: %s is a %s now, not %s %d -- not matched\n", obj, u->name,
                      kn->name, kn->number);
                continue;
            }
            kn->unit = unit;
            u->number = kn->number;
            logf_("known: %s %d is %s again\n", kn->name, kn->number, obj);
            break;
        }
    }
}

// Whether `squad` is a human player, asked by class every time. Remembered
// per pointer, it outlived the player: the 2026-09-28 (18:05) log loaded a
// save mid-mission, the old XGPlayer's memory came back as a live
// SpotLightComponent, and "field: no m_iTurn on SpotLightComponent" was read
// from it as the squad -- unit_is_live alone cannot tell, since the new
// object is live.
int squad_is_human(void* squad)
{
    char cls[64];
    return squad && unit_is_live(squad) && object_class_name(squad, cls, sizeof cls) &&
           (strcmp(cls, "XGPlayer") == 0 || strcmp(cls, "XGPlayer_MP") == 0);
}

// The table for this squad: a different human player is a new mission or a
// load, and everything known belonged to the last one.
static int known_for(void* squad)
{
    if (!squad_is_human(squad)) return 0;
    if (squad != g_known_squad) {
        if (g_known_squad) known_reset();
        g_known_squad = squad;
    }
    if (!known_settle()) return 0;
    known_relink();
    return 1;
}

static Known* known_find(const void* unit)
{
    if (!unit) return NULL;
    for (int i = 0; i < g_nknown; i++)
        if (g_known[i].unit == unit) return &g_known[i];
    return NULL;
}

// Numbers what the squad sees for the first time, by kind: the next after
// the highest this mission has given that kind, so a number is never given
// twice, even once its alien is dead.
static void known_number(void* squad, const SeenSet* set)
{
    if (!known_for(squad)) return;
    for (int k = 0; k < set->n; k++) {
        void* e = set->unit[k];
        if (!e || known_find(e) || g_nknown >= KNOWN_MAX) continue;
        UnitName* u = unit_by_unit(e);
        if (!u || !u->name[0] || unit_team(e) == TEAM_NEUTRAL) continue;
        Known* kn = &g_known[g_nknown++];
        memset(kn, 0, sizeof *kn);
        kn->unit = e;
        kn->lost_turn = -1;
        object_name(e, kn->obj, sizeof kn->obj);
        strncpy_s(kn->name, sizeof kn->name, u->name, _TRUNCATE);
        int top = 0;
        for (int i = 0; i < g_nknown - 1; i++)
            if (strcmp(g_known[i].name, kn->name) == 0 && g_known[i].number > top)
                top = g_known[i].number;
        kn->number = top + 1;
        u->number = kn->number;
        logf_("known: %s %d first seen\n", kn->name, kn->number);
    }
}

// The squad's turn: XGPlayer.m_iTurn, counted up as each of its turns begins
// (XGPlayer.BeginTurn, not on a load) and kept in the save.
static FieldSlot g_player_turn;

static int squad_turn(void* squad)
{
    const void* v;
    if (!squad || !field_ptr(squad, "m_iTurn", &g_player_turn, sizeof(int32_t), &v))
        return -1;
    return *(const int32_t*)v;
}

// Whether a known enemy is no longer anywhere to look for: dead, or stunned.
// A stunned alien never dies -- XGUnit.AddCriticallyWoundedAction(bStunAlien)
// sets m_bStunned and m_bCriticallyWounded, and IsAlive stays true -- so the
// 2026-09-29 (18:34) log kept "Floater 5, last seen .." on the scanner for
// six turns after the Arc Thrower took it.
static const void* g_stunned_prop;

static int known_gone(const Known* kn)
{
    if (!unit_is_live(kn->unit)) return 1;
    UnitTestFn alive = (UnitTestFn)tile_vfn(kn->unit, g_unit_slot_alive);
    if (alive && !alive(kn->unit, NULL)) return 1;
    // m_bStunned is XGUnitNativeBase's, the same property for every unit.
    if (!g_stunned_prop) g_stunned_prop = object_field_prop(kn->unit, "m_bStunned");
    int stunned = 0;
    if (g_stunned_prop &&
        props_read_object_bool(g_stunned_prop, (const uint8_t*)kn->unit, &stunned) &&
        stunned) {
        logf_("known: %s %d is stunned -- off the last seen list\n", kn->name, kn->number);
        return 1;
    }
    return 0;
}

void known_seen(void* squad, void* const* units, const float (*locs)[3], int n)
{
    if (!known_for(squad)) return;
    // Every poll, for known_reset: one in sight when a load comes was last
    // seen on the turn before it.
    int turn = squad_turn(squad);
    if (turn >= 0) g_known_turn = turn;
    for (int i = 0; i < g_nknown; i++) {
        Known* kn = &g_known[i];
        if (!kn->unit) continue;
        int j;
        for (j = 0; j < n && units[j] != kn->unit; j++) {}
        if (j < n) {
            if (!kn->seen && kn->placed)
                logf_("known: %s %d in sight again\n", kn->name, kn->number);
            kn->seen = 1;
            kn->placed = 1;
            memcpy(kn->loc, locs[j], sizeof kn->loc);
            continue;
        }
        // Out of sight: a death is noted here, on every poll, and not only
        // when the scanner asks (known_lost). A load keeps the entry but not
        // the unit, and the corpse has no flag to be matched to, so a death
        // not noted before the load would come back as "last seen".
        if (!kn->dead && unit_is_live(kn->unit) && known_gone(kn)) kn->dead = 1;
        if (!kn->seen) continue;
        kn->seen = 0;
        kn->lost_turn = turn;
        logf_("known: %s %d out of sight at %.0f, %.0f, %.0f (turn %d)\n", kn->name,
              kn->number, kn->loc[0], kn->loc[1], kn->loc[2], turn);
    }
}

int known_lost(void* squad, const SeenSet* now, KnownLost* out, int max)
{
    // The table is the human squad's; in the aliens' turn the cursor's player
    // is theirs, and the last human one still owns it.
    if (!squad_is_human(squad)) squad = g_known_squad;
    if (!squad || squad != g_known_squad || !squad_is_human(squad)) return 0;
    if (!known_settle()) return 0;
    known_relink();
    int turn = squad_turn(squad);
    int k = 0;
    for (int i = 0; i < g_nknown && k < max; i++) {
        Known* kn = &g_known[i];
        if (kn->seen || !kn->placed || kn->dead) continue;
        // Not matched since a load: still where the squad last saw it.
        if (kn->unit) {
            if (seen_has(now, kn->unit)) continue;
            // Dead, stunned, or its object gone: no longer somewhere to look.
            if (known_gone(kn)) { kn->dead = 1; continue; }
        }
        _snprintf_s(out[k].label, sizeof out[k].label, _TRUNCATE, "%s %d", kn->name,
                    kn->number);
        memcpy(out[k].loc, kn->loc, sizeof out[k].loc);
        out[k].turns_ago = turn >= 0 && kn->lost_turn >= 0 && turn >= kn->lost_turn
                               ? turn - kn->lost_turn : -1;
        k++;
    }
    return k;
}

// ---- what the squad sees: the one rule -------------------------------------
//
// The rule and why it is one are in units.h.

// Taken once per readout; the sets are pointer lists, compared, never
// dereferenced (see tile_exposure for what that costs when forgotten).
void squad_sight_take(void* squad, SquadSight* v)
{
    v->squad = squad;
    squad_sight(squad, &v->enemies);
    squad_sight_civilians(squad, &v->civilians);
}

int squad_sees(const SquadSight* v, void* unit, const float* loc,
               int friendly, const char* name)
{
    if (friendly) return 1;
    if (unit_team(unit) == TEAM_NEUTRAL)
        return civilian_seen(v->squad, &v->civilians, unit, loc, name);
    return seen_has(&v->enemies, unit);
}

void unit_label(const UnitName* u, char* out, size_t out_sz)
{
    char num[16] = "";
    if (u->number > 0) _snprintf_s(num, sizeof num, _TRUNCATE, " %d", u->number);
    _snprintf_s(out, out_sz, _TRUNCATE, u->nick[0] ? "%s%s, %s" : "%s%s", u->name, num,
                u->nick);
}

// Whether a unit is on overwatch, asked of the game's own native. Only for
// units unit_seen has just vouched for: a dead unit's natives are not safe to
// call.
int unit_overwatch(void* unit)
{
    UnitTestFn on = unit ? (UnitTestFn)tile_vfn(unit, g_unit_slot_overwatch) : NULL;
    return on && on(unit, NULL) != 0;
}

// The name with what the screen shows about the unit: "Sectoid, 3 of 4 HP, on
// overwatch". HP is the flag's; overwatch is said of enemies the squad sees,
// which is when the game floats "Overwatch" over one (XGAbilityTree's target
// message) and shows its stance -- never of a hidden one, which unit_seen and
// the squad's sight have already kept out.
void unit_label_state(const UnitName* u, void* unit, int enemy,
                      char* out, size_t out_sz)
{
    unit_label(u, out, out_sz);
    char state[64];
    combat_unit_state(u->hp, u->hp_max, enemy && unit_overwatch(unit), state, sizeof state);
    size_t used = strlen(out);
    if (state[0] && used < out_sz)
        _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s", state);
}

// ---- the soldier being moved -----------------------------------------------

// The soldier being moved: the active unit, the one the cursor is chained to
// and the target strip is drawn for.
static FieldSlot g_active_unit;

void* soldier_unit(void)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn ||
        !field_ptr(pawn, "m_kGameUnit", &g_active_unit, sizeof(void*), &v))
        return NULL;
    void* unit = *(void* const*)v;
    return unit && unit_is_live(unit) ? unit : NULL;
}

// The soldier's own tile, from the pawn the cursor is chained to. `z` gets the
// pawn's height, which stands in for a cursor's: both are compared with the
// floor through NAVH_LIFT.
//
// Needed because the cursor is not the soldier. In mouse mode it follows the
// mouse every frame, and when the soldier changes the camera pans while the
// mouse stays put, so the cursor lands wherever the mouse now points -- in one
// run, three soldiers in a row began at the map's northern edge, rows 54 to
// 60 of 61, where no path went anywhere.
static FieldSlot g_soldier_loc;

int soldier_tile(const CursorGrid* g, int* tx, int* ty, float* z)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn ||
        !field_ptr(pawn, "Location", &g_soldier_loc, 3 * sizeof(float), &v))
        return 0;
    const float* loc = (const float*)v;
    *tx = grid_x(g, loc[0]);
    *ty = grid_y(g, loc[1]);
    *z = loc[2];
    return 1;
}

// Whether the soldier being moved has no moves left (XGUnit.m_iMoves, what
// GetMoves returns), and so no path action for the cursor to drive.
static FieldSlot g_unit_moves;
int soldier_out_of_moves(void)
{
    void* unit = soldier_unit();
    const void* v;
    if (!unit || !field_ptr(unit, "m_iMoves", &g_unit_moves, sizeof(int32_t), &v)) return 0;
    return *(const int32_t*)v <= 0;
}

static FieldSlot g_aim_action;

int soldier_aiming(void)
{
    void* unit = soldier_unit();
    const void* v;
    if (!unit || !field_ptr(unit, "m_kCurrAction", &g_aim_action, sizeof(void*), &v))
        return 0;
    void* action = *(void* const*)v;
    char name[128];
    if (!action || !unit_is_live(action) || !object_name(action, name, sizeof name))
        return 0;
    return strncmp(name, "XGAction_Targeting", 18) == 0 ||
           strncmp(name, "XGAction_Fire", 13) == 0;
}

// ---- units with no flag ----------------------------------------------------

// Civilians with no flag over them, which scan_add_units cannot see.
//
// The unit table is built from UIUnitFlag.SetNames, and not every unit has a
// flag: UIUnitFlagManager.OnInit gives one to every XGUnit NOT on the neutral
// team, and a civilian gets one only when spawned with bAddFlag or when
// XGBattle.SwapTeams moves them to a side. The terror civilians were thought
// to be the bAddFlag kind; on the Novgorod terror map (2026-09-28, 20:27 log)
// not one had a flag. A mission's
// survivor is neither until rescued -- the 2026-09-27 (23:33) log has no
// "SetNames Survivor" until the escort, and "No civilians" at every press
// before it. So the units come from the object walk too, and a neutral one
// with no flag is listed from it (flagless_units, main.c) through this. Whether
// the squad sees them is the caller's to ask (squad_sees), after whatever
// cheaper test it has -- a tile, a blast.
//
// Named as UIUnitFlag.OnInit would name them: a civilian character's
// strLastName (and nickname), else the unit's behavior says what it is.
static FieldSlot g_fl_char, g_fl_last, g_fl_nick, g_fl_behavior;

static void flagless_name(void* unit, char* out, size_t out_sz)
{
    const void* v;
    out[0] = 0;
    if (field_ptr(unit, "m_kCharacter", &g_fl_char, sizeof(void*), &v)) {
        void* ch = *(void* const*)v;
        char last[64] = "", nick[64] = "";
        if (ch && unit_is_live(ch) &&
            field_ptr(ch, "strLastName", &g_fl_last, sizeof(FString), &v))
            read_fstring((const FString*)v, last, sizeof last);
        if (last[0] && field_ptr(ch, "strNickName", &g_fl_nick, sizeof(FString), &v))
            read_fstring((const FString*)v, nick, sizeof nick);
        if (last[0]) {
            _snprintf_s(out, out_sz, _TRUNCATE, nick[0] ? "%s, %s" : "%s", last, nick);
            return;
        }
    }
    char cls[64];
    if (field_ptr(unit, "m_kBehavior", &g_fl_behavior, sizeof(void*), &v) &&
        *(void* const*)v && object_class_name(*(void* const*)v, cls, sizeof cls) &&
        strcmp(cls, "XGAIBehavior_Survivor") == 0)
        strcpy_s(out, out_sz, "Survivor");
    else
        strcpy_s(out, out_sz, "Civilian");
}

int flagless_unit(void* unit, FlaglessUnit* out)
{
    if (!unit_is_live(unit) || unit_team(unit) != TEAM_NEUTRAL) return 0;
    if (unit_by_unit(unit)) return 0;       // has a flag: the table's own
    // IsAlive, not IsAliveAndVisible: whether the squad sees them is
    // squad_sees' to say, and a survivor may never be "visible" in the
    // sense the flags use.
    UnitTestFn alive = (UnitTestFn)tile_vfn(unit, g_unit_slot_alive);
    if (!alive || !alive(unit, NULL)) return 0;
    void* pawn = unit_pawn(unit);
    const void* v;
    if (!pawn || !unit_is_live(pawn) ||
        !field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v))
        return 0;
    memcpy(out->loc, v, sizeof out->loc);
    out->unit = unit;
    out->pawn = pawn;
    flagless_name(unit, out->name, sizeof out->name);
    return 1;
}
