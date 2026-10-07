// Installing, updating and removing the mod.  See install.h.

#include "install.h"
#include "injector.h"
#include "version.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" MOD_NAME

#define SHORTCUT_NAME MOD_NAME ".lnk"

enum { REQUIRED = 1, X86 = 2 };

// Everything a release holds, and so everything an install copies.  The DLL
// loads the sounds and the NVDA client from beside itself (main.c's init,
// speech.c's load_from_nearby).  Tolk and the System Access and SuperNova
// clients its drivers load are a 32-bit build (tools\build_tolk.bat); a
// 64-bit one would not load in the game, so the X86 check refuses it.
static const struct { const char* name; int flags; } FILES[] = {
    { "launcher.exe",               REQUIRED },
    { "xcom_uihook.dll",            REQUIRED | X86 },
    { "inject.exe",                 REQUIRED },
    { "ekgbeep.wav",                REQUIRED },
    { "alienbeat.wav",              REQUIRED },
    { "doorsound.wav",              REQUIRED },
    { "windowsound.wav",            REQUIRED },
    { "CREDITS.md",                 REQUIRED },     // the CC BY / CC BY-NC sounds need it
    { "nvdaControllerClient32.dll", REQUIRED | X86 },
    { "Tolk.dll",                   REQUIRED | X86 },
    { "SAAPI32.dll",                REQUIRED | X86 },
    { "dolapi32.dll",               REQUIRED | X86 },
};
#define FILE_COUNT (sizeof FILES / sizeof FILES[0])

// What the DLL writes beside the game's exe (main.c, settings.c, log.c).
static const char* const GAME_FILES[] = {
    "xcom_uihook_settings.ini", "xcom_uihook_ammo.ini", "xcom_uihook.log",
};

// ------------------------------------------------------------------- paths --

static void exe_dir(char* out, size_t out_sz)
{
    GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    char* slash = strrchr(out, '\\');
    if (slash) *slash = 0;
}

static BOOL same_dir(const char* a, const char* b)
{
    char fa[MAX_PATH], fb[MAX_PATH];
    if (!GetFullPathNameA(a, MAX_PATH, fa, NULL) || !GetFullPathNameA(b, MAX_PATH, fb, NULL))
        return FALSE;
    PathRemoveBackslashA(fa);
    PathRemoveBackslashA(fb);
    return _stricmp(fa, fb) == 0;
}

static BOOL reg_string(const char* name, char* out, size_t out_sz)
{
    DWORD size = (DWORD)out_sz;
    out[0] = 0;
    return RegGetValueA(HKEY_CURRENT_USER, UNINSTALL_KEY, name, RRF_RT_REG_SZ, NULL,
                        out, &size) == ERROR_SUCCESS && out[0];
}

void install_dir(char* out, size_t out_sz)
{
    if (reg_string("InstallLocation", out, out_sz)) return;
    char local[MAX_PATH] = { 0 };
    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local);
    sprintf_s(out, out_sz, "%s\\Programs\\%s", local, MOD_NAME);
}

