// Enemies coming into and going out of sight. See sight.h.

#include "sight.h"
#include "tile.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    SightUnit          last;        // as last seen
    int                in;          // announced as in sight
    int                ever;        // announced in sight at least once
    unsigned long long differ_at;   // since when seen and announced disagree, 0 for not
} Entry;

static Entry g_e[SIGHT_MAX];
static int   g_n;

void sight_reset(void)
{
    memset(g_e, 0, sizeof g_e);
    g_n = 0;
}

static Entry* find(void* unit, int add)
{
    for (int i = 0; i < g_n; i++)
        if (g_e[i].last.unit == unit) return &g_e[i];
    if (!add || g_n >= SIGHT_MAX) return NULL;
    Entry* e = &g_e[g_n++];
    memset(e, 0, sizeof *e);
    e->last.unit = unit;
    return e;
}

static int seen_now(const SightUnit* cur, int n, void* unit)
{
    for (int i = 0; i < n; i++)
        if (cur[i].unit == unit) return 1;
    return 0;
}

// Whether a disagreement has held long enough; starts the clock if not running.
static int settled(Entry* e, unsigned long long now)
{
    if (!e->differ_at) { e->differ_at = now ? now : 1; return 0; }
    return now - e->differ_at >= SIGHT_SETTLE_MS;
}

int sight_step(unsigned long long now, const SightUnit* cur, int n,
               SightEvent* ev, int max)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        Entry* e = find(cur[i].unit, 1);
        if (!e) continue;
        e->last = cur[i];
        if (e->in) { e->differ_at = 0; continue; }
        if (!settled(e, now) || k >= max) continue;
        ev[k].u = e->last;
        ev[k].kind = e->ever ? SIGHT_AGAIN : SIGHT_NEW;
        k++;
        e->in = 1;
        e->ever = 1;
        e->differ_at = 0;
    }
    for (int i = 0; i < g_n; i++) {
        Entry* e = &g_e[i];
        if (seen_now(cur, n, e->last.unit)) continue;
        if (!e->in) { e->differ_at = 0; continue; }
        if (!settled(e, now) || k >= max) continue;
        ev[k].u = e->last;
        ev[k].kind = SIGHT_GONE;
        k++;
        e->in = 0;
        e->differ_at = 0;
    }
    return k;
}

static void append(char* out, size_t out_sz, const char* s)
{
    size_t used = strlen(out);
    if (used + 1 < out_sz) _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s", s);
}

// "Sighted: Sectoid, 5 north, 3 east. Muton, 2 north." -- with where they
// are when every one has a position, and the names alone otherwise.
static void group(const SightEvent* ev, int n, SightKind kind, const char* head,
                  char* out, size_t out_sz)
{
    TileContact c[SIGHT_MAX];
    int m = 0, all_pos = 1;
    for (int i = 0; i < n && m < SIGHT_MAX; i++) {
        if (ev[i].kind != kind) continue;
        c[m].name = ev[i].u.label;
        c[m].dx = ev[i].u.dx;
        c[m].dy = ev[i].u.dy;
        if (!ev[i].u.has_pos || kind == SIGHT_GONE) all_pos = 0;
        m++;
    }
    if (!m) return;
    if (out[0]) append(out, out_sz, " ");
    append(out, out_sz, head);
    append(out, out_sz, " ");
    if (all_pos) {
        char list[SIGHT_TEXT];
        tile_contacts(c, m, "", list, sizeof list);
        append(out, out_sz, list);
    } else {
        for (int i = 0; i < m; i++) {
            if (i) append(out, out_sz, " ");
            append(out, out_sz, c[i].name);
            append(out, out_sz, ".");
        }
    }
}

void sight_text(const SightEvent* ev, int n, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    group(ev, n, SIGHT_NEW,   "Sighted:",        out, out_sz);
    group(ev, n, SIGHT_AGAIN, "In sight again:", out, out_sz);
    group(ev, n, SIGHT_GONE,  "Out of sight:",   out, out_sz);
}
