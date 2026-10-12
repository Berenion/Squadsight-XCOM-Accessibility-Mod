// The scanner's game side: where its lists come from, and its keys. scan.c
// holds the list, the categories and the words; see scan.h.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scanner.h"
#include "fog.h"
#include "strings.h"
#include "scan.h"
#include "units.h"
#include "world.h"
#include "where.h"
#include "counters.h"
#include "sounds.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "shot.h"
#include "speech.h"
#include "ue3.h"

// ---- the scanner -----------------------------------------------------------
//
// Ported from the Wasteland 2 accessibility mod, key for key. scan.h holds the
// keys and the reasoning; this is where the lists come from.
//
// Three sources, because XCOM keeps these things in three different places:
//
//   units   the flag table (units.c). A flag exists for every soldier,
//           alien and civilian, and unit_seen has already settled whether it
//           can be seen at all. The side comes from the unit's own m_eTeam
//           rather than from the flag: eTeam_Neutral is a civilian, and the
//           flag's m_bIsFriendly shares a dword with m_bIsDead in this build.
//
//   world   the game's object table, walked in world.c. Doors, windows,
//           panels, ladders, Meld canisters and the radar array are level
//           actors that never pass through the UI, and no native lists them:
//           GetInteractionPoints comes closest but covers only
//           XComInteractiveLevelActor, which a ladder is not.
//
//   climbs  the tiles around the soldier, from the cover flags the mod already
//           asks for. COVER_ClimbOnto_* and COVER_ClimbOver_* live in the same
//           flags word walls_scan reads, so a way up a ledge -- a ramp, a
//           crate, a low wall -- costs one query per tile and nothing else.
//
// Units are gated on being in sight now; level actors on the fog having lifted
// off them once (world_unseen in world.c). XCOM paints a tile nobody has seen
// black, so a sighted player does not know where the doors, the ladders or a
// UFO's power source are until the squad has looked; a tile seen before is
// only greyed, and what is on it stays on the list.

// COVER_ClimbOnto_N..W and COVER_ClimbOver_N..W, from XComWorldData.
#define COVER_CLIMB_ONTO 0x001E0000
#define COVER_CLIMB_OVER 0x01E00000

#define SCAN_CLIMB_RADIUS 12    // tiles each way the climb scan covers
#define SCAN_CLIMB_APART   4    // tiles between two climbs worth naming apart

// Where the scan was measured from, and the grid it was taken on, so Home and
// End answer about the same scan the player has just heard.
static CursorGrid g_scan_grid;
static int        g_scan_from[3];
static float      g_scan_world_z;   // the origin's own height, for tile queries
static int        g_scan_have;

// Fills in an item's tile from its world position. 0 when the grid is unknown.
//
// `lift` is how far the position stands above the floor it is on. A pawn's
// Location is its middle, NAVH_LIFT above its feet, and scan_origin
// takes that off the tile the scan is measured from -- so an item that does
// not take it off too reads one storey high. The first run said "Payne, here,
// one floor up" about the very soldier the scan was measured from. A level
// actor's Location is already at its base, so it passes 0 (and the level
// actors are placed by world.c on their own grid).
static int scan_item_at(ScanItem* it, const float* world, float lift)
{
    if (!g_scan_have) return 0;
    memcpy(it->world, world, 3 * sizeof(float));
    it->tx = grid_x(&g_scan_grid, world[0]);
    it->ty = grid_y(&g_scan_grid, world[1]);
    // Off the grid is not a place the cursor can go, so it is not a place the
    // scanner may offer. A class default object put "Radar array" on tile
    // 65, -12 -- world (0, 0), which is where an object with no position sits
    // -- and Home sent the cursor over the edge of the map after it.
    if (it->tx < 0 || it->ty < 0 ||
        it->tx >= g_scan_grid.num_x || it->ty >= g_scan_grid.num_y)
        return 0;
    float feet[3] = { world[0], world[1], world[2] - lift };
    it->tz = floor_of(feet);
    it->feet = feet[2];
    return 1;
}

// The floor to measure `it` from: the scan's own, unless the player and the
// item are in the same building -- then its own storeys, as F / C and a
// step's units count them (where_levels_apart). The 2026-10-01 (13:34) log
// had "Panel, here, one floor up" from the ground at -64 under a panel at
// 448, where F went -64 -> 450 as "2 floors up. Floor 2 of 3 does not reach
// this tile"; the cursor's floors (floor_of) and the building's disagreed.
static int scan_from_floor(const ScanItem* it)
{
    int dz;
    if (it->unplaced || !g_scan_have) return g_scan_from[2];
    if (!where_levels_apart(g_scan_from[0], g_scan_from[1], g_scan_world_z,
                            it->tx, it->ty, it->feet, &dz))
        return g_scan_from[2];
    if (it->tz - dz != g_scan_from[2])
        logf_("scan: %s is %d of the building's floors from here, %d of the cursor's\n",
              it->name, dz, it->tz - g_scan_from[2]);
    return it->tz - dz;
}

// ---- the units -------------------------------------------------------------

// The player the squad belongs to, kept from the last time it could be read.
//
// squad_player goes through the cursor's ChainedPawn, and during a soldier
// switch that is briefly nothing -- at which point every unit reads as not
// friendly and the scan comes back empty. The first run showed it: "Squad, 5
// found" and then, one key later, "No squad", with "nav: released (the soldier
// changed)" between them. The player does not change within a mission, so the
// last one read is the right answer while the cursor is between soldiers.
static void* g_scan_squad;