BOOL install_present(char* version, size_t version_sz)
{
    char dir[MAX_PATH], exe[MAX_PATH];
    version[0] = 0;
    if (!reg_string("InstallLocation", dir, sizeof dir)) return FALSE;
    sprintf_s(exe, sizeof exe, "%s\\launcher.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return FALSE;
    reg_string("DisplayVersion", version, version_sz);
    return TRUE;
}

BOOL install_running_installed(void)
{
    char here[MAX_PATH], dir[MAX_PATH];
    exe_dir(here, sizeof here);
    install_dir(dir, sizeof dir);
    return same_dir(here, dir);
}

const char* install_game_running(void)
{
    if (injector_find_pid("XComEW.exe")) return "XCOM: Enemy Within";
    if (injector_find_pid("XComGame.exe")) return "XCOM: Enemy Unknown";
    return NULL;
}

// ------------------------------------------------------------------- files --

static BOOL is_x86(const char* path)
{
    BOOL ok = FALSE;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    IMAGE_DOS_HEADER dos;
    DWORD got = 0;
    if (ReadFile(f, &dos, sizeof dos, &got, NULL) && got == sizeof dos &&
        dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew + 4, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        WORD machine = 0;
        ok = ReadFile(f, &machine, sizeof machine, &got, NULL) && got == sizeof machine &&
             machine == IMAGE_FILE_MACHINE_I386;
    }
    CloseHandle(f);
    return ok;
}

// The first of the folders that has this file, usable as the mod's.
static BOOL find_source(const char* const* dirs, int n, int index, char* out, size_t out_sz,
                        char* err, size_t err_sz)
{
    for (int d = 0; d < n; ++d) {
        sprintf_s(out, out_sz, "%s\\%s", dirs[d], FILES[index].name);
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) continue;
        if ((FILES[index].flags & X86) && !is_x86(out)) {
            launcher_log("install: %s is not 32-bit -- passed over\n", out);
            continue;
        }
        return TRUE;
    }
    out[0] = 0;
    if (FILES[index].flags & REQUIRED) {
        sprintf_s(err, err_sz, "%s is missing from %s.", FILES[index].name, dirs[0]);
        return FALSE;
    }
    return TRUE;
}

// A file being replaced may still be held for a moment: the old launcher is
// exiting while the new one installs over it.
static BOOL copy_patiently(const char* from, const char* to)
{
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (CopyFileA(from, to, FALSE)) return TRUE;
        DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED &&
            e != ERROR_LOCK_VIOLATION)
            return FALSE;
        Sleep(250);
    }
    return FALSE;
}

// Every source is found before anything is copied, so that a broken package
// leaves the installed version as it was.
static BOOL copy_files(const char* const* dirs, int n, const char* dst, char* err, size_t err_sz)
{
    char src[FILE_COUNT][MAX_PATH];
    for (int i = 0; i < (int)FILE_COUNT; ++i)
        if (!find_source(dirs, n, i, src[i], MAX_PATH, err, err_sz)) return FALSE;

    int rc = SHCreateDirectoryExA(NULL, dst, NULL);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
        sprintf_s(err, err_sz, "Could not create the folder %s (error %d).", dst, rc);
        return FALSE;
    }

    for (int i = 0; i < (int)FILE_COUNT; ++i) {
        char to[MAX_PATH];
        sprintf_s(to, sizeof to, "%s\\%s", dst, FILES[i].name);
        if (!src[i][0]) {
            // An optional file this version does not have: an older one left
            // in place would be loaded as if it belonged.
            DeleteFileA(to);
            continue;
        }
        if (!copy_patiently(src[i], to)) {
            sprintf_s(err, err_sz, "Could not copy %s to %s (error %lu).",
                      FILES[i].name, dst, GetLastError());
            return FALSE;
        }
        launcher_log("install: %s -> %s\n", src[i], to);
    }
    return TRUE;
}

BOOL install_delete_tree(const char* dir)
{
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) return TRUE;
    char from[MAX_PATH + 2] = { 0 };            // double-terminated
    strcpy_s(from, MAX_PATH, dir);
    SHFILEOPSTRUCTA op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    return SHFileOperationA(&op) == 0 && !op.fAnyOperationsAborted;
}

// ---------------------------------------------------------------- shortcuts --

static BOOL shortcut(const char* lnk, const char* target, const char* args,
                     const char* work_dir, const char* description)
{
    IShellLinkA* link = NULL;
    IPersistFile* file = NULL;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkA, (void**)&link)))
        return FALSE;
    link->lpVtbl->SetPath(link, target);
    link->lpVtbl->SetArguments(link, args);
    link->lpVtbl->SetWorkingDirectory(link, work_dir);
    link->lpVtbl->SetDescription(link, description);
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void**)&file))) {
        wchar_t wlnk[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, MAX_PATH);
        ok = SUCCEEDED(file->lpVtbl->Save(file, wlnk, TRUE));
        file->lpVtbl->Release(file);
    }
    link->lpVtbl->Release(link);
    launcher_log("install: shortcut %s %s\n", lnk, ok ? "written" : "FAILED");
    return ok;
}

