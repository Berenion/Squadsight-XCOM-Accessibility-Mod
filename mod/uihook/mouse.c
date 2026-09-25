// The mouse, kept still. See mouse.h.

#include "mouse.h"
#include "settings.h"
#include <windows.h>
#include <stdio.h>

static HHOOK          g_hook;
static volatile LONG  g_swallowed;

static int game_in_front(void)
{
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

int mouse_blocking(void)
{
    return g_hook && settings_get(SET_MOUSE) && game_in_front();
}

unsigned mouse_swallowed(void)
{
    return (unsigned)g_swallowed;
}

// Kept short: Windows drops a low-level hook that takes longer than
// LowLevelHooksTimeout to answer, silently.
static LRESULT CALLBACK on_mouse(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
        if (!(m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) &&
            settings_get(SET_MOUSE) && game_in_front()) {
            InterlockedIncrement(&g_swallowed);
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI pump(LPVOID ready)
{
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, on_mouse, GetModuleHandleW(NULL), 0);
    SetEvent((HANDLE)ready);
    if (!g_hook) return 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return 0;
}

int mouse_start(char* why, size_t why_sz)
{
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready) {
        _snprintf_s(why, why_sz, _TRUNCATE, "no event (0x%08lx) -- the mouse is not held",
                    GetLastError());
        return 0;
    }
    HANDLE t = CreateThread(NULL, 0, pump, ready, 0, NULL);
    if (!t) {
        _snprintf_s(why, why_sz, _TRUNCATE, "no thread (0x%08lx) -- the mouse is not held",
                    GetLastError());
        CloseHandle(ready);
        return 0;
    }
    WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    CloseHandle(t);
    if (!g_hook) {
        _snprintf_s(why, why_sz, _TRUNCATE, "hook refused -- the mouse is not held");
        return 0;
    }
    _snprintf_s(why, why_sz, _TRUNCATE, "held while the game is in front (%s now)",
                settings_get(SET_MOUSE) ? "on" : "off");
    return 1;
}
