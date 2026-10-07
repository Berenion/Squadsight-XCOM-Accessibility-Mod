// Offline checks for the update check's parsing: versions, and the fields of
// a GitHub releases/latest answer.  Prints PASS.

#include "update.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } } while (0)

static int cmp(const char* a, const char* b)
{
    int va[3], vb[3];
    update_parse_version(a, va);
    update_parse_version(b, vb);
    return update_compare(va, vb);
}

// Shaped like the real answer: the asset's url (the API's) comes before
// browser_download_url, and a source tarball's address precedes the zip's.
static const char RELEASE[] =
    "{\n"
    "  \"url\": \"https://api.github.com/repos/x/y/releases/1\",\n"
    "  \"html_url\": \"https://github.com/x/y/releases/tag/v0.9.1\",\n"
    "  \"tag_name\": \"v0.9.1\",\n"
    "  \"name\": \"Squadsight 0.9.1\",\n"
    "  \"assets\": [\n"
    "    { \"url\": \"https://api.github.com/repos/x/y/releases/assets/7\",\n"
    "      \"name\": \"notes.txt\",\n"
    "      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.1/notes.txt\" },\n"
    "    { \"url\": \"https://api.github.com/repos/x/y/releases/assets/8\",\n"
    "      \"name\": \"Squadsight-0.9.1.zip\",\n"
    "      \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.9.1/Squadsight-0.9.1.zip\" }\n"
    "  ],\n"
    "  \"body\": \"## Changes\\r\\n- Say \\\"hello\\\" \\u00e9\\ud83d\\ude00\\r\\n- Path C:\\\\x\"\n"
    "}\n";

int main(void)
{
    int v[3];

    CHECK(update_parse_version("v0.9.1", v) && v[0] == 0 && v[1] == 9 && v[2] == 1);
    CHECK(update_parse_version("1.2", v) && v[0] == 1 && v[1] == 2 && v[2] == 0);
    CHECK(update_parse_version("V10.0.3-beta", v) && v[0] == 10 && v[2] == 3);
    CHECK(!update_parse_version("latest", v));

    CHECK(cmp("0.9.1", "0.9.0") > 0);
    CHECK(cmp("v0.9.0", "0.9.0") == 0);
    CHECK(cmp("0.10.0", "0.9.9") > 0);          // numbers, not text
    CHECK(cmp("1.0", "0.99.99") > 0);
    CHECK(cmp("0.8.5", "0.9.0") < 0);

    // The three numbers the resource compiler uses must say what the string says.
    CHECK(update_parse_version(MOD_VERSION, v) && v[0] == MOD_VERSION_MAJOR &&
          v[1] == MOD_VERSION_MINOR && v[2] == MOD_VERSION_PATCH);

    char s[512];
    CHECK(update_json_string(RELEASE, "tag_name", s, sizeof s) && !strcmp(s, "v0.9.1"));
    CHECK(!update_json_string(RELEASE, "missing", s, sizeof s));
    CHECK(!update_json_string("{\"body\": null}", "body", s, sizeof s));
    CHECK(update_json_string(RELEASE, "body", s, sizeof s) &&
          !strcmp(s, "## Changes\n- Say \"hello\" \xC3\xA9\xF0\x9F\x98\x80\n- Path C:\\x"));

    CHECK(update_zip_url(RELEASE, s, sizeof s) &&
          !strcmp(s, "https://github.com/x/y/releases/download/v0.9.1/Squadsight-0.9.1.zip"));
    CHECK(!update_zip_url("{\"assets\": []}", s, sizeof s));

    Release r;
    char err[256];
    CHECK(update_parse_release(RELEASE, &r, err, sizeof err));
    CHECK(r.version[0] == 0 && r.version[1] == 9 && r.version[2] == 1);
    CHECK(!strcmp(r.page_url, "https://github.com/x/y/releases/tag/v0.9.1"));

    // What a private or missing repository answers.
    CHECK(!update_parse_release("{\"message\": \"Not Found\", \"status\": \"404\"}",
                                &r, err, sizeof err) &&
          !strcmp(err, "GitHub answered: Not Found"));
    CHECK(!update_parse_release("{\"tag_name\": \"v1.0.0\", \"assets\": []}", &r, err, sizeof err) &&
          strstr(err, "no zip"));

    if (g_fail) { printf("%d failed\n", g_fail); return 1; }
    printf("PASS\n");
    return 0;
}
