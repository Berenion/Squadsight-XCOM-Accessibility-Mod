// The level actors the scanner, the door sounds and the blast list share:
// doors, windows, panels, ladders, Meld canisters, the radar array, what
// explodes, and units with no flag. See world.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "world.h"
#include "strings.h"
#include "where.h"
#include "units.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "objects.h"
#include "props.h"
#include "names.h"
#include "fog.h"
#include "nav.h"

// XComInteractiveLevelActor.IconSocket: how the level designer classified it.
// XGDOOR_Icon 0, XGWINDOW_Icon 1, XGBUTTON_Icon 2. Read rather than asking the
// native IsDoor(), because this is a plain byte on the actor and calling into
// script is something this DLL does not do.
#define ICON_WINDOW  1
#define ICON_BUTTON  2

static FieldSlot g_icon, g_ladder_loc, g_ilact_loc;
static FieldSlot g_meld_loc, g_meld_turns;

// ---- whether a door is shut ------------------------------------------------
//
// A door is shut while it is in its first state. XComDestructibleActor is
// `auto state _Pristine`; XGAction_Interact opening it ends in
// _Pristine.EndInteraction -> GotoState('_Inactive'), whose CanInteract is
// false, and a door shot off its hinges goes to _Destroyed. So any state but
// _Pristine is open.
//
// The state is UObject.StateFrame (an FStateFrame*, at 0x14 -- between
// HashOuterNext and _Linker, below the Index at 0x20 that objects.c found)
// and the frame's Node (FFRAME_NODE), set to the state by GotoState. The
// 0x14 is UE3's layout, not read out of a disassembly, so what it yields is
// only believed when it is a live object of class State; otherwise the door's
// collision decides, which the 2026-10-01 (11:46) log showed going from 1 to
// 0 when Hagen opened XComInteractiveLevelActor_3 with V
// (_Pristine.BeginInteraction -> SetCollision(false, ...)).
#define UOBJECT_STATEFRAME 0x14

// `why` gets what was found instead when it is not a state, for the log.
static int object_state(void* obj, char* out, size_t out_sz, char* why, size_t why_sz)
{
    const uint8_t* o = (const uint8_t*)obj;
    strcpy_s(why, why_sz, "no state frame");
    if (!readable(o + UOBJECT_STATEFRAME, sizeof(void*))) return 0;
    const uint8_t* frame = *(const uint8_t* const*)(o + UOBJECT_STATEFRAME);
    if (!frame || !readable(frame + FFRAME_NODE, sizeof(void*))) return 0;
    void* node = *(void* const*)(frame + FFRAME_NODE);
    strcpy_s(why, why_sz, "no node");
    if (!node || !objects_live(node)) return 0;
    char cls[32] = "?", name[64] = "?";
    object_class_name(node, cls, sizeof cls);
    object_name(node, name, sizeof name);
    if (strcmp(cls, "State") != 0) {
        _snprintf_s(why, why_sz, _TRUNCATE, "node %s of class %s", name, cls);
        return 0;
    }
    strcpy_s(out, out_sz, name);
    return 1;
}

// Which actors' state reads have been logged: once each, not once a refresh.
// The 2026-10-01 (13:11) log flipped between "reads" and "cannot be read"
// 956 times, door after door, saying nothing about which or why.
#define STATE_LOGGED_MAX 256
static void* g_state_logged[STATE_LOGGED_MAX];
static int   g_state_logged_n;

static void state_log_once(void* actor, int have, const char* state, const char* why)
{
    for (int i = 0; i < g_state_logged_n; i++) if (g_state_logged[i] == actor) return;
    if (g_state_logged_n < STATE_LOGGED_MAX) g_state_logged[g_state_logged_n++] = actor;
    char name[80] = "?";
    object_name(actor, name, sizeof name);
    if (have) logf_("world: %s is in state %s\n", name, state);
    else      logf_("world: %s has no readable state (%s)\n", name, why);
}

