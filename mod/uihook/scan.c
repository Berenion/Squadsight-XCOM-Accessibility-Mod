// The scanner: the part that does not touch the game. See scan.h.

#include "scan.h"
#include "tile.h"
#include <stdio.h>
#include <string.h>

static ScanItem     g_items[SCAN_MAX];
static int          g_n;
static int          g_index = -1;
static ScanCategory g_category = SCAN_ALL;
static int          g_floor = SCAN_ALL_FLOORS;

// Where the scan was taken from, kept so a rebuilt list sorts the same way.
static int g_from[3];

// The selection is held by what it is rather than by index: the list is thrown
// away and rebuilt on every key press, and a unit that walked a tile would
// otherwise change places in it and the cycling would jump.
static int  g_have_sel;
static char g_sel_name[SCAN_NAME];
static int  g_sel_tile[3];

const char* scan_category_name(ScanCategory c)
{
    switch (c) {
    case SCAN_ALL:        return "Everything";
    case SCAN_SQUAD:      return "Squad";
    case SCAN_ENEMIES:    return "Enemies";
    case SCAN_TARGETS:    return "Targets";
    case SCAN_EXPLOSIVES: return "Explosives";
    case SCAN_CIVILIANS:  return "Civilians";
    case SCAN_DOORS:      return "Doors";
    case SCAN_OBJECTIVES: return "Objectives";
    case SCAN_INTERACT:   return "Interactables";
    default:              return "Unknown";
    }
}

ScanCategory scan_category(void) { return g_category; }
int scan_floor(void)             { return g_floor; }
int scan_count(void)             { return g_n; }
int scan_index(void)             { return g_index < 0 ? 0 : g_index + 1; }

void scan_forget(void)
{
    g_n = 0;
    g_index = -1;
    g_have_sel = 0;
    g_sel_name[0] = 0;
}

void scan_begin(int from_tx, int from_ty, int from_tz)
{
    g_n = 0;
    g_from[0] = from_tx;
    g_from[1] = from_ty;
    g_from[2] = from_tz;
}

int scan_add(const ScanItem* item)
{
    if (!item || g_n >= SCAN_MAX) return 0;
    // "Everything" takes each item under its own kind; a named category takes
    // only its own. An item with no name is not worth cycling to.
    if (g_category != SCAN_ALL && item->kind != g_category) return 0;
    if (!item->name[0]) return 0;
    if (g_floor != SCAN_ALL_FLOORS && item->tz != g_floor) return 0;
    g_items[g_n++] = *item;
    return 1;
}

// Straight-line distance in tiles, squared, from where the scan began. The
// storey counts: a door directly overhead is not as close as it looks from
// above, and sorting without it puts the floor above first on every stairwell.
static int dist2(const ScanItem* it)
{
    int dx = it->tx - g_from[0];
    int dy = it->ty - g_from[1];
    int dz = it->tz - g_from[2];
    return dx * dx + dy * dy + 4 * dz * dz;
}

// Whether `a` goes after `b`: the higher rank first, then the nearer.
static int after(const ScanItem* a, const ScanItem* b)
{
    if (a->rank != b->rank) return a->rank < b->rank;
    return dist2(a) > dist2(b);
}

int scan_end(void)
{
    // Insertion sort: the lists are short (a mission's doors and units, not a
    // map's polygons) and it keeps the order items were added in for ties, so
    // two doors the same distance away are always cycled the same way round.
    for (int i = 1; i < g_n; i++) {
        ScanItem k = g_items[i];
        int j = i - 1;
        while (j >= 0 && after(&g_items[j], &k)) {
            g_items[j + 1] = g_items[j];
            j--;
        }
        g_items[j + 1] = k;
    }

    // Put the selection back where it was. The name alone is not enough --
    // there are five doors -- and the tile alone is not either, since units
    // move, so the name decides and the tile breaks the tie.
    g_index = -1;
    if (g_have_sel) {
        int by_name = -1;
        for (int i = 0; i < g_n; i++) {
            if (strcmp(g_items[i].name, g_sel_name) != 0) continue;
            if (by_name < 0) by_name = i;
            if (g_items[i].tx == g_sel_tile[0] && g_items[i].ty == g_sel_tile[1] &&
                g_items[i].tz == g_sel_tile[2]) {
                g_index = i;
                break;
            }
        }
        if (g_index < 0) g_index = by_name;     // it moved; follow the name
        if (g_index < 0) g_have_sel = 0;        // it is gone
    }
    return g_n;
}

static void remember(int index)
{
    g_index = index;
    g_have_sel = 1;
    strncpy_s(g_sel_name, sizeof g_sel_name, g_items[index].name, _TRUNCATE);
    g_sel_tile[0] = g_items[index].tx;
    g_sel_tile[1] = g_items[index].ty;
    g_sel_tile[2] = g_items[index].tz;
}

