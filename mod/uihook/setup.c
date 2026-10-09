// Squadsight-Setup.exe: the one file a player downloads.
//
// It holds none of the mod.  It asks GitHub for the latest release, downloads
// that release's files (each one an asset of its own, update.h) into the temp
// folder, and starts the launcher among them with `/install /setup`, which
// copies them into the install folder, writes the uninstall entry and the
// desktop shortcut, and opens the installed launcher.  So the setup never goes
// stale: whatever release is latest is what it installs, and the installing
// itself is the launcher's, the same code an update runs.
//
// Message boxes and the progress window only, so a screen reader reads every
// step without help.  Linked with an asInvoker manifest (build.bat): without
// one, Windows takes "Setup" in the name to mean an installer and asks for
// administrator rights, which a per-user install does not need.

#include "progress.h"
#include "update.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TITLE MOD_NAME " Setup"
#define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" MOD_NAME

typedef struct {
    Release release;
    char work[MAX_PATH];
    char err[512];
    BOOL ok;
} Fetch;

// %TEMP%\Squadsight-setup.log: the setup leaves nothing beside itself, and
// the launcher's own log takes over once it runs.
static void setup_log(const char* fmt, ...)
{
    static char path[MAX_PATH];
    if (!path[0]) {
        char temp[MAX_PATH];
        GetTempPathA(MAX_PATH, temp);
        sprintf_s(path, sizeof path, "%s%s-setup.log", temp, MOD_NAME);
    }
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void say_error(const char* fmt, ...)
{
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(text, sizeof text, fmt, ap);
    va_end(ap);
    setup_log("error: %s\n", text);
    MessageBoxA(NULL, text, TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

// The installed version, from the uninstall entry install.c writes; "" if none.
static void installed_version(char* out, DWORD out_sz)
{
    out[0] = 0;
    if (RegGetValueA(HKEY_CURRENT_USER, UNINSTALL_KEY, "DisplayVersion", RRF_RT_REG_SZ,
                     NULL, out, &out_sz) != ERROR_SUCCESS)
        out[0] = 0;
}

static DWORD WINAPI fetch_thread(LPVOID param)
{
    Fetch* f = (Fetch*)param;
    if (!update_fetch_files(&f->release, f->work, progress_say, NULL, f->err, sizeof f->err))
        return 0;
    f->ok = TRUE;
    return 0;
}

// Empties the work folder, or makes it.  Files only: nothing the setup or an
// update puts there is a folder.
static BOOL fresh_work_dir(const char* dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    sprintf_s(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            sprintf_s(path, sizeof path, "%s\\%s", dir, fd.cFileName);
            DeleteFileA(path);
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    return CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)inst; (void)prev; (void)args; (void)show;

    {
        time_t now = time(NULL);
        struct tm local;
        localtime_s(&local, &now);
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        setup_log("---- %s setup\n", stamp);
    }

    static Fetch f;
    char err[512];
    if (!update_latest(MOD_REPO, &f.release, err, sizeof err)) {
        say_error("Could not find the latest %s release on GitHub.\n\n%s\n\n"
                  "Check the internet connection and run the setup again, or download "
                  "the files from https://github.com/%s/releases/latest",
                  MOD_NAME, err, MOD_REPO);
        return 1;
    }
    setup_log("latest release %s, %d files attached\n", f.release.tag, f.release.asset_count);

    char have[32], text[1024];
    installed_version(have, sizeof have);
    sprintf_s(text, sizeof text,
              "%s %s, the accessibility mod for XCOM: Enemy Unknown and Enemy Within, "
              "will be downloaded from GitHub and installed in your programs folder, "
              "with a %s shortcut on the desktop.%s%s%s\n\nInstall it now?",
              MOD_NAME, f.release.tag, MOD_NAME,
              have[0] ? "\n\nVersion " : "", have, have[0] ? " is installed now and will be replaced." : "");
    if (MessageBoxA(NULL, text, TITLE, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
        setup_log("declined\n");
        return 0;
    }

    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    sprintf_s(f.work, sizeof f.work, "%s%s", temp, UPDATE_DIR);
    if (!fresh_work_dir(f.work)) {
        say_error("Could not create %s (error %lu).", f.work, GetLastError());
        return 1;
    }

    sprintf_s(text, sizeof text, "Downloading %s %s, please wait...", MOD_NAME, f.release.tag);
    if (!progress_run(NULL, text, fetch_thread, &f) || !f.ok) {
        say_error("%s could not be downloaded.\n\n%s\n\nThe release page is %s",
                  MOD_NAME, f.err[0] ? f.err : "The download did not start.",
                  f.release.page_url);
        return 1;
    }
    setup_log("downloaded into %s\n", f.work);

    // The launcher installs, says where, and opens the installed copy; this
    // program's part is done once it has started.
    char launcher[MAX_PATH], command[MAX_PATH + 32];
    sprintf_s(launcher, sizeof launcher, "%s\\launcher.exe", f.work);
    sprintf_s(command, sizeof command, "\"%s\" /install /setup", launcher);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(launcher, command, NULL, NULL, FALSE, 0, NULL, f.work, &si, &pi)) {
        say_error("The downloaded launcher could not be started (error %lu).\n\n%s",
                  GetLastError(), launcher);
        return 1;
    }
    setup_log("started %s\n", command);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
