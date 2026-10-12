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

// GOG's installer and Galaxy both register the game under the product id, with
// its folder as "path".  The key is in the 32-bit view (WOW6432Node), which is
// asked for by name so that a 64-bit build of this would find it too.
BOOL gamepaths_gog_root(char* root, size_t root_sz)
{
    DWORD size = (DWORD)root_sz;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games\\" XCOM_GOG_ID, "path",
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, NULL, root, &size) != ERROR_SUCCESS)
        return FALSE;
    size_t len = strlen(root);                       // stored with or without one
    if (len > 3 && root[len - 1] == '\\') root[len - 1] = 0;
    return root[0] != 0 && gamepaths_root_holds_xcom(root);
}

static void gog_info_path(const char* root, char* out, size_t out_sz)
{
    sprintf_s(out, out_sz, "%s\\goggame-%s.info", root, XCOM_GOG_ID);
}

BOOL gamepaths_root_is_gog(const char* root)
{
    char info[MAX_PATH];
    gog_info_path(root, info, sizeof info);
    return PathFileExistsA(info);
}

BOOL gamepaths_find(const char* override_root, GamePaths* out)
{
    memset(out, 0, sizeof *out);

    if (override_root && override_root[0] && gamepaths_root_holds_xcom(override_root))
        strcpy_s(out->root, MAX_PATH, override_root);
    else if (!find_root(out->root, MAX_PATH) && !gamepaths_gog_root(out->root, MAX_PATH))
        return FALSE;
    out->gog = gamepaths_root_is_gog(out->root);

    char probe[MAX_PATH];
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", out->root, EU_SUBDIR, EU_EXE);
    if (PathFileExistsA(probe)) strcpy_s(out->eu_exe, MAX_PATH, probe);
    sprintf_s(probe, sizeof probe, "%s\\%s\\%s", out->root, EW_SUBDIR, EW_EXE);
    if (PathFileExistsA(probe)) strcpy_s(out->ew_exe, MAX_PATH, probe);

    return out->eu_exe[0] != 0 || out->ew_exe[0] != 0;
}

// ---------------------------------------------------------------- language --

// Steam's names (the Steamworks API language codes, as the manifest stores
// them) for the languages the game ships, against the engine's folder names
// under XComGame\Localization. Latin American Spanish has no folder of its
// own in either build, so it gets Spain's.
static const struct { const char* steam; const char* code; } g_languages[] = {
    { "english",  "INT" }, { "french",   "FRA" }, { "german",   "DEU" },
    { "italian",  "ITA" }, { "spanish",  "ESN" }, { "latam",    "ESN" },
    { "polish",   "POL" }, { "russian",  "RUS" }, { "japanese", "JPN" },
    { "koreana",  "KOR" }, { "tchinese", "CHT" },
    { "korean",   "KOR" },                           // GOG's name for it
};

const char* gamepaths_language_code(const char* steam_name)
{
    for (size_t i = 0; i < sizeof g_languages / sizeof g_languages[0]; ++i)
        if (_stricmp(steam_name, g_languages[i].steam) == 0) return g_languages[i].code;
    return NULL;
}

// The manifest has "language" twice: under UserConfig, what the player picked
// in the game's Properties, and under MountedConfig, what Steam has actually
// installed. They differ while a change is still downloading, and then only
// the mounted one has its files on disk, so that one is preferred. Neither is
// nested deeper than one block, so the block a pair sits in is simply the
// last key that stood alone on its line.
BOOL gamepaths_steam_language(const char* manifest, char* lang, size_t lang_sz)
{
    char* text = gamepaths_slurp(manifest);
    if (!text) return FALSE;

    char block[64] = "", user[64] = "", mounted[64] = "";
    char* next = NULL;
    for (char* line = strtok_s(text, "\r\n", &next); line;
         line = strtok_s(NULL, "\r\n", &next)) {
        char key[64], value[64];
        if (gamepaths_vdf_pair(line, key, sizeof key, value, sizeof value)) {
            if (_stricmp(key, "language") != 0) continue;
            if (_stricmp(block, "MountedConfig") == 0) strcpy_s(mounted, sizeof mounted, value);
            else if (_stricmp(block, "UserConfig") == 0) strcpy_s(user, sizeof user, value);
            continue;
        }
        const char* q = strchr(line, '"');
        const char* end = q ? strchr(q + 1, '"') : NULL;
        if (end && (size_t)(end - q - 1) < sizeof block) {
            memcpy(block, q + 1, (size_t)(end - q - 1));
            block[end - q - 1] = 0;
        }
    }
    free(text);

    const char* found = mounted[0] ? mounted : user;
    if (!found[0]) return FALSE;
    strcpy_s(lang, lang_sz, found);
    return TRUE;
}

// The .info is JSON, but the one key wanted is unique in it and flat:
//     "language": "German",
// beside "languages": [ "de-DE" ], which the closing quote keeps from matching.
BOOL gamepaths_gog_language(const char* root, char* lang, size_t lang_sz)
{
    char info[MAX_PATH];
    gog_info_path(root, info, sizeof info);
    char* text = gamepaths_slurp(info);
    if (!text) return FALSE;

    BOOL found = FALSE;
    const char* p = strstr(text, "\"language\"");
    if (p) p = strchr(p + 10, ':');
    if (p) p = strchr(p, '"');
    if (p) {
        const char* end = strchr(++p, '"');
        if (end && end > p && (size_t)(end - p) < lang_sz) {
            memcpy(lang, p, (size_t)(end - p));
            lang[end - p] = 0;
            found = TRUE;
        }
    }
    free(text);
    return found;
}

// The manifest sits two folders above the install: the root is
// <library>\steamapps\common\<installdir>, the manifest
// <library>\steamapps\appmanifest_*.acf. A GameRoot override outside a Steam
// library has none.
void gamepaths_manifest_for(const char* root, char* out, size_t out_sz)
{
    sprintf_s(out, out_sz, "%s\\..\\..\\appmanifest_%s.acf", root, XCOM_APPID);
}

// Each build has its own XComGame folder beside its Binaries: the root's for
// Enemy Unknown, XEW's for Enemy Within, and they do not ship the same
// languages (this install's EU has INT alone, its EW all ten).
BOOL gamepaths_has_language(const char* exe_path, const char* code)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof dir, exe_path);
    for (int up = 0; up < 3; ++up) {                 // the exe, Win32, Binaries
        char* slash = strrchr(dir, '\\');
        if (!slash) return FALSE;
        *slash = 0;
    }
    char probe[MAX_PATH];
    sprintf_s(probe, sizeof probe, "%s\\XComGame\\Localization\\%s", dir, code);
    DWORD attr = GetFileAttributesA(probe);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}
