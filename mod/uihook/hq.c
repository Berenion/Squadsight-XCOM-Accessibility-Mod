// The headquarters' facility menu, kept from its update stream. See hq.h.

#include "hq.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

static void copy_without_section(char* out, size_t out_sz, const char* in);
static const char* rank_from_abbrev(const char* name, const char** rest);

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
static char g_human[64];               // "ENGINEERS: 10" (hq_status_human)
// Two panels draw the events: Mission Control's, which redraws while it is
// shown whenever the events change, and the base screen's, which redraws only
// when that screen takes focus. Each is kept as it last drew, and the status
// reads the one drawn last. It read the longer one, and a list drawn before
// the council report came in outlived it: "Council Report, 3 days" on the day
// after the report (log of 2026-09-27).
typedef struct {
    const void* list;
    char        ev[HQ_EVENTS][96];
    int         n;
    ULONGLONG   at;         // when it last drew, 0 never
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
                 : (g_evl[0].at <= g_evl[1].at ? &g_evl[0] : &g_evl[1]);
    e->list = list;
    e->n = 0;
    e->at = 0;
    return e;
}

void hq_status_events_clear(const void* list)
{
    st_lock();
    EventList* e = evl_for(list);
    e->n = 0;
    e->at = GetTickCount64();
    st_unlock();
}