// The actor's state name; 0 when it cannot be read.
static int actor_state(void* actor, char* out, size_t out_sz)
{
    char why[128] = "";
    int have = 0;
    GUARDED("world: state", have = object_state(actor, out, out_sz, why, sizeof why), have = 0);
    state_log_once(actor, have, out, why);
    return have;
}

int world_door_shut(void* door, char* how, size_t how_sz)
{
    char state[64] = "";
    int shut = -1;
    int have = actor_state(door, state, sizeof state);
    if (have) {
        shut = strcmp(state, "_Pristine") == 0;
        if (how) _snprintf_s(how, how_sz, _TRUNCATE, "state %s", state);
        return shut;
    }
    const void* prop = object_field_prop(door, "bCollideActors");
    int b = -1;
    if (prop && props_read_object_bool(prop, (const uint8_t*)door, &b)) shut = b != 0;
    if (how) _snprintf_s(how, how_sz, _TRUNCATE, "bCollideActors %d", b);
    return shut;
}

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
    int   revealed; // the fog has lifted off it once (world_unseen)
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
    it->tx = grid_x(&g_world_grid, world[0]);
    it->ty = grid_y(&g_world_grid, world[1]);
    if (it->tx < 0 || it->ty < 0 ||
        it->tx >= g_world_grid.num_x || it->ty >= g_world_grid.num_y)
        return 0;
    it->tz = floor_of(world);
    it->feet = world[2];
    return 1;
}

// ---- the fog --------------------------------------------------------------
//
// A tile nobody has seen is black on the screen (fog.h), and whatever stands
// on it is not drawn: a sighted player does not know where the doors are in a
// building nobody has looked into, and does not know a UFO's power source is
// there until someone has seen it. So a level actor is left out until the fog
// has lifted off it, and kept from then on: seen before is grey, and grey
// still shows the door. The 3D fog is per tile, and an actor is not one tile
// -- a door stands on the line between two, a car covers several, a ladder
// climbs a storey -- so it counts as seen when any tile within `reach` of its
// Location, and up to `up` layers above its base, is.
//
// When the fog cannot be read (FOG_UNKNOWN everywhere asked) nothing is
// hidden: the scanner goes back to what it said before the fog, rather than
// going silent about every door on the map.
static int g_fog_hidden;            // this refresh: actors the fog kept out

static int world_unseen(WorldActor* wa, const ScanItem* it, int reach, int up)
{
    if (wa->revealed) return 0;
    int tz = grid_floor_layer(&g_world_grid, it->world[2]);
    int never = 0;
    for (int z = tz; z <= tz + up; z++)
        for (int dy = -reach; dy <= reach; dy++)
            for (int dx = -reach; dx <= reach; dx++) {
                int f = fog_tile(&g_world_grid, it->tx + dx, it->ty + dy, z);
                if (f == FOG_SEEN) {
                    wa->revealed = 1;
                    logf_("fog: %s at %d, %d revealed (tile %d, %d, %d)\n", it->name,
                          it->tx, it->ty, it->tx + dx, it->ty + dy, z);
                    return 0;
                }
                if (f == FOG_NEVER) never++;
            }
    if (!never) return 0;
    g_fog_hidden++;
    return 1;
}

// How far round an actor the fog is asked: a tile each way for what stands in
// a wall, two for what is several tiles across.
#define FOG_REACH_WALL  1
#define FOG_REACH_WIDE  2
#define FOG_UP_LAYERS   1       // its base may be a hair below its floor
#define FOG_UP_LADDER   3       // a storey: its top can be seen from a roof

// Whether a panel is the mission's to press. Pressing one runs
// XComInteractiveLevelActor.Interact -> Kismet_OnInteract ->
// RemoteEvent(InteractRemoteEvent): a panel with a remote event is wired into
// the level's script, which also switches it on and off
// (OnEnableInteractiveActor -> _Pristine, OnDisableInteractiveActor and a
// press -> _Inactive). So a wired panel that can still be pressed is an
// objective -- the transponder of the Newfoundland mission, which the
// 2026-10-01 (13:11) log had under Interactables only ("Panel, 9 north, 10
// west") once the objective "Reactivate the ship's transponder" was up.
// Unreadable state: the wiring alone decides.
//
// Except a joke. The train mission (DLC1_2_CnfndLight_Stream) has a button
// on the platform wired to Easter_Egg_Activated, whose Easter_Egg sequence
// plays RailroadsSound_Cue, toggles two emitters and switches the button off
// -- no objective, no reward. The 2026-10-01 (17:42) log counted it among
// the Objectives ("6 found" with four transponders and the turn counter).
// The event's name is the only mark it carries, so a panel whose event names
// an easter egg stays under Interactables.
static FieldSlot g_remote_event;

