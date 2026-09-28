// The log beside the game exe: see log.h.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "log.h"

// As long as the longest captured string (main.c's MAX_STR) and its prefix.
#define LOG_MAX_LINE (4096 + 256)

// Guards the buffer and the collapsing below.
static CRITICAL_SECTION g_lock;

// Consecutive duplicates are collapsed: the UI re-sends the same string on
// every refresh, which would otherwise bury the interesting transitions.
static char g_last[LOG_MAX_LINE];
static long g_repeat;
static ULONGLONG g_repeat_since;

// The log. It is the one part of this mod its user cannot read, so it has to
// survive a crash and stay readable while the game runs -- and it is written
// from the game's own UI thread, a line per call.
//
// It used to be flushed per line, which met both needs and cost a WriteFile
// on the game thread for every line: the 2026-09-25 mission log had 37,275
// call lines, 16,243 of them UIUnitFlag.SetPosition. (OutputDebugStringA
// beside it was worse still -- every call took the machine-wide DBWinMutex --
// and went long ago.)
//
// So a line is only copied into memory here, and log_writer puts it in the
// file every LOG_FLUSH_MS from a thread of its own. The two needs are kept
// another way:
//   - Readable while running: the file is shared for reading, and at most
//     LOG_FLUSH_MS behind.
//   - A crash: what WriteFile has handed the system survives the process, so
//     only the buffer is at risk, and log_on_crash writes it out on the first
//     sign of a fatal exception, before any handler runs. A clean exit writes
//     it from DllMain.
#define LOG_BUF      (256 * 1024)
#define LOG_FLUSH_MS 200

static HANDLE           g_log = INVALID_HANDLE_VALUE;
static char             g_log_buf[2][LOG_BUF];
static size_t           g_log_n;            // bytes waiting in g_log_buf[g_log_cur]
static int              g_log_cur;
static long             g_log_dropped;      // lines that found the buffer full
static CRITICAL_SECTION g_log_write;        // one WriteFile at a time, in order
static HANDLE           g_log_wake;

// Under g_lock. A line that does not fit is counted rather than written from
// here: writing needs g_log_write, which log_flush takes before g_lock.
static void emit(const char* line)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    size_t len = strlen(line);
    if (g_log_n + len > LOG_BUF) {
        g_log_dropped++;
        SetEvent(g_log_wake);
        return;
    }
    memcpy(g_log_buf[g_log_cur] + g_log_n, line, len);
    g_log_n += len;
    if (g_log_n > LOG_BUF / 2) SetEvent(g_log_wake);
}

static void log_write(const char* data, size_t n)
{
    DWORD wrote;
    if (n) WriteFile(g_log, data, (DWORD)n, &wrote, NULL);
}

// Hands the filled buffer over and writes it outside g_lock, so the game
// thread waits only for the swap. Lock order: g_log_write, then g_lock.
static void log_flush(void)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_log_write);
    EnterCriticalSection(&g_lock);
    const char* data = g_log_buf[g_log_cur];
    size_t n = g_log_n;
    long dropped = g_log_dropped;
    g_log_cur ^= 1;
    g_log_n = 0;
    g_log_dropped = 0;
    LeaveCriticalSection(&g_lock);
    log_write(data, n);
    if (dropped) {
        char note[96];
        int w = _snprintf_s(note, sizeof note, _TRUNCATE,
                            "log: %ld lines lost -- the buffer was full\n", dropped);
        if (w > 0) log_write(note, (size_t)w);
    }
    LeaveCriticalSection(&g_log_write);
}

static DWORD WINAPI log_writer(LPVOID param)
{
    (void)param;
    for (;;) {
        WaitForSingleObject(g_log_wake, LOG_FLUSH_MS);
        log_flush();
    }
}

// The exceptions that end a process when nobody handles them. Vectored, so
// it runs first -- before the game's own crash handler, and before the
// __try blocks of this DLL, which catch access violations of their own and
// cost one early write each; they are rare. Locks are only tried: the thread
// that faulted may hold one, and a crash must not become a hang. Waits up to
// ~50 ms for a write already under way, so the file stays in order.
static LONG CALLBACK log_on_crash(EXCEPTION_POINTERS* info)
{
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case 0xC0000409:    // STATUS_STACK_BUFFER_OVERRUN, /GS and fast-fail
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_log == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    int got = 0;
    for (int i = 0; i < 50 && !(got = TryEnterCriticalSection(&g_log_write)); i++)
        Sleep(1);
    if (!got) return EXCEPTION_CONTINUE_SEARCH;
    if (TryEnterCriticalSection(&g_lock)) {
        log_write(g_log_buf[g_log_cur], g_log_n);
        g_log_n = 0;
        LeaveCriticalSection(&g_lock);
    }
    LeaveCriticalSection(&g_log_write);
    return EXCEPTION_CONTINUE_SEARCH;
}

// At process exit the writer thread is already gone, and whatever lock it
// held is held for good -- so nothing is taken, and what is waiting is
// written as it is.
void log_on_exit(void)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    log_write(g_log_buf[g_log_cur], g_log_n);
    g_log_n = 0;
}

// Opens the log beside the game exe, shared for reading so it can be tailed.
void log_open(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_log_write);
    g_log_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    // No writer, no log: every line would sit in the buffer until it filled.
    if (!g_log_wake) { CloseHandle(h); return; }
    g_log = h;
    if (!CreateThread(NULL, 0, log_writer, NULL, 0, NULL)) {
        g_log = INVALID_HANDLE_VALUE;
        CloseHandle(h);
        return;
    }
    AddVectoredExceptionHandler(1, log_on_crash);
}

void logf_(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[LOG_MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);

    // Compare past the "[N] " counter, otherwise every line is unique and the
    // collapsing never fires.
    const char* key = line;
    if (key[0] == '[') {
        const char* b = strchr(key, ']');
        if (b) key = b + 1;
    }

    EnterCriticalSection(&g_lock);
    if (strcmp(key, g_last) == 0) {
        g_repeat++;
        // Flush periodically. Holding the count until a *different* line
        // arrives makes a live tail look frozen -- which is exactly how this
        // looked when the main menu was repeating one call.
        ULONGLONG now = GetTickCount64();
        if (now - g_repeat_since > 1000) {
            char note[64];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "      ... repeated %ld times\n", g_repeat);
            emit(note);
            g_repeat = 0;
            g_repeat_since = now;
        }
    } else {
        if (g_repeat) {
            char note[64];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "      ... repeated %ld more times\n", g_repeat);
            emit(note);
            g_repeat = 0;
        }
        g_repeat_since = GetTickCount64();
        emit(line);
        strcpy_s(g_last, sizeof g_last, key);
    }
    LeaveCriticalSection(&g_lock);
}
