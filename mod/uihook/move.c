// A confirmed move, followed to its end. See move.h.
//
// The 2026-09-28 (18:43) log: Hagen was sent up to 15, 40, floor 2 of 2, the
// path built and confirmed ("path to 15, 40 (47.0, 1583.0, 587.6) built"),
// and the scanner found him afterwards on 15, 42 at ground height -- the game
// agreed, pathing to that tile came back NONE with him on it. The mission's
// load before had the same: sent to the roof at 14, 39, found on 14, 42. Both
// times the next soldier was selected straight after the click, with no cover
// update from his flag, where a move that arrived had one first. Nothing in
// the log said what the game had been told to do or where he stopped, and
// nothing told the player until they went looking.
//
// So the path is read as the click is sent -- XComTacticalController.
// ParsePath turns exactly this XComPathingPawn.Path into the unit's action
// queue on the click's release, and ExecutingSchedule's BeginState clears it
// soon after -- and the soldier is watched until their current action is idle
// again and their pawn has stopped.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "move.h"
#include "game.h"
#include "units.h"
#include "cursor.h"
#include "nav.h"
#include "where.h"
#include "names.h"
#include "speech.h"
#include "history.h"
#include "numpad.h"
#include "log.h"
#include "world.h"
#include "props.h"

// XComPathData's PathPoint: Vector Position (12), ETraversalType Traversal (a
// byte, padded to 4), Actor Actor (4). A traversal past the enum's end means
// the layout is not this, and the path is not read.
#define PATH_POINT_SIZE   20
#define PATH_POINTS_MAX   96
#define TRAVERSAL_MAX     15

// How far the pawn may stand from the path's end in height and still be on
// it: a pawn stands NAVH_LIFT over its floor, the path's points the same, and
// a storey is 192. Half a layer either way.
#define MOVE_Z_SLACK      48.0f

// The move is over once the unit's action is idle and the pawn has been still
// this long; or still this long whatever the action says; or never started.
#define MOVE_SETTLE_MS    700
#define MOVE_STILL_MS     10000
#define MOVE_START_MS     6000
#define MOVE_GIVE_UP_MS   90000

// ETraversalType (XComWorldData), for the log.
static const char* const TRAVERSAL[TRAVERSAL_MAX] = {
    "none", "normal", "climb over", "climb onto", "ladder", "drop down", "grapple",
    "landing", "break window", "kick door", "wall climb", "jump up", "ramp",
    "break wall", "unreachable",
};

static struct {
    int       on;
    void*     unit;
    void*     pawn;
    char      name[64];
    int       tx, ty;         // where the path ends, or the numpad's target
    float     z;              // the pawn's height there (floor + NAVH_LIFT)
    float     last[3];
    int       started;
    ULONGLONG at, moved_at;
    char      action[64];
} g_move;

static FieldSlot g_mv_ppawn, g_mv_path, g_mv_points, g_mv_loc, g_mv_action;

static int pawn_loc(void* pawn, float* out)
{
    const void* v;
    if (!field_ptr(pawn, "Location", &g_mv_loc, 3 * sizeof(float), &v)) return 0;
    memcpy(out, v, 3 * sizeof(float));
    return 1;
}

// The unit's m_kCurrAction by name, "" when there is none.
static void unit_action(void* unit, char* out, size_t out_sz)
{
    const void* v;
    out[0] = 0;
    if (!field_ptr(unit, "m_kCurrAction", &g_mv_action, sizeof(void*), &v)) return;
    void* action = *(void* const*)v;
    if (!action || !unit_is_live(action) || !object_name(action, out, out_sz)) out[0] = 0;
}

// The points of the path a pathing pawn holds (XComPathingPawn.Path, an
// XComPath, whose own Path is the array). How many, 0 when there is none or
// it cannot be read; -1 when there are too many or they are unreadable.
static int path_points(void* ppawn, const uint8_t** data)
{
    const void* v;
    if (!ppawn || !unit_is_live(ppawn)) return 0;
    if (!field_ptr(ppawn, "Path", &g_mv_path, sizeof(void*), &v)) return 0;
    void* path = *(void* const*)v;
    if (!path || !unit_is_live(path)) return 0;
    if (!field_ptr(path, "Path", &g_mv_points, sizeof(FArray), &v)) return 0;
    const FArray* a = (const FArray*)v;
    if (a->Num <= 0) return 0;
    if (a->Num > PATH_POINTS_MAX ||
        !readable(a->Data, (size_t)a->Num * PATH_POINT_SIZE))
        return -1;
    *data = (const uint8_t*)a->Data;
    return a->Num;
}

