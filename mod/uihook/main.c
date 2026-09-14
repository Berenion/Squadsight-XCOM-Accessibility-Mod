// XCOM EU/EW accessibility hook: capture UI text on its way to Scaleform and
// speak it.
//
// All of the game's on-screen text reaches Flash through a small number of
// natives that share a useful property: the text is a parameter of the
// *calling* UnrealScript function, already evaluated and sitting in
// Stack.Locals by the time the native runs.
//
//   GFxMoviePlayer.ActionScriptVoid  -- 85% of text.  The call site passes
//       only a movieclip path and the native reads the rest off the caller:
//
//           simulated function AS_SetText(string DisplayText)
//           { manager.ActionScriptVoid(string(GetMCPath()) $ ".SetText"); }
//
//   UI_FxsPanel.Invoke               -- the remaining 15%.  Its arguments are
//       explicit ASValues, but the strings they are built from are still
//       locals of the caller:
//
//           function SetShotChance(string Label, string Desc)
//           { ...; myValue.S = Label; ...; Invoke("SetShotChance", myArray); }
//
// So one capture routine serves every target: read Stack.Locals, never
// Stack.Code.  Re-stepping the bytecode would re-evaluate argument
// expressions and double any side effects they carry; reading locals cannot.
//
// Parameters are located by walking the caller's UProperty chain -- the same
// walk the engine's own ActionScript marshaller performs -- so each value is
// read at its declared offset and reported with its declared name.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <share.h>
#include <stdint.h>
#include <string.h>

#include "ue3.h"
#include "natives.h"
#include "names.h"
#include "speech.h"
#include "../../tools/vendor/MinHook/include/MinHook.h"

#define MAX_NATIVES 8192
#define MAX_STR     4096
#define MAX_PARAMS  32

static FILE*            g_log;
static CRITICAL_SECTION g_lock;
static volatile LONG    g_calls;
static int              g_speak = 1;

// Consecutive duplicates are collapsed: the UI re-sends the same string on
// every refresh, which would otherwise bury the interesting transitions --
// and, more importantly, would make the speech unusable.
static char g_last[MAX_STR + 256];
static long g_repeat;
static char g_last_spoken[MAX_STR];

static void emit(const char* line)
{
    if (!g_log) return;
    fputs(line, g_log);
    fflush(g_log);
    OutputDebugStringA(line);
}

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    char line[MAX_STR + 256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_lock);
    if (strcmp(line, g_last) == 0) {
        g_repeat++;
    } else {
        if (g_repeat) {
            char note[64];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "      ... repeated %ld more times\n", g_repeat);
            emit(note);
            g_repeat = 0;
        }
        emit(line);
        strcpy_s(g_last, sizeof g_last, line);
    }
    LeaveCriticalSection(&g_lock);
}

// A pointer is only dereferenced after VirtualQuery says the whole range is
// committed and readable -- this runs on the game's own UI thread and a stray
// read would take the process down with it.  Non-static: names.c uses it too.
int readable(const void* p, size_t n)
{
    if (!p) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* cur = (const uint8_t*)p;
    const uint8_t* end = cur + n;
    while (cur < end) {
        if (!VirtualQuery(cur, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return 0;
        if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE ||
              prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
            return 0;
        cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

// Reads an FString at a known-good address.  Num counts the terminating NUL.
static int read_fstring(const FString* s, char* out, size_t out_sz)
{
    if (!readable(s, sizeof *s)) return 0;
    if (s->Num < 2 || s->Num > MAX_STR) return 0;
    if (s->Max < s->Num) return 0;
    if (!readable(s->Data, (size_t)s->Num * sizeof(wchar_t))) return 0;
    if (s->Data[s->Num - 1] != 0) return 0;

    int n = WideCharToMultiByte(CP_UTF8, 0, s->Data, s->Num - 1,
                                out, (int)out_sz - 1, NULL, NULL);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

// Flash markup leaks into a lot of these strings; a screen reader should not
// read tags aloud.
static void strip_markup(char* s)
{
    char* w = s;
    int depth = 0;
    for (char* r = s; *r; r++) {
        if (*r == '<') { depth++; continue; }
        if (*r == '>') { if (depth) depth--; continue; }
        if (!depth) *w++ = *r;
    }
    *w = 0;

    // Collapse runs of whitespace left behind by the removed tags.
    char* out = s;
    int space = 0;
    for (char* r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!space && out != s) *out++ = ' ';
            space = 1;
        } else {
            *out++ = (char)c;
            space = 0;
        }
    }
    while (out > s && out[-1] == ' ') out--;
    *out = 0;
}

static void capture(const char* tag, LONG n, void* stack)
{
    if (!readable(stack, 0x20)) {
        logf_("[%ld] %s unreadable frame %p\n", n, tag, stack);
        return;
    }

    void* node      = *(void**)((uint8_t*)stack + FFRAME_NODE);
    void* object    = *(void**)((uint8_t*)stack + FFRAME_OBJECT);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);

    char fn_name[128] = "?", obj_name[128] = "?";
    object_name(node, fn_name, sizeof fn_name);
    object_name(object, obj_name, sizeof obj_name);

    // Walk the caller's declared parameters, exactly as the engine's own
    // ActionScript marshaller does: Children -> Next, keeping CPF_Parm
    // entries that are not the return value, and reading each at its
    // declared offset into the frame.
    char utterance[MAX_STR];
    utterance[0] = 0;
    int spoken_len = 0, shown = 0;

    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);

    for (int guard = 0; prop && guard < MAX_PARAMS; guard++) {
        if (!readable(prop, 0x68)) break;

        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);

        if ((flags & (CPF_PARM | CPF_RETURNPARM)) == CPF_PARM && locals) {
            char val[MAX_STR];
            const FString* s = (const FString*)(locals + off);
            if (read_fstring(s, val, sizeof val)) {
                strip_markup(val);
                if (*val) {
                    char pname[128] = "?";
                    object_name(prop, pname, sizeof pname);
                    logf_("[%ld] %s %s.%s  %s=\"%s\"\n",
                          n, tag, obj_name, fn_name, pname, val);
                    shown++;
                    // Several parameters can make up one announcement (a
                    // label and its description), so join rather than speak
                    // each separately.
                    int len = (int)strlen(val);
                    if (spoken_len + len + 2 < MAX_STR) {
                        if (spoken_len) {
                            strcat_s(utterance, MAX_STR, ". ");
                            spoken_len += 2;
                        }
                        strcat_s(utterance, MAX_STR, val);
                        spoken_len += len;
                    }
                }
            }
        }
        prop = next;
    }

    if (!shown) {
        logf_("[%ld] %s %s.%s (no string params)\n", n, tag, obj_name, fn_name);
        return;
    }

    if (g_speak && spoken_len) {
        EnterCriticalSection(&g_lock);
        int same = (strcmp(utterance, g_last_spoken) == 0);
        if (!same) strcpy_s(g_last_spoken, sizeof g_last_spoken, utterance);
        LeaveCriticalSection(&g_lock);
        if (!same) speech_say(utterance);
    }
}

