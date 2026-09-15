// Picks a game, starts it, and attaches the UI hook to it.
//
//   launcher.exe            ask which game
//   launcher.exe /eu        XCOM: Enemy Unknown, no question
//   launcher.exe /ew        XCOM: Enemy Within, no question
//
// The choice is a real dialog resource, so a screen reader announces it and
// tracks the focused button without any help from us.  Everything after the
// choice is silent on success -- the mod speaks its own readiness once it is
// armed, and a message box over a fullscreen game would be worse than useless.
//
// 32-bit, like the game and the DLL it injects.

#include "gamepaths.h"
#include "injector.h"
#include "resource.h"

#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EU_EXE       "XComGame.exe"
#define EW_EXE       "XComEW.exe"
#define DEFAULT_DLL  "xcom_uihook.dll"
#define LOG_NAME     "xcom_uihook.log"

// What the publisher's own launcher passes, and it is not cosmetic: started
// without -FROMLAUNCHER the game boots all the way to the main menu and then
// closes itself about sixteen seconds later, which the engine log records only
// as "Closing by request".  Every working session in this install's log
// history was started with it.
#define DEFAULT_ARGS "-FROMLAUNCHER -LANGUAGE=INT"

// How long to wait for the process to appear, then for it to put a window up.
// Both are generous: a cold start off a mechanical disk is slow, and waiting
// costs nothing, because the player is listening to the game boot either way.
#define PROCESS_TIMEOUT_MS   120000
#define RELAUNCH_TIMEOUT_MS   30000
#define WINDOW_TIMEOUT_MS     90000
#define BANNER_TIMEOUT_MS     10000
#define DEFAULT_SETTLE_MS      1500

typedef struct {
    GamePaths game;
    char dll[MAX_PATH];
    char args[512];
    unsigned settle_ms;
} Config;

