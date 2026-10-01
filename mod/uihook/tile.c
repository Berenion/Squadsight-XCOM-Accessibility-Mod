// What is on a tile: the part that does not touch the game.  See tile.h.

#include "tile.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

const unsigned char* tile_direct_target(const unsigned char* code, size_t n,
                                        const unsigned char* lo,
                                        const unsigned char* hi)
{
    const unsigned char* found = NULL;
    int this_loaded = 0;
    for (size_t i = 0; i + 5 <= n; i++) {
        if (code[i] == 0xC2 && i + 2 < n && code[i + 1] == 8 && code[i + 2] == 0)
            break;                                  // ret 8: the thunk's end

        // `mov ecx, ebx/ebp/esi/edi` -- the saved `this` going back into the
        // register the call will take it in. The same discriminator
        // tile_vtable_slot uses, and for the same reason: a thunk makes other
        // calls, and those load ecx from memory or with a constant (the
        // `mov ecx, 10` before the struct copy's rep movsd).
        if (code[i] == 0x8B && (code[i + 1] == 0xCB || code[i + 1] == 0xCD ||
                                code[i + 1] == 0xCE || code[i + 1] == 0xCF)) {
            this_loaded = 1;
            continue;
        }
        if (code[i] != 0xE8 || !this_loaded) continue;

        int32_t rel = (int32_t)((uint32_t)code[i + 1] | (uint32_t)code[i + 2] << 8 |
                                (uint32_t)code[i + 3] << 16 | (uint32_t)code[i + 4] << 24);
        const unsigned char* target = code + i + 5 + rel;
        if (target < lo || target >= hi) continue;  // not in the game's image
        found = target;                             // the last one wins
        this_loaded = 0;
    }
    return found;
}

int tile_vtable_slot(const unsigned char* code, size_t n)
{
    int found = -1;
    for (size_t i = 0; i + 6 <= n; i++) {
        if (code[i] == 0xC2 && i + 2 < n && code[i + 1] == 8 && code[i + 2] == 0)
            break;                                  // ret 8: the thunk's end
        // mov r32, [r32 + disp32]: mod 10, no SIB.
        if (code[i] != 0x8B || (code[i + 1] & 0xC0) != 0x80 || (code[i + 1] & 7) == 4)
            continue;
        int reg = (code[i + 1] >> 3) & 7;
        unsigned disp = (unsigned)code[i + 2] | (unsigned)code[i + 3] << 8 |
                        (unsigned)code[i + 4] << 16 | (unsigned)code[i + 5] << 24;
        if (disp % 4 || disp < 0x100 || disp >= 0x800) continue;

        // The call through that register, with `this` loaded just before it
        // from a callee-saved register: `mov ecx, ebx` in the thunks that
        // decode arguments, `mov ecx, esi` in those that take none. Thunks
        // also call through a register for the out-parameter notify, which
        // loads ecx from memory instead and is passed over.
        int this_loaded = 0;
        for (size_t j = i + 6; j + 1 < n && j < i + 0x30; j++) {
            if (code[j] == 0x8B && (code[j + 1] == 0xCB || code[j + 1] == 0xCD ||
                                    code[j + 1] == 0xCE || code[j + 1] == 0xCF))
                this_loaded = 1;
            if (code[j] == 0xFF && code[j + 1] == 0xD0 + reg) {
                if (this_loaded) found = (int)disp;
                break;
            }
        }
    }
    return found;
}

// The game's four cover bits in the numpad's directions, straight and turned
// 45 degrees. From the table GetWorldDirection reads (EW .data 0x1c1b050 /
// 0x1c1b1a0, filled at startup): North (0,1), South (0,-1), East (-1,0),
// West (1,0); diagonal North (-.71,.71), South (.71,-.71), East (-.71,-.71),
// West (.71,.71). Each vector points from the tile towards its cover.
enum { D_N, D_NE, D_E, D_SE, D_S, D_SW, D_W, D_NW, D_COUNT };
static const char* const DIR_NAME[D_COUNT] = {
    "north", "northeast", "east", "southeast", "south", "southwest", "west", "northwest",
};
static const int STRAIGHT[4] = { D_N, D_S, D_W, D_E };
static const int DIAGONAL[4] = { D_NW, D_SE, D_SW, D_NE };

