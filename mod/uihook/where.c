// Where a tile is: inside a building and on which storey, on the roof, or
// outside; the evac zone; and where a storey F / C missed can be found. See
// where.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "where.h"
#include "game.h"
#include "units.h"
#include "log.h"
#include "cursor.h"
#include "objects.h"
#include "props.h"
#include "mission.h"
#include "tile.h"
#include "names.h"

// ---- inside or outside -----------------------------------------------------
//
// "Inside building, floor 2 of 3." as a step crosses a wall, "On the roof.",
// "Outside." -- said on the crossing, as the game redraws its cut-away only
// then. Taken from X2Access (xcom2access, alex19EP), whose tile cursor says
// the same; the rule itself is EU/EW's own.
//
// The game keeps the answer for the cursor in XComPawnIndoorOutdoorInfo, fed
// by the cursor's Touch events on XComFloorVolumes -- a frame behind every
// placement, and about the mouse's cursor, not the numpad's target. So the
// same question is asked of the same volumes at the tile: every floor volume
// whose brush holds a point just above the floor (Volume.EncompassesPoint),
// then CheckForFloorVolumeEvents' rule over them:
//   - if any of them belongs to an internal building (a building within a
//     building), only those count -- for the game. Here it is the other way
//     round: the outer building's volumes win when there are any (where_at);
//   - the first building met is the one, and the floor is the highest
//     FloorNumber among its volumes;
//   - a floor volume with no building behind it still counts as inside.
// The roof is the game's IsOnRoof -- floor == Floors.Length with more than one
// floor -- which the weather uses to rain on a soldier indoors or not. A
// building whose own IsInside is off is taken as outside, as X2Access found
// the native to hold in XCOM 2; EU/EW's native IsInside is virtual and was not
// read, so that part is a guess, and the log line says so when it decides.
//
// The floor volumes are placed with the map and never spawned, so they are
// found by one walk of the object table per map, with each one's box kept to
// rule it out before the native is asked.
#define WHERE_VOLUMES 2048
#define WHERE_HITS    16
#define WHERE_LIFT    32.0f      // above the floor: inside a storey's volume, below the next

typedef struct { void* v; int idx; float lo[3], hi[3]; } WhereVolume;
static WhereVolume g_where_vol[WHERE_VOLUMES];
static int         g_where_n, g_where_next, g_where_full;
static void*       g_where_world;
static const void* g_where_cls;
static const void* g_where_walk[4];      // XComFloorVolume, XComBuildingVolume,
                                         // SeqAct_GetExtractionVolume,
                                         // XComCapturePointVolume

// The evac zone: a level's XComBuildingVolume with IsDropShip, which is what
// SeqAct_GetExtractionVolume hands the mission scripts ("Get Extraction
// Volume"), and what XComUnitPawn.Touch watches to raise "On Unit Touched
// Dropship Volume" and set m_bInDropShip. It has no floor volumes of its own,
// so where_at never met it: no log has ever had "dropship 1". Found on the
// same walk as the floor volumes. Only said while an open objective mentions
// evac ("Escort the survivor to the EVAC Zone."): every map has one, and a
// mission that does not ask for it shows the player no zone.
//
// A map can hold several. The 2026-09-28 (13:02) log, escorting Zhang
// (Slingshot's first mission), had four: one off the map at x 784..1040,
// another off it past x 496, the soldiers' spawn
// (SoldierSpawns_DrpshipVol_Arc0, with an empty box) and the zone on the
// south edge. The first one kept was the one off the map, and the scanner
// found nothing; the 13:06 log found the south edge's, the only one left. The game takes
// the first from AllActors, the level's actor order, which is not the object
// table's -- so the one the mission asked for is read from the Get Extraction
// Volume action itself (its ExtractionVolume, set when it ran), and only
// while no action has run are all the ones on the map offered.
#define EVAC_MAX 8
#define EVAC_SEQ 4
static WhereVolume g_evac[EVAC_MAX];
static int         g_evac_n;
static void*       g_evac_seq[EVAC_SEQ];
static int         g_evac_seq_idx[EVAC_SEQ], g_evac_seq_n;
static FieldSlot   g_seq_extraction;
static int where_bool(void* bv, const char* name, int dflt);

// The capture zones of the covert data recovery (XGBattle_SPCaptureAndHold):
// one XComCapturePointVolume each, kept off the same walk. See capture_at.
#define CAPTURE_MAX 4
static WhereVolume g_capture[CAPTURE_MAX];
static int         g_capture_n;
static FieldSlot   g_fv_brush, g_fv_number, g_fv_building, g_brush_bounds, g_bv_floors;
static TileWhere   g_where_heard;    // what the player last heard, for the crossing
static void where_storeys_forget(void);

