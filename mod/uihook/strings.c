// The mod's own lines, in the game's language (strings.h has the why and the
// file format).

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strings.h"

enum { KIND_TEXT, KIND_FORMAT, KIND_PLURAL };

typedef struct {
    const char* key;
    const char* text;
    int         kind;
} StrDef;

// A plural line's forms are kept in one string, split by FORM_SEP; a
// translation writes | between them (and \| for a | of its own).
#define FORM_SEP '\x1F'

static const StrDef k_defs[STR_COUNT] = {
#define S(key, text) { #key, text, KIND_TEXT },
#define F(key, text) { #key, text, KIND_FORMAT },
#define P(key, one, other) { #key, one "\x1F" other, KIND_PLURAL },
#include "strings.def"
#undef S
#undef F
#undef P
};

// The current table. A load builds a new one and swaps the pointer, so a
// reader on another thread sees the old table or the new one whole; the old
// one is not freed, since a reader may still hold a line from it (a load
// happens once or twice a run).
static const char* const* volatile g_table;
static char g_lang[16] = "INT";

// The @ phrases: pairs, swapped whole with the table.
typedef struct { const char* from; const char* to; } Phrase;
typedef struct { int n; Phrase p[1]; } Phrases;
static const Phrases* volatile g_phrases;

const char* strings_phrase(const char* english)
{
    const Phrases* ph = g_phrases;
    if (!english || !ph) return english;
    for (int i = 0; i < ph->n; i++)
        if (!_stricmp(ph->p[i].from, english)) return ph->p[i].to;
    return english;
}

const char* T(StrId id)
{
    if ((unsigned)id >= STR_COUNT) return "";
    const char* const* t = g_table;
    return t && t[id] ? t[id] : k_defs[id].text;
}

const char* strings_lang(void) { return g_lang; }

int strings_is_english(void) { return g_table == NULL; }

// Back off a cut that fell inside a UTF-8 character, so a translated line cut
// to fit never ends in half a letter.
static void cut_utf8(char* s, size_t len)
{
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    s[len] = 0;
}

static int vfmt(char* out, size_t out_sz, const char* fmt, va_list ap)
{
    if (!out || !out_sz) return 0;
    // The positional printf (_vsprintf_p) takes %n$ and plain formats alike,
    // but calls the invalid-parameter handler on a buffer too small rather
    // than cutting; so it is measured first, and a long line goes through a
    // buffer of its own.
    va_list ap2;
    va_copy(ap2, ap);
    int need = _vscprintf_p(fmt, ap2);
    va_end(ap2);
    if (need < 0) { out[0] = 0; return 0; }
    if ((size_t)need < out_sz) return _vsprintf_p(out, out_sz, fmt, ap);
    char* all = (char*)malloc((size_t)need + 1);
    if (!all) { out[0] = 0; return 0; }
    _vsprintf_p(all, (size_t)need + 1, fmt, ap);
    memcpy(out, all, out_sz - 1);
    free(all);
    cut_utf8(out, out_sz - 1);
    return (int)strlen(out);
}

int tfmt(char* out, size_t out_sz, StrId id, ...)
{
    va_list ap;
    va_start(ap, id);
    int n = vfmt(out, out_sz, T(id), ap);
    va_end(ap);
    return n;
}

int tfmt_cat(char* out, size_t out_sz, size_t* len, StrId id, ...)
{
    if (!out || !len || *len >= out_sz) return 0;
    va_list ap;
    va_start(ap, id);
    int n = vfmt(out + *len, out_sz - *len, T(id), ap);
    va_end(ap);
    *len += (size_t)n;
    return n;
}

// ---- plurals ---------------------------------------------------------------

// How many forms a language's plurals have, and which one `n` takes (CLDR's
// cardinal rules, for whole numbers).
static int plural_forms(const char* lang)
{
    if (!_stricmp(lang, "RUS") || !_stricmp(lang, "POL")) return 3;
    if (!_stricmp(lang, "JPN") || !_stricmp(lang, "KOR") || !_stricmp(lang, "CHT")) return 1;
    return 2;
}

