// Numpad navigation's game side. See numpad.h; the rules are nav.c's.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "numpad.h"
#include "ue3.h"
#include "natives.h"
#include "names.h"
#include "speech.h"
#include "focus.h"
#include "shot.h"
#include "combat.h"
#include "history.h"
#include "soldier.h"
#include "sight.h"
#include "mission.h"
#include "abar.h"
#include "hq.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "sonar.h"
#include "audio.h"
#include "learn.h"
#include "settings.h"
#include "mouse.h"
#include "props.h"
#include "input.h"
#include "log.h"
#include "game.h"
#include "units.h"
#include "report.h"
#include "where.h"
#include "world.h"
#include "sounds.h"
#include "scanner.h"
#include "menus.h"
#include "move.h"

// The soldier's m_kCurrAction, which says whether they are aiming.
static FieldSlot g_nav_curr_action;

// Aiming a cursor-moving ability (a rocket, a grenade) rather than choosing a
// move. The game's aim point is the battle cursor's feet
// (ActiveUnit_Firing_WithMoveCharacteristics.PostProcessCheckGameLogic ->
// SetTargetLoc(CURSOR.GetCursorFeetLocation())), and Mouse_CheckForFreeAim
// places the cursor through CursorSetLocation, the same chain movement uses.
// So navigation drives the aim the same way it drives a move, with three
// differences: it starts from the aim rather than the soldier; the tile goes
// in at ProcessChainedDistance, the range leash, rather than after it, so the
// game still clamps the aim to the ability's range; and a step is announced
// when the aim lands, since no path is ever built to judge it by.
int              g_nav_aim;
static float     g_aim_floor;           // the floor the aim stands on (aim_floor)

// The cursor, watched rather than driven -- for now.
//
// AXCom3DCursor::GetCursorMode is native and the cursor's own Tick calls it
// every frame, so `self` hands the object over for nothing. Identifying it is
// the whole point: the grid cannot be navigated by relabelling a command,
// because the cursor is flown rather than stepped, so the next piece has to
// write a position into this object -- and that is worth proving readable,
// against a real mission, before anything writes.
//
// The position is read four times a second rather than every frame. The read
// itself is twelve bytes at a fixed offset, but it is guarded by VirtualQuery
// like every other read into the game, and that is a syscall on the game's
// own thread.
#define CURSOR_WATCH_MS 250

static ULONGLONG g_cursor_at;
static float g_cursor_last[3];
static CursorGrid g_grid_last;

// ---- numpad navigation -----------------------------------------------------
//
// The keys are read here rather than through the game's input, because the
// numpad is unbound in [XComGame.XComTacticalInput]: an unbound key never
// becomes an InputEvent, so no hook on the input path would ever see it. That
// also means the game does nothing with them, so nothing has to be swallowed.
// They are polled on the cursor's per-frame native, so they are live exactly
// while a battle cursor exists, and only while the game has the foreground.
//
// Num Lock must be on. With it off, Windows reports numpad 8 as the Up arrow
// -- which pans the camera -- and NVDA's desktop layout takes the numpad for
// its own review commands.
//
// A move is not written into the cursor. In mouse mode Mouse_CheckForPathing
// puts the cursor under the mouse on every frame, so a written Location would
// last one frame. Instead the target is handed to hook_validpos below, which
// substitutes it where the game turns that frame's pick into a cursor position
// -- so the game's own validation, floor snap and path preview run on it.
//
// A tap on a direction steps one tile. Holding it glides, tile after tile,
// until it is let go -- see "holding a direction" below, next to nav_press.

static int       g_numpad_down[10];
static int       g_radar_down[2];       // numpad +, numpad -
static int       g_walls_down;          // numpad *
// Numpad * turns the wall field off and on. A sound that never stops and
// cannot be stopped is a trap, and the player who wants the words without it
// -- or who is working next to someone -- has no other way out. Like the
// digits and the radar keys, numpad * is bound to nothing in a mission:
// DefaultInput.ini mentions Multiply only in the alias lists of edit boxes and
// sliders. The switch is the options menu's (settings.h, SET_FIELD), so the
// choice is saved and either key reaches it.
static void*     g_nav_cursor;          // the cursor navigation began on
static void*     g_nav_pawn;            // ChainedPawn when navigation began
static POINT     g_nav_mouse;           // where the mouse was, to notice it moving
static float     g_nav_world[3];        // the target, in world units, for the hook
static int       g_nav_live;            // the hook substitutes while this is set
static ULONGLONG g_nav_key_at;          // last direction key
static ULONGLONG g_nav_placed_at;       // last frame the hook substituted
static int       g_nav_parked;          // mouse already moved to the centre once

// Heights and reachability. The cursor must be put at the floor's height or
// the game builds no path to it, and whether a tile can be reached at all is
// known only from the path the game builds. Both are settled per tile by the
// phases in nav.h -- floor search, then probing heights against the
// pathfinder -- which main.c drives from three hooks: the pick asks
// navh_query_z, hook_floorz reports the search, and hook_computepath reports
// the path.
//
// The evidence behind the phases, from the logs:
//   - getValidLocation adds exactly 64 to every placement (NAVH_LIFT).
//   - GetFloorZForPosition returns the height it was given when it finds no
//     floor (XGUnit.IsAttemptingToHover tests `FloorZ != PathDestination.Z`),
//     so a search must never start from its own last answer: that climbed 64
//     a step.
//   - It looks down a limited way, and never found a floor below -129 from
//     any start -- which left two soldiers unable to move by numpad at all.
//   - The ground estimate at the start is the cursor less 64. The soldier
//     pawn's Location.Z less CollisionHeight read 215.8 for two soldiers on
//     different ground, so it is not used.
#define NAV_CURSOR_LIFT NAVH_LIFT
static int       g_nav_path_tile[2] = { -1, -1 };     // the tile being decided
static NavHeightPhase g_nav_phase_logged = (NavHeightPhase)-1;
static int       g_nav_tile_logged[2] = { -1, -1 };

int game_has_focus(void)
{
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// ---- a step, waiting to be said ---------------------------------------------
//
// What is on the tile is worked out in report.c; this is the step that waits
// for it, and when it gives up waiting.

static ULONGLONG g_tile_due;            // when the target tile is to be described
static int       g_tile_due_at[2];
static int       g_tile_due_dash;       // whether its path says anything about it

// A step is announced once, when what is on the tile is known, with the
// coordinates last: "Ellis. Low cover south. 44, 12." Until then the step's
// coordinates, and anyone found standing there, wait here. If nothing decides
// the tile in STEP_FALLBACK_MS, what is known is said anyway.
#define STEP_FALLBACK_MS 1500
static int       g_step_pending;
static ULONGLONG g_step_deadline;

// How many times the game has computed a path since the current step began.
// Counted for every caller, not only the navigated tile, because the question
// it answers is whether the game is pathing at all.
static volatile LONG g_path_calls;
// The fallback speaks the coordinates and gives up on the description -- but
// the description is not always gone, only late. The pathfinder does not run
// while a soldier is walking, and in the 2026-09-20 log a step taken during a
// move had its path built one line *after* the deadline had already said
// "44, 6.", so "Low cover west." was worked out and thrown away. When that
// happens the tile is remembered here and the missing half is said on its own
// when it turns up; the coordinates are not repeated, having just been heard.
static int       g_step_late;
static int       g_step_late_at[2];
static char      g_step_coords[NAV_MAX_TEXT];
// Who stands in the target's column, found on arrival. A step does not know
// its floor yet then, so they are worded when the step is said, against the
// floor it settled on: "Godongwana, one floor up." (step_who).
static ColumnUnit g_step_units[COLUMN_UNITS];
static int        g_step_nunits;
static char      g_step_note[160];              // said first: "Floor 2." after F / C
static char      g_step_where[64];             // then "Inside building, floor 2 of 3."
// The floor F / C put the target on, until the next step. The tile does not
// change, so a path the game already had for it -- on the floor it was just
// taken off -- is still reported, and was taken as proof of that floor: the
// first runs of F went 212.6 -> 466.2 and settled straight back on 212.6.
static int       g_floor_hold;
static float     g_floor_hold_z;
// How far a path's end, or a pick's floor, may sit from the held floor and
// still be on it: half a storey, well clear of the floors either side.
#define FLOOR_HOLD_SLACK 96.0f

// How long after a tile's first path it is described. None: the next frame.
// It was 200 ms while "Dash" came from DestinationReachability, which the
// dash rebuild (ChangeDashState) changes a tick later. The path's own cost
// needs no such wait -- the pathfinder builds the whole path at once, past
// the dash limit included (costs of 54 against a MaxPathCost of 24).
#define TILE_DESCRIBE_DELAY_MS 0
// The soldier's own tile has no verdict to wait for, only the floor search,
// which on level ground settles on its first frame.
#define OWN_TILE_DELAY_MS 60

// Says the pending step: anyone standing there, `body`, then the
// coordinates. Once per step.
// The floor the step stands on, as far as it is known: the aim's, or the
// ground the height search settled on.
static float step_floor(void)
{
    return g_nav_aim ? g_aim_floor : navh_ground();
}


// The units in the target's column, worded against the step's floor:
// "Godongwana, one floor up." With `same_only`, only those on that floor --
// the ones that can be standing in the way. Inside a building the floors are
// its own storeys (where_levels_between); elsewhere, storeys of 192.
static void step_who(int same_only, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    float floor = step_floor();
    int tx = 0, ty = 0;
    int have_tile = nav_target(&tx, &ty);
    for (int i = 0; i < g_step_nunits; i++) {
        int dz = scan_storey_diff(g_step_units[i].feet, floor);
        int levels;
        if (have_tile && dz && where_levels_between(tx, ty, floor, g_step_units[i].feet, &levels))
            dz = levels;
        if (same_only && dz) continue;
        char piece[TILE_MAX_TEXT];
        scan_unit_floor_text(g_step_units[i].label, dz, piece, sizeof piece);
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s",
                            used ? " " : "", piece);
        if (w < 0) break;
        used += (size_t)w;
    }
}

static int nav_step_say(const char* body)
{
    if (!g_step_pending) return 0;
    g_step_pending = 0;
    char who[TILE_MAX_TEXT];
    step_who(0, who, sizeof who);
    char say[TILE_MAX_TEXT + TILE_MAX_TEXT + NAV_MAX_TEXT + sizeof g_step_note +
             sizeof g_step_where];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s%s%s%s%s.", g_step_note,
                g_step_note[0] ? " " : "", g_step_where,
                g_step_where[0] ? " " : "", who,
                who[0] && body[0] ? " " : "", body,
                who[0] || body[0] ? " " : "", g_step_coords);
    g_step_note[0] = 0;
    g_step_where[0] = 0;
    logf_("nav: said \"%s\"\n", say);
    speech_say_now(say);
    return 1;
}

// Why a tile no path reaches is refused, from the game's own tile flags (see
// tile.h). The floor is not known -- that is what failed -- so every layer the
// height probe tried is asked about: the ground's layer and three either side,
// which is PROBE_HEIGHTS' span. `say` stays "No path." whenever the game
// cannot be asked.
#define REFUSAL_LAYERS 3

// The game's own flags for the layers of a tile's column, `span` either side
// of the ground's layer: floor, a valid destination, occupied. Returns how
// many layers were asked, or -1 when the game cannot be asked; `seen` gets
// them for the log, "7:FD-" per layer, and `mid` the ground's layer.
#define QUICK_LAYERS 6

static int tile_layers(int tx, int ty, float ground, int span, TileLayerFlags* layers,
                       int max, char* seen, size_t seen_sz, int* mid_out)
{
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return -1;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    PositionTestFn standable = (PositionTestFn)tile_vfn(world, g_tile_slot_standable);
    TileTestFn occupied = (TileTestFn)tile_vfn(world, g_tile_slot_occupied);
    if (!on_floor || !standable) return -1;

    // The layer is found as tile_report finds it, from the floor plus 4.
    int mid = grid_floor_layer(&g, ground);
    if (mid_out) *mid_out = mid;
    size_t used = 0;
    int n = 0;
    seen[0] = 0;
    for (int tz = mid - span; tz <= mid + span && n < max; tz++) {
        if (tz < 0 || (g.num_z > 0 && tz >= g.num_z)) continue;
        // The middle of the layer: the natives make a tile of it themselves,
        // and the middle is as far as can be from either edge's rounding.
        float pos[3] = {
            grid_centre_x(&g, tx),
            grid_centre_y(&g, ty),
            grid_layer_middle(&g, tz),
        };
        TileLayerFlags* l = &layers[n++];
        l->floor = on_floor(world, NULL, pos) != 0;
        l->destination = standable(world, NULL, pos) != 0;
        l->occupied = occupied ? occupied(world, NULL, tx, ty, tz) != 0 : 0;
        l->below = tz < mid;
        // "7:FD-" -- floor, destination, occupied, per layer, for the log.
        int w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, "%s%d:%c%c%c",
                            used ? " " : "", tz, l->floor ? 'F' : '-',
                            l->destination ? 'D' : '-', l->occupied ? 'O' : '-');
        if (w > 0) used += (size_t)w;
    }
    return n;
}

static void tile_refusal_probe(int tx, int ty, float ground, char* say, size_t say_sz)
{
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(TILE_REFUSE_NO_PATH));
    TileLayerFlags layers[2 * REFUSAL_LAYERS + 1];
    char seen[(2 * REFUSAL_LAYERS + 1) * 16];
    int mid;
    int n = tile_layers(tx, ty, ground, REFUSAL_LAYERS, layers,
                        2 * REFUSAL_LAYERS + 1, seen, sizeof seen, &mid);
    if (n < 0) return;
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(tile_refusal(layers, n)));
    logf_("nav: %d, %d refused, ground %.1f (layer %d), layers %s -> \"%s\"\n",
          tx, ty, ground, mid, seen, say);
}

// A step onto a tile nothing can stand on, decided the moment it lands. The
// height search needs a path answer per height, and the game gives about ten
// a second: on 48, 6 (2026-09-23), where every height failed, the search and
// the probe never finished inside STEP_FALLBACK_MS, and all four visits were
// 1.5 s of silence and then the bare coordinates, "No path" never said. The
// tile's flags say the same thing at once: if no layer within QUICK_LAYERS of
// the ground -- further than the search reaches -- is a place a move may end
// (IsPositionOnFloorAndValidDestination), no height will build a path. Only
// then is the step decided here; a tile with any destination on it searches
// as ever, so nothing reachable is refused early.
static int tile_blocked_now(int tx, int ty, float ground, char* say, size_t say_sz)
{
    TileLayerFlags layers[2 * QUICK_LAYERS + 1];
    char seen[(2 * QUICK_LAYERS + 1) * 16];
    int mid;
    int n = tile_layers(tx, ty, ground, QUICK_LAYERS, layers, 2 * QUICK_LAYERS + 1,
                        seen, sizeof seen, &mid);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) if (layers[i].destination) return 0;
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(tile_refusal(layers, n)));
    logf_("nav: %d, %d decided on arrival, ground %.1f (layer %d), layers %s -> \"%s\"\n",
          tx, ty, ground, mid, seen, say);
    return 1;
}