static int where_collect(void* obj, int which, int idx, void* ctx)
{
    (void)ctx;
    if (which == 2) {
        if (g_evac_seq_n < EVAC_SEQ) {
            g_evac_seq[g_evac_seq_n] = obj;
            g_evac_seq_idx[g_evac_seq_n++] = idx;
        }
        return 1;
    }
    if (which == 0 && g_where_n >= WHERE_VOLUMES) { g_where_full = 1; return 0; }
    if (which == 3 && g_capture_n >= CAPTURE_MAX) return 1;
    if (which == 1 && (g_evac_n >= EVAC_MAX || !where_bool(obj, "IsDropShip", 0))) return 1;
    const void* v;
    if (!field_ptr(obj, "BrushComponent", &g_fv_brush, sizeof(void*), &v) || !*(void* const*)v)
        return 1;
    void* brush = *(void* const*)v;
    if (!field_ptr(brush, "Bounds", &g_brush_bounds, 7 * sizeof(float), &v)) return 1;
    const float* b = (const float*)v;           // Origin, BoxExtent, SphereRadius
    // A dropship volume with no brush size (the Zhang map's soldier spawn, 0..0)
    // holds no tile, and would count as on the map at the origin.
    if (which == 1 && b[3] <= 0.0f && b[4] <= 0.0f) return 1;
    WhereVolume* w = which == 1 ? &g_evac[g_evac_n++]
                   : which == 3 ? &g_capture[g_capture_n++] : &g_where_vol[g_where_n++];
    w->v = obj;
    w->idx = idx;
    for (int k = 0; k < 3; k++) {
        w->lo[k] = b[k] - b[3 + k];
        w->hi[k] = b[k] + b[3 + k];
    }
    if (which == 1) {
        char name[64] = "?";
        object_name(obj, name, sizeof name);
        logf_("where: dropship volume %d, %s, spans %.0f..%.0f, %.0f..%.0f, %.0f..%.0f\n",
              g_evac_n, name, w->lo[0], w->hi[0], w->lo[1], w->hi[1], w->lo[2], w->hi[2]);
    }
    if (which == 3) {
        char name[64] = "?";
        object_name(obj, name, sizeof name);
        logf_("where: capture zone %d, %s, spans %.0f..%.0f, %.0f..%.0f, %.0f..%.0f\n",
              g_capture_n, name, w->lo[0], w->hi[0], w->lo[1], w->hi[1], w->lo[2], w->hi[2]);
    }
    return 1;
}

// The floor volumes of this map, walked in full once per world and topped up
// from where the last walk stopped after that. 0 when there are none to ask.
static int where_volumes(void)
{
    if (!objects_ready()) return 0;
    if (!g_where_cls) {
        static const char* const names[] = { "XComFloorVolume", "XComBuildingVolume",
                                             "SeqAct_GetExtractionVolume",
                                             "XComCapturePointVolume" };
        objects_classes(names, g_where_walk, 4);
        g_where_cls = g_where_walk[0];
    }
    if (!g_where_cls) return 0;
    // The list stops at the first class not found, so each one keeps its index.
    int nwalk = 1;
    while (nwalk < 4 && g_where_walk[nwalk]) nwalk++;
    void* world = cursor_world();
    if (world != g_where_world || g_where_next > objects_count()) {
        g_where_world = world;
        g_where_n = g_where_next = g_where_full = 0;
        g_evac_n = g_evac_seq_n = g_capture_n = 0;
        where_storeys_forget();
        memset(&g_where_heard, 0, sizeof g_where_heard);
        objects_each_from(g_where_walk, nwalk, 0, &g_where_next, where_collect, NULL);
        unsigned ms;
        int entries;
        objects_last_walk(&ms, &entries);
        logf_("where: %d floor volumes on this map%s, %d dropship volumes, %d Get Extraction "
              "Volume actions, %d capture zones (%d entries, %u ms)\n", g_where_n,
              g_where_full ? ", the list is full" : "", g_evac_n, g_evac_seq_n, g_capture_n,
              entries, ms);
    } else if (!g_where_full) {
        int had = g_where_n;
        objects_each_from(g_where_walk, nwalk, g_where_next, &g_where_next, where_collect, NULL);
        if (g_where_n != had) logf_("where: %d more floor volumes\n", g_where_n - had);
    }
    return g_where_n;
}

// A bool on a building volume, or `dflt` while the bool mask is not known.
static int where_bool(void* bv, const char* name, int dflt)
{
    if (!props_mask_offset()) return dflt;
    static struct { const void* cls; const void* prop; char name[24]; } cache[8];
    uint32_t class_off = props_class_offset();
    if (!class_off || !readable((uint8_t*)bv + class_off, sizeof(void*))) return dflt;
    const void* cls = *(void* const*)((uint8_t*)bv + class_off);
    const void* prop = NULL;
    int k;
    for (k = 0; k < 8 && cache[k].cls; k++)
        if (cache[k].cls == cls && strcmp(cache[k].name, name) == 0) break;
    if (k < 8 && cache[k].cls) {
        prop = cache[k].prop;
    } else {
        prop = object_field_prop(bv, name);
        if (k == 8) k = 7;
        cache[k].cls = cls;
        cache[k].prop = prop;
        strncpy_s(cache[k].name, sizeof cache[k].name, name, _TRUNCATE);
    }
    int b = dflt;
    if (!prop || !props_read_object_bool(prop, (const uint8_t*)bv, &b)) return dflt;
    return b;
}

// The storeys a building really has. Its Floors array is its cut-away bands,
// and a band is not a floor: in the 22:29 log of 2026-09-27 a building's
// volumes were bands 0..192, 192..384 and 384..576 under a roof band, but the
// only surfaces in it were the ground at 2 and an upper floor at 388 -- F from
// the ground went straight to 388 on every tile tried, and the middle band held
// nothing but a stair landing at 290 on one tile. Said by band it was "floor 1
// of 3" under a ceiling with one floor above it. So a band counts as a storey
// only when the game has a floor in it somewhere: each of the building's own
// volumes is asked, tile by tile across its box and layer by layer up its
// height, with the probe F uses (GetFloorZForPosition from the layer's top,
// else IsPositionOnFloor at its middle), and the floor found must lie inside
// that same volume. Pieces the map marks as not to be entered
// (m_bNonEnterableBuildingPiece) and the roof band are left out. Worked out
// once per building, and its make-up logged then, with each volume's
// m_FloorVolumeType -- the map's own word for a band, whose meaning for an
// empty one is not known yet.
#define WHERE_STOREYS    16
#define WHERE_BAND_TILES 400     // tiles asked per layer of a volume, at most
static void* g_where_bv;                 // the building the list below is for
static int   g_where_bv_nums[WHERE_STOREYS], g_where_bv_n;
static FieldSlot g_fv_type;

