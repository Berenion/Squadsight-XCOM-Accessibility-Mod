// The level actors the scanner, the door sounds and the blast list share:
// doors, windows, panels, ladders, Meld canisters, the radar array, what
// explodes, and units with no flag. See world.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "world.h"
#include "where.h"
#include "units.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "objects.h"
#include "props.h"
#include "names.h"

// XComInteractiveLevelActor.IconSocket: how the level designer classified it.
// XGDOOR_Icon 0, XGWINDOW_Icon 1, XGBUTTON_Icon 2. Read rather than asking the
// native IsDoor(), because this is a plain byte on the actor and calling into
// script is something this DLL does not do.
#define ICON_WINDOW  1
#define ICON_BUTTON  2

static FieldSlot g_icon, g_ladder_loc, g_ilact_loc;
static FieldSlot g_meld_loc, g_meld_turns;

// An actor's Location, through the same field walk everything else uses.
static int actor_location(void* actor, FieldSlot* slot, float* out)
{
    const void* v;
    if (!field_ptr(actor, "Location", slot, 3 * sizeof(float), &v)) return 0;
    memcpy(out, v, 3 * sizeof(float));
    return 1;
}

// ---- the level actors, kept between key presses ----------------------------
//
// A full walk of the object table is 175,000 entries and tens of milliseconds
// -- one run measured 78 ms, five frames, with the game thread stopped for
// all of it -- and doing one per key press was felt as lag. The first attempt
// at that was a three-second cache, which only moved the stall around: every
// press more than three seconds after the last one paid for it again, which
// is most presses.
//
// So the walk is done once per mission and then kept up to date instead. What
// is kept is the *actors*, not the finished items: what the scanner says
// about one -- its tile, and a Meld canister's countdown -- is worked out
// again on every press, so nothing here is ever stale.
//
// Keeping up to date is two cheap things. Actors that have gone are dropped,
// because a door can be blown off its hinges and a canister can expire, and
// objects_still asks the table rather than trusting the pointer. Then the
// walk resumes where it stopped, over whatever the mission has added since,
// which is usually nothing. A different cursor or a different grid is a
// different map, and starts again from the beginning.
typedef struct {
    void* actor;
    int   idx;      // its slot in the object table, for objects_still
    int   kind;     // 0 interactive, 1 ladder, 2 Meld canister, 3 window,
                    // 4 a blast (its actor is the action, its owner explodes),
                    // 5 a unit (scan_add_flagless: civilians with no flag)
} WorldActor;

static WorldActor g_wactors[SCAN_MAX];
static int        g_wactor_n;
static int        g_world_next;     // where the last walk stopped
static int        g_world_have;
static void*      g_world_cursor;
static CursorGrid g_world_grid;

static ScanItem   g_world[SCAN_MAX];
static int        g_world_n;

static void world_keep(const ScanItem* it)
{
    if (g_world_n < SCAN_MAX) g_world[g_world_n++] = *it;
}

// An item's tile on the grid the actors are held for, from its world position.
// 0 off the grid: a class default object put "Radar array" on tile 65, -12 --
// world (0, 0), where an object with no position sits -- and Home sent the
// cursor over the edge of the map after it. A level actor's Location is at its
// base, so the floor is asked at the Location itself.
//
// Not scan_item_at, which this was until 2026-09-28: that refuses everything
// until the scanner has measured from somewhere (g_scan_have), so the doors
// and windows the door sounds play were not placed until the first Page Up or
// Page Down of a session.
static int world_item_at(ScanItem* it, const float* world)
{
    memcpy(it->world, world, 3 * sizeof(float));
    it->tx = cursor_tile_axis(world[0], g_world_grid.min_x, CURSOR_TILE);
    it->ty = cursor_tile_axis(world[1], g_world_grid.min_y, CURSOR_TILE);
    if (it->tx < 0 || it->ty < 0 ||
        it->tx >= g_world_grid.num_x || it->ty >= g_world_grid.num_y)
        return 0;
    it->tz = floor_of(world);
    return 1;
}