static int plural_form(const char* lang, int n)
{
    unsigned a = (unsigned)(n < 0 ? -n : n);
    if (!_stricmp(lang, "RUS")) {
        if (a % 10 == 1 && a % 100 != 11) return 0;
        if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) return 1;
        return 2;
    }
    if (!_stricmp(lang, "POL")) {
        if (a == 1) return 0;
        if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) return 1;
        return 2;
    }
    if (plural_forms(lang) == 1) return 0;
    if (!_stricmp(lang, "FRA")) return a <= 1 ? 0 : 1;
    return a == 1 ? 0 : 1;
}

// Form `k` of a joined plural line, into `buf`; the last form there is when
// the line has fewer.
static const char* plural_pick(const char* joined, int k, char* buf, size_t buf_sz)
{
    const char* p = joined;
    for (int i = 0; i < k; i++) {
        const char* sep = strchr(p, FORM_SEP);
        if (!sep) break;
        p = sep + 1;
    }
    const char* end = strchr(p, FORM_SEP);
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= buf_sz) len = buf_sz - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    return buf;
}

static int vpfmt(char* out, size_t out_sz, StrId id, int n, va_list ap)
{
    if ((unsigned)id >= STR_COUNT) { if (out && out_sz) out[0] = 0; return 0; }
    const char* const* t = g_table;
    int translated = t && t[id];
    const char* joined = translated ? t[id] : k_defs[id].text;
    // English's own rule for the English forms, the language's for its own.
    int k = plural_form(translated ? g_lang : "INT", n);
    char form[1024];
    return vfmt(out, out_sz, plural_pick(joined, k, form, sizeof form), ap);
}

int tpfmt(char* out, size_t out_sz, StrId id, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int w = vpfmt(out, out_sz, id, n, ap);
    va_end(ap);
    return w;
}

int tpfmt_cat(char* out, size_t out_sz, size_t* len, StrId id, int n, ...)
{
    if (!out || !len || *len >= out_sz) return 0;
    va_list ap;
    va_start(ap, n);
    int w = vpfmt(out + *len, out_sz - *len, id, n, ap);
    va_end(ap);
    *len += (size_t)w;
    return w;
}

// ---- format checking -------------------------------------------------------

#define MAX_ARGS 16

