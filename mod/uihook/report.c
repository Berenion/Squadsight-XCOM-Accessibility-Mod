// The tile report: what a numpad step says about the tile it lands on. See
// report.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "report.h"
#include "where.h"
#include "world.h"
#include "units.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "props.h"

// The pathing pawn that built the last path (hook_computepath), and the one
// the path offsets were resolved on.
void*        g_path_pawn;
static void* g_reach_pawn;

// ---- how far the path goes -------------------------------------------------

// How far the path just built goes, against how far the soldier may go:
//   0 a standard move, 1 a dash, 2 past this turn's reach, -1 unreadable.
// *turns_out gets how many turns the path takes (tile_turns).
//
// DestinationReachability was the first try and said "dash" on every tile:
// SetActive(kUnit, bCanDash) sets it to 1 for any soldier who *can* dash. The
// second -- the path's XComPath.Cost over XComPathingPawn.StandardMoveLength,
// the test the tutorial's "Dashing!" makes -- was right up to the dash limit
// and then called everything beyond it a dash as well: the pathfinder builds
// paths far past the limit (costs of 54 against a standard move of 12). The
// limit is the one XGUnit.SetDashing applies: twice the standard move, and
// only while m_iMovesActionsPerformed is 0. MaxPathCost is the pawn's current
// allowance, which is either of those depending on which the path last asked
// for, so it only ever raises the limit.
static uint32_t g_path_off, g_std_off, g_maxcost_off, g_cost_off;
static void*    g_cost_class_path;
static FieldSlot g_gameunit, g_moves;

int tile_dash(int* cost_out, int* std_out, int* max_out, int* moves_out,
                     int* turns_out)
{
    *cost_out = *std_out = *max_out = *moves_out = -1;
    *turns_out = 0;
    void* pawn = g_path_pawn;
    if (!pawn) return -1;
    if (pawn != g_reach_pawn) {
        if (!object_field_offset(pawn, "Path", &g_path_off) ||
            !object_field_offset(pawn, "StandardMoveLength", &g_std_off) ||
            !object_field_offset(pawn, "MaxPathCost", &g_maxcost_off))
            return -1;
        g_reach_pawn = pawn;
    }
    const uint8_t* p = (const uint8_t*)pawn;
    if (!readable(p + g_path_off, sizeof(void*)) || !readable(p + g_std_off, 4) ||
        !readable(p + g_maxcost_off, 4))
        return -1;
    void* path = *(void**)(p + g_path_off);
    *std_out = *(const int32_t*)(p + g_std_off);
    *max_out = *(const int32_t*)(p + g_maxcost_off);
    if (!path) return -1;
    if (path != g_cost_class_path) {
        if (!object_field_offset(path, "Cost", &g_cost_off)) return -1;
        g_cost_class_path = path;
    }
    if (!readable((const uint8_t*)path + g_cost_off, 4)) return -1;
    *cost_out = *(const int32_t*)((const uint8_t*)path + g_cost_off);
    if (*std_out <= 0 || *cost_out < 0) return -1;

    // Moves already made this turn, off the soldier: ChainedPawn.m_kGameUnit.
    void* soldier = NULL;
    const void* v;
    if (cursor_chained_pawn(&soldier) && soldier &&
        field_ptr(soldier, "m_kGameUnit", &g_gameunit, sizeof(void*), &v)) {
        void* unit = *(void* const*)v;
        if (field_ptr(unit, "m_iMovesActionsPerformed", &g_moves, 4, &v))
            *moves_out = *(const int32_t*)v;
    }
    int limit = *moves_out == 0 ? 2 * *std_out : *std_out;
    if (*max_out > limit) limit = *max_out;

    *turns_out = tile_turns(*cost_out, limit, *std_out);
    if (*cost_out > limit) return 2;
    return *cost_out > *std_out;
}

// ---- who is on the tile ----------------------------------------------------