int path_end(void* ppawn, float* end)
{
    const uint8_t* data = NULL;
    int n = path_points(ppawn, &data);
    if (n <= 0) return 0;
    const uint8_t* last = data + (size_t)(n - 1) * PATH_POINT_SIZE;
    if (last[12] >= TRAVERSAL_MAX) return 0;    // not the layout assumed
    memcpy(end, last, 3 * sizeof(float));
    return 1;
}

// Logs the path the unit's pathing pawn holds, one point after another with
// how the next leg is crossed (ParsePath switches on GetTraversalType(Index)
// to go from point Index to Index + 1), and the actor a leg uses -- the
// ladder, the window. *end gets the last point. 0 when it cannot be read.
static int move_path_log(void* unit, float* end)
{
    const void* v;
    if (!field_ptr(unit, "m_kPathingPawn", &g_mv_ppawn, sizeof(void*), &v)) return 0;
    void* ppawn = *(void* const*)v;
    const uint8_t* data = NULL;
    int n = path_points(ppawn, &data);
    if (n == 0) {
        logf_("move: the pathing pawn's path is empty\n");
        return 0;
    }
    if (n < 0) {
        logf_("move: the path's points cannot be read\n");
        return 0;
    }
    CursorGrid g;
    if (!cursor_grid(&g)) return 0;

    char line[3072];
    size_t used = 0;
    line[0] = 0;
    FArray whole = { (void*)data, n, n };
    const FArray* a = &whole;
    for (int i = 0; i < a->Num; i++) {
        const uint8_t* p = data + (size_t)i * PATH_POINT_SIZE;
        const float* pos = (const float*)p;
        uint8_t t = p[12];
        void* actor = *(void* const*)(p + 16);
        if (t >= TRAVERSAL_MAX) {
            logf_("move: path point %d has traversal %u -- PathPoint is not %d bytes\n",
                  i, t, PATH_POINT_SIZE);
            return 0;
        }
        char who[96] = "";
        if (actor && unit_is_live(actor)) {
            char an[80];
            if (object_name(actor, an, sizeof an))
                _snprintf_s(who, sizeof who, _TRUNCATE, " (%s)", an);
        }
        int w = _snprintf_s(line + used, sizeof line - used, _TRUNCATE,
                            "%s%d, %d z %.1f%s%s%s", i ? " | " : "",
                            grid_x(&g, pos[0]), grid_y(&g, pos[1]), pos[2],
                            i + 1 < a->Num ? " -> " : "",
                            i + 1 < a->Num ? TRAVERSAL[t] : "", who);
        if (w < 0) break;
        used += (size_t)w;
    }
    logf_("move: path of %d points: %s\n", a->Num, line);
    memcpy(end, data + (size_t)(a->Num - 1) * PATH_POINT_SIZE, 3 * sizeof(float));
    return 1;
}

// A confirm held back because the path stops short, so a second press on the
// same target within HOLD_MS goes anyway.
#define HOLD_MS 5000
static struct { int tx, ty; float floor; ULONGLONG at; } g_hold;

static void offset_words(int dx, int dy, char* out, size_t out_sz);

// Whether a door stands at a path's end: on its tile or the next one, on its
// storey -- and each such door's state, logged. The 19:19 log: every path
// into the building at 14, 40 ended on 14, 42 or 15, 42, and the scanner had
// "Door, here." on 15, 42. The 19:32 log settled what that is: a path to a
// tile behind a closed door ends at the door (Moletta, 11, 45 to 14, 40,
// stopped on 14, 42), and even from the door tile it still did ("14, 41 ends
// on 14, 42") until the player pressed V -- XComTacticalInput.Key_V ->
// PerformAction -> XGUnit.PerformInteract, the path being empty with the
// cursor on the soldier -- after which "14, 42 -> 14, 40" went through. The
// door, XComInteractiveLevelActor_1, is not touch-activated (bTouchActivated
// 0, so _Pristine.Bump does not break it), never used, and blocks. Doors a
// move goes through on its own are the kind whose path carries a kick-door
// step (KickDoorTraversalPoint, an XComDestructibleActor).
static FieldSlot g_door_sock, g_door_points;

