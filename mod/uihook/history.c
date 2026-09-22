// What has been announced. See history.h.

#include "history.h"
#include <stdio.h>
#include <string.h>

// A ring of the last HISTORY_MAX. Entry `seq` lives at seq % HISTORY_MAX;
// g_next is the seq the next one gets, so g_next - count .. g_next - 1 are kept.
static char g_text[HISTORY_MAX][HISTORY_TEXT];
static unsigned g_next;
static int g_count;

static int g_open;
static unsigned g_at;       // the seq under the cursor

void history_reset(void)
{
    g_next = 0;
    g_count = 0;
    g_open = 0;
}

void history_add(const char* text)
{
    if (!text || !*text) return;
    strncpy_s(g_text[g_next % HISTORY_MAX], HISTORY_TEXT, text, _TRUNCATE);
    g_next++;
    if (g_count < HISTORY_MAX) g_count++;
    // The cursor's entry may just have been overwritten by the ring; keep it
    // on the oldest one still there.
    if (g_open && g_next - g_at > (unsigned)g_count) g_at = g_next - (unsigned)g_count;
}

void history_extend(const char* prefix, const char* text)
{
    if (!text || !*text) return;
    if (g_count > 0 && prefix && *prefix) {
        char* last = g_text[(g_next - 1) % HISTORY_MAX];
        size_t n = strlen(prefix);
        if (strncmp(last, prefix, n) == 0) {
            size_t used = strlen(last);
            _snprintf_s(last + used, HISTORY_TEXT - used, _TRUNCATE, " %s", text);
            return;
        }
    }
    if (prefix && *prefix) {
        char both[HISTORY_TEXT];
        _snprintf_s(both, sizeof both, _TRUNCATE, "%s, %s", prefix, text);
        history_add(both);
    } else {
        history_add(text);
    }
}

int history_count(void) { return g_count; }

static const char* entry(unsigned seq) { return g_text[seq % HISTORY_MAX]; }

int history_open(char* out, size_t out_sz)
{
    if (g_count == 0) {
        _snprintf_s(out, out_sz, _TRUNCATE, "No announcements yet.");
        return 0;
    }
    g_open = 1;
    g_at = g_next - 1;
    _snprintf_s(out, out_sz, _TRUNCATE, "Announcements, %d. %s", g_count, entry(g_at));
    return 1;
}

void history_close(void) { g_open = 0; }

int history_is_open(void) { return g_open; }

void history_step(int dir, char* out, size_t out_sz)
{
    unsigned oldest = g_next - (unsigned)g_count;
    if (dir < 0) {
        if (g_at > oldest) {
            g_at--;
            _snprintf_s(out, out_sz, _TRUNCATE, "%s", entry(g_at));
        } else {
            _snprintf_s(out, out_sz, _TRUNCATE, "Oldest. %s", entry(g_at));
        }
    } else {
        if (g_at + 1 < g_next) {
            g_at++;
            _snprintf_s(out, out_sz, _TRUNCATE, "%s", entry(g_at));
        } else {
            _snprintf_s(out, out_sz, _TRUNCATE, "Newest. %s", entry(g_at));
        }
    }
}

void history_current(char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s", g_count ? entry(g_at) : "");
}
