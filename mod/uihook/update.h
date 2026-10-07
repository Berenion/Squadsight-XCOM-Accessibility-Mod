// Finding and fetching a newer release.
//
// The releases are GitHub's: the launcher asks the API for the repository's
// latest release (drafts and pre-releases are never "latest"), compares its
// tag with MOD_VERSION, and downloads the release's .zip.  Unpacking is left to
// Windows' own tar.exe (bsdtar, which reads zip), present since Windows 10 1803,
// rather than to a zip reader of our own.
//
// The parsing is split from the network so that test_release.exe can check it
// without one.
#ifndef UPDATE_H
#define UPDATE_H

#include <windows.h>

typedef struct {
    char tag[64];               // "v0.9.1"
    int  version[3];
    char zip_url[1024];         // the first .zip asset's download address
    char page_url[512];         // the release's page, for a failed download
    char notes[2048];           // the release's text, unescaped, may be cut
} Release;

// "0.9.1" or "v0.9.1" (a missing part is 0).  FALSE if it starts with no number.
BOOL update_parse_version(const char* text, int out[3]);

// <0, 0, >0 as a is older than, the same as, or newer than b.
int update_compare(const int a[3], const int b[3]);

// The string value of the first "key" in json, unescaped into out.  FALSE when
// the key is missing or its value is not a string (null, for an empty body).
BOOL update_json_string(const char* json, const char* key, char* out, size_t out_sz);

// The first browser_download_url ending in .zip.
BOOL update_zip_url(const char* json, char* out, size_t out_sz);

// Fills r from the text of a releases/latest answer.
BOOL update_parse_release(const char* json, Release* r, char* err, size_t err_sz);

// Asks GitHub for repo's latest release ("owner/name").
BOOL update_latest(const char* repo, Release* r, char* err, size_t err_sz);

// Downloads url into the file path.
BOOL update_download(const char* url, const char* path, char* err, size_t err_sz);

// Unpacks a zip into dir (which must exist) with Windows' tar.exe.
BOOL update_unzip(const char* zip, const char* dir, char* err, size_t err_sz);

#endif
