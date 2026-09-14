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

    if (!g_tolk_output) {
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

int speech_init(const char* dll_dir, char* why, size_t why_sz)
{
    InitializeCriticalSection(&g_qlock);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);

    // Tolk is optional: if the player drops Tolk.dll next to this DLL we use
    // their real screen reader instead of the generic SAPI voice.
    char tolk_path[MAX_PATH];
    _snprintf_s(tolk_path, sizeof tolk_path, _TRUNCATE, "%sTolk.dll", dll_dir);
    g_tolk = LoadLibraryA(tolk_path);
    if (!g_tolk) g_tolk = LoadLibraryA("Tolk.dll");

    if (g_tolk) {
        TolkLoadFn load = (TolkLoadFn)GetProcAddress(g_tolk, "Tolk_Load");
        g_tolk_output = (TolkOutputFn)GetProcAddress(g_tolk, "Tolk_Output");
        g_tolk_silence = (TolkSilenceFn)GetProcAddress(g_tolk, "Tolk_Silence");
        if (load && g_tolk_output) {
            load();
            _snprintf_s(why, why_sz, _TRUNCATE, "Tolk (screen reader bridge)");
        } else {
            g_tolk_output = NULL;
            _snprintf_s(why, why_sz, _TRUNCATE, "Tolk.dll present but unusable; using SAPI");
        }
    } else {
        _snprintf_s(why, why_sz, _TRUNCATE, "SAPI (no Tolk.dll found)");
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