// Everyone in sight whose pawn stands in the column of (tx, ty), on any
// storey, with where their feet are: a pawn's origin is its middle,
// NAVH_LIFT above its feet. `mine` marks the soldier being moved.
int units_in_column(int tx, int ty, ColumnUnit* out, int max)
{
    int n = 0;
    CursorGrid g;
    if (!cursor_grid(&g)) return 0;
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);
    // Someone not on the squad is named only once the squad has seen them,
    // or stepping onto a hidden alien's tile would give it away. The tile is
    // asked first: it is two subtractions, and a civilian's sight can be a
    // line check per soldier.
    void* squad = squad_player();
    static SquadSight sight;
    squad_sight_take(squad, &sight);
    for (int i = 0; i < g_nunits && n < max; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        if (grid_x(&g, s.loc[0]) != tx ||
            grid_y(&g, s.loc[1]) != ty)
            continue;
        if (!squad_sees(&sight, s.unit, s.loc, s.friendly, s.who->name)) continue;
        unit_label_state(&g_units[i], s.unit, !s.friendly, out[n].label, sizeof out[n].label);
        out[n].feet = s.loc[2] - NAVH_LIFT;
        out[n].mine = s.pawn == soldier;
        n++;
    }
    // And those with no flag at all -- a mission's survivor, until rescued.
    // The scanner found one on 8, 13 in the 2026-09-27 (23:38) run, and the
    // step onto that tile said only "No path. 8, 13."
    if (squad && n < max) {
        static FlaglessUnit found[COLUMN_FLAGLESS];
        int nf = flagless_units(0, found, COLUMN_FLAGLESS);
        for (int i = 0; i < nf && n < max; i++) {
            if (grid_x(&g, found[i].loc[0]) != tx ||
                grid_y(&g, found[i].loc[1]) != ty)
                continue;
            if (!squad_sees(&sight, found[i].unit, found[i].loc, 0, found[i].name))
                continue;
            strcpy_s(out[n].label, sizeof out[n].label, found[i].name);
            out[n].feet = found[i].loc[2] - NAVH_LIFT;
            out[n].mine = 0;
            n++;
        }
    }
    return n;
}

// "Wright, Disco. Sectoid." -- everyone in sight whose pawn stands on
// (tx, ty). With a floor known, a unit on another storey of the same column
// is left out. *mine is set when one of them is the soldier being moved.
void units_on_tile(int tx, int ty, int have_floor, float floor,
                          char* out, size_t out_sz, int* mine)
{
    size_t used = 0;
    out[0] = 0;
    if (mine) *mine = 0;
    ColumnUnit u[COLUMN_UNITS];
    int n = units_in_column(tx, ty, u, COLUMN_UNITS);
    for (int i = 0; i < n; i++) {
        float mid = u[i].feet + NAVH_LIFT;
        if (have_floor && (mid < floor - 32.0f || mid > floor + 192.0f)) {
            logf_("tile: %s stands in this column at %.1f, not on floor %.1f\n",
                  u[i].label, mid, floor);
            continue;
        }
        if (mine && u[i].mine) *mine = 1;
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s.",
                            used ? " " : "", u[i].label);
        if (w < 0) break;
        used += (size_t)w;
    }
}

// ---- would a soldier be seen here, and would the cover hold ----------------
//
// Measured against the enemies the squad has ALREADY seen, and no others.
// The game will answer the wider question -- XGPlayer.IsEnemyUnitVisibleFromTile
// walks a whole player's squad, tile and alternate height both -- but every
// caller of it is the AI or a pod reveal, and nothing draws it: EU/EW puts no
// eye marker over a hovered tile the way XCOM 2 does. Counting aliens nobody
// has met would hand the player a fact the screen never shows.
//
// Within that restriction it is parity, and the flanking half is parity
// outright: XGAction_Path.Update calls XComActionIconManager.AddFlankingIcons
// every frame while the cursor moves, and IsLocationFlanking takes its
// enemies from GetAllVisibleTargets -- the same restriction, made by the game
// for the same reason.
//
// Flanking is the ENEMY's question, asked of the cover point: the game's own
// XComActionIconManager.IsLocationFlanked -- the thing that turns a cover
// icon red -- walks the visible enemies and asks each
// Enemy.IsFlankingCoverPoint(CoverPoint). The first attempt here went through
// XGPlayer.TestUnitCoverExposure instead, which is the AI's cover scorer, and
// it never once said flanked over a mission's worth of tiles.
//
// â›” The range gate is the game's and is not optional. IsLocationFlanked
// skips an enemy for which
//
//     Enemy.IsPointWithinFiringRange(.., Enemy, CoverPoint.CoverLocation,
//                                    Enemy.GetLocation())
//
// is false, and that is what makes flanking mean something against MELEE.
// Cover is protection from being shot; a Chryssalid or a zombie does not
// shoot, so a pack of them across the map turns no cover icon red and must
// turn no readout red either. Without this gate the mod would say "flanked"
// about an enemy the screen shows as no threat to the cover at all.
//
// fDistSq is the check on the whole call. It comes back as the squared
// distance the native measured, and the two positions that went in are ours,
// so it can be compared with the distance we can work out ourselves. A match
// proves the Vectors landed where they were wanted, which nothing about a
// bool return could. It also catches the cover point arriving empty, which
// was the standing suspicion after the first run.
#define EXPOSE_LOG_MAX 12
static int g_expose_ok = 1;
static int g_expose_logged;

