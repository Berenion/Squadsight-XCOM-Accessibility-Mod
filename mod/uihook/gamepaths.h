// Where the two builds live.
//
// Enemy Unknown and Enemy Within share one Steam app and one install folder:
// EU sits in Binaries\Win32 and EW in XEW\Binaries\Win32 beneath it.  GOG's
// Complete Pack is laid out the same way, and its executables are the same
// builds as Steam's (the native tables match entry for entry), so only the
// finding differs.  Nothing here is hardcoded to this machine -- the folder is
// whatever Steam or GOG says it is -- so that the launcher works on an install
// it has never seen.
//
// Split out of launcher.c so that it can be exercised without a game, in the
// same way as the rest of the mod.
#ifndef GAMEPATHS_H
#define GAMEPATHS_H

#include <windows.h>

// The Steam app id covers both builds: Enemy Within ships as a branch of the
// Enemy Unknown app rather than as an app of its own.
#define XCOM_APPID "200510"

// GOG's product id for the Complete Pack, which holds both builds the same way.
// It names the install's goggame-<id>.info and its key under GOG.com\Games.
#define XCOM_GOG_ID "1558688142"

typedef struct {
    char root[MAX_PATH];        // the install directory, e.g. ...\XCom-Enemy-Unknown
    char eu_exe[MAX_PATH];      // "" when that build is not installed
    char ew_exe[MAX_PATH];
    BOOL gog;                   // GOG's install: no Steam manifest, no Steam launch
} GamePaths;

// Locates the install and fills in whichever executables exist.  override_root
// may be "", otherwise it is an install directory to trust ahead of Steam and
// GOG.  Steam is asked before GOG.
// Returns FALSE, and leaves out zeroed, when neither build can be found.
BOOL gamepaths_find(const char* override_root, GamePaths* out);

// TRUE when root has at least one of the two builds where it should be.
BOOL gamepaths_root_holds_xcom(const char* root);

// TRUE when a Steam library folder holds the game, writing its install
// directory to root.
BOOL gamepaths_root_in_library(const char* library, char* root, size_t root_sz);

// TRUE when GOG's registry has the game, writing its install directory to root.
BOOL gamepaths_gog_root(char* root, size_t root_sz);

// TRUE when the install at root is GOG's: it carries GOG's goggame-<id>.info.
BOOL gamepaths_root_is_gog(const char* root);

// Parses one Steam key-value line:  "key"  "value".
BOOL gamepaths_vdf_pair(const char* line, char* key, size_t key_sz,
                        char* value, size_t value_sz);

// Reads a whole small text file.  Caller frees.  NULL if it cannot be read.
char* gamepaths_slurp(const char* path);

// The game has no language setting of its own: it plays in whatever
// -LANGUAGE= it is started with, and the publisher's launcher passes the
// language Steam has for the game.  These let ours do the same.

// The engine's folder name ("DEU") for one of Steam's language names
// ("german"), or NULL for a language the game does not ship.
const char* gamepaths_language_code(const char* steam_name);

// Steam's language for the game, from its manifest.  FALSE if unreadable.
BOOL gamepaths_steam_language(const char* manifest, char* lang, size_t lang_sz);

// GOG's language for the game ("German"), from the goggame-<id>.info in root,
// which GOG's installer and Galaxy both keep current.  FALSE if unreadable.
// GOG writes Steam's names capitalised, which gamepaths_language_code takes,
// and "Korean" for Steam's "koreana", which it takes as well.
BOOL gamepaths_gog_language(const char* root, char* lang, size_t lang_sz);

// Where the manifest of the install at root would be.
void gamepaths_manifest_for(const char* root, char* out, size_t out_sz);

// TRUE when the build at exe_path has that language's files.
BOOL gamepaths_has_language(const char* exe_path, const char* code);

#endif