void hq_status_event(const void* list, const char* title, const char* unit,
                     const char* count)
{
    if (!title || !*title) return;
    st_lock();
    EventList* e = evl_for(list);
    e->at = GetTickCount64();
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
    const EventList* ev = g_evl[0].at >= g_evl[1].at ? &g_evl[0] : &g_evl[1];
    int any = g_date[0] || g_human[0] || g_nres || ev->n;
    size_t used = 0;
    if (g_date[0]) put_piece(out, out_sz, &used, "", g_date);
    if (g_human[0]) put_piece(out, out_sz, &used, used ? ". " : "", g_human);
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

// ---- the Situation Room ------------------------------------------------------

typedef struct {
    int  known;
    char name[64];
    char cash[32];
    int  panic;
    int  active;
} SitCountry;

static SitCountry g_sit[HQ_SIT_COUNTRIES];
static char g_sit_news_title[64];
static char g_sit_news[HQ_SIT_NEWS][HQ_SIT_TEXT];
static int  g_sit_nnews;
static int  g_sit_doom = -1;
static char g_sit_brief[HQ_SIT_TEXT];
static char g_sit_large[HQ_SIT_TEXT];

void hq_sit_country(int index, const char* name, const char* cash, int panic, int active)
{
    if (index < 0 || index >= HQ_SIT_COUNTRIES || !name || !*name) return;
    st_lock();
    SitCountry* c = &g_sit[index];
    c->known = 1;
    strncpy_s(c->name, sizeof c->name, name, _TRUNCATE);
    copy_without_section(c->cash, sizeof c->cash, cash);
    c->panic = panic;
    c->active = active;
    st_unlock();
}

void hq_sit_news(const char* title, const char* text)
{
    st_lock();
    strncpy_s(g_sit_news_title, sizeof g_sit_news_title, title ? title : "", _TRUNCATE);
    g_sit_nnews = 0;
    const char* r = text ? text : "";
    while (*r && g_sit_nnews < HQ_SIT_NEWS) {
        const char* end = strstr(r, "//");
        size_t len = end ? (size_t)(end - r) : strlen(r);
        while (len && *r == ' ') { r++; len--; }
        while (len && r[len - 1] == ' ') len--;
        if (len) {
            char* d = g_sit_news[g_sit_nnews++];
            size_t k = len < HQ_SIT_TEXT - 1 ? len : HQ_SIT_TEXT - 1;
            memcpy(d, r, k);
            d[k] = 0;
        }
        if (!end) break;
        r = end + 2;
    }
    st_unlock();
}

void hq_sit_doom(int lost) { st_lock(); g_sit_doom = lost; st_unlock(); }

void hq_sit_objectives(const char* brief, const char* large)
{
    st_lock();
    if (brief) strncpy_s(g_sit_brief, sizeof g_sit_brief, brief, _TRUNCATE);
    if (large) strncpy_s(g_sit_large, sizeof g_sit_large, large, _TRUNCATE);
    st_unlock();
}

int hq_sit_lines(char lines[][HQ_SIT_TEXT], int max)
{
    int n = 0;
    st_lock();
    if (g_sit_doom >= 0 && n < max)
        _snprintf_s(lines[n++], HQ_SIT_TEXT, _TRUNCATE, "Countries lost: %d", g_sit_doom);
    for (int i = 0; i < HQ_SIT_COUNTRIES && n < max; i++) {
        const SitCountry* c = &g_sit[i];
        if (!c->known) continue;
        _snprintf_s(lines[n++], HQ_SIT_TEXT, _TRUNCATE, "%s, panic %d of 5%s%s%s", c->name,
                    c->panic, c->cash[0] ? ", funding " : "", c->cash,
                    c->active ? "" : ", left XCOM");
    }
    for (int i = 0; i < g_sit_nnews && n < max; i++)
        _snprintf_s(lines[n++], HQ_SIT_TEXT, _TRUNCATE, "%s%s%s",
                    i == 0 && g_sit_news_title[0] ? g_sit_news_title : "",
                    i == 0 && g_sit_news_title[0] ? ": " : "", g_sit_news[i]);
    // The brief lists the sub-objectives; the large text is each again with
    // its in-depth description, or "NONE" when there are none -- the brief is
    // empty then.
    if (g_sit_brief[0] && n < max)
        _snprintf_s(lines[n++], HQ_SIT_TEXT, _TRUNCATE, "OBJECTIVES: %s", g_sit_brief);
    if (g_sit_large[0] && n < max)
        _snprintf_s(lines[n++], HQ_SIT_TEXT, _TRUNCATE, "%s%s",
                    g_sit_brief[0] ? "" : "OBJECTIVES: ", g_sit_large);
    st_unlock();
    return n;
}

// ---- Build Facilities ------------------------------------------------------------

static void sat_put(char* out, size_t out_sz, size_t* w, const char* s);

static struct {
    char name[128];
    char icon[64];
} g_base[HQ_BASE_W][HQ_BASE_H];

void hq_base_card(int x, int y, const char* name, const char* icon)
{
    if (x < 0 || x >= HQ_BASE_W || y < 0 || y >= HQ_BASE_H) return;
    char n[128];
    copy_without_section(n, sizeof n, name);
    // The steam label is taken off the tile under the cursor; the tile is
    // no less a steam vent for that, so an empty name on the same ground
    // keeps the one before.
    if (!n[0] && strcmp(icon ? icon : "", g_base[x][y].icon) == 0) return;
    strncpy_s(g_base[x][y].name, sizeof g_base[x][y].name, n, _TRUNCATE);
    strncpy_s(g_base[x][y].icon, sizeof g_base[x][y].icon, icon ? icon : "", _TRUNCATE);
}

// The tile as words: the facility's own name when it has one, the ground
// otherwise, with a steam vent said as such.
static void base_tile_words(int x, int y, char* out, size_t out_sz)
{
    out[0] = 0;
    if (x < 0 || x >= HQ_BASE_W || y < 0 || y >= HQ_BASE_H) return;
    const char* name = g_base[x][y].name;
    const char* icon = g_base[x][y].icon;
    // "STEAM." is the whole label on a vent; said once, as a vent.
    int steam = _strnicmp(name, "STEAM", 5) == 0 && (!name[5] || name[5] == '.');
    const char* ground = strcmp(icon, "Rock") == 0      ? "Rock"
                       : strcmp(icon, "RockSteam") == 0 ? "Rock"
                       : strcmp(icon, "Excavated") == 0 ? "Excavated, empty"
                       : strcmp(icon, "BeingExcavated") == 0 ? "Being excavated"
                       : strcmp(icon, "Construction") == 0   ? "Under construction"
                       : NULL;
    if (strcmp(icon, "RockSteam") == 0) steam = 1;
    if (ground) {
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s", ground, steam ? ", steam vent" : "",
                    name[0] && !steam ? ": " : "", name[0] && !steam ? name : "");
    } else {
        strncpy_s(out, out_sz, name[0] ? name : icon, _TRUNCATE);
    }
}