static float dist_sq_between(const float* a, const float* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Whether a soldier standing at `here` would flank `enemy`: the game's own
// XComActionIconManager.IsLocationFlanking, which AddFlankingIcons asks for
// every enemy in GetAllVisibleTargets while the cursor moves, and which puts
// the flanking mark over the ones it answers yes for --
//
//     skip unless PlayerUnit.IsPointWithinFiringRange(.., Enemy,
//                     Enemy.GetLocation(), CoverPoint.CoverLocation)
//     CoverPointLocation.Z = 0
//     Enemy.IsFlankedBy_EnemyAtLocation(PlayerUnit, CoverPointLocation)
//
// The same range gate as the other way round, and for the same reason: out
// of the soldier's reach, a flank is nothing the screen marks.
static int tile_flanks(void* soldier, void* enemy, const float* eloc, const float* here)
{
    FiringRangeFn in_range = (FiringRangeFn)tile_vfn(soldier, g_unit_slot_range);
    FlankedByFn flanked_by = (FlankedByFn)tile_vfn(enemy, g_unit_slot_flankedby);
    if (!in_range || !flanked_by) return 0;
    float height_bonus = 0.0f, dist_sq = SENTINEL_FLOAT;
    int in = in_range(soldier, NULL, &height_bonus, &dist_sq, enemy,
                      eloc[0], eloc[1], eloc[2], here[0], here[1], here[2], NULL, 0.0f);
    if (!in) return 0;
    float flat[3] = { here[0], here[1], 0.0f };
    return flanked_by(enemy, NULL, soldier, flat, 0) != 0;
}

// The rings the game draws while a move is hovered. XGAction_Path calls
// XGUnit.DrawRanges(cursor) on every update, which puts a ring round each unit
// the selected soldier could reach with an ability -- the medikit (heal,
// revive) and the Arc Thrower (stun, a drone hack, a SHIV repair) round squad
// members and seen enemies, Close and Personal round seen enemies, the
// civilian rescue ring on a terror mission -- sized to the ability's range:
//     XComUnitPawn.AttachRangeIndicator(fDiameter, kMesh):
//         RangeIndicator.SetStaticMesh(kMesh)       -- which ring
//         RangeIndicator.SetScale(fDiameter / 512)  -- how wide
//         RangeIndicator.SetHidden(false)
// and DetachRangeIndicator only hides it. The sighted player sees whether the
// hovered tile falls inside a ring. So the rings are read as drawn, not worked
// out again: a unit whose indicator is showing, with one of the pawn's four
// ring meshes, and the tile within scale * 256 of the unit on the ground.
// The medikit ring is one mesh for heal, revive and repair, and the Arc
// Thrower ring one for stun and hack, so the ring is named, not the ability.
static FieldSlot g_pawn_ring, g_ring_mesh, g_ring_scale;
static FieldSlot g_ring_kind[4];
static const char* const RING_FIELD[4] = { "MedikitRing", "ArcThrowerRing",
                                           "CloseAndPersonalRing", "CivilianRescueRing" };
static const char* const RING_SAY[4] = { "Medikit reaches %s.", "Arc Thrower reaches %s.",
                                         "Close and Personal on %s.", "Rescues %s." };

static void tile_rings(const float* here, char* out, size_t out_sz)
{
    out[0] = 0;
    size_t used = 0;
    void* squad = squad_player();
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        if (s.pawn == soldier) continue;            // the kinetic strike and flamer cards
        const void* v;
        if (!field_ptr(s.pawn, "RangeIndicator", &g_pawn_ring, sizeof(void*), &v)) continue;
        void* comp = *(void* const*)v;
        if (!comp || !unit_is_live(comp)) continue;
        const void* hidden_prop = object_field_prop(comp, "HiddenGame");
        int hidden = 1;
        if (!hidden_prop || !props_read_object_bool(hidden_prop, (const uint8_t*)comp, &hidden) ||
            hidden)
            continue;
        if (!field_ptr(comp, "StaticMesh", &g_ring_mesh, sizeof(void*), &v)) continue;
        void* mesh = *(void* const*)v;
        if (!mesh) continue;
        int kind = -1;
        for (int k = 0; k < 4 && kind < 0; k++)
            if (field_ptr(s.pawn, RING_FIELD[k], &g_ring_kind[k], sizeof(void*), &v) &&
                *(void* const*)v == mesh)
                kind = k;
        if (kind < 0) continue;
        if (!field_ptr(comp, "Scale", &g_ring_scale, sizeof(float), &v)) continue;
        float radius = *(const float*)v * 256.0f;
        float dx = here[0] - s.loc[0], dy = here[1] - s.loc[1];
        if (!(radius > 0.0f) || dx * dx + dy * dy > radius * radius) continue;
        char name[96], one[160];
        unit_label(s.who, name, sizeof name);
        _snprintf_s(one, sizeof one, _TRUNCATE, RING_SAY[kind], name);
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s", used ? " " : "", one);
        if (w < 0) break;
        used += (size_t)w;
    }
}

