// Speech output for the XCOM accessibility hook.
//
// Prefers Tolk (which routes to whatever screen reader the player already
// runs -- NVDA, JAWS) and falls back to SAPI, which ships with Windows so the
// prototype speaks without the player installing anything.
//
// Speaking never happens on the caller's thread.  The hook runs on the game's
// UI thread, and both Tolk and SAPI can block; a stall there would show up as
// a frame hitch.  Instead callers push onto a small ring buffer and a worker
// drains it.  When the queue overflows the *oldest* entry is dropped, because
// stale UI text is worth less than what just changed on screen.

#define COBJMACROS
#include <objbase.h>
#include <sapi.h>
#include "speech.h"
#include <stdio.h>
#include <string.h>

#define QUEUE_SIZE 32
#define MAX_UTTER  512

typedef int(__cdecl* TolkLoadFn)(void);
typedef int(__cdecl* TolkOutputFn)(const wchar_t*, int);
typedef int(__cdecl* TolkSilenceFn)(void);

static HMODULE       g_tolk;
static TolkOutputFn  g_tolk_output;
static TolkSilenceFn g_tolk_silence;

// NVDA's controller client is driven directly when Tolk is unavailable.
// Tolk is only a thin multiplexer over exactly these entry points, and it
// ships x64-first, so depending on it would add a dependency without adding
// capability for an NVDA user.  Exports are undecorated __stdcall.
typedef unsigned long(__stdcall* NvdaTestFn)(void);
typedef unsigned long(__stdcall* NvdaSpeakFn)(const wchar_t*);
typedef unsigned long(__stdcall* NvdaCancelFn)(void);

static HMODULE      g_nvda;
static NvdaTestFn   g_nvda_test;
static NvdaSpeakFn  g_nvda_speak;
static NvdaCancelFn g_nvda_cancel;

static ISpVoice*     g_voice;

static wchar_t       g_queue[QUEUE_SIZE][MAX_UTTER];
static int           g_head, g_tail;
static CRITICAL_SECTION g_qlock;
static HANDLE        g_wake;
static HANDLE        g_thread;
static volatile LONG g_stop;
static volatile LONG g_dropped;

static void speak_now(const wchar_t* text, int interrupt)
{
    if (g_tolk_output) {
        if (interrupt && g_tolk_silence) g_tolk_silence();
        g_tolk_output(text, interrupt);
        return;
    }
    if (g_nvda_speak) {
        // NVDA can be shut down mid-session; testIfRunning keeps us from
        // speaking into a dead pipe on every single UI update.
        if (g_nvda_test && g_nvda_test() != 0) return;
        if (interrupt && g_nvda_cancel) g_nvda_cancel();
        g_nvda_speak(text);
        return;
    }
    if (g_voice) {
        DWORD flags = SPF_ASYNC | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
        g_voice->lpVtbl->Speak(g_voice, text, flags, NULL);
    }
}

static DWORD WINAPI worker(LPVOID param)
{
    (void)param;
    // COM must be initialised on the thread that creates and uses the voice.
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int owns_com = SUCCEEDED(hr);

    if (!g_tolk_output && !g_nvda_speak) {
        hr = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice,
                              (void**)&g_voice);
        if (FAILED(hr)) g_voice = NULL;
    }

    while (!g_stop) {
        WaitForSingleObject(g_wake, 250);
        for (;;) {
            wchar_t line[MAX_UTTER];
            EnterCriticalSection(&g_qlock);
            if (g_head == g_tail) { LeaveCriticalSection(&g_qlock); break; }
            wcscpy_s(line, MAX_UTTER, g_queue[g_tail]);
            g_tail = (g_tail + 1) % QUEUE_SIZE;
            LeaveCriticalSection(&g_qlock);
            speak_now(line, 0);
        }
    }

    if (g_voice) { g_voice->lpVtbl->Release(g_voice); g_voice = NULL; }
    if (owns_com) CoUninitialize();
    return 0;
}

// Tolk and its screen-reader client DLLs must match the *host process*, and
// XCOM is 32-bit.  A 64-bit Tolk.dll fails LoadLibrary with
// ERROR_BAD_EXE_FORMAT, which is worth saying out loud rather than reporting
// as "not found" -- the person who needs this message may not be able to see
// the file they just copied.
static HMODULE try_load(const char* path, const char* what,
                        char* why, size_t why_sz)
{
    HMODULE h = LoadLibraryA(path);
    if (h) return h;

    DWORD err = GetLastError();
    if (err == ERROR_BAD_EXE_FORMAT) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "%s at %s is 64-bit; XCOM is a 32-bit process and needs "
                    "the x86 build", what, path);
    } else if (err != ERROR_MOD_NOT_FOUND && err != ERROR_FILE_NOT_FOUND) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "%s at %s failed to load (error %lu)", what, path, err);
    }
    return NULL;
}