void hq_base_cursor(int x, int y, const char* text, char* out, size_t out_sz)
{
    char tile[256], t[512];
    base_tile_words(x, y, tile, sizeof tile);
    copy_without_section(t, sizeof t, text);
    size_t w = 0;
    out[0] = 0;
    sat_put(out, out_sz, &w, tile);
    sat_put(out, out_sz, &w, t);
    char pos[48];
    _snprintf_s(pos, sizeof pos, _TRUNCATE, "Level %d, column %d.", y, x + 1);
    sat_put(out, out_sz, &w, pos);
}

// ---- choosing a country on the map --------------------------------------------

static struct {
    char country[64];
    char body[HQ_SIT_TEXT];
    int  panic;
    char continent[64];
    char cont_body[HQ_SIT_TEXT];
    char button[2][96];
    int  button_on[2];
    int  available, in_orbit, max;      // available -1: not drawn
    int  said_count;
    char said_cont[64 + HQ_SIT_TEXT];
} g_sat = { .available = -1 };

void hq_sat_country(const char* name, const char* body, int panic)
{
    strncpy_s(g_sat.country, sizeof g_sat.country, name ? name : "", _TRUNCATE);
    copy_without_section(g_sat.body, sizeof g_sat.body, body);
    g_sat.panic = panic;
}

void hq_sat_continent(const char* name, const char* body)
{
    strncpy_s(g_sat.continent, sizeof g_sat.continent, name ? name : "", _TRUNCATE);
    strncpy_s(g_sat.cont_body, sizeof g_sat.cont_body, body ? body : "", _TRUNCATE);
}

void hq_sat_button(int which, const char* label, int enabled)
{
    if (which < 0 || which > 1) return;
    strncpy_s(g_sat.button[which], sizeof g_sat.button[which], label ? label : "", _TRUNCATE);
    g_sat.button_on[which] = enabled && label && *label;
}

void hq_sat_count(int available, int in_orbit, int max)
{
    g_sat.available = available;
    g_sat.in_orbit = in_orbit;
    g_sat.max = max;
}

void hq_sat_reset(void)
{
    memset(&g_sat, 0, sizeof g_sat);
    g_sat.available = -1;
}

static void sat_put(char* out, size_t out_sz, size_t* w, const char* s)
{
    if (!s || !*s) return;
    if (*w) {
        // A stop between parts, unless the part before ended in one.
        char last = out[*w - 1];
        _snprintf_s(out + *w, out_sz - *w, _TRUNCATE, "%s", strchr(".!?:", last) ? " " : ". ");
        *w += strlen(out + *w);
    }
    _snprintf_s(out + *w, out_sz - *w, _TRUNCATE, "%s", s);
    *w += strlen(out + *w);
}