// The arguments a format takes, as one letter each in the order they are
// passed: i an int-sized integer, L a 64-bit one, f a double, s a narrow
// string, w a wide one, p a pointer. Returns the count, or -1 with the reason.
// A positional format (%2$s) is mapped back to argument order, and must not
// mix with plain specifiers.
static int format_args(const char* f, char* types, char* why, size_t why_sz)
{
    int n = 0, positional = -1;
    memset(types, 0, MAX_ARGS);
    for (const char* p = f; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        if (!*p) { _snprintf_s(why, why_sz, _TRUNCATE, "a lone %% at the end"); return -1; }

        int pos = 0;
        const char* q = p;
        while (*q >= '0' && *q <= '9') pos = pos * 10 + (*q++ - '0');
        int this_positional = *q == '$' && q > p;
        if (this_positional) p = q + 1; else pos = 0;
        if (positional >= 0 && positional != this_positional) {
            _snprintf_s(why, why_sz, _TRUNCATE, "mixes %%n$ with plain placeholders");
            return -1;
        }
        positional = this_positional;

        while (*p && strchr("-+ #0", *p)) p++;
        int stars = 0;
        if (*p == '*') { stars++; p++; } else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') { stars++; p++; } else while (*p >= '0' && *p <= '9') p++;
        }
        if (stars && positional) {
            _snprintf_s(why, why_sz, _TRUNCATE, "a * width in a %%n$ placeholder");
            return -1;
        }

        int wide = 0, big = 0, narrow = 0;
        for (;;) {
            if (*p == 'l') { wide = 1; if (p[1] == 'l') { big = 1; p++; } p++; }
            else if (*p == 'h') { narrow = 1; p++; if (*p == 'h') p++; }
            else if (*p == 'w') { wide = 1; p++; }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { big = 1; p += 3; }
            else if (*p == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            else if (*p == 'z' || *p == 't' || *p == 'j' || *p == 'L' || *p == 'I') p++;
            else break;
        }

        char t;
        switch (*p) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            t = big ? 'L' : 'i'; break;
        case 'c': case 'C': t = 'i'; break;
        case 's': t = wide && !narrow ? 'w' : 's'; break;
        case 'S': t = narrow ? 's' : 'w'; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            t = 'f'; break;
        case 'p': t = 'p'; break;
        default:
            _snprintf_s(why, why_sz, _TRUNCATE, "an unknown placeholder %%%c",
                        *p ? *p : '?');
            return -1;
        }

        if (positional) {
            if (pos < 1 || pos > MAX_ARGS) {
                _snprintf_s(why, why_sz, _TRUNCATE, "placeholder %d$ out of range", pos);
                return -1;
            }
            if (types[pos - 1] && types[pos - 1] != t) {
                _snprintf_s(why, why_sz, _TRUNCATE, "%d$ used as two types", pos);
                return -1;
            }
            types[pos - 1] = t;
            if (pos > n) n = pos;
        } else {
            for (int k = 0; k < stars + 1; k++) {
                if (n >= MAX_ARGS) {
                    _snprintf_s(why, why_sz, _TRUNCATE, "more than %d placeholders", MAX_ARGS);
                    return -1;
                }
                types[n++] = k < stars ? 'i' : t;
            }
        }
    }
    for (int k = 0; k < n; k++)
        if (!types[k]) {
            _snprintf_s(why, why_sz, _TRUNCATE, "placeholder %d$ is never used", k + 1);
            return -1;
        }
    return n;
}

int strings_formats_match(const char* english, const char* other, char* why, size_t why_sz)
{
    char a[MAX_ARGS], b[MAX_ARGS];
    char sub[96] = "";
    int na = format_args(english, a, sub, sizeof sub);
    if (na < 0) { _snprintf_s(why, why_sz, _TRUNCATE, "the English %s", sub); return 0; }
    int nb = format_args(other, b, sub, sizeof sub);
    if (nb < 0) { _snprintf_s(why, why_sz, _TRUNCATE, "%s", sub); return 0; }
    if (nb > na) {
        _snprintf_s(why, why_sz, _TRUNCATE, "%d placeholders where the English has %d", nb, na);
        return 0;
    }
    for (int k = 0; k < nb; k++)
        if (a[k] != b[k]) {
            _snprintf_s(why, why_sz, _TRUNCATE, "placeholder %d is %c where the English has %c",
                        k + 1, b[k], a[k]);
            return 0;
        }
    return 1;
}

// ---- the file --------------------------------------------------------------

static int find_key(const char* key, size_t len)
{
    for (int i = 0; i < STR_COUNT; i++)
        if (strlen(k_defs[i].key) == len && !memcmp(k_defs[i].key, key, len)) return i;
    return -1;
}

// The quoted value at `p`, unescaped in place (\" \\ \n, and \| for a | that
// does not split a plural's forms); NULL if it is not one quoted string.
// `plural` turns each bare | into FORM_SEP.
static char* unquote(char* p, int plural)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    char* start = ++p;
    char* w = start;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *w++ = *p == 'n' ? '\n' : *p;
        } else {
            *w++ = plural && *p == '|' ? FORM_SEP : *p;
        }
    }
    if (*p != '"') return NULL;
    *w = 0;
    return start;
}

