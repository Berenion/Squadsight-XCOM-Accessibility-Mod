// What the player hears of the map around them: the wall field and the
// heartbeats, doors and windows. See sounds.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sounds.h"
#include "units.h"
#include "world.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "sonar.h"
#include "audio.h"
#include "heart.h"
#include "settings.h"
#include "soldier.h"
#include "scan.h"

// ---- the walls around a tile -----------------------------------------------
//
// Walls in XCOM sit between tiles, not on them: that is why cover is named by
// side -- COVER_North is a wall on this tile's northern edge -- and why
// IsTileOccupied, which asks whether a tile is filled with solid stuff, finds
// pillars and trucks but walks straight through a partition. So the scan asks
// both questions of every tile within range:
//
//   its cover bits, each of which is a wall face on one of its edges, and
//   IsTileOccupied, which is a solid object standing on the tile itself.
//
// Every face found is one emitter in the field (sonar.h): a wall face sounds
// from the edge it is on, a solid tile from its own middle. Nothing in range
// is open ground, and is silence.
//
// Each wall is read once, not twice. A wall between two tiles shows in one's
// north bit and the other's south, so only each tile's own north and east
// faces are taken; the other two belong to its neighbours and are picked up
// when the scan reaches them. Reading all four would count every wall twice,
// which would make a partition as loud as two walls and a corner louder than
// either.
//
// Two limits worth naming. A tile whose cover frame is turned 45 degrees
// (COVER_Diagonal) describes its corners rather than its sides, so it has
// nothing to say about any of the four and its cover bits are passed over -- a
// diagonal wall is heard only as whatever solid stands behind it. And every
// tile in range is asked about at the floor height of the tile being listened
// from, because that is the only floor the mod knows; where the ground changes
// level within range the game answers for whatever tile it finds there
// instead, which the check on the cover point's own coordinates below turns
// into silence rather than into a wall that is not there.
//
// The game's compass is the mirror of the mod's (see tile.h): its East is this
// mod's west. So the two faces read off each tile are the game's North bit --
// the mod's north -- and its West bit, which is the mod's east.
// Only the two bits that say a wall is there. The matching low-cover bits are
// deliberately not read: low cover blocks the way as surely as a wall and the
// field says so, and whether it is waist high is said in words (tile_describe)
// rather than folded into a level that has to carry distance.
#define WALL_N_BIT      TILE_COVER_N
#define WALL_E_BIT      TILE_COVER_W

// How far the scan reaches, in whole tiles: everything the range can hear.
#define WALL_TILES ((int)SONAR_RANGE)

static int walls_scan(const CursorGrid* g, int tx, int ty, float floor,
                      SonarField* out)
{
    void* world = cursor_world();
    sonar_field_clear(out);
    if (!world) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;
    TileTestFn occupied = (TileTestFn)tile_vfn(world, g_tile_slot_occupied);

    float z = floor + 4.0f;
    int tz = grid_layer(g, z);

    for (int dy = -WALL_TILES; dy <= WALL_TILES; dy++) {
        for (int dx = -WALL_TILES; dx <= WALL_TILES; dx++) {
            int x = tx + dx, y = ty + dy;
            if (x < 0 || y < 0 || x >= g->num_x || y >= g->num_y) {
                // Off the map. The edge stops a soldier as surely as a wall
                // does, and a player walking towards it should hear it
                // coming, so the tile that is not there sounds as solid.
                sonar_block(out, (float)dx, (float)dy);
                continue;
            }

            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            float wx = grid_centre_x(g, x);
            float wy = grid_centre_y(g, y);
            // An answer about some other tile is an answer about some other
            // floor, and is worth less than no answer at all.
            if (cover(world, NULL, wx, wy, z, &cp) && cp.x == x && cp.y == y &&
                !(cp.flags & TILE_COVER_DIAGONAL)) {
                if (cp.flags & WALL_N_BIT)
                    sonar_face(out, SONAR_AXIS_NS, (float)dx, (float)dy + 0.5f);
                if (cp.flags & WALL_E_BIT)
                    sonar_face(out, SONAR_AXIS_EW, (float)dx + 0.5f, (float)dy);
            }

            // A solid tile sounds from where it stands, and has no side to it.
            // The tile being listened from is not one of them -- sonar_block
            // drops one with no bearing -- which is right: what fills the
            // cursor's own tile is not a wall around it.
            if (occupied && occupied(world, NULL, x, y, tz))
                sonar_block(out, (float)dx, (float)dy);
        }
    }
    sonar_field_finish(out);
    return 1;
}