int hq_sat_say(char* out, size_t out_sz)
{
    if (!out || !out_sz) return 0;
    out[0] = 0;
    if (!g_sat.country[0]) return 0;
    size_t w = 0;
    char part[HQ_SIT_TEXT + 96];
    if (!g_sat.said_count && g_sat.available >= 0) {
        _snprintf_s(part, sizeof part, _TRUNCATE, "Satellites: %d available, %d of %d in orbit",
                    g_sat.available, g_sat.in_orbit, g_sat.max);
        sat_put(out, out_sz, &w, part);
        g_sat.said_count = 1;
    }
    if (g_sat.panic > 0)
        _snprintf_s(part, sizeof part, _TRUNCATE, "%s, panic %d of 5", g_sat.country, g_sat.panic);
    else
        strncpy_s(part, sizeof part, g_sat.country, _TRUNCATE);
    sat_put(out, out_sz, &w, part);
    sat_put(out, out_sz, &w, g_sat.body);
    if (g_sat.button_on[0]) {
        _snprintf_s(part, sizeof part, _TRUNCATE, "Enter: %s", g_sat.button[0]);
        sat_put(out, out_sz, &w, part);
    }
    if (g_sat.button_on[1]) sat_put(out, out_sz, &w, g_sat.button[1]);
    char cont[sizeof g_sat.said_cont];
    _snprintf_s(cont, sizeof cont, _TRUNCATE, "%s%s%s", g_sat.continent,
                g_sat.continent[0] && g_sat.cont_body[0] ? ": " : "", g_sat.cont_body);
    if (cont[0] && strcmp(cont, g_sat.said_cont) != 0) {
        sat_put(out, out_sz, &w, cont);
        strncpy_s(g_sat.said_cont, sizeof g_sat.said_cont, cont, _TRUNCATE);
    }
    return 1;
}

// ---- a day passing ------------------------------------------------------------

static char g_day[96];
static unsigned long long g_day_at;

int hq_day_passed(const char* date_a, const char* date_b, unsigned long long now_ms)
{
    char day[96];
    _snprintf_s(day, sizeof day, _TRUNCATE, "%s|%s", date_a ? date_a : "", date_b ? date_b : "");
    int passed = g_day[0] && strcmp(day, g_day) != 0 && now_ms - g_day_at < HQ_DAY_GAP_MS;
    strncpy_s(g_day, sizeof g_day, day, _TRUNCATE);
    g_day_at = now_ms;
    return passed;
}

// ---- Mission Control's notices ------------------------------------------------

static char g_notice[HQ_NOTICES][256];
static int  g_nnotice;

int hq_notices_new(const char* raw, char* out, size_t out_sz)
{
    char now[HQ_NOTICES][256];
    int  nnow = 0;
    const char* r = raw ? raw : "";
    while (*r && nnow < HQ_NOTICES) {
        const char* end = strchr(r, '\n');
        size_t len = end ? (size_t)(end - r) : strlen(r);
        while (len && (*r == ' ' || *r == '\r')) { r++; len--; }
        while (len && (r[len - 1] == ' ' || r[len - 1] == '\r')) len--;
        if (len) {
            size_t k = len < sizeof now[0] - 1 ? len : sizeof now[0] - 1;
            memcpy(now[nnow], r, k);
            now[nnow][k] = 0;
            nnow++;
        }
        if (!end) break;
        r = end + 1;
    }
    if (out && out_sz) out[0] = 0;
    size_t used = 0;
    int fresh = 0;
    // Newest first on the screen; said oldest first, as they happened.
    for (int i = nnow - 1; i >= 0; i--) {
        int seen = 0;
        for (int j = 0; j < g_nnotice && !seen; j++) seen = strcmp(now[i], g_notice[j]) == 0;
        if (seen) continue;
        fresh++;
        if (!out || !out_sz) continue;
        const char* rest;
        const char* rank = rank_from_abbrev(now[i], &rest);
        char line[300];
        _snprintf_s(line, sizeof line, _TRUNCATE, "%s%s%s", rank ? rank : "", rank ? " " : "", rest);
        put_piece(out, out_sz, &used, used ? " " : "", line);
    }
    memcpy(g_notice, now, sizeof now);
    g_nnotice = nnow;
    return fresh;
}

// ---- Engineering ----------------------------------------------------------------

static char g_queue_title[64];
static char g_queue[HQ_QUEUE][HQ_SIT_TEXT];
static int  g_nqueue;

void hq_status_human(const char* label, const char* value)
{
    st_lock();
    if (!label || !*label) {
        g_human[0] = 0;
    } else {
        size_t n = strlen(label);
        _snprintf_s(g_human, sizeof g_human, _TRUNCATE, "%s%s %s", label,
                    label[n - 1] == ':' ? "" : ":", value ? value : "");
    }
    st_unlock();
}