static int panel_objective(void* actor)
{
    const void* v;
    char ev[64] = "";
    if (!field_ptr(actor, "InteractRemoteEvent", &g_remote_event, sizeof(FName), &v) ||
        !name_to_string((const FName*)v, ev, sizeof ev) || !ev[0] || !_stricmp(ev, "None"))
        return 0;
    char state[64] = "";
    int live = !actor_state(actor, state, sizeof state) || strcmp(state, "_Pristine") == 0;
    char low[64];
    strcpy_s(low, sizeof low, ev);
    _strlwr_s(low, sizeof low);
    int joke = strstr(low, "easter") != NULL;
    static struct { void* actor; int live; } said[16];
    static int nsaid;
    int k;
    for (k = 0; k < nsaid && said[k].actor != actor; k++) {}
    if (k == nsaid || said[k].live != live) {
        if (k == nsaid && nsaid < 16) nsaid++;
        if (k < 16) { said[k].actor = actor; said[k].live = live; }
        char name[80] = "?";
        object_name(actor, name, sizeof name);
        logf_("world: panel %s sends %s, state %s -- %s\n", name, ev,
              state[0] ? state : "unread",
              !live ? "used or switched off" : joke ? "an easter egg, not an objective"
                                                    : "an objective");
    }
    return live && !joke;
}

static int arrow_on(const void* actor);

// What a comm array is to the player. XGBattle_SPCovertOpsExtraction.
// InitRadarArrays switches two of the map's arrays on with their objective
// visuals on the operative's extraction (mission type 5) -- a waypoint and an
// arrow, XComRadarArrayActor.SetActive -- and on the data recovery (type 6,
// XGBattle_SPCaptureAndHold) switches every one on with none, so there they
// are props a sighted player finds by looking. The operative's hack
// (OnExaltHackingArrayInteraction -> SetActive(false)) leaves one used. The
// 2026-10-04 (18:47) log, a data recovery, listed all four from the start,
// through the fog, as "Radar array": the game's own hint calls them "EXALT
// comm arrays". -1 unread (the bool mask unknown), 0 never switched on, 1 on,
// 2 used.
static int radar_array_state(void* actor)
{
    if (!props_mask_offset()) return -1;
    static const void* s_cls;
    static const void* s_active;
    static const void* s_ever;
    uint32_t class_off = props_class_offset();
    const void* cls = class_off && readable((uint8_t*)actor + class_off, sizeof(void*))
                          ? *(void* const*)((uint8_t*)actor + class_off) : NULL;
    if (cls && cls != s_cls) {
        s_cls = cls;
        s_active = object_field_prop(actor, "m_bActive");
        s_ever = object_field_prop(actor, "m_bWasEverActive");
    }
    int active = 0, ever = 0;
    if (!s_active || !s_ever || !props_read_object_bool(s_active, actor, &active) ||
        !props_read_object_bool(s_ever, actor, &ever))
        return -1;
    return active ? 1 : ever ? 2 : 0;
}