void strings_load(const char* dll_dir, const char* code, char* why, size_t why_sz)
{
    if (why && why_sz) why[0] = 0;
    if (!code || !*code || !_stricmp(code, "INT")) {
        g_table = NULL;
        g_phrases = NULL;
        strcpy_s(g_lang, sizeof g_lang, "INT");
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "English (the game is INT)");
        return;
    }

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%slang\\%s.txt", dll_dir ? dll_dir : "", code);
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") || !f) {
        g_table = NULL;
        g_phrases = NULL;
        strcpy_s(g_lang, sizeof g_lang, code);
        if (why) _snprintf_s(why, why_sz, _TRUNCATE,
                             "the game is %s, but there is no %s -- English", code, path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)(size > 0 ? size : 0) + 1);
    const char** table = (const char**)calloc(STR_COUNT, sizeof *table);
    if (!buf || !table) {
        fclose(f);
        free(buf);
        free(table);
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "out of memory reading %s", path);
        return;
    }
    size_t got = fread(buf, 1, (size_t)(size > 0 ? size : 0), f);
    fclose(f);
    buf[got] = 0;

    // Room for an @ phrase on every line, which is more than there will be.
    int lines = 1;
    for (size_t i = 0; i < got; i++) lines += buf[i] == '\n';
    Phrases* phrases = (Phrases*)calloc(1, sizeof(Phrases) + (size_t)lines * sizeof(Phrase));
    if (!phrases) {
        free(buf);
        free(table);
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "out of memory reading %s", path);
        return;
    }

    // The buffer is kept: the table points into it.
    char* p = buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    int taken = 0, refused = 0, unknown = 0, line_no = 0;
    char first_problem[160] = "";
    while (*p) {
        char* line = p;
        char* nl = strchr(p, '\n');
        if (nl) { *nl = 0; p = nl + 1; } else p += strlen(p);
        line_no++;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#' || *line == ';') continue;

        char* eq = strchr(line, '=');
        if (!eq) {
            refused++;
            if (!first_problem[0])
                _snprintf_s(first_problem, sizeof first_problem, _TRUNCATE,
                            "line %d has no =", line_no);
            continue;
        }
        char* kend = eq;
        while (kend > line && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
        if (*line == '@') {
            // A made-up name's translation (strings_phrase).
            char* value = unquote(eq + 1, 0);
            *kend = 0;
            if (value && line[1]) {
                phrases->p[phrases->n].from = line + 1;
                phrases->p[phrases->n].to = value;
                phrases->n++;
            } else {
                refused++;
                if (!first_problem[0])
                    _snprintf_s(first_problem, sizeof first_problem, _TRUNCATE,
                                "line %d (an @ phrase) is not one quoted string", line_no);
            }
            continue;
        }
        int id = find_key(line, (size_t)(kend - line));
        if (id < 0) {
            unknown++;
            continue;
        }
        char* value = unquote(eq + 1, k_defs[id].kind == KIND_PLURAL);
        if (!value) {
            refused++;
            if (!first_problem[0])
                _snprintf_s(first_problem, sizeof first_problem, _TRUNCATE,
                            "line %d (%s) is not one quoted string", line_no, k_defs[id].key);
            continue;
        }
        char fwhy[96] = "";
        int ok = 1;
        if (k_defs[id].kind == KIND_FORMAT) {
            ok = strings_formats_match(k_defs[id].text, value, fwhy, sizeof fwhy);
        } else if (k_defs[id].kind == KIND_PLURAL) {
            // Every form against the English "other", which names every
            // argument; and no more forms than the language has.
            char other[1024], form[1024];
            plural_pick(k_defs[id].text, 1, other, sizeof other);
            int forms = 1;
            for (const char* q = value; *q; q++) forms += *q == FORM_SEP;
            if (forms > plural_forms(code)) {
                ok = 0;
                _snprintf_s(fwhy, sizeof fwhy, _TRUNCATE, "%d forms where %s has %d",
                            forms, code, plural_forms(code));
            }
            for (int k = 0; ok && k < forms; k++)
                ok = strings_formats_match(other, plural_pick(value, k, form, sizeof form),
                                           fwhy, sizeof fwhy);
        }
        if (!ok) {
            refused++;
            if (!first_problem[0])
                _snprintf_s(first_problem, sizeof first_problem, _TRUNCATE,
                            "%s kept English: %s", k_defs[id].key, fwhy);
            continue;
        }
        table[id] = value;
        taken++;
    }

    g_table = table;
    g_phrases = phrases;
    strcpy_s(g_lang, sizeof g_lang, code);
    if (why)
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "%s: %d of %d lines translated, %d named things, %d refused, "
                    "%d unknown keys%s%s",
                    path, taken, (int)STR_COUNT, phrases->n, refused, unknown,
                    first_problem[0] ? "; first problem: " : "", first_problem);
}