static void scan_describe_interactive(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);

    const void* v;
    int icon = 0;
    if (field_ptr(actor, "IconSocket", &g_icon, 1, &v))
        icon = *(const uint8_t*)v;

    // The radar array is the objective on the missions that have one, and it
    // is an interactive actor like any other -- so it is named and filed
    // before the icon gets a say.
    if (object_is_a(actor, "XComRadarArrayActor")) {
        it.kind = SCAN_OBJECTIVES;
        strncpy_s(it.name, sizeof it.name, "Radar array", _TRUNCATE);
    } else if (icon == ICON_WINDOW) {
        it.kind = SCAN_INTERACT;
        strncpy_s(it.name, sizeof it.name, "Window", _TRUNCATE);
    } else if (icon == ICON_BUTTON) {
        it.kind = SCAN_INTERACT;
        strncpy_s(it.name, sizeof it.name, "Panel", _TRUNCATE);
    } else {
        it.kind = SCAN_DOORS;
        strncpy_s(it.name, sizeof it.name, "Door", _TRUNCATE);
    }

    float world[3];
    if (!actor_location(actor, &g_ilact_loc, world)) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

// ---- what explodes ---------------------------------------------------------
//
// A destructible that explodes carries an XComDestructibleActor_Action_Radial
// Damage in its DamagedEvents or DestroyedEvents: the blast, with its radius
// (500 units, five tiles, by default) and damage. The events are structs with
// an editor-only string in them, whose size in a cooked build cannot be taken
// on trust, so they are not read. The action is walked for instead: it is
// declared `within XComDestructibleActor`, so its Outer is the actor that
// blows up. A car with a blast on being damaged and another on being destroyed
// is one car (g_blast_owner). An owner already destroyed is left out; one
// under three quarters of its toughness is "damaged", the game's own
// DestructibleActorDamagedThreshold, which is where a car starts to burn.
static FieldSlot g_blast_outer, g_blast_radius, g_blast_loc, g_blast_health,
                 g_blast_tough, g_blast_tough_hp, g_blast_smc, g_blast_mesh;
static void*     g_blast_owner[SCAN_MAX];
static int       g_blast_owner_n;

