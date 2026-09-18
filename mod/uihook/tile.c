// What is on a tile: the part that does not touch the game.  See tile.h.

#include "tile.h"
#include <stdio.h>
#include <string.h>

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

    if (r->turns >= 2) {
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

    if (r->smoke)  append(out, out_sz, &used, "Smoke. ");
    if (r->poison) append(out, out_sz, &used, "Poison. ");

    // The last sentence's trailing space.
    while (used > 0 && out[used - 1] == ' ') out[--used] = 0;
}

int tile_turns(int cost, int this_turn, int standard)
{
    if (cost < 0 || this_turn <= 0 || standard <= 0) return 0;
    if (cost <= this_turn) return 1;
    int per_turn = 2 * standard;
    return 1 + (cost - this_turn + per_turn - 1) / per_turn;
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