static void* scan_squad_player(void)
{
    void* p = squad_player();
    if (p) g_scan_squad = p;
    return p ? p : g_scan_squad;
}

static void scan_add_flagless(const SquadSight* sight);

static void scan_add_units(void)
{
    void* squad = scan_squad_player();
    static SquadSight sight;
    squad_sight_take(squad, &sight);

    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        if (!squad_sees(&sight, s.unit, s.loc, s.friendly, s.who->name)) continue;

        ScanItem it;
        memset(&it, 0, sizeof it);
        if (s.friendly) {
            it.kind = SCAN_SQUAD;
        } else if (unit_team(s.unit) == TEAM_NEUTRAL) {
            it.kind = SCAN_CIVILIANS;
        } else {
            it.kind = SCAN_ENEMIES;
            if (unit_overwatch(s.unit))
                strncpy_s(it.detail, sizeof it.detail, T(SCAN_ON_OVERWATCH), _TRUNCATE);
        }
        unit_label(&g_units[i], it.name, sizeof it.name);
        if (scan_item_at(&it, s.loc, NAVH_LIFT)) scan_add(&it);
    }
    if (squad) scan_add_flagless(&sight);

    // Enemies out of sight, at the place the squad last saw them, after
    // those in sight (rank -1). Home goes there as for any item.
    static KnownLost lost[KNOWN_MAX];
    int nlost = known_lost(squad, &sight.enemies, lost, KNOWN_MAX);
    for (int i = 0; i < nlost; i++) {
        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_ENEMIES;
        it.last_seen = 1;
        it.turns_ago = lost[i].turns_ago;
        it.rank = -1;
        strncpy_s(it.name, sizeof it.name, lost[i].label, _TRUNCATE);
        if (scan_item_at(&it, lost[i].loc, NAVH_LIFT)) scan_add(&it);
    }
}

// ---- the targets -----------------------------------------------------------
//
// What the soldier can shoot: the target strip's own list (strip_enemies),
// best shot first. Each is said with what the screen offers about it -- the
// hit chance its icon shows under the mouse, the cover shield and hit points
// on its flag, and the strip's flanked and squadsight marks.
//
// The hit chance is the one the strip itself shows. Hovering an icon
// (UISightlineHUD_SightlineContainer.OnMouseEvent, case 392) walks the
// soldier's m_aAbilities for the standard shot (iType 7, eAbility_ShotStandard)
// whose primary target is that enemy and puts its GetUIHitChance on the icon;
// for a standard shot that is GetHitChance, which is m_iHitChance. So the
// same walk is made here with field reads: the ability's m_aTargets[0]
// .m_kTarget stands for GetPrimaryTarget (native, and a standard shot has one
// target), and nothing is called.
static FieldSlot g_nabilities, g_abilities, g_ab_targets, g_ab_chance;
static uint32_t  g_itype_off;           // XGAbility.iType, the same in every subclass
static int       g_itype_have;

#define ABILITY_SHOT_STANDARD 7

static int soldier_chance_at(void* soldier, const void* enemy)
{
    const void* v;
    if (!field_ptr(soldier, "m_iNumAbilities", &g_nabilities, 4, &v)) return -1;
    int n = *(const int32_t*)v;
    if (n <= 0 || n > 64) return -1;
    if (!field_ptr(soldier, "m_aAbilities", &g_abilities, 64 * sizeof(void*), &v))
        return -1;
    void* abilities[64];
    memcpy(abilities, v, (size_t)n * sizeof(void*));
    for (int i = 0; i < n; i++) {
        uint8_t* a = (uint8_t*)abilities[i];
        if (!a || !unit_is_live(a)) continue;
        // One lookup for every ability class: iType is XGAbility's, so it
        // sits at the same offset in all of them, and asking each class in
        // turn would make the field cache walk a class chain per ability.
        if (!g_itype_have) {
            if (!object_field_offset(a, "iType", &g_itype_off)) return -1;
            g_itype_have = 1;
        }
        if (!readable(a + g_itype_off, 4) ||
            *(const int32_t*)(a + g_itype_off) != ABILITY_SHOT_STANDARD)
            continue;
        if (!field_ptr(a, "m_aTargets", &g_ab_targets, sizeof(void*), &v) ||
            *(void* const*)v != enemy)
            continue;
        if (!field_ptr(a, "m_iHitChance", &g_ab_chance, 4, &v)) continue;
        return *(const int32_t*)v;
    }
    return -1;
}

