#include "gamepaths.h"

#include <shlwapi.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EU_SUBDIR    "Binaries\\Win32"
#define EW_SUBDIR    "XEW\\Binaries\\Win32"
#define EU_EXE       "XComGame.exe"
#define EW_EXE       "XComEW.exe"

// NB: _fsopen, not fopen_s.  fopen_s opens *exclusively*, and every file this
// reads is one somebody else may be holding: the launcher reads the mod's log
// back to see whether the hooks armed, and the mod keeps that log open for
// the whole session (_SH_DENYWR, so that it stays readable).  Opened
// exclusively the read always failed, and the launcher reported "the mod
// wrote no log" over a log that was sitting right there, on every launch.
// Steam's own .vdf files are open at times for the same reason.
char* gamepaths_slurp(const char* path)
{
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char* text = (char*)malloc((size_t)size + 1);
    if (text) {
        size_t got = fread(text, 1, (size_t)size, f);
        text[got] = 0;
    }
    fclose(f);
    return text;
}

// Steam's key-value files put both halves of a line in quotes. Nothing here
// needs the nesting, only the pairs, so this is a scanner rather than a tree
// walk -- and the keys it looks for are unique within the files it reads.
BOOL gamepaths_vdf_pair(const char* line, char* key, size_t key_sz,
                        char* value, size_t value_sz)
{
    const char* p = strchr(line, '"');
    if (!p) return FALSE;
    size_t n = 0;
    for (++p; *p && *p != '"'; ++p)
        if (n + 1 < key_sz) key[n++] = *p;
    if (*p != '"') return FALSE;
    key[n] = 0;

    p = strchr(p + 1, '"');
    if (!p) return FALSE;
    n = 0;
    for (++p; *p && *p != '"'; ++p) {
        char c = *p;
        if (c == '\\' && p[1]) c = *++p;   // "C:\\Steam" is one backslash
        if (n + 1 < value_sz) value[n++] = c;
    }
    if (*p != '"') return FALSE;
    value[n] = 0;
    return TRUE;
}

// Finds the value of one key anywhere in a vdf or acf file.
static BOOL vdf_lookup(const char* path, const char* want, char* value, size_t value_sz)
{
    char* text = gamepaths_slurp(path);
    if (!text) return FALSE;

    BOOL found = FALSE;
    char* next = NULL;
    for (char* line = strtok_s(text, "\r\n", &next); line && !found;
         line = strtok_s(NULL, "\r\n", &next)) {
        char key[64], val[MAX_PATH];
        if (gamepaths_vdf_pair(line, key, sizeof key, val, sizeof val) &&
            _stricmp(key, want) == 0) {
            strcpy_s(value, value_sz, val);
            found = TRUE;
        }
    }
    free(text);
    return found;
}

BOOL gamepaths_root_holds_xcom(const char* root)
{
    char probe[MAX_PATH];
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", root, EU_SUBDIR, EU_EXE);
    if (PathFileExistsA(probe)) return TRUE;
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", root, EW_SUBDIR, EW_EXE);
    return PathFileExistsA(probe);
}

// The manifest's installdir is the only reliable name for the folder: it is
// not derived from the store title, and it is not the appid either.
BOOL gamepaths_root_in_library(const char* library, char* root, size_t root_sz)
{
    char manifest[MAX_PATH], installdir[MAX_PATH];
    sprintf_s(manifest, sizeof manifest, "%s\\steamapps\\appmanifest_%s.acf",
              library, XCOM_APPID);
    if (!PathFileExistsA(manifest)) return FALSE;
    if (!vdf_lookup(manifest, "installdir", installdir, sizeof installdir)) return FALSE;

    sprintf_s(root, root_sz, "%s\\steamapps\\common\\%s", library, installdir);
    return gamepaths_root_holds_xcom(root);
}

static BOOL is_all_digits(const char* s)
{
    if (!*s) return FALSE;
    for (; *s; ++s) if (*s < '0' || *s > '9') return FALSE;
    return TRUE;
}

static BOOL steam_path(char* out, size_t out_sz)
{
    DWORD size = (DWORD)out_sz;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
                     RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS)
        return FALSE;
    for (char* p = out; *p; ++p) if (*p == '/') *p = '\\';   // stored with slashes
    return out[0] != 0;
}

// Walks every Steam library folder.  The file has had two layouts -- the path
// used to hang off a numbered key directly and now sits under "path" inside a
// block -- so both shapes are accepted.
static BOOL find_root(char* root, size_t root_sz)
{
    char steam[MAX_PATH];
    if (!steam_path(steam, sizeof steam)) return FALSE;

    // Steam's own folder is a library too, and is not always listed as one.
    if (gamepaths_root_in_library(steam, root, root_sz)) return TRUE;

    char vdf[MAX_PATH];
    sprintf_s(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", steam);
    char* text = gamepaths_slurp(vdf);
    if (!text) return FALSE;

    BOOL found = FALSE;
    char* next = NULL;
    for (char* line = strtok_s(text, "\r\n", &next); line && !found;
         line = strtok_s(NULL, "\r\n", &next)) {
        char key[64], value[MAX_PATH];
        if (!gamepaths_vdf_pair(line, key, sizeof key, value, sizeof value)) continue;
        // A drive letter is what separates an old-style "1" "D:\\Games" entry
        // from the numbered appid-to-size pairs that share the same shape.
        if (_stricmp(key, "path") != 0 && !(is_all_digits(key) && strchr(value, ':')))
            continue;
        found = gamepaths_root_in_library(value, root, root_sz);
    }
    free(text);
    return found;
}

BOOL gamepaths_find(const char* override_root, GamePaths* out)
{
    memset(out, 0, sizeof *out);

    if (override_root && override_root[0] && gamepaths_root_holds_xcom(override_root))
        strcpy_s(out->root, MAX_PATH, override_root);
    else if (!find_root(out->root, MAX_PATH))
        return FALSE;

    char probe[MAX_PATH];
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", out->root, EU_SUBDIR, EU_EXE);
    if (PathFileExistsA(probe)) strcpy_s(out->eu_exe, MAX_PATH, probe);
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", out->root, EW_SUBDIR, EW_EXE);
    if (PathFileExistsA(probe)) strcpy_s(out->ew_exe, MAX_PATH, probe);

    return out->eu_exe[0] != 0 || out->ew_exe[0] != 0;
}