static void where_storeys_forget(void)
{
    g_where_bv = NULL;
    g_where_bv_n = 0;
}

// The exact floor inside the 64-unit layer from `bottom`: GetFloorZForPosition
// asked from the layer's top, which searches down and finds it. `bottom` when
// the game cannot be asked or finds nothing in the layer (aim_floor, main.c,
// has the history).
float aim_floor_exact(void* world, float* pos, float bottom)
{
    FloorZFn floorz = (FloorZFn)tile_vfn(world, g_tile_slot_floorz);
    if (!floorz) return bottom;
    pos[2] = bottom + CURSOR_LAYER;
    float z = floorz(world, NULL, pos, 0);
    // The height it was given back means none found; anything outside the
    // layer is a different floor from the one the flags found.
    if (z == pos[2] || !(z >= bottom - 1.0f && z <= bottom + CURSOR_LAYER)) return bottom;
    return z;
}

// The game's floor queries, as F asks them, gathered once per question.
typedef struct {
    void*          world;
    EncompassFn    inside;
    PositionTestFn on_floor;
    FloorZFn       floorz;
    CursorGrid     g;
} WhereProbe;

static int where_probe_init(WhereProbe* p)
{
    p->world = cursor_world();
    p->inside = (EncompassFn)g_volume_fn_encompass;
    if (!p->world || !p->inside || !cursor_grid(&p->g)) return 0;
    p->on_floor = (PositionTestFn)tile_vfn(p->world, g_tile_slot_onfloor);
    p->floorz = (FloorZFn)tile_vfn(p->world, g_tile_slot_floorz);
    return p->on_floor || p->floorz;
}

// The tile range of volume `w`'s box on the grid, clamped to the map. 0 when
// it lies off the map.
static int where_box(const CursorGrid* g, const WhereVolume* w,
                     int* x0, int* x1, int* y0, int* y1, int* z0, int* z1)
{
    *x0 = grid_x(g, w->lo[0] + 1.0f);
    *x1 = grid_x(g, w->hi[0] - 1.0f);
    *y0 = grid_y(g, w->lo[1] + 1.0f);
    *y1 = grid_y(g, w->hi[1] - 1.0f);
    *z0 = grid_layer(g, w->lo[2] + 1.0f);
    *z1 = grid_layer(g, w->hi[2] - 1.0f);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 >= g->num_x) *x1 = g->num_x - 1;
    if (*y1 >= g->num_y) *y1 = g->num_y - 1;
    return *x1 >= *x0 && *y1 >= *y0 && *z1 >= *z0;
}

// A floor on tile (tx, ty) inside volume `w`, lowest layer first; its height
// in `*found`. F's own probe: GetFloorZForPosition from each layer's top, else
// IsPositionOnFloor at its middle, and the floor must lie in the volume --
// just above it, or, with `reach` past WHERE_LIFT, anywhere up to `reach`
// above it (a unit's body, for the evac zone).
static int where_tile_floor(const WhereProbe* p, const WhereVolume* w, int tx, int ty,
                            int z0, int z1, float reach, float* found)
{
    float x = grid_centre_x(&p->g, tx);
    float y = grid_centre_y(&p->g, ty);
    for (int tz = z0; tz <= z1; tz++) {
        float bottom = grid_layer_bottom(&p->g, tz);
        float z = 0.0f;
        int has = 0;
        if (p->floorz) {
            float top[3] = { x, y, bottom + 63.0f };
            z = p->floorz(p->world, NULL, top, 0);
            has = z != top[2] && z >= bottom - 1.0f && z <= bottom + 63.0f;
        }
        if (!has && p->on_floor) {
            float mid[3] = { x, y, bottom + 32.0f };
            if (p->on_floor(p->world, NULL, mid)) {
                z = aim_floor_exact(p->world, mid, bottom);
                has = 1;
            }
        }
        if (!has) continue;
        int in = 0;
        for (float up = WHERE_LIFT; !in && up <= reach + 0.5f; up += WHERE_LIFT)
            in = p->inside(w->v, NULL, x, y, z + up, 0.0f, 0.0f, 0.0f) != 0;
        if (!in) continue;
        *found = z;
        return 1;
    }
    return 0;
}

// Whether the game has a floor anywhere inside volume `w`; where, in
// `*fx, *fy, *found`. Asks at most WHERE_BAND_TILES tiles, spread over the box.
static int where_band_floor(const WhereVolume* w, int* fx, int* fy, float* found)
{
    WhereProbe p;
    int x0, x1, y0, y1, z0, z1;
    if (!where_probe_init(&p) || !where_box(&p.g, w, &x0, &x1, &y0, &y1, &z0, &z1)) return 0;
    int area = (x1 - x0 + 1) * (y1 - y0 + 1);
    int step = 1;
    while (area / (step * step) > WHERE_BAND_TILES) step++;
    for (int ty = y0; ty <= y1; ty += step)
        for (int tx = x0; tx <= x1; tx += step)
            if (where_tile_floor(&p, w, tx, ty, z0, z1, WHERE_LIFT, found)) {
                *fx = tx;
                *fy = ty;
                return 1;
            }
    return 0;
}