static void scan_add_targets(void)
{
    void* const* list;
    int n = strip_enemies(&list);
    if (n <= 0) return;
    void* enemies[SEEN_MAX];
    memcpy(enemies, list, (size_t)n * sizeof(void*));

    // The soldier's own sight, for squadsight: the strip marks an enemy that
    // is on it but not in the soldier's m_arrVisibleEnemies.
    void* soldier = soldier_unit();
    void* own[SEEN_MAX];
    int nown = -1;
    const void* v;
    if (soldier && field_ptr(soldier, "m_arrVisibleEnemies", &g_visen, sizeof(FArray), &v)) {
        const FArray* a = (const FArray*)v;
        if (a->Num == 0) nown = 0;
        else if (a->Num > 0 && a->Num <= SEEN_MAX &&
                 readable(a->Data, (size_t)a->Num * sizeof(void*))) {
            memcpy(own, a->Data, (size_t)a->Num * sizeof(void*));
            nown = a->Num;
        }
    }

    for (int i = 0; i < n; i++) {
        void* e = enemies[i];
        if (!e || !unit_is_live(e)) continue;
        UnitName* u = unit_by_unit(e);
        if (!u) continue;
        void* pawn = unit_pawn(e);
        if (!pawn || !unit_is_live(pawn) ||
            !field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v))
            continue;
        float loc[3];
        memcpy(loc, v, sizeof loc);

        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_TARGETS;
        unit_label(u, it.name, sizeof it.name);

        int chance = soldier ? soldier_chance_at(soldier, e) : -1;
        int squadsight = 0;
        if (nown >= 0) {
            squadsight = 1;
            for (int k = 0; k < nown; k++)
                if (own[k] == e) { squadsight = 0; break; }
        }
        ShotTarget t = { it.name, u->cover, u->strip_flanked, u->hp, u->hp_max, -1, 0 };
        shot_list_detail(&t, chance, squadsight, it.detail, sizeof it.detail);
        // Best shot first; an enemy with no shot at it goes last, nearest
        // first among themselves.
        it.rank = chance >= 0 ? chance + 1 : 0;
        if (scan_item_at(&it, loc, NAVH_LIFT)) scan_add(&it);
    }
}

static void scan_add_world(void)
{
    int n;
    if (!world_refresh(&g_scan_grid)) return;
    const ScanItem* items = world_items(&n);
    for (int i = 0; i < n; i++) scan_add(&items[i]);
}

static void scan_add_flagless(const SquadSight* sight)
{
    static FlaglessUnit found[COLUMN_FLAGLESS];
    int n = flagless_units(1, found, COLUMN_FLAGLESS);
    for (int i = 0; i < n; i++) {
        if (!squad_sees(sight, found[i].unit, found[i].loc, 0, found[i].name)) continue;
        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_CIVILIANS;
        strcpy_s(it.name, sizeof it.name, found[i].name);
        if (scan_item_at(&it, found[i].loc, NAVH_LIFT)) scan_add(&it);
    }
}

// ---- where the tutorial is holding the cursor ------------------------------
//
// The EW tutorial does not merely suggest a move, it refuses every other one,
// and a player who cannot see the pulsing marker has no way to find out
// where. It is one Kismet action: SeqAct_RestrictMovementCursor takes a
// Locator placed in the map, lifts its Z by 24, and hands it to
// XComPathingPawn.SetDirectedTargetPoint, which is what fills vTargetPoint.
// SeqAct_UnrestrictMovementCursor clears it again.
//
// â›” bUseTargetPoint is not read, although it exists. It is a bool among ten
// on that pawn and this build found no UBoolProperty::BitMask ("props: no
// BitMask"), so it would read as whatever its neighbours are. The game does
// not trust it alone either: XComDirectedTacticalExperience.InvalidMovement
// asks for the bool AND for X + Y + Z != 0, and the vector test is the one
// that survives having no mask.
//
// Filed under the objectives, because a tutorial waypoint is where the player
// has to go. Meld canisters were here too, and have their own category now
// (SCAN_MELD): one entry each, placed only once seen.
//
// What is NOT here: SeqAct_RestrictMovementCursorToCover, the tutorial's
// other restriction, which sets bFirstMoveOutOfCover and no position at all.
// That one is a rule and not a place; it has no tile to point at, and its
// flag is a bool with the same mask problem and no vector to fall back on.
static FieldSlot g_soldier_unit, g_path_pawn_field, g_target_point;

// The Locator's own Z, before the game lifted it: scan_item_at takes the lift
// back off to find the floor the marker stands on.
#define TUTORIAL_POINT_LIFT 24.0f

// The evac zone, under Objectives, on its tile nearest where the scan was
// measured from -- so Home takes the cursor to the edge of it that is
// closest, and the offsets say the shortest way in.
static void scan_add_evac(void)
{
    int nx, ny;
    float nz;
    int found = 0;
    GUARDED("scan: evac", found = evac_nearest(g_scan_from[0], g_scan_from[1], &nx, &ny, &nz),
            found = 0);
    if (!found) return;
    float at[3] = { grid_centre_x(&g_scan_grid, nx),
                    grid_centre_y(&g_scan_grid, ny), nz };
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;
    strncpy_s(it.name, sizeof it.name, T(SCAN_EVAC_ZONE), _TRUNCATE);
    if (scan_item_at(&it, at, 0.0f)) scan_add(&it);
}

// A terror mission's civilian count, under Objectives, as the HUD's counter
// top right draws it: UITerrorInfo.UpdateTerrorInfo writes "N Remaining"
// (m_nLiveCivilians - m_nSavedCivilians), "Saved" and "Lost" off its
// m_civilians, the XGAIPlayer_Animal XComTacticalController hands it with
// SetCiviliansData, redrawn whenever one of the three changes. The panel is
// the one that last drew (scanner_terror_panel, from main.c's capture of its
// SetDisplayText); it exists on terror missions only. No place to go, so it
// is unplaced: said as it stands, after everything that has a tile.
static void*     g_terror_panel;
static FieldSlot g_terror_civs, g_civs_live, g_civs_saved, g_civs_dead;

void scanner_terror_panel(void* panel)
{
    if (panel == g_terror_panel) return;
    g_terror_panel = panel;
    logf_("scan: the terror counter is %p\n", panel);
}

