// Picks a game, starts it, and attaches the UI hook to it.  Also installs,
// updates and removes the mod (install.c, update.c).
//
//   launcher.exe            ask which game
//   launcher.exe /eu        XCOM: Enemy Unknown, no question
//   launcher.exe /ew        XCOM: Enemy Within, no question
//   launcher.exe /install   copy this folder's mod into the install folder
//   launcher.exe /uninstall remove the installed mod (Windows' Uninstall runs this)
//   launcher.exe /noupdate  skip the check for a newer release
//   launcher.exe /stage DIR copy a release's files into DIR (package.bat)
//
// On every start the launcher asks GitHub for the latest release and offers it
// when it is newer than MOD_VERSION.  An accepted update is downloaded and
// unpacked into the temp folder, and the new launcher is started there with
// `/install /after <pid> /then <the original arguments>`: it waits for this one
// to exit, installs its own files, and starts the installed launcher with the
// original arguments, so a desktop shortcut's /ew still ends in the game.
//
// The choice is a real dialog resource, so a screen reader announces it and
// tracks the focused button without any help from us.  Everything after the
// choice is silent on success -- the mod speaks its own readiness once it is
// armed, and a message box over a fullscreen game would be worse than useless.
//
// 32-bit, like the game and the DLL it injects.

#include "gamepaths.h"
#include "injector.h"
#include "install.h"
#include "resource.h"
#include "update.h"
#include "version.h"

#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EU_EXE       "XComGame.exe"
#define EW_EXE       "XComEW.exe"
#define DEFAULT_DLL  "xcom_uihook.dll"
#define LOG_NAME     "xcom_uihook.log"
#define TITLE        "XCOM Accessibility Launcher"
#define UPDATE_DIR   MOD_NAME "-update"

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

// What the dialog returns when it handed over to an update, so that WinMain
// exits instead of starting a game.
#define CHOICE_UPDATING      9001

typedef struct {
    GamePaths game;
    BOOL game_found;
    char dll[MAX_PATH];
    char args[512];
    unsigned settle_ms;
    BOOL check_updates;
} Config;