// The tile nearest (tx, ty) with a floor in storey `num` of building `bv`:
// every tile of that storey's volumes is asked, nearest first wins. For F / C
// when the storey is not over or under the tile they were pressed on.
static int where_storey_near(const void* bv, int num, int tx, int ty, int* nx, int* ny)
{
    WhereProbe p;
    if (!where_probe_init(&p)) return 0;
    int best = -1;
    for (int i = 0; i < g_where_n; i++) {
        WhereVolume* w = &g_where_vol[i];
        if (!objects_still(w->v, w->idx, &g_where_cls, 1)) continue;
        const void* v;
        if (!field_ptr(w->v, "CachedBuildingVolume", &g_fv_building, sizeof(void*), &v) ||
            *(void* const*)v != bv)
            continue;
        if (!field_ptr(w->v, "FloorNumber", &g_fv_number, sizeof(int32_t), &v) ||
            *(const int32_t*)v != num)
            continue;
        if (where_bool(w->v, "m_bNonEnterableBuildingPiece", 0)) continue;
        int x0, x1, y0, y1, z0, z1;
        if (!where_box(&p.g, w, &x0, &x1, &y0, &y1, &z0, &z1)) continue;
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                int d = (x - tx) * (x - tx) + (y - ty) * (y - ty);
                if (best >= 0 && d >= best) continue;
                float z;
                if (!where_tile_floor(&p, w, x, y, z0, z1, WHERE_LIFT, &z)) continue;
                best = d;
                *nx = x;
                *ny = y;
            }
        }
    }
    return best >= 0;
}

// Whether `bv`'s storey with floor number `num` has a floor on (tx, ty)
// itself: where_storey_near, asked of the one tile. 1 yes, 0 no, -1 when the
// game could not be asked.
static int where_storey_here(const void* bv, int num, int tx, int ty)
{
    WhereProbe p;
    if (!where_probe_init(&p)) return -1;
    for (int i = 0; i < g_where_n; i++) {
        WhereVolume* w = &g_where_vol[i];
        if (!objects_still(w->v, w->idx, &g_where_cls, 1)) continue;
        const void* v;
        if (!field_ptr(w->v, "CachedBuildingVolume", &g_fv_building, sizeof(void*), &v) ||
            *(void* const*)v != bv)
            continue;
        if (!field_ptr(w->v, "FloorNumber", &g_fv_number, sizeof(int32_t), &v) ||
            *(const int32_t*)v != num)
            continue;
        if (where_bool(w->v, "m_bNonEnterableBuildingPiece", 0)) continue;
        int x0, x1, y0, y1, z0, z1;
        if (!where_box(&p.g, w, &x0, &x1, &y0, &y1, &z0, &z1)) continue;
        if (tx < x0 || tx > x1 || ty < y0 || ty > y1) continue;
        float z;
        if (where_tile_floor(&p, w, tx, ty, z0, z1, WHERE_LIFT, &z)) return 1;
    }
    return 0;
}

// The 1-based storey `floor_no` is of `bv`: how many of its real storeys are
// at or below that band, so a stair landing in an empty band is said as the
// storey under it. 0 when the building has none to count.
static int where_storeys(void* bv, int bands, int floor_no)
{
    if (bv != g_where_bv) {
        g_where_bv = bv;
        g_where_bv_n = 0;
        char parts[768] = "";
        size_t used = 0;
        ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < g_where_n; i++) {
            WhereVolume* w = &g_where_vol[i];
            if (!objects_still(w->v, w->idx, &g_where_cls, 1)) continue;
            const void* v;
            if (!field_ptr(w->v, "CachedBuildingVolume", &g_fv_building, sizeof(void*), &v) ||
                *(void* const*)v != bv)
                continue;
            int num = field_ptr(w->v, "FloorNumber", &g_fv_number, sizeof(int32_t), &v)
                ? *(const int32_t*)v : 0;
            int type = field_ptr(w->v, "m_FloorVolumeType", &g_fv_type, 1, &v)
                ? *(const uint8_t*)v : -1;
            int closed = where_bool(w->v, "m_bNonEnterableBuildingPiece", 0);
            int roof = bands > 1 && num == bands;
            float z = 0.0f;
            int fx = 0, fy = 0;
            int floored = !closed && !roof && num > 0 && where_band_floor(w, &fx, &fy, &z);
            char what[48];
            if (floored)
                _snprintf_s(what, sizeof what, _TRUNCATE, " floor at %.0f on %d, %d", z, fx, fy);
            else strcpy_s(what, sizeof what, closed ? " closed" : roof ? " roof" : " empty");
            int wrote = _snprintf_s(parts + used, sizeof parts - used, _TRUNCATE,
                                    "%s#%d z %.0f..%.0f type %d%s", used ? ", " : "", num,
                                    w->lo[2], w->hi[2], type, what);
            if (wrote > 0) used += (size_t)wrote;
            if (!floored) continue;
            int at = 0;
            while (at < g_where_bv_n && g_where_bv_nums[at] < num) at++;
            if (at < g_where_bv_n && g_where_bv_nums[at] == num) continue;
            if (g_where_bv_n == WHERE_STOREYS) continue;
            memmove(&g_where_bv_nums[at + 1], &g_where_bv_nums[at],
                    (size_t)(g_where_bv_n - at) * sizeof(int));
            g_where_bv_nums[at] = num;
            g_where_bv_n++;
        }
        logf_("where: building %p, %d bands, %d storeys with a floor (%u ms): %s\n", bv, bands,
              g_where_bv_n, (unsigned)(GetTickCount64() - t0), parts);
    }
    int rank = 0;
    for (int i = 0; i < g_where_bv_n; i++)
        if (g_where_bv_nums[i] <= floor_no) rank = i + 1;
    return rank;
}