static void scan_add_civilian_count(void)
{
    void* panel = g_terror_panel;
    if (!panel || !unit_is_live(panel) || !object_is_a(panel, "UITerrorInfo")) return;
    const void* v;
    if (!field_ptr(panel, "m_civilians", &g_terror_civs, sizeof(void*), &v)) return;
    void* civs = *(void* const*)v;
    if (!civs || !unit_is_live(civs)) return;
    int live, saved, dead;
    if (!field_ptr(civs, "m_nLiveCivilians", &g_civs_live, sizeof(int), &v)) return;
    live = *(const int*)v;
    if (!field_ptr(civs, "m_nSavedCivilians", &g_civs_saved, sizeof(int), &v)) return;
    saved = *(const int*)v;
    if (!field_ptr(civs, "m_nDeadCivilians", &g_civs_dead, sizeof(int), &v)) return;
    dead = *(const int*)v;

    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;
    it.unplaced = 1;
    strncpy_s(it.name, sizeof it.name, T(SCAN_CAT_CIVILIANS), _TRUNCATE);
    tfmt(it.detail, sizeof it.detail, SCAN_CIVILIAN_COUNT, live - saved, saved, dead);
    scan_add(&it);
}

// The mission's turn counters top right (counters.c), under Objectives:
// "Turns until Air Strike, 8." Unplaced, like the civilian count. A Meld
// canister's counter is left to the Meld category, which says the same of
// the canister itself.
static void scan_add_counters(void)
{
    Counter c[COUNTERS_MAX];
    int n = counters_read(c, COUNTERS_MAX);
    for (int i = 0; i < n; i++) {
        if (c[i].meld) continue;
        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_OBJECTIVES;
        it.unplaced = 1;
        strncpy_s(it.name, sizeof it.name, c[i].label[0] ? c[i].label : T(SCAN_COUNTER), _TRUNCATE);
        strncpy_s(it.detail, sizeof it.detail, c[i].detail, _TRUNCATE);
        scan_add(&it);
    }
}

static void scan_add_tutorial(void)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn) return;
    if (!field_ptr(pawn, "m_kGameUnit", &g_soldier_unit, sizeof(void*), &v)) return;
    void* unit = *(void* const*)v;
    if (!unit || !unit_is_live(unit)) return;

    if (!field_ptr(unit, "m_kPathingPawn", &g_path_pawn_field, sizeof(void*), &v)) return;
    void* ppawn = *(void* const*)v;
    if (!ppawn || !unit_is_live(ppawn)) return;

    if (!field_ptr(ppawn, "vTargetPoint", &g_target_point, 3 * sizeof(float), &v)) return;
    const float* pt = (const float*)v;
    if (pt[0] + pt[1] + pt[2] == 0.0f) return;      // the game's own test

    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;
    strncpy_s(it.name, sizeof it.name, T(SCAN_TUTORIAL_TARGET), _TRUNCATE);
    int placed = scan_item_at(&it, pt, TUTORIAL_POINT_LIFT);
    if (placed) scan_add(&it);

    // Logged when the point moves, which is when the tutorial advances a
    // step -- not once per press, and not every frame.
    static float said[3];
    if (pt[0] != said[0] || pt[1] != said[1] || pt[2] != said[2]) {
        memcpy(said, pt, sizeof said);
        logf_("tutorial: movement restricted to (%.0f, %.0f, %.0f)%s\n",
              pt[0], pt[1], pt[2],
              placed ? "" : " -- off the grid, not offered");
        if (placed) logf_("tutorial: that is tile %d, %d, floor %d\n",
                          it.tx, it.ty, it.tz);
    }
}

// ---- the ways up -----------------------------------------------------------
//
// A ramp is not an actor: XComWorldData holds the ways up a ledge as
// COVER_ClimbOnto_* and COVER_ClimbOver_* in the same flags word GetCoverPoint
// already answers with, and a ramp shows as the ClimbOnto that leads onto it.
// So this walks the tiles around the soldier and keeps the ones whose flags
// say a unit can get up there. One query per tile, so it is bounded, and it
// runs only for its own category.

static void scan_add_climbs(void)
{
    void* world_data = cursor_world();
    if (!g_scan_have || !world_data) return;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world_data, g_tile_slot_cover);
    if (!cover) return;

    const CursorGrid* g = &g_scan_grid;
    float z = g_scan_world_z + 4.0f;    // just off the floor, as walls_scan asks
    static int kept[SCAN_MAX][2];
    int nkept = 0;

    for (int dy = -SCAN_CLIMB_RADIUS; dy <= SCAN_CLIMB_RADIUS; dy++) {
        for (int dx = -SCAN_CLIMB_RADIUS; dx <= SCAN_CLIMB_RADIUS; dx++) {
            int x = g_scan_from[0] + dx, y = g_scan_from[1] + dy;
            if (x < 0 || y < 0 || x >= g->num_x || y >= g->num_y) continue;

            float wx = grid_centre_x(g, x);
            float wy = grid_centre_y(g, y);
            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            // An answer about some other tile is an answer about some other
            // floor, and is worth less than no answer at all -- the same test
            // walls_scan makes.
            if (!cover(world_data, NULL, wx, wy, z, &cp)) continue;
            if (cp.x != x || cp.y != y) continue;

            int onto = (cp.flags & COVER_CLIMB_ONTO) != 0;
            int over = (cp.flags & COVER_CLIMB_OVER) != 0;
            if (!onto && !over) continue;

            // One entry per ledge, not per tile. Climbable cover is
            // everywhere -- the first run found 155 of them within twelve
            // tiles, which is a list nobody can use -- and a wall you can
            // vault is one place, however many tiles long it is. So a tile is
            // kept only when nothing already kept is within SCAN_CLIMB_APART.
            // NB: not `near` -- windows.h still defines that as nothing,
            // and `int near = 0;` compiles to `int = 0;`.
            int crowded = 0;
            for (int k = 0; k < nkept && !crowded; k++) {
                int kx = kept[k][0] - x, ky = kept[k][1] - y;
                if (kx * kx + ky * ky <= SCAN_CLIMB_APART * SCAN_CLIMB_APART)
                    crowded = 1;
            }
            if (crowded) continue;
            if (nkept < SCAN_MAX) {
                kept[nkept][0] = x;
                kept[nkept][1] = y;
                nkept++;
            }

            ScanItem it;
            memset(&it, 0, sizeof it);
            it.kind = SCAN_INTERACT;
            strncpy_s(it.name, sizeof it.name,
                      T(onto ? SCAN_LEDGE_UP : SCAN_LOW_WALL), _TRUNCATE);
            float here[3] = { wx, wy, z };
            if (scan_item_at(&it, here, 0.0f)) scan_add(&it);
        }
    }
}