// A sentence break in hq_cost_text: a full stop unless the text already
// ends in punctuation, then a space. Nothing at the very start.
static void cost_break(char* out, size_t out_sz, size_t* w)
{
    while (*w && out[*w - 1] == ' ') (*w)--;
    if (!*w) return;
    if (!strchr(".!?:;,", out[*w - 1]) && *w + 1 < out_sz) out[(*w)++] = '.';
    if (*w + 1 < out_sz) out[(*w)++] = ' ';
}

static void cost_puts(char* out, size_t out_sz, size_t* w, const char* s)
{
    for (; *s && *w + 1 < out_sz; s++) out[(*w)++] = *s;
}

void hq_cost_text(const char* raw, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    size_t w = 0;
    int red = 0;
    size_t run = 0;             // where the current coloured run began
    for (const char* r = raw ? raw : ""; *r; r++) {
        if (*r == '<') {
            const char* end = strchr(r, '>');
            if (!end) break;
            int closing = r[1] == '/';
            const char* t = r + 1 + closing;
            if (!closing && _strnicmp(t, "br", 2) == 0) {
                cost_break(out, out_sz, &w);
            } else if (!closing && _strnicmp(t, "font", 4) == 0) {
                const char* c = strstr(r, "color='#");
                red = c && c < end && _strnicmp(c + 8, "EE1C25", 6) == 0;
                run = w;
            } else if (closing && _strnicmp(t, "font", 4) == 0) {
                size_t e = w;
                while (e > run && out[e - 1] == ' ') e--;
                if (red && e > run && !strchr(".!?", out[e - 1])) {
                    w = e;
                    cost_puts(out, out_sz, &w, " (not enough)");
                }
                red = 0;
            }
            r = end;
            continue;
        }
        if (*r == '\n' || *r == '\r') { cost_break(out, out_sz, &w); continue; }
        // The section sign before a sum: the sum, then what it is.
        if ((unsigned char)r[0] == 0xC2 && (unsigned char)r[1] == 0xA7) {
            r += 2;
            int digits = 0;
            while ((*r >= '0' && *r <= '9') ||
                   ((*r == ',' || *r == '.') && r[1] >= '0' && r[1] <= '9')) {
                if (w + 1 < out_sz) out[w++] = *r;
                r++;
                digits++;
            }
            if (digits) cost_puts(out, out_sz, &w, " credits");
            r--;
            continue;
        }
        if (*r == ',') {
            if (w + 1 < out_sz) out[w++] = ',';
            if (r[1] != ' ' && w + 1 < out_sz) out[w++] = ' ';
            continue;
        }
        if (w + 1 < out_sz) out[w++] = *r;
    }
    out[w] = 0;
    // One space at a time, none at either end.
    size_t o = 0;
    int space = 0;
    for (size_t i = 0; out[i]; i++) {
        char c = out[i] == '\t' ? ' ' : out[i];
        if (c == ' ') { if (o && !space) out[o++] = ' '; space = 1; continue; }
        // "(not enough) ," from a comma outside the run: close it up.
        if ((c == ',' || c == '.') && o && out[o - 1] == ' ') o--;
        out[o++] = c;
        space = 0;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

void hq_build_row(const char* raw_label, int quantity, const char* qty_label,
                  char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    char name[128];
    strip_tags(raw_label ? raw_label : "", name, sizeof name);
    int red = raw_label && strstr(raw_label, "EE1C25") != NULL;
    char qty[64] = "";
    if (quantity > 0) {
        if (qty_label && *qty_label)
            _snprintf_s(qty, sizeof qty, _TRUNCATE, ", %s%s %d", qty_label,
                        qty_label[strlen(qty_label) - 1] == ':' ? "" : ":", quantity);
        else
            _snprintf_s(qty, sizeof qty, _TRUNCATE, ", %d", quantity);
    }
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", name, red ? ", unavailable" : "", qty);
}

void hq_queue_row(const char* desc, const char* qty, const char* eta,
                  char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    char done[64] = "";
    const char* slash = qty ? strchr(qty, '/') : NULL;
    if (slash && slash > qty && slash[1])
        _snprintf_s(done, sizeof done, _TRUNCATE, ", %.*s of %s done",
                    (int)(slash - qty), qty, slash + 1);
    const char* when = !eta || !*eta ? "" : strcmp(eta, "--") == 0 ? "no engineers" : eta;
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s", desc ? desc : "", done,
                *when ? ", " : "", when);
}

void hq_queue_clear(void) { st_lock(); g_nqueue = 0; st_unlock(); }

void hq_queue_title(const char* title)
{
    st_lock();
    strncpy_s(g_queue_title, sizeof g_queue_title, title ? title : "", _TRUNCATE);
    st_unlock();
}

void hq_queue_add(const char* row)
{
    if (!row || !*row) return;
    st_lock();
    if (g_nqueue < HQ_QUEUE)
        strncpy_s(g_queue[g_nqueue++], sizeof g_queue[0], row, _TRUNCATE);
    st_unlock();
}

int hq_eng_lines(char lines[][HQ_SIT_TEXT], int max)
{
    int n = 0;
    st_lock();
    if (g_queue_title[0] && n < max)
        strncpy_s(lines[n++], HQ_SIT_TEXT, g_queue_title, _TRUNCATE);
    for (int i = 0; i < g_nqueue && n < max; i++)
        strncpy_s(lines[n++], HQ_SIT_TEXT, g_queue[i], _TRUNCATE);
    st_unlock();
    // Takes the lock itself.
    if (n < max && hq_status_line(lines[n], HQ_SIT_TEXT)) n++;
    return n;
}

// ---- a research report ----------------------------------------------------------

static char g_rep_title[128];
static char g_rep_sub[256];
static char g_rep_subject[256];
static char g_rep_notes[4096];
static char g_rep_results[16][256];
static int  g_rep_nresults;

// Trimmed, with line breaks as sentence breaks: the codename and the date
// are one string split by "\n".
static void rep_copy(char* out, size_t out_sz, const char* in)
{
    size_t w = 0;
    out[0] = 0;
    for (const char* r = in ? in : ""; *r && w + 3 < out_sz; r++) {
        if (*r == '\r') continue;
        if (*r == '\n') {
            while (w && out[w - 1] == ' ') w--;
            if (w && !strchr(".!?:;,", out[w - 1])) out[w++] = '.';
            if (w) out[w++] = ' ';
            continue;
        }
        if (*r == ' ' && (!w || out[w - 1] == ' ')) continue;
        out[w++] = *r;
    }
    while (w && out[w - 1] == ' ') w--;
    out[w] = 0;
}

void hq_report_titles(const char* title, const char* sub)
{
    rep_copy(g_rep_title, sizeof g_rep_title, title);
    rep_copy(g_rep_sub, sizeof g_rep_sub, sub);
}

void hq_report_item(const char* subject, const char* notes)
{
    rep_copy(g_rep_subject, sizeof g_rep_subject, subject);
    rep_copy(g_rep_notes, sizeof g_rep_notes, notes);
}

void hq_report_results_clear(void) { g_rep_nresults = 0; }

void hq_report_result(const char* text)
{
    if (!text || !*text || g_rep_nresults >= 16) return;
    rep_copy(g_rep_results[g_rep_nresults], sizeof g_rep_results[0], text);
    if (g_rep_results[g_rep_nresults][0]) g_rep_nresults++;
}

int hq_report_ready(void) { return g_rep_subject[0] != 0; }

// Ends a piece with a full stop unless it already has one.
static void rep_stop(char* s, size_t sz)
{
    size_t n = strlen(s);
    if (n && !strchr(".!?", s[n - 1]) && n + 1 < sz) { s[n] = '.'; s[n + 1] = 0; }
}

// The title and the codename line: "Research Report. Codename: Sagaris. March, 2015."
static void rep_heading(char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", g_rep_title,
                g_rep_title[0] && g_rep_sub[0] ? ". " : "", g_rep_sub);
    rep_stop(out, out_sz);
}