static void say_error(const char* fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    MessageBoxA(NULL, text, "XCOM Accessibility Launcher",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// ------------------------------------------------------------------ config --

static void exe_directory(char* out, size_t out_sz)
{
    GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    char* slash = strrchr(out, '\\');
    if (slash) *slash = 0;
}

// launcher.ini is optional and is not shipped: discovery is expected to work,
// and the file exists for installs Steam cannot account for.
static BOOL load_config(Config* cfg)
{
    char here[MAX_PATH], ini[MAX_PATH];
    exe_directory(here, sizeof here);
    sprintf_s(ini, sizeof ini, "%s\\launcher.ini", here);

    char root[MAX_PATH] = { 0 };
    GetPrivateProfileStringA("launcher", "GameRoot", "", root, MAX_PATH, ini);

    char dll[MAX_PATH] = { 0 };
    GetPrivateProfileStringA("launcher", "Dll", "", dll, MAX_PATH, ini);
    if (dll[0])
        strcpy_s(cfg->dll, sizeof cfg->dll, dll);
    else
        sprintf_s(cfg->dll, sizeof cfg->dll, "%s\\%s", here, DEFAULT_DLL);

    GetPrivateProfileStringA("launcher", "Args", DEFAULT_ARGS,
                             cfg->args, sizeof cfg->args, ini);

    cfg->settle_ms = (unsigned)GetPrivateProfileIntA("launcher", "InjectDelayMs",
                                                     DEFAULT_SETTLE_MS, ini);

    return gamepaths_find(root, &cfg->game);
}

// -------------------------------------------------------------- the dialog --

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const GamePaths* game = &((const Config*)lparam)->game;

        EnableWindow(GetDlgItem(dlg, IDC_EU), game->eu_exe[0] != 0);
        EnableWindow(GetDlgItem(dlg, IDC_EW), game->ew_exe[0] != 0);

        // A disabled button is skipped by Tab and so goes unheard; the status
        // line is where a missing build gets said out loud instead.
        char status[MAX_PATH + 64];
        if (!game->eu_exe[0])
            strcpy_s(status, sizeof status, "Enemy Unknown is not installed.");
        else if (!game->ew_exe[0])
            strcpy_s(status, sizeof status, "Enemy Within is not installed.");
        else
            sprintf_s(status, sizeof status, "Found in %s", game->root);
        SetDlgItemTextA(dlg, IDC_STATUS, status);

        int first = game->eu_exe[0] ? IDC_EU : IDC_EW;
        SendMessageA(dlg, DM_SETDEFID, (WPARAM)first, 0);
        SetFocus(GetDlgItem(dlg, first));
        return FALSE;                    // focus was set here, not by the default
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_EU:
        case IDC_EW:
        case IDCANCEL:
            EndDialog(dlg, LOWORD(wparam));
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// -------------------------------------------------------- launch + inject --

// The banner the mod writes as its last act of arming.  Reading it back is the
// only way from out here to tell a working hook from a loaded-but-dead one.
static BOOL read_banner(const char* log, char* out, size_t out_sz, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        char* text = gamepaths_slurp(log);
        if (text) {
            const char* fatal = strstr(text, "FATAL");
            const char* armed = strstr(text, "hooks armed");
            const char* hit = fatal ? fatal : armed;
            if (hit) {
                const char* begin = hit;            // back up to the line start
                while (begin > text && begin[-1] != '\n') --begin;
                const char* end = strchr(hit, '\n');
                if (!end) end = hit + strlen(hit);
                size_t len = (size_t)(end - begin);
                if (len >= out_sz) len = out_sz - 1;
                memcpy(out, begin, len);
                out[len] = 0;
                free(text);
                return fatal == NULL;
            }
            free(text);
        }
        if (GetTickCount() - start >= timeout_ms) {
            // Which of the two it is matters: a missing log means the DLL
            // never ran, while a log without a banner means it ran and gave
            // up part way.  Reporting both as "no log" sent the last search
            // in the wrong direction entirely.
            if (GetFileAttributesA(log) != INVALID_FILE_ATTRIBUTES)
                strcpy_s(out, out_sz, "The mod wrote a log but never reported "
                                      "arming, so it stopped part way.");
            else
                strcpy_s(out, out_sz, "The mod wrote no log, so it may not have loaded.");
            return FALSE;
        }
        Sleep(250);
    }
}

// TRUE once the process has a window, or once the wait runs out with it still
// alive -- injecting late beats not injecting.  FALSE means it exited first,
// which is what a handover to another process looks like from out here.
static BOOL wait_for_window(DWORD pid, DWORD timeout_ms)
{
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    DWORD start = GetTickCount();
    BOOL alive = TRUE;

    while (!injector_has_window(pid)) {
        if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) { alive = FALSE; break; }
        if (GetTickCount() - start >= timeout_ms) break;
        Sleep(250);
    }
    if (proc) CloseHandle(proc);
    return alive;
}

// Steam's own launch sets these, and the game's Steamworks layer reads them.
// Without them the executable hands the launch straight back to Steam, which
// starts the publisher's launcher instead of the build that was asked for --
// so the process we started is gone and a different one takes its place.
// Setting them here is what makes starting the exe behave like a Steam launch.
static void adopt_steam_environment(void)
{
    SetEnvironmentVariableA("SteamAppId", XCOM_APPID);
    SetEnvironmentVariableA("SteamGameId", XCOM_APPID);
}