static void say_error(const char* fmt, ...)
{
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    launcher_log("error: %s\n", text);
    MessageBoxA(NULL, text, TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

static void say_info(HWND owner, const char* fmt, ...)
{
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    MessageBoxA(owner, text, TITLE, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------------- log --

static void exe_directory(char* out, size_t out_sz)
{
    GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    char* slash = strrchr(out, '\\');
    if (slash) *slash = 0;
}

// The launcher's own log, like the DLL's the only record of what happened.  It
// goes in the install folder when there is one, so that the old launcher, the
// new one in the temp folder and the freshly installed one of an update all
// write to the same file; it is appended to, and cut back once it is large.
// Opened per line, so that nothing holds it while an uninstall deletes it.
void launcher_log(const char* fmt, ...)
{
    static char path[MAX_PATH];
    if (!path[0]) {
        char dir[MAX_PATH];
        install_dir(dir, sizeof dir);
        if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) exe_directory(dir, sizeof dir);
        sprintf_s(path, sizeof path, "%s\\launcher.log", dir);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
            DeleteFileA(path);
    }
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

// ------------------------------------------------------------------ config --

// launcher.ini is optional and is not shipped: discovery is expected to work,
// and the file exists for installs Steam cannot account for -- and, with
// CheckUpdates=0, for a development build that should not offer releases.
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
    cfg->check_updates = GetPrivateProfileIntA("launcher", "CheckUpdates", 1, ini) != 0;

    cfg->game_found = gamepaths_find(root, &cfg->game);
    return cfg->game_found;
}

// -------------------------------------------------------------------- update --

typedef struct {
    Release release;
    char work[MAX_PATH];        // %TEMP%\Squadsight-update
    char launcher[MAX_PATH];    // the new launcher.exe, once unpacked
    char err[512];
    BOOL ok;
    HWND progress;
} Fetch;

static void update_work_dir(char* out, size_t out_sz)
{
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    sprintf_s(out, out_sz, "%s%s", temp, UPDATE_DIR);
}

static void progress_say(HWND progress, const char* text)
{
    if (!progress) return;
    SetDlgItemTextA(progress, IDC_PROGRESS_TEXT, text);
    SetWindowTextA(progress, text);
}

// The release zip holds one folder (Squadsight-0.9.1\...); a flat zip is taken
// too, so that a hand-made one still works.
static BOOL find_unpacked_launcher(const char* dir, char* out, size_t out_sz)
{
    sprintf_s(out, out_sz, "%s\\launcher.exe", dir);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return TRUE;
    char pattern[MAX_PATH];
    sprintf_s(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    BOOL found = FALSE;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                continue;
            sprintf_s(out, out_sz, "%s\\%s\\launcher.exe", dir, fd.cFileName);
            found = GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
        } while (!found && FindNextFileA(find, &fd));
        FindClose(find);
    }
    if (!found) out[0] = 0;
    return found;
}

static DWORD WINAPI fetch_thread(LPVOID param)
{
    Fetch* f = (Fetch*)param;
    char zip[MAX_PATH], unpacked[MAX_PATH], text[256];

    install_delete_tree(f->work);
    if (!CreateDirectoryA(f->work, NULL)) {
        sprintf_s(f->err, sizeof f->err, "Could not create %s (error %lu).", f->work,
                  GetLastError());
        return 0;
    }
    sprintf_s(zip, sizeof zip, "%s\\%s-%s.zip", f->work, MOD_NAME, f->release.tag);
    launcher_log("update: downloading %s\n", f->release.zip_url);
    if (!update_download(f->release.zip_url, zip, f->err, sizeof f->err)) return 0;

    sprintf_s(text, sizeof text, "Unpacking %s %s...", MOD_NAME, f->release.tag);
    progress_say(f->progress, text);
    sprintf_s(unpacked, sizeof unpacked, "%s\\files", f->work);
    CreateDirectoryA(unpacked, NULL);
    if (!update_unzip(zip, unpacked, f->err, sizeof f->err)) return 0;
    if (!find_unpacked_launcher(unpacked, f->launcher, sizeof f->launcher)) {
        sprintf_s(f->err, sizeof f->err, "The downloaded release has no launcher.exe in it.");
        return 0;
    }
    launcher_log("update: unpacked, the new launcher is %s\n", f->launcher);
    f->ok = TRUE;
    return 0;
}

static INT_PTR CALLBACK progress_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    (void)dlg; (void)msg; (void)wparam; (void)lparam;
    return FALSE;
}

// Downloads and unpacks on a thread while this one keeps the progress window
// alive, so that a screen reader hears it come up and Windows does not call
// the launcher hung.
static BOOL fetch_release(HWND owner, Fetch* f)
{
    char text[256];
    sprintf_s(text, sizeof text, "Downloading %s %s, please wait...", MOD_NAME, f->release.tag);
    f->progress = CreateDialogParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_PROGRESS),
                                     owner, progress_proc, 0);
    progress_say(f->progress, text);
    if (f->progress) SetForegroundWindow(f->progress);
    if (owner) EnableWindow(owner, FALSE);

    HANDLE thread = CreateThread(NULL, 0, fetch_thread, f, 0, NULL);
    if (!thread) {
        strcpy_s(f->err, sizeof f->err, "Could not start the download.");
    } else {
        while (MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT) ==
               WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                if (!f->progress || !IsDialogMessageA(f->progress, &m)) {
                    TranslateMessage(&m);
                    DispatchMessageA(&m);
                }
            }
        }
        CloseHandle(thread);
    }

    if (owner) EnableWindow(owner, TRUE);
    if (f->progress) DestroyWindow(f->progress);
    f->progress = NULL;
    return f->ok;
}