int scan_cycle(int dir)
{
    if (g_n <= 0) { g_index = -1; g_have_sel = 0; return 0; }
    if (g_index < 0) {
        // Nothing selected yet: forwards starts at the nearest, backwards at
        // the farthest, so the first press in either direction says something
        // rather than wrapping past the whole list.
        g_index = dir >= 0 ? 0 : g_n - 1;
    } else {
        g_index += dir >= 0 ? 1 : -1;
        if (g_index >= g_n) g_index = 0;
        else if (g_index < 0) g_index = g_n - 1;
    }
    remember(g_index);
    return 1;
}

void scan_cycle_category(int dir)
{
    int c = (int)g_category + (dir >= 0 ? 1 : -1);
    if (c >= (int)SCAN_CATEGORIES) c = 0;
    else if (c < 0) c = (int)SCAN_CATEGORIES - 1;
    g_category = (ScanCategory)c;
    g_index = -1;
    g_have_sel = 0;
    g_n = 0;
}

int scan_cycle_floor(int dir, int nz)
{
    // The filter runs off the bottom storey to the top and then off again to
    // "all floors", so the way back to no filter is one press in either
    // direction from an end rather than a walk through the whole map.
    if (nz <= 0) nz = 1;
    int f = g_floor;
    if (dir >= 0) {
        if (f == SCAN_ALL_FLOORS) f = 0;
        else if (++f >= nz) f = SCAN_ALL_FLOORS;
    } else {
        if (f == SCAN_ALL_FLOORS) f = nz - 1;
        else if (--f < 0) f = SCAN_ALL_FLOORS;
    }
    g_floor = f;
    g_index = -1;
    g_have_sel = 0;
    g_n = 0;
    return g_floor;
}

int scan_selected(ScanItem* out)
{
    if (g_index < 0 || g_index >= g_n) return 0;
    if (out) *out = g_items[g_index];
    return 1;
}

// "one floor up", "two floors down", or nothing at all on the same storey.
static void floor_offset_text(int dz, char* out, size_t out_sz)
{
    static const char* small[] = { "", "one", "two", "three", "four", "five" };
    if (!dz) { out[0] = 0; return; }
    int n = dz > 0 ? dz : -dz;
    const char* count = n <= 5 ? small[n] : NULL;
    if (count)
        _snprintf_s(out, out_sz, _TRUNCATE, "%s floor%s %s", count,
                    n == 1 ? "" : "s", dz > 0 ? "up" : "down");
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%d floors %s", n, dz > 0 ? "up" : "down");
}

int scan_storey_diff(float feet, float floor)
{
    float d = (feet - floor) / 192.0f;
    return (int)(d >= 0.0f ? d + 0.5f : d - 0.5f);
}

void scan_unit_floor_text(const char* label, int dz, char* out, size_t out_sz)
{
    char storey[48];
    floor_offset_text(dz, storey, sizeof storey);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s.", label, storey[0] ? ", " : "", storey);
}

void scan_describe(const ScanItem* item, int from_tx, int from_ty, int from_tz,
                   char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    if (!item) return;

    char where[64];
    tile_offset_text(item->tx - from_tx, item->ty - from_ty, where, sizeof where);

    char storey[48];
    floor_offset_text(item->tz - from_tz, storey, sizeof storey);

    _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s%s%s%s%s.", item->name,
                item->detail[0] ? item->detail : "", item->detail[0] ? ", " : "",
                where, storey[0] ? ", " : "", storey);
}

void scan_category_text(ScanCategory c, int floor, int count,
                        char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    if (floor == SCAN_ALL_FLOORS)
        _snprintf_s(out, out_sz, _TRUNCATE, "%s, %d found.",
                    scan_category_name(c), count);
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%s, floor %d, %d found.",
                    scan_category_name(c), floor + 1, count);
}

void scan_floor_text(int floor, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    if (floor == SCAN_ALL_FLOORS)
        _snprintf_s(out, out_sz, _TRUNCATE, "All floors.");
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "Floor %d.", floor + 1);
}

void scan_mesh_words(const char* mesh, const char* fallback, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    char base[SCAN_NAME];
    size_t n = 0;
    for (; mesh && mesh[n] && mesh[n] != '_' && n + 1 < sizeof base; n++) base[n] = mesh[n];
    base[n] = 0;
    while (n && base[n - 1] >= '0' && base[n - 1] <= '9') base[--n] = 0;
    if (n >= 2 && base[n - 1] >= 'A' && base[n - 1] <= 'Z' &&
        base[n - 2] >= 'a' && base[n - 2] <= 'z')
        base[--n] = 0;
    if (!n) {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s", fallback ? fallback : "");
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < n && used + 2 < out_sz; i++) {
        char c = base[i];
        int cap = c >= 'A' && c <= 'Z';
        if (i && cap && base[i - 1] >= 'a' && base[i - 1] <= 'z') out[used++] = ' ';
        out[used++] = (i && cap) ? (char)(c | 0x20) : c;
    }
    out[used] = 0;
}

void scan_empty_text(ScanCategory c, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    // "No enemies." reads better than "Enemies, none", and it is the answer to
    // the key that was actually pressed -- a cycle, not a category change.
    const char* name = scan_category_name(c);
    if (c == SCAN_ALL)
        _snprintf_s(out, out_sz, _TRUNCATE, "Nothing found.");
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "No %c%s.", name[0] | 0x20, name + 1);
}
