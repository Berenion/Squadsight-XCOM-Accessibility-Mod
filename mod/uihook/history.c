// What has been announced. See history.h.

#include "history.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

// A ring of the last HISTORY_MAX. Entry `seq` lives at seq % HISTORY_MAX;
// g_next is the seq the next one gets, so g_next - count .. g_next - 1 are kept.
static char g_text[HISTORY_MAX][HISTORY_TEXT];
static unsigned g_next;
static int g_count;

static int g_open;
static unsigned g_at;       // the seq under the cursor

static void h_reset(void)
{
    g_next = 0;
    g_count = 0;
    g_open = 0;
}

static void h_add(const char* text)
{
    if (!text || !*text) return;
    strncpy_s(g_text[g_next % HISTORY_MAX], HISTORY_TEXT, text, _TRUNCATE);
    g_next++;
    if (g_count < HISTORY_MAX) g_count++;
    // The cursor's entry may just have been overwritten by the ring; keep it
    // on the oldest one still there.
    if (g_open && g_next - g_at > (unsigned)g_count) g_at = g_next - (unsigned)g_count;
}

static void h_extend(const char* prefix, const char* text)
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
        h_add(both);
    } else {
        h_add(text);
    }
}

static int h_count(void) { return g_count; }

static const char* entry(unsigned seq) { return g_text[seq % HISTORY_MAX]; }

static int h_open(char* out, size_t out_sz)
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

static void h_close(void) { g_open = 0; }

static int h_is_open(void) { return g_open; }

static void h_step(int dir, char* out, size_t out_sz)
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

static void h_current(char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s", g_count ? entry(g_at) : "");
}

// ---------------------------------------------------------------- locked --
//
// Written from the game thread (every announcement) and read from the review
// thread (Insert, which has to work in the base and the shell, where no
// per-frame game hook runs), so every entry point takes one lock. A
// critical section is recursive, which history_extend relies on.
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK lock_init(PINIT_ONCE o, PVOID p, PVOID* c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}
static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, lock_init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}
static void unlock(void) { LeaveCriticalSection(&g_lock); }

void history_reset(void) { lock(); h_reset(); unlock(); }
void history_add(const char* text) { lock(); h_add(text); unlock(); }
void history_extend(const char* prefix, const char* text) { lock(); h_extend(prefix, text); unlock(); }
int  history_count(void) { lock(); int r = h_count(); unlock(); return r; }
int  history_open(char* out, size_t out_sz) { lock(); int r = h_open(out, out_sz); unlock(); return r; }
void history_close(void) { lock(); h_close(); unlock(); }
int  history_is_open(void) { lock(); int r = h_is_open(); unlock(); return r; }
void history_step(int dir, char* out, size_t out_sz) { lock(); h_step(dir, out, out_sz); unlock(); }
void history_current(char* out, size_t out_sz) { lock(); h_current(out, out_sz); unlock(); }