int strings_write_template(const char* path)
{
    FILE* f = NULL;
    if (fopen_s(&f, path, "wb") || !f) return 0;
    fputs("# Squadsight's own lines. Copy this file to lang\\<CODE>.txt beside\r\n"
          "# xcom_uihook.dll, where CODE is the game's language (DEU, FRA, ITA,\r\n"
          "# ESN, POL, RUS, JPN, KOR, CHT), and translate the quoted text. Keep\r\n"
          "# every %s / %d of a line; %2$s takes the second one, to reorder them.\r\n"
          "# A line with | between forms changes with a count: give your language's\r\n"
          "# forms (one|other; RUS and POL one|few|many; JPN, KOR, CHT just one).\r\n"
          "# A line left out stays English. UTF-8.\r\n"
          "#\r\n"
          "# The scanner names objects from their models (\"Wooden crate stack\"),\r\n"
          "# which have no key. Give any of those as the English with an @ in front:\r\n"
          "#     @Wooden crate stack = \"Holzkistenstapel\"\r\n"
          "# The names met in a mission are in xcom_uihook.log.\r\n\r\n", f);
    for (int i = 0; i < STR_COUNT; i++) {
        fprintf(f, "%s = \"", k_defs[i].key);
        for (const char* s = k_defs[i].text; *s; s++) {
            if (*s == '"' || *s == '\\' || *s == '|') { fputc('\\', f); fputc(*s, f); }
            else if (*s == FORM_SEP) fputc('|', f);
            else if (*s == '\n') fputs("\\n", f);
            else fputc(*s, f);
        }
        fputs("\"\r\n", f);
    }
    fclose(f);
    return 1;
}

// ---- text the game drew ----------------------------------------------------

#define TEXT_WIDE 4096

// UTF-8 to UTF-16 into `w` (TEXT_WIDE); the length, 0 on failure.
static int widen(const char* in, wchar_t* w)
{
    if (!in) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, w, TEXT_WIDE);
    if (n <= 0) { w[0] = 0; return 0; }
    return n - 1;
}

static void narrow(const wchar_t* w, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_sz, NULL, NULL);
    if (n <= 0) {
        // Too long for `out`: convert all of it and cut at a whole character.
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char* all = need > 0 ? (char*)malloc((size_t)need) : NULL;
        if (!all) { out[0] = 0; return; }
        WideCharToMultiByte(CP_UTF8, 0, w, -1, all, need, NULL, NULL);
        memcpy(out, all, out_sz - 1);
        free(all);
        cut_utf8(out, out_sz - 1);
    }
}

// Per thread, not on the stack: callers can be deep in the game's script VM.
static __declspec(thread) wchar_t t_a[TEXT_WIDE], t_b[TEXT_WIDE];

const char* text_find_ci(const char* hay, const char* needle)
{
    if (!hay || !needle || !*needle) return NULL;
    int nh = widen(hay, t_a), nn = widen(needle, t_b);
    if (!nh || !nn) return NULL;
    CharLowerBuffW(t_a, (DWORD)nh);
    CharLowerBuffW(t_b, (DWORD)nn);
    const wchar_t* at = wcsstr(t_a, t_b);
    if (!at) return NULL;
    // Back to a byte offset in `hay`: the UTF-8 length of what came before.
    int bytes = (int)(at - t_a) ? WideCharToMultiByte(CP_UTF8, 0, t_a, (int)(at - t_a),
                                                     NULL, 0, NULL, NULL) : 0;
    return hay + bytes;
}

