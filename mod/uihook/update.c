// Finding and fetching a newer release.  See update.h.

#include "update.h"

#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------- parsing --

BOOL update_parse_version(const char* text, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!text) return FALSE;
    while (*text == ' ') ++text;
    if (*text == 'v' || *text == 'V') ++text;
    if (*text < '0' || *text > '9') return FALSE;
    for (int i = 0; i < 3; ++i) {
        int n = 0;
        while (*text >= '0' && *text <= '9') n = n * 10 + (*text++ - '0');
        out[i] = n;
        if (*text != '.') break;
        ++text;
    }
    return TRUE;
}

int update_compare(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Unescapes the JSON string starting just after its opening quote.  \uXXXX is
// written as UTF-8 (a surrogate pair is one code point), which is what the
// message box gets converted from.
static void json_unescape(const char* p, char* out, size_t out_sz)
{
    size_t n = 0;
    while (*p && *p != '"' && n + 4 < out_sz) {
        char c = *p++;
        if (c != '\\') { out[n++] = c; continue; }
        c = *p++;
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': break;                     // \r\n is said once as \n
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i) {
                int d = hex_digit(*p);
                if (d < 0) break;
                cp = cp * 16 + (unsigned)d;
                ++p;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = 0;
                for (int i = 0; i < 4; ++i) {
                    int d = hex_digit(p[2 + i]);
                    if (d < 0) { lo = 0; break; }
                    lo = lo * 16 + (unsigned)d;
                }
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (cp < 0x80) out[n++] = (char)cp;
            else if (cp < 0x800) {
                out[n++] = (char)(0xC0 | (cp >> 6));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[n++] = (char)(0xE0 | (cp >> 12));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[n++] = (char)(0xF0 | (cp >> 18));
                out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        case 0: --p; break;
        default: out[n++] = c; break;        // \" \\ \/
        }
    }
    out[n] = 0;
}

// The value after "key" and its colon, or NULL.  Searching from `from` lets the
// caller walk every occurrence of a key (the assets' download addresses).
static const char* json_value(const char* from, const char* key, const char** next)
{
    char pattern[96];
    sprintf_s(pattern, sizeof pattern, "\"%s\"", key);
    const char* hit = strstr(from, pattern);
    if (!hit) return NULL;
    const char* p = hit + strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') { if (next) *next = p; return json_value(p, key, next); }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (next) *next = p;
    return p;
}

BOOL update_json_string(const char* json, const char* key, char* out, size_t out_sz)
{
    out[0] = 0;
    const char* v = json ? json_value(json, key, NULL) : NULL;
    if (!v || *v != '"') return FALSE;
    json_unescape(v + 1, out, out_sz);
    return TRUE;
}

// The name is taken from the address rather than from the asset's "name": the
// asset object holds other "name" keys (its uploader's, in "login" and the
// like, and a release's own title comes before the assets), while the last
// segment of browser_download_url is always the file as it was uploaded.
int update_assets(const char* json, ReleaseAsset* out, int max)
{
    int n = 0;
    const char* from = json;
    const char* v;
    while (n < max && from && (v = json_value(from, "browser_download_url", &from)) != NULL) {
        if (*v != '"') continue;
        ReleaseAsset* a = &out[n];
        json_unescape(v + 1, a->url, sizeof a->url);
        const char* slash = strrchr(a->url, '/');
        if (!slash || !slash[1]) continue;
        strncpy_s(a->name, sizeof a->name, slash + 1, _TRUNCATE);
        ++n;
    }
    return n;
}

const ReleaseAsset* update_asset(const Release* r, const char* name)
{
    for (int i = 0; i < r->asset_count; ++i)
        if (_stricmp(r->assets[i].name, name) == 0) return &r->assets[i];
    return NULL;
}

BOOL update_asset_is_mod_file(const ReleaseAsset* a)
{
    size_t len = strlen(a->name);
    if (_stricmp(a->name, SETUP_NAME) == 0) return FALSE;
    if (len > 4 && _stricmp(a->name + len - 4, ".zip") == 0) return FALSE;
    return TRUE;
}

BOOL update_parse_release(const char* json, Release* r, char* err, size_t err_sz)
{
    memset(r, 0, sizeof *r);
    if (!update_json_string(json, "tag_name", r->tag, sizeof r->tag) ||
        !update_parse_version(r->tag, r->version)) {
        // A missing tag is what GitHub's error answers look like ("Not Found"
        // for a private or renamed repository, a rate limit message).
        char message[256];
        if (update_json_string(json, "message", message, sizeof message))
            sprintf_s(err, err_sz, "GitHub answered: %s", message);
        else
            strcpy_s(err, err_sz, "GitHub's answer named no release.");
        return FALSE;
    }
    update_json_string(json, "html_url", r->page_url, sizeof r->page_url);
    update_json_string(json, "body", r->notes, sizeof r->notes);
    r->asset_count = update_assets(json, r->assets, RELEASE_ASSETS);
    if (!update_asset(r, "launcher.exe")) {
        sprintf_s(err, err_sz, "Release %s has no launcher.exe attached.", r->tag);
        return FALSE;
    }
    return TRUE;
}

// ----------------------------------------------------------------- network --

// Short timeouts: the check runs before the game starts, and an offline
// machine should cost the player a moment, not half a minute.
#define RESOLVE_MS   4000
#define CONNECT_MS   4000
#define SEND_MS      4000
#define RECEIVE_MS  15000

static void win_error(char* err, size_t err_sz, const char* what)
{
    DWORD code = GetLastError();
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
        sprintf_s(err, err_sz, "%s: could not reach the internet.", what);
        break;
    case ERROR_WINHTTP_TIMEOUT:
        sprintf_s(err, err_sz, "%s: the connection timed out.", what);
        break;
    default:
        sprintf_s(err, err_sz, "%s failed (error %lu).", what, code);
    }
}