static void scan_describe_interactive(WorldActor* wa)
{
    void* actor = wa->actor;
    ScanItem it;
    memset(&it, 0, sizeof it);

    const void* v;
    int icon = 0;
    if (field_ptr(actor, "IconSocket", &g_icon, 1, &v))
        icon = *(const uint8_t*)v;

    // The comm array is the objective on the missions that have one, and it
    // is an interactive actor like any other -- so it is named and filed
    // before the icon gets a say. Only one with an arrow on it shows through
    // the fog (radar_array_state).
    int marked = 0;
    if (object_is_a(actor, "XComRadarArrayActor")) {
        int st = radar_array_state(actor);
        marked = arrow_on(actor);
        static struct { void* actor; int st, marked; } said[8];
        static int nsaid;
        int k;
        for (k = 0; k < nsaid && said[k].actor != actor; k++) {}
        if (k == nsaid || said[k].st != st || said[k].marked != marked) {
            if (k == nsaid && nsaid < 8) nsaid++;
            if (k < 8) { said[k].actor = actor; said[k].st = st; said[k].marked = marked; }
            char name[80] = "?";
            object_name(actor, name, sizeof name);
            logf_("world: comm array %s is %s, %s\n", name,
                  st < 0 ? "of unread state" : st == 0 ? "never switched on"
                  : st == 1 ? "on" : "used",
                  marked ? "an arrow on it -- listed through the fog" : "no arrow -- once seen");
        }
        // One never switched on is a decoy on the extraction map, no objective.
        if (st == 0 && !marked) return;
        it.kind = SCAN_OBJECTIVES;
        strncpy_s(it.name, sizeof it.name, T(WORLD_COMM_ARRAY), _TRUNCATE);
        if (st == 2) strncpy_s(it.detail, sizeof it.detail, T(WORLD_USED), _TRUNCATE);
    } else if (icon == ICON_WINDOW) {
        it.kind = SCAN_WINDOWS;
        strncpy_s(it.name, sizeof it.name, T(WORLD_WINDOW), _TRUNCATE);
    } else if (icon == ICON_BUTTON) {
        it.kind = panel_objective(actor) ? SCAN_OBJECTIVES : SCAN_INTERACT;
        strncpy_s(it.name, sizeof it.name, T(WORLD_PANEL), _TRUNCATE);
        // An objective panel is no more on the screen than a door is until
        // the fog lifts off it, unless an arrow points at it. The Furies
        // console (DLC2_3_Furies_Stream) gets its arrow only when a soldier
        // comes within sight of the XComSquadVisiblePoint beside it
        // (XComSquadVisiblePoint.Tick -> ConsoleSpotted ->
        // SeqAct_DisplayUIArrowPointingToActor_3), and the 2026-10-07 (19:30)
        // log listed it from the first turn, 68 tiles off inside the ship.
        if (it.kind == SCAN_OBJECTIVES) {
            marked = arrow_on(actor);
            static struct { void* actor; int marked; } said[16];
            static int nsaid;
            int k;
            for (k = 0; k < nsaid && said[k].actor != actor; k++) {}
            if (k == nsaid || said[k].marked != marked) {
                if (k == nsaid && nsaid < 16) nsaid++;
                if (k < 16) { said[k].actor = actor; said[k].marked = marked; }
                char name[80] = "?";
                object_name(actor, name, sizeof name);
                logf_("world: objective panel %s, %s\n", name,
                      marked ? "an arrow on it -- listed through the fog"
                             : "no arrow -- once seen");
            }
        }
    } else {
        it.kind = SCAN_DOORS;
        strncpy_s(it.name, sizeof it.name, T(WORLD_DOOR), _TRUNCATE);
        int shut = world_door_shut(actor, NULL, 0);
        if (shut >= 0)
            strncpy_s(it.detail, sizeof it.detail, T(shut ? WORLD_CLOSED : WORLD_OPEN), _TRUNCATE);
    }

    float world[3];
    if (!actor_location(actor, &g_ilact_loc, world)) return;
    if (!world_item_at(&it, world)) return;
    // An objective with an arrow on it is not held back: the arrow (and a
    // comm array's waypoint) shows through the fog.
    int through = it.kind == SCAN_OBJECTIVES && marked;
    if (!through && world_unseen(wa, &it, FOG_REACH_WALL, FOG_UP_LAYERS))
        return;
    world_keep(&it);
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

static void scan_describe_explosive(WorldActor* wa)
{
    void* action = wa->actor;
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
    scan_mesh_words(mesh, T(WORLD_EXPLOSIVE), it.name, sizeof it.name);

    float radius = 0.0f;
    if (field_ptr(action, "DamageRadius", &g_blast_radius, sizeof(float), &v))
        radius = *(const float*)v;
    int tiles = (int)(radius / CURSOR_TILE + 0.5f);
    int damaged = health > 0 && most > 0 && health < most * 3 / 4;
    size_t dw = 0;
    it.detail[0] = 0;
    if (tiles > 0) tpfmt_cat(it.detail, sizeof it.detail, &dw, WORLD_BLAST_TILES, tiles, tiles);
    else           tfmt_cat(it.detail, sizeof it.detail, &dw, WORLD_EXPLODES);
    if (damaged)   tfmt_cat(it.detail, sizeof it.detail, &dw, WORLD_DAMAGED);

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
    if (world_item_at(&it, world) && !world_unseen(wa, &it, FOG_REACH_WIDE, FOG_UP_LAYERS))
        world_keep(&it);
}

static FieldSlot g_window_loc;

static void scan_describe_window(WorldActor* wa)
{
    void* actor = wa->actor;
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_WINDOWS;
    strncpy_s(it.name, sizeof it.name, T(WORLD_WINDOW), _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_window_loc, world)) return;
    if (world_item_at(&it, world) && !world_unseen(wa, &it, FOG_REACH_WALL, FOG_UP_LAYERS))
        world_keep(&it);
}