// Where the tile whose floor is at `floor` stands. 0 when the game could not
// be asked at all -- which is silence, never "Outside".
static int where_at(int tx, int ty, float floor, TileWhere* out)
{
    memset(out, 0, sizeof *out);
    CursorGrid g;
    EncompassFn inside = (EncompassFn)g_volume_fn_encompass;
    if (!inside || !cursor_grid(&g) || !where_volumes()) return 0;
    float p[3] = { grid_centre_x(&g, tx),
                   grid_centre_y(&g, ty),
                   floor + WHERE_LIFT };

    struct { void* bv; int number; int internal; } hit[WHERE_HITS];
    int n = 0, any_internal = 0, any_outer = 0;
    for (int i = 0; i < g_where_n && n < WHERE_HITS; i++) {
        WhereVolume* w = &g_where_vol[i];
        if (p[0] < w->lo[0] || p[0] > w->hi[0] || p[1] < w->lo[1] || p[1] > w->hi[1] ||
            p[2] < w->lo[2] || p[2] > w->hi[2])
            continue;
        if (!objects_still(w->v, w->idx, &g_where_cls, 1)) continue;
        if (!inside(w->v, NULL, p[0], p[1], p[2], 0.0f, 0.0f, 0.0f)) continue;
        const void* v;
        hit[n].number = field_ptr(w->v, "FloorNumber", &g_fv_number, sizeof(int32_t), &v)
            ? *(const int32_t*)v : 0;
        hit[n].bv = field_ptr(w->v, "CachedBuildingVolume", &g_fv_building, sizeof(void*), &v)
            ? *(void* const*)v : NULL;
        if (hit[n].bv && !unit_is_live(hit[n].bv)) hit[n].bv = NULL;
        hit[n].internal = hit[n].bv ? where_bool(hit[n].bv, "m_bIsInternalBuilding", 0) : 0;
        if (hit[n].internal) any_internal = 1;
        else if (hit[n].bv) any_outer = 1;
        n++;
    }

    out->state = TILE_WHERE_OUTSIDE;
    if (!n) return 1;

    // CheckForFloorVolumeEvents, which walks its list from the end -- but
    // with the outer building ahead of an internal one. The game prefers the
    // room inside, for what its cut-away hides; for the player the room is
    // part of the building around it. The 2026-10-01 (13:34) log had a room
    // of two bands (floor at -53, its top band 122..302) inside a building
    // of three storeys: its ceiling at 129.7 is the outer building's floor 2
    // (floor_missed: "nearest on 16, 40", the very tile), and was said "On
    // the roof." between "Inside building." and "floor 3 of 3".
    void* bv = NULL;
    int floor_no = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (any_outer ? hit[i].internal : any_internal && !hit[i].internal) continue;
        if (bv && hit[i].bv != bv) continue;
        bv = hit[i].bv;
        if (hit[i].number > floor_no) floor_no = hit[i].number;
    }

    int floors = 0, is_inside = 1, ufo = 0, dropship = 0;
    if (bv) {
        const void* v;
        if (field_ptr(bv, "Floors", &g_bv_floors, 3 * sizeof(int32_t), &v))
            floors = ((const int32_t*)v)[1];
        if (floors < 0 || floors > 64) floors = 0;
        is_inside = where_bool(bv, "IsInside", 1);
        ufo = where_bool(bv, "IsUfo", 0);
        dropship = where_bool(bv, "IsDropShip", 0);
    }

    if (!is_inside)
        out->state = TILE_WHERE_OUTSIDE;
    else if (bv && floors > 1 && floor_no == floors)
        out->state = TILE_WHERE_ROOF;
    else
        out->state = TILE_WHERE_INSIDE;
    out->floor = floor_no;
    out->storeys = floors > 1 ? floors - 1 : floors;
    int rank = bv ? where_storeys(bv, floors, floor_no) : 0;
    if (rank > 0) {
        out->floor = rank;
        out->storeys = g_where_bv_n;
    }
    out->kind = ufo ? TILE_UFO : dropship ? TILE_DROPSHIP : TILE_BUILDING;
    out->building = bv;

    // "Floor 1 of 3" counts the building's storeys, and a building is often
    // taller in one part than another: the 2026-10-06 (11:31) log said
    // "Inside building, floor 1 of 3." on 41, 17, a one-storey wing, and F
    // there found nothing ("nearest on 37, 15") -- reported as floor 1 of 3
    // with no floor above. So whether the storey above has a floor on this
    // very tile is asked too, and said when it has not.
    int above = -1;
    if (out->state == TILE_WHERE_INSIDE && rank > 0 && out->floor < out->storeys &&
        bv == g_where_bv) {
        above = where_storey_here(bv, g_where_bv_nums[out->floor], tx, ty);
        out->no_above = above == 0;
    }

    logf_("where: %d, %d floor %.1f: %d volume%s, building %p (%d bands, inside %d, "
          "internal %d, ufo %d, dropship %d), floor number %d, storey %d of %d, "
          "above here %d -> %s\n",
          tx, ty, floor, n, n == 1 ? "" : "s", bv, floors, is_inside, any_internal,
          ufo, dropship, floor_no, out->floor, out->storeys, above,
          out->state == TILE_WHERE_ROOF ? "roof" : out->state == TILE_WHERE_INSIDE
              ? "inside" : "outside (the building's IsInside is off)");
    return 1;
}

// The words for arriving at (tx, ty), said only on a crossing -- or always,
// with `force`. Empty when nothing changed or the game could not be asked.
void where_say(int tx, int ty, float floor, int force, char* out, size_t out_sz)
{
    out[0] = 0;
    TileWhere now;
    int ok = 0;
    GUARDED("where", ok = where_at(tx, ty, floor, &now), ok = 0);
    if (!ok) return;
    tile_where_text(&g_where_heard, &now, force, out, out_sz);
    g_where_heard = now;
}