// The floor an aim lands on at a tile. The aim is the cursor's feet, and
// nav_aim_substitute puts in a height as well as the tile -- only X and Y went
// in at first, and every aim of the 2026-09-22 run sat at 259.2, the height of
// the soldier on the roof who was lent to it, three storeys above the
// Chryssalid it was fired at. So the height is the tile's own floor: the first
// layer at or below the aim's current floor that the game marks as floor
// (IsPositionOnFloor), which steps off a roof onto the ground and stays on a
// roof walked along; failing that, the nearest above, a few layers up.
//
// The layer only says a floor is somewhere inside it: 64 units. The exact
// height is then asked of GetFloorZForPosition from the layer's top, which
// searches down and finds it (floors were found from 64 above in the height
// probe, never from 247). The layer's bottom stands in if that cannot be
// called: 0 lies in layer -1..63 on the map with Min.Z -193. `from` is
// returned when the game cannot be asked or finds nothing.
#define AIM_FLOOR_UP 3

static float aim_floor(const CursorGrid* g, int tx, int ty, float from)
{
    void* world = cursor_world();
    if (!world) return from;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    if (!on_floor) return from;
    int start = grid_floor_layer(g, from);
    float pos[3] = {
        grid_centre_x(g, tx),
        grid_centre_y(g, ty),
        0.0f,
    };
    Fault f;
    __try {
        for (int tz = start; tz >= 0; tz--) {
            if (g->num_z > 0 && tz >= g->num_z) continue;
            pos[2] = grid_layer_middle(g, tz);
            if (on_floor(world, NULL, pos))
                return aim_floor_exact(world, pos, grid_layer_bottom(g, tz));
        }
        for (int tz = start + 1; tz <= start + AIM_FLOOR_UP; tz++) {
            if (tz < 0 || (g->num_z > 0 && tz >= g->num_z)) break;
            pos[2] = grid_layer_middle(g, tz);
            if (on_floor(world, NULL, pos))
                return aim_floor_exact(world, pos, grid_layer_bottom(g, tz));
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: aim floor", &f, NULL);
    }
    return from;
}

// Where the path the game holds for a described tile ends, when that is not
// the tile -- logged, never said. The end is not a verdict on the tile:
// XComPath.Path is cut at the pathing pawn's allowance of the moment, which
// rises from one move to a dash a frame or more after the path asks for it.
// Taken as "No path", the 19:08 log refused 14, 43 (cost 13) while the
// allowance was still 12 -- the path ended on 14, 44, cost 11 -- and a frame
// later the same tile read "Dash" with max 24. It was taken as a verdict after
// the 18:58 log, where Hagen, confirmed onto floor 2 of 14, 40, walked a path
// that ended on 14, 42 outside; that is caught at the confirm instead
// (move_confirmed), when the path is the one the click performs. Read in the
// ComputePath2 hook it is also a path behind: DrawPath updates it afterwards.
static void path_note(int tx, int ty, float floor)
{
    float end[3];
    CursorGrid g;
    if (!g_path_pawn || !cursor_grid(&g) || !path_end(g_path_pawn, end)) return;
    int ex = grid_x(&g, end[0]), ey = grid_y(&g, end[1]);
    if (ex == tx && ey == ty && fabsf(end[2] - NAV_CURSOR_LIFT - floor) <= PATH_END_Z_SLACK)
        return;
    int cost, std, max, moves, turns;
    tile_dash(&cost, &std, &max, &moves, &turns);
    logf_("nav: the path to %d, %d floor %.1f ends on %d, %d (z %.1f) for now; "
          "cost %d, max %d\n", tx, ty, floor, ex, ey, end[2], cost, max);
}

// A tile no path reaches. A unit standing on it is the likeliest reason, and
// worth more than the verdict: when one was found on arrival, its name is the
// whole answer. Otherwise the tile's flags say why.
static void nav_say_no_path(int tx, int ty)
{
    char here[TILE_MAX_TEXT];
    step_who(1, here, sizeof here);
    logf_("nav: %d, %d unreachable%s\n", tx, ty, here[0] ? " -- occupied" : "");
    if (here[0]) {
        nav_step_say("");
        return;
    }
    if (!g_step_pending) return;    // already said: nothing to ask the game for
    char why[48];
    GUARDED("nav: refusal", tile_refusal_probe(tx, ty, navh_ground(), why, sizeof why),
            _snprintf_s(why, sizeof why, _TRUNCATE, "%s", tile_refusal_text(TILE_REFUSE_NO_PATH)));
    nav_step_say(why);
}

// ---- the radar -------------------------------------------------------------
//
// Numpad + lists the enemies in sight, numpad - the rest of the squad, each
// by offset from the soldier being moved, nearest first: "Chryssalid, 2
// north, 5 east." Offsets are tiles in the numpad's directions, so the answer
// is also the way there. The minimap does the same job for a sighted player,
// and draws from the same condition: a contact is on it while it is in sight.
static void radar(int friendly)
{
    CursorGrid g;
    int sx, sy;
    float sz;
    if (!cursor_grid(&g) || !soldier_tile(&g, &sx, &sy, &sz)) {
        speech_say_now("No soldier.");
        return;
    }
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);
    void* squad = squad_player();
    if (!squad) {
        logf_("radar: the soldier's player is unreadable\n");
        speech_say_now("No soldier.");
        return;
    }

    // Measured from the tile being navigated to, so the offsets are the keys
    // to press from where the player is now; from the soldier until a step
    // has been taken. The soldier is left out only when measuring from their
    // own tile -- away from it, where they stand is worth hearing too.
    int ox = sx, oy = sy;
    nav_target(&ox, &oy);
    int from_soldier = ox == sx && oy == sy;
    static SeenSet sight;
    if (!friendly) squad_sight(squad, &sight);

    // The squad list is also the squad at a glance: each soldier with what
    // their flag shows over their head -- the action pips, hit points, panic,
    // bleeding out -- so who can still act is heard without switching to
    // each in turn. The selected soldier is in it too, "here".
    static char labels[UNIT_MAX][256];
    TileContact c[UNIT_MAX];
    int n = 0;
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || s.friendly != friendly ||
            (!friendly && from_soldier && s.pawn == soldier) ||
            (!friendly && !seen_has(&sight, s.unit)))
            continue;
        unit_label(&g_units[i], labels[n], sizeof labels[n]);
        if (friendly) {
            const UnitName* u = &g_units[i];
            char state[128];
            soldier_squad_words(u->moves, u->hp, u->hp_max, u->panicked, u->wounded,
                                u->bleed_turns, state, sizeof state);
            if (state[0]) {
                size_t used = strlen(labels[n]);
                _snprintf_s(labels[n] + used, sizeof labels[n] - used, _TRUNCATE, ", %s", state);
            }
        }
        c[n].name = labels[n];
        c[n].dx = grid_x(&g, s.loc[0]) - ox;
        c[n].dy = grid_y(&g, s.loc[1]) - oy;
        n++;
    }
    static char say[2048];
    tile_contacts(c, n,
                  !friendly ? "No enemies in sight." : "No squad in sight.",
                  say, sizeof say);
    logf_("radar: %s from %d, %d%s%s: %s\n", friendly ? "squad" : "enemies", ox, oy,
          from_soldier ? " (the soldier)" : " (the target)",
          friendly ? "" : sight.n ? "" : ", the squad sees no one", say);
    speech_say_now(say);
}


// Defined with the pick, below: the interface lent to the mouse's own pick.
static void nav_forget_interface(void);

// ---- a step up or down a floor ---------------------------------------------
//
// "You just stepped from the roof to the ground": a cue when a described step
// stands on a different floor from the one before -- rising for up, falling
// for down, once a storey (192 units, the game's floor), so a roof two
// storeys up is two. Less than a third of a storey (a kerb, a ramp) is
// nothing. Only a described tile has a settled floor, so a glide is heard
// where it stops, against where it started; a tile no path reaches has no
// floor and sounds nothing.
#define HEIGHT_MIN      64.0f
#define HEIGHT_STOREY   192.0f
#define HEIGHT_MAX_CUES 4

static int   g_floor_prev_ok;
static float g_floor_prev;

static void height_step(int tx, int ty, float floor)
{
    if (g_floor_prev_ok && settings_get(SET_STEPS)) {
        float dz = floor - g_floor_prev;
        float up = dz < 0.0f ? -dz : dz;
        if (up >= HEIGHT_MIN) {
            int n = (int)(up / HEIGHT_STOREY + 0.5f);
            if (n < 1) n = 1;
            if (n > HEIGHT_MAX_CUES) n = HEIGHT_MAX_CUES;
            audio_cue(dz > 0.0f ? HEART_STEP_UP : HEART_STEP_DOWN, n);
            logf_("height: %d, %d floor %.1f from %.1f -- %d %s\n", tx, ty, floor,
                  g_floor_prev, n, dz > 0.0f ? "up" : "down");
        }
    }
    g_floor_prev = floor;
    g_floor_prev_ok = 1;
}

// ---- letting go of the target, the mouse, confirming ------------------------

void nav_stop(const char* why)
{
    if (!nav_active()) return;
    g_floor_prev_ok = 0;
    nav_end();
    nav_forget_interface();
    // The field is not silenced. With no target held it simply goes back to
    // listening from the selected soldier (listen_tile) -- the walls are
    // still there, and the player has not stopped needing to hear them.
    g_nav_live = 0;
    g_nav_aim = 0;
    g_nav_parked = 0;
    g_tile_due = 0;
    g_step_pending = 0;
    g_step_late = 0;
    logf_("nav: released (%s)\n", why);
}

// The mouse picks nothing while it rests on the HUD or off the map, and then
// Mouse_CheckForPathing never reaches the placement at all -- a key would move
// the target and nothing would follow. Moving the mouse to the middle of the
// game window puts it over the battlefield. Done once per navigation, and only
// after a key has gone unanswered, so a mouse already over the map is left
// where it is.
static int mouse_to_centre(POINT* c)
{
    HWND w = GetForegroundWindow();
    RECT r;
    // A minimised window is still in front for a moment on the way back, and
    // its "centre" is off the screen at -32000; SetCursorPos then pins the
    // mouse to a corner. Not parked, so the next poll tries again.
    if (!w || IsIconic(w) || !GetClientRect(w, &r) ||
        r.right <= r.left || r.bottom <= r.top) return 0;
    c->x = (r.right - r.left) / 2;
    c->y = (r.bottom - r.top) / 2;
    if (!ClientToScreen(w, c)) return 0;
    SetCursorPos(c->x, c->y);
    GetCursorPos(&g_nav_mouse);
    return 1;
}

static void nav_park_mouse(void)
{
    POINT c;
    if (!mouse_to_centre(&c)) return;
    g_nav_parked = 1;
    logf_("nav: no placement since the key; mouse moved to the window centre "
          "(%ld, %ld)\n", c.x, c.y);
}

// With the mouse blocked (mouse.h) it stays wherever it was left, and where it
// was left may be the HUD, where the game will not path, or a screen edge,
// where the camera scrolls for as long as it rests there. So once the block
// takes hold in a mission -- and again each time the game comes back to the
// front, since the mouse is free while it is away -- it is put in the middle
// of the window, over the battlefield. g_nav_mouse follows, so a held target
// does not read this as the mouse moving.
static int g_mouse_parked;

static void mouse_hold_poll(void)
{
    if (!mouse_blocking()) { g_mouse_parked = 0; return; }
    if (g_mouse_parked) return;
    POINT c;
    if (!mouse_to_centre(&c)) return;
    g_mouse_parked = 1;
    logf_("mouse: blocked, parked at the window centre (%ld, %ld); %u device events "
          "swallowed so far\n", c.x, c.y, mouse_swallowed());
}

// Numpad 0. The game moves a soldier from the path it has already built out
// to the cursor, and a right click is the mouse-mode way to ask for that:
// RMouse's release runs ClickToPath -> PerformPath. Sent as a click rather
// than called, because nothing in this DLL calls into script.
static ULONGLONG g_nav_confirm_at;      // opens nav_watch_input's window

static void nav_confirm(void)
{
    // Before the click: the path is read while it is still the one the click
    // will perform (move.c). An aim fires rather than moves.
    if (!g_nav_aim) {
        int mx = -1, my = -1, go = 1;
        if (!nav_target(&mx, &my)) mx = my = -1;
        GUARDED("move: confirmed", go = move_confirmed(mx, my, navh_ground()), go = 1);
        if (!go) return;
    }
    g_nav_confirm_at = GetTickCount64();
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    UINT sent = SendInput(2, in, sizeof(INPUT));
    int tx = -1, ty = -1;
    nav_target(&tx, &ty);
    logf_("nav: confirm -- right click sent (%u of 2), target %d, %d\n",
          sent, tx, ty);
}

// ---- holding a direction ---------------------------------------------------
//
// A tap steps one tile and says what is on it. Holding the key glides: after
// NAV_HOLD_MS the step repeats, quickening from NAV_GLIDE_MS to
// NAV_GLIDE_FAST_MS over NAV_GLIDE_RAMP steps, and carries on until the key is
// let go. It is meant to feel like the camera under WASD -- a way to cross the
// map or sweep a room -- and it is the field (walls_poll) that makes it worth
// having: the walls swell and fade as the cursor passes them, so a glide down
// a corridor is heard as a corridor.
//
// Nothing is spoken while it runs. A description costs a path from the game
// and a floor search, and twenty of them a second would arrive long after the
// player had stopped and would say the wrong tiles when they did. So the glide
// announces exactly one tile: the one it ends on.
//
// The repeat is driven from here rather than from Windows' own key repeat. The
// numpad is unbound in the mission, so no key message for it ever reaches the
// game and there is nothing to read a repeat off; GetAsyncKeyState says only
// that the key is down now.
#define NAV_HOLD_MS       260   // held this long before it starts to repeat
// The first repeat and where it settles, per speed in the options menu
// (settings.h, GLIDE_*). Normal is the speed verified live.
static const int NAV_GLIDE_MS[3]      = { 170, 110, 75 };
static const int NAV_GLIDE_FAST_MS[3] = {  85,  50, 30 };
#define NAV_GLIDE_RAMP     10   // repeats spent getting there