// The release's text, cut to what a message box can sensibly hold.  It is
// UTF-8 from GitHub; MessageBoxA would read it in the ANSI code page.
static void notes_for_box(const char* notes, wchar_t* out, size_t out_n)
{
    char cut[1200];
    strncpy_s(cut, sizeof cut, notes, _TRUNCATE);
    if (strlen(notes) >= sizeof cut - 1) strcat_s(cut, sizeof cut, "...");
    MultiByteToWideChar(CP_UTF8, 0, cut, -1, out, (int)out_n);
}

// Asks GitHub for a newer release and offers it.  TRUE means the update is
// under way and this launcher must exit now.  `asked` is the Check for updates
// button: only then are "nothing new" and "could not check" said aloud; on a
// start they go to the log alone, so that an offline machine starts the game
// without a word.
static BOOL offer_update(HWND owner, BOOL asked, const char* passthrough)
{
    const char* running = install_game_running();
    if (running) {
        launcher_log("update: not checked, %s is running\n", running);
        if (asked)
            say_info(owner, "%s is running. Close the game, then check for updates again.",
                     running);
        return FALSE;
    }

    Fetch* f = (Fetch*)calloc(1, sizeof *f);
    if (!f) return FALSE;
    char err[512];
    if (!update_latest(MOD_REPO, &f->release, err, sizeof err)) {
        launcher_log("update: check failed -- %s\n", err);
        if (asked) say_info(owner, "Could not check for updates.\n\n%s", err);
        free(f);
        return FALSE;
    }

    int mine[3];
    update_parse_version(MOD_VERSION, mine);
    launcher_log("update: latest release %s, this is %s\n", f->release.tag, MOD_VERSION);
    if (update_compare(f->release.version, mine) <= 0) {
        if (asked) say_info(owner, "You have the latest version, %s.", MOD_VERSION);
        free(f);
        return FALSE;
    }

    char dir[MAX_PATH];
    install_dir(dir, sizeof dir);
    char installed[32];
    BOOL present = install_present(installed, sizeof installed);

    wchar_t notes[1300], text[2400];
    notes_for_box(f->release.notes, notes, sizeof notes / sizeof notes[0]);
    swprintf_s(text, sizeof text / sizeof text[0],
               L"%hs %hs is available. You have %hs.\n\n%s%s"
               L"Download and install it now? %hs",
               MOD_NAME, f->release.tag, MOD_VERSION, notes, notes[0] ? L"\n\n" : L"",
               present ? "The launcher closes and opens again when it is done."
                       : "It is installed in your programs folder with a shortcut on the "
                         "desktop, and the launcher opens again from there.");
    if (MessageBoxW(owner, text, L"" TITLE, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND)
            != IDYES) {
        launcher_log("update: %s declined\n", f->release.tag);
        free(f);
        return FALSE;
    }

    update_work_dir(f->work, sizeof f->work);
    if (!fetch_release(owner, f)) {
        say_error("The update could not be downloaded.\n\n%s\n\nThe release page is %s",
                  f->err, f->release.page_url);
        free(f);
        return FALSE;
    }

    char command[2 * MAX_PATH + 512], work_dir[MAX_PATH];
    sprintf_s(command, sizeof command, "\"%s\" /install /after %lu /then %s",
              f->launcher, GetCurrentProcessId(), passthrough);
    strcpy_s(work_dir, sizeof work_dir, f->launcher);
    *strrchr(work_dir, '\\') = 0;
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(f->launcher, command, NULL, NULL, FALSE, 0, NULL, work_dir, &si, &pi)) {
        say_error("The new launcher could not be started (error %lu).\n\n%s",
                  GetLastError(), f->launcher);
        free(f);
        return FALSE;
    }
    launcher_log("update: started %s\n", command);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(f);
    return TRUE;
}

// ------------------------------------------------------------------ install --