static void shell_folder(int csidl, char* out)
{
    out[0] = 0;
    SHGetFolderPathA(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, out);
}

// One shortcut, on the desktop, to the launcher itself: it asks which game to
// start, and holds install, uninstall and the update check.  The uninstall
// entry in Windows' list of installed apps is the other way back in.
static void write_shortcut(const char* dir)
{
    char exe[MAX_PATH], desktop[MAX_PATH], lnk[MAX_PATH];
    sprintf_s(exe, sizeof exe, "%s\\launcher.exe", dir);
    shell_folder(CSIDL_DESKTOPDIRECTORY, desktop);
    if (!desktop[0]) return;
    sprintf_s(lnk, sizeof lnk, "%s\\%s", desktop, SHORTCUT_NAME);

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    shortcut(lnk, exe, "", dir, "Start XCOM with " MOD_NAME ", or install, update or remove it");
    if (SUCCEEDED(com)) CoUninitialize();
}

static void remove_shortcut(void)
{
    char desktop[MAX_PATH], path[MAX_PATH];
    shell_folder(CSIDL_DESKTOPDIRECTORY, desktop);
    if (!desktop[0]) return;
    sprintf_s(path, sizeof path, "%s\\%s", desktop, SHORTCUT_NAME);
    DeleteFileA(path);
    launcher_log("uninstall: shortcut removed\n");
}

// ----------------------------------------------------------------- registry --

static void reg_set(HKEY key, const char* name, const char* value)
{
    RegSetValueExA(key, name, 0, REG_SZ, (const BYTE*)value, (DWORD)strlen(value) + 1);
}

static void reg_set_dword(HKEY key, const char* name, DWORD value)
{
    RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof value);
}

static BOOL write_uninstall_entry(const char* dir)
{
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL,
                        &key, NULL) != ERROR_SUCCESS)
        return FALSE;
    char exe[MAX_PATH], uninstall[MAX_PATH + 16];
    sprintf_s(exe, sizeof exe, "%s\\launcher.exe", dir);
    sprintf_s(uninstall, sizeof uninstall, "\"%s\" /uninstall", exe);

    DWORD kb = 0;
    for (int i = 0; i < (int)FILE_COUNT; ++i) {
        char path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        sprintf_s(path, sizeof path, "%s\\%s", dir, FILES[i].name);
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
            kb += (fa.nFileSizeLow + 1023) / 1024;
    }

    reg_set(key, "DisplayName", MOD_NAME " (XCOM accessibility mod)");
    reg_set(key, "DisplayVersion", MOD_VERSION);
    reg_set(key, "Publisher", "Berenion");
    reg_set(key, "InstallLocation", dir);
    reg_set(key, "UninstallString", uninstall);
    reg_set(key, "DisplayIcon", exe);
    reg_set(key, "URLInfoAbout", "https://github.com/" MOD_REPO);
    reg_set_dword(key, "NoModify", 1);
    reg_set_dword(key, "NoRepair", 1);
    reg_set_dword(key, "EstimatedSize", kb);
    RegCloseKey(key);
    return TRUE;
}

// ----------------------------------------------------------- install/remove --