static int       g_glide_digit;      // the direction being held, 0 for none
static int       g_glide_steps;      // repeats taken, for the ramp
static ULONGLONG g_glide_next;       // when the next step is due
static ULONGLONG g_numpad_at[10];    // when each key last went down

static int glide_interval(int steps)
{
    int sp = settings_get(SET_GLIDE);
    if (steps >= NAV_GLIDE_RAMP) return NAV_GLIDE_FAST_MS[sp];
    return NAV_GLIDE_MS[sp] +
           (NAV_GLIDE_FAST_MS[sp] - NAV_GLIDE_MS[sp]) * steps / NAV_GLIDE_RAMP;
}

// What a step arrives to: who is standing on the tile, the floor under it, the
// path the game builds to it, and in the end the announcement. Skipped on
// every step of a glide and run once on the tile it stops on.
static void nav_arrive(int tx, int ty)
{
    g_tile_due = 0;
    g_step_note[0] = 0;
    g_step_where[0] = 0;
    g_floor_hold = 0;
    navh_begin_tile();
    g_nav_path_tile[0] = tx;
    g_nav_path_tile[1] = ty;
    g_nav_phase_logged = (NavHeightPhase)-1;

    // Who stands there is found now, on arrival, and not from the tile's
    // verdict: another unit's tile gets only "No path", and the soldier's
    // own tile gets no verdict at all -- the game builds no path to
    // within 64 units of the soldier (XGAction_Path.DoPathingTick).
    // Every storey of the column: which floor the step is on is not known
    // until it settles, so step_who words them then.
    int mine = 0;
    Fault f;
    __try {
        g_step_nunits = units_in_column(tx, ty, g_step_units, COLUMN_UNITS);
        for (int i = 0; i < g_step_nunits; i++) mine |= g_step_units[i].mine;
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("tile: units", &f, NULL);
        g_step_nunits = 0;
    }
    nav_describe(tx, ty, g_step_coords, sizeof g_step_coords);
    g_step_late = 0;
    g_step_pending = 1;
    g_step_deadline = GetTickCount64() + STEP_FALLBACK_MS;
    g_path_calls = 0;
    // An aim is announced when it lands (nav_aim_landed): no path is built
    // to it, and the soldier's own tile is nothing special to a rocket.
    if (g_nav_aim) return;
    // Nothing can stand here: say so now rather than after the search times
    // out. A tile with a unit on it is left to the search, whose verdict
    // names them.
    if (!mine && !g_step_nunits) {
        char why[48];
        int blocked = 0;
        GUARDED("nav: arrival check",
                blocked = tile_blocked_now(tx, ty, navh_ground(), why, sizeof why),
                blocked = 0);
        if (blocked) {
            navh_decide_none();
            nav_step_say(why);
            return;
        }
    }
    // With no verdict coming for the soldier's own tile, its cover is
    // described on a timer instead, once the floor search has settled. The
    // same when the soldier has no moves left: XGUnit.AddPathAction gives an
    // idle action instead of a path when GetMoves() is 0 -- after Run and
    // Gun's dash, with only the shot to come -- so no path is ever computed.
    // Every step then waited out STEP_FALLBACK_MS and said its coordinates
    // alone (the 2026-09-28 00:20 log: "nothing decided the tile in 1500 ms
    // (0 path calls)", five steps running).
    int stuck = !mine && soldier_out_of_moves();
    if (stuck)
        logf_("nav: %d, %d described without a path -- the soldier has no moves left\n",
              tx, ty);
    if (mine || stuck) {
        g_tile_due = GetTickCount64() + OWN_TILE_DELAY_MS;
        g_tile_due_at[0] = tx;
        g_tile_due_at[1] = ty;
        g_tile_due_dash = 0;
    }
}

// `gliding` when this is a repeat of a held key rather than a fresh press.
static void nav_press(int digit, int gliding)
{
    CursorGrid g;
    int tx, ty;
    float z;
    if (!cursor_grid(&g) || !cursor_tile(&g, &tx, &ty, &z)) {
        // Said on the press only. A glide that loses the grid under it would
        // otherwise say this twenty times a second.
        if (gliding) return;
        logf_("nav: numpad %d with no grid or cursor yet\n", digit);
        speech_say_now("No map yet.");
        return;
    }

    // Moving and aiming place the cursor for different reasons, so a
    // navigation begun for one is not carried into the other.
    int aiming = soldier_aiming();
    if (nav_active() && aiming != g_nav_aim)
        nav_stop(aiming ? "aiming began" : "aiming ended");

    // Until a step has been taken, "here" is the soldier, not the mouse --
    // except while aiming, where the cursor IS the aim, and the aim is what
    // the player is moving.
    if (!nav_active() && aiming) {
        logf_("nav: aiming -- starting from the aim on %d, %d (z %.1f)\n", tx, ty, z);
    } else if (!nav_active()) {
        int sx, sy;
        float sz;
        if (soldier_tile(&g, &sx, &sy, &sz) &&
            sx >= 0 && sy >= 0 && sx < g.num_x && sy < g.num_y) {
            // The cursor's height is kept when it is on the soldier already:
            // it is the one the floor search is known to work from. An earlier
            // run read this pawn's height as 215.8 for two soldiers on
            // different ground, so it is only a fallback, and the search
            // sweeps several hundred units either way.
            if (sx != tx || sy != ty) {
                logf_("nav: cursor on %d, %d (z %.1f), soldier on %d, %d (z %.1f) -- "
                      "using the soldier\n", tx, ty, z, sx, sy, sz);
                z = sz;
            }
            tx = sx;
            ty = sy;
        } else {
            logf_("nav: soldier's position unreadable, using the cursor's\n");
        }
    }

    if (digit == 5) {
        // Where the cursor actually is, which is what the game accepted --
        // not necessarily the target, if that was somewhere it cannot stand.
        // And what is there, from the cursor's own height: it stands
        // NAVH_LIFT above the floor it was placed on.
        char say[NAV_MAX_TEXT + TILE_MAX_TEXT + 64];
        char what[TILE_MAX_TEXT] = "";
        char coords[NAV_MAX_TEXT];
        nav_describe(tx, ty, coords, sizeof coords);
        logf_("nav: numpad 5 -> cursor on %s\n", coords);
        // The last path is this tile's only while navigating to it, and never
        // on the soldier's own tile: the game builds no path to where the
        // soldier stands, so the one left over belongs to somewhere else.
        int ntx, nty, sx, sy;
        float sz;
        int own = soldier_tile(&g, &sx, &sy, &sz) && sx == tx && sy == ty;
        int path_is_here = nav_target(&ntx, &nty) && ntx == tx && nty == ty && !own;
        Fault f;
        __try {
            if (!tile_report(tx, ty, z - NAV_CURSOR_LIFT, path_is_here, 1, what, sizeof what))
                what[0] = 0;
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("tile: report", &f, NULL);
            what[0] = 0;
        }
        char where[64];
        where_say(tx, ty, z - NAV_CURSOR_LIFT, 1, where, sizeof where);
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s.", where, where[0] ? " " : "",
                    what, what[0] ? " " : "", coords);
        // Nothing is played here. Numpad 5 is the key for "where am I", and
        // the shape of the room is the larger half of that answer -- but the
        // field has been answering it all along, so the key only has to
        // supply the words.
        speech_say_now(say);
        return;
    }
    if (digit == 0) {
        nav_confirm();
        return;
    }

    int dx, dy;
    if (!nav_step_for_digit(digit, &dx, &dy)) return;

    if (!nav_active()) {
        nav_begin(tx, ty);
        g_nav_cursor = cursor_object();
        g_nav_pawn = NULL;
        cursor_chained_pawn(&g_nav_pawn);
        GetCursorPos(&g_nav_mouse);
        g_nav_parked = 0;
        navh_set_ground(z - NAV_CURSOR_LIFT);
        navh_begin_tile();
        g_nav_aim = aiming;
        g_aim_floor = z - NAV_CURSOR_LIFT;
        logf_("nav: begins on %d, %d, ground estimate %.1f%s\n", tx, ty, navh_ground(),
              aiming ? ", aiming" : "");
        // The soldier's floor, so the first step off a roof is heard too.
        g_floor_prev = navh_ground();
        g_floor_prev_ok = 1;
        where_forget();
    }

    char say[NAV_MAX_TEXT + TILE_MAX_TEXT];
    NavGrid ng = { g.num_x, g.num_y };
    int moved = nav_move(&ng, dx, dy, say, sizeof say);
    nav_target(&tx, &ty);
    if (moved && !gliding) {
        nav_arrive(tx, ty);
    } else if (moved) {
        // A glide describes nothing, and must not leave anything half
        // decided behind it either: a description still pending belongs to a
        // tile the cursor has left, and a path verdict is about to arrive for
        // one too.
        g_tile_due = 0;
        g_step_pending = 0;
        g_nav_path_tile[0] = -1;
        g_nav_path_tile[1] = -1;
        // The floor search is not restarted per step. It takes several frames
        // and a glide gives it fifty milliseconds, so restarting it would
        // leave the height permanently unsettled; the ground carries over
        // instead, which is right on the level ground a glide is for. The tile
        // it stops on gets a search of its own, from nav_arrive. Whether the
        // cursor can actually be placed at that height does not hold the glide
        // up: what the field listens from is nav's own target, not the cursor.
    }

    // The middle of the tile; the height comes from navh_query_z each frame.
    // An aim is asked at one height instead: the floor search is driven by
    // path verdicts, aiming builds no paths, and the cursor snaps itself to
    // the floor as it moves (XCom3DCursor's CursorSnapToFloor).
    g_nav_world[0] = grid_centre_x(&g, tx);
    g_nav_world[1] = grid_centre_y(&g, ty);
    if (g_nav_aim) {
        float was = g_aim_floor;
        g_aim_floor = aim_floor(&g, tx, ty, g_aim_floor);
        if (g_aim_floor != was)
            logf_("nav: aim floor %.1f -> %.1f at %d, %d\n", was, g_aim_floor, tx, ty);
    }
    // The floor itself, not the cursor's height above it: getValidLocation
    // adds the cursor's collision height to what it is given, as it does for
    // a move, whose pick point is the ground. Given floor + lift, the aim sat
    // 63 units up on flat ground in the run of 2026-09-22.
    g_nav_world[2] = g_nav_aim ? g_aim_floor : navh_query_z();
    g_nav_live = 1;
    g_nav_key_at = GetTickCount64();

    if (!gliding)
        logf_("nav: numpad %d -> target %d, %d  \"%s\"\n", digit, tx, ty, say);
    // A step waits for its description (nav_step_say); only the edge, which
    // moves nothing, is answered at once -- and not while gliding, where it
    // would repeat "Edge" twenty times a second against the side of the map.
    if (!moved && !gliding) speech_say_now(say);
}

// ---- putting the target on a tile: Home, F and C ---------------------------
//
// The scanner's Home (scanner.c) and the floor keys below both end here.
// Home puts the cursor on the selection, so the camera goes there and the
// tile describes itself as a step would. Shift+Home goes back to the soldier.
//
// This is the same start nav_press makes on its first key, with one
// difference: the item's own height is a far better ground estimate than the
// cursor's, so the floor search usually settles on its first frame.
void nav_focus(int tx, int ty, float ground, const char* what)
{
    CursorGrid g;
    if (!cursor_grid(&g)) return;

    nav_begin(tx, ty);
    g_nav_cursor = cursor_object();
    g_nav_pawn = NULL;
    cursor_chained_pawn(&g_nav_pawn);
    GetCursorPos(&g_nav_mouse);
    g_nav_parked = 0;
    navh_set_ground(ground);
    navh_begin_tile();
    // While aiming, Home puts the aim on the selection -- at the floor under
    // it, which for a unit on a roof is the roof. It is also the one way to
    // lift an aim onto a higher floor: numpad steps only ever go down to one.
    g_nav_aim = soldier_aiming();
    if (g_nav_aim) g_aim_floor = aim_floor(&g, tx, ty, ground);
    nav_arrive(tx, ty);

    g_nav_world[0] = grid_centre_x(&g, tx);
    g_nav_world[1] = grid_centre_y(&g, ty);
    g_nav_world[2] = g_nav_aim ? g_aim_floor : navh_query_z();
    g_nav_live = 1;
    g_nav_key_at = GetTickCount64();
    logf_("scan: cursor to %s on %d, %d, ground %.1f\n", what, tx, ty, ground);
}

// F and C: the target one storey up or down -- the game's own keys for it
// ("Change Cursor Altitude", also the mouse wheel). In the moving state and
// while aiming a rocket or grenade the game runs XCom3DCursor.AscendFloor /
// DescendFloor on them, which is the next *storey* (WorldZToCursorFloor, 192
// units), snapped to whatever surface it has there. Numpad navigation holds
// the cursor's height itself every frame, so without this the keys moved the
// camera's cut-away and nothing else. They still reach the game, which keeps
// that cut-away in step.
//
// The next floor is the first surface, going the way asked through the grid's
// layers, at least FLOOR_STEP_MIN from where the target stands. A layer's
// surface is asked of GetFloorZForPosition from the layer's top -- the floor at
// or below that point -- with IsPositionOnFloor at its middle as the fallback.
// It used to be the midpoint test alone, and the first surface in a different
// *game storey* (WorldZToCursorFloor): on 2026-09-27 a raised floor at 32 and
// a floor at 129.8 were one storey to the game, so F refused, and on the tile
// beside it the midpoint test found nothing below 129.8, so C refused too --
// "no floors below" from a floor the player had just stepped up to. A step
// or a crate top, closer than FLOOR_STEP_MIN, is still passed over. The
// target is then put there exactly as Home puts it on a scanner item, so a
// move gets its path verdict and an aim its odds, with "Floor N." in front.
#define FLOOR_KEYS 2
#define FLOOR_STEP_MIN 96.0f
static int g_floor_down[FLOOR_KEYS];      // F, C