// ---- high cover ------------------------------------------------------------
//
// Every tile with high cover on at least one side, on every storey, that the
// squad has seen: the places worth running to. The same cover query the
// climbs and the tile report make, so "high" is what tile_describe calls high
// -- a direction bit without its low bit -- and the sides are said the same
// way ("to the north and east").
//
// Gated on the fog, tile by tile: a black tile shows no wall, so a sighted
// player does not know there is cover on it. Seen before is grey and still
// shows it. As with the level actors, a fog that cannot be read hides nothing
// (fog.h, FOG_UNKNOWN), and the sweep's log line says which it was.
//
// The whole grid, not a radius: the fog is what bounds it, and the list is
// nearest first. The cover point's z is the layer asked about (tile: lines,
// "floor 0.0 (layer 1) ... at 25, 5, 1"), so a layer of air answers about the
// floor under it and is passed over.
//
// The first build asked the fog tile by tile and swept the whole map on every
// press: 940 ms each in the 2026-10-12 log (68 x 96 x 12, 9612 tiles asked,
// 84 kept), a stall a player could feel. Now the fog is read in one go
// (fog_tiles), and each tile's answer is kept for the mission: a press asks
// only about tiles that have come out of the fog since the last one. Cover
// can still be blown away, so what is about to be listed is asked again
// (cover_still_high) -- at most SCAN_MAX queries, not the map.

#define COVER_MAX       8192
#define COVER_HIGH_BITS (TILE_COVER_N | TILE_COVER_S | TILE_COVER_E | TILE_COVER_W)

enum { COVER_UNASKED, COVER_NOT_HIGH, COVER_HIGH };

typedef struct {
    short x, y, z;          // tile and layer
    short tz;               // the storey (floor_of)
    int   flags;
    float feet;             // the floor's height, for Home
} CoverTile;

static CoverTile      g_cover[COVER_MAX];
static int            g_ncover;
static unsigned char* g_cover_state;    // per tile and layer, COVER_*
static unsigned char* g_cover_fog;      // fog_tiles' answer, per tile and layer
static int            g_cover_cells;
static CursorGrid     g_cover_grid;
static void*          g_cover_world;

static int cover_is_high(int flags)
{
    // A side is high when its direction bit is on and its low bit is not.
    return (flags & COVER_HIGH_BITS & ~(flags >> 4)) != 0;
}

// The cover point for tile (x, y) on layer z, when the game's answer is about
// that very tile.
static int cover_ask(TileCoverFn cover, void* world_data, const CursorGrid* g,
                     int x, int y, int z, TileCoverPoint* cp)
{
    memset(cp, 0, sizeof *cp);
    if (!cover(world_data, NULL, grid_centre_x(g, x), grid_centre_y(g, y),
               grid_layer_middle(g, z), cp))
        return 0;
    return cp->x == x && cp->y == y && cp->z == z;
}

// A new map, or the first press: everything is unasked again.
static int cover_reset(void* world_data, const CursorGrid* g)
{
    int cells = g->num_x * g->num_y * g->num_z;
    g_ncover = 0;
    g_cover_grid = *g;
    g_cover_world = world_data;
    if (cells != g_cover_cells) {
        free(g_cover_state);
        free(g_cover_fog);
        g_cover_state = cells > 0 ? (unsigned char*)malloc((size_t)cells) : NULL;
        g_cover_fog = cells > 0 ? (unsigned char*)malloc((size_t)cells) : NULL;
        g_cover_cells = g_cover_state && g_cover_fog ? cells : 0;
    }
    if (!g_cover_cells) return 0;
    memset(g_cover_state, COVER_UNASKED, (size_t)cells);
    return 1;
}