// Up to EXPOSE_NAMES names kept of `total`, as tile_names_counted says them.
#define EXPOSE_NAMES 6
static void names_counted(char (*names)[48], int total, char* out, size_t out_sz)
{
    const char* p[TILE_NAMES_MAX];
    int n = total < EXPOSE_NAMES ? total : EXPOSE_NAMES;
    for (int i = 0; i < n; i++) p[i] = names[i];
    tile_names_counted(p, n, total, out, out_sz);
}

static void tile_exposure(int tx, int ty, int tz, const TileCoverPoint* cp,
                          int has_cover, const float* here, TileReport* r)
{
    r->enemies_known = 0;
    r->seen_by = 0;
    r->flanked = 0;
    r->flanks[0] = 0;
    r->height_over[0] = r->height_under[0] = 0;
    int nover = 0, nunder = 0;
    char over_names[EXPOSE_NAMES][48], under_names[EXPOSE_NAMES][48];
    // The soldier who would stand here, and where: the cover point when the
    // tile has one, as the game asks it, else the tile itself.
    void* soldier = g_expose_ok ? soldier_unit() : NULL;
    const float* stand = has_cover ? cp->cover_location : here;
    int nflank = 0;
    char flank_names[EXPOSE_NAMES][48];

    void* world = cursor_world();
    void* squad = squad_player();
    if (!world || !squad) return;

    SeeTileFn see = (SeeTileFn)tile_vfn(world, g_world_slot_seetile);
    if (!see) return;

    static SeenSet sight;
    squad_sight(squad, &sight);
    if (!sight.n) return;
    r->enemies_known = sight.n;

    for (int i = 0; i < sight.n; i++) {
        void* unit = sight.unit[i];

        // â›” Nothing had ever DEREFERENCED these before. squad_sight builds
        // the set out of each soldier's m_arrVisibleEnemies and the radar
        // only ever compared the pointers, so a dead unit in it cost nothing;
        // handing one to the game's own natives cost a crash. The 2026-09-21
        // log has both halves of it one step apart -- "tile: units faulted",
        // then "tile: report faulted (0xc0000005) reading 00000000" inside
        // XComEW.exe -- as the one enemy in sight went down.
        //
        // Two guards, because they catch different things. objects_live asks
        // the object table whether the pointer is still a live object, which
        // catches a freed one; IsAliveAndVisible is the game's own test of
        // the unit, and is the check XComActionIconManager.IsLocationFlanked
        // opens with (`Enemy.IsAliveAndWell()`) and that this port dropped.
        if (!unit_is_live(unit)) continue;
        UnitTestFn alive = (UnitTestFn)tile_vfn(unit, g_unit_slot_visible);
        if (!alive || !alive(unit, NULL)) continue;

        void* pawn = unit_pawn(unit);
        if (!pawn || !unit_is_live(pawn)) continue;

        // Height advantage, by the game's rule: a storey (192,
        // XGTacticalGameCoreNativeBase.RELATIVE_HEIGHT_BONUS_ZDIFF) between the
        // shooter's floor and the target's, +20 aim ("Height" in the shot
        // breakdown). The game's own test, HasHeightAdvantageOver, takes two
        // units where they stand, not a tile, so the rule is applied here: the
        // tile's floor against the enemy's feet (its pawn sits NAVH_LIFT
        // above them). IsPointWithinFiringRange's height output was logged for
        // this and gave 1.000 every time, from above an enemy as well.
        //
        // Only against an enemy with a line to the tile: the bonus is on a
        // shot, and the game's penalty reads "An enemy unit has elevated
        // position and can see this unit". The 17:37 log of 2026-09-27 said
        // "Out of sight. Height advantage on Sectoid." 19 times. The same
        // CanSeeActorToTile as the "Seen by" count below.
        int sees = see(world, NULL, pawn, tx, ty, tz, 0) != 0;
        if (sees) {
            const void* hv;
            if (field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &hv)) {
                float diff = here[2] - (((const float*)hv)[2] - NAVH_LIFT);
                int over = diff >= 192.0f, under = diff <= -192.0f;
                if (over || under) {
                    int* cnt = over ? &nover : &nunder;
                    char (*names)[48] = over ? over_names : under_names;
                    if (*cnt < EXPOSE_NAMES) {
                        UnitName* un = unit_by_unit(unit);
                        if (un) unit_label(un, names[*cnt], sizeof names[0]);
                        else strcpy_s(names[*cnt], sizeof names[0], "an enemy");
                    }
                    (*cnt)++;
                }
            }
        }

        // Flanking them does not wait on their seeing the tile: the game
        // asks it of every visible enemy in range.
        if (soldier && soldier != unit) {
            const void* lv;
            if (field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &lv) &&
                tile_flanks(soldier, unit, (const float*)lv, stand)) {
                if (nflank < EXPOSE_NAMES) {
                    UnitName* un = unit_by_unit(unit);
                    if (un) unit_label(un, flank_names[nflank], sizeof flank_names[0]);
                    else strcpy_s(flank_names[nflank], sizeof flank_names[0], "an enemy");
                }
                nflank++;
            }
        }

        if (!sees) continue;
        r->seen_by++;

        if (!has_cover || r->flanked || !g_expose_ok) continue;

        // Not virtual, so there is no vtable to go through -- the address is
        // fixed and `this` still travels in ecx.
        FlankCoverFn flanking = g_unit_fn_flanking
            ? (FlankCoverFn)g_unit_fn_flanking
            : (FlankCoverFn)tile_vfn(unit, g_unit_slot_flanking);
        FiringRangeFn in_range = (FiringRangeFn)tile_vfn(unit, g_unit_slot_range);
        if (!flanking || !in_range) continue;

        const void* v;
        if (!field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v)) continue;
        const float* eloc = (const float*)v;
        const float* cov = cp->cover_location;

        float height_bonus = 0.0f, dist_sq = SENTINEL_FLOAT;
        int reaches = in_range(unit, NULL, &height_bonus, &dist_sq, unit,
                               cov[0], cov[1], cov[2],
                               eloc[0], eloc[1], eloc[2], NULL, 0.0f);
        float want = dist_sq_between(cov, eloc);
        int past = reaches ? flanking(unit, NULL, *cp) : 0;

        if (g_expose_logged < EXPOSE_LOG_MAX) {
            g_expose_logged++;
            logf_("exposure: cover %d,%d,%d flags 0x%05X at (%.0f, %.0f, %.0f); "
                  "enemy at (%.0f, %.0f, %.0f); in range %d, dist^2 %.0f "
                  "(ours %.0f), height bonus %.2f -> flanking %d\n",
                  cp->x, cp->y, cp->z, (unsigned)cp->flags,
                  cov[0], cov[1], cov[2], eloc[0], eloc[1], eloc[2],
                  reaches, dist_sq, want, height_bonus, past);
        }

        // A squared distance the native disagrees with by more than a few
        // per cent means the Vectors did not go over the way it reads them,
        // and every answer built on them is noise. Said once, then dropped.
        if (dist_sq == SENTINEL_FLOAT || dist_sq < 0.0f ||
            (want > 1.0f && (dist_sq < want * 0.9f || dist_sq > want * 1.1f))) {
            g_expose_ok = 0;
            logf_("exposure: the native measured dist^2 %.0f where the two "
                  "positions give %.0f -- the call is not landing; flanking "
                  "dropped for this session\n", dist_sq, want);
            continue;
        }

        if (past) r->flanked = 1;
    }

    // "Sectoid", "2 Sectoids, Muton": the game names every alien of a kind
    // alike, and "Sectoid, Sectoid" read as a stammer (2026-09-27).
    names_counted(over_names, nover, r->height_over, sizeof r->height_over);
    names_counted(under_names, nunder, r->height_under, sizeof r->height_under);
    names_counted(flank_names, nflank, r->flanks, sizeof r->flanks);
}