static BOOL start_installed(const char* args)
{
    char dir[MAX_PATH], exe[MAX_PATH], command[MAX_PATH + 600];
    install_dir(dir, sizeof dir);
    sprintf_s(exe, sizeof exe, "%s\\launcher.exe", dir);
    sprintf_s(command, sizeof command, "\"%s\" %s", exe, args);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(exe, command, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        say_error("The installed launcher could not be started (error %lu).\n\n%s",
                  GetLastError(), exe);
        return FALSE;
    }
    launcher_log("install: started %s\n", command);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static void installed_summary(char* out, size_t out_sz)
{
    char dir[MAX_PATH];
    install_dir(dir, sizeof dir);
    sprintf_s(out, out_sz,
              "%s %s is installed in %s.\n\nThe desktop has a %s shortcut, which starts "
              "this launcher.\nTo remove the mod, use Uninstall here or Windows' list of "
              "installed apps.", MOD_NAME, MOD_VERSION, dir, MOD_NAME);
}

// `/install [/after pid] [/then args]`: the second half of an update, or an
// install from the command line.  The old launcher's process is waited for,
// because its launcher.exe is one of the files being replaced.
static int run_install(const Config* cfg, const char* args)
{
    DWORD after = 0;
    const char* a = StrStrIA(args, "/after ");
    if (a) after = strtoul(a + 7, NULL, 10);
    const char* then = StrStrIA(args, "/then");
    BOOL updating = then != NULL;
    char passthrough[600] = { 0 };
    if (then) {
        then += 5;
        while (*then == ' ') ++then;
        // /noupdate: should a release's tag ever run ahead of the version
        // inside it, the installed launcher would offer the same update again
        // forever; once per start is the most it gets to ask.
        sprintf_s(passthrough, sizeof passthrough, "%s /noupdate", then);
    }

    if (after) {
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, after);
        if (old) {
            WaitForSingleObject(old, 20000);
            CloseHandle(old);
        }
    }

    char here[MAX_PATH], err[512];
    exe_directory(here, sizeof here);
    if (!install_from(here, err, sizeof err)) {
        say_error("%s %s could not be installed.\n\n%s", MOD_NAME, MOD_VERSION, err);
        return 1;
    }
    if (updating) {
        say_info(NULL, "%s is updated to version %s.", MOD_NAME, MOD_VERSION);
        return start_installed(passthrough) ? 0 : 1;
    }
    char text[1024];
    installed_summary(text, sizeof text);
    say_info(NULL, "%s", text);
    return 0;
}

