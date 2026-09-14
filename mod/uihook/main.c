// XCOM EU/EW accessibility prototype: capture UI text on its way to Scaleform.
//
// 85% of the game's on-screen text reaches Flash through
// GFxMoviePlayer.ActionScriptVoid.  That native is unusual: the call site
// passes only a movieclip path,
//
//     simulated function AS_SetText(string DisplayText)
//     {
//         manager.ActionScriptVoid(string(GetMCPath()) $ ".SetText");
//     }
//
// and the native then reads DisplayText off the *caller's* stack frame.
// That is exactly what makes interception cheap: the text is already
// evaluated and sitting in Stack.Locals when our hook runs, so we can read
// it without touching Stack.Code.  Re-stepping the bytecode would risk
// double-evaluating argument expressions; this cannot.
//
// This prototype answers one question: does real, readable UI text come out?
// It therefore reports the raw strings plus the caller's UFunction pointer.
// Resolving that pointer to a name needs GNames and a UProperty walk, which
// is deliberately left to the next iteration.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "ue3.h"
#include "natives.h"
#include "../../tools/vendor/MinHook/include/MinHook.h"

#define MAX_NATIVES   8192
#define LOCALS_WINDOW 0x120   // bytes of the caller frame to sweep for FStrings
#define MAX_STR       4096

static FILE*            g_log;
static CRITICAL_SECTION g_lock;
static ExecFn           g_orig_asvoid;
static volatile LONG    g_calls;

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    EnterCriticalSection(&g_lock);
    vfprintf(g_log, fmt, ap);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
    va_end(ap);
}

// A pointer is only dereferenced after VirtualQuery says the whole range is
// committed and readable -- the game calls this hook on its own UI thread and
// a stray read would take the process down with it.
static int readable(const void* p, size_t n)
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

// Does this look like a live UE3 FString?  UE3 stores the terminating NUL in
// Num, so Num is the character count + 1.
// The caller validates the whole locals window, so the triple is safe to read;
// only Data needs a check, and only once the cheap field tests have passed.
static int looks_like_fstring(const FString* s, char* out, size_t out_sz)
{
    if (s->Num < 2 || s->Num > MAX_STR) return 0;
    if (s->Max < s->Num) return 0;
    if (!readable(s->Data, (size_t)s->Num * sizeof(wchar_t))) return 0;
    if (s->Data[s->Num - 1] != 0) return 0;

    int printable = 0;
    for (int i = 0; i < s->Num - 1; i++) {
        wchar_t c = s->Data[i];
        if (c == 0) return 0;                       // embedded NUL: not a string
        if (c >= 32 || c == '\n' || c == '\t' || c == '\r') printable++;
    }
    // Reject control-character soup; real UI text is overwhelmingly printable.
    if (printable < s->Num - 1) return 0;

    int n = WideCharToMultiByte(CP_UTF8, 0, s->Data, s->Num - 1,
                                out, (int)out_sz - 1, NULL, NULL);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

static void capture(LONG n, void* stack)
{

    if (readable(stack, 0x20)) {
        void* node   = *(void**)((uint8_t*)stack + FFRAME_NODE);
        void* object = *(void**)((uint8_t*)stack + FFRAME_OBJECT);
        uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);

        char buf[MAX_STR];
        int found = 0;
        if (readable(locals, LOCALS_WINDOW)) {
            // Parameters are laid out at 4-byte-aligned property offsets, so
            // sweep the frame looking for anything shaped like an FString.
            for (size_t off = 0; off + sizeof(FString) <= LOCALS_WINDOW; off += 4) {
                const FString* s = (const FString*)(locals + off);
                if (looks_like_fstring(s, buf, sizeof buf)) {
                    logf_("[%ld] node=%p obj=%p +%03zu \"%s\"\n",
                          n, node, object, off, buf);
                    found++;
                    off += sizeof(FString) - 4;   // don't re-match inside it
                }
            }
        }
        if (!found)
            logf_("[%ld] node=%p obj=%p (no string params)\n", n, node, object);
    } else {
        logf_("[%ld] unreadable frame %p\n", n, stack);
    }
}

static void __fastcall hook_asvoid(void* self, void* edx, void* stack, void* result)
{
    LONG n = InterlockedIncrement(&g_calls);
    // A stray read must never take the game down with it.
    __try {
        capture(n, stack);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("[%ld] capture faulted (0x%08lx) -- frame skipped\n",
              n, GetExceptionCode());
    }
    // The game's call is issued whether or not capture succeeded.
    g_orig_asvoid(self, edx, stack, result);
}

static DWORD WINAPI init(LPVOID param)
{
    (void)param;
    InitializeCriticalSection(&g_lock);

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(path, MAX_PATH, "xcom_uihook.log");
    fopen_s(&g_log, path, "w");

    HMODULE mod = GetModuleHandleA(NULL);
    logf_("xcom_uihook: module base %p\n", (void*)mod);

    NativeEntry* tbl = (NativeEntry*)malloc(sizeof(NativeEntry) * MAX_NATIVES);
    if (!tbl) { logf_("out of memory\n"); return 1; }

    int n = natives_scan(mod, tbl, MAX_NATIVES);
    logf_("xcom_uihook: %d natives discovered\n", n);
    if (n == 0) {
        logf_("FATAL: native table not found -- unexpected build?\n");
        return 1;
    }

    void* target = natives_find(tbl, n, "UGFxMoviePlayerexecActionScriptVoid");
    logf_("UGFxMoviePlayer::execActionScriptVoid = %p (rva %p)\n",
          target, (void*)((uint8_t*)target - (uint8_t*)mod));
    // Reported for the next iteration; not hooked yet.
    void* inv = natives_find(tbl, n, "AUI_FxsPanelexecInvoke");
    logf_("AUI_FxsPanel::execInvoke            = %p (rva %p)\n",
          inv, inv ? (void*)((uint8_t*)inv - (uint8_t*)mod) : NULL);

    if (!target) { logf_("FATAL: target native not found\n"); return 1; }

    if (MH_Initialize() != MH_OK) { logf_("FATAL: MH_Initialize\n"); return 1; }
    MH_STATUS st = MH_CreateHook(target, (LPVOID)hook_asvoid, (LPVOID*)&g_orig_asvoid);
    if (st != MH_OK) { logf_("FATAL: MH_CreateHook %d\n", st); return 1; }
    st = MH_EnableHook(target);
    if (st != MH_OK) { logf_("FATAL: MH_EnableHook %d\n", st); return 1; }

    logf_("xcom_uihook: hook armed, waiting for UI traffic\n---\n");
    free(tbl);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Loader lock: do the real work on our own thread.
        CreateThread(NULL, 0, init, NULL, 0, NULL);
    }
    return TRUE;
}