int text_equal_ci(const char* a, const char* b)
{
    if (!a || !b) return 0;
    int na = widen(a, t_a), nb = widen(b, t_b);
    if (na != nb) return 0;
    CharLowerBuffW(t_a, (DWORD)na);
    CharLowerBuffW(t_b, (DWORD)nb);
    return wcscmp(t_a, t_b) == 0;
}

// Text too long for the wide buffer, or not UTF-8, is passed on as it came
// rather than lost.
static int pass_through(const char* in, int n, char* out, size_t out_sz)
{
    if (n || !in || !*in) return 0;
    strncpy_s(out, out_sz, in, _TRUNCATE);
    return 1;
}

void text_sentence_case(const char* in, char* out, size_t out_sz)
{
    int n = widen(in, t_a);
    if (pass_through(in, n, out, out_sz)) return;
    if (n) {
        CharLowerBuffW(t_a, (DWORD)n);
        CharUpperBuffW(t_a, 1);
    }
    narrow(t_a, out, out_sz);
}

void text_title_case(const char* in, char* out, size_t out_sz)
{
    int n = widen(in, t_a);
    if (pass_through(in, n, out, out_sz)) return;
    if (n) CharLowerBuffW(t_a, (DWORD)n);
    for (int i = 0; i < n; i++)
        if (IsCharAlphaW(t_a[i]) && (i == 0 || !IsCharAlphaNumericW(t_a[i - 1])) &&
            (i == 0 || t_a[i - 1] != '\''))
            CharUpperBuffW(t_a + i, 1);
    narrow(t_a, out, out_sz);
}

void text_lower(const char* in, char* out, size_t out_sz)
{
    int n = widen(in, t_a);
    if (pass_through(in, n, out, out_sz)) return;
    if (n) CharLowerBuffW(t_a, (DWORD)n);
    narrow(t_a, out, out_sz);
}

int text_has_lower(const char* s)
{
    int n = widen(s, t_a);
    for (int i = 0; i < n; i++)
        if (IsCharLowerW(t_a[i])) return 1;
    return 0;
}

int text_ends_sentence(const char* s)
{
    if (!s || !*s) return 0;
    int n = widen(s, t_a);
    while (n > 0 && (t_a[n - 1] == ' ' || t_a[n - 1] == '"' || t_a[n - 1] == 0x201D)) n--;
    if (!n) return 0;
    wchar_t c = t_a[n - 1];
    return c == '.' || c == '!' || c == '?' || c == 0x3002 || c == 0xFF01 ||
           c == 0xFF1F || c == 0x2026 || c == 0xFF0E;
}

void text_end_sentence(char* s, size_t s_sz)
{
    if (!s || !*s || text_ends_sentence(s)) return;
    strncat_s(s, s_sz, T(TXT_STOP), _TRUNCATE);
}

// ---- the game's language ---------------------------------------------------

static int code_readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

const unsigned short* strings_language_buffer(const unsigned char* exec)
{
    // The thunk is P_FINISH, then `call getter`, then the FString assignment;
    // the getter is `mov eax, imm32; ret`. The first call whose target has
    // that shape is the one.
    if (!exec || !code_readable(exec, 0x40)) return NULL;
    // P_FINISH's own call is `call [imm]` (FF 15), not E8, so the first
    // E8 that lands on a `mov eax, imm32; ret` is the getter.
    for (int i = 0; i + 5 <= 0x40; i++) {
        if (exec[i] != 0xE8) continue;
        const unsigned char* target = exec + i + 5 + *(const int*)(exec + i + 1);
        if (!code_readable(target, 6)) continue;
        if (target[0] == 0xB8 && target[5] == 0xC3)
            return *(const unsigned short* const*)(target + 1);
    }
    return NULL;
}