static void scan_describe_ladder(WorldActor* wa)
{
    void* actor = wa->actor;
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_INTERACT;
    strncpy_s(it.name, sizeof it.name, T(WORLD_LADDER), _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_ladder_loc, world)) return;
    if (world_item_at(&it, world) && !world_unseen(wa, &it, FOG_REACH_WALL, FOG_UP_LADDER))
        world_keep(&it);
}

static void scan_describe_meld(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_MELD;
    strncpy_s(it.name, sizeof it.name, T(WORLD_MELD_CANISTER), _TRUNCATE);

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

    char timer[64] = "";
    if (turns > 0)
        tpfmt(timer, sizeof timer, WORLD_TURNS_LEFT, turns, turns);
    if (got || turns == 0) {
        it.unplaced = 1;
        strncpy_s(it.detail, sizeof it.detail, T(got ? WORLD_COLLECTED : WORLD_LOST), _TRUNCATE);
        world_keep(&it);
        return;
    }
    if (!seen) {
        it.unplaced = 1;
        // The HUD's counter shows "?" for its turns until it is seen.
        strncpy_s(it.detail, sizeof it.detail, T(WORLD_MELD_UNKNOWN), _TRUNCATE);
        world_keep(&it);
        return;
    }
    strncpy_s(it.detail, sizeof it.detail, timer, _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_meld_loc, world)) return;
    if (world_item_at(&it, world)) world_keep(&it);
}

// ---- the HUD's objective arrows --------------------------------------------
//
// Not every objective is something to press. The train mission's "Activate
// the train's drive system from the control room" has no button: the map's
// Kismet (DLC1_2_CnfndLight_Stream, Manage_Trigger_Volumes) fires
// ControlButtonActivated when a soldier stands in the control room's
// TriggerVolume, and all a sighted player gets is the yellow arrow that
// Manage_Control_Button points at PointInSpace_9 -- the 2026-10-01 (17:42)
// log has SetArrow "PointInSpace_9" as the objective came up, and
// "Objectives, 1 found" with only the turn counter in it.
//
// The arrows are SeqAct_DisplayUIArrowPointingToActor ->
// UISpecialMissionHUD_Arrows.AddArrowPointingAtActor, kept in arr3DArrows
// until RemoveArrowPointingAtActor; Update redraws each one every frame with
// SetArrow(KActor.Name, ...), which is where main.c finds the panel. They are
// drawn through the fog, so nothing here is held back for it.
//
// T3DArrowActor is { Vector Offset; Actor KActor; byte arrowState; int
// arrowCounter; }: 12 + 4 + 1 (+3) + 4. Nothing here reads a struct's size
// from the game, so the stride is UE3's layout, and an element whose KActor
// is not a live object ends the read.
#define ARROW_STRIDE    24
#define ARROW_ACTOR     12
#define ARROWS_MAX      16