// A sentence ends at . ! or ? (and any closing quote or bracket after it),
// followed by a space and a capital, a digit or an opening quote. Not after
// a short capitalised word -- "Dr. Vahlen", "Mr. Shen" -- nor inside an
// acronym, "S.C.O.P.E. available", which is followed by lower case anyway.
static int rep_sentence_end(const char* s, const char* p)
{
    if (!strchr(".!?", *p)) return 0;
    const char* q = p + 1;
    while (*q == '"' || *q == '\'' || *q == ')') q++;
    if (*q != ' ') return 0;
    unsigned char next = (unsigned char)q[1];
    if (!((next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9') || next == '"' ||
          next == '\'' || next >= 0x80))
        return 0;
    if (*p == '.') {
        const char* w = p;
        while (w > s && w[-1] != ' ') w--;
        size_t len = (size_t)(p - w);
        if (len && len <= 3 && w[0] >= 'A' && w[0] <= 'Z') {
            int lower = 1;
            for (const char* c = w + 1; c < p; c++) lower &= *c >= 'a' && *c <= 'z';
            if (lower) return 0;
        }
    }
    return (int)(q - p);
}

int hq_report_pieces(char pieces[][HQ_REPORT_TEXT], int max)
{
    int k = 0;
    if (g_rep_subject[0] && k < max)
        strncpy_s(pieces[k++], HQ_REPORT_TEXT, g_rep_subject, _TRUNCATE);
    const char* s = g_rep_notes;
    const char* start = s;
    for (const char* p = s; *p && k < max; p++) {
        int tail = rep_sentence_end(s, p);
        if (!tail && p[1]) continue;
        size_t len = (size_t)(p - start) + (tail ? (size_t)tail : 1);
        if (len >= HQ_REPORT_TEXT) len = HQ_REPORT_TEXT - 1;
        memcpy(pieces[k], start, len);
        pieces[k][len] = 0;
        if (pieces[k][0]) k++;
        start = p + (tail ? tail : 1);
        while (*start == ' ') start++;
        p = start - 1;
        if (!*start) break;
    }
    for (int i = 0; i < g_rep_nresults && k < max; i++) {
        _snprintf_s(pieces[k], HQ_REPORT_TEXT, _TRUNCATE, "%s%s", i ? "" : "Results: ",
                    g_rep_results[i]);
        rep_stop(pieces[k], HQ_REPORT_TEXT);
        k++;
    }
    if ((g_rep_title[0] || g_rep_sub[0]) && k < max) rep_heading(pieces[k++], HQ_REPORT_TEXT);
    return k;
}

void hq_report_text(char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    char piece[512];
    if (g_rep_subject[0]) {
        strncpy_s(piece, sizeof piece, g_rep_subject, _TRUNCATE);
        rep_stop(piece, sizeof piece);
        put_piece(out, out_sz, &used, "", piece);
    }
    if (g_rep_notes[0]) {
        put_piece(out, out_sz, &used, used ? " " : "", g_rep_notes);
        size_t n = strlen(out);
        if (n && !strchr(".!?\"'", out[n - 1])) put_piece(out, out_sz, &used, "", ".");
    }
    for (int i = 0; i < g_rep_nresults; i++) {
        strncpy_s(piece, sizeof piece, g_rep_results[i], _TRUNCATE);
        rep_stop(piece, sizeof piece);
        put_piece(out, out_sz, &used, used ? (i ? " " : " Results: ") : "Results: ", piece);
    }
    if (g_rep_title[0] || g_rep_sub[0]) {
        rep_heading(piece, sizeof piece);
        put_piece(out, out_sz, &used, used ? " " : "", piece);
    }
}
