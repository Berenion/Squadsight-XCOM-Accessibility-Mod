// Offline checks for install discovery.  Run before the game:
//
//   build\test_paths.exe
//
// Most of it works against a Steam library built in the temp directory, so the
// walk is exercised whether or not this machine owns the game.  The last check
// reports what discovery finds on this machine, which is the part that cannot
// be faked.

#include "gamepaths.h"

#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int ok, const char* what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

// ------------------------------------------------------------- vdf parsing --

static void test_vdf_pair(void)
{
    char key[64], value[MAX_PATH];

    check(gamepaths_vdf_pair("\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"",
                             key, sizeof key, value, sizeof value) &&
          strcmp(key, "path") == 0 &&
          strcmp(value, "C:\\Program Files (x86)\\Steam") == 0,
          "vdf: a quoted pair, with doubled backslashes collapsed");

    check(gamepaths_vdf_pair("\t\"installdir\"\t\t\"XCom-Enemy-Unknown\"",
                             key, sizeof key, value, sizeof value) &&
          strcmp(key, "installdir") == 0 &&
          strcmp(value, "XCom-Enemy-Unknown") == 0,
          "vdf: installdir");

    check(gamepaths_vdf_pair("\t\"2\"\t\t\"D:\\\\SteamLibrary\"",
                             key, sizeof key, value, sizeof value) &&
          strcmp(key, "2") == 0 && strcmp(value, "D:\\SteamLibrary") == 0,
          "vdf: the old numbered-key layout");

    check(!gamepaths_vdf_pair("libraryfolders", key, sizeof key, value, sizeof value),
          "vdf: a line with no quotes is rejected");
    check(!gamepaths_vdf_pair("\t\"apps\"", key, sizeof key, value, sizeof value),
          "vdf: a key with no value is rejected");
    check(!gamepaths_vdf_pair("\t\"path\"\t\t\"unterminated",
                              key, sizeof key, value, sizeof value),
          "vdf: an unterminated value is rejected");

    // The value is longer than the buffer it is given, and must not run past it.
    char tiny[8];
    gamepaths_vdf_pair("\t\"path\"\t\t\"0123456789abcdef\"",
                       key, sizeof key, tiny, sizeof tiny);
    check(strlen(tiny) < sizeof tiny, "vdf: an overlong value is truncated, not overrun");
}

// ------------------------------------------------------- a fake Steam library --

static void make_tree(const char* path)
{
    char build[MAX_PATH];
    strcpy_s(build, sizeof build, path);
    for (char* p = build + 3; *p; ++p) {          // past "C:\"
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryA(build, NULL);
        *p = '\\';
    }
    CreateDirectoryA(build, NULL);
}

static void make_file(const char* path, const char* text)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof dir, path);
    char* slash = strrchr(dir, '\\');
    if (slash) { *slash = 0; make_tree(dir); }

    FILE* f = NULL;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        if (text) fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

static void remove_tree(const char* path)
{
    char pattern[MAX_PATH];
    sprintf_s(pattern, sizeof pattern, "%s\\*", path);

    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                continue;
            char child[MAX_PATH];
            sprintf_s(child, sizeof child, "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else
                DeleteFileA(child);
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

static void test_library_walk(const char* sandbox)
{
    char library[MAX_PATH], manifest[MAX_PATH], root[MAX_PATH], path[MAX_PATH];
    sprintf_s(library, sizeof library, "%s\\lib", sandbox);
    sprintf_s(manifest, sizeof manifest, "%s\\steamapps\\appmanifest_200510.acf", library);

    check(!gamepaths_root_in_library(library, root, sizeof root),
          "library: no manifest means the game is not here");

    // A manifest naming an install directory that does not exist. Steam leaves
    // these behind after a move, so believing one would send the launcher to a
    // folder with no game in it.
    make_file(manifest,
              "\"AppState\"\r\n{\r\n\t\"appid\"\t\t\"200510\"\r\n"
              "\t\"installdir\"\t\t\"XCom-Enemy-Unknown\"\r\n}\r\n");
    check(!gamepaths_root_in_library(library, root, sizeof root),
          "library: a manifest without the files is not believed");

    // Enemy Within only, which is how the folder looks with EU not installed.
    sprintf_s(path, sizeof path,
              "%s\\steamapps\\common\\XCom-Enemy-Unknown\\XEW\\Binaries\\Win32\\XComEW.exe",
              library);
    make_file(path, NULL);
    check(gamepaths_root_in_library(library, root, sizeof root) &&
          PathFileExistsA(root),
          "library: Enemy Within alone is enough to name the root");

    GamePaths game;
    check(gamepaths_find(root, &game) && game.ew_exe[0] && !game.eu_exe[0],
          "paths: Enemy Within found, Enemy Unknown correctly absent");

    sprintf_s(path, sizeof path,
              "%s\\steamapps\\common\\XCom-Enemy-Unknown\\Binaries\\Win32\\XComGame.exe",
              library);
    make_file(path, NULL);
    check(gamepaths_find(root, &game) && game.eu_exe[0] && game.ew_exe[0] &&
          PathFileExistsA(game.eu_exe) && PathFileExistsA(game.ew_exe),
          "paths: both builds found once both are present");

    // An override naming a folder with no game must be ignored rather than
    // trusted, or a stale launcher.ini would break discovery that would
    // otherwise have worked.
    char empty[MAX_PATH];
    sprintf_s(empty, sizeof empty, "%s\\not-the-game", sandbox);
    make_tree(empty);
    check(!gamepaths_root_holds_xcom(empty), "paths: an empty folder is not an install");
}

// ------------------------------------------------------------ this machine --

static void report_this_machine(void)
{
    GamePaths game;
    if (!gamepaths_find("", &game)) {
        printf("\ndiscovery on this machine: nothing found "
               "(fine if XCOM is not installed here)\n");
        return;
    }

    printf("\ndiscovery on this machine:\n  root: %s\n  EU:   %s\n  EW:   %s\n",
           game.root,
           game.eu_exe[0] ? game.eu_exe : "(not installed)",
           game.ew_exe[0] ? game.ew_exe : "(not installed)");

    check(gamepaths_root_holds_xcom(game.root),
          "machine: the discovered root really holds a build");
    check(!game.eu_exe[0] || PathFileExistsA(game.eu_exe),
          "machine: the Enemy Unknown path exists");
    check(!game.ew_exe[0] || PathFileExistsA(game.ew_exe),
          "machine: the Enemy Within path exists");
}

int main(void)
{
    char temp[MAX_PATH], sandbox[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    sprintf_s(sandbox, sizeof sandbox, "%sxcom_uihook_test_paths", temp);
    remove_tree(sandbox);

    test_vdf_pair();
    test_library_walk(sandbox);
    report_this_machine();

    remove_tree(sandbox);

    printf("\n%s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
