// Offline checks of the string table (strings.c): the English formats parse,
// a translation's placeholders are checked before it is trusted, a file loads
// over the English and a bad line stays English.
//
//   test_strings.exe                      the checks; prints PASS
//   test_strings.exe template <file>      writes the translators' template
//   test_strings.exe lang <game exe>      finds GetLanguage's buffer in the exe

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "strings.h"
#include "natives.h"

static int g_fail;

static void check(int ok, const char* what)
{
    if (!ok) { printf("FAIL: %s\n", what); g_fail = 1; }
}

static void match(const char* en, const char* other, int want)
{
    char why[128] = "";
    int got = strings_formats_match(en, other, why, sizeof why);
    char what[256];
    _snprintf_s(what, sizeof what, _TRUNCATE, "\"%s\" vs \"%s\" -> %d (%s)", en, other, got, why);
    check(got == want, what);
}

static int find_language(const char* exe)
{
    HMODULE mod = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) { printf("LoadLibraryEx failed (%lu)\n", GetLastError()); return 1; }
    static NativeEntry tbl[8192];
    int n = natives_scan(mod, tbl, 8192);
    void* exec = natives_find(tbl, n, "UObjectexecGetLanguage");
    printf("natives %d, UObjectexecGetLanguage at rva %p\n", n,
           exec ? (void*)((char*)exec - (char*)mod) : NULL);
    const unsigned short* buf = strings_language_buffer((const unsigned char*)exec);
    // The image is mapped without its relocations applied when it lands
    // elsewhere, so the address is the one the exe was linked for.
    printf("language buffer at %p (linked address)\n", (const void*)buf);
    printf(buf ? "PASS\n" : "FAIL\n");
    return buf ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "template")) {
        int ok = strings_write_template(argv[2]);
        printf(ok ? "wrote %s\n" : "could not write %s\n", argv[2]);
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "lang")) return find_language(argv[2]);

    // Every English format parses and matches itself; a plural's forms are
    // each checked against its last (T gives them joined by \x1F).
    char why[128];
    for (int i = 0; i < STR_COUNT; i++) {
        const char* all = T((StrId)i);
        const char* other = strrchr(all, '\x1F');
        other = other ? other + 1 : all;
        for (const char* form = all; form; ) {
            const char* sep = strchr(form, '\x1F');
            char one[512];
            size_t len = sep ? (size_t)(sep - form) : strlen(form);
            _snprintf_s(one, sizeof one, _TRUNCATE, "%.*s", (int)len, form);
            if (!strings_formats_match(other, one, why, sizeof why)) {
                char what[256];
                _snprintf_s(what, sizeof what, _TRUNCATE, "line %d (\"%s\"): %s", i, one, why);
                check(0, what);
            }
            form = sep ? sep + 1 : NULL;
        }
    }

    match("%s, %d of %d", "%s, %d von %d", 1);
    match("%s, %d of %d", "%2$d von %3$d: %1$s", 1);
    match("%s, %d of %d", "%d of %d, %s", 0);           // types moved, not positioned
    match("%s, %d of %d", "%1$s, %2$d", 1);             // trailing ones may go
    match("%s, %d of %d", "%1$s, %3$d", 0);             // not one in the middle
    match("%s, %d of %d", "%s, %d of %d %d", 0);        // nor one more
    match("%s, %d of %d", "%1$s, %2$d %d", 0);          // mixed
    match("%s took %d", "%s nahm %s", 0);
    match("%d%%", "%d %%", 1);
    match("%s", "%n", 0);
    match("%.*s", "%.*s", 1);
    match("%.1f m", "%.1f m", 1);
    match("%.1f m", "%1$.1f m", 1);
    match("%lu", "%u", 1);
    match("plain", "%s", 0);

    // Formatting, positional or not, and cut to fit at a whole character.
    char out[64];
    tfmt(out, sizeof out, LEARN_ITEM_POS, "Wall sound", 3, 12);
    check(!strcmp(out, "Wall sound. 3 of 12"), out);
    char tiny[8];
    tfmt(tiny, sizeof tiny, LEARN_ITEM_POS, "Wall sound", 3, 12);
    check(!strcmp(tiny, "Wall so"), tiny);
    size_t len = 0;
    out[0] = 0;
    tpfmt_cat(out, sizeof out, &len, TXT_TILES, 4, 4);
    tpfmt_cat(out, sizeof out, &len, TXT_TILES, 1, 1);
    check(!strcmp(out, "4 tiles1 tile"), out);

    // The game's own text in other languages: case and sentence ends are not
    // ASCII. (UTF-8 spelt out, so the source stays plain ASCII.)
    char t[128];
    text_sentence_case("\xD0\x90\xD0\x9A\xD0\xA2\xD0\x98\xD0\x92\xD0\x9D\xD0\x9E\xD0\xA1\xD0\xA2\xD0\xAC", t, sizeof t);
    check(!strcmp(t, "\xD0\x90\xD0\xBA\xD1\x82\xD0\xB8\xD0\xB2\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C"),
          "Russian capitals to sentence case");                   // AKTIVNOST' -> Aktivnost'
    text_title_case("H\xC3\x9C" "GEL DECKUNG", t, sizeof t);
    check(!strcmp(t, "H\xC3\xBCgel Deckung"), t);                  // HUeGEL -> Huegel
    text_title_case("HUNKER DOWN", t, sizeof t);
    check(!strcmp(t, "Hunker Down"), t);
    text_title_case("OPPONENT'S TURN", t, sizeof t);
    check(!strcmp(t, "Opponent's Turn"), t);
    check(text_find_ci("6 KRYTYCZNE \xC5\x9A" "CIEG", "krytyczne \xC5\x9B" "cieg") != NULL,
          "Polish, case-insensitive");
    const char* hay = "\xC3\x84" "rger KRITISCH!";
    check(text_find_ci(hay, "kritisch") == hay + 7, "the match is a byte offset into the UTF-8");
    check(text_equal_ci("Leer", "LEER") && !text_equal_ci("Leer", "Lee"), "equal, any case");
    check(text_has_lower("Abc") && !text_has_lower("ABC") && !text_has_lower("123"), "has lower");
    check(text_ends_sentence("Fertig.") && text_ends_sentence("\xE5\xAE\x8C\xE4\xBA\x86\xE3\x80\x82") &&
          !text_ends_sentence("Fertig"), "sentence ends, CJK too");
    strcpy_s(t, sizeof t, "Fertig");
    text_end_sentence(t, sizeof t);
    check(!strcmp(t, "Fertig."), t);

    // A translation over the English: good lines taken, a bad format and an
    // unknown key left alone, a missing line still English.
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    strcat_s(dir, sizeof dir, "squadsight_strings_test\\");
    CreateDirectoryA(dir, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang", dir);
    CreateDirectoryA(path, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang\\DEU.txt", dir);
    FILE* f = NULL;
    fopen_s(&f, path, "wb");
    check(f != NULL, "write the test file");
    if (f) {
        fputs("\xEF\xBB\xBF# test\r\n"
              "LEARN_SILENT = \"Stumm.\"\r\n"
              "LEARN_ITEM_POS = \"%2$d von %3$d: %1$s\"\r\n"
              "COMBAT_HP_LEFT = \"%s Felder\"\r\n"
              "NOT_A_KEY = \"x\"\r\n"
              "TXT_AND   =   \" und \\\"so\\\" \"\r\n"
              "SET_ON = \"Ein\"\n"
              "TXT_TURNS = \"%d Runde|%d Runden\"\n"
              "TXT_TILES = \"%s Feld|%d Felder\"\n"
              "@Wooden crate stack = \"Holzkistenstapel\"\n", f);
        fclose(f);
    }
    char lwhy[512];
    strings_load(dir, "DEU", lwhy, sizeof lwhy);
    printf("load: %s\n", lwhy);
    check(!strcmp(strings_lang(), "DEU"), "language is DEU");
    check(!strcmp(T(LEARN_SILENT), "Stumm."), T(LEARN_SILENT));
    check(!strcmp(T(SET_ON), "Ein"), T(SET_ON));
    check(!strcmp(T(TXT_AND), " und \"so\" "), T(TXT_AND));
    check(!strcmp(T(COMBAT_HP_LEFT), "%d of %d HP left."), "a bad format stays English");
    check(!strcmp(T(LEARN_MENU_CLOSED), "Mod options closed."), "a missing line stays English");
    tfmt(out, sizeof out, LEARN_ITEM_POS, "Wandklang", 3, 12);
    check(!strcmp(out, "3 von 12: Wandklang"), out);
    check(strstr(lwhy, "2 refused") && strstr(lwhy, "1 unknown"), "the load counts what it left");
    check(!strcmp(strings_phrase("wooden crate STACK"), "Holzkistenstapel"), "an @ phrase");
    check(!strcmp(strings_phrase("Car"), "Car"), "a phrase not given is itself");
    tpfmt(out, sizeof out, TXT_TURNS, 1, 1);
    check(!strcmp(out, "1 Runde"), out);
    tpfmt(out, sizeof out, TXT_TURNS, 4, 4);
    check(!strcmp(out, "4 Runden"), out);
    tpfmt(out, sizeof out, TXT_TILES, 4, 4);
    check(!strcmp(out, "4 tiles"), "a bad plural stays English");

    // Russian's three forms, and Polish's, which part at 21 and 22.
    fopen_s(&f, path, "wb");
    if (f) { fputs("TXT_TURNS = \"%d hod|%d hoda|%d hodov\"\n", f); fclose(f); }
    strings_load(dir, "DEU", lwhy, sizeof lwhy);
    check(strstr(lwhy, "1 refused") != NULL, "three forms are too many for DEU");
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang\\RUS.txt", dir);
    fopen_s(&f, path, "wb");
    if (f) { fputs("TXT_TURNS = \"%d hod|%d hoda|%d hodov\"\n", f); fclose(f); }
    strings_load(dir, "RUS", lwhy, sizeof lwhy);
    static const struct { int n; const char* want; } rus[] = {
        { 1, "1 hod" }, { 2, "2 hoda" }, { 5, "5 hodov" }, { 11, "11 hodov" },
        { 21, "21 hod" }, { 22, "22 hoda" }, { 12, "12 hodov" }, { 0, "0 hodov" },
    };
    for (int i = 0; i < 8; i++) {
        tpfmt(out, sizeof out, TXT_TURNS, rus[i].n, rus[i].n);
        check(!strcmp(out, rus[i].want), out);
    }
    DeleteFileA(path);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang\\POL.txt", dir);
    fopen_s(&f, path, "wb");
    if (f) { fputs("TXT_TURNS = \"%d tura|%d tury|%d tur\"\n", f); fclose(f); }
    strings_load(dir, "POL", lwhy, sizeof lwhy);
    tpfmt(out, sizeof out, TXT_TURNS, 21, 21);
    check(!strcmp(out, "21 tur"), out);
    tpfmt(out, sizeof out, TXT_TURNS, 22, 22);
    check(!strcmp(out, "22 tury"), out);
    DeleteFileA(path);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang\\DEU.txt", dir);

    strings_load(dir, "INT", lwhy, sizeof lwhy);
    check(!strcmp(T(LEARN_SILENT), "Silent."), "INT is English again");
    tpfmt(out, sizeof out, TXT_TURNS, 1, 1);
    check(!strcmp(out, "1 turn"), out);
    tpfmt(out, sizeof out, TXT_TURNS, 0, 0);
    check(!strcmp(out, "0 turns"), out);
    strings_load(dir, "FRA", lwhy, sizeof lwhy);
    check(!strcmp(T(LEARN_SILENT), "Silent."), "a language with no file is English");
    printf("load: %s\n", lwhy);

    DeleteFileA(path);
    printf("%d lines\n", (int)STR_COUNT);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail;
}
