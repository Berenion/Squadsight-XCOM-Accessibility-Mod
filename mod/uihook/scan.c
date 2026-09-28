// The scanner: the part that does not touch the game. See scan.h.

#include "scan.h"
#include "tile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
static char g_sel_detail[96];
static int  g_sel_pos;          // where it stood in the list, for a tie

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
    case SCAN_MELD:       return "Meld";
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
    if (g_floor != SCAN_ALL_FLOORS && (item->unplaced || item->tz != g_floor)) return 0;
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
    if (a->unplaced != b->unplaced) return a->unplaced;
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
    // move, so the name decides and the tile breaks the tie. Items with no
    // tile all stand on 0, 0, 0, and two Meld canisters, one collected and
    // one not yet seen, were the same item by name and tile: Up from the
    // second landed back on it (2026-09-25). So the detail counts too, and
    // among items that are the same in every way the one nearest the old
    // place in the list is taken.
    g_index = -1;
    if (g_have_sel) {
        int by_name = -1, by_tile = -1;
        for (int i = 0; i < g_n; i++) {
            if (strcmp(g_items[i].name, g_sel_name) != 0) continue;
            if (by_name < 0) by_name = i;
            if (g_items[i].tx != g_sel_tile[0] || g_items[i].ty != g_sel_tile[1] ||
                g_items[i].tz != g_sel_tile[2])
                continue;
            if (by_tile < 0) by_tile = i;
            if (strcmp(g_items[i].detail, g_sel_detail) != 0) continue;
            if (g_index < 0 || abs(i - g_sel_pos) < abs(g_index - g_sel_pos)) g_index = i;
        }
        if (g_index < 0) g_index = by_tile;     // its detail changed
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
    strncpy_s(g_sel_detail, sizeof g_sel_detail, g_items[index].detail, _TRUNCATE);
    g_sel_pos = index;
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
    if (item->unplaced) {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s.", item->name,
                    item->detail[0] ? ", " : "", item->detail);
        return;
    }

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

// Words in a mesh name that name no object: the maps' prefixes, texture and
// material suffixes, and the artists' words for a state or a variant. Taken
// from the names in the map packages (URB_CommercialAlley and four others,
// read 2026-09-28): WoodenCrateStackBShortA, WoodenCrateStackB_DestroACharred,
// GenUtilityBox_A, INT_PROP_Mop_and_Bucket, CrateDestBurnGeneric96x96A,
// BoxStack_DIFF. Compared whole and without case.
static const char* const k_mesh_noise[] = {
    "int", "ext", "prop", "props", "gen", "bldg", "urb", "cty", "rur", "sm", "lod",
    "mesh", "mat", "dif", "diff", "nrm", "norm", "spc", "spec",
    "dest", "destro", "destruction", "destroyed", "damaged", "damage", "dmg",
    "fractured", "charred", "burn", "broken", "plain", "alone", "multi", "rev",
    "var", "variant", "generic",
};

static int mesh_noise(const char* w, size_t n)
{
    for (size_t i = 0; i < sizeof k_mesh_noise / sizeof k_mesh_noise[0]; i++)
        if (strlen(k_mesh_noise[i]) == n && _strnicmp(w, k_mesh_noise[i], n) == 0) return 1;
    return 0;
}