static void cover_sweep(void)
{
    void* world_data = cursor_world();
    const CursorGrid* g = &g_scan_grid;
    if (!world_data) return;
    if (world_data != g_cover_world || memcmp(g, &g_cover_grid, sizeof *g) != 0 ||
        !g_cover_cells)
        if (!cover_reset(world_data, g)) return;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world_data, g_tile_slot_cover);
    if (!cover) return;

    LARGE_INTEGER freq, t0, t1, t2;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    world_fog_calibrate(g);
    // No fog to read hides nothing, as for the level actors: every tile is
    // taken as seen.
    int fog_ok = fog_tiles(g, g_cover_fog, g_cover_cells);
    if (!fog_ok) memset(g_cover_fog, FOG_SEEN, (size_t)g_cover_cells);
    QueryPerformanceCounter(&t1);

    int asked = 0, high = 0, dropped = 0;
    static int sample_said;
    for (int z = 0; z < g->num_z; z++) {
        for (int y = 0; y < g->num_y; y++) {
            int row = (z * g->num_y + y) * g->num_x;
            for (int x = 0; x < g->num_x; x++) {
                if (g_cover_state[row + x] != COVER_UNASKED) continue;
                if (g_cover_fog[row + x] == FOG_NEVER) continue;

                TileCoverPoint cp;
                asked++;
                if (!cover_ask(cover, world_data, g, x, y, z, &cp) || !cover_is_high(cp.flags)) {
                    g_cover_state[row + x] = COVER_NOT_HIGH;
                    continue;
                }
                if (g_ncover >= COVER_MAX) {
                    // Left unasked, so a later press can take it if room
                    // is made.
                    dropped++;
                    continue;
                }
                g_cover_state[row + x] = COVER_HIGH;
                high++;

                // The floor: the cover location's height when it is in this
                // layer, else the layer's bottom. The first run's line was
                // "tile location z 31.0, cover location z 0.0, layer bottom
                // -1.0": the tile location is the layer's middle, the cover
                // location the floor. Home settles the exact height itself.
                float bottom = grid_layer_bottom(g, z);
                float feet = cp.cover_location[2];
                if (feet < bottom - CURSOR_ABOVE_FLOOR || feet >= bottom + CURSOR_LAYER)
                    feet = bottom;
                if (!sample_said) {
                    sample_said = 1;
                    logf_("cover: first high cover %d, %d layer %d flags 0x%05X -- tile "
                          "location z %.1f, cover location z %.1f, layer bottom %.1f, "
                          "floor taken %.1f\n", x, y, z, (unsigned)cp.flags,
                          cp.tile_location[2], cp.cover_location[2], bottom, feet);
                }
                CoverTile* c = &g_cover[g_ncover++];
                c->x = (short)x;
                c->y = (short)y;
                c->z = (short)z;
                c->flags = cp.flags;
                c->feet = feet;
                float at[3] = { grid_centre_x(g, x), grid_centre_y(g, y), feet };
                c->tz = (short)floor_of(at);
            }
        }
    }
    QueryPerformanceCounter(&t2);

    // Only a sweep that asked something is worth a line: paging through a
    // list already swept asks nothing.
    if (asked) {
        double ms = 1000.0 / (double)freq.QuadPart;
        logf_("cover: asked %d newly seen tiles, %d high, %d held, in %.1f ms (fog %.1f, "
              "cover %.1f; %d x %d x %d grid%s%s)\n", asked, high, g_ncover,
              (t2.QuadPart - t0.QuadPart) * ms, (t1.QuadPart - t0.QuadPart) * ms,
              (t2.QuadPart - t1.QuadPart) * ms, g->num_x, g->num_y, g->num_z,
              fog_ok ? "" : ", fog unreadable -- not hidden",
              dropped ? ", list full" : "");
    }
}

// Whether a held tile still has high cover. A wall blown away since it was
// asked is dropped -- and its tile forgotten, so it is asked afresh if it
// comes back as something else.
static int cover_still_high(CoverTile* c)
{
    void* world_data = cursor_world();
    TileCoverFn cover = world_data ? (TileCoverFn)tile_vfn(world_data, g_tile_slot_cover) : NULL;
    if (!cover) return 1;
    TileCoverPoint cp;
    if (cover_ask(cover, world_data, &g_cover_grid, c->x, c->y, c->z, &cp) &&
        cover_is_high(cp.flags)) {
        c->flags = cp.flags;
        return 1;
    }
    logf_("cover: %d, %d layer %d has no high cover now -- dropped\n", c->x, c->y, c->z);
    g_cover_state[(c->z * g_cover_grid.num_y + c->y) * g_cover_grid.num_x + c->x] =
        COVER_NOT_HIGH;
    return 0;
}

// Nearest first, as scan_end sorts, so that the SCAN_MAX nearest are the
// ones kept when there are more.
static int g_cover_from[3];

static int cover_dist2(const CoverTile* c)
{
    int dx = c->x - g_cover_from[0], dy = c->y - g_cover_from[1];
    int dz = c->tz - g_cover_from[2];
    return dx * dx + dy * dy + 4 * dz * dz;
}

static int __cdecl cover_nearer(const void* a, const void* b)
{
    int da = cover_dist2((const CoverTile*)a), db = cover_dist2((const CoverTile*)b);
    return da < db ? -1 : da > db;
}