static void append(char* out, size_t out_sz, size_t* used, const char* s)
{
    if (*used >= out_sz) return;
    int w = _snprintf_s(out + *used, out_sz - *used, _TRUNCATE, "%s", s);
    *used = w < 0 ? out_sz - 1 : *used + (size_t)w;
}

// "High cover north and east. " -- the directions in clockwise order.
static void cover_sentence(const char* kind, const int* dirs_on, char* out,
                           size_t out_sz, size_t* used)
{
    int n = 0, total = 0;
    for (int d = 0; d < D_COUNT; d++) total += dirs_on[d];
    if (!total) return;
    append(out, out_sz, used, kind);
    for (int d = 0; d < D_COUNT; d++) {
        if (!dirs_on[d]) continue;
        n++;
        append(out, out_sz, used, n == 1 ? " " : n == total ? " and " : ", ");
        append(out, out_sz, used, DIR_NAME[d]);
    }
    append(out, out_sz, used, ". ");
}

void tile_describe(const TileReport* r, char* out, size_t out_sz)
{
    size_t used = 0;
    if (!out || !out_sz) return;
    out[0] = 0;

    if (r->no_route) {
        append(out, out_sz, &used, "No route within reach. ");
    } else if (r->turns >= 2) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%d turns. ", r->turns);
        append(out, out_sz, &used, t);
    } else if (r->dash) {
        append(out, out_sz, &used, "Dash. ");
    }

    int high[D_COUNT] = { 0 }, low[D_COUNT] = { 0 };
    const int* map = (r->cover_flags & TILE_COVER_DIAGONAL) ? DIAGONAL : STRAIGHT;
    int any = 0;
    for (int b = 0; b < 4; b++) {
        if (!(r->cover_flags & (1 << b))) continue;
        any = 1;
        if (r->cover_flags & (0x10 << b)) low[map[b]] = 1;
        else                              high[map[b]] = 1;
    }
    if (any) {
        cover_sentence("High cover", high, out, out_sz, &used);
        cover_sentence("Low cover", low, out, out_sz, &used);
    } else {
        append(out, out_sz, &used, "No cover. ");
    }

    // Exposure comes straight after the cover, because it is what says
    // whether that cover is worth anything. Nothing at all when no enemy has
    // been seen: there is no one for the tile to be exposed to, and a player
    // stepping around an empty map does not want "Out of sight" on every
    // tile. "Flanked" is said only where there IS cover -- flanking is cover
    // being got around, and on open ground "Seen by 2" has said it already.
    if (r->enemies_known > 0) {
        if (r->seen_by <= 0) {
            append(out, out_sz, &used, "Out of sight. ");
        } else {
            char t[48];
            _snprintf_s(t, sizeof t, _TRUNCATE, "Seen by %d%s. ",
                        r->seen_by, any && r->flanked ? ", flanked" : "");
            append(out, out_sz, &used, t);
        }
    }

    // The other side of flanking: the game marks a seen enemy the hovered
    // tile would flank (XComActionIconManager.AddFlankingIcons), so it is
    // said with the exposure it answers.
    if (r->flanks[0]) {
        char t[128];
        _snprintf_s(t, sizeof t, _TRUNCATE, "Flanks %s. ", r->flanks);
        append(out, out_sz, &used, t);
    }

    // Height advantage either way: the game's rule is a storey (192) of
    // difference between the shooter's floor and the target's.
    if (r->height_over[0]) {
        char t[160];
        _snprintf_s(t, sizeof t, _TRUNCATE, "Height advantage on %s. ", r->height_over);
        append(out, out_sz, &used, t);
    }
    if (r->height_under[0]) {
        char t[160];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s above you. ", r->height_under);
        append(out, out_sz, &used, t);
    }

    // Who an ability reaches from here: the game rings them while the move
    // is hovered. Already sentences.
    if (r->reach[0]) {
        append(out, out_sz, &used, r->reach);
        append(out, out_sz, &used, " ");
    }

    if (r->smoke)  append(out, out_sz, &used, "Smoke. ");
    if (r->poison) append(out, out_sz, &used, "Poison. ");

    // The last sentence's trailing space.
    while (used > 0 && out[used - 1] == ' ') out[--used] = 0;
}