static BOOL launch_and_inject(const Config* cfg, const char* exe_path, const char* exe_name)
{
    char work_dir[MAX_PATH];
    strcpy_s(work_dir, sizeof work_dir, exe_path);
    char* slash = strrchr(work_dir, '\\');
    if (slash) *slash = 0;

    char log[MAX_PATH];
    sprintf_s(log, sizeof log, "%s\\%s", work_dir, LOG_NAME);

    // Attaching to a game that is already up is the same job minus the start,
    // and it keeps the launcher useful after an accidental plain launch.
    DWORD pid = injector_find_pid(exe_name);
    BOOL ready = pid && wait_for_window(pid, WINDOW_TIMEOUT_MS);

    if (!pid) {
        // The old log, removed while nothing holds it, so that a stale banner
        // cannot be mistaken for this run's.
        DeleteFileA(log);

        adopt_steam_environment();

        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi = { 0 };
        char command[MAX_PATH + sizeof cfg->args + 8];
        sprintf_s(command, sizeof command, "\"%s\" %s", exe_path, cfg->args);
        if (!CreateProcessA(exe_path, command, NULL, NULL, FALSE, 0, NULL,
                            work_dir, &si, &pi)) {
            say_error("Could not start the game.\n\n%s\n\nWindows reported error %lu.",
                      exe_path, GetLastError());
            return FALSE;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // Found by name rather than by the process just started, and searched
        // for again if that one dies: should Steam still route the launch
        // through itself, the game we want is a later process with the same
        // name, and following it there beats reporting a failure.
        for (int attempt = 0; attempt < 2 && !ready; ++attempt) {
            pid = injector_wait_for_pid(exe_name, attempt == 0 ? PROCESS_TIMEOUT_MS
                                                              : RELAUNCH_TIMEOUT_MS);
            if (!pid) break;
            ready = wait_for_window(pid, WINDOW_TIMEOUT_MS);
        }
    }

    if (!pid || !ready) {
        say_error("%s started but exited again before the mod could attach.\n\n"
                  "If the Steam client is not running, start it and try again.",
                  exe_name);
        return FALSE;
    }

    // The engine has its names and its native table long before it has a
    // window, so this pause is not about waiting for those.  It is about not
    // racing the first frame -- and it is the one number worth tuning, because
    // injecting after a screen is built leaves the mod with nothing to say
    // about that screen until something redraws it.
    Sleep(cfg->settle_ms);

    char detail[512];
    if (!injector_inject(pid, cfg->dll, detail, sizeof detail)) {
        say_error("The mod could not be attached to %s.\n\n%s", exe_name, detail);
        return FALSE;
    }

    char banner[512];
    if (!read_banner(log, banner, sizeof banner, BANNER_TIMEOUT_MS)) {
        say_error("The mod attached to %s but did not arm.\n\n%s\n\n"
                  "The full log is at %s.", exe_name, banner, log);
        return FALSE;
    }
    return TRUE;                 // the mod speaks for itself from here
}

// --------------------------------------------------------------------- main --

static int choice_from_command_line(const char* args)
{
    if (StrStrIA(args, "/ew") || StrStrIA(args, "-ew")) return IDC_EW;
    if (StrStrIA(args, "/eu") || StrStrIA(args, "-eu")) return IDC_EU;
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)prev; (void)show;

    Config cfg;
    if (!load_config(&cfg)) {
        say_error("No XCOM: Enemy Unknown installation was found.\n\n"
                  "The launcher asks Steam where the game is. If Steam cannot say, "
                  "put the install folder in launcher.ini beside this program:\n\n"
                  "[launcher]\nGameRoot=D:\\path\\to\\XCom-Enemy-Unknown");
        return 1;
    }

    int choice = choice_from_command_line(args);
    if (!choice)
        choice = (int)DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_LAUNCHER), NULL,
                                      dialog_proc, (LPARAM)&cfg);

    if (choice == IDC_EU && cfg.game.eu_exe[0])
        return launch_and_inject(&cfg, cfg.game.eu_exe, EU_EXE) ? 0 : 1;
    if (choice == IDC_EW && cfg.game.ew_exe[0])
        return launch_and_inject(&cfg, cfg.game.ew_exe, EW_EXE) ? 0 : 1;

    if (choice == IDC_EU || choice == IDC_EW) {
        say_error("%s is not installed under\n%s.",
                  choice == IDC_EU ? "XCOM: Enemy Unknown" : "XCOM: Enemy Within",
                  cfg.game.root);
        return 1;
    }
    return 0;                    // cancelled
}
