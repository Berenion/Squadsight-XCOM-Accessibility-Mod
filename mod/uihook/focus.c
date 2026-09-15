// Per-object label lists, so a selection index can be resolved to a label.
//
// Only a handful of screens are live at once, so this is a small fixed array
// reclaimed least-recently-used rather than a hash table.  It is written from
// the game's UI thread and read from the same, but the speech worker never
// touches it, so a single lock is enough.

#include "focus.h"
#include <string.h>
#include <stdio.h>

#define FOCUS_SLOTS 12

typedef struct {
    void*     obj;
    ULONGLONG touched;
    int       count;
    char      labels[FOCUS_MAX_LABELS][FOCUS_MAX_LABEL];
    char      values[FOCUS_MAX_LABELS][FOCUS_MAX_LABEL];
} Slot;

static Slot g_slots[FOCUS_SLOTS];
static CRITICAL_SECTION g_lock;
static LONG g_ready;

static void ensure_init(void)
{
    if (InterlockedCompareExchange(&g_ready, 1, 0) == 0)
        InitializeCriticalSection(&g_lock);
    // A racing thread may arrive before InitializeCriticalSection returns;
    // in practice the first call happens long before any second thread, and
    // the hook is only ever driven from the game's UI thread.
}

// Clears both parts of every entry, not just the ones below `count`.  A
// reclaimed slot would otherwise let the previous occupant's value survive
// under a new object's label -- "Shadows: 1920 x 1080".
static void slot_clear(Slot* s)
{
    for (int i = 0; i < FOCUS_MAX_LABELS; i++) {
        s->labels[i][0] = 0;
        s->values[i][0] = 0;
    }
    s->count = 0;
}

static Slot* slot_for(void* obj, int create)
{
    Slot* oldest = &g_slots[0];
    for (int i = 0; i < FOCUS_SLOTS; i++) {
        if (g_slots[i].obj == obj) return &g_slots[i];
        if (g_slots[i].touched < oldest->touched) oldest = &g_slots[i];
    }
    if (!create) return NULL;

    oldest->obj = obj;
    slot_clear(oldest);
    return oldest;
}

void focus_begin(void* obj)
{
    if (!obj) return;
    ensure_init();
    EnterCriticalSection(&g_lock);
    Slot* s = slot_for(obj, 1);
    slot_clear(s);
    s->touched = GetTickCount64();
    LeaveCriticalSection(&g_lock);
}

void focus_add(void* obj, const char* text)
{
    if (!obj || !text || !*text) return;
    ensure_init();
    EnterCriticalSection(&g_lock);
    Slot* s = slot_for(obj, 1);
    if (s->count < FOCUS_MAX_LABELS) {
        strncpy_s(s->labels[s->count], FOCUS_MAX_LABEL, text, _TRUNCATE);
        s->values[s->count][0] = 0;
        s->count++;
    }
    s->touched = GetTickCount64();
    LeaveCriticalSection(&g_lock);
}

void focus_set(void* obj, int index, const char* text)
{
    focus_set_part(obj, index, FOCUS_PART_LABEL, text);
}

void focus_set_part(void* obj, int index, int part, const char* text)
{
    if (!obj || !text || !*text) return;
    if (index < 0 || index >= FOCUS_MAX_LABELS) return;
    ensure_init();
    EnterCriticalSection(&g_lock);
    Slot* s = slot_for(obj, 1);
    // Slots skipped over stay empty rather than shifting anything: the index
    // is the screen's own, so it must map straight through.
    for (int i = s->count; i < index; i++) { s->labels[i][0] = 0; s->values[i][0] = 0; }
    char* dst = (part == FOCUS_PART_VALUE) ? s->values[index] : s->labels[index];
    strncpy_s(dst, FOCUS_MAX_LABEL, text, _TRUNCATE);
    if (index >= s->count) s->count = index + 1;
    s->touched = GetTickCount64();
    LeaveCriticalSection(&g_lock);
}

int focus_label_at(void* obj, int index, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!obj || index < 0) return 0;
    ensure_init();

    int ok = 0;
    EnterCriticalSection(&g_lock);
    Slot* s = slot_for(obj, 0);
    if (s && index < s->count && (s->labels[index][0] || s->values[index][0])) {
        // "Mode:" + "Fullscreen" -> "Mode: Fullscreen".  A control with only
        // one of the two (a button, a slider whose value never arrives as
        // text) still reads correctly, so no special case is needed.
        const char* lab = s->labels[index];
        const char* val = s->values[index];
        if (lab[0] && val[0])
            _snprintf_s(out, out_sz, _TRUNCATE, "%s %s", lab, val);
        else
            strncpy_s(out, out_sz, lab[0] ? lab : val, _TRUNCATE);
        s->touched = GetTickCount64();
        ok = 1;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

int focus_count(void* obj)
{
    if (!obj) return 0;
    ensure_init();
    EnterCriticalSection(&g_lock);
    Slot* s = slot_for(obj, 0);
    int n = s ? s->count : 0;
    LeaveCriticalSection(&g_lock);
    return n;
}