static void*     g_arrows;
static FieldSlot g_arrows_3d, g_arrow_loc;

void world_arrows_note(void* panel)
{
    if (panel == g_arrows) return;
    g_arrows = panel;
    char name[80] = "?";
    object_name(panel, name, sizeof name);
    logf_("world: objective arrows drawn by %s\n", name);
}

// Where a soldier can stand for an arrow. The point hangs above what it marks
// (PointInSpace_9 is 160 units up), and what it marks can be solid: the
// 2026-10-01 (17:59) log had Home take the cursor to 20, 11 at 160.0 --
// GetFloorZForPosition had given the height back, finding no floor -- and
// "Blocked." there and on every tile round it but 21, 11, where the player
// stood to set the train going. So the place listed is the nearest tile, out
// to ARROW_REACH, with a layer the game takes as a move's end
// (IsPositionOnFloorAndValidDestination, as the numpad's "Blocked." asks it)
// at or below the point, down to ARROW_DOWN layers, else the layer above; its
// exact floor from the top of that layer. Nearest on the grid first, and in
// a tile the layer nearest the point. 0 when nothing is found: the point is
// kept.
#define ARROW_REACH 3
#define ARROW_DOWN  4

static int arrow_stand(const CursorGrid* g, const float* at, float* out, int* moved)
{
    void* w = cursor_world();
    PositionTestFn standable = w ? (PositionTestFn)tile_vfn(w, g_tile_slot_standable) : NULL;
    if (!standable) return 0;
    int cx = grid_x(g, at[0]), cy = grid_y(g, at[1]), top = grid_layer(g, at[2]);
    for (int r = 0; r <= ARROW_REACH; r++) {
        int found = 0, best_d2 = 0;
        float best[3];
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) != r && abs(dy) != r) continue;     // this ring only
                int tx = cx + dx, ty = cy + dy;
                if (tx < 0 || ty < 0 || tx >= g->num_x || ty >= g->num_y) continue;
                int d2 = dx * dx + dy * dy;
                if (found && d2 >= best_d2) continue;
                for (int k = 0; k <= ARROW_DOWN + 1; k++) {
                    int tz = k <= ARROW_DOWN ? top - k : top + 1;
                    if (tz < 0 || (g->num_z > 0 && tz >= g->num_z)) continue;
                    float pos[3] = { grid_centre_x(g, tx), grid_centre_y(g, ty),
                                     grid_layer_middle(g, tz) };
                    if (!standable(w, NULL, pos)) continue;
                    pos[2] = aim_floor_exact(w, pos, grid_layer_bottom(g, tz));
                    memcpy(best, pos, sizeof best);
                    best_d2 = d2;
                    found = 1;
                    break;
                }
            }
        if (found) {
            memcpy(out, best, sizeof best);
            *moved = r > 0;
            return 1;
        }
    }
    return 0;
}

// Whether one of the HUD's arrows points at this actor.
static int arrow_on(const void* actor)
{
    if (!g_arrows || !unit_is_live(g_arrows)) return 0;
    const void* v;
    if (!field_ptr(g_arrows, "arr3DArrows", &g_arrows_3d, sizeof(FArray), &v)) return 0;
    const FArray* a = (const FArray*)v;
    if (a->Num <= 0 || a->Num > ARROWS_MAX ||
        !readable(a->Data, (size_t)a->Num * ARROW_STRIDE))
        return 0;
    for (int i = 0; i < a->Num; i++)
        if (*(void* const*)((const uint8_t*)a->Data + i * ARROW_STRIDE + ARROW_ACTOR) == actor)
            return 1;
    return 0;
}