static void scan_add_cover(void)
{
    if (!g_scan_have) return;
    cover_sweep();
    g_cover_from[0] = g_scan_from[0];
    g_cover_from[1] = g_scan_from[1];
    g_cover_from[2] = g_scan_from[2];
    qsort(g_cover, (size_t)g_ncover, sizeof g_cover[0], cover_nearer);

    int floor = scan_floor();
    for (int i = 0; i < g_ncover && scan_count() < SCAN_MAX; i++) {
        CoverTile* c = &g_cover[i];
        // The storey filter first, so that only what will be listed is
        // asked again.
        if (floor != SCAN_ALL_FLOORS && c->tz != floor) continue;
        if (!cover_still_high(c)) {
            g_cover[i--] = g_cover[--g_ncover];     // order is restored next press
            continue;
        }
        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_COVER;
        strncpy_s(it.name, sizeof it.name, T(SCAN_HIGH_COVER), _TRUNCATE);
        char sides[128];
        if (tile_high_cover_sides(c->flags, sides, sizeof sides))
            tfmt(it.detail, sizeof it.detail, SCAN_COVER_TOWARDS, sides);
        it.tx = c->x;
        it.ty = c->y;
        it.tz = c->tz;
        it.feet = c->feet;
        it.world[0] = grid_centre_x(&g_scan_grid, c->x);
        it.world[1] = grid_centre_y(&g_scan_grid, c->y);
        it.world[2] = c->feet;
        scan_add(&it);
    }
}

// ---- building and saying ---------------------------------------------------

// Where the scan is measured from: the soldier being moved, falling back to
// the cursor when there is none. The height convention is nav's own -- a
// pawn's Location is a lift above its feet.
static int scan_origin(CursorGrid* g, int* tx, int* ty, int* tz, float* world_z)
{
    float z;
    if (!cursor_grid(g)) return 0;
    if (!soldier_tile(g, tx, ty, &z) && !cursor_tile(g, tx, ty, &z)) return 0;

    // While a step is being navigated, THAT tile is where the player is, and
    // it is what everything here is measured from: an offset is the keys left
    // to press, so after stepping one north towards a Floater two north the
    // answer has to be one north. The radar has always worked this way; the
    // scanner measured from the soldier and so kept saying two.
    //
    // Only while navigating. Off the numpad the cursor cannot be trusted to
    // say where the player is -- in mouse mode it follows the mouse every
    // frame, and a soldier switch leaves it wherever the mouse happens to
    // point -- which is the same reason navigation itself begins from the
    // soldier rather than from the cursor.
    int ntx, nty;
    if (nav_active() && nav_target(&ntx, &nty)) {
        int cx, cy;
        float cz;
        // The height comes from the cursor, which the mod places on the
        // navigated tile -- but only once it is actually there. A placement
        // that has not landed yet would otherwise hand over a height from
        // the tile the cursor is still on, which on a stairwell is a
        // different floor; the soldier's stands in until it does.
        if (cursor_tile(g, &cx, &cy, &cz) && cx == ntx && cy == nty) z = cz;
        *tx = ntx;
        *ty = nty;
    }

    float feet[3];
    feet[0] = grid_centre_x(g, *tx);
    feet[1] = grid_centre_y(g, *ty);
    feet[2] = z - NAVH_LIFT;
    *tz = floor_of(feet);
    // The climb scan asks the cover native, which wants a world height and
    // not a floor number -- so the height is carried out separately rather
    // than worked back out of a floor that is three grid rows deep.
    if (world_z) *world_z = feet[2];
    return 1;
}

static int scan_rebuild(void)
{
    CursorGrid g;
    int tx, ty, tz;
    // Asked before scan_begin, so a moment when there is no cursor to measure
    // from leaves the list that was there rather than emptying it.
    if (!scan_origin(&g, &tx, &ty, &tz, &g_scan_world_z)) return scan_count();
    g_scan_have = 1;
    g_scan_grid = g;
    g_scan_from[0] = tx;
    g_scan_from[1] = ty;
    g_scan_from[2] = tz;

    // Each part timed: all of this runs on the game's thread, inside one
    // frame. Written after the 2026-10-07 (23:21) log, where stepping through
    // the categories stuttered the sound and the frame rate fell from 107 to
    // 79 a second, with nothing in the log to say which part took the time.
    LARGE_INTEGER freq, t[6];
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t[0]);

    scan_begin(tx, ty, tz);
    ScanCategory c = scan_category();
    if (c == SCAN_ALL || c == SCAN_SQUAD || c == SCAN_ENEMIES || c == SCAN_CIVILIANS)
        scan_add_units();
    QueryPerformanceCounter(&t[1]);
    // Its own category only: "Everything" already has these enemies once,
    // under Enemies.
    if (c == SCAN_TARGETS) scan_add_targets();
    QueryPerformanceCounter(&t[2]);
    if (c == SCAN_ALL || c == SCAN_DOORS || c == SCAN_WINDOWS || c == SCAN_OBJECTIVES ||
        c == SCAN_INTERACT || c == SCAN_EXPLOSIVES || c == SCAN_MELD)
        scan_add_world();
    QueryPerformanceCounter(&t[3]);
    // Three field reads and no walk, so it costs nothing outside a tutorial
    // -- and inside one it is the only objective that matters.
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_tutorial();
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_evac();
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_civilian_count();
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_counters();
    QueryPerformanceCounter(&t[4]);
    // The climb scan is a query per tile, so it runs only when its own
    // category is showing: "Everything" would pay for it on every press, and
    // a hundred ledges would bury the doors and the people in it anyway.
    if (c == SCAN_INTERACT) scan_add_climbs();
    // The same for high cover, which sweeps every seen tile (cover_sweep).
    if (c == SCAN_COVER) scan_add_cover();
    int n = scan_end();
    QueryPerformanceCounter(&t[5]);

    double ms = 1000.0 / (double)freq.QuadPart;
    double total = (t[5].QuadPart - t[0].QuadPart) * ms;
    // A cheap rebuild says nothing: only one long enough to be heard.
    if (total >= 5.0)
        logf_("scan: %s rebuilt in %.1f ms -- units %.1f, targets %.1f, level %.1f, "
              "objectives %.1f, tiles %.1f\n",
              scan_category_name(c), total,
              (t[1].QuadPart - t[0].QuadPart) * ms, (t[2].QuadPart - t[1].QuadPart) * ms,
              (t[3].QuadPart - t[2].QuadPart) * ms, (t[4].QuadPart - t[3].QuadPart) * ms,
              (t[5].QuadPart - t[4].QuadPart) * ms);
    return n;
}