// The same words, always said, and without touching what the numpad has
// heard: a unit's place is not a step, and taking it as one would make the
// next step's crossing go unsaid.
void where_is(int tx, int ty, float floor, char* out, size_t out_sz)
{
    out[0] = 0;
    TileWhere now, none;
    int ok = 0;
    GUARDED("where", ok = where_at(tx, ty, floor, &now), ok = 0);
    if (!ok) return;
    memset(&none, 0, sizeof none);
    tile_where_text(&none, &now, 1, out, out_sz);
}

// How far above its floor a unit reaches into the evac zone. The game counts
// a unit in when its collision cylinder touches the dropship volume
// (XComUnitPawn.Touch / UnTouch set m_bInDropShip), and that cylinder stands
// from the floor to twice CollisionHeight (Pawn's default 78) above it. The
// 2026-09-28 (12:51) log had the zone's box from 128 to 384, above the
// ground, so a floor asked for inside the box, and a point 32 over it, never
// met it: the scanner had "Objectives, 0 found." on a mission to escort Zhang
// to the zone.
#define EVAC_REACH 144.0f

// Whether the evac zone is one to speak of, and which dropship volumes make it:
// their indices into g_evac, in `use`. The one a Get Extraction Volume action
// picked when there is one, else every one whose box is on the map. Why none,
// and which were picked, logged when it changes.
static int evac_live(int* use)
{
    static int said = -1, said_n = -1;
    static void* said_pick;
    int why = 0, n = 0;
    void* pick = NULL;
    CursorGrid g;
    if (!where_volumes()) why = 1;
    else if (!g_evac_n) why = 2;
    else if (!mission_open_mentions("evac")) why = 3;
    else if (!cursor_grid(&g)) why = 4;
    else {
        for (int i = 0; i < g_evac_seq_n && !pick; i++) {
            const void* v;
            if (!objects_still(g_evac_seq[i], g_evac_seq_idx[i], &g_where_walk[2], 1)) continue;
            if (field_ptr(g_evac_seq[i], "ExtractionVolume", &g_seq_extraction, sizeof(void*), &v))
                pick = *(void* const*)v;
        }
        for (int i = 0; i < g_evac_n; i++) {
            if (!objects_still(g_evac[i].v, g_evac[i].idx, &g_where_walk[1], 1)) continue;
            if (pick && g_evac[i].v != pick) continue;
            int x0, x1, y0, y1, z0, z1;
            if (!where_box(&g, &g_evac[i], &x0, &x1, &y0, &y1, &z0, &z1)) continue;
            use[n++] = i;
        }
        if (!n) why = pick ? 5 : 6;
    }
    if (why != said || n != said_n || pick != said_pick) {
        static const char* const text[] = { "", "no floor volumes", "no dropship volume",
                                             "no open objective mentions evac", "no grid",
                                             "the volume Get Extraction Volume picked is not "
                                             "on the map",
                                             "no dropship volume on the map" };
        if (why) {
            logf_("where: evac zone not said: %s\n", text[why]);
        } else {
            char name[64] = "?";
            object_name(g_evac[use[0]].v, name, sizeof name);
            logf_("where: evac zone from %s%s, %d volume%s\n", name,
                  pick ? " (Get Extraction Volume picked it)"
                       : " and the rest on the map (no Get Extraction Volume has run)",
                  n, n == 1 ? "" : "s");
        }
    }
    said = why;
    said_n = n;
    said_pick = pick;
    return why ? 0 : n;
}

// Whether the tile whose floor is at `floor` is in the evac zone: the dropship
// volume's brush asked at points up a unit's body, as the Touch would find it.
int evac_at(int tx, int ty, float floor)
{
    CursorGrid g;
    int use[EVAC_MAX];
    EncompassFn inside = (EncompassFn)g_volume_fn_encompass;
    if (!inside || !cursor_grid(&g)) return 0;
    int n = evac_live(use);
    float x = grid_centre_x(&g, tx);
    float y = grid_centre_y(&g, ty);
    for (int i = 0; i < n; i++) {
        const WhereVolume* w = &g_evac[use[i]];
        if (x < w->lo[0] || x > w->hi[0] || y < w->lo[1] || y > w->hi[1] ||
            floor + EVAC_REACH < w->lo[2] || floor + WHERE_LIFT > w->hi[2])
            continue;
        for (float up = WHERE_LIFT; up <= EVAC_REACH + 0.5f; up += WHERE_LIFT)
            if (inside(w->v, NULL, x, y, floor + up, 0.0f, 0.0f, 0.0f)) return 1;
    }
    return 0;
}

// Which capture zone the tile whose floor is at `floor` lies in, as the HUD
// names it, or NULL. The objective is "Block EXALT's hack attempts by
// occupying the capture zone", and the 2026-10-04 (18:47) log had nothing on
// a step to say a soldier stood in one. The game counts a unit in by the
// volume's TouchingActors (XComCapturePointVolume.UpdateCaptureState), the
// same touch of the collision cylinder as the evac zone's, so the brush is
// asked at the same points up a unit's body.
//
// Only a zone the screen shows: UpdateBorderEffect draws the border only
// while IsActive -- not captured, and the one before it in the sequence
// captured or none -- and one captured has its arrow taken away
// (UpdateIndicatorArrow). The one waiting its turn keeps a grey arrow and no
// border: said with "not yet active". The volume with
// m_iCaptureSequenceIndex 0 is ENCODER, any other TRANSMITTER
// (UISpecialMissionHUD_CapturePointStats.UpdatePanel).
static FieldSlot g_cp_progress, g_cp_turns, g_cp_prev, g_cp_next, g_cp_seq;