// The capture zone an arrow marks, by the counter's name for it. On the data
// recovery the arrows point at each XComCapturePointVolume's
// m_kActorBeingCaptured (UpdateIndicatorArrow), and the HUD names the volume
// with m_iCaptureSequenceIndex 0 ENCODER, any other TRANSMITTER
// (UISpecialMissionHUD_CapturePointStats.UpdatePanel). The 2026-10-04 (18:47)
// log listed both as "Objective marker", which said neither which was which
// nor that it is a zone to hold.
static FieldSlot g_cp_volume, g_cp_index;

static const char* capture_zone_name(void* actor)
{
    if (!object_is_a(actor, "XComCapturePointActor")) return NULL;
    const void* v;
    if (!field_ptr(actor, "m_kCapturePointVolume", &g_cp_volume, sizeof(void*), &v)) return NULL;
    void* vol = *(void* const*)v;
    if (!vol || !unit_is_live(vol)) return NULL;
    if (!field_ptr(vol, "m_iCaptureSequenceIndex", &g_cp_index, sizeof(int32_t), &v)) return NULL;
    return T(*(const int32_t*)v == 0 ? WORLD_ENCODER_ZONE : WORLD_TRANSMITTER_ZONE);
}

static void scan_add_arrows(void)
{
    if (!g_arrows || !unit_is_live(g_arrows)) return;
    const void* v;
    if (!field_ptr(g_arrows, "arr3DArrows", &g_arrows_3d, sizeof(FArray), &v)) return;
    const FArray* a = (const FArray*)v;
    // Logged when the set of arrows changes, not every refresh.
    static void* said[ARROWS_MAX];
    static int   nsaid = -1;
    if (a->Num <= 0) { nsaid = 0; return; }
    if (a->Num > ARROWS_MAX || !readable(a->Data, (size_t)a->Num * ARROW_STRIDE))
        return;
    int changed = a->Num != nsaid;

    for (int i = 0; i < a->Num; i++) {
        const uint8_t* e = (const uint8_t*)a->Data + i * ARROW_STRIDE;
        void* actor = *(void* const*)(e + ARROW_ACTOR);
        if (!actor || !unit_is_live(actor)) {
            logf_("world: arrow %d of %d points at %p, not a live object -- stopped\n",
                  i + 1, a->Num, actor);
            break;
        }
        if (said[i] != actor) { said[i] = actor; changed = 1; }
        // A canister's arrow: the Meld category has it already.
        if (object_is_a(actor, "XComMeldContainerActor")) continue;

        float world[3];
        if (!actor_location(actor, &g_arrow_loc, world)) continue;
        const float* off = (const float*)e;
        for (int k = 0; k < 3; k++) world[k] += off[k];

        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_OBJECTIVES;
        const char* zone = capture_zone_name(actor);
        strncpy_s(it.name, sizeof it.name, zone ? zone : T(WORLD_OBJECTIVE_MARKER), _TRUNCATE);
        if (!world_item_at(&it, world)) continue;

        // The transponders' arrows hang over their panels, which are listed
        // already (panel_objective): one entry for the place, not two.
        int dup = 0;
        for (int j = 0; j < g_world_n && !dup; j++)
            dup = (g_world[j].kind == SCAN_OBJECTIVES || g_world[j].kind == SCAN_MELD) &&
                  g_world[j].tz == it.tz &&
                  abs(g_world[j].tx - it.tx) <= 1 && abs(g_world[j].ty - it.ty) <= 1;
        if (dup) {
            if (changed) {
                char name[80] = "?";
                object_name(actor, name, sizeof name);
                logf_("world: arrow %d of %d at %s, tile %d, %d floor %d -- an objective "
                      "listed there already\n", i + 1, a->Num, name, it.tx, it.ty, it.tz);
            }
            continue;
        }

        // Then to where a soldier can stand for it.
        float stand[3];
        int found = 0, moved = 0;
        GUARDED("world: arrow stand",
                found = arrow_stand(&g_world_grid, world, stand, &moved), found = 0);
        if (found) {
            ScanItem at = it;
            if (world_item_at(&at, stand)) {
                it = at;
                if (moved) strncpy_s(it.detail, sizeof it.detail, T(WORLD_NEAREST_STAND), _TRUNCATE);
            } else {
                found = 0;
            }
        }
        if (changed) {
            char name[80] = "?";
            object_name(actor, name, sizeof name);
            logf_("world: arrow %d of %d at %s (%.1f, %.1f, %.1f) -- listed on %d, %d floor %d "
                  "at %.1f, %s\n", i + 1, a->Num, name, world[0], world[1], world[2],
                  it.tx, it.ty, it.tz, it.feet,
                  !found ? "no tile to stand on found, the point itself"
                  : moved ? "the nearest tile to stand on" : "its own tile");
        }
        world_keep(&it);
    }
    nsaid = a->Num;
}