// Looks beside this DLL, then one directory up.  A build/ subdirectory is not
// where anyone would naturally drop a screen-reader DLL.
static HMODULE load_from_nearby(const char* dll_dir, const char* name,
                                char* detail, size_t detail_sz)
{
    char path[MAX_PATH];
    char more[256];

    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%s", dll_dir, name);
    HMODULE h = try_load(path, name, detail, detail_sz);
    if (h) return h;

    char parent[MAX_PATH];
    strcpy_s(parent, sizeof parent, dll_dir);
    size_t len = strlen(parent);
    if (len > 1) {
        parent[len - 1] = 0;                       /* drop trailing slash */
        char* slash = strrchr(parent, '\\');
        if (slash) {
            *(slash + 1) = 0;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s%s", parent, name);
            more[0] = 0;
            h = try_load(path, name, more, sizeof more);
            if (!detail[0] && more[0]) strcpy_s(detail, detail_sz, more);
            if (h) return h;
        }
    }

    more[0] = 0;
    h = try_load(name, name, more, sizeof more);
    if (!detail[0] && more[0]) strcpy_s(detail, detail_sz, more);
    return h;
}

int speech_init(const char* dll_dir, char* why, size_t why_sz)
{
    InitializeCriticalSection(&g_qlock);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);

    // Preference order: Tolk (drives whatever reader the player uses), then
    // NVDA directly, then the SAPI voice built into Windows.
    char detail[256];
    detail[0] = 0;

    g_tolk = load_from_nearby(dll_dir, "Tolk.dll", detail, sizeof detail);

    if (g_tolk) {
        TolkLoadFn load = (TolkLoadFn)GetProcAddress(g_tolk, "Tolk_Load");
        g_tolk_output = (TolkOutputFn)GetProcAddress(g_tolk, "Tolk_Output");
        g_tolk_silence = (TolkSilenceFn)GetProcAddress(g_tolk, "Tolk_Silence");
        if (load && g_tolk_output) {
            load();
            _snprintf_s(why, why_sz, _TRUNCATE, "Tolk (screen reader bridge)");
            g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            return g_thread != NULL;
        }
        g_tolk_output = NULL;
    }

    // No Tolk: talk to NVDA directly.  Same two directories, same reasoning.
    g_nvda = load_from_nearby(dll_dir, "nvdaControllerClient32.dll",
                              detail, sizeof detail);
    if (g_nvda) {
        g_nvda_test = (NvdaTestFn)GetProcAddress(g_nvda, "nvdaController_testIfRunning");
        g_nvda_speak = (NvdaSpeakFn)GetProcAddress(g_nvda, "nvdaController_speakText");
        g_nvda_cancel = (NvdaCancelFn)GetProcAddress(g_nvda, "nvdaController_cancelSpeech");

        if (g_nvda_speak && g_nvda_test) {
            unsigned long st = g_nvda_test();
            if (st == 0) {
                _snprintf_s(why, why_sz, _TRUNCATE, "NVDA (controller client)");
            } else {
                // The client loads fine whether or not NVDA is up; say which
                // it is, because "silence" has two very different causes.
                g_nvda_speak = NULL;
                _snprintf_s(why, why_sz, _TRUNCATE,
                            "SAPI -- nvdaControllerClient32 loaded but NVDA is "
                            "not running (status %lu)", st);
            }
        } else {
            g_nvda_speak = NULL;
            _snprintf_s(why, why_sz, _TRUNCATE,
                        "SAPI -- nvdaControllerClient32 lacks the expected exports");
        }
    } else if (detail[0]) {
        _snprintf_s(why, why_sz, _TRUNCATE, "SAPI -- %s", detail);
    } else {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "SAPI (no Tolk.dll or nvdaControllerClient32.dll found)");
    }

    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    return g_thread != NULL;
}

void speech_say(const char* utf8)
{
    if (!g_thread || !utf8 || !*utf8) return;

    wchar_t wide[MAX_UTTER];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_UTTER);
    if (n <= 0) return;

    EnterCriticalSection(&g_qlock);
    int next = (g_head + 1) % QUEUE_SIZE;
    if (next == g_tail) {
        // Full: drop the oldest so the newest still gets through.
        g_tail = (g_tail + 1) % QUEUE_SIZE;
        InterlockedIncrement(&g_dropped);
    }
    wcscpy_s(g_queue[g_head], MAX_UTTER, wide);
    g_head = next;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_wake);
}

long speech_dropped(void) { return g_dropped; }

void speech_shutdown(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    if (g_tolk) { FreeLibrary(g_tolk); g_tolk = NULL; }
}
