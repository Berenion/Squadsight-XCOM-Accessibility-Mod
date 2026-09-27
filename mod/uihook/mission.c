// The mission's objectives. See mission.h.

#include "mission.h"
#include <stdio.h>
#include <string.h>

enum { OPEN, DONE, FAILED };

typedef struct {
    char id[48];
    char text[256];
    int  state;         // OPEN, DONE, FAILED
    int  hint;          // drawn with no checkbox
    int  order;         // position in the last sorted list, else arrival
} Objective;

static Objective g_now[MISSION_MAX], g_was[MISSION_MAX];
static int       g_nnow, g_nwas;
static int       g_dirty;
static int       g_arrivals;

void mission_reset(void)
{
    memset(g_now, 0, sizeof g_now);
    memset(g_was, 0, sizeof g_was);
    g_nnow = g_nwas = 0;
    g_dirty = 0;
    g_arrivals = 0;
}

static Objective* find(Objective* list, int n, const char* id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].id, id) == 0) return &list[i];
    return NULL;
}

int mission_open_mentions(const char* word)
{
    size_t wn = strlen(word);
    for (int i = 0; i < g_nnow; i++) {
        if (g_now[i].state != OPEN) continue;
        for (const char* p = g_now[i].text; *p; p++)
            if (_strnicmp(p, word, wn) == 0) return 1;
    }
    return 0;
}

void mission_clear(void)
{
    g_nnow = 0;
    g_dirty = 1;
}

void mission_add(const char* id, const char* title, const char* desc, int checkmark)
{
    if (!id || !*id) return;
    Objective* o = find(g_now, g_nnow, id);
    if (!o) {
        if (g_nnow >= MISSION_MAX) return;
        o = &g_now[g_nnow++];
        memset(o, 0, sizeof *o);
        strncpy_s(o->id, sizeof o->id, id, _TRUNCATE);
        o->order = 1000 + g_arrivals++;
    }
    // AddObjective always starts it open, as the game's own record does.
    o->state = OPEN;
    o->hint = !checkmark;
    if (title && *title && desc && *desc)
        _snprintf_s(o->text, sizeof o->text, _TRUNCATE, "%s: %s", title, desc);
    else
        _snprintf_s(o->text, sizeof o->text, _TRUNCATE, "%s", title && *title ? title : desc ? desc : "");
    g_dirty = 1;
}

static void set_state(const char* id, int state)
{
    Objective* o = find(g_now, g_nnow, id ? id : "");
    if (!o) return;
    o->state = state;
    g_dirty = 1;
}

void mission_complete(const char* id) { set_state(id, DONE); }
void mission_fail(const char* id)     { set_state(id, FAILED); }

void mission_remove(const char* id)
{
    for (int i = 0; i < g_nnow; i++)
        if (strcmp(g_now[i].id, id ? id : "") == 0) {
            memmove(&g_now[i], &g_now[i + 1], (size_t)(g_nnow - i - 1) * sizeof g_now[0]);
            g_nnow--;
            g_dirty = 1;
            return;
        }
}

void mission_order(const char* const* ids, int n)
{
    for (int i = 0; i < n; i++) {
        Objective* o = find(g_now, g_nnow, ids[i]);
        if (o) o->order = i;
    }
}

int mission_dirty(void) { return g_dirty; }

static void append(char* out, size_t out_sz, const char* s)
{
    size_t used = strlen(out);
    if (used + 1 < out_sz) _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s", s);
}

// The text ends with a full stop, once.
static void sentence(char* out, size_t out_sz, const char* text)
{
    append(out, out_sz, text);
    size_t n = strlen(text);
    if (n && text[n - 1] != '.' && text[n - 1] != '!' && text[n - 1] != '?')
        append(out, out_sz, ".");
}