static int is_up(char c)    { return c >= 'A' && c <= 'Z'; }
static int is_low(char c)   { return c >= 'a' && c <= 'z'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

// Words the artists ran together, as they are said: BarrelMetalonPalletD.
static const char* const k_mesh_split[][2] = {
    { "metalon", "metal on" },
};

// Size and state words, said first so a count reads right: the 12:17 log of
// 2026-09-28 said "2 Wooden crate stack talls" (WoodenCrateStackBTallB);
// now "2 Tall wooden crate stacks".
static const char* const k_mesh_front[] = {
    "short", "tall", "small", "medium", "large", "big", "long", "wide", "narrow", "tipped",
};

static int word_is(const char* w, size_t n, const char* const* list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (strlen(list[i]) == n && _strnicmp(w, list[i], n) == 0) return 1;
    return 0;
}

#define MESH_WORDS    16
#define MESH_WORD_LEN 32

void scan_mesh_words(const char* mesh, const char* fallback, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    char words[MESH_WORDS][MESH_WORD_LEN];
    int  nwords = 0;
    const char* s = mesh ? mesh : "";
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len && nwords < MESH_WORDS) {
        // One word: up to an underscore, a small letter followed by a
        // capital ("Crate|Stack"), a capital that starts a word after
        // another capital ("B|Short"), or a change between letters and
        // digits ("Medium|02", "96|x|96").
        if (s[i] == '_' || s[i] == '-' || s[i] == ' ') { i++; continue; }
        size_t j = i + 1;
        while (j < len && s[j] != '_' && s[j] != '-' && s[j] != ' ') {
            char a = s[j - 1], c = s[j];
            if (is_low(a) && is_up(c)) break;
            if (is_up(a) && is_up(c) && j + 1 < len && is_low(s[j + 1])) break;
            if (is_digit(a) != is_digit(c)) break;
            j++;
        }
        const char* w = s + i;
        size_t n = j - i;
        i = j;
        // A lone letter is a variant, a number or a size names nothing, and
        // the noise words above are the art's bookkeeping.
        int digits = 0;
        for (size_t k = 0; k < n; k++) if (is_digit(w[k])) digits = 1;
        if (n < 2 || n >= MESH_WORD_LEN || digits || mesh_noise(w, n)) continue;
        const char* split = NULL;
        for (size_t k = 0; k < sizeof k_mesh_split / sizeof k_mesh_split[0]; k++)
            if (strlen(k_mesh_split[k][0]) == n && _strnicmp(w, k_mesh_split[k][0], n) == 0)
                split = k_mesh_split[k][1];
        if (split) {
            // Its words, one by one ("metal on").
            const char* p = split;
            while (*p && nwords < MESH_WORDS) {
                size_t m = strcspn(p, " ");
                _snprintf_s(words[nwords++], MESH_WORD_LEN, _TRUNCATE, "%.*s", (int)m, p);
                p += m;
                while (*p == ' ') p++;
            }
            continue;
        }
        _snprintf_s(words[nwords++], MESH_WORD_LEN, _TRUNCATE, "%.*s", (int)n, w);
    }

    // Size and state words first, then the rest, each in the order found.
    size_t used = 0;
    int first = 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < nwords; k++) {
            const char* w = words[k];
            size_t n = strlen(w);
            int front = word_is(w, n, k_mesh_front, sizeof k_mesh_front / sizeof k_mesh_front[0]);
            if (front != (pass == 0)) continue;
            if (used + n + 2 >= out_sz) break;
            if (!first) out[used++] = ' ';
            // "Wooden crate stack": capital at the front only. A word in
            // capitals throughout ("TV") keeps them.
            int all_caps = 1;
            for (size_t c = 0; c < n; c++) if (is_low(w[c])) all_caps = 0;
            for (size_t c = 0; c < n; c++) {
                char ch = w[c];
                if (!all_caps) ch = (first && c == 0) ? (char)(is_low(ch) ? ch - 32 : ch)
                                                      : (char)(is_up(ch) ? ch + 32 : ch);
                out[used++] = ch;
            }
            out[used] = 0;
            first = 0;
        }
    }
    if (!used) _snprintf_s(out, out_sz, _TRUNCATE, "%s", fallback ? fallback : "");
}

int scan_mesh_is_dressing(const char* mesh)
{
    static const char* const dressing[] = { "decal", "graffiti", "poster", "sticker" };
    char low[SCAN_NAME];
    size_t n = 0;
    for (; mesh && mesh[n] && n + 1 < sizeof low; n++)
        low[n] = is_up(mesh[n]) ? (char)(mesh[n] + 32) : mesh[n];
    low[n] = 0;
    for (size_t i = 0; i < sizeof dressing / sizeof dressing[0]; i++)
        if (strstr(low, dressing[i])) return 1;
    return 0;
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