// `seen` gets what each layer tried answered, for the log: "6:- 5:F0@212.6"
// is no floor on layer 6, a floor at 212.6 in storey 0 on layer 5.
static int floor_probe(const CursorGrid* g, int tx, int ty, float from, int dir,
                       float* out, int* storey, char* seen, size_t seen_sz)
{
    size_t used = 0;
    seen[0] = 0;
    void* world = cursor_world();
    if (!world) return 0;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    FloorZFn floorz = (FloorZFn)tile_vfn(world, g_tile_slot_floorz);
    if (!on_floor) return 0;
    float x = grid_centre_x(g, tx);
    float y = grid_centre_y(g, ty);
    float here[3] = { x, y, from + 4.0f };
    int cur = floor_of(here);
    int layer = grid_floor_layer(g, from);
    int w = _snprintf_s(seen, seen_sz, _TRUNCATE, "from layer %d storey %d:", layer, cur);
    if (w > 0) used = (size_t)w;
    (void)cur;
    // Down asks the game first for the floor at or below a step under the
    // target, which needs no grid layer: a sunken floor can lie under the
    // grid's bottom. The log of 2026-09-27 had one at -59, layer -1; F went up
    // from it to 69, and C, walking layers 1 and 0 and no further, could not
    // find it again.
    if (dir < 0 && floorz) {
        float probe[3] = { x, y, from - FLOOR_STEP_MIN };
        float z = floorz(world, NULL, probe, 1);           // bUnlimitedSearch
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below(raw %.1f)", z);
        if (w > 0) used += (size_t)w;
        if (z != probe[2] && z <= probe[2] + 1.0f && z > probe[2] - 4096.0f) {
            float at[3] = { x, y, z + 4.0f };
            int f = floor_of(at);
            w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below:F%d@%.1f", f, z);
            if (w > 0) used += (size_t)w;
            *out = z;
            *storey = f;
            return 1;
        }
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below:-");
        if (w > 0) used += (size_t)w;
    }
    // Down starts in the target's own layer: a surface below it there is
    // still further than the step, or not.
    for (int tz = dir < 0 ? layer : layer + dir; tz >= 0 && (g->num_z <= 0 || tz < g->num_z);
         tz += dir) {
        float bottom = grid_layer_bottom(g, tz);
        float top[3] = { x, y, bottom + 63.0f };
        float z = 0.0f;
        int has = 0;
        if (floorz) {
            z = floorz(world, NULL, top, 0);
            has = z != top[2] && z >= bottom - 1.0f && z <= bottom + 63.0f;
        }
        if (!has) {
            float mid[3] = { x, y, bottom + 32.0f };
            if (on_floor(world, NULL, mid)) {
                z = aim_floor_exact(world, mid, bottom);
                has = 1;
            }
        }
        if (!has) {
            w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " %d:-", tz);
            if (w > 0) used += (size_t)w;
            continue;
        }
        float at[3] = { x, y, z + 4.0f };
        int f = floor_of(at);
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " %d:F%d@%.1f", tz, f, z);
        if (w > 0) used += (size_t)w;
        if ((dir > 0 && z < from + FLOOR_STEP_MIN) || (dir < 0 && z > from - FLOOR_STEP_MIN))
            continue;
        *out = z;
        *storey = f;
        return 1;
    }
    return 0;
}

// The surfaces the cursor has stood on, per tile. GetFloorZForPosition does
// not report every surface the game lets a soldier stand on: on 2026-09-27 the
// cursor settled on 102.2 at 31, 47 (the top of something, found by the game's
// own cursor validation), F went up to 219.3, and neither the direct query nor
// any layer found 102.2 again -- "No floor below" from where the player had
// just been. So every height the search settles on, and every floor F / C
// reaches, is kept, and F / C take the nearest kept one in the way asked when
// it is closer than what the probe found.
#define KNOWN_SURFACES 512
static struct { short tx, ty; float z; } g_known[KNOWN_SURFACES];
static int g_known_n, g_known_next;
static void* g_known_world;           // another map is another set

static void surface_forget_if_new_map(void)
{
    void* w = cursor_world();
    if (w == g_known_world) return;
    g_known_world = w;
    g_known_n = g_known_next = 0;
}

static void surface_note(int tx, int ty, float z)
{
    surface_forget_if_new_map();
    for (int i = 0; i < g_known_n; i++)
        if (g_known[i].tx == tx && g_known[i].ty == ty &&
            g_known[i].z > z - 16.0f && g_known[i].z < z + 16.0f)
            return;
    int slot = g_known_n < KNOWN_SURFACES ? g_known_n++ : g_known_next++ % KNOWN_SURFACES;
    g_known[slot].tx = (short)tx;
    g_known[slot].ty = (short)ty;
    g_known[slot].z = z;
}

static int surface_known(int tx, int ty, float from, int dir, float* out)
{
    surface_forget_if_new_map();
    int found = 0;
    for (int i = 0; i < g_known_n; i++) {
        if (g_known[i].tx != tx || g_known[i].ty != ty) continue;
        float d = (g_known[i].z - from) * (float)dir;
        if (d < FLOOR_STEP_MIN) continue;
        if (!found || d < (*out - from) * (float)dir) { *out = g_known[i].z; found = 1; }
    }
    return found;
}

// The next floor: the probe's answer or a kept surface, whichever is nearer.
static int floor_next(const CursorGrid* g, int tx, int ty, float from, int dir,
                      float* out, int* storey, char* seen, size_t seen_sz)
{
    float zp = 0.0f, zk = 0.0f;
    int sp = 0;
    int probe = floor_probe(g, tx, ty, from, dir, &zp, &sp, seen, seen_sz);
    int known = surface_known(tx, ty, from, dir, &zk);
    size_t used = strlen(seen);
    if (known)
        _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " known:%.1f", zk);
    if (!probe && !known) return 0;
    if (known && (!probe || (zk - from) * (float)dir < (zp - from) * (float)dir)) {
        float at[3] = { grid_centre_x(g, tx),
                        grid_centre_y(g, ty), zk + 4.0f };
        *out = zk;
        *storey = floor_of(at);
        return 1;
    }
    *out = zp;
    *storey = sp;
    return 1;
}

// What the game's own F / C made of the key: it still runs AscendFloor /
// DescendFloor on its cursor, and the storey it reached is kept there even
// though navigation sets the cursor's position. Read a moment after the key,
// so the log can say whether the game found a floor where the mod did not.
#define FLOOR_GAME_CHECK_MS 250
static ULONGLONG g_floor_check_at;
static FieldSlot g_cur_requested, g_cur_effective, g_cur_camfloor;

static void floor_game_check(void)
{
    void* cur = cursor_object();
    const void* v;
    int req = -99, eff = -99;
    float cam = -99999.0f;
    if (!cur) return;
    if (field_ptr(cur, "m_iRequestedFloor", &g_cur_requested, sizeof(int32_t), &v))
        req = *(const int32_t*)v;
    if (field_ptr(cur, "m_iLastEffectiveFloorIndex", &g_cur_effective, sizeof(int32_t), &v))
        eff = *(const int32_t*)v;
    // EW only; EU keeps the floor's bounds in other fields.
    if (field_ptr(cur, "m_fLogicalCameraFloorHeight", &g_cur_camfloor, sizeof(float), &v))
        cam = *(const float*)v;
    logf_("nav: the game's own cursor after the key: requested floor %d, reached %d, "
          "camera floor height %.1f\n", req, eff, cam);
}

static void nav_floor(int dir)
{
    CursorGrid g;
    int tx, ty;
    float from, z;
    if (!cursor_grid(&g)) return;
    if (nav_active() && nav_target(&tx, &ty)) {
        from = g_nav_aim ? g_aim_floor : navh_ground();
    } else if (cursor_tile(&g, &tx, &ty, &z)) {
        from = z - NAV_CURSOR_LIFT;
    } else {
        return;
    }
    float to = from;
    int storey = 0, found = 0;
    char seen[512] = "";
    g_floor_check_at = GetTickCount64() + FLOOR_GAME_CHECK_MS;
    GUARDED("nav: floor key",
            found = floor_next(&g, tx, ty, from, dir, &to, &storey, seen, sizeof seen),
            found = 0);
    if (!found) {
        logf_("nav: %s at %d, %d from %.1f -- no floor that way (%s)\n",
              dir > 0 ? "F" : "C", tx, ty, from, seen);
        char missed[128] = "", say[160];
        GUARDED("nav: floor missed",
                floor_missed(tx, ty, from, 0, from, dir, missed, sizeof missed),
                missed[0] = 0);
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s",
                    dir > 0 ? "No floor above here." : "No floor below here.",
                    missed[0] ? " " : "", missed);
        speech_cancel_pending();
        speech_say_now(say);
        return;
    }
    logf_("nav: %s at %d, %d: %.1f -> %.1f, storey %d (%s)\n", dir > 0 ? "F" : "C",
          tx, ty, from, to, storey, seen);
    nav_focus(tx, ty, to, dir > 0 ? "the floor above" : "the floor below");
    // How far: inside a building, in its own floors (asked for 2026-09-27,
    // after "2 storeys up" from a tall ground floor to the one above it);
    // elsewhere, or between two heights on the same floor of it, in storeys
    // of 192 -- not the camera's floor number (tile_height_step).
    int levels = 0;
    if (where_levels_between(tx, ty, from, to, &levels) && levels)
        tile_floor_step(levels, g_step_note, sizeof g_step_note);
    else
        tile_height_step(to - from, g_step_note, sizeof g_step_note);
    {
        char missed[128] = "";
        GUARDED("nav: floor missed",
                floor_missed(tx, ty, from, 1, to, dir, missed, sizeof missed),
                missed[0] = 0);
        if (missed[0]) {
            size_t used = strlen(g_step_note);
            _snprintf_s(g_step_note + used, sizeof g_step_note - used, _TRUNCATE, " %s", missed);
        }
    }
    g_floor_hold = 1;
    g_floor_hold_z = to;
    surface_note(tx, ty, from);
    surface_note(tx, ty, to);
    // Arrival counted the soldier as on this tile whatever their floor. Their
    // own tile is described on a timer, since no path is built to it -- but
    // on another floor of the column it gets a path like any other.
    int mine = 0;
    for (int i = 0; i < g_step_nunits; i++)
        if (g_step_units[i].mine && scan_storey_diff(g_step_units[i].feet, to) == 0) mine = 1;
    if (!mine) g_tile_due = 0;
}

// ---- who is in a blast ----------------------------------------------------
//
// Who an area attack would hit, as the game marks them while it is aimed.
// XGAction_Targeting.DrawSplashRadius, on every update of an aim at a spot
// (a rocket, a grenade, and the other blasts it lists), works out the centre
// and the radius and hands them to the native UpdateShotTargetLocation, which
// marks the actors inside (MarkTargetedActors) and keeps them in the action's
// m_arrMarkedTargets -- the highlight the sighted player sees -- beside the
// radius it used, m_fSplashRadiusCache. So the list is read as marked, not
// worked out again. Said a moment after each numpad aim step, once the game
// has drawn the blast at the new spot, queued behind the step and its odds:
// "In the blast: Sectoid, White." A blast with nobody in it says so.
#define BLAST_DELAY_MS 200
static ULONGLONG g_blast_due;
static FieldSlot g_marked_slot, g_splash_slot;

// Whether a destructible is cover, and how high: 2 high, 1 low, 0 not cover.
// Nothing on the actor says so -- ShouldIgnoreForCover is native and only
// rules things out -- so the game's own cover map is asked, as the tile
// readout asks it: the tiles just outside the object's bounds (its mesh's
// world box, PrimitiveComponent.Bounds), and whether the cover point there
// faces the object. North is +Y and the game's East is -X (see tile.h), so a
// tile on the object's -X side needs the game's West bit.
static FieldSlot g_blast_bounds, g_blast_health, g_blast_smc, g_blast_mesh;
static int destructible_cover(void* a)
{
    const void* v;
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;
    if (!field_ptr(a, "StaticMeshComponent", &g_blast_smc, sizeof(void*), &v) ||
        !*(void* const*)v)
        return 0;
    void* smc = *(void* const*)v;
    if (!field_ptr(smc, "Bounds", &g_blast_bounds, 7 * sizeof(float), &v)) return 0;
    const float* b = (const float*)v;           // Origin, BoxExtent, SphereRadius
    int x0 = grid_x(&g, b[0] - b[3] + 8.0f);
    int x1 = grid_x(&g, b[0] + b[3] - 8.0f);
    int y0 = grid_y(&g, b[1] - b[4] + 8.0f);
    int y1 = grid_y(&g, b[1] + b[4] - 8.0f);
    if (x1 < x0) x1 = x0;                       // thinner than a tile
    if (y1 < y0) y1 = y0;
    if (x1 - x0 > 12) x1 = x0 + 12;             // a long wall: its first stretch
    if (y1 - y0 > 12) y1 = y0 + 12;
    float z = b[2] - b[5] + 4.0f;               // the floor it stands on, as asked of a tile

    int best = 0;
    for (int side = 0; side < 4; side++) {
        int along = side < 2 ? x1 - x0 : y1 - y0;
        for (int i = 0; i <= along; i++) {
            int x, y, bit;
            switch (side) {
            case 0:  x = x0 + i; y = y0 - 1; bit = TILE_COVER_N; break;   // south of it
            case 1:  x = x0 + i; y = y1 + 1; bit = TILE_COVER_S; break;   // north of it
            case 2:  x = x0 - 1; y = y0 + i; bit = TILE_COVER_W; break;   // west of it
            default: x = x1 + 1; y = y0 + i; bit = TILE_COVER_E; break;   // east of it
            }
            if (x < 0 || y < 0 || x >= g.num_x || y >= g.num_y) continue;
            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            float wx = grid_centre_x(&g, x);
            float wy = grid_centre_y(&g, y);
            if (!cover(world, NULL, wx, wy, z, &cp) || cp.x != x || cp.y != y ||
                (cp.flags & TILE_COVER_DIAGONAL) || !(cp.flags & bit))
                continue;
            int level = (cp.flags & (bit << 4)) ? 1 : 2;   // the matching low bit
            if (level > best) best = level;
            if (best == 2) return 2;
        }
    }
    return best;
}