// What the scanner would say about each actor it is holding, worked out
// afresh: the tiles are relative to a grid, and a canister's countdown is
// relative to the turn.
static void scan_world_items(void)
{
    // The squad's own tiles tell fog.c what its bytes mean (fog_calibrate).
    // The player is kept across a soldier switch, when squad_player is
    // briefly nothing (scan_squad_player has the same).
    static void* squad_was;
    void* squad = squad_player();
    if (squad) squad_was = squad; else squad = squad_was;
    int tiles[UNIT_MAX][3];
    const char* names[UNIT_MAX];
    int nsquad = 0;
    for (int i = 0; i < g_nunits && squad; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || !s.friendly) continue;
        tiles[nsquad][0] = grid_x(&g_world_grid, s.loc[0]);
        tiles[nsquad][1] = grid_y(&g_world_grid, s.loc[1]);
        tiles[nsquad][2] = grid_floor_layer(&g_world_grid, s.loc[2] - NAVH_LIFT);
        names[nsquad++] = s.who->name;
    }
    fog_calibrate(&g_world_grid, (const int (*)[3])tiles, names, nsquad);

    static int hidden_said = -1;
    g_fog_hidden = 0;
    g_world_n = 0;
    g_blast_owner_n = 0;
    for (int i = 0; i < g_wactor_n; i++) {
        switch (g_wactors[i].kind) {
        case 0:  scan_describe_interactive(&g_wactors[i]);      break;
        case 1:  scan_describe_ladder(&g_wactors[i]);           break;
        case 2:  scan_describe_meld(g_wactors[i].actor);        break;
        case 3:  scan_describe_window(&g_wactors[i]);           break;
        case 5:  break;     // units are scan_add_flagless's, not items
        default: scan_describe_explosive(&g_wactors[i]);        break;
        }
    }
    // After the actors, so an arrow over a listed panel is known as one.
    scan_add_arrows();
    // Every refresh would be a line a second with the door sounds on, so
    // only when the count moves: each actor the fog lets go is logged on its
    // own by world_unseen.
    if (g_fog_hidden != hidden_said) {
        hidden_said = g_fog_hidden;
        logf_("fog: %d level actors kept out, not yet seen\n", g_fog_hidden);
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
    g_wactors[g_wactor_n].revealed = 0;
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

// The doors whose tile is within a tile of (tx, ty), on storey tz, as of the
// last world_refresh: the actors themselves, for move.c to read their state.
int world_doors_near(int tx, int ty, int tz, void** out, int max)
{
    int n = 0;
    for (int i = 0; i < g_wactor_n && n < max; i++) {
        void* a = g_wactors[i].actor;
        if (g_wactors[i].kind != 0 || !objects_live(a) ||
            !object_is_a(a, "XComInteractiveLevelActor"))
            continue;
        const void* v;
        int icon = 0;
        if (field_ptr(a, "IconSocket", &g_icon, 1, &v)) icon = *(const uint8_t*)v;
        if (icon == ICON_WINDOW || icon == ICON_BUTTON ||
            object_is_a(a, "XComRadarArrayActor"))
            continue;
        float w[3];
        if (!actor_location(a, &g_ilact_loc, w)) continue;
        int x = grid_x(&g_world_grid, w[0]), y = grid_y(&g_world_grid, w[1]);
        if (abs(x - tx) > 1 || abs(y - ty) > 1 || floor_of(w) != tz) continue;
        out[n++] = a;
    }
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
