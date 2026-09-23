// The headquarters' facility menu, kept from its update stream. See hq.h.

#include "hq.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

static void copy_without_section(char* out, size_t out_sz, const char* in);

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

// ---- the promotion tree ----------------------------------------------------

typedef struct {
    char label[48];
    int  state;              // -1 until sent
    int  rows;               // how many abilities the rank has
    int  chosen[2];
    int  unknown[2];
} PromoCol;

static PromoCol g_promo[HQ_PROMO_COLS];
static char     g_promo_title[96];
static int      g_promo_sel_col = -1, g_promo_sel_row;
static int      g_promo_said_col = -1;

void hq_promo_reset(const char* title)
{
    memset(g_promo, 0, sizeof g_promo);
    for (int c = 0; c < HQ_PROMO_COLS; c++) g_promo[c].state = -1;
    strncpy_s(g_promo_title, sizeof g_promo_title, title ? title : "", _TRUNCATE);
    g_promo_sel_col = -1;
    g_promo_said_col = -1;
}

void hq_promo_icon(int col, int row, const char* icon, int chosen)
{
    if (col < 0 || col >= HQ_PROMO_COLS || row < 0 || row > 1) return;
    PromoCol* c = &g_promo[col];
    if (row + 1 > c->rows) c->rows = row + 1;
    c->chosen[row] = chosen != 0;
    c->unknown[row] = icon && strcmp(icon, "unknown") == 0;
}

void hq_promo_column(int col, const char* label, int state)
{
    if (col < 0 || col >= HQ_PROMO_COLS) return;
    strncpy_s(g_promo[col].label, sizeof g_promo[col].label, label ? label : "", _TRUNCATE);
    g_promo[col].state = state;
}

void hq_promo_select(int col, int row)
{
    g_promo_sel_col = col;
    g_promo_sel_row = row;
}

static const char* promo_state_word(int state)
{
    switch (state) {
        case 0:  return "earned";
        case 1:  return "choose now";
        case 2:  return "to choose after";
        case 3:  return "not reached";
        default: return "";
    }
}

void hq_promo_describe(const char* name, const char* desc, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    int col = g_promo_sel_col, row = g_promo_sel_row;
    const PromoCol* c = (col >= 0 && col < HQ_PROMO_COLS) ? &g_promo[col] : NULL;

    char rank[96] = "";
    if (c && col != g_promo_said_col && c->label[0]) {
        const char* w = promo_state_word(c->state);
        _snprintf_s(rank, sizeof rank, _TRUNCATE, "%s%s%s. ", c->label, *w ? ", " : "", w);
    }
    g_promo_said_col = col;

    char what[160] = "";
    // A rank with one ability shows it whichever side was picked: left
    // selects row 1, which it does not have.
    if (c && c->rows == 1) row = 0;
    int r_ok = c && row >= 0 && row < 2 && row < c->rows;
    _snprintf_s(what, sizeof what, _TRUNCATE, "%s%s%s",
                name && *name ? name : "Unknown",
                r_ok && c->chosen[row] ? ", chosen" : "",
                c && c->rows > 1 ? (row == 0 ? ", right" : ", left") : "");

    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s%s%s",
                g_promo_title, g_promo_title[0] ? ". " : "",
                rank, what, desc && *desc ? ". " : "", desc ? desc : "");
    g_promo_title[0] = 0;       // the title is said once, on arrival
}

void hq_abduction_line(const char* panic_label, int panic,
                       const char* diff_label, const char* diff,
                       const char* reward_label, const char* reward,
                       char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    char r[128];
    copy_without_section(r, sizeof r, reward);     // "\xC2\xA7" "200" -> "200"
    _snprintf_s(out, out_sz, _TRUNCATE, "%s %d of 5. %s %s. %s %s",
                panic_label && *panic_label ? panic_label : "PANIC:", panic,
                diff_label && *diff_label ? diff_label : "DIFFICULTY:", diff ? diff : "",
                reward_label && *reward_label ? reward_label : "REWARD:", r);
}

