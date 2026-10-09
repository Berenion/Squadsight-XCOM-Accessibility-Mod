// The download's progress window.  See progress.h.

#include "progress.h"
#include "resource.h"

static HWND g_window;

static INT_PTR CALLBACK progress_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    (void)dlg; (void)msg; (void)wparam; (void)lparam;
    return FALSE;
}

// The caption carries the text too: with no control to focus, the caption is
// what a screen reader says when the window comes up.
void progress_say(void* ctx, const char* text)
{
    (void)ctx;
    HWND w = g_window;
    if (!w) return;
    SetDlgItemTextA(w, IDC_PROGRESS_TEXT, text);
    SetWindowTextA(w, text);
}

BOOL progress_run(HWND owner, const char* text, LPTHREAD_START_ROUTINE work, void* ctx)
{
    g_window = CreateDialogParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_PROGRESS),
                                  owner, progress_proc, 0);
    progress_say(NULL, text);
    if (g_window) SetForegroundWindow(g_window);
    if (owner) EnableWindow(owner, FALSE);

    HANDLE thread = CreateThread(NULL, 0, work, ctx, 0, NULL);
    if (thread) {
        while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT) ==
               WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                if (!g_window || !IsDialogMessageA(g_window, &m)) {
                    TranslateMessage(&m);
                    DispatchMessageA(&m);
                }
            }
        }
        CloseHandle(thread);
    }

    if (owner) EnableWindow(owner, TRUE);
    HWND w = g_window;
    g_window = NULL;
    if (w) DestroyWindow(w);
    return thread != NULL;
}