// The field, handed to the mixer. Silent where the scan cannot run at all --
// no world data, no cover slot -- because a field meaning "the mod could not
// ask" would be indistinguishable from one meaning "open".
//
// The tile listened from is listen_tile's, below. A scan is two questions of
// the game about each of the (2 * SONAR_RANGE + 1)^2 tiles in range, so it is rescanned when that tile
// changes and at most every WALLS_SCAN_MS; while the tile does not change, only
// every WALLS_IDLE_MS, which is there to catch a wall being blown up rather
// than to track the player. Between scans the mixer's own glide carries the
// level, so a tile crossed faster than the scan rate loses nothing but detail.
#define WALLS_SCAN_MS   40
#define WALLS_IDLE_MS  250
#define WALLS_LOG_MS   400

static SonarField g_walls_field;        // the last scan, renewed every frame
static int        g_walls_have;          // a tile has been scanned
static int        g_walls_tile[2];
static ULONGLONG  g_walls_at;
static ULONGLONG  g_walls_logged;

// Where the field and the hearts listen from:
//   - the navigation target while one is held;
//   - the cursor while aiming, since the cursor is the aim;
//   - otherwise the selected soldier.
// Not the cursor when nothing is held: in mouse mode it sits under the mouse,
// and the mouse is parked mid-window (mouse.h), which in the 2026-09-25 log
// was 24 tiles from the soldier after every switch -- 28, 35 for Vargas on
// 28, 11. The cursor is the fallback only when the soldier cannot be read.
// The soldier's Location.Z runs a few units above the cursor's resting height
// (83.1 against 80.0 on the same tile), so it takes the same lift off; the
// cursor's own height is used when the two share a tile, as nav_press does.
static int listen_tile(const CursorGrid* g, int* tx, int* ty, float* floor)
{
    if (nav_active() && nav_target(tx, ty)) {
        *floor = navh_ground();
        return 1;
    }
    float z;
    int have_cursor = cursor_tile(g, tx, ty, &z);
    if (!soldier_aiming()) {
        int sx, sy;
        float sz;
        if (soldier_tile(g, &sx, &sy, &sz) &&
            sx >= 0 && sy >= 0 && sx < g->num_x && sy < g->num_y) {
            if (!have_cursor || sx != *tx || sy != *ty) z = sz;
            *tx = sx;
            *ty = sy;
            *floor = z - NAVH_LIFT;
            return 1;
        }
    }
    if (!have_cursor) return 0;
    *floor = z - NAVH_LIFT;
    return 1;
}

void walls_quiet(void)
{
    audio_field_off();
    g_walls_have = 0;
}

void walls_rescan(void)
{
    g_walls_have = 0;
}