static void blast_say(void)
{
    void* unit = soldier_unit();
    const void* v;
    if (!unit || !field_ptr(unit, "m_kCurrAction", &g_nav_curr_action, sizeof(void*), &v)) return;
    void* action = *(void* const*)v;
    char name[64];
    if (!action || !unit_is_live(action) || !object_name(action, name, sizeof name) ||
        strncmp(name, "XGAction_Targeting", 18) != 0)
        return;
    if (!field_ptr(action, "m_fSplashRadiusCache", &g_splash_slot, sizeof(float), &v)) return;
    float radius = *(const float*)v;
    if (!(radius > 0.0f)) return;                 // not an area attack
    if (!field_ptr(action, "m_arrMarkedTargets", &g_marked_slot, sizeof(FArray), &v)) return;
    const FArray* arr = (const FArray*)v;
    int num = arr->Num;
    void* const* data = (void* const*)arr->Data;
    if (num < 0 || num > 64 || (num && !readable(data, (size_t)num * sizeof(void*)))) return;

    // The marked actors are units or their pawns, matched against the units
    // the squad can see, which is what the screen can highlight. The squad's
    // own are said apart -- the first run had the rocket's own heavy in it as
    // "In the blast: Vargas, Dozer.", which also sounded like two people -- so
    // a soldier is named by surname alone there.
    const char* them[TILE_NAMES_MAX];
    const char* ours[TILE_NAMES_MAX];
    int nthem = 0, nours = 0, tthem = 0, tours = 0;
    unsigned char matched[64] = { 0 };
    void* squad = squad_player();
    // MarkTargetedActors marks every unit in the radius, hidden or not, and
    // a hidden one's highlight is never drawn -- so an alien or civilian is
    // named only while the squad sees them. It still counts as matched: it
    // is a unit, and must not be offered again as a destructible below.
    static SquadSight sight;
    squad_sight_take(squad, &sight);
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        int in = 0;
        for (int k = 0; k < num; k++)
            if (data[k] == s.unit || data[k] == s.pawn) { in = 1; matched[k] = 1; }
        if (!in) continue;
        if (!squad_sees(&sight, s.unit, s.loc, s.friendly, s.who->name)) continue;
        if (s.friendly) { if (nours < TILE_NAMES_MAX) ours[nours++] = s.who->name; tours++; }
        else            { if (nthem < TILE_NAMES_MAX) them[nthem++] = s.who->name; tthem++; }
    }
    // And those with no flag: a survivor in the blast went unmentioned,
    // being neither a flag's unit nor a destructible. `them` holds
    // pointers, so the names stay in `found` until the line is built.
    {
        static FlaglessUnit found[COLUMN_FLAGLESS];
        int nf = squad ? flagless_units(0, found, COLUMN_FLAGLESS) : 0;
        for (int i = 0; i < nf; i++) {
            int in = 0;
            for (int k = 0; k < num; k++)
                if (data[k] == found[i].unit || data[k] == found[i].pawn) { in = 1; matched[k] = 1; }
            if (!in || !squad_sees(&sight, found[i].unit, found[i].loc, 0, found[i].name))
                continue;
            if (nthem < TILE_NAMES_MAX) them[nthem++] = found[i].name;
            tthem++;
        }
    }

    // The destructibles. Only what explodes is named -- a car or a gas
    // canister chains on, which is worth hearing on every step; the rest,
    // cover chunks, posters, rubble, is a count. Named after their mesh, as
    // the scanner names them, and told from the rest by the scanner's own
    // list of blast owners (scan_describe_explosive). Already destroyed ones
    // (Health 0, as BeginDestroyed leaves them) are left out. The blast
    // owners are brought up to date first: the scanner and the door sounds
    // are what refresh them otherwise, and either may not have run here.
    {
        CursorGrid bg;
        if (cursor_grid(&bg)) world_refresh(&bg);
    }
    // Three lists: what explodes, and what is high or low cover
    // (destructible_cover). Anything else is a count.
    static char names[3][TILE_NAMES_MAX][SCAN_NAME];
    const char* namep[3][TILE_NAMES_MAX];
    int kept[3] = { 0 }, total[3] = { 0 }, others = 0, wrecked = 0, dressing = 0;
    // The meshes behind the words, for the log: what each name came from.
    char meshes[512] = "";
    size_t mused = 0;
    for (int k = 0; k < num; k++) {
        void* a = data[k];
        if (matched[k] || !a || !unit_is_live(a) || !object_is_a(a, "XComDestructibleActor"))
            continue;
        if (field_ptr(a, "Health", &g_blast_health, sizeof(int32_t), &v) &&
            *(const int32_t*)v <= 0) { wrecked++; continue; }
        char mesh[SCAN_NAME] = "";
        if (field_ptr(a, "StaticMeshComponent", &g_blast_smc, sizeof(void*), &v) &&
            *(void* const*)v &&
            field_ptr(*(void* const*)v, "StaticMesh", &g_blast_mesh, sizeof(void*), &v) &&
            *(void* const*)v)
            object_name(*(void* const*)v, mesh, sizeof mesh);
        int explodes = world_explodes(a);
        int list;
        if (explodes) list = 0;
        else {
            // Graffiti, a poster, a decal: flat on a wall, so the cover beside
            // it is the wall's. The 12:05 log of 2026-09-28 had "High cover:
            // .. Graffiti decals" in a blast.
            if (scan_mesh_is_dressing(mesh)) { dressing++; others++; continue; }
            int c = destructible_cover(a);
            if (!c) { others++; continue; }
            list = c == 2 ? 1 : 2;
        }
        total[list]++;
        if (kept[list] >= TILE_NAMES_MAX) continue;
        char* out = names[list][kept[list]];
        scan_mesh_words(mesh, list ? "Cover" : "Explosive", out, SCAN_NAME);
        namep[list][kept[list]++] = out;
        if (mused < sizeof meshes) {
            int w = _snprintf_s(meshes + mused, sizeof meshes - mused, _TRUNCATE, "%s%s",
                                mused ? ", " : "", mesh[0] ? mesh : "?");
            if (w > 0) mused += (size_t)w;
        }
    }

    // "In the blast: 2 Floaters. Squad in the blast: Vargas. Explodes: Car.
    // High cover: Wall. Low cover: 2 Crates. 8 other objects."
    char say[768], text[256];
    size_t used = 0;
    say[0] = 0;
    if (tthem) {
        tile_names_counted(them, nthem, tthem, text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    "In the blast: %s.", text);
    }
    if (tours && used < sizeof say) {
        tile_names_counted(ours, nours, tours, text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    "%sSquad in the blast: %s.", used ? " " : "", text);
    }
    if (!tthem && !tours) {
        strcpy_s(say, sizeof say, "No one in the blast.");
        used = strlen(say);
    }
    static const char* heads[3] = { "Explodes", "High cover", "Low cover" };
    for (int l = 0; l < 3; l++) {
        if (!total[l] || used >= sizeof say) continue;
        tile_names_counted(namep[l], kept[l], total[l], text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    " %s: %s.", heads[l], text);
    }
    if (others && used < sizeof say)
        _snprintf_s(say + used, sizeof say - used, _TRUNCATE, " %d other object%s.", others,
                    others == 1 ? "" : "s");
    logf_("blast: radius %.0f, %d marked, %d of them, %d of ours, %d explode, %d high cover, "
          "%d low cover, %d other objects (%d on a wall), %d wrecked -> \"%s\"  [%s]\n",
          radius, num, tthem, tours, total[0], total[1], total[2], others, dressing, wrecked,
          say, meshes);
    if (g_speak) speech_say(say);
}

static void blast_poll(void)
{
    if (!g_blast_due || GetTickCount64() < g_blast_due) return;
    g_blast_due = 0;
    if (!g_nav_aim) return;
    GUARDED("blast: read", blast_say());
}

// ---- every frame in a mission ----------------------------------------------
//
// From the battle cursor's per-frame native: the numpad, the menus, the
// scanner's keys, and the sounds last, so a step taken this frame is heard.
static void nav_poll(void)
{
    // Practice owns the numpad while it is on (learn.h). Navigation stands
    // aside completely rather than filtering the keys one at a time: a held
    // target with nobody reading the keys is a cursor stuck where it was left,
    // so the target is released and the field is left to practice to drive.
    if (learn_active()) {
        if (nav_active()) nav_stop("sound practice");
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        scan_keys_forget();
        g_radar_down[0] = g_radar_down[1] = 0;
        g_walls_down = 0;
        g_glide_digit = 0;
        g_glide_steps = 0;
        walls_rescan();
        return;
    }

    blast_poll();

    if (nav_active()) {
        void* pawn = NULL;
        POINT m;
        if (cursor_object() != g_nav_cursor) {
            nav_stop("the cursor was replaced");
        } else if (cursor_chained_pawn(&pawn) && pawn != g_nav_pawn) {
            nav_stop("the soldier changed");
        } else if (GetCursorPos(&m) &&
                   (labs(m.x - g_nav_mouse.x) > 2 || labs(m.y - g_nav_mouse.y) > 2)) {
            nav_stop("the mouse moved");
        } else if (g_tile_due && GetTickCount64() >= g_tile_due) {
            // A reached tile, described on the frame after its first path
            // (TILE_DESCRIBE_DELAY_MS), outside the pathfinder's own call.
            int tx, ty;
            g_tile_due = 0;
            if (nav_target(&tx, &ty) && tx == g_tile_due_at[0] && ty == g_tile_due_at[1]) {
                GUARDED("nav: path end", path_note(tx, ty, navh_ground()));
                char what[TILE_MAX_TEXT] = "";
                Fault f;
                __try {
                    if (!tile_report(tx, ty, navh_ground(), g_tile_due_dash, 0,
                                     what, sizeof what))
                        what[0] = 0;
                }
                __except (fault_note(GetExceptionInformation(), &f)) {
                    fault_log("tile: report", &f, NULL);
                    what[0] = 0;
                }
                height_step(tx, ty, navh_ground());
                // Asked only when it will be said: the crossing is kept as
                // heard, and one worked out for a step nobody hears is lost.
                int late = g_step_late && what[0] &&
                           tx == g_step_late_at[0] && ty == g_step_late_at[1];
                if (g_step_pending || late)
                    where_say(tx, ty, navh_ground(), 0, g_step_where, sizeof g_step_where);
                if (!nav_step_say(what) && late) {
                    g_step_late = 0;
                    char both[sizeof g_step_where + TILE_MAX_TEXT];
                    _snprintf_s(both, sizeof both, _TRUNCATE, "%s%s%s", g_step_where,
                                g_step_where[0] ? " " : "", what);
                    g_step_where[0] = 0;
                    logf_("nav: %d, %d described late -- \"%s\"\n", tx, ty, both);
                    speech_say_now(both);
                }
            }
        } else if (!g_nav_aim && navh_poll(GetTickCount64()) == NAVH_NO_PATH) {
            logf_("nav: %d, %d has no path on its floor %.1f\n",
                  g_nav_path_tile[0], g_nav_path_tile[1], navh_ground());
            nav_say_no_path(g_nav_path_tile[0], g_nav_path_tile[1]);
        } else if (g_step_pending && GetTickCount64() >= g_step_deadline) {
            // How many times the game computed a path while this step was
            // waiting. Zero means the game never tried, which is a different
            // fault from a path that came back late, and the two are not
            // otherwise distinguishable from out here: both look like
            // silence. XGAction_Path.m_bDoPathingTick is what gates it, and
            // Mouse_CheckForPathing clears that whenever the Flash hit test
            // says the mouse was consumed -- so a run of zeroes here should
            // be read next to the "Flash hit test" lines.
            logf_("nav: nothing decided the tile in %d ms (%ld path calls) "
                  "-- saying what is known\n", STEP_FALLBACK_MS, g_path_calls);
            nav_step_say("");
            g_step_late = nav_target(&g_step_late_at[0], &g_step_late_at[1]);
        } else if (!g_nav_parked && g_nav_placed_at < g_nav_key_at &&
                   GetTickCount64() - g_nav_key_at > 300) {
            nav_park_mouse();
        }
    }

    mouse_hold_poll();

    if (!game_has_focus()) {
        // Forget what was held, so a key released while the game was in the
        // background does not read as a fresh press on return.
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        scan_keys_forget();
        g_radar_down[0] = g_radar_down[1] = 0;
        g_walls_down = 0;
        g_glide_digit = 0;
        g_glide_steps = 0;
        // And the walls go quiet: a field playing on over another window is
        // describing a game the player is not looking at.
        walls_quiet();
        return;
    }
    // The ability menu holds the numpad while it is open, as practice does;
    // so does the unit information screen (F1) while it is up.
    // Insert's list is polled on its own thread (review_pump); while it is
    // open the numpad is its, as before.
    // The mission summary: the battle is over.
    if (msum_screen_up()) {
        if (nav_active()) nav_stop("the mission summary");
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        g_glide_digit = 0;
        g_glide_steps = 0;
        return;
    }
    if (info_poll() || abar_menu_poll() || history_is_open()) {
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        g_glide_digit = 0;
        g_glide_steps = 0;
        return;
    }

    ULONGLONG now = GetTickCount64();
    for (int d = 0; d <= 9; d++) {
        int down = (GetAsyncKeyState(VK_NUMPAD0 + d) & 0x8000) != 0;
        if (down && !g_numpad_down[d]) {
            g_numpad_at[d] = now;
            nav_press(d, 0);
        }
        g_numpad_down[d] = down;
    }

    // Which direction is being held. The one pressed most recently wins, so
    // rolling from one key to the next turns the glide instead of arguing
    // with it.
    int held = 0;
    for (int d = 1; d <= 9; d++) {
        int dx, dy;
        if (!g_numpad_down[d] || !nav_step_for_digit(d, &dx, &dy)) continue;
        if (!held || g_numpad_at[d] > g_numpad_at[held]) held = d;
    }

    if (!held) {
        // Let go. The tile it stopped on is the one worth describing, and the
        // only one the glide says anything about.
        if (g_glide_digit && g_glide_steps > 0) {
            int tx, ty;
            logf_("nav: glide of %d step%s ends\n", g_glide_steps,
                  g_glide_steps == 1 ? "" : "s");
            if (nav_active() && nav_target(&tx, &ty)) nav_arrive(tx, ty);
        }
        g_glide_digit = 0;
        g_glide_steps = 0;
    } else if (held != g_glide_digit) {
        int was_gliding = g_glide_digit && g_glide_steps > 0;
        g_glide_digit = held;
        // A fresh hold waits out NAV_HOLD_MS so that a tap is a tap. A turn
        // taken mid-glide does not: the player is already moving and a pause
        // there would read as the key being missed.
        if (!was_gliding) {
            g_glide_steps = 0;
            g_glide_next = g_numpad_at[held] + NAV_HOLD_MS;
        }
    } else if (now >= g_glide_next) {
        nav_press(held, 1);
        if (g_glide_steps < NAV_GLIDE_RAMP) g_glide_steps++;
        g_glide_next = now + glide_interval(g_glide_steps);
    }

    // The radar: numpad + for enemies, numpad - for the squad. Neither key is
    // bound in [XComGame.XComTacticalInput], so, like the digits, they never
    // reach the game and need no swallowing.
    static const int radar_keys[2] = { VK_ADD, VK_SUBTRACT };
    for (int k = 0; k < 2; k++) {
        int down = (GetAsyncKeyState(radar_keys[k]) & 0x8000) != 0;
        if (down && !g_radar_down[k]) {
            GUARDED("radar", radar(k == 1));
        }
        g_radar_down[k] = down;
    }
    // Numpad *: the wall field off and on. Answered in words, because a
    // feature that has just gone quiet cannot announce itself with a sound.
    int walls = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    if (walls && !g_walls_down) {
        int on = settings_step(SET_FIELD, 1);
        logf_("walls: field %s\n", on ? "on" : "off");
        speech_say_now(on ? "Wall sound on." : "Wall sound off.");
    }
    g_walls_down = walls;

    // F and C: the target a storey up or down. They are the game's own keys
    // for it and reach the game as well; see nav_floor.
    static const int floor_keys[FLOOR_KEYS] = { 'F', 'C' };
    for (int k = 0; k < FLOOR_KEYS; k++) {
        int down = (GetAsyncKeyState(floor_keys[k]) & 0x8000) != 0;
        if (down && !g_floor_down[k]) nav_floor(k == 0 ? 1 : -1);
        g_floor_down[k] = down;
    }
    // M: the mission's objectives, as the HUD lists them. M is bound in no
    // section of DefaultInput.ini or BaseInput.ini, so it never reaches the
    // game.
    static int mission_key_down;
    int mkey = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mkey && !mission_key_down) {
        char say[MISSION_TEXT];
        mission_list(say, sizeof say);
        if (mission_visible() == 0) {
            logf_("mission: M, the list hidden: \"%s\"\n", say);
            strcpy_s(say, sizeof say, "No objectives on screen.");
        }
        logf_("mission: M -> \"%s\"\n", say);
        speech_cancel_pending();
        speech_say_now(say);
    }
    mission_key_down = mkey;

    // X: the game's weapon switch. Only noted; the change it makes is said
    // when the HUD redraws the equipped weapon (weapon_note).
    int x = (GetAsyncKeyState('X') & 0x8000) != 0;
    if (x && !g_weapon_x_down) weapon_x_pressed();
    g_weapon_x_down = x;

    if (g_floor_check_at && GetTickCount64() >= g_floor_check_at) {
        g_floor_check_at = 0;
        GUARDED("nav: floor check", floor_game_check());
    }

    // Delete: the selected soldier (soldier.h). Its only binding, Camera
    // Default, is removed with -Bindings in [Engine.PlayerInput], so like
    // Insert it never reaches the game.
    static int soldier_key_down;
    int soldier_key = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (soldier_key && !soldier_key_down) {
        GUARDED("soldier: readout", soldier_readout());
    }
    soldier_key_down = soldier_key;

    // The scanner: Page Up, Page Down, Home, End. Read here rather than in a
    // poll of its own so it shares the guards this one already applies --
    // practice has taken the keys, the game has the foreground.
    scan_poll();

    // Last, so a step taken this frame is already in the target the field
    // listens from. The hearts listen from the same tile. Both timed for the
    // perf line, which is how a slowdown blamed on the sounds is settled.
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    walls_poll();
    QueryPerformanceCounter(&t1);
    hearts_poll();
    QueryPerformanceCounter(&t2);
    perf_note(t1.QuadPart - t0.QuadPart, t2.QuadPart - t1.QuadPart);
}