static void door_bool(void* door, const char* name, char* out, size_t out_sz)
{
    const void* prop = object_field_prop(door, name);
    int b = -1;
    if (!prop || !props_read_object_bool(prop, (const uint8_t*)door, &b)) b = -1;
    size_t used = strlen(out);
    _snprintf_s(out + used, out_sz - used, _TRUNCATE, " %s %d", name, b);
}

static int door_at_end(const CursorGrid* g, const float* end)
{
    int ok = 0;
    GUARDED("move: doors", ok = world_refresh(g), ok = 0);
    if (!ok) return 0;
    int ex = grid_x(g, end[0]), ey = grid_y(g, end[1]);
    float feet[3] = { end[0], end[1], end[2] - NAVH_LIFT };
    void* doors[4];
    int n = world_doors_near(ex, ey, floor_of(feet), doors, 4);
    for (int i = 0; i < n; i++) {
        void* d = doors[i];
        char name[80] = "?", cls[80] = "?", sock[64] = "-", flags[256] = "";
        object_name(d, name, sizeof name);
        object_class_name(d, cls, sizeof cls);
        const void* v;
        if (field_ptr(d, "ActiveSocketName", &g_door_sock, sizeof(FName), &v))
            name_to_string((const FName*)v, sock, sizeof sock);
        int points = -1;
        if (field_ptr(d, "InteractionPoints", &g_door_points, sizeof(FArray), &v))
            points = ((const FArray*)v)->Num;
        door_bool(d, "bTouchActivated", flags, sizeof flags);
        door_bool(d, "bWasTouchActivated", flags, sizeof flags);
        door_bool(d, "bPlayingAnim", flags, sizeof flags);
        door_bool(d, "bCollideActors", flags, sizeof flags);
        door_bool(d, "bBlockActors", flags, sizeof flags);
        door_bool(d, "bHidden", flags, sizeof flags);
        float w[3] = { 0 };
        if (field_ptr(d, "Location", &g_mv_loc, 3 * sizeof(float), &v)) memcpy(w, v, sizeof w);
        logf_("move: the path ends by door %s (%s) at %.1f, %.1f, %.1f, tile %d, %d:%s, "
              "socket %s, %d interaction sockets\n", name, cls, w[0], w[1], w[2],
              grid_x(g, w[0]), grid_y(g, w[1]), flags, sock, points);
    }
    return n > 0;
}

// For the log only, when a confirm is held: whether the game counts the
// target tile as occupied, and the level objects near the path's end. The
// 19:46 log: Moletta on 17, 42, every path to 17, 41 (inside, "Out of
// sight") ending on 17, 42 at cost 2, no door in reach and V finding nothing.
// A unit the squad cannot see standing on the target would do that -- a path
// never ends on an occupied tile -- and a sighted player sees no more than the
// path stopping short, so it is logged and never said.
static void held_why(const CursorGrid* g, int tx, int ty, float floor, const float* end)
{
    void* world = cursor_world();
    TileTestFn occupied = world ? (TileTestFn)tile_vfn(world, g_tile_slot_occupied) : NULL;
    int occ = occupied ? occupied(world, NULL, tx, ty, grid_floor_layer(g, floor)) != 0 : -1;

    char nearby[512] = "";
    size_t used = 0;
    int n = 0;
    const ScanItem* items = world_items(&n);
    int ex = grid_x(g, end[0]), ey = grid_y(g, end[1]);
    for (int i = 0; i < n; i++) {
        const ScanItem* it = &items[i];
        if (it->unplaced || abs(it->tx - ex) > 1 || abs(it->ty - ey) > 1) continue;
        int w = _snprintf_s(nearby + used, sizeof nearby - used, _TRUNCATE, "%s%s on %d, %d (storey %d)",
                            used ? ", " : "", it->name, it->tx, it->ty, it->tz);
        if (w < 0) break;
        used += (size_t)w;
    }
    logf_("move: held -- target %d, %d occupied %d; near the path's end: %s\n", tx, ty, occ,
          nearby[0] ? nearby : "nothing the scanner knows");
}