int hq_summary_factors(const char* raw, char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    if (!raw) return 0;
    size_t used = 0;
    int rows = 0;
    const char* row = raw;
    while (*row) {
        const char* end = strchr(row, ';');
        size_t len = end ? (size_t)(end - row) : strlen(row);
        char f[6][96];
        int nf = 0;
        const char* c = row;
        while (nf < 6) {
            const char* comma = memchr(c, ',', len - (size_t)(c - row));
            size_t flen = comma ? (size_t)(comma - c) : len - (size_t)(c - row);
            if (flen >= sizeof f[0]) flen = sizeof f[0] - 1;
            memcpy(f[nf], c, flen);
            f[nf][flen] = 0;
            nf++;
            if (!comma) break;
            c = comma + 1;
        }
        for (int i = nf; i < 6; i++) f[i][0] = 0;
        if (f[0][0]) {
            const char* result = f[3];
            const char* rating = f[5];
            int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s%s%s%s%s",
                                used ? ". " : "", f[0], *result || *rating ? ": " : "",
                                result, *result && *rating ? ", " : "", rating);
            if (w < 0) break;
            used += (size_t)w;
            rows++;
        }
        if (!end) break;
        row = end + 1;
    }
    return rows;
}

// ---- the base's status -------------------------------------------------------

#define HQ_RESOURCES 8
#define HQ_EVENTS    12

static char g_res[HQ_RESOURCES][64];
static int  g_nres;
static char g_date[64];
typedef struct {
    const void* list;
    char        ev[HQ_EVENTS][96];
    int         n;
} EventList;
static EventList g_evl[2];

static CRITICAL_SECTION g_st_lock;
static INIT_ONCE g_st_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK st_init(PINIT_ONCE o, PVOID p, PVOID* c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_st_lock);
    return TRUE;
}
static void st_lock(void)
{
    InitOnceExecuteOnce(&g_st_once, st_init, NULL, NULL);
    EnterCriticalSection(&g_st_lock);
}
static void st_unlock(void) { LeaveCriticalSection(&g_st_lock); }

// The section sign the game puts before a sum of money, U+00A7 (C2 A7): a
// screen reader says "section". Dropped.
static void copy_without_section(char* out, size_t out_sz, const char* in)
{
    size_t w = 0;
    for (const char* r = in ? in : ""; *r && w + 1 < out_sz; r++) {
        if ((unsigned char)r[0] == 0xC2 && (unsigned char)r[1] == 0xA7) { r++; continue; }
        out[w++] = *r;
    }
    out[w] = 0;
}

void hq_status_resources_clear(void) { st_lock(); g_nres = 0; st_unlock(); }

void hq_status_resource(const char* text)
{
    if (!text || !*text) return;
    st_lock();
    if (g_nres < HQ_RESOURCES)
        copy_without_section(g_res[g_nres++], sizeof g_res[0], text);
    st_unlock();
}

void hq_status_date(const char* day_month, const char* year, const char* hour,
                    const char* minute)
{
    st_lock();
    if (hour && *hour && minute && *minute)
        _snprintf_s(g_date, sizeof g_date, _TRUNCATE, "%s %s, %s:%s%s",
                    day_month ? day_month : "", year ? year : "", hour,
                    strlen(minute) == 1 ? "0" : "", minute);
    else
        _snprintf_s(g_date, sizeof g_date, _TRUNCATE, "%s %s",
                    day_month ? day_month : "", year ? year : "");
    st_unlock();
}

// The list kept for `list`, taking the other slot when it is new.
static EventList* evl_for(const void* list)
{
    for (int i = 0; i < 2; i++) if (g_evl[i].list == list) return &g_evl[i];
    EventList* e = !g_evl[0].list ? &g_evl[0] : !g_evl[1].list ? &g_evl[1]
                 : (g_evl[0].n <= g_evl[1].n ? &g_evl[0] : &g_evl[1]);
    e->list = list;
    e->n = 0;
    return e;
}

void hq_status_events_clear(const void* list)
{
    st_lock();
    evl_for(list)->n = 0;
    st_unlock();
}