// MinHook needs a distinct trampoline per target, so each native gets its own
// thunk; they all funnel into capture().
#define THUNK(id, tag)                                                        \
    static ExecFn g_orig_##id;                                                \
    static void __fastcall hook_##id(void* self, void* edx,                   \
                                     void* stack, void* result)               \
    {                                                                         \
        LONG n = InterlockedIncrement(&g_calls);                              \
        __try { capture(tag, n, stack); }                                     \
        __except (EXCEPTION_EXECUTE_HANDLER) {                                \
            logf_("[%ld] %s capture faulted (0x%08lx)\n",                     \
                  n, tag, GetExceptionCode());                                \
        }                                                                     \
        g_orig_##id(self, edx, stack, result);                                \
    }

THUNK(movie_asvoid, "ASVoid/Movie ")
THUNK(object_asvoid, "ASVoid/Object")
THUNK(panel_invoke, "Invoke/Panel ")

static int arm(const NativeEntry* tbl, int n, HMODULE mod,
               const char* name, void* thunk, void** orig)
{
    void* target = natives_find(tbl, n, name);
    if (!target) {
        logf_("  %-38s NOT FOUND\n", name);
        return 0;
    }
    logf_("  %-38s %p (rva %p)\n", name, target,
          (void*)((uint8_t*)target - (uint8_t*)mod));

    MH_STATUS st = MH_CreateHook(target, thunk, orig);
    if (st != MH_OK) { logf_("    MH_CreateHook failed: %d\n", st); return 0; }
    st = MH_EnableHook(target);
    if (st != MH_OK) { logf_("    MH_EnableHook failed: %d\n", st); return 0; }
    return 1;
}

static HINSTANCE g_self;

static DWORD WINAPI init(LPVOID param)
{
    (void)param;
    InitializeCriticalSection(&g_lock);

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(path, MAX_PATH, "xcom_uihook.log");
    // NB: fopen_s opens exclusively, which would lock the log for the whole
    // session; _SH_DENYWR keeps it readable while the game runs.
    g_log = _fsopen(path, "w", _SH_DENYWR);

    HMODULE mod = GetModuleHandleA(NULL);
    logf_("xcom_uihook: module base %p\n", (void*)mod);

    char why[256];
    if (names_init(mod, why, sizeof why))
        logf_("names: %s\n", why);
    else
        logf_("names: UNAVAILABLE (%s) -- falling back to raw pointers\n", why);

    char dll_dir[MAX_PATH];
    GetModuleFileNameA(g_self, dll_dir, MAX_PATH);
    slash = strrchr(dll_dir, '\\');
    if (slash) *(slash + 1) = 0;
    speech_init(dll_dir, why, sizeof why);
    logf_("speech: %s\n", why);

    NativeEntry* tbl = (NativeEntry*)malloc(sizeof(NativeEntry) * MAX_NATIVES);
    if (!tbl) { logf_("FATAL: out of memory\n"); return 1; }

    int n = natives_scan(mod, tbl, MAX_NATIVES);
    logf_("natives: %d discovered\n", n);
    if (n == 0) {
        logf_("FATAL: native table not found -- unexpected build?\n");
        free(tbl);
        return 1;
    }

    if (MH_Initialize() != MH_OK) {
        logf_("FATAL: MH_Initialize\n");
        free(tbl);
        return 1;
    }

    int armed = 0;
    armed += arm(tbl, n, mod, "UGFxMoviePlayerexecActionScriptVoid",
                 (LPVOID)hook_movie_asvoid, (LPVOID*)&g_orig_movie_asvoid);
    armed += arm(tbl, n, mod, "UGFxObjectexecActionScriptVoid",
                 (LPVOID)hook_object_asvoid, (LPVOID*)&g_orig_object_asvoid);
    armed += arm(tbl, n, mod, "AUI_FxsPanelexecInvoke",
                 (LPVOID)hook_panel_invoke, (LPVOID*)&g_orig_panel_invoke);

    free(tbl);
    if (!armed) { logf_("FATAL: nothing armed\n"); return 1; }

    logf_("%d/3 hooks armed -- navigate the UI to produce traffic\n---\n", armed);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, init, NULL, 0, NULL);   // stay off the loader lock
    }
    return TRUE;
}