static void cursor_watch(void* self)
{
    cursor_seen(self);

    // Every frame, not at the watch's four times a second: a key press lasts
    // a few frames, and a quarter-second poll would drop quick ones.
    if (cursor_resolved()) nav_poll();
    GUARDED("move: poll", move_poll());
    combat_poll();
    GUARDED("soldier: poll", soldier_poll());
    GUARDED("mission: poll", mission_poll());
    GUARDED("sight: poll", sight_poll());
    GUARDED("info: settle", info_settle());

    ULONGLONG now = GetTickCount64();
    if (now - g_cursor_at < CURSOR_WATCH_MS) return;
    g_cursor_at = now;

    char why[256];
    int fields = cursor_fields(why, sizeof why);
    if (!fields) {
        // Said once per cursor, not once per frame: g_tried latches inside
        // cursor.c, so a failure reports itself and then stays quiet.
        if (strcmp(why, "already failed") != 0)
            logf_("cursor: %s\n", why);
        return;
    }
    // Success is worth a line too: these are the offsets anything that writes
    // into the cursor will be trusting.
    if (fields == 2) logf_("cursor: %s\n", why);

    // The grid's origin, off XComWorldData. Waiting for GetWorldData to be
    // called is not a failure and is not logged; the tile then reads "?".
    char grid_why[256];
    int grid = cursor_world_fields(grid_why, sizeof grid_why);
    if (grid == 2)
        logf_("grid: %s\n", grid_why);
    else if (!grid && strcmp(grid_why, "already failed") != 0 &&
             strcmp(grid_why, "no world data yet") != 0 &&
             strcmp(grid_why, "UObject::Class not probed yet") != 0)
        logf_("grid: %s\n", grid_why);

    CursorGrid g;
    int have_grid = grid && cursor_grid(&g);
    if (have_grid && memcmp(&g, &g_grid_last, sizeof g) != 0) {
        g_grid_last = g;
        logf_("grid: Min %.1f, %.1f, %.1f  size %d x %d x %d tiles\n",
              g.min_x, g.min_y, g.min_z, g.num_x, g.num_y, g.num_z);
    }

    float x, y, z;
    if (!cursor_position(&x, &y, &z)) return;
    if (x == g_cursor_last[0] && y == g_cursor_last[1] && z == g_cursor_last[2])
        return;
    g_cursor_last[0] = x; g_cursor_last[1] = y; g_cursor_last[2] = z;

    if (!have_grid) {
        logf_("cursor: at %.1f, %.1f, %.1f  tile ? (%s)\n", x, y, z,
              grid ? "grid unreadable" : grid_why);
        return;
    }

    // The native's own arithmetic (see cursor.h): floor((pos - Min) / 96).
    // The fraction is printed so the log can confirm it rather than trust it
    // -- a cursor at rest in the middle of a tile should read +0.50 on both
    // axes, and anything else means Min is not the origin the native uses.
    int tx = grid_x(&g, x);
    int ty = grid_y(&g, y);
    float fx = (x - g.min_x) / CURSOR_TILE - (float)tx;
    float fy = (y - g.min_y) / CURSOR_TILE - (float)ty;
    int off_grid = tx < 0 || ty < 0 || tx >= g.num_x || ty >= g.num_y;
    logf_("cursor: at %.1f, %.1f, %.1f  tile %d, %d (+%.2f, +%.2f)%s\n",
          x, y, z, tx, ty, fx, fy, off_grid ? "  OFF GRID" : "");
}

// Hands over XComWorldData, which owns the grid's origin. The native is static
// and writes the object into Result unconditionally (both builds), so the
// object is read after the original has run. Half the tactical script calls
// this, so the hook does nothing beyond one pointer comparison.
ExecFn g_orig_worlddata;
static LONG g_worlddata_faulted;
void __fastcall hook_worlddata(void* self, void* edx,
                                      void* stack, void* result)
{
    g_orig_worlddata(self, edx, stack, result);
    __try { cursor_world_seen(*(void**)result); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_worlddata_faulted, 1))
            logf_("grid: reading GetWorldData's result faulted (0x%08lx)\n",
                  GetExceptionCode());
    }
}

ExecFn g_orig_cursormode;
void __fastcall hook_cursormode(void* self, void* edx,
                                       void* stack, void* result)
{
    GUARDED("cursor: watch", cursor_watch(self));
    g_orig_cursormode(self, edx, stack, result);
}

// Where navigation takes over the cursor.
//
// Every placement of the cursor ends in GetClosestValidCursorPosition, called
// with the position the script wants: from getValidLocation(Vector NewLoc) in
// EW, straight from CursorSetLocation(Vector NewLoc, ...) in EU. Either way the
// position is the *caller's first parameter*, and the native reads it out of
// the caller's frame when it runs -- so writing the target there just before
// the original is called is the whole substitution. What comes back is the
// game's own answer: the nearest position a cursor may occupy.
//
// Only the placement that follows the mouse is taken over. The script frames
// above are walked for Mouse_CheckForPathing, which is declared once per build,
// on XComTacticalInput, and exists only while the soldier is choosing where to
// move. The other callers -- MoveToUnit when the soldier changes, the aiming
// camera -- are left alone.
#define NAV_CHAIN_DEPTH 6

static int g_nav_chain_logged;

static int nav_substitute(void* stack)
{
    char chain[512] = "";
    size_t used = 0;
    int from_mouse = 0;
    void* frame = stack;

    for (int depth = 0; frame && depth < NAV_CHAIN_DEPTH; depth++) {
        if (!readable(frame, FFRAME_PREVIOUS + sizeof(void*))) break;
        void* node = *(void**)((uint8_t*)frame + FFRAME_NODE);
        char name[128];
        if (!object_name(node, name, sizeof name) || !name[0]) break;
        used += (size_t)_snprintf_s(chain + used, sizeof chain - used, _TRUNCATE,
                                    depth ? " <- %s" : "%s", name);
        if (used >= sizeof chain) used = sizeof chain - 1;
        if (strcmp(name, "Mouse_CheckForPathing") == 0) { from_mouse = 1; break; }
        if (strcmp(name, "Mouse_CheckForFreeAim") == 0) {
            // The aim. Its tile was put in at ProcessChainedDistance, ahead
            // of the range leash (nav_aim_substitute); writing it again here,
            // after the leash, would undo the game's clamp. Reported as a
            // placement so what the game made of it is heard.
            static int aim_logged;
            if (!g_nav_aim) return 0;
            if (!aim_logged) {
                aim_logged = 1;
                logf_("nav: aim placement call chain %s\n", chain);
            }
            return 1;
        }
        frame = *(void**)((uint8_t*)frame + FFRAME_PREVIOUS);
    }

    // The chain is the evidence that FFRAME_PREVIOUS is right, so it is put on
    // record the first time navigation meets this native.
    if (!g_nav_chain_logged && from_mouse) {
        g_nav_chain_logged = 1;
        logf_("nav: placement call chain %s\n", chain);
    }
    if (!from_mouse) {
        // And if the mouse's placement is never recognised, the chains that
        // were seen instead are what says why -- a few, not one per frame.
        static int misses;
        if (!g_nav_chain_logged && misses < 3) {
            misses++;
            logf_("nav: not a mouse placement: %s\n", chain);
        }
        return 0;
    }

    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        return 0;

    // The first parameter, by position: NewLoc in both builds.
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            float* v = (float*)(locals + off);
            if (off >= 0x1000 || !writable(v, 3 * sizeof(float))) return 0;
            // A vector the mouse picked is a world position. Anything else in
            // this slot means the frame is not the one described above.
            for (int i = 0; i < 3; i++)
                if (!(v[i] > -1.0e6f && v[i] < 1.0e6f)) return 0;
            // X and Y only. The height is the ground under the target, which
            // hook_floorz has already had the game work out for this frame.
            // Writing the cursor's own height here instead raised it by its
            // collision height on every step -- getValidLocation adds that to
            // whatever comes back -- until it hung hundreds of units in the
            // air and ClickToPath read the move as a hover it could not make.
            v[0] = g_nav_world[0];
            v[1] = g_nav_world[1];
            return 1;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return 0;
}

// Where an aim step landed. The game's range leash may have pulled it short of
// the tile asked for; then the aim IS where it landed, and navigation follows
// it there, so the next step goes on from where the aim really is and the
// coordinates spoken are the ones a shot would go to.
static void nav_aim_landed(const CursorGrid* g, int px, int py)
{
    int tx, ty;
    if (!nav_target(&tx, &ty)) return;
    // Who the blast takes in, once the game has drawn it here (blast_poll).
    g_blast_due = GetTickCount64() + BLAST_DELAY_MS;
    // What the aim hits from here is heard after the coordinates, even when
    // it is what the last tile had: the odds (brief, see shot_set_brief) and
    // whether the shot is blocked. The game resends the blocked message only
    // when it changes, so that one is said only if it does.
    shot_forget_said();
    g_reticle_said[0] = 0;
    if (px == tx && py == ty) {
        nav_step_say("");
        return;
    }
    logf_("nav: aim for %d, %d held at %d, %d -- out of range\n", tx, ty, px, py);
    nav_begin(px, py);
    g_nav_world[0] = grid_centre_x(g, px);
    g_nav_world[1] = grid_centre_y(g, py);
    g_aim_floor = aim_floor(g, px, py, g_aim_floor);
    g_nav_world[2] = g_aim_floor;
    nav_describe(px, py, g_step_coords, sizeof g_step_coords);
    // Who was found on arrival stands on the tile asked for, not this one.
    g_step_nunits = 0;
    nav_step_say("Out of range.");
}

// What the game made of the target. Logged when the tile changes, so the log
// shows each step landing -- or being moved somewhere else by the validation.
static void nav_placed(const float* v)
{
    g_nav_placed_at = GetTickCount64();
    CursorGrid g;
    int tx, ty;
    if (!cursor_grid(&g) || !readable(v, 3 * sizeof(float))) return;
    int px = grid_x(&g, v[0]);
    int py = grid_y(&g, v[1]);
    // An aim is announced as it lands, and before the same-tile check below:
    // a step pushed against the range limit lands where the last one did.
    if (g_nav_aim && g_step_pending) nav_aim_landed(&g, px, py);
    if (px == g_nav_tile_logged[0] && py == g_nav_tile_logged[1]) return;
    g_nav_tile_logged[0] = px;
    g_nav_tile_logged[1] = py;
    nav_target(&tx, &ty);
    logf_("nav: placed on %d, %d (%.1f, %.1f, %.1f)%s\n", px, py, v[0], v[1], v[2],
          (px == tx && py == ty) ? "" : "  -- NOT the target");
}

// The ground under the target.
//
// GetAdjustedMousePickPoint -- declared once per build, on XComTacticalInput --
// opens with
//
//     fGroundLocation = kWorldData.GetFloorZForPosition(kHUD.CachedHitLocation);
//
// and then takes the tile and the cursor's height from that same
// CachedHitLocation. The HUD fills it from a mouse trace every frame. Writing
// the target into it just before this native reads it makes the whole pick --
// ground height, tile, snap -- come out for the target instead of the mouse,
// for this frame only; the next trace overwrites it again.
//
// The HUD is found by shape: the local in that frame whose object has a
// property called CachedHitLocation. Found once, then recognised by pointer.
static void*    g_pick_node;
static uint32_t g_pick_hud_local;       // offset of the HUD local in the frame
static void*    g_pick_hud;
static uint32_t g_pick_hit_off;         // CachedHitLocation, on the HUD
static int      g_pick_logged;

