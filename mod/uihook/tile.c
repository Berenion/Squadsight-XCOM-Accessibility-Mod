// What is on a tile: the part that does not touch the game.  See tile.h.

#include "tile.h"
#include "strings.h"
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
static const StrId DIR_NAME[D_COUNT] = {
    TILE_DIR_N, TILE_DIR_NE, TILE_DIR_E, TILE_DIR_SE, TILE_DIR_S, TILE_DIR_SW, TILE_DIR_W,
    TILE_DIR_NW,
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
static void cover_sentence(StrId kind, const int* dirs_on, char* out,
                           size_t out_sz, size_t* used)
{
    int n = 0, total = 0;
    for (int d = 0; d < D_COUNT; d++) total += dirs_on[d];
    if (!total) return;
    char dirs[256] = "";
    size_t dw = 0;
    for (int d = 0; d < D_COUNT; d++) {
        if (!dirs_on[d]) continue;
        n++;
        if (n > 1) append(dirs, sizeof dirs, &dw, n == total ? T(TXT_AND) : ", ");
        append(dirs, sizeof dirs, &dw, T(DIR_NAME[d]));
    }
    char t[320];
    tfmt(t, sizeof t, kind, dirs);
    append(out, out_sz, used, t);
    append(out, out_sz, used, " ");
}

void tile_describe(const TileReport* r, char* out, size_t out_sz)
{
    size_t used = 0;
    if (!out || !out_sz) return;
    out[0] = 0;

    if (r->no_route) {
        append(out, out_sz, &used, T(TILE_NO_ROUTE));
        append(out, out_sz, &used, " ");
    } else if (r->turns >= 2) {
        char t[64];
        tpfmt(t, sizeof t, TILE_TURNS, r->turns, r->turns);
        append(out, out_sz, &used, t);
        append(out, out_sz, &used, " ");
    } else if (r->dash) {
        append(out, out_sz, &used, T(TILE_DASH));
        append(out, out_sz, &used, " ");
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
        cover_sentence(TILE_HIGH_COVER_AT, high, out, out_sz, &used);
        cover_sentence(TILE_LOW_COVER_AT, low, out, out_sz, &used);
    } else {
        append(out, out_sz, &used, T(TILE_NO_COVER));
        append(out, out_sz, &used, " ");
    }

    // Exposure comes straight after the cover, because it is what says
    // whether that cover is worth anything. Nothing at all when no enemy has
    // been seen: there is no one for the tile to be exposed to, and a player
    // stepping around an empty map does not want "Out of sight" on every
    // tile. "Flanked" is said only where there IS cover -- flanking is cover
    // being got around, and on open ground "Seen by 2" has said it already.
    if (r->enemies_known > 0) {
        if (r->seen_by <= 0) {
            append(out, out_sz, &used, T(TILE_OUT_OF_SIGHT));
            append(out, out_sz, &used, " ");
        } else {
            char t[96];
            tfmt(t, sizeof t, any && r->flanked ? TILE_SEEN_BY_FLANKED : TILE_SEEN_BY, r->seen_by);
            append(out, out_sz, &used, t);
            append(out, out_sz, &used, " ");
        }
    }

    // The other side of flanking: the game marks a seen enemy the hovered
    // tile would flank (XComActionIconManager.AddFlankingIcons), so it is
    // said with the exposure it answers.
    if (r->flanks[0]) {
        char t[256];
        tfmt(t, sizeof t, TILE_FLANKS, r->flanks);
        append(out, out_sz, &used, t);
        append(out, out_sz, &used, " ");
    }

    // Height advantage either way: the game's rule is a storey (192) of
    // difference between the shooter's floor and the target's.
    if (r->height_over[0]) {
        char t[320];
        tfmt(t, sizeof t, TILE_HEIGHT_OVER, r->height_over);
        append(out, out_sz, &used, t);
        append(out, out_sz, &used, " ");
    }
    if (r->height_under[0]) {
        char t[320];
        tfmt(t, sizeof t, TILE_HEIGHT_UNDER, r->height_under);
        append(out, out_sz, &used, t);
        append(out, out_sz, &used, " ");
    }

    // Who an ability reaches from here: the game rings them while the move
    // is hovered. Already sentences.
    if (r->reach[0]) {
        append(out, out_sz, &used, r->reach);
        append(out, out_sz, &used, " ");
    }

    if (r->smoke)  { append(out, out_sz, &used, T(TILE_SMOKE)); append(out, out_sz, &used, " "); }
    if (r->poison) { append(out, out_sz, &used, T(TILE_POISON)); append(out, out_sz, &used, " "); }

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
    case TILE_REFUSE_NO_STOP:  return T(TILE_CANNOT_STOP);
    case TILE_REFUSE_BLOCKED:  return T(TILE_BLOCKED);
    case TILE_REFUSE_NO_FLOOR: return T(TILE_NO_FLOOR);
    default:                   return T(TILE_NO_PATH);
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
    if (!dx && !dy) { strncpy_s(out, out_sz, T(TILE_HERE), _TRUNCATE); return; }
    char ns[64] = "", ew[64] = "";
    if (dy) tfmt(ns, sizeof ns, dy > 0 ? TILE_N_NORTH : TILE_N_SOUTH, dy > 0 ? dy : -dy);
    if (dx) tfmt(ew, sizeof ew, dx > 0 ? TILE_N_EAST : TILE_N_WEST, dx > 0 ? dx : -dx);
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
        char where[128];
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
    int up = delta >= 0.0f;
    float d = delta >= 0.0f ? delta : -delta;
    int halves = (int)(d / 96.0f + 0.5f);          // half storeys
    if (halves == 0)
        tfmt(out, out_sz, up ? TILE_STEP_UP : TILE_STEP_DOWN);
    else if (halves == 1)
        tfmt(out, out_sz, up ? TILE_HALF_UP : TILE_HALF_DOWN);
    else if (halves % 2)
        tpfmt(out, out_sz, up ? TILE_STOREYS_HALF_UP : TILE_STOREYS_HALF_DOWN, halves / 2,
              halves / 2);
    else
        tpfmt(out, out_sz, up ? TILE_STOREYS_UP : TILE_STOREYS_DOWN, halves / 2, halves / 2);
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
    int n = levels >= 0 ? levels : -levels;
    tpfmt(out, out_sz, levels >= 0 ? TILE_FLOORS_UP : TILE_FLOORS_DOWN, n, n);
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
        char t[192];
        if (same > 1) {
            // English makes the name plural itself; any other language
            // cannot from here (the names are the game's, in its language),
            // so the line takes the name as it is ("%2$s x%1$d").
            char many[160];
            if (strings_is_english()) plural(names[i], many, sizeof many);
            else strncpy_s(many, sizeof many, names[i], _TRUNCATE);
            tfmt(t, sizeof t, TILE_NAME_COUNTED, same, many);
        } else {
            _snprintf_s(t, sizeof t, _TRUNCATE, "%s", names[i]);
        }
        if (parts++) append(out, out_sz, &used, ", ");
        append(out, out_sz, &used, t);
    }
    if (total > n) {
        char t[64];
        tfmt(t, sizeof t, parts ? TILE_AND_N_MORE : TILE_N_MORE, total - n);
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
        strncpy_s(out, out_sz, T(TILE_OUTSIDE), _TRUNCATE);
        return;
    }
    if (now->state == TILE_WHERE_ROOF) {
        strncpy_s(out, out_sz, T(TILE_ON_ROOF), _TRUNCATE);
        return;
    }

    // A one-storey building has no floor worth numbering. Each wording has a
    // line of its own -- inside a sentence, and starting one -- rather than
    // a capital made by hand.
    char floor[96] = "", floor_alone[96] = "";
    if (now->storeys > 1) {
        tfmt(floor, sizeof floor, TILE_FLOOR_OF, now->floor, now->storeys);
        tfmt(floor_alone, sizeof floor_alone, TILE_FLOOR_OF_ALONE, now->floor, now->storeys);
    } else if (now->storeys == 0 && now->floor > 1) {
        tfmt(floor, sizeof floor, TILE_FLOOR_N, now->floor);
        tfmt(floor_alone, sizeof floor_alone, TILE_FLOOR_N_ALONE, now->floor);
    }

    // The building is taller somewhere else than over this tile.
    char above[96] = "";
    if (floor[0] && now->no_above) _snprintf_s(above, sizeof above, _TRUNCATE, " %s", T(TILE_NO_FLOOR_ABOVE));
    if (force || moved_in) {
        const char* what = T(now->kind == TILE_UFO      ? TILE_INSIDE_UFO
                           : now->kind == TILE_DROPSHIP ? TILE_INSIDE_DROPSHIP
                           :                              TILE_INSIDE_BUILDING);
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s.%s", what, floor[0] ? ", " : "", floor,
                    above);
    } else if (floor[0]) {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s", floor_alone, above);
    }
}