int move_confirmed(int tx, int ty, float floor)
{
    memset(&g_move, 0, sizeof g_move);
    void* unit = soldier_unit();
    void* pawn = unit ? unit_pawn(unit) : NULL;
    float loc[3];
    if (!pawn || !unit_is_live(pawn) || !pawn_loc(pawn, loc)) {
        logf_("move: no soldier to watch\n");
        return 1;
    }
    CursorGrid g;
    if (!cursor_grid(&g)) return 1;

    UnitName* u = unit_by_unit(unit);
    if (u) unit_label(u, g_move.name, sizeof g_move.name);
    if (!g_move.name[0]) strcpy_s(g_move.name, sizeof g_move.name, "The soldier");

    float end[3];
    int have = 0;
    GUARDED("move: path", have = move_path_log(unit, end), have = 0);
    if (have) {
        g_move.tx = grid_x(&g, end[0]);
        g_move.ty = grid_y(&g, end[1]);
        g_move.z = end[2];
        // The click performs the pathing pawn's path, not the numpad's
        // target. Where the two part, the pathfinder could not get there and
        // built a path to the nearest place it could: the 18:58 log, Hagen
        // sent to floor 2 of 14, 40 and walking to 14, 42 outside. The click
        // is held back and the player told where the path stops; the same
        // confirm again goes there.
        if (tx >= 0 && (g_move.tx != tx || g_move.ty != ty ||
                        fabsf(end[2] - NAVH_LIFT - floor) > PATH_END_Z_SLACK)) {
            ULONGLONG now = GetTickCount64();
            int again = g_hold.at && now - g_hold.at <= HOLD_MS && g_hold.tx == tx &&
                        g_hold.ty == ty && g_hold.floor == floor;
            logf_("move: the path ends on %d, %d (z %.1f), not on the target %d, %d "
                  "floor %.1f -- %s\n", g_move.tx, g_move.ty, end[2], tx, ty, floor,
                  again ? "confirmed again, going" : "held");
            if (!again) {
                g_hold.tx = tx;
                g_hold.ty = ty;
                g_hold.floor = floor;
                g_hold.at = now;
                char where[64], words[48], off[64], say[256];
                where_is(g_move.tx, g_move.ty, end[2] - NAVH_LIFT, where, sizeof where);
                offset_words(g_move.tx - tx, g_move.ty - ty, words, sizeof words);
                if (words[0])
                    _snprintf_s(off, sizeof off, _TRUNCATE, "%s of the target", words);
                else
                    strcpy_s(off, sizeof off, end[2] - NAVH_LIFT < floor ? "Below the target"
                                                                         : "Above the target");
                int door = door_at_end(&g, end);
                GUARDED("move: held why", held_why(&g, tx, ty, floor, end));
                if (door)
                    _snprintf_s(say, sizeof say, _TRUNCATE,
                                "The path stops at a closed door, %d, %d. "
                                "Numpad 0 again to go to it, then open it with V.",
                                g_move.tx, g_move.ty);
                else
                    _snprintf_s(say, sizeof say, _TRUNCATE,
                                "The path stops short. %s%s%s, %d, %d. "
                                "Numpad 0 again to go there.",
                                where, where[0] ? " " : "", off, g_move.tx, g_move.ty);
                speech_say_now(say);
                return 0;
            }
        }
    } else if (tx >= 0) {
        g_move.tx = tx;
        g_move.ty = ty;
        g_move.z = floor + NAVH_LIFT;
    } else {
        return 1;
    }
    g_hold.at = 0;
    g_move.unit = unit;
    g_move.pawn = pawn;
    memcpy(g_move.last, loc, sizeof loc);
    g_move.at = g_move.moved_at = GetTickCount64();
    unit_action(unit, g_move.action, sizeof g_move.action);
    g_move.on = 1;
    logf_("move: %s from %d, %d (z %.1f) to %d, %d (z %.1f), action %s\n", g_move.name,
          grid_x(&g, loc[0]), grid_y(&g, loc[1]), loc[2], g_move.tx, g_move.ty, g_move.z,
          g_move.action[0] ? g_move.action : "none");
    return 1;
}

// "2 south, 1 east" -- the scanner's words for an offset (+y is north).
static void offset_words(int dx, int dy, char* out, size_t out_sz)
{
    char ns[24] = "", ew[24] = "";
    if (dy) _snprintf_s(ns, sizeof ns, _TRUNCATE, "%d %s", abs(dy), dy > 0 ? "north" : "south");
    if (dx) _snprintf_s(ew, sizeof ew, _TRUNCATE, "%d %s", abs(dx), dx > 0 ? "east" : "west");
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", ns, ns[0] && ew[0] ? ", " : "", ew);
}