static void scan_say(const char* what)
{
    logf_("scan: %s\n", what);
    speech_say_now(what);
}

static void scan_say_selected(void)
{
    ScanItem it;
    char say[SCAN_MAX_TEXT];
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    scan_describe(&it, g_scan_from[0], g_scan_from[1], scan_from_floor(&it),
                  say, sizeof say);
    logf_("scan: %s  [%d of %d, %s]\n", say, scan_index(), scan_count(),
          scan_category_name(scan_category()));
    // A soldier picked here is the one "Follow one soldier" hears. Anything
    // else leaves the one already followed.
    if (it.kind == SCAN_SQUAD) hearts_follow(it.name);
    speech_say_now(say);
}

// Page Up and Page Down, with Ctrl for the category and Alt for the storey.
static void scan_press(int dir, int ctrl, int alt)
{
    char say[SCAN_MAX_TEXT];

    if (ctrl) {
        scan_cycle_category(dir);
        int n = scan_rebuild();
        scan_category_text(scan_category(), scan_floor(), n, say, sizeof say);
        scan_say(say);
        return;
    }

    if (alt) {
        scan_cycle_floor(dir, floor_count());
        int n = scan_rebuild();
        char where[48];
        scan_floor_text(scan_floor(), where, sizeof where);
        scan_category_text(scan_category(), scan_floor(), n, say, sizeof say);
        logf_("scan: %s %s\n", where, say);
        speech_say_now(say);
        return;
    }

    scan_rebuild();
    scan_cycle(dir);
    scan_say_selected();
}

static void scan_home(int shift)
{
    CursorGrid g;
    char say[SCAN_MAX_TEXT];
    int tx, ty;
    float z;

    if (shift) {
        if (!cursor_grid(&g) || !soldier_tile(&g, &tx, &ty, &z)) {
            scan_say(T(SCAN_NO_SOLDIER));
            return;
        }
        nav_focus(tx, ty, z - NAVH_LIFT, "the soldier");
        return;
    }

    ScanItem it;
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    if (it.unplaced) {
        scan_describe(&it, 0, 0, 0, say, sizeof say);
        scan_say(say);
        return;
    }
    nav_focus(it.tx, it.ty, it.world[2], it.name);
}

// End: how far the selection is and which way. Shift+End answers the same
// about the soldier, measured from wherever the cursor is now -- the way back.
static void scan_distance(int shift)
{
    CursorGrid g;
    char say[SCAN_MAX_TEXT];

    if (shift) {
        int sx, sy, cx, cy;
        float sz, cz;
        if (!cursor_grid(&g) || !soldier_tile(&g, &sx, &sy, &sz) ||
            !cursor_tile(&g, &cx, &cy, &cz)) {
            scan_say(T(SCAN_NO_SOLDIER));
            return;
        }
        char where[64];
        tile_offset_text(sx - cx, sy - cy, where, sizeof where);
        tfmt(say, sizeof say, SCAN_SOLDIER_AT, where);
        scan_say(say);
        return;
    }

    ScanItem it;
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    // Measured afresh from where the player is NOW -- the navigated tile
    // while there is one, the soldier otherwise -- and not from where the
    // scan was taken: the whole point of the key is to ask again after
    // moving, and the answer is the keys still to press.
    int tx, ty, tz;
    if (scan_origin(&g, &tx, &ty, &tz, &g_scan_world_z)) {
        g_scan_grid = g;
        g_scan_from[0] = tx; g_scan_from[1] = ty; g_scan_from[2] = tz;
        g_scan_have = 1;
    }
    scan_describe(&it, g_scan_from[0], g_scan_from[1], scan_from_floor(&it),
                  say, sizeof say);
    scan_say(say);
}

// The scanner's keys. Page Up and Page Down are unbound in a mission, and Home
// only raises an InputEvent nothing handles, so all three are read the way the
// numpad is. End is the exception -- it is End Turn -- and is swallowed in
// hook_moviecheck, which is the only reason it can be used here.
static int g_scan_down[4];      // Page Up, Page Down, Home, End

void scan_poll(int act)
{
    static const int keys[4] = { VK_PRIOR, VK_NEXT, VK_HOME, VK_END };
    int ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    int shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;

    for (int k = 0; k < 4; k++) {
        int down = (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
        if (down && !g_scan_down[k] && !act) {
            logf_("scan: key %d with another screen first -- not read\n", k);
        } else if (down && !g_scan_down[k]) {
            Fault f;
            __try {
                switch (k) {
                case 0: scan_press(-1, ctrl, alt); break;
                case 1: scan_press(+1, ctrl, alt); break;
                case 2: scan_home(shift);          break;
                case 3: scan_distance(shift);      break;
                }
            }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("scan", &f, NULL);
            }
        }
        g_scan_down[k] = down;
    }
}

void scan_keys_forget(void)
{
    memset(g_scan_down, 0, sizeof g_scan_down);
}