// One kind of change, or one state of the list: "Head: A. B." with the head
// made plural by `heads` when there are several.
static void group(char* out, size_t out_sz, const Objective* const* items, int n,
                  const char* head, const char* heads)
{
    if (!n) return;
    if (out[0]) append(out, out_sz, " ");
    append(out, out_sz, n == 1 ? head : heads);
    append(out, out_sz, " ");
    for (int i = 0; i < n; i++) {
        if (i) append(out, out_sz, " ");
        sentence(out, out_sz, items[i]->text);
    }
}

static void sorted(Objective* const* in, int n, const Objective** out)
{
    for (int i = 0; i < n; i++) out[i] = in[i];
    for (int i = 1; i < n; i++) {
        const Objective* k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->order > k->order) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
}

// The whole list in the order drawn, each with its state after it. Grouped
// under headings it was misheard: "... back to the Skyranger. Complete:
// Locate General Van Doorn. Approach ..." was taken for the escort being
// complete and the other two not (2026-09-22). A state after each objective
// cannot attach to the wrong one.
void mission_list(char* out, size_t out_sz)
{
    out[0] = 0;
    if (!g_nnow) { append(out, out_sz, "No objectives."); return; }
    Objective* all[MISSION_MAX];
    const Objective* s[MISSION_MAX];
    for (int i = 0; i < g_nnow; i++) all[i] = &g_now[i];
    sorted(all, g_nnow, s);
    append(out, out_sz, g_nnow == 1 ? "Objective:" : "Objectives:");
    for (int i = 0; i < g_nnow; i++) {
        char text[sizeof s[i]->text];
        strncpy_s(text, sizeof text, s[i]->text, _TRUNCATE);
        size_t n = strlen(text);
        if (s[i]->state != OPEN && n && text[n - 1] == '.') text[n - 1] = 0;
        append(out, out_sz, " ");
        if (s[i]->state == DONE) {
            append(out, out_sz, text);
            append(out, out_sz, ", complete.");
        } else if (s[i]->state == FAILED) {
            append(out, out_sz, text);
            append(out, out_sz, ", failed.");
        } else {
            sentence(out, out_sz, text);
        }
    }
}

void mission_changes(char* out, size_t out_sz)
{
    out[0] = 0;
    if (!g_dirty) return;
    g_dirty = 0;

    // Replaced outright -- a new mission, a load -- when nothing of the old
    // list is still there with the same text.
    int kept = 0;
    for (int i = 0; i < g_nnow; i++) {
        const Objective* w = find(g_was, g_nwas, g_now[i].id);
        if (w && strcmp(w->text, g_now[i].text) == 0) kept++;
    }
    if (g_nnow && !kept) {
        mission_list(out, out_sz);
    } else {
        Objective* all[MISSION_MAX];
        const Objective* s[MISSION_MAX];
        for (int i = 0; i < g_nnow; i++) all[i] = &g_now[i];
        sorted(all, g_nnow, s);
        const Objective* added[MISSION_MAX]; const Objective* changed[MISSION_MAX];
        const Objective* done[MISSION_MAX];  const Objective* failed[MISSION_MAX];
        int na = 0, nc = 0, nd = 0, nf = 0;
        for (int i = 0; i < g_nnow; i++) {
            const Objective* o = s[i];
            const Objective* w = find(g_was, g_nwas, o->id);
            if (o->state == DONE && (!w || w->state != DONE)) done[nd++] = o;
            else if (o->state == FAILED && (!w || w->state != FAILED)) failed[nf++] = o;
            else if (!w) added[na++] = o;
            else if (strcmp(w->text, o->text) != 0) changed[nc++] = o;
        }
        group(out, out_sz, added, na, "New objective:", "New objectives:");
        group(out, out_sz, changed, nc, "Objective:", "Objectives:");
        group(out, out_sz, failed, nf, "Objective failed:", "Objectives failed:");
        group(out, out_sz, done, nd, "Objective complete:", "Objectives complete:");
    }
    memcpy(g_was, g_now, sizeof g_now);
    g_nwas = g_nnow;
}