TileRefusal tile_refusal(const TileLayerFlags* layers, int n)
{
    int floor = 0, occupied = 0;
    for (int i = 0; i < n; i++) {
        // A layer a move may end on outranks everything: the tile is fine,
        // and what fails is getting there.
        if (layers[i].destination) return TILE_REFUSE_NO_PATH;
        floor |= layers[i].floor;
        // Solid stuff under the ground's layer is what a drop lands on, not
        // something in the way: at the map's west edge in the first run the
        // layers under the soldier were full and the three above empty, and
        // "Blocked." was said of a tile with nothing at all at head height.
        if (!layers[i].below) occupied |= layers[i].occupied;
    }
    if (floor) return TILE_REFUSE_NO_STOP;
    if (occupied) return TILE_REFUSE_BLOCKED;
    // No layer at all is no evidence, and says only what was said before.
    return n > 0 ? TILE_REFUSE_NO_FLOOR : TILE_REFUSE_NO_PATH;
}

const char* tile_refusal_text(TileRefusal r)
{
    switch (r) {
    case TILE_REFUSE_NO_STOP:  return "Cannot stop here.";
    case TILE_REFUSE_BLOCKED:  return "Blocked.";
    case TILE_REFUSE_NO_FLOOR: return "No floor.";
    default:                   return "No path.";
    }
}

int tile_turns(int cost, int this_turn, int standard)
{
    if (cost < 0 || this_turn <= 0 || standard <= 0) return 0;
    if (cost <= this_turn) return 1;
    int per_turn = 2 * standard;
    return 1 + (cost - this_turn + per_turn - 1) / per_turn;
}

int tile_no_route(int dx, int dy, int cost, int max)
{
    if (cost < 0 || max <= 0) return 0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int steps = ax > ay ? ax : ay;
    return max - cost > steps + TILE_NO_ROUTE_SLACK;
}

void tile_offset_text(int dx, int dy, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    if (!dx && !dy) { _snprintf_s(out, out_sz, _TRUNCATE, "here"); return; }
    char ns[32] = "", ew[32] = "";
    if (dy) _snprintf_s(ns, sizeof ns, _TRUNCATE, "%d %s", dy > 0 ? dy : -dy,
                        dy > 0 ? "north" : "south");
    if (dx) _snprintf_s(ew, sizeof ew, _TRUNCATE, "%d %s", dx > 0 ? dx : -dx,
                        dx > 0 ? "east" : "west");
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", ns, ns[0] && ew[0] ? ", " : "", ew);
}

#define CONTACTS_MAX 64

void tile_contacts(const TileContact* c, int n, const char* none,
                   char* out, size_t out_sz)
{
    size_t used = 0;
    if (!out || !out_sz) return;
    out[0] = 0;
    if (n <= 0) { append(out, out_sz, &used, none); return; }
    if (n > CONTACTS_MAX) n = CONTACTS_MAX;

    // Nearest first, by straight-line distance; ties keep the order given.
    int order[CONTACTS_MAX];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        int dk = c[k].dx * c[k].dx + c[k].dy * c[k].dy;
        while (j >= 0 && c[order[j]].dx * c[order[j]].dx +
                         c[order[j]].dy * c[order[j]].dy > dk) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; i++) {
        const TileContact* t = &c[order[i]];
        char where[64];
        tile_offset_text(t->dx, t->dy, where, sizeof where);
        if (i) append(out, out_sz, &used, " ");
        append(out, out_sz, &used, t->name);
        append(out, out_sz, &used, ", ");
        append(out, out_sz, &used, where);
        append(out, out_sz, &used, ".");
    }
}