// ---- the pick the mouse has to agree to -----------------------------------
//
// For a soldier who is not flying, GetAdjustedMousePickPoint ends
//
//     if(kHUD.CachedMouseInteractionInterface != none) { ... return true; }
//     return false;
//
// and Mouse_CheckForPathing places the cursor only when it returns true. That
// interface is whatever the *mouse's own* trace hit this frame -- an actor
// inside the level volume, standing on a floor, from
// XComTacticalHUD.GetMousePickActor. So when the mouse rests where its ray
// hits nothing worth picking -- over the HUD, off the map, or on the sky after
// the camera panned out from under it -- the pick is refused, and with it the
// whole chain navigation rides on: no CursorSetLocation, no floor snap, no
// GetClosestValidCursorPosition, no path. The target moves and the game never
// hears of it, so every step falls through to STEP_FALLBACK_MS and comes out
// as bare coordinates a second and a half late. In the log of 2026-09-20 that
// started on the first tile after a release and never recovered: thirteen
// steps in a row, each one a wait.
//
// The refusal turns on a comparison against none and nothing else -- nothing
// calls the interface between that test and the placement -- so the last actor
// the mouse really did hit is lent back for exactly that stretch and taken out
// again before anything can read it. Nothing is invented: until the mouse has
// picked something at least once this navigation, the frame is left as it was.
static uint32_t g_pick_iface_off;       // CachedMouseInteractionInterface
static void*    g_pick_iface_seen[2];   // the last one the mouse itself hit
static void*    g_pick_iface_cursor;    // the cursor it was seen under
static void**   g_pick_iface_lent;      // where it was lent, to take back
static int      g_pick_iface_state = -1;

// An UnrealScript interface is two pointers: the object, then its interface
// table. Both are put back, and only if they are still the ones lent.
static void nav_return_interface(void)
{
    void** slot = g_pick_iface_lent;
    if (!slot) return;
    g_pick_iface_lent = NULL;
    if (!writable(slot, 2 * sizeof(void*))) return;
    if (slot[0] != g_pick_iface_seen[0] || slot[1] != g_pick_iface_seen[1]) return;
    slot[0] = NULL;
    slot[1] = NULL;
}

static void nav_return_interface_guarded(void)
{
    GUARDED("nav: interface", nav_return_interface());
}

static void nav_lend_interface(void* hud)
{
    nav_return_interface();
    if (!g_pick_iface_off) return;
    void** slot = (void**)((uint8_t*)hud + g_pick_iface_off);
    if (!writable(slot, 2 * sizeof(void*))) return;

    if (slot[0]) {                      // the mouse picked something itself
        // Once: whether a real pick holds (object, object), which is what
        // lending the soldier to an aim assumes (nav_aim_lend).
        static int pair_logged;
        if (!pair_logged) {
            pair_logged = 1;
            logf_("nav: a real pick holds %p, %p (%s)\n", slot[0], slot[1],
                  slot[0] == slot[1] ? "the same object twice" : "two different pointers");
        }
        g_pick_iface_seen[0] = slot[0];
        g_pick_iface_seen[1] = slot[1];
        g_pick_iface_cursor = cursor_object();
        // Only worth a line as the answer to one that said it had stopped.
        if (g_pick_iface_state == 0)
            logf_("nav: the mouse picks the map again\n");
        g_pick_iface_state = 1;
        return;
    }

    // Lending a destroyed actor would be read once, by the next frame's
    // mouse-out, so the remembered one is checked for still being an object
    // before it goes back in -- and for having been seen on this map, since a
    // new mission spawns a new cursor and everything the old one pointed at is
    // gone.
    char name[128];
    int alive = g_pick_iface_seen[0] &&
                g_pick_iface_cursor == cursor_object() &&
                readable(g_pick_iface_seen[0], 0x60) &&
                object_name(g_pick_iface_seen[0], name, sizeof name) && name[0];
    const char* what = "lending back the last actor it hit";
    if (!alive) {
        // Nothing remembered: a fresh map, and with the mouse blocked
        // (mouse.h) the player can no longer move it by hand to give the
        // game something to pick, which was the only way out -- the run of
        // 2026-09-25 after the block went in: every step "nothing decided
        // the tile in 1500 ms". The soldier is lent instead, as nav_aim_lend
        // does for an aim: past the test against none,
        // GetAdjustedMousePickPoint reads only CachedHitLocation, never the
        // interface. Kept as seen, as there, so it is what the next miss
        // lends back; a real pick replaces it.
        void* pawn = NULL;
        if (cursor_chained_pawn(&pawn) && pawn && unit_is_live(pawn)) {
            g_pick_iface_seen[0] = pawn;
            g_pick_iface_seen[1] = pawn;
            g_pick_iface_cursor = cursor_object();
            alive = 1;
            what = "none remembered, lending the soldier";
        } else {
            g_pick_iface_seen[0] = NULL;
            g_pick_iface_seen[1] = NULL;
            what = "none to lend, the game will refuse the placement";
        }
    }
    if (alive) {
        slot[0] = g_pick_iface_seen[0];
        slot[1] = g_pick_iface_seen[1];
        g_pick_iface_lent = slot;
    }
    if (g_pick_iface_state != 0) {
        g_pick_iface_state = 0;
        logf_("nav: the mouse picks nothing -- %s\n", what);
    }
}

// Navigation is over. What was lent goes back, but what was *seen* is kept:
// the mouse having picked the map once is the only thing that lets a later
// navigation start placing straight away, and dropping it per navigation left
// "none to lend, the game will refuse the placement" -- thirteen steps of bare
// coordinates, 1.5 s apart, in the run of 2026-09-20. It is dropped when the
// cursor is replaced instead, which is where it actually stops being valid;
// nav_lend_interface makes that check.
static void nav_forget_interface(void)
{
    nav_return_interface_guarded();
    g_pick_iface_state = -1;
}

static int nav_aim_pick(void* stack)
{
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals) return 0;

    if (node != g_pick_node) {
        char name[128];
        if (!object_name(node, name, sizeof name) ||
            strcmp(name, "GetAdjustedMousePickPoint") != 0)
            return 0;

        // Walk the frame's properties for a local holding a HUD.
        void* prop = NULL;
        if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
            prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
        for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
            if (!readable(prop, 0x68)) return 0;
            uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
            uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
            if (!(flags & CPF_PARM) && off < 0x1000 &&
                readable(locals + off, sizeof(void*))) {
                void* obj = *(void**)(locals + off);
                uint32_t hit;
                if (obj && readable(obj, 0x60) &&
                    object_field_offset(obj, "CachedHitLocation", &hit)) {
                    g_pick_node = node;
                    g_pick_hud_local = off;
                    g_pick_hud = obj;
                    g_pick_hit_off = hit;
                    if (!object_field_offset(obj, "CachedMouseInteractionInterface",
                                             &g_pick_iface_off))
                        g_pick_iface_off = 0;
                    break;
                }
            }
            prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        }
        if (node != g_pick_node) {
            if (!g_pick_logged) {
                g_pick_logged = 1;
                logf_("nav: GetAdjustedMousePickPoint has no HUD local -- "
                      "the ground height will come from the mouse\n");
            }
            return 0;
        }
        logf_("nav: pick HUD local +0x%X, CachedHitLocation +0x%X, "
              "CachedMouseInteractionInterface +0x%X\n",
              g_pick_hud_local, g_pick_hit_off, g_pick_iface_off);
    }

    if (!readable(locals + g_pick_hud_local, sizeof(void*))) return 0;
    void* hud = *(void**)(locals + g_pick_hud_local);
    if (hud != g_pick_hud) {
        uint32_t hit;
        if (!hud || !object_field_offset(hud, "CachedHitLocation", &hit)) return 0;
        g_pick_hud = hud;
        g_pick_hit_off = hit;
        if (!object_field_offset(hud, "CachedMouseInteractionInterface",
                                 &g_pick_iface_off))
            g_pick_iface_off = 0;
    }
    float* v = (float*)((uint8_t*)hud + g_pick_hit_off);
    if (!writable(v, 3 * sizeof(float))) return 0;
    v[0] = g_nav_world[0];
    v[1] = g_nav_world[1];
    v[2] = g_nav_aim ? g_nav_world[2] : navh_query_z();
    g_nav_world[2] = v[2];
    // Last, because it decides whether the game will take any of the above.
    nav_lend_interface(hud);
    return 1;
}

// Says in the log when a tile's height changes phase, once per change.
static void nav_log_phase(void)
{
    NavHeightPhase p = navh_phase();
    if (p == g_nav_phase_logged) return;
    g_nav_phase_logged = p;
    if (p == 2) surface_note(g_nav_path_tile[0], g_nav_path_tile[1], navh_ground());
    static const char* names[] = { "searching", "probing heights", "settled", "no height works" };
    logf_("nav: %d, %d height %s, ground %.1f\n", g_nav_path_tile[0], g_nav_path_tile[1],
          names[p], navh_ground());
}

// The frames above a native, named: "IsAttemptingToHover <- ClickToPath <-
// RMouse". What the confirm diagnostics print.
static void frame_chain(void* stack, int depth, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    void* frame = stack;
    for (int d = 0; frame && d < depth; d++) {
        if (!readable(frame, FFRAME_PREVIOUS + sizeof(void*))) break;
        char name[128];
        if (!object_name(*(void**)((uint8_t*)frame + FFRAME_NODE), name, sizeof name) ||
            !name[0])
            break;
        used += (size_t)_snprintf_s(out + used, out_sz - used, _TRUNCATE,
                                    d ? " <- %s" : "%s", name);
        if (used >= out_sz) break;
        frame = *(void**)((uint8_t*)frame + FFRAME_PREVIOUS);
    }
}

// Whether a confirm is recent enough for its consequences to be logged.
static int nav_confirm_window(void)
{
    return g_nav_confirm_at && GetTickCount64() - g_nav_confirm_at <= 2000;
}