// The two questions an uninstall asks.  FALSE when the player said no.
static BOOL run_uninstall(HWND owner, const Config* cfg)
{
    char dir[MAX_PATH], err[512];
    install_dir(dir, sizeof dir);
    char text[1024];
    sprintf_s(text, sizeof text,
              "Remove %s from %s, with its desktop shortcut and its entry in Windows' list of "
              "installed apps?", MOD_NAME, dir);
    if (MessageBoxA(owner, text, TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 |
                                        MB_SETFOREGROUND) != IDYES) {
        launcher_log("uninstall: declined\n");
        return FALSE;
    }
    BOOL settings = MessageBoxA(owner,
        "Also delete your settings? That is the options and sound levels you saved, the "
        "ammunition the mod has learnt, and its log, all kept in the game's folder.\n\n"
        "Choose No to keep them for a later install.",
        TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES;

    if (!install_remove(&cfg->game, settings, err, sizeof err)) {
        say_error("%s could not be fully removed.\n\n%s", MOD_NAME, err);
        return FALSE;
    }
    launcher_log("uninstall: done\n");
    say_info(owner, "%s is uninstalled.%s", MOD_NAME,
             settings ? "" : " Your settings are kept in the game's folder.");
    return TRUE;
}

// -------------------------------------------------------------- the dialog --

static Config* g_cfg;

// The status line says where the game is and what state the mod is in, and is
// what explains a hidden Install or Uninstall button.
static void dialog_refresh(HWND dlg)
{
    const GamePaths* game = &g_cfg->game;
    char installed[32];
    BOOL present = install_present(installed, sizeof installed);
    BOOL here = install_running_installed();

    ShowWindow(GetDlgItem(dlg, IDC_INSTALL), here ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(dlg, IDC_UNINSTALL), present ? SW_SHOW : SW_HIDE);

    char where[MAX_PATH + 64], mod[160];
    if (!g_cfg->game_found)
        strcpy_s(where, sizeof where, "XCOM was not found.");
    else if (!game->eu_exe[0])
        strcpy_s(where, sizeof where, "Enemy Unknown is not installed.");
    else if (!game->ew_exe[0])
        strcpy_s(where, sizeof where, "Enemy Within is not installed.");
    else
        sprintf_s(where, sizeof where, "Found in %s.", game->root);

    if (here)
        sprintf_s(mod, sizeof mod, "%s %s, installed.", MOD_NAME, MOD_VERSION);
    else if (present)
        sprintf_s(mod, sizeof mod, "%s %s, not the installed copy (that is %s).",
                  MOD_NAME, MOD_VERSION, installed[0] ? installed : "unknown");
    else
        sprintf_s(mod, sizeof mod, "%s %s, not installed.", MOD_NAME, MOD_VERSION);

    char status[sizeof where + sizeof mod + 2];
    sprintf_s(status, sizeof status, "%s\n%s", where, mod);
    SetDlgItemTextA(dlg, IDC_STATUS, status);
}

static void dialog_focus_game(HWND dlg)
{
    const GamePaths* game = &g_cfg->game;
    int first = game->eu_exe[0] ? IDC_EU : game->ew_exe[0] ? IDC_EW : IDC_UPDATE;
    SendMessageA(dlg, DM_SETDEFID, (WPARAM)first, 0);
    SetFocus(GetDlgItem(dlg, first));
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_cfg = (Config*)lparam;
        const GamePaths* game = &g_cfg->game;
        EnableWindow(GetDlgItem(dlg, IDC_EU), game->eu_exe[0] != 0);
        EnableWindow(GetDlgItem(dlg, IDC_EW), game->ew_exe[0] != 0);
        dialog_refresh(dlg);
        dialog_focus_game(dlg);
        return FALSE;                    // focus was set here, not by the default
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_EU:
        case IDC_EW:
        case IDCANCEL:
            EndDialog(dlg, LOWORD(wparam));
            return TRUE;

        case IDC_INSTALL: {
            char here[MAX_PATH], err[512], text[1024];
            exe_directory(here, sizeof here);
            if (!install_from(here, err, sizeof err)) {
                say_error("%s %s could not be installed.\n\n%s", MOD_NAME, MOD_VERSION, err);
                return TRUE;
            }
            // From here on a game started from this dialog gets the installed
            // copy, the one updates will replace.
            char dir[MAX_PATH];
            install_dir(dir, sizeof dir);
            sprintf_s(g_cfg->dll, sizeof g_cfg->dll, "%s\\%s", dir, DEFAULT_DLL);
            installed_summary(text, sizeof text);
            say_info(dlg, "%s", text);
            dialog_refresh(dlg);
            dialog_focus_game(dlg);
            return TRUE;
        }

        case IDC_UNINSTALL:
            if (run_uninstall(dlg, g_cfg)) EndDialog(dlg, IDCANCEL);
            return TRUE;

        case IDC_UPDATE:
            if (offer_update(dlg, TRUE, "")) EndDialog(dlg, CHOICE_UPDATING);
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
    launcher_log("launch: %s with %s\n", exe_path, cfg->dll);

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
    launcher_log("launch: armed -- %s\n", banner);
    return TRUE;                 // the mod speaks for itself from here
}

// --------------------------------------------------------------------- main --

static int choice_from_command_line(const char* args)
{
    if (StrStrIA(args, "/ew") || StrStrIA(args, "-ew")) return IDC_EW;
    if (StrStrIA(args, "/eu") || StrStrIA(args, "-eu")) return IDC_EU;
    return 0;
}

// `/stage DIR`, the folder possibly quoted.
static int run_stage(const char* args)
{
    const char* p = StrStrIA(args, "/stage") + 6;
    while (*p == ' ') ++p;
    char dir[MAX_PATH] = { 0 };
    if (*p == '"') {
        ++p;
        const char* end = strchr(p, '"');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        strncpy_s(dir, sizeof dir, p, len);
    } else {
        strcpy_s(dir, sizeof dir, p);
    }
    char here[MAX_PATH], err[512];
    exe_directory(here, sizeof here);
    if (!dir[0] || !install_stage(here, dir, err, sizeof err)) {
        say_error("The release could not be staged.\n\n%s", dir[0] ? err : "No folder given.");
        return 1;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)prev; (void)show;

    {
        char self[MAX_PATH];
        GetModuleFileNameA(NULL, self, MAX_PATH);
        time_t now = time(NULL);
        struct tm local;
        localtime_s(&local, &now);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        launcher_log("---- %s launcher %s, %s, args \"%s\"\n", stamp, MOD_VERSION, self, args);
    }

    if (StrStrIA(args, "/stage")) return run_stage(args);

    Config cfg;
    memset(&cfg, 0, sizeof cfg);
    BOOL found = load_config(&cfg);

    // Installing needs no game, and removing needs it only for the settings
    // files, so both go ahead without one.
    if (StrStrIA(args, "/uninstall")) return run_uninstall(NULL, &cfg) ? 0 : 1;
    if (StrStrIA(args, "/install")) return run_install(&cfg, args);

    // What an earlier update left in the temp folder.  Not while that copy is
    // still exiting, which is why a failure here is ignored: the next start
    // gets it.
    {
        char work[MAX_PATH], here[MAX_PATH];
        update_work_dir(work, sizeof work);
        exe_directory(here, sizeof here);
        if (!StrStrIA(here, work)) install_delete_tree(work);
    }

    if (cfg.check_updates && !StrStrIA(args, "/noupdate")) {
        if (offer_update(NULL, FALSE, args)) return 0;
    } else {
        launcher_log("update: not checked (%s)\n",
                     cfg.check_updates ? "/noupdate" : "CheckUpdates=0 in launcher.ini");
    }

    int choice = choice_from_command_line(args);
    if (!found && !choice) {
        say_error("No XCOM: Enemy Unknown installation was found.\n\n"
                  "The launcher asks Steam where the game is. If Steam cannot say, "
                  "put the install folder in launcher.ini beside this program:\n\n"
                  "[launcher]\nGameRoot=D:\\path\\to\\XCom-Enemy-Unknown\n\n"
                  "The launcher opens anyway, to install, update or remove the mod.");
    }
    if (!choice)
        choice = (int)DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_LAUNCHER), NULL,
                                      dialog_proc, (LPARAM)&cfg);
    if (choice == CHOICE_UPDATING) return 0;

    if (choice == IDC_EU && cfg.game.eu_exe[0])
        return launch_and_inject(&cfg, cfg.game.eu_exe, EU_EXE) ? 0 : 1;
    if (choice == IDC_EW && cfg.game.ew_exe[0])
        return launch_and_inject(&cfg, cfg.game.ew_exe, EW_EXE) ? 0 : 1;

    if (choice == IDC_EU || choice == IDC_EW) {
        if (!found)
            say_error("No XCOM: Enemy Unknown installation was found.\n\n"
                      "Put the install folder in launcher.ini beside this program:\n\n"
                      "[launcher]\nGameRoot=D:\\path\\to\\XCom-Enemy-Unknown");
        else
            say_error("%s is not installed under\n%s.",
                      choice == IDC_EU ? "XCOM: Enemy Unknown" : "XCOM: Enemy Within",
                      cfg.game.root);
        return 1;
    }
    return 0;                    // cancelled
}
