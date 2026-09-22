// Tile-by-tile navigation: the part that does not touch the game.  See nav.h.

#include "nav.h"
#include <stdio.h>

static int g_active;
static int g_tx, g_ty;

int nav_step_for_digit(int digit, int* dx, int* dy)
{
    // Laid out as the numpad is: the row a key sits on is how far north it
    // goes, the column how far east.
    static const int step[10][2] = {
        { 0,  0}, {-1, -1}, { 0, -1}, { 1, -1},     // 0, 1 2 3
        {-1,  0}, { 0,  0}, { 1,  0},               // 4 5 6
        {-1,  1}, { 0,  1}, { 1,  1},               // 7 8 9
    };
    if (digit < 0 || digit > 9) return 0;
    if (!step[digit][0] && !step[digit][1]) return 0;
    *dx = step[digit][0];
    *dy = step[digit][1];
    return 1;
}

void nav_begin(int tx, int ty)
{
    g_tx = tx;
    g_ty = ty;
    g_active = 1;
}

int nav_active(void) { return g_active; }

void nav_end(void) { g_active = 0; }

int nav_target(int* tx, int* ty)
{
    if (!g_active) return 0;
    if (tx) *tx = g_tx;
    if (ty) *ty = g_ty;
    return 1;
}

void nav_describe(int tx, int ty, char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%d, %d", tx, ty);
}

// ---- the floor under a target tile -----------------------------------------

// Floor search starts, relative to the ground: outwards in both directions.
// Logged finds came from 64, 158 and 186 above the floor; starts 247 or more
// above never found one.
static const float SEARCH_STARTS[] = {
    32.0f, -32.0f, 96.0f, -96.0f, 160.0f, -160.0f,
    224.0f, -224.0f, 288.0f, -288.0f, 352.0f, -352.0f,
};
// Heights tried when no search finds a floor, one floor layer apart.
static const float PROBE_HEIGHTS[] = {
    0.0f, -64.0f, 64.0f, -128.0f, 128.0f, -192.0f, 192.0f,
};
#define COUNT(a) (int)(sizeof a / sizeof a[0])

static NavHeightPhase g_phase;
static float g_ground;
static float g_settled;
static int   g_search, g_probe;
static int   g_decided;
static int   g_have_far;        // a floor found far below the ground (NAVH_FAR_BELOW)
static float g_far;             // the highest such floor
static unsigned long long g_failed_at;

static int near_z(float a, float b) { return a - b < 1.0f && b - a < 1.0f; }

void navh_set_ground(float ground) { g_ground = ground; }

void navh_begin_tile(void)
{
    g_phase = NAVH_SEARCH;
    g_search = 0;
    g_probe = 0;
    g_decided = 0;
    g_failed_at = 0;
    g_have_far = 0;
}

float navh_query_z(void)
{
    switch (g_phase) {
    case NAVH_SEARCH:  return g_ground + SEARCH_STARTS[g_search];
    case NAVH_PROBE:   return g_ground + PROBE_HEIGHTS[g_probe];
    case NAVH_SETTLED: return g_settled;
    default:           return g_ground;
    }
}

static void settle(float z)
{
    g_phase = NAVH_SETTLED;
    g_settled = z;
    g_ground = z;
    g_failed_at = 0;
}

void navh_floor_result(float asked, float got)
{
    if (g_phase == NAVH_SETTLED || g_phase == NAVH_NONE) return;
    if (!near_z(asked, got)) {          // it found a floor
        if (got >= g_ground - NAVH_FAR_BELOW) {
            settle(got);
            return;
        }
        // Far below: kept, and the search goes on for a nearer one.
        if (!g_have_far || got > g_far) g_far = got;
        g_have_far = 1;
    }
    if (g_phase == NAVH_SEARCH && ++g_search >= COUNT(SEARCH_STARTS)) {
        if (g_have_far) {
            settle(g_far);
            return;
        }
        g_phase = NAVH_PROBE;
        g_probe = 0;
    }
}

NavVerdict navh_path_result(float dest_z, int ok, unsigned long long now_ms)
{
    if (g_decided) return NAVH_WAIT;
    float placed = dest_z - NAVH_LIFT;

    if (ok) {
        // A path to any height is proof enough; keep that height.
        if (g_phase != NAVH_SETTLED || !near_z(placed, g_settled)) settle(placed);
        g_failed_at = 0;
        g_decided = 1;
        return NAVH_REACHABLE;
    }

    switch (g_phase) {
    case NAVH_PROBE:
        // Only the answer for the height being probed moves the probe on;
        // a result from a frame ago is for the height before.
        if (near_z(placed, g_ground + PROBE_HEIGHTS[g_probe]) &&
            ++g_probe >= COUNT(PROBE_HEIGHTS)) {
            g_phase = NAVH_NONE;
            g_decided = 1;
            return NAVH_NO_PATH;
        }
        return NAVH_WAIT;
    case NAVH_SETTLED:
        if (near_z(placed, g_settled) && !g_failed_at) g_failed_at = now_ms ? now_ms : 1;
        return NAVH_WAIT;
    default:
        // Still searching: a failure at an unsettled height says nothing.
        return NAVH_WAIT;
    }
}

NavVerdict navh_poll(unsigned long long now_ms)
{
    if (g_decided || g_phase != NAVH_SETTLED || !g_failed_at) return NAVH_WAIT;
    if (now_ms - g_failed_at < NAVH_SETTLE_MS) return NAVH_WAIT;
    g_decided = 1;
    return NAVH_NO_PATH;
}

NavHeightPhase navh_phase(void) { return g_phase; }
float navh_ground(void) { return g_ground; }

int nav_move(const NavGrid* g, int dx, int dy, char* say, size_t say_sz)
{
    if (say && say_sz) say[0] = 0;
    if (!g_active || !g || g->num_x <= 0 || g->num_y <= 0) return 0;

    int nx = g_tx + dx;
    int ny = g_ty + dy;
    // Each axis is clamped on its own, so a diagonal along a wall still slides
    // along it rather than refusing to move at all.
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= g->num_x) nx = g->num_x - 1;
    if (ny >= g->num_y) ny = g->num_y - 1;

    if (nx == g_tx && ny == g_ty) {
        if (say) _snprintf_s(say, say_sz, _TRUNCATE, "Edge");
        return 0;
    }
    g_tx = nx;
    g_ty = ny;
    if (say) nav_describe(nx, ny, say, say_sz);
    return 1;
}