ExecFn g_orig_floorz;
void __fastcall hook_floorz(void* self, void* edx, void* stack, void* result)
{
    Fault f;
    int aimed = 0;
    if (g_nav_live) {
        GUARDED("nav: aim pick", aimed = nav_aim_pick(stack));
    }
    g_orig_floorz(self, edx, stack, result);

    __try {
        float z = *(float*)result;
        // A floor F / C chose is held against the game's own answer for the
        // pick. The game does not report every surface it lets a soldier
        // stand on: on 2026-09-27 C chose 96.0 at 31, 48, this pick answered
        // 220.3, the height search settled on that, and the target went back
        // up without a word -- C was silent and the next C started from 220.3
        // again. Only the pick made for the target (aimed), only while held.
        if (aimed && g_floor_hold && fabsf(z - g_floor_hold_z) > FLOOR_HOLD_SLACK &&
            writable(result, sizeof(float))) {
            static int logged;
            if (!logged) {
                logged = 1;
                logf_("nav: the pick's floor %.1f replaced by the held floor %.1f\n", z,
                      g_floor_hold_z);
            }
            z = g_floor_hold_z;
            *(float*)result = z;
        }
        if (aimed && !g_nav_aim) {
            navh_floor_result(g_nav_world[2], z);
            nav_log_phase();
        }

        // ClickToPath calls IsAttemptingToHover only once it has a path, and
        // that is the only reason this native is called from there: seeing
        // it after a confirm proves the click got through to the move.
        if (nav_confirm_window()) {
            char chain[256];
            frame_chain(stack, 4, chain, sizeof chain);
            if (strncmp(chain, "IsAttemptingToHover", 19) == 0)
                logf_("nav: after confirm, %s -- floor %.1f\n", chain, z);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: floor result", &f, NULL);
    }
}

// Whether the game could build a path to where navigation put the cursor.
// XGAction_Path.Perform_ComputePath(Vector vLoc, ...) asks the pathing pawn's
// native ComputePath2(vLoc, ...) with the cursor's location, and ClickToPath
// moves only along what that produced -- an empty path makes a confirm do
// nothing, silently. The destination is the caller's first parameter, as for
// the placement. Logged when the answer or the tile changes, while navigating.
static int   g_path_logged_ok = -1;
static int   g_path_logged_tile[2] = { -1, -1 };
static float g_path_logged_z;

static void nav_path_result(void* self, void* stack, void* result)
{
    if (!nav_active() || !readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return;

    float* dest = NULL;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            if (off < 0x1000 && readable(locals + off, 3 * sizeof(float)))
                dest = (float*)(locals + off);
            break;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }

    int ok = *(int32_t*)result != 0;
    CursorGrid g;
    int tx = -1, ty = -1;
    if (dest && cursor_grid(&g)) {
        tx = grid_x(&g, dest[0]);
        ty = grid_y(&g, dest[1]);
    }
    // After F / C, a path whose end is not on the chosen floor is the one
    // from before the key: the same tile, the old storey.
    int stale = dest && g_floor_hold &&
                fabsf(dest[2] - NAV_CURSOR_LIFT - g_floor_hold_z) > FLOOR_HOLD_SLACK;
    if (dest && !stale && tx == g_nav_path_tile[0] && ty == g_nav_path_tile[1]) {
        g_path_pawn = self;
        NavVerdict v = navh_path_result(dest[2], ok, GetTickCount64());
        nav_log_phase();
        if (v == NAVH_NO_PATH) {
            logf_("nav: %d, %d has no path at any height\n", tx, ty);
            nav_say_no_path(tx, ty);
        } else if (v == NAVH_REACHABLE) {
            logf_("nav: %d, %d reachable, floor %.1f\n", tx, ty, navh_ground());
            g_tile_due = GetTickCount64() + TILE_DESCRIBE_DELAY_MS;
            g_tile_due_at[0] = tx;
            g_tile_due_at[1] = ty;
            g_tile_due_dash = 1;
        }
    }

    // Only the tile being navigated is logged. Every other caller -- the
    // aliens' turn runs ComputePathForAIUnit for each move it considers --
    // filled the log once navigation had been left on.
    if (!dest || tx != g_nav_path_tile[0] || ty != g_nav_path_tile[1]) return;

    // Logged when the answer, the tile or the height changes: probing moves
    // the height with the tile unchanged, and each try is worth a line.
    float dz = dest[2];
    if (ok == g_path_logged_ok && tx == g_path_logged_tile[0] &&
        ty == g_path_logged_tile[1] && dz == g_path_logged_z)
        return;
    g_path_logged_ok = ok;
    g_path_logged_tile[0] = tx;
    g_path_logged_tile[1] = ty;
    g_path_logged_z = dz;

    char name[128] = "?";
    object_name(node, name, sizeof name);
    if (dest)
        logf_("nav: path to %d, %d (%.1f, %.1f, %.1f) %s  [from %s]\n", tx, ty,
              dest[0], dest[1], dest[2], ok ? "built" : "NONE", name);
    else
        logf_("nav: path %s  [from %s, destination unreadable]\n",
              ok ? "built" : "NONE", name);
}

ExecFn g_orig_computepath;
void __fastcall hook_computepath(void* self, void* edx, void* stack, void* result)
{
    InterlockedIncrement(&g_path_calls);
    g_orig_computepath(self, edx, stack, result);
    GUARDED("nav: path result", nav_path_result(self, stack, result));
}

// Whether Flash took a click. InputEvent asks this, through
// TestMouseConsumedByFlash, before a mouse button may reach RMouse.
//
// It is asked several times a frame, so the answer is written when it
// *changes* and once per confirm, rather than every time. One mission left
// 1,370 identical "miss" lines, and each had paid for a frame_chain first --
// five UnrealScript frames walked, a name decoded for each -- to repeat what
// the first line had already said. The chain is now built only for a line
// that is going to be written.
//
// The change is logged whether or not a confirm is recent, because this
// answer is not only about clicks: Mouse_CheckForPathing reads it every frame
// and clears XGAction_Path.m_bDoPathingTick the moment it comes back true,
// which stops the game pathing until a later frame says false again. Cutting
// this line back to the confirm window took away the only evidence of that,
// and a run where nothing was ever pathed could not be told from a run where
// the paths were merely late. A flip is a handful of lines a mission; the
// silence in between is the useful part.
static ULONGLONG g_flash_said_for;
static int       g_flash_last = -1;

ExecFn g_orig_flashhit;
void __fastcall hook_flashhit(void* self, void* edx, void* stack, void* result)
{
    g_orig_flashhit(self, edx, stack, result);
    Fault f;
    __try {
        int hit = *(int32_t*)result != 0;
        int changed = hit != g_flash_last;
        int confirmed = nav_confirm_window() && g_flash_said_for != g_nav_confirm_at;
        if (changed || confirmed) {
            g_flash_last = hit;
            if (confirmed) g_flash_said_for = g_nav_confirm_at;
            char chain[256];
            frame_chain(stack, 5, chain, sizeof chain);
            logf_("nav: Flash hit test %s%s: %s\n",
                  hit ? "HIT -- the mouse is on the HUD, pathing stops" : "miss",
                  confirmed ? ", after confirm" : "", chain);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: flash hit", &f, NULL);
    }
}

// What the game did with a confirm. InputEvent calls the native
// XComEngine.IsAnyMoviePlaying on every bound key and mouse button before it
// acts on it, in both builds, so its caller's frame carries (Cmd, Actionmask)
// -- taken by position, as rewrite_cmd does. Logged only for two seconds after
// Numpad 0, so the log shows whether the right click arrived as 405 and in
// what order, without a line per key for the rest of the mission.
// Reads the Cmd off an InputEvent frame, or -1 when this is not one. The
// native XComEngine.IsAnyMoviePlaying is called from InputEvent on every bound
// key, in both builds, so its caller's frame carries (Cmd, Actionmask) -- by
// position, as rewrite_cmd takes them.
int input_event_cmd(void* stack, int* mask_out)
{
    if (!readable(stack, 0x20)) return -1;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return -1;

    int vals[2], nvals = 0;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS && nvals < 2; guard++) {
        if (!readable(prop, 0x68)) return -1;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000 &&
            readable(locals + off, sizeof(int32_t)))
            vals[nvals++] = *(int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    if (nvals < 1) return -1;
    if (mask_out) *mask_out = nvals == 2 ? vals[1] : 0;
    return vals[0];
}

// Where InputEvent's Cmd lives in its frame -- the first int parameter, as
// input_event_cmd reads it -- so it can be rewritten before InputEvent goes
// on to act on it.
int32_t* input_event_cmd_slot(void* stack)
{
    if (!readable(stack, 0x20)) return NULL;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return NULL;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return NULL;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000)
            return (int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return NULL;
}

// End is the scanner's "how far, and which way" key, and it is also the
// secondary binding for Backspace_Key_Press -- which is PerformEndTurn. So it
// has to be taken away from the game, and InputEvent offers exactly one place
// to do that:
//
//     if(Class'XComGame.XComEngine'.static.IsAnyMoviePlaying())
//     {
//         return;
//     }
//
// which sits above PreProcessCheckGameLogic and everything that acts on a key.
// Forcing that native true for one call makes InputEvent return, and the turn
// does not end.
//
// Only for End. Cmd 512 is raised by Backspace as well -- that is its primary
// binding -- so the key itself decides: End down and Backspace up, or the
// press is left alone. A player who wants to end the turn with Backspace still
// can, and one who wants to with End can hold Backspace... which is why the
// help says End is the scanner's now.
#define INPUT_CMD_BACKSPACE 512

static int input_is_our_end(int cmd)
{
    if (cmd != INPUT_CMD_BACKSPACE) return 0;
    if (!(GetAsyncKeyState(VK_END) & 0x8000)) return 0;
    if (GetAsyncKeyState(VK_BACK) & 0x8000) return 0;
    return 1;
}

static int g_end_swallowed;

static void nav_watch_input(void* stack)
{
    if (!nav_confirm_window()) return;
    if (!readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return;

    int vals[2], nvals = 0;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS && nvals < 2; guard++) {
        if (!readable(prop, 0x68)) return;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000 &&
            readable(locals + off, sizeof(int32_t)))
            vals[nvals++] = *(int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    if (nvals == 2)
        logf_("nav: after confirm, InputEvent %d mask %d\n", vals[0], vals[1]);
}

ExecFn g_orig_moviecheck;
void __fastcall hook_moviecheck(void* self, void* edx, void* stack, void* result)
{
    Fault f;
    int swallow = 0;
    __try {
        nav_watch_input(stack);
        int mask = 0;
        int cmd = input_event_cmd(stack, &mask);
        swallow = input_is_our_end(cmd);
        if (!swallow && abar_menu_swallow(cmd, mask))
            swallow = 2;
        if (swallow == 1 && !g_end_swallowed) {
            g_end_swallowed = 1;
            logf_("scan: End taken from the game (cmd %d, mask %d) -- "
                  "the turn does not end\n", INPUT_CMD_BACKSPACE, mask);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: watch input", &f, NULL);
    }
    g_orig_moviecheck(self, edx, stack, result);

    // After the original, because what is being replaced is its answer.
    if (swallow) {
        __try {
            if (writable(result, sizeof(int32_t))) *(int32_t*)result = 1;
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("scan: swallow End", &f, NULL);
        }
    }
}

// Engine.GetEngine: the first native InputEvent calls, which is the one point
// early enough to rewrite Enter into an ability's number key (see
// abar_menu_pick). Called from all over the script, so it does nothing unless
// a pick is under way.
ExecFn g_orig_getengine;
void __fastcall hook_getengine(void* self, void* edx, void* stack, void* result)
{
    if (abar_pick_pending()) {
        GUARDED("abar: pick", abar_menu_pick(stack));
    }
    g_orig_getengine(self, edx, stack, result);
}

// The aim's tile, put in ahead of the range leash. CursorSetLocation opens
//
//     NewLoc = ProcessChainedDistance(NewLoc);
//
// in both builds, and the native reads NewLoc out of CursorSetLocation's frame
// when it runs -- so the target written there is what the leash clamps, and
// what comes back is where the game will let the aim go. Only while aiming,
// and only for the placement Mouse_CheckForFreeAim makes.
static int nav_aim_substitute(void* stack)
{
    // The frames above are CursorSetLocation again, then Mouse_CheckForFreeAim:
    // the live cursor is XCom3DCursorMouseForCursorVolumes, whose
    // CursorSetLocation calls its parent's, and it is the parent's that calls
    // the leash. The log of 2026-09-22 printed "CursorSetLocation <-
    // CursorSetLocation <- Mouse_CheckForFreeAim"; looking only one frame up
    // found the first and never substituted.
    if (!readable(stack, FFRAME_PREVIOUS + sizeof(void*))) return 0;
    void* above = *(void**)((uint8_t*)stack + FFRAME_PREVIOUS);
    char name[128];
    int from_aim = 0;
    for (int depth = 0; above && depth < 3; depth++) {
        if (!readable(above, FFRAME_PREVIOUS + sizeof(void*)) ||
            !object_name(*(void**)((uint8_t*)above + FFRAME_NODE), name, sizeof name))
            return 0;
        if (strcmp(name, "Mouse_CheckForFreeAim") == 0) { from_aim = 1; break; }
        if (strcmp(name, "CursorSetLocation") != 0) return 0;
        above = *(void**)((uint8_t*)above + FFRAME_PREVIOUS);
    }
    if (!from_aim) return 0;

    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        return 0;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            float* v = (float*)(locals + off);
            if (off >= 0x1000 || !writable(v, 3 * sizeof(float))) return 0;
            for (int i = 0; i < 3; i++)
                if (!(v[i] > -1.0e6f && v[i] < 1.0e6f)) return 0;
            v[0] = g_nav_world[0];
            v[1] = g_nav_world[1];
            v[2] = g_nav_world[2];      // the tile's floor (aim_floor)
            return 1;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return 0;
}

// The pick an aim has to be allowed. Mouse_CheckForFreeAim gives up unless
//
//     MouseTarget = GetMouseInterfaceTarget();   // HUD.CachedMouseInteractionInterface
//     if(MouseTarget == none) return;
//
// and it asks that BEFORE GetAdjustedMousePickPoint, where nav_lend_interface
// works for a move -- so with the mouse over nothing, every aim step went
// nowhere (the first aim of 2026-09-22: five steps, no placement). The lend is
// made one call earlier for an aim: ActiveUnit_Firing_WithMoveCharacteristics.
// PostProcessCheckGameLogic calls the native Engine.GetCurrentWorldInfo (for
// IsPaused) just before Mouse_CheckForFreeAim, and nothing between reads the
// interface. hook_validpos takes it back, as it does for a move.
//
// The HUD is reached by name from the cursor: Pawn.Controller, then
// Controller.myHUD. That works before any move has shown the mod the HUD.
static void*     g_aim_frame_node;      // that PostProcessCheckGameLogic, once seen
static void*     g_aim_frame_miss[16];  // other callers, so their names are not re-read
static int       g_aim_frame_nmiss;
static FieldSlot g_cursor_controller, g_controller_hud;

static void* aim_hud(void)
{
    void* cursor = cursor_object();
    const void* v;
    if (!cursor || !field_ptr(cursor, "Controller", &g_cursor_controller, sizeof(void*), &v))
        return NULL;
    void* controller = *(void* const*)v;
    if (!controller || !unit_is_live(controller) ||
        !field_ptr(controller, "myHUD", &g_controller_hud, sizeof(void*), &v))
        return NULL;
    void* hud = *(void* const*)v;
    return hud && unit_is_live(hud) ? hud : NULL;
}

static void nav_aim_lend(void* stack)
{
    if (!readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    if (node != g_aim_frame_node) {
        for (int i = 0; i < g_aim_frame_nmiss; i++)
            if (g_aim_frame_miss[i] == node) return;
        char name[128];
        if (!object_name(node, name, sizeof name) ||
            strcmp(name, "PostProcessCheckGameLogic") != 0) {
            if (g_aim_frame_nmiss < (int)(sizeof g_aim_frame_miss / sizeof g_aim_frame_miss[0]))
                g_aim_frame_miss[g_aim_frame_nmiss++] = node;
            return;
        }
        // Only aiming's own: the moving state has one of the same name, but
        // this is called only while an aim is being navigated.
        g_aim_frame_node = node;
    }
    void* hud = aim_hud();
    if (!hud) return;
    if (!g_pick_iface_off &&
        !object_field_offset(hud, "CachedMouseInteractionInterface", &g_pick_iface_off))
        return;
    nav_return_interface();
    void** slot = (void**)((uint8_t*)hud + g_pick_iface_off);
    if (!writable(slot, 2 * sizeof(void*))) return;
    if (slot[0]) return;                // the mouse picks something itself

    // Nothing under the mouse, and with a fresh map nothing remembered either
    // (the run of 2026-09-22: "none to lend", parking the mouse did not help,
    // the player had to move it by hand). So the soldier is lent: over a unit
    // pawn, Mouse_CheckForFreeAim does CursorSetLocation(unit.GetLocation()),
    // and nav_aim_substitute puts the aim's tile in place of that location at
    // the leash. IMouseInteractionInterface is a script interface (no native
    // keyword, no VfTable property), so the game reaches it through the object
    // and the pair is (object, object); nav_lend_interface logs what a real
    // pick holds, to confirm it.
    void* pawn = NULL;
    if (!cursor_chained_pawn(&pawn) || !pawn || !unit_is_live(pawn)) return;
    slot[0] = pawn;
    slot[1] = pawn;
    g_pick_iface_seen[0] = pawn;
    g_pick_iface_seen[1] = pawn;
    g_pick_iface_cursor = cursor_object();
    g_pick_iface_lent = slot;
    if (g_pick_iface_state != 2) {
        g_pick_iface_state = 2;
        logf_("nav: the mouse picks nothing -- lending the soldier to the aim\n");
    }
}

ExecFn g_orig_worldinfo;
void __fastcall hook_worldinfo(void* self, void* edx, void* stack, void* result)
{
    if (g_nav_live && g_nav_aim) {
        GUARDED("nav: aim lend", nav_aim_lend(stack));
    }
    g_orig_worldinfo(self, edx, stack, result);
}

ExecFn g_orig_chained;
void __fastcall hook_chained(void* self, void* edx, void* stack, void* result)
{
    if (g_nav_live && g_nav_aim) {
        Fault f;
        static int logged;
        __try {
            if (nav_aim_substitute(stack) && !logged) {
                logged = 1;
                logf_("nav: aim tile put in at the range leash\n");
            }
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: aim substitute", &f, NULL);
        }
    }
    g_orig_chained(self, edx, stack, result);
}

ExecFn g_orig_validpos;
void __fastcall hook_validpos(void* self, void* edx,
                                     void* stack, void* result)
{
    int placed = 0;
    // Reaching here means the pick was accepted, so whatever was lent to make
    // it accepted has done its work and comes straight back out -- before the
    // native runs, and long before anything else on this frame reads it.
    nav_return_interface_guarded();
    if (g_nav_live) {
        GUARDED("nav: substitute", placed = nav_substitute(stack));
    }
    g_orig_validpos(self, edx, stack, result);
    if (placed) {
        GUARDED("nav: placed", nav_placed((const float*)result));
    }
}