void hq_status_event(const void* list, const char* title, const char* unit,
                     const char* count)
{
    if (!title || !*title) return;
    st_lock();
    EventList* e = evl_for(list);
    if (e->n < HQ_EVENTS) {
        char u[32];
        // "Days" with a count of 1 reads "1 day".
        strncpy_s(u, sizeof u, unit ? unit : "", _TRUNCATE);
        for (char* c = u; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        size_t ul = strlen(u);
        if (count && strcmp(count, "1") == 0 && ul > 1 && u[ul - 1] == 's') u[ul - 1] = 0;
        if (count && *count)
            _snprintf_s(e->ev[e->n], sizeof e->ev[0], _TRUNCATE, "%s, %s %s", title, count, u);
        else
            strncpy_s(e->ev[e->n], sizeof e->ev[0], title, _TRUNCATE);
        e->n++;
    }
    st_unlock();
}

// Appends, by hand: _snprintf_s answers -1 on truncation, which added to a
// length would wrap it.
static void put_piece(char* out, size_t out_sz, size_t* used, const char* sep,
                      const char* piece)
{
    if (*used + 1 >= out_sz) return;
    int k = _snprintf_s(out + *used, out_sz - *used, _TRUNCATE, "%s%s", sep, piece);
    *used = k < 0 ? out_sz - 1 : *used + (size_t)k;
}

int hq_status_line(char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    st_lock();
    const EventList* ev = g_evl[0].n >= g_evl[1].n ? &g_evl[0] : &g_evl[1];
    int any = g_date[0] || g_nres || ev->n;
    size_t used = 0;
    if (g_date[0]) put_piece(out, out_sz, &used, "", g_date);
    for (int i = 0; i < g_nres; i++) put_piece(out, out_sz, &used, used ? ". " : "", g_res[i]);
    for (int i = 0; i < ev->n; i++) put_piece(out, out_sz, &used, used ? ". " : "", ev->ev[i]);
    st_unlock();
    return any;
}

// ---- the squad for a mission --------------------------------------------------

static const char* rank_from_abbrev(const char* name, const char** rest)
{
    static const struct { const char* ab; const char* word; } ranks[] = {
        { "RK.", "Rookie" }, { "SQ.", "Squaddie" }, { "CPL.", "Corporal" },
        { "SGT.", "Sergeant" }, { "LT.", "Lieutenant" }, { "CPT.", "Captain" },
        { "MAJ.", "Major" }, { "COL.", "Colonel" },
    };
    *rest = name;
    for (int i = 0; i < (int)(sizeof ranks / sizeof *ranks); i++) {
        size_t n = strlen(ranks[i].ab);
        if (_strnicmp(name, ranks[i].ab, n) == 0 && name[n] == ' ') {
            *rest = name + n + 1;
            return ranks[i].word;
        }
    }
    return NULL;
}

void hq_item_from_image(const char* path, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    const char* p = path ? strstr(path, "Inv_") : NULL;
    if (!p) return;
    p += 4;
    size_t w = 0;
    for (const char* r = p; *r && w + 2 < out_sz; r++) {
        if (*r == '.' || *r == '_') break;
        // A space before a capital that follows a lower-case letter.
        if (w && *r >= 'A' && *r <= 'Z' && r[-1] >= 'a' && r[-1] <= 'z') out[w++] = ' ';
        out[w++] = *r;
    }
    out[w] = 0;
}

void hq_squad_row(const char* name, const char* nick, const char* class_desc,
                  const char* item1, const char* item2, const char* promote,
                  char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    const char* rest;
    const char* rank = rank_from_abbrev(name ? name : "", &rest);
    char who[128], i1[64], i2[64];
    _snprintf_s(who, sizeof who, _TRUNCATE, "%s%s%s%s%s%s", rank ? rank : "",
                rank ? " " : "", rest, nick && *nick ? " '" : "", nick ? nick : "",
                nick && *nick ? "'" : "");
    hq_item_from_image(item1, i1, sizeof i1);
    hq_item_from_image(item2, i2, sizeof i2);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s%s%s%s%s%s", who,
                class_desc && *class_desc ? ", " : "", class_desc ? class_desc : "",
                i1[0] ? ", " : "", i1, i2[0] ? ", " : "", i2,
                promote && *promote ? ", " : "", promote ? promote : "");
}