void walls_poll(void)
{
    CursorGrid g;
    int tx, ty;
    float floor;

    // Switched off from here or from the options menu: quiet once, on the
    // change, since the menu's thread must not touch the field itself.
    static int was_on = 1;
    int on = settings_get(SET_FIELD);
    if (!on && was_on) walls_quiet();
    was_on = on;
    if (!on || !audio_available()) return;
    if (!cursor_grid(&g)) { walls_quiet(); return; }

    if (!listen_tile(&g, &tx, &ty, &floor)) { walls_quiet(); return; }

    ULONGLONG now = GetTickCount64();
    int same = g_walls_have && tx == g_walls_tile[0] && ty == g_walls_tile[1];
    if (g_walls_have &&
        now - g_walls_at < (ULONGLONG)(same ? WALLS_IDLE_MS : WALLS_SCAN_MS)) {
        // The scan is what is throttled, not the field: the mixer lets an
        // unrenewed field lapse (audio.h), so the last one has to be handed
        // over again every frame to say it still holds.
        audio_field(&g_walls_field);
        return;
    }
    g_walls_at = now;

    Fault flt;
    __try {
        if (!walls_scan(&g, tx, ty, floor, &g_walls_field)) { walls_quiet(); return; }
    }
    __except (fault_note(GetExceptionInformation(), &flt)) {
        fault_log("walls: scan", &flt, NULL);
        walls_quiet();
        return;
    }
    audio_field(&g_walls_field);
    g_walls_have = 1;
    g_walls_tile[0] = tx;
    g_walls_tile[1] = ty;

    // On a change of tile, and rate limited: a glide crosses twenty tiles a
    // second and a line for each would bury everything else in the log.
    if (!same && now - g_walls_logged >= WALLS_LOG_MS) {
        g_walls_logged = now;
        logf_("walls: %d, %d floor %.1f -- W %.2f N %.2f S %.2f E %.2f\n",
              tx, ty, floor,
              g_walls_field.level[SONAR_W], g_walls_field.level[SONAR_N],
              g_walls_field.level[SONAR_S], g_walls_field.level[SONAR_E]);
    }
}

// ---- heartbeats, doors and windows -----------------------------------------
//
// Heartbeats (heart.h), allies' and seen enemies', heard from the tile the
// field listens from. The units are read at most every HEARTS_SCAN_MS --
// unit_seen calls into the game for each flag -- and what was read is handed
// to the mixer every frame, as the field is, so it lapses when this stops
// being called.
#define HEARTS_SCAN_MS 150
#define HEARTS_MAX     32

// Every 5 s in a mission, a line saying how the frames went: how many there
// were, and the worst the wall field and the hearts each took of one on the
// game's thread. Written after a report of slowdowns while moving with beeps
// on, when the log had no timings to tell a slow frame from a slow decision.
#define PERF_MS 5000

void perf_note(long long walls_ticks, long long hearts_ticks)
{
    static LARGE_INTEGER freq;
    static ULONGLONG since;
    static int frames;
    static long long walls_max, hearts_max;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    ULONGLONG now = GetTickCount64();
    if (!since || now - since > 4 * PERF_MS) {
        // First frame, or back from a stretch with no mission: start afresh
        // rather than average the gap in.
        since = now;
        frames = 0;
        walls_max = hearts_max = 0;
    }
    frames++;
    if (walls_ticks > walls_max) walls_max = walls_ticks;
    if (hearts_ticks > hearts_max) hearts_max = hearts_ticks;
    if (now - since < PERF_MS) return;
    double ms = 1000.0 / (double)freq.QuadPart;
    static unsigned walks_seen, climbs_seen;
    logf_("perf: %.1f frames a second; worst frame's walls %.2f ms, hearts %.2f ms; "
          "%u field walks, %u climbs\n",
          frames * 1000.0 / (double)(now - since), walls_max * ms, hearts_max * ms,
          g_field_walks - walks_seen, g_field_climbs - climbs_seen);
    walks_seen = g_field_walks;
    climbs_seen = g_field_climbs;
    since = now;
    frames = 0;
    walls_max = hearts_max = 0;
}

// Door and window sounds (SET_DOORS, SET_WINDOWS): every door and window
// within DOOR_RANGE tiles of where the field listens from, one sound per
// doorway or window, taking turns in the mixer, in one round together. The
// doors are the scanner's (world_refresh), refreshed every DOORS_SCAN_MS --
// doors do not move -- and the first refresh of a mission is the object walk
// the scanner would otherwise make on its first press.
#define DOOR_RANGE      10
#define DOORS_SCAN_MS   1000
#define DOOR_ALONE_S    2.7f    // a door with no other near knocks this often
#define DOORS_MAX       64