static void move_finish(const float* loc, const char* why)
{
    g_move.on = 0;
    CursorGrid g;
    if (!cursor_grid(&g)) return;
    int ax = grid_x(&g, loc[0]), ay = grid_y(&g, loc[1]);
    float feet = loc[2] - NAVH_LIFT;
    int dx = ax - g_move.tx, dy = ay - g_move.ty;
    float dz = loc[2] - g_move.z;
    char where[64];
    where_is(ax, ay, feet, where, sizeof where);

    if (!dx && !dy && fabsf(dz) <= MOVE_Z_SLACK) {
        logf_("move: %s arrived on %d, %d (z %.1f)%s%s [%s]\n", g_move.name, ax, ay,
              loc[2], where[0] ? " -- " : "", where, why);
        return;
    }
    logf_("move: %s stopped short on %d, %d (z %.1f), the path ended on %d, %d "
          "(z %.1f); action %s%s%s [%s]\n", g_move.name, ax, ay, loc[2], g_move.tx,
          g_move.ty, g_move.z, g_move.action[0] ? g_move.action : "none",
          where[0] ? " -- " : "", where, why);

    char off[64];
    if (dx || dy) {
        char words[48];
        offset_words(dx, dy, words, sizeof words);
        _snprintf_s(off, sizeof off, _TRUNCATE, "%s of the target", words);
    } else {
        strcpy_s(off, sizeof off, dz < 0 ? "Below the target" : "Above the target");
    }
    char say[256];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s stopped short. %s%s%s, %d, %d.",
                g_move.name, where, where[0] ? " " : "", off, ax, ay);
    history_add(say);
    if (g_speak) speech_say(say);
}

// ---- what the soldier can open from where they stand -------------------------
//
// XGUnit.UpdateInteractClaim, run as a move ends, on a switch and after an
// interaction, keeps in m_arrInteractPoints every interaction point within
// reach whose actor CanInteract -- a closed door, a panel, a Meld canister's
// lid. V acts on the first (XComTacticalInput.Key_V -> PerformAction ->
// XGUnit.PerformInteract), but only while the soldier has no path: Key_V
// asks XGAction_Path.IsEmpty first, and a path is cleared only with the
// cursor on the soldier -- Shift+Home. The 19:32 log: Moletta on 14, 42, the
// door shut, every path in stopping at it; V with the cursor on her opened it.
//
// XComWorldData's XComInteractPoint: Location (12), Rotation (12),
// InteractiveActor (4) at 24, InteractSocketName (8), ModifyTileStaticFlags
// (4) -- 40 bytes. An actor that is not an XComInteractiveLevelActor means
// the layout is not this, and nothing is said.
#define INTERACT_POINT_SIZE 40
#define INTERACT_ACTOR_OFF  24
#define INTERACT_POLL_MS    200

static FieldSlot g_mv_ipoints, g_mv_icon, g_mv_owner;
static struct { void* unit; void* actor; ULONGLONG next; int v_down; } g_ia;

// What the actor is, for the words: the level designer's icon
// (XComInteractiveLevelActor.IconSocket: door 0, window 1, button 2), the
// radar array by class. 0 for a Meld canister's lid, which the canister's own
// prompt says (main.c, the world messages).
static int interact_words(void* actor, const char** what, const char** verb)
{
    const void* v;
    if (field_ptr(actor, "Owner", &g_mv_owner, sizeof(void*), &v)) {
        void* owner = *(void* const*)v;
        if (owner && unit_is_live(owner) && object_is_a(owner, "XComMeldContainerActor"))
            return 0;
    }
    int icon = 0;
    if (field_ptr(actor, "IconSocket", &g_mv_icon, 1, &v)) icon = *(const uint8_t*)v;
    *verb = "open";
    if (object_is_a(actor, "XComRadarArrayActor")) { *what = "Radar array"; *verb = "use"; }
    else if (icon == 1) *what = "Window";
    else if (icon == 2) { *what = "Panel"; *verb = "use"; }
    else *what = "Door";
    return 1;
}