void tile_height_step(float delta, char* out, size_t out_sz)
{
    const char* way = delta >= 0.0f ? "up" : "down";
    float d = delta >= 0.0f ? delta : -delta;
    int halves = (int)(d / 96.0f + 0.5f);          // half storeys
    if (halves == 0)
        _snprintf_s(out, out_sz, _TRUNCATE, "A step %s.", way);
    else if (halves == 1)
        _snprintf_s(out, out_sz, _TRUNCATE, "Half a storey %s.", way);
    else if (halves == 2)
        _snprintf_s(out, out_sz, _TRUNCATE, "One storey %s.", way);
    else if (halves % 2)
        _snprintf_s(out, out_sz, _TRUNCATE, "%d and a half storeys %s.", halves / 2, way);
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%d storeys %s.", halves / 2, way);
}

// "Floater" -> "Floaters", "Thin Man" -> "Thin Men", "Chryssalis" ->
// "Chryssalises". Good enough for the names the game gives units and meshes.
static void plural(const char* name, char* out, size_t out_sz)
{
    size_t n = strlen(name);
    if (n >= 3 && strcmp(name + n - 3, "Man") == 0)
        _snprintf_s(out, out_sz, _TRUNCATE, "%.*sMen", (int)(n - 3), name);
    else if (n && (name[n - 1] == 's' || name[n - 1] == 'x'))
        _snprintf_s(out, out_sz, _TRUNCATE, "%ses", name);
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%ss", name);
}

void tile_floor_step(int levels, char* out, size_t out_sz)
{
    const char* way = levels >= 0 ? "up" : "down";
    int n = levels >= 0 ? levels : -levels;
    if (n == 1) _snprintf_s(out, out_sz, _TRUNCATE, "One floor %s.", way);
    else _snprintf_s(out, out_sz, _TRUNCATE, "%d floors %s.", n, way);
}

void tile_names_counted(const char* const* names, int n, int total, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    char done[TILE_NAMES_MAX];
    if (n > TILE_NAMES_MAX) n = TILE_NAMES_MAX;
    memset(done, 0, sizeof done);
    int parts = 0;
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        int same = 1;
        for (int j = i + 1; j < n; j++)
            if (!done[j] && strcmp(names[j], names[i]) == 0) { done[j] = 1; same++; }
        char t[96];
        if (same > 1) {
            char many[80];
            plural(names[i], many, sizeof many);
            _snprintf_s(t, sizeof t, _TRUNCATE, "%d %s", same, many);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s", names[i]);
        }
        if (parts++) append(out, out_sz, &used, ", ");
        append(out, out_sz, &used, t);
    }
    if (total > n) {
        char t[32];
        _snprintf_s(t, sizeof t, _TRUNCATE, "%s%d more", parts ? ", and " : "", total - n);
        append(out, out_sz, &used, t);
    }
}

void tile_where_text(const TileWhere* before, const TileWhere* now, int force,
                     char* out, size_t out_sz)
{
    out[0] = 0;
    int moved_in = now->state != before->state ||
                   (now->state != TILE_WHERE_OUTSIDE && now->building != before->building);
    int changed = moved_in ||
                  (now->state == TILE_WHERE_INSIDE && now->floor != before->floor);
    if (before->state == TILE_WHERE_UNKNOWN && now->state == TILE_WHERE_OUTSIDE) changed = 0;
    if (!changed && !force) return;

    if (now->state == TILE_WHERE_OUTSIDE) {
        _snprintf_s(out, out_sz, _TRUNCATE, "Outside.");
        return;
    }
    if (now->state == TILE_WHERE_ROOF) {
        _snprintf_s(out, out_sz, _TRUNCATE, "On the roof.");
        return;
    }

    // A one-storey building has no floor worth numbering.
    char floor[48] = "";
    if (now->storeys > 1)
        _snprintf_s(floor, sizeof floor, _TRUNCATE, "floor %d of %d", now->floor, now->storeys);
    else if (now->storeys == 0 && now->floor > 1)
        _snprintf_s(floor, sizeof floor, _TRUNCATE, "floor %d", now->floor);

    if (force || moved_in) {
        const char* what = now->kind == TILE_UFO      ? "Inside the UFO"
                         : now->kind == TILE_DROPSHIP ? "Inside the dropship"
                         :                              "Inside building";
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s.", what, floor[0] ? ", " : "", floor);
    } else if (floor[0]) {
        _snprintf_s(out, out_sz, _TRUNCATE, "F%s.", floor + 1);
    }
}