// One entry per doorway or window tile, kept for the mission: its address is
// the id in the mixer, so it keeps its place in the round. A double door is
// two actors on one tile, and is one entry. [2] is its HEART_* kind.
static int   g_door_tile[DOORS_MAX][3];
static int   g_door_n;
static void* g_door_map;        // the cursor the table was built for
static int   g_door_near[DOORS_MAX];    // this refresh's doors within range
static int   g_door_near_n;

// The doors and windows within range of (tx, ty), of the kinds switched on,
// into g_door_near; the table and its reasons are above hearts_poll.
static void doors_refresh(const CursorGrid* g, int tx, int ty, int doors, int windows)
{
    int new_map = g_door_map != cursor_object();
    if (new_map) { g_door_n = 0; g_door_map = cursor_object(); }
    g_door_near_n = 0;
    if (!world_refresh(g)) return;
    int nitems;
    const ScanItem* items = world_items(&nitems);
    // Once a map: how many were placed. Until 2026-09-28 none were until the
    // scanner had been pressed (world_item_at); the 11:26 log that day showed
    // 20 doors and 16 windows placed before it had.
    if (new_map) {
        int nd = 0, nw = 0;
        for (int i = 0; i < nitems; i++) {
            if (items[i].kind == SCAN_DOORS) nd++;
            else if (items[i].kind == SCAN_INTERACT && strcmp(items[i].name, "Window") == 0) nw++;
        }
        logf_("doors: %d doors and %d windows placed on this map\n", nd, nw);
    }
    for (int i = 0; i < nitems; i++) {
        const ScanItem* it = &items[i];
        int kind;
        if (it->kind == SCAN_DOORS && doors) kind = HEART_DOOR;
        else if (it->kind == SCAN_INTERACT && windows && strcmp(it->name, "Window") == 0)
            kind = HEART_WINDOW;
        else continue;
        int dx = it->tx - tx, dy = it->ty - ty;
        if (dx * dx + dy * dy > DOOR_RANGE * DOOR_RANGE) continue;
        int k;
        for (k = 0; k < g_door_n; k++)
            if (g_door_tile[k][0] == it->tx && g_door_tile[k][1] == it->ty &&
                g_door_tile[k][2] == kind) break;
        if (k == g_door_n) {
            if (g_door_n >= DOORS_MAX) continue;
            g_door_tile[k][0] = it->tx;
            g_door_tile[k][1] = it->ty;
            g_door_tile[k][2] = kind;
            g_door_n++;
        }
        int dup = 0;
        for (int j = 0; j < g_door_near_n; j++) if (g_door_near[j] == k) dup = 1;
        if (!dup && g_door_near_n < DOORS_MAX) g_door_near[g_door_near_n++] = k;
    }
}

// "Follow one soldier" (settings.h, SET_HEART_SOLO): the soldier last picked
// in the scanner, by the name the scanner gives them (unit_label). Set by
// scan_say_selected, on the same thread as hearts_poll.
static char g_heart_follow[SCAN_NAME];

void hearts_follow(const char* name)
{
    if (strcmp(name, g_heart_follow) == 0) return;
    strncpy_s(g_heart_follow, sizeof g_heart_follow, name, _TRUNCATE);
    if (settings_get(SET_HEART_SOLO))
        logf_("hearts: following %s\n", g_heart_follow);
}