static void scan_describe_explosive(void* action)
{
    const void* v;
    if (!field_ptr(action, "Outer", &g_blast_outer, sizeof(void*), &v)) return;
    void* owner = *(void* const*)v;
    if (!owner || !unit_is_live(owner) || !object_is_a(owner, "XComDestructibleActor")) return;
    for (int i = 0; i < g_blast_owner_n; i++) if (g_blast_owner[i] == owner) return;
    if (g_blast_owner_n < SCAN_MAX) g_blast_owner[g_blast_owner_n++] = owner;

    int health = -1, most = -1;
    if (field_ptr(owner, "Health", &g_blast_health, sizeof(int32_t), &v))
        health = *(const int32_t*)v;
    if (field_ptr(owner, "Toughness", &g_blast_tough, sizeof(void*), &v) && *(void* const*)v &&
        field_ptr(*(void* const*)v, "Health", &g_blast_tough_hp, sizeof(int32_t), &v))
        most = *(const int32_t*)v;
    if (health == 0) return;                // already blown up

    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_EXPLOSIVES;
    char mesh[SCAN_NAME] = "";
    if (field_ptr(owner, "StaticMeshComponent", &g_blast_smc, sizeof(void*), &v) &&
        *(void* const*)v &&
        field_ptr(*(void* const*)v, "StaticMesh", &g_blast_mesh, sizeof(void*), &v))
        object_name(*(void* const*)v, mesh, sizeof mesh);
    scan_mesh_words(mesh, "Explosive", it.name, sizeof it.name);

    float radius = 0.0f;
    if (field_ptr(action, "DamageRadius", &g_blast_radius, sizeof(float), &v))
        radius = *(const float*)v;
    int tiles = (int)(radius / CURSOR_TILE + 0.5f);
    int damaged = health > 0 && most > 0 && health < most * 3 / 4;
    if (tiles > 0)
        _snprintf_s(it.detail, sizeof it.detail, _TRUNCATE, "blast %d tile%s%s",
                    tiles, tiles == 1 ? "" : "s", damaged ? ", damaged" : "");
    else
        _snprintf_s(it.detail, sizeof it.detail, _TRUNCATE, "explodes%s",
                    damaged ? ", damaged" : "");

    // A blast on an archetype (ARC_...) has the archetype as its Outer, which
    // is no car on the map: it stands at the origin, which can fall on a real
    // tile. Those are left out by name and by place.
    char owner_name[SCAN_NAME];
    if (object_name(owner, owner_name, sizeof owner_name) &&
        (!strncmp(owner_name, "ARC_", 4) || !strncmp(owner_name, "Default__", 9)))
        return;
    float world[3];
    if (!actor_location(owner, &g_blast_loc, world)) return;
    if (world[0] == 0.0f && world[1] == 0.0f && world[2] == 0.0f) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

static FieldSlot g_window_loc;

static void scan_describe_window(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_INTERACT;
    strncpy_s(it.name, sizeof it.name, "Window", _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_window_loc, world)) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

static void scan_describe_ladder(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_INTERACT;
    strncpy_s(it.name, sizeof it.name, "Ladder", _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_ladder_loc, world)) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

static void scan_describe_meld(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_MELD;
    strncpy_s(it.name, sizeof it.name, "Meld canister", _TRUNCATE);

    // How long it lasts is the whole decision about a canister: -1 when it
    // has no timer (the Meld tutorial's), 0 once it has run out.
    const void* v;
    int turns = -1;
    if (field_ptr(actor, "m_iTurnsUntilDestroyed", &g_meld_turns, sizeof(int32_t), &v))
        turns = *(const int32_t*)v;

    // Where it is, only as far as the HUD tells a sighted player
    // (UISpecialMissionHUD_MeldStats.UpdatePanel): a canister nobody has seen
    // is "LOCATION UNKNOWN" and gets no arrow; one seen is pointed at from
    // then on, with its countdown -- "?" until then; one collected is
    // "COLLECTED" (m_strRecoveredLabel), one run out "LOST". The first Gateway run (2026-09-25) had the unseen one's
    // tile and timer under Objectives. m_bHasBeenSeen,
    // m_bVisibleToSquad and m_bCollected share a dword, so they are read
    // only once the bool mask is known; before that it is placed as seen.
    int seen = 1, got = 0;
    if (props_mask_offset()) {
        static const void* s_cls;
        static const void* s_seen;
        static const void* s_got;
        uint32_t class_off = props_class_offset();
        const void* cls = class_off && readable((uint8_t*)actor + class_off, sizeof(void*))
                              ? *(void* const*)((uint8_t*)actor + class_off) : NULL;
        if (cls && cls != s_cls) {
            s_cls = cls;
            s_seen = object_field_prop(actor, "m_bHasBeenSeen");
            s_got = object_field_prop(actor, "m_bCollected");
        }
        if (s_seen) props_read_object_bool(s_seen, actor, &seen);
        if (s_got) props_read_object_bool(s_got, actor, &got);
    }

    char timer[32] = "";
    if (turns > 0)
        _snprintf_s(timer, sizeof timer, _TRUNCATE, "%d turn%s left", turns,
                    turns == 1 ? "" : "s");
    if (got || turns == 0) {
        it.unplaced = 1;
        strncpy_s(it.detail, sizeof it.detail, got ? "collected" : "lost", _TRUNCATE);
        world_keep(&it);
        return;
    }
    if (!seen) {
        it.unplaced = 1;
        // The HUD's counter shows "?" for its turns until it is seen.
        strncpy_s(it.detail, sizeof it.detail, "location unknown, turns unknown", _TRUNCATE);
        world_keep(&it);
        return;
    }
    strncpy_s(it.detail, sizeof it.detail, timer, _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_meld_loc, world)) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

// What the scanner would say about each actor it is holding, worked out
// afresh: the tiles are relative to a grid, and a canister's countdown is
// relative to the turn.
static void scan_world_items(void)
{
    g_world_n = 0;
    g_blast_owner_n = 0;
    for (int i = 0; i < g_wactor_n; i++) {
        switch (g_wactors[i].kind) {
        case 0:  scan_describe_interactive(g_wactors[i].actor); break;
        case 1:  scan_describe_ladder(g_wactors[i].actor);      break;
        case 2:  scan_describe_meld(g_wactors[i].actor);        break;
        case 3:  scan_describe_window(g_wactors[i].actor);      break;
        case 5:  break;     // units are scan_add_flagless's, not items
        default: scan_describe_explosive(g_wactors[i].actor);   break;
        }
    }
}

// The walk hands back an index into the class list it was given; this carries
// the mapping across it, since a visitor gets no state of its own beyond ctx
// and this keeps the call cheap.
static const int* g_scan_kinds;

// A window is a plain XComDestructibleActor -- the class of every crate, car
// and fence -- and nothing in script says which are windows: the window icon
// on interactive actors exists, but no map seen uses it, and the traversal a
// soldier makes through one (eTraversal_BreakWindow) is in pathing data with
// no script accessor. A survey of one map's 948 destructibles (2026-09-23,
// 22:37 log) showed them by their static mesh: WindowSolidSingleE,
// WindowSolidDoubleA, WindowSolidDouble_DAMAGE, all Toughness_GLASS, and
// BoardedWindows in wood. Glass alone is not the test -- WarningLight is glass
// too -- so the test is the mesh's name.
static FieldSlot g_win_smc, g_win_mesh;

static int is_window(void* actor)
{
    const void* v;
    char mesh[64];
    if (!field_ptr(actor, "StaticMeshComponent", &g_win_smc, sizeof(void*), &v)) return 0;
    void* smc = *(void* const*)v;
    if (!smc || !field_ptr(smc, "StaticMesh", &g_win_mesh, sizeof(void*), &v)) return 0;
    if (!object_name(*(void* const*)v, mesh, sizeof mesh)) return 0;
    return strstr(mesh, "Window") != NULL || strstr(mesh, "window") != NULL;
}

// Whether a Meld canister is one on the map. The Meld walk of 2026-09-25
// found three where the HUD had two counters, and the third said "lost"
// every time: m_iTurnsUntilDestroyed at its default 0. Class defaults are
// already dropped by name (objects.c); what is left is a template kept in a
// package. The HUD counts AllActors -- the actors in a level -- so the same
// test is made here: the canister's Outer is a Level. When the Outer cannot
// be read, the names archetypes and defaults go by are refused instead.
static FieldSlot g_meld_outer;
static int meld_on_map(void* actor)
{
    char name[SCAN_NAME] = "?", outer_name[SCAN_NAME] = "?", outer_cls[SCAN_NAME] = "?";
    object_name(actor, name, sizeof name);
    const void* v;
    void* outer = NULL;
    if (field_ptr(actor, "Outer", &g_meld_outer, sizeof(void*), &v)) outer = *(void* const*)v;
    int ok;
    if (outer && object_class_name(outer, outer_cls, sizeof outer_cls)) {
        object_name(outer, outer_name, sizeof outer_name);
        ok = strcmp(outer_cls, "Level") == 0;
    } else {
        ok = strncmp(name, "ARC_", 4) != 0 && strncmp(name, "Default__", 9) != 0;
    }
    logf_("scan: Meld canister %s in %s (%s) -- %s\n", name, outer_name, outer_cls,
          ok ? "on the map" : "not on the map, left out");
    return ok;
}

static int scan_collect_world(void* actor, int which, int idx, void* ctx)
{
    (void)ctx;
    if (g_wactor_n >= SCAN_MAX) return 0;
    // Every destructible on the map comes through, nearly a thousand; only
    // the windows are kept.
    if (g_scan_kinds[which] == 3 && !is_window(actor)) return 1;
    if (g_scan_kinds[which] == 2 && !meld_on_map(actor)) return 1;
    g_wactors[g_wactor_n].actor = actor;
    g_wactors[g_wactor_n].idx   = idx;
    g_wactors[g_wactor_n].kind  = g_scan_kinds[which];
    g_wactor_n++;
    return 1;
}

// The four classes, resolved together, because finding a class by name costs
// a pass over the whole table. After the first scan of a mission they come
// from the cache. `map` receives the kind each entry of `use` stands for.
// XComDestructibleActor last: interactive actors are destructibles too, and
// an object matching two entries goes to the first.
static int scan_world_classes(const void** use, int* map)
{
    static const char* const names[] = {
        "XComInteractiveLevelActor", "XComLadder", "XComMeldContainerActor",
        "XComDestructibleActor", "XComDestructibleActor_Action_RadialDamage",
        "XGUnit",
    };
    const void* cls[6];
    objects_classes(names, cls, 6);

    int n = 0;
    for (int i = 0; i < 6; i++)
        if (cls[i]) { use[n] = cls[i]; map[n] = i; n++; }
    return n;
}

static void scan_world_full(const void* const* use, int nclasses)
{
    g_wactor_n = 0;
    g_world_next = 0;
    if (objects_each_from(use, nclasses, 0, &g_world_next,
                          scan_collect_world, NULL) < 0) {
        logf_("scan: the object table could not be read this press\n");
        return;
    }

    unsigned ms;
    int entries;
    objects_last_walk(&ms, &entries);
    int blasts = 0;
    for (int i = 0; i < g_wactor_n; i++) if (g_wactors[i].kind == 4) blasts++;
    logf_("scan: full object walk %d entries in %u ms, %d actors kept, %d of them blasts\n",
          entries, ms, g_wactor_n, blasts);
}

static void scan_world_catch_up(const void* const* use, int nclasses)
{
    int had = g_wactor_n;

    int keep = 0;
    for (int i = 0; i < g_wactor_n; i++)
        if (objects_still(g_wactors[i].actor, g_wactors[i].idx, use, nclasses))
            g_wactors[keep++] = g_wactors[i];
    g_wactor_n = keep;

    int was = g_world_next;
    objects_each_from(use, nclasses, g_world_next, &g_world_next,
                      scan_collect_world, NULL);

    // Silent when nothing has changed, which is the usual answer and the
    // whole point of not walking the table again.
    int gone = had - keep, found = g_wactor_n - keep;
    if (gone || found)
        logf_("scan: %d gone, %d new over %d entries added since\n",
              gone, found, g_world_next - was);
}

// Brings the level actors and their items (world_items) up to date for the
// map on grid `g`: the scanner adds them to its list, the door sounds and the
// blast list read them. Returns 0 when there is nothing.
int world_refresh(const CursorGrid* g)
{
    g_world_n = 0;
    if (!objects_ready()) {
        // The probe at startup runs while the game is still in its shell,
        // where the object table can be too small to recognise. In a mission
        // it is not, so it is worth another look -- and the answer is logged
        // whichever way it goes, once.
        char why[256];
        int got = objects_retry(why, sizeof why);
        if (why[0]) logf_("objects: %s%s\n", got ? "" : "still unavailable -- ", why);
        if (!got) return 0;
    }

    const void* use[6];
    int map[6];
    int nclasses = scan_world_classes(use, map);
    if (!nclasses) return 0;
    g_scan_kinds = map;

    // A different cursor, or a different grid, is a different map: what was
    // found last time belongs to one that is gone. So is a table that has
    // shrunk below where the last walk stopped -- it cannot be the table
    // those indices were taken from, and resuming into it would count
    // everything a second time.
    int same_map = g_world_have && g_world_cursor == cursor_object() &&
                   memcmp(&g_world_grid, g, sizeof g_world_grid) == 0 &&
                   g_world_next <= objects_count();
    // Several callers can ask within one key press -- the units, then the
    // level actors, for "Everything" -- and the second catch-up walk would
    // find nothing the first did not.
    static ULONGLONG walked_at;
    ULONGLONG now = GetTickCount64();
    if (!same_map)                     scan_world_full(use, nclasses);
    else if (now - walked_at >= 100)   scan_world_catch_up(use, nclasses);
    walked_at = now;

    g_world_have = 1;
    g_world_cursor = cursor_object();
    g_world_grid = *g;

    scan_world_items();
    return 1;
}

// Civilians with no flag over them, from the object walk (flagless_unit in
// units.c has why and how they are named).
int flagless_units(int refresh, FlaglessUnit* out, int max)
{
    // The scanner brings the walk up to date; a step only walks the table
    // the first time, since a unit on the map at the start stays in it.
    if (refresh || !g_world_have) {
        CursorGrid g;
        if (!cursor_grid(&g) || !world_refresh(&g)) return 0;
    }
    int n = 0;
    for (int i = 0; i < g_wactor_n && n < max; i++)
        if (g_wactors[i].kind == 5 && flagless_unit(g_wactors[i].actor, &out[n])) n++;
    return n;
}

const ScanItem* world_items(int* n)
{
    *n = g_world_n;
    return g_world;
}

int world_explodes(const void* actor)
{
    for (int i = 0; i < g_blast_owner_n; i++) if (g_blast_owner[i] == actor) return 1;
    return 0;
}