BOOL install_from(const char* src_dir, char* err, size_t err_sz)
{
    char dir[MAX_PATH];
    install_dir(dir, sizeof dir);
    launcher_log("install: version %s from %s to %s\n", MOD_VERSION, src_dir, dir);
    if (same_dir(src_dir, dir)) {
        strcpy_s(err, err_sz, "This copy is already the installed one.");
        return FALSE;
    }
    const char* running = install_game_running();
    if (running) {
        sprintf_s(err, err_sz, "%s is running. Close the game first: its copy of the mod "
                               "cannot be replaced while it is loaded.", running);
        return FALSE;
    }
    const char* dirs[] = { src_dir };
    if (!copy_files(dirs, 1, dir, err, err_sz)) {
        launcher_log("install: FAILED -- %s\n", err);
        return FALSE;
    }
    if (!write_uninstall_entry(dir)) {
        strcpy_s(err, err_sz, "The files were copied, but Windows' list of installed apps "
                              "could not be written.");
        launcher_log("install: FAILED -- %s\n", err);
        return FALSE;
    }
    write_shortcut(dir);
    launcher_log("install: done, version %s\n", MOD_VERSION);
    return TRUE;
}

BOOL install_stage(const char* src_dir, const char* dir, char* err, size_t err_sz)
{
    char parent[MAX_PATH];
    strcpy_s(parent, sizeof parent, src_dir);
    char* slash = strrchr(parent, '\\');
    if (slash) *slash = 0;
    const char* dirs[] = { src_dir, parent };
    launcher_log("stage: version %s from %s to %s\n", MOD_VERSION, src_dir, dir);
    BOOL ok = copy_files(dirs, 2, dir, err, err_sz);
    launcher_log("stage: %s%s\n", ok ? "done" : "FAILED -- ", ok ? "" : err);
    return ok;
}

static void remove_game_files(const GamePaths* game)
{
    const char* exes[] = { game->eu_exe, game->ew_exe };
    for (int g = 0; g < 2; ++g) {
        if (!exes[g][0]) continue;
        char dir[MAX_PATH];
        strcpy_s(dir, sizeof dir, exes[g]);
        char* slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        for (int i = 0; i < (int)(sizeof GAME_FILES / sizeof GAME_FILES[0]); ++i) {
            char path[MAX_PATH];
            sprintf_s(path, sizeof path, "%s\\%s", dir, GAME_FILES[i]);
            if (DeleteFileA(path)) launcher_log("uninstall: deleted %s\n", path);
        }
    }
}

BOOL install_remove(const GamePaths* game, BOOL settings, char* err, size_t err_sz)
{
    const char* running = install_game_running();
    if (running) {
        sprintf_s(err, err_sz, "%s is running. Close the game first: the mod cannot be "
                               "removed while it is loaded.", running);
        return FALSE;
    }

    char dir[MAX_PATH];
    install_dir(dir, sizeof dir);
    launcher_log("uninstall: %s%s\n", dir, settings ? ", with the settings" : "");

    remove_shortcut();
    RegDeleteKeyA(HKEY_CURRENT_USER, UNINSTALL_KEY);
    if (settings) remove_game_files(game);

    if (!install_running_installed()) {
        if (!install_delete_tree(dir)) {
            sprintf_s(err, err_sz, "Some files in %s could not be removed.", dir);
            return FALSE;
        }
        return TRUE;
    }

    // A running exe cannot delete itself, so everything else goes now and a
    // hidden cmd.exe takes the exe and the folder once this process is gone.
    // It runs from the temp folder: a cmd whose current directory is the
    // install folder would keep that folder from being removed.
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    sprintf_s(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char path[MAX_PATH];
            sprintf_s(path, sizeof path, "%s\\%s", dir, fd.cFileName);
            if (_stricmp(path, self) == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) install_delete_tree(path);
            else DeleteFileA(path);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }

    char temp[MAX_PATH], cmd[MAX_PATH], line[3 * MAX_PATH + 128];
    GetTempPathA(MAX_PATH, temp);
    GetSystemDirectoryA(cmd, MAX_PATH);
    strcat_s(cmd, sizeof cmd, "\\cmd.exe");
    sprintf_s(line, sizeof line,
              "\"%s\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir /s /q \"%s\"",
              cmd, self, dir);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(cmd, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, temp, &si, &pi)) {
        sprintf_s(err, err_sz, "Everything but launcher.exe was removed; delete %s by hand.", dir);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}