void hearts_poll(void)
{
    static const void* ids[HEARTS_MAX];
    static HeartSound  sounds[HEARTS_MAX];
    static int         n = -1;          // -1: nothing read yet
    static ULONGLONG   at;
    static int         was_on = 1;
    static int         logged = -1;

    int allies = settings_get(SET_HEARTS) && audio_hearts_available(HEART_ALLY);
    int solo = settings_get(SET_HEART_SOLO);
    int aliens = settings_get(SET_ALIENS) && audio_hearts_available(HEART_ALIEN);
    int doors = settings_get(SET_DOORS) && audio_hearts_available(HEART_DOOR);
    int windows = settings_get(SET_WINDOWS) && audio_hearts_available(HEART_WINDOW);
    int on = allies || aliens || doors || windows;
    if (!on && was_on) audio_hearts_off();
    was_on = on;
    if (!on) return;

    CursorGrid g;
    int tx, ty;
    if (!cursor_grid(&g)) { audio_hearts_off(); return; }
    float floor;
    if (!listen_tile(&g, &tx, &ty, &floor)) { audio_hearts_off(); return; }

    ULONGLONG now = GetTickCount64();
    if (n >= 0 && now - at < HEARTS_SCAN_MS) {
        audio_hearts(ids, sounds, n);
        return;
    }
    at = now;

    void* squad = squad_player();
    if (!squad) { audio_hearts_off(); n = -1; return; }
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);

    Fault flt;
    int k = 0, seen_aliens = 0, near_doors = 0, near_windows = 0;
    static SeenSet sight;
    static ULONGLONG doors_at;
    __try {
        if (!doors && !windows) g_door_near_n = 0;
        else if (!doors_at || now - doors_at >= DOORS_SCAN_MS) {
            doors_at = now;
            doors_refresh(&g, tx, ty, doors, windows);
        }
        for (int d = 0; d < g_door_near_n && k < HEARTS_MAX; d++) {
            int* t = g_door_tile[g_door_near[d]];
            // Checked here too, so a switch in the menu is heard at once
            // rather than at the next refresh.
            if (t[2] == HEART_DOOR ? !doors : !windows) continue;
            heart_sound(t[0] - tx, t[1] - ty, -1, -1, 0, SOLDIER_WOUND_NONE, &sounds[k]);
            sounds[k].kind = t[2];
            sounds[k].period = DOOR_ALONE_S;
            if (t[2] == HEART_DOOR) near_doors++; else near_windows++;
            ids[k++] = t;
        }
        // Enemies only while a squad member sees them: the radar's rule.
        if (aliens) squad_sight(squad, &sight);
        for (int i = 0; i < g_nunits && k < HEARTS_MAX; i++) {
            UnitSeen s;
            if (!unit_seen(&g_units[i], squad, &s)) continue;
            if (s.friendly ? !allies : (!aliens || !seen_has(&sight, s.unit))) continue;
            const UnitName* u = &g_units[i];
            int followed = 0;
            if (s.friendly && solo) {
                char label[SCAN_NAME];
                unit_label(u, label, sizeof label);
                if (!g_heart_follow[0] || strcmp(label, g_heart_follow) != 0) continue;
                followed = 1;
            }
            int dx = grid_x(&g, s.loc[0]) - tx;
            int dy = grid_y(&g, s.loc[1]) - ty;
            // The selected soldier is where the player is listening from
            // until they navigate away; then their heart marks the spot.
            // One the player chose to follow is heard even there.
            if (s.pawn == soldier && !dx && !dy && !followed) continue;
            if (s.friendly) {
                heart_sound(dx, dy, u->hp, u->hp_max, u->panicked, u->wounded, &sounds[k]);
            } else {
                heart_sound(dx, dy, u->hp, u->hp_max, 0, SOLDIER_WOUND_NONE, &sounds[k]);
                sounds[k].kind = HEART_ALIEN;
                seen_aliens++;
            }
            ids[k++] = s.unit;
        }
    }
    __except (fault_note(GetExceptionInformation(), &flt)) {
        fault_log("hearts: squad", &flt, NULL);
        audio_hearts_off();
        n = -1;
        return;
    }
    n = k;
    audio_hearts(ids, sounds, n);
    int sig = ((n * 64 + seen_aliens) * 64 + near_doors) * 64 + near_windows;
    if (sig != logged) {
        logged = sig;
        logf_("hearts: %d sounding: %d aliens, %d doors, %d windows\n", n, seen_aliens,
              near_doors, near_windows);
    }
}