// XComCapturePointVolume.IsCaptured: its progress reached its turns, or the
// next one in the sequence is captured. -1 unread.
static int capture_captured(void* v, int depth)
{
    const void* f;
    if (!v || depth > CAPTURE_MAX || !objects_live(v)) return -1;
    if (!field_ptr(v, "m_iCaptureProgress", &g_cp_progress, sizeof(int32_t), &f)) return -1;
    int progress = *(const int32_t*)f;
    if (!field_ptr(v, "m_iTurnsToCapture", &g_cp_turns, sizeof(int32_t), &f)) return -1;
    if (progress >= *(const int32_t*)f) return 1;
    if (!field_ptr(v, "m_kNextCapturePoint", &g_cp_next, sizeof(void*), &f)) return -1;
    void* next = *(void* const*)f;
    return next ? capture_captured(next, depth + 1) : 0;
}

// XComCapturePointVolume.IsActive, read: 1 active, 0 waiting its turn,
// 2 captured, -1 unread.
static int capture_state(void* v)
{
    const void* f;
    int cap = capture_captured(v, 0);
    if (cap < 0) return -1;
    if (cap) return 2;
    if (!field_ptr(v, "m_kPreviousCapturePoint", &g_cp_prev, sizeof(void*), &f)) return -1;
    void* prev = *(void* const*)f;
    if (!prev) return 1;
    int pc = capture_captured(prev, 0);
    return pc < 0 ? -1 : pc ? 1 : 0;
}

const char* capture_at(int tx, int ty, float floor, int* waiting)
{
    CursorGrid g;
    EncompassFn inside = (EncompassFn)g_volume_fn_encompass;
    *waiting = 0;
    if (!inside || !where_volumes() || !g_capture_n || !cursor_grid(&g)) return NULL;
    float x = grid_centre_x(&g, tx);
    float y = grid_centre_y(&g, ty);
    for (int i = 0; i < g_capture_n; i++) {
        const WhereVolume* w = &g_capture[i];
        if (x < w->lo[0] || x > w->hi[0] || y < w->lo[1] || y > w->hi[1] ||
            floor + EVAC_REACH < w->lo[2] || floor + WHERE_LIFT > w->hi[2])
            continue;
        if (!objects_still(w->v, w->idx, &g_where_walk[3], 1)) continue;
        int in = 0;
        for (float up = WHERE_LIFT; up <= EVAC_REACH + 0.5f && !in; up += WHERE_LIFT)
            in = inside(w->v, NULL, x, y, floor + up, 0.0f, 0.0f, 0.0f);
        if (!in) continue;
        int st = capture_state(w->v);
        const void* f;
        int seq = field_ptr(w->v, "m_iCaptureSequenceIndex", &g_cp_seq, sizeof(int32_t), &f)
                      ? *(const int32_t*)f : -1;
        // Once per zone and state, not per step.
        static void* said_v;
        static int said_st = -9;
        if (w->v != said_v || st != said_st) {
            said_v = w->v;
            said_st = st;
            char name[64] = "?";
            object_name(w->v, name, sizeof name);
            logf_("where: %d, %d floor %.1f is in capture zone %s, sequence %d, %s\n", tx, ty,
                  floor, name, seq, st < 0 ? "state unread" : st == 0 ? "waiting its turn"
                  : st == 1 ? "active" : "captured -- not said");
        }
        if (st == 2) continue;
        *waiting = st == 0;
        return seq == 0 ? "Encoder capture zone" : "Transmitter capture zone";
    }
    return NULL;
}

// The tile of the evac zone nearest (ox, oy) that has a floor in it, and that
// floor's height. Every tile of each volume's box is asked, as
// where_storey_near does, from the layers a unit could stand in and still
// reach the box.
int evac_nearest(int ox, int oy, int* nx, int* ny, float* nz)
{
    WhereProbe p;
    int use[EVAC_MAX];
    int n = evac_live(use);
    if (!n) return 0;
    if (!where_probe_init(&p)) {
        logf_("where: evac zone: no floor probe\n");
        return 0;
    }
    int best = -1;
    for (int i = 0; i < n; i++) {
        const WhereVolume* w = &g_evac[use[i]];
        int x0, x1, y0, y1, z0, z1;
        if (!where_box(&p.g, w, &x0, &x1, &y0, &y1, &z0, &z1)) continue;
        z0 -= (int)(EVAC_REACH / CURSOR_LAYER);
        if (z0 < 0) z0 = 0;
        int found = 0;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int d = (x - ox) * (x - ox) + (y - oy) * (y - oy);
                if (best >= 0 && d >= best) continue;
                float z;
                if (!where_tile_floor(&p, w, x, y, z0, z1, EVAC_REACH, &z)) continue;
                best = d;
                found = 1;
                *nx = x;
                *ny = y;
                *nz = z;
            }
        char name[64] = "?";
        object_name(w->v, name, sizeof name);
        if (found)
            logf_("where: evac zone %s nearest (%d, %d): tile (%d, %d), floor %.0f "
                  "(tiles %d..%d, %d..%d, layers %d..%d)\n", name, ox, oy, *nx, *ny, *nz,
                  x0, x1, y0, y1, z0, z1);
        else
            logf_("where: evac zone %s: nothing nearer on tiles %d..%d, %d..%d, layers "
                  "%d..%d\n", name, x0, x1, y0, y1, z0, z1);
    }
    return best >= 0;
}

// A new navigation starts from nothing heard, as X2Access's plant does: the
// first tile says so if it is inside, and outside goes unsaid.
void where_forget(void)
{
    memset(&g_where_heard, 0, sizeof g_where_heard);
}

