// The ability bar, kept from its update stream. See abar.h.

#include "abar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[64];      // SetAntennaText
    char icon[48];      // SetIconLabel, if the name never came
    char cooldown[16];  // SetCooldown: "T-2", "" for none
    char charge[16];    // SetCharge: "x1", "" for none
    char hotkey[16];    // SetHotkeyLabel
    int  available;     // SetAvailable
} Slot;

static Slot g_slots[ABAR_SLOTS];
static int  g_count;

static void set(char* dst, size_t sz, const char* text)
{
    strncpy_s(dst, sz, text ? text : "", _TRUNCATE);
}

void abar_reset(void)
{
    memset(g_slots, 0, sizeof g_slots);
    g_count = 0;
    abar_menu_close();      // a new bar is a new mission; nothing is open
}

void abar_set_count(int n)
{
    if (n < 0) n = 0;
    if (n > ABAR_SLOTS) n = ABAR_SLOTS;
    g_count = n;
}

// One function's argument, applied to a slot. Names the bar does not speak
// (colour, overwatch help) are consumed and dropped.
static void apply(Slot* s, const char* fn, const AbarValue* v)
{
    const char* str = v->type == ABAR_STRING && v->s ? v->s : "";
    if (strcmp(fn, "SetAntennaText") == 0)      set(s->name, sizeof s->name, str);
    else if (strcmp(fn, "SetIconLabel") == 0)   set(s->icon, sizeof s->icon, str);
    else if (strcmp(fn, "SetCooldown") == 0)    set(s->cooldown, sizeof s->cooldown, str);
    else if (strcmp(fn, "SetCharge") == 0)      set(s->charge, sizeof s->charge, str);
    else if (strcmp(fn, "SetHotkeyLabel") == 0) set(s->hotkey, sizeof s->hotkey, str);
    else if (strcmp(fn, "SetAvailable") == 0 && v->type == ABAR_BOOL)
        s->available = v->b != 0;
}

int abar_feed(const AbarValue* v, int n)
{
    if (!v || n <= 0) return 0;

    // Parsed into a copy first: a stream that goes wrong halfway must not
    // leave half its changes behind.
    static Slot next[ABAR_SLOTS];
    memcpy(next, g_slots, sizeof next);
    int touched = 0;
    int i = 0;
    while (i < n) {
        // A slot's header: null, then its index.
        if (i + 1 >= n || v[i].type != ABAR_NULL || v[i + 1].type != ABAR_NUMBER)
            return -1;
        int idx = (int)v[i + 1].n;
        if (idx < 0 || idx >= ABAR_SLOTS) return -1;
        Slot* s = &next[idx];
        touched++;
        i += 2;
        // Its pairs, until the next header or the end.
        while (i < n && v[i].type == ABAR_STRING) {
            if (i + 1 >= n) return -1;
            apply(s, v[i].s ? v[i].s : "", &v[i + 1]);
            i += 2;
        }
    }
    memcpy(g_slots, next, sizeof g_slots);
    return touched;
}

// "HUNKER DOWN" -> "Hunker Down". The bar sends names in capitals
// (Caps(kAbility.strName)), and a screen reader may spell a short
// all-capitals word out as letters.
static void title_case(char* s)
{
    int start = 1;
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z' && !start) *s = (char)(*s | 0x20);
        start = *s == ' ' || *s == '-';
    }
}

// "T-2" -> 2; "x1" -> 1. The prefix is the game's (m_strCooldownPrefix,
// m_strChargePrefix); only the number is wanted.
static int trailing_number(const char* s)
{
    const char* p = s + strlen(s);
    while (p > s && p[-1] >= '0' && p[-1] <= '9') p--;
    return *p ? atoi(p) : -1;
}

// "2 Headshot, cooldown 2 turns" -- one slot's key, name and status, with no
// full stop. Returns 0 for a slot with no name to give.
static int slot_phrase(int i, char* piece, size_t piece_sz)
{
    piece[0] = 0;
    if (i < 0 || i >= g_count) return 0;
    const Slot* s = &g_slots[i];
    char name[64];
    set(name, sizeof name, s->name[0] ? s->name : s->icon);
    if (!name[0]) return 0;
    title_case(name);

    int w = s->hotkey[0]
        ? _snprintf_s(piece, piece_sz, _TRUNCATE, "%s %s", s->hotkey, name)
        : _snprintf_s(piece, piece_sz, _TRUNCATE, "%s", name);
    if (w < 0) w = (int)strlen(piece);

    // A cooldown is why an ability is unavailable, and says more than
    // "unavailable" does; so it stands in for it.
    int cd = s->cooldown[0] ? trailing_number(s->cooldown) : -1;
    if (cd > 0)
        _snprintf_s(piece + w, piece_sz - w, _TRUNCATE, ", cooldown %d turn%s",
                    cd, cd == 1 ? "" : "s");
    else if (!s->available)
        _snprintf_s(piece + w, piece_sz - w, _TRUNCATE, ", unavailable");
    w = (int)strlen(piece);
    int ch = s->charge[0] ? trailing_number(s->charge) : -1;
    if (ch > 0)
        _snprintf_s(piece + w, piece_sz - w, _TRUNCATE, ", %d charge%s",
                    ch, ch == 1 ? "" : "s");
    return 1;
}

int abar_describe(char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < g_count; i++) {
        char piece[160];
        if (!slot_phrase(i, piece, sizeof piece)) continue;
        int k = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s.",
                            used ? " " : "", piece);
        if (k < 0) break;
        used += (size_t)k;
    }
    return used > 0;
}

// ---- the menu --------------------------------------------------------------

static int g_open;
static int g_at;

int abar_count(void)        { return g_count; }
int abar_menu_is_open(void) { return g_open; }

int abar_menu_index(void)
{
    if (!g_open || g_count <= 0) return -1;
    // The bar can shrink under an open menu -- an ability used up, or the
    // soldier switched -- so the position is kept inside it.
    if (g_at >= g_count) g_at = g_count - 1;
    if (g_at < 0) g_at = 0;
    return g_at;
}

void abar_menu_open(void)  { g_open = 1; g_at = 0; }
void abar_menu_close(void) { g_open = 0; }

int abar_menu_step(int dir)
{
    if (!g_open || g_count <= 0) return 0;
    abar_menu_index();
    g_at += dir >= 0 ? 1 : -1;
    if (g_at >= g_count) g_at = 0;
    else if (g_at < 0) g_at = g_count - 1;
    return 1;
}

int abar_entry(int index, const char* help, char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    char piece[160];
    if (!slot_phrase(index, piece, sizeof piece)) return 0;
    if (help && help[0])
        _snprintf_s(out, out_sz, _TRUNCATE, "%s. %s", piece, help);
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%s.", piece);
    return 1;
}