// Describes tile (tx, ty) with its floor at `floor`. Returns 0 when the game
// could not be asked, leaving `say` empty. `with_dash` is off where the last
// path is not this tile's; `with_who` off where the units were said already.
int tile_report(int tx, int ty, float floor, int with_dash, int with_who,
                       char* say, size_t say_sz)
{
    say[0] = 0;
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;

    // Where XGAction_EndMove asks: the floor under the destination, plus 4.
    float x = grid_centre_x(&g, tx);
    float y = grid_centre_y(&g, ty);
    float z = floor + 4.0f;
    TileCoverPoint cp;
    memset(&cp, 0, sizeof cp);
    int has_cover = cover(world, NULL, x, y, z, &cp) != 0;

    // The layer is WORLD_FloorHeight (64) deep, measured from Min.Z as the
    // tile natives do.
    int tz = grid_layer(&g, z);
    TileTestFn smoke = (TileTestFn)tile_vfn(world, g_tile_slot_smoke);
    TileTestFn poison = (TileTestFn)tile_vfn(world, g_tile_slot_poison);

    TileReport r;
    memset(&r, 0, sizeof r);
    r.cover_flags = has_cover ? cp.flags : 0;
    r.smoke = smoke ? smoke(world, NULL, tx, ty, tz) != 0 : 0;
    r.poison = poison ? poison(world, NULL, tx, ty, tz) != 0 : 0;
    float here[3] = { x, y, floor };
    tile_exposure(tx, ty, tz, &cp, has_cover, here, &r);
    tile_rings(here, r.reach, sizeof r.reach);
    int cost = -1, std = -1, maxc = -1, moves = -1, turns = 0;
    int reach = with_dash ? tile_dash(&cost, &std, &maxc, &moves, &turns) : -1;
    r.dash = reach == 1;
    r.turns = reach == 2 ? turns : 0;

    // Who is standing there comes first: it is what the tile *is*.
    char who[TILE_MAX_TEXT] = "", what[TILE_MAX_TEXT];
    if (with_who) units_on_tile(tx, ty, 1, floor, who, sizeof who, NULL);
    tile_describe(&r, what, sizeof what);
    // After who stands there and before the cover: where the tile is comes
    // before what it offers. "Evac zone. Low cover. Seen by 1."
    int evac = 0;
    {
        Fault f;
        __try { evac = evac_at(tx, ty, floor); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("tile: evac", &f, NULL);
            evac = 0;
        }
    }
    _snprintf_s(say, say_sz, _TRUNCATE, "%s%s%s%s", who, who[0] ? " " : "",
                evac ? "Evac zone. " : "", what);

    // The cover point carries its own tile, which is the check on the one
    // asked about -- and on the layer this file worked out for smoke.
    logf_("tile: %d, %d floor %.1f (layer %d): cover %s flags 0x%05X at %d, %d, %d; "
          "path cost %d, standard move %d, max %d, moves made %d, turns %d, smoke %d, "
          "poison %d, seen by %d of %d known%s%s%s%s%s%s%s%s%s%s -> \"%s\"\n",
          tx, ty, floor, tz, has_cover ? "yes" : "no", (unsigned)cp.flags,
          cp.x, cp.y, cp.z, cost, std, maxc, moves, turns, r.smoke, r.poison,
          r.seen_by, r.enemies_known, r.flanked ? ", flanked" : "",
          r.flanks[0] ? ", flanks " : "", r.flanks,
          r.reach[0] ? ", rings " : "", r.reach,
          r.height_over[0] ? ", height on " : "", r.height_over,
          r.height_under[0] ? ", below " : "", r.height_under,
          evac ? ", evac zone" : "", say);
    return 1;
}
