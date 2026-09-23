// The headquarters' facility menu, kept from its update stream. See hq.h.

#include "hq.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    int  known;
    char name[64];
    int  unavailable;
    int  alert;
} Facility;

static Facility g_fac[HQ_FACILITIES];

void hq_facility_reset(void)
{
    memset(g_fac, 0, sizeof g_fac);
}

void hq_facility_set(int id, const char* name, int unavailable, int alert)
{
    if (id < 0 || id >= HQ_FACILITIES || !name || !*name) return;
    Facility* f = &g_fac[id];
    f->known = 1;
    strncpy_s(f->name, sizeof f->name, name, _TRUNCATE);
    f->unavailable = unavailable != 0;
    f->alert = alert != 0;
}

// GetHTMLColoredText's state 1 (disabled) is grey.
static int is_grey(const char* raw)
{
    return strstr(raw, "808080") != NULL;
}

// The name without its <font> wrapper.
static void strip_tags(const char* in, char* out, size_t out_sz)
{
    size_t w = 0;
    int depth = 0;
    for (const char* r = in; *r && w + 1 < out_sz; r++) {
        if (*r == '<') { depth++; continue; }
        if (*r == '>') { if (depth) depth--; continue; }
        if (!depth) out[w++] = *r;
    }
    out[w] = 0;
}

int hq_facility_feed(const AbarValue* v, int n)
{
    if (!v || n <= 0) return 0;

    // Parsed into a copy first, so a stream that turns out malformed leaves
    // the menu as it was.
    Facility next[HQ_FACILITIES];
    memcpy(next, g_fac, sizeof next);
    int touched = 0;

    int i = 0;
    while (i < n) {
        // A record: null, then the Id.
        if (v[i].type != ABAR_NULL || i + 1 >= n || v[i + 1].type != ABAR_NUMBER)
            return -1;
        int id = (int)v[i + 1].n;
        if (id < 0 || id >= HQ_FACILITIES) return -1;
        i += 2;
        // Then <name, value> pairs until the next record.
        while (i < n && v[i].type == ABAR_STRING) {
            if (i + 1 >= n) return -1;
            const char* fn = v[i].s ? v[i].s : "";
            const AbarValue* val = &v[i + 1];
            if (strcmp(fn, "SetAlert") == 0 && val->type == ABAR_BOOL) {
                next[id].alert = val->b;
            } else if (strcmp(fn, "SetButtonText") == 0 && val->type == ABAR_STRING &&
                       val->s) {
                char name[64];
                strip_tags(val->s, name, sizeof name);
                if (name[0]) {
                    strncpy_s(next[id].name, sizeof next[id].name, name, _TRUNCATE);
                    next[id].known = 1;
                }
                next[id].unavailable = is_grey(val->s);
            }
            // Anything else is a setter this does not read; its value is
            // stepped over all the same.
            i += 2;
            touched |= 1 << id;
        }
    }
    memcpy(g_fac, next, sizeof g_fac);
    return touched;
}

int hq_facility_label(int id, char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    if (id < 0 || id >= HQ_FACILITIES || !g_fac[id].known) return 0;
    const Facility* f = &g_fac[id];
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", f->name,
                f->unavailable ? ", unavailable" : "",
                f->alert ? ", needs attention" : "");
    return 1;
}

const char* hq_pc_icon_label(const char* label)
{
    if (!label || !label[0] || label[1]) return NULL;
    switch (label[0]) {
        case '0': return "Previous soldier";
        case '1': return "Next soldier";
        case '2': return "Hologlobe";
        case '3': return "Accept";
        case '4': return "Back";
        case '5': return "Pause";
        default:  return NULL;
    }
}

static const char* rank_word(const char* label)
{
    static const char* const ranks[] = { "Rookie", "Squaddie", "Corporal", "Sergeant",
                                         "Lieutenant", "Captain", "Major", "Colonel" };
    if (!label) return "";
    if (strncmp(label, "rank", 4) == 0 && label[4] >= '0' && label[4] <= '7' && !label[5])
        return ranks[label[4] - '0'];
    if (strncmp(label, "shiv", 4) == 0) return "SHIV";
    return "";
}

void hq_soldier_row(const char* name, const char* nick, const char* cls,
                    const char* status, const char* rank_label, int disabled,
                    int promotable, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    const char* rank = rank_word(rank_label);
    // A ranked soldier's name already carries the rank, abbreviated: "Sq. Cesar
    // Vargas". The word replaces it -- read aloud, "Sq." is two letters.
    if (*rank && name) {
        const char* sp = strchr(name, ' ');
        if (sp && sp > name && sp[-1] == '.' && sp - name <= 5) name = sp + 1;
    }
    char who[160];
    if (nick && *nick)
        _snprintf_s(who, sizeof who, _TRUNCATE, "%s%s%s '%s'", rank, *rank ? " " : "",
                    name ? name : "", nick);
    else
        _snprintf_s(who, sizeof who, _TRUNCATE, "%s%s%s", rank, *rank ? " " : "",
                    name ? name : "");
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s%s%s%s", who,
                cls && *cls ? ", " : "", cls ? cls : "",
                status && *status ? ", " : "", status ? status : "",
                promotable ? ", promotion" : "",
                disabled ? ", unavailable" : "");
}

int hq_soldier_count(const char* label, char* out, size_t out_sz)
{
    int have = 0, of = 0;
    char tail;
    if (!label || sscanf_s(label, "%d/%d%c", &have, &of, &tail, 1) != 2) return 0;
    _snprintf_s(out, out_sz, _TRUNCATE, "%d of %d soldiers available", have, of);
    return 1;
}

void hq_card_clean(char* s)
{
    if (!s) return;
    char* w = s;
    for (const char* r = s; *r; ) {
        // U+00B7, the middle dot, is C2 B7 in UTF-8.
        if ((unsigned char)r[0] == 0xC2 && (unsigned char)r[1] == 0xB7) {
            r += 2;
            while (*r == ' ') r++;
            // Trim what came before, then break the sentence -- unless this
            // is the first thing, or a sentence already ended there.
            while (w > s && w[-1] == ' ') w--;
            if (w > s && w[-1] != '.' && w[-1] != ':') *w++ = '.';
            if (w > s) *w++ = ' ';
            continue;
        }
        *w++ = *r++;
    }
    *w = 0;
}