// GETs an https address.  The body goes to `file` if given, else into a
// malloc'd, zero-terminated buffer in *body.
static BOOL http_get(const char* url, FILE* file, char** body, char* err, size_t err_sz)
{
    wchar_t wurl[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);

    URL_COMPONENTS parts = { sizeof parts };
    wchar_t host[256], path[1024];
    parts.lpszHostName = host;  parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;   parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts)) {
        sprintf_s(err, err_sz, "Bad address: %s", url);
        return FALSE;
    }

    BOOL ok = FALSE;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    char* buf = NULL;
    size_t len = 0;

    // The automatic proxy is the system's own setting, as a browser would use.
    session = WinHttpOpen(L"Squadsight-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)  // before Windows 8.1 there is no automatic proxy
        session = WinHttpOpen(L"Squadsight-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { win_error(err, err_sz, "Starting the connection"); goto done; }
    WinHttpSetTimeouts(session, RESOLVE_MS, CONNECT_MS, SEND_MS, RECEIVE_MS);

    connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect) { win_error(err, err_sz, "Connecting"); goto done; }

    request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { win_error(err, err_sz, "Opening the request"); goto done; }

    // GitHub's API wants an Accept of its own; the download host ignores it.
    // Redirects (the asset address moves to GitHub's file host) are followed
    // by WinHTTP itself.
    if (!WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        win_error(err, err_sz, "Asking GitHub");
        goto done;
    }

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            win_error(err, err_sz, "Reading the answer");
            goto done;
        }
        if (!avail) break;
        char* grown = (char*)realloc(buf, len + avail + 1);
        if (!grown) { strcpy_s(err, err_sz, "Out of memory."); goto done; }
        buf = grown;
        DWORD got = 0;
        if (!WinHttpReadData(request, buf + len, avail, &got)) {
            win_error(err, err_sz, "Reading the answer");
            goto done;
        }
        if (file && status == 200 && fwrite(buf + len, 1, got, file) != got) {
            strcpy_s(err, err_sz, "Could not write the download to disk.");
            goto done;
        }
        if (file) { len = 0; continue; }
        len += got;
    }
    if (buf) buf[len] = 0;

    // A 404 from the API still carries a JSON message worth reporting, so
    // only a download treats a non-200 as the end of it.
    if (file && status != 200) {
        sprintf_s(err, err_sz, "The download failed: the server answered %lu.", status);
        goto done;
    }
    if (!file) {
        if (!buf) buf = (char*)calloc(1, 1);
        *body = buf;
        buf = NULL;
    }
    ok = TRUE;

done:
    free(buf);
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

BOOL update_latest(const char* repo, Release* r, char* err, size_t err_sz)
{
    char url[512];
    sprintf_s(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", repo);
    char* body = NULL;
    if (!http_get(url, NULL, &body, err, err_sz)) return FALSE;
    BOOL ok = update_parse_release(body, r, err, err_sz);
    free(body);
    return ok;
}

BOOL update_download(const char* url, const char* path, char* err, size_t err_sz)
{
    FILE* f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        sprintf_s(err, err_sz, "Could not create %s.", path);
        return FALSE;
    }
    BOOL ok = http_get(url, f, NULL, err, err_sz);
    if (fclose(f) != 0 && ok) {
        sprintf_s(err, err_sz, "Could not finish writing %s.", path);
        ok = FALSE;
    }
    if (!ok) DeleteFileA(path);
    return ok;
}

BOOL update_fetch_files(const Release* r, const char* dir,
                        void (*progress)(void* ctx, const char* text), void* ctx,
                        char* err, size_t err_sz)
{
    int total = 0, done = 0;
    for (int i = 0; i < r->asset_count; ++i)
        if (update_asset_is_mod_file(&r->assets[i])) ++total;
    for (int i = 0; i < r->asset_count; ++i) {
        const ReleaseAsset* a = &r->assets[i];
        if (!update_asset_is_mod_file(a)) continue;
        // A name that could climb out of the folder is not a file of ours.
        if (strchr(a->name, '\\') || strchr(a->name, '/') ||strstr(a->name, "..") || strchr(a->name, ':')) {
            sprintf_s(err, err_sz, "The release has a file with a strange name: %s", a->name);
            return FALSE;
        }
        if (progress) {
            char text[256];
            sprintf_s(text, sizeof text, "Downloading %s, %d of %d...", a->name, ++done, total);
            progress(ctx, text);
        }
        char path[MAX_PATH];
        sprintf_s(path, sizeof path, "%s\\%s", dir, a->name);
        char why[512];
        if (!update_download(a->url, path, why, sizeof why)) {
            sprintf_s(err, err_sz, "%s: %s", a->name, why);
            return FALSE;
        }
    }
    return TRUE;
}