static void interact_poll(void)
{
    ULONGLONG now = GetTickCount64();

    // V as it goes down, with what the soldier could act on: the next log
    // says whether a press had anything to open.
    int v_down = game_has_focus() && (GetAsyncKeyState('V') & 0x8000) != 0;
    if (v_down && !g_ia.v_down)
        logf_("move: V pressed -- %s\n", g_ia.actor ? "something to open in reach" : "nothing in reach");
    g_ia.v_down = v_down;

    if (now < g_ia.next) return;
    g_ia.next = now + INTERACT_POLL_MS;

    void* unit = soldier_unit();
    void* actor = NULL;
    int n = 0;
    const void* v;
    if (unit && field_ptr(unit, "m_arrInteractPoints", &g_mv_ipoints, sizeof(FArray), &v)) {
        const FArray* a = (const FArray*)v;
        n = a->Num;
        if (n > 0 && n <= 16 && readable(a->Data, (size_t)n * INTERACT_POINT_SIZE))
            actor = *(void* const*)((const uint8_t*)a->Data + INTERACT_ACTOR_OFF);
    }
    if (actor && (!unit_is_live(actor) || !object_is_a(actor, "XComInteractiveLevelActor")))
        actor = NULL;
    if (unit == g_ia.unit && actor == g_ia.actor) return;
    int same_unit = unit == g_ia.unit;
    void* was = g_ia.actor;
    g_ia.unit = unit;
    g_ia.actor = actor;
    if (!actor) {
        if (same_unit && was) logf_("move: nothing to open in reach any more\n");
        return;
    }

    const char* what = "Door";
    const char* verb = "open";
    char aname[80] = "?";
    object_name(actor, aname, sizeof aname);
    if (!interact_words(actor, &what, &verb)) {
        logf_("move: in reach of %s, a Meld canister's -- left to its own prompt\n", aname);
        return;
    }
    char name[64] = "";
    UnitName* u = unit_by_unit(unit);
    if (u) unit_label(u, name, sizeof name);
    logf_("move: %s in reach of %s (%s), %d interaction point%s\n",
          name[0] ? name : "the soldier", aname, what, n, n == 1 ? "" : "s");
    char say[160];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s here. Shift+Home, then V to %s it.", what, verb);
    history_add(say);
    if (g_speak) speech_say(say);
}

void move_poll(void)
{
    GUARDED("move: interact", interact_poll());
    if (!g_move.on) return;
    ULONGLONG now = GetTickCount64();
    float loc[3];
    if (!unit_is_live(g_move.unit) || !unit_is_live(g_move.pawn) ||
        !pawn_loc(g_move.pawn, loc)) {
        logf_("move: %s is gone -- not watched\n", g_move.name);
        g_move.on = 0;
        return;
    }
    float d = fabsf(loc[0] - g_move.last[0]) + fabsf(loc[1] - g_move.last[1]) +
              fabsf(loc[2] - g_move.last[2]);
    if (d > 1.0f) {
        g_move.started = 1;
        g_move.moved_at = now;
        memcpy(g_move.last, loc, sizeof loc);
    }

    // Each action the unit goes through, as it starts: the ladder, the
    // climb, and whatever it was doing when it stopped.
    char action[64];
    unit_action(g_move.unit, action, sizeof action);
    if (strcmp(action, g_move.action) != 0) {
        CursorGrid g;
        if (cursor_grid(&g))
            logf_("move: %s -- %s on %d, %d (z %.1f)\n", g_move.name,
                  action[0] ? action : "no action", grid_x(&g, loc[0]), grid_y(&g, loc[1]),
                  loc[2]);
        strcpy_s(g_move.action, sizeof g_move.action, action);
    }
    // XGAction_Path extends XGAction_Idle: the soldier waiting for orders.
    int idle = !action[0] || strncmp(action, "XGAction_Idle", 13) == 0 ||
               strncmp(action, "XGAction_Path", 13) == 0;

    if (g_move.started) {
        ULONGLONG still = now - g_move.moved_at;
        if (idle && still >= MOVE_SETTLE_MS) move_finish(loc, "idle");
        else if (still >= MOVE_STILL_MS) move_finish(loc, "still, not idle");
    } else if (now - g_move.at >= MOVE_START_MS) {
        logf_("move: %s never moved (action %s)\n", g_move.name,
              action[0] ? action : "none");
        g_move.on = 0;
        char say[128];
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s did not move.", g_move.name);
        history_add(say);
        if (g_speak) speech_say(say);
    }
    if (g_move.on && now - g_move.at >= MOVE_GIVE_UP_MS) {
        logf_("move: %s still moving after %d s -- not watched\n", g_move.name,
              MOVE_GIVE_UP_MS / 1000);
        g_move.on = 0;
    }
}