// How many of a building's floors lie between heights `from` and `to` on tile
// (tx, ty), when both are in the same building: its storeys, and the roof one
// above the top one. The 22:42 log of 2026-09-27 had "Hagen, two floors up"
// from the ground at 64 to the roof at 450 of a building with one storey --
// 386 is two storeys of 192, but there is no floor between. 0 when either
// height is outside, or the two are in different buildings: then the storeys
// of 192 stand.
static int where_level(const TileWhere* w)
{
    if (w->state == TILE_WHERE_ROOF) return w->storeys + 1;
    if (w->state == TILE_WHERE_INSIDE) return w->floor;
    return 0;
}

int where_levels_between(int tx, int ty, float from, float to, int* dz)
{
    return where_levels_apart(tx, ty, from, tx, ty, to, dz);
}

// The same between two tiles: the scanner's "Panel, one floor up" is measured
// from the player's tile to the item's.
int where_levels_apart(int ax, int ay, float from, int bx, int by, float to, int* dz)
{
    TileWhere a, b;
    int ok = 0;
    Fault f;
    __try {
        ok = where_at(ax, ay, from, &a) && where_at(bx, by, to, &b) &&
             a.building && a.building == b.building &&
             where_level(&a) > 0 && where_level(&b) > 0 && a.storeys > 0;
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("where: levels", &f, NULL);
        ok = 0;
    }
    if (!ok) return 0;
    *dz = where_level(&b) - where_level(&a);
    return 1;
}

// After F / C inside a building: the next storey that way, when the key did
// not land on it -- passed over, or nothing found at all -- and where the
// nearest tile of it is. In the 22:35 log of 2026-09-27 a building had floors
// at 2, 193 and 388, but 193 was not over the tiles F was pressed on: F went
// 2 -> 388 as "2 storeys up. Floor 3 of 3.", and elsewhere said "No floor
// above." under "floor 1 of 3". Both were true and neither said where floor 2
// was. Now: "Floor 2 of 3 does not reach this tile; nearest 3 north, 2 east."
// Empty when outside, on a storey reached as asked, or with nothing that way.
void floor_missed(int tx, int ty, float from, int found, float to, int dir,
                         char* out, size_t out_sz)
{
    out[0] = 0;
    TileWhere here, there;
    if (!where_at(tx, ty, from, &here) || here.state != TILE_WHERE_INSIDE ||
        !here.building || here.building != g_where_bv || here.floor < 1 ||
        here.floor > g_where_bv_n)
        return;
    int next = here.floor + dir;                 // the storey rank asked for
    if (next < 1 || next > g_where_bv_n) return;
    if (found && where_at(tx, ty, to, &there) && there.building == here.building &&
        there.state == TILE_WHERE_INSIDE && there.floor == next)
        return;
    int num = g_where_bv_nums[next - 1];
    int nx, ny;
    if (!where_storey_near(here.building, num, tx, ty, &nx, &ny)) {
        logf_("where: storey %d (floor number %d) has no tile with a floor\n", next, num);
        return;
    }
    char where[64];
    tile_offset_text(nx - tx, ny - ty, where, sizeof where);
    _snprintf_s(out, out_sz, _TRUNCATE, "Floor %d of %d does not reach this tile; nearest %s.",
                next, g_where_bv_n, where);
    logf_("where: F / C at %d, %d %s storey %d (floor number %d); nearest on %d, %d\n",
          tx, ty, found ? "passed" : "found nothing, missing", next, num, nx, ny);
}

// ---- what a floor is -------------------------------------------------------
//
// Not a row of the world grid. XComWorldData steps its Z axis by
// WORLD_FloorHeight, 64 units, and a map 18 of those tall was announced as
// having eighteen floors -- on a building with two. A *floor*, as the game and
// the player mean it, is XCom3DCursor.CURSOR_OUTDOOR_FLOOR_HEIGHT: 192 units,
// three grid rows. The first run had a soldier on 259.1 and another on 533.4
// called five floors apart; they are one.
//
// The game will answer this itself -- WorldZToCursorFloor is native on the
// cursor, and takes the whole position, so it can tell an indoor floor from
// the ground outside it. That is the answer used. The division is only the
// fallback for a build where the thunk does not have the shape tile_vtable_slot
// reads, and it is the same division the constant describes.
#define CURSOR_FLOOR_HEIGHT 192.0f

int floor_of(const float* world)
{
    void* cur = cursor_object();
    CursorFloorFn fn = (CursorFloorFn)tile_vfn(cur, g_cursor_slot_floor);
    if (fn) return fn(cur, NULL, world[0], world[1], world[2]);
    CursorGrid g;
    if (!cursor_grid(&g)) return 0;
    return cursor_tile_axis(world[2], g.min_z, CURSOR_FLOOR_HEIGHT);
}

// How many floors the map has. XCom3DCursorForCursorVolumes works m_iMaxFloor
// out from the cursor volumes the level was built with, so it is the map's own
// count; the grid's height in floors is the fallback.
static FieldSlot g_maxfloor;

int floor_count(void)
{
    const void* v;
    void* cur = cursor_object();
    if (cur && field_ptr(cur, "m_iMaxFloor", &g_maxfloor,
                         sizeof(int32_t), &v)) {
        int n = *(const int32_t*)v + 1;         // m_iMaxFloor is the top index
        if (n > 0 && n < 64) return n;
    }
    CursorGrid g;
    if (!cursor_grid(&g)) return 1;
    int n = (int)((float)g.num_z * CURSOR_LAYER / CURSOR_FLOOR_HEIGHT);
    return n > 0 ? n : 1;
}
