// Finding and fetching a newer release.
//
// The releases are GitHub's: the launcher asks the API for the repository's
// latest release (drafts and pre-releases are never "latest"), compares its
// tag with MOD_VERSION, and downloads the release's files.  Each file of the
// mod is an asset of its own, beside the setup (SETUP_NAME), so the setup and
// an update fetch the same files the same way and nothing has to be unpacked.
//
// The parsing is split from the network so that test_release.exe can check it
// without one.
#ifndef UPDATE_H
#define UPDATE_H

#include <windows.h>
#include "version.h"

// The one file a player downloads.  No version in the name, so that
// releases/latest/download/Squadsight-Setup.exe is a link that never goes
// stale.  It is never fetched by itself or by an update.
#define SETUP_NAME   MOD_NAME "-Setup.exe"

// %TEMP%\Squadsight-update: where the setup and an update put the files they
// download.  The installed launcher clears it on its next start.
#define UPDATE_DIR   MOD_NAME "-update"

#define RELEASE_ASSETS 32

typedef struct {
    char name[128];             // the file name, from the end of the address
    char url[512];              // its browser_download_url
} ReleaseAsset;

typedef struct {
    char tag[64];               // "v0.9.1"
    int  version[3];
    char page_url[512];         // the release's page, for a failed download
    char notes[2048];           // the release's text, unescaped, may be cut
    int  asset_count;
    ReleaseAsset assets[RELEASE_ASSETS];
} Release;

// "0.9.1" or "v0.9.1" (a missing part is 0).  FALSE if it starts with no number.
BOOL update_parse_version(const char* text, int out[3]);

// <0, 0, >0 as a is older than, the same as, or newer than b.
int update_compare(const int a[3], const int b[3]);

// The string value of the first "key" in json, unescaped into out.  FALSE when
// the key is missing or its value is not a string (null, for an empty body).
BOOL update_json_string(const char* json, const char* key, char* out, size_t out_sz);

// Every browser_download_url in json, named by its last path segment.
// Returns how many were kept (at most max).
int update_assets(const char* json, ReleaseAsset* out, int max);

// The asset with this file name, or NULL.
const ReleaseAsset* update_asset(const Release* r, const char* name);

// Whether an asset is one of the mod's files, the ones an install copies:
// everything but the setup and a zip (the 0.9.0 release's package).
BOOL update_asset_is_mod_file(const ReleaseAsset* a);

// Fills r from the text of a releases/latest answer.  A release without a
// launcher.exe among its assets is refused: there would be nothing to install
// the rest with.
BOOL update_parse_release(const char* json, Release* r, char* err, size_t err_sz);

// Asks GitHub for repo's latest release ("owner/name").
BOOL update_latest(const char* repo, Release* r, char* err, size_t err_sz);

// Downloads url into the file path.
BOOL update_download(const char* url, const char* path, char* err, size_t err_sz);

// Downloads every one of the release's mod files into dir (which must exist).
// `progress`, if given, is told each file as it starts ("Downloading
// xcom_uihook.dll, 3 of 12...").
BOOL update_fetch_files(const Release* r, const char* dir,
                        void (*progress)(void* ctx, const char* text), void* ctx,
                        char* err, size_t err_sz);

#endif
