// The unit information screen, in words. See info.h.

#include "info.h"
#include "soldier.h"
#include <stdio.h>
#include <string.h>

#define INFO_ITEMS 32
#define INFO_MODS  16

typedef struct {
    char title[48];
    int  n;
    char name[INFO_ITEMS][64];
    char desc[INFO_ITEMS][512];
} InfoList;

typedef struct {
    char label[64];
    char value[16];
} InfoMod;

static struct {
    int  any;                   // anything arrived since the reset
    char who[256];              // the header, already worded
    char stats[4][48];
    int  nstats;
    char weapons[256];
    InfoList lists[INFO_LISTS];
    char shot[64], hit[16], hit_label[48], crit[16], crit_label[48];
    InfoMod mods[2][INFO_MODS]; // [0] hit, [1] crit
    int  nmods[2];
} g;

static int g_open;
static int g_at;                // -1 before the first line

static void copy(char* dst, size_t sz, const char* src)
{
    strncpy_s(dst, sz, src ? src : "", _TRUNCATE);
}

// Appends `piece` after `sep` (or alone at the start); bounded by hand.
static void add(char* out, size_t out_sz, const char* sep, const char* piece)
{
    if (!piece || !*piece) return;
    size_t used = strlen(out);
    if (used + 1 >= out_sz) return;
    _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s", used ? sep : "", piece);
}

// "Chance to Hit:" and "72%" -> "Chance to Hit: 72%". Either may be missing.
static void labelled(const char* label, const char* value, char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", label, *label && *value ? " " : "", value);
}

InfoCall info_call(const char* obj, const char* fn)
{
    if (!obj || !fn || strncmp(obj, "UIUnitGermanMode_", 17) != 0) return INFO_NONE;
    if (strncmp(fn, "AS_", 3) == 0) fn += 3;
    const char* kind = obj + 17;
    if (*kind >= '0' && *kind <= '9') {
        if (strcmp(fn, "SetSoldierInformation") == 0) return INFO_SOLDIER;
        if (strcmp(fn, "SetAlienInformation") == 0)   return INFO_ALIEN;
        if (strcmp(fn, "SetUnitStats") == 0)          return INFO_STATS;
    } else if (strncmp(kind, "PerkList_", 9) == 0) {
        if (strcmp(fn, "SetTitle") == 0) return INFO_TITLE;
        if (strcmp(fn, "AddPerk") == 0)  return INFO_PERK;
    } else if (strncmp(kind, "ShotInfo_", 9) == 0) {
        if (strcmp(fn, "SetShotInfo") == 0)    return INFO_SHOT;
        if (strcmp(fn, "AddRegularItem") == 0) return INFO_HIT_MOD;
        if (strcmp(fn, "AddCritItem") == 0)    return INFO_CRIT_MOD;
    }
    return INFO_NONE;
}

void info_reset(void)
{
    memset(&g, 0, sizeof g);
    g_open = 0;
    g_at = -1;
}

void info_soldier(const char* name, const char* nick, const char* cls,
                  const char* rank, int promotion)
{
    char title[64], words[64], piece[128];
    g.any = 1;
    g.who[0] = 0;
    soldier_title_case(name ? name : "", title, sizeof title);
    _snprintf_s(g.who, sizeof g.who, _TRUNCATE, "%s%s%s.", title,
                nick && *nick ? ", " : "", nick ? nick : "");
    soldier_class_words(cls ? cls : "", words, sizeof words);
    const char* r = soldier_rank_word(rank ? rank : "");
    if (*r || *words) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s%s%s.", r, *r && *words ? ", " : "", words);
        add(g.who, sizeof g.who, " ", piece);
    }
    if (promotion == 1) add(g.who, sizeof g.who, " ", "Promotion available.");
}

void info_alien(const char* name)
{
    g.any = 1;
    _snprintf_s(g.who, sizeof g.who, _TRUNCATE, "%s.", name && *name ? name : "Unknown");
}

void info_stats(const char* const* stats, int n)
{
    g.any = 1;
    g.nstats = 0;
    for (int i = 0; i < n && g.nstats < 4; i++)
        if (stats[i] && *stats[i]) copy(g.stats[g.nstats++], sizeof g.stats[0], stats[i]);
}

void info_weapons(const char* text)
{
    copy(g.weapons, sizeof g.weapons, text);
}

void info_list_title(int list, const char* title)
{
    if (list < 0 || list >= INFO_LISTS) return;
    g.any = 1;
    soldier_title_case(title ? title : "", g.lists[list].title, sizeof g.lists[list].title);
}

void info_list_add(int list, const char* name, const char* desc)
{
    if (list < 0 || list >= INFO_LISTS) return;
    InfoList* l = &g.lists[list];
    if (l->n >= INFO_ITEMS || !name || !*name) return;
    g.any = 1;
    copy(l->name[l->n], sizeof l->name[0], name);
    copy(l->desc[l->n], sizeof l->desc[0], desc);
    l->n++;
}

void info_shot(const char* name, const char* hit, const char* hit_label,
               const char* crit, const char* crit_label)
{
    if (!*name && !*hit && !*hit_label && !*crit && !*crit_label) {
        g.shot[0] = g.hit[0] = g.hit_label[0] = g.crit[0] = g.crit_label[0] = 0;
        g.nmods[0] = g.nmods[1] = 0;
        return;
    }
    g.any = 1;
    soldier_title_case(name, g.shot, sizeof g.shot);
    copy(g.hit, sizeof g.hit, hit);
    copy(g.hit_label, sizeof g.hit_label, hit_label);
    copy(g.crit, sizeof g.crit, crit);
    copy(g.crit_label, sizeof g.crit_label, crit_label);
}

void info_modifier(int crit, const char* label, const char* value)
{
    int k = crit ? 1 : 0;
    if (g.nmods[k] >= INFO_MODS || !label || !*label) return;
    g.any = 1;
    InfoMod* m = &g.mods[k][g.nmods[k]++];
    copy(m->label, sizeof m->label, label);
    copy(m->value, sizeof m->value, value);
}

int info_has_content(void) { return g.any; }

// Whether the shot half has anything to say.
static int has_shot(void)
{
    return g.shot[0] || g.hit[0] || g.crit[0] || g.nmods[0] || g.nmods[1];
}

static int has_crit(void) { return g.crit[0] || g.nmods[1]; }

static void stats_line(char* out, size_t out_sz)
{
    out[0] = 0;
    for (int i = 0; i < g.nstats; i++) add(out, out_sz, ". ", g.stats[i]);
    if (out[0]) add(out, out_sz, "", ".");
}

// "Fire. Chance to Hit: 72%." -- or with no chance shown, the name alone.
static void hit_line(char* out, size_t out_sz)
{
    char piece[96];
    out[0] = 0;
    if (g.shot[0]) { add(out, out_sz, " ", g.shot); add(out, out_sz, "", "."); }
    labelled(g.hit[0] ? g.hit_label : "", g.hit, piece, sizeof piece);
    if (piece[0]) { add(out, out_sz, " ", piece); add(out, out_sz, "", "."); }
}

static void crit_line(char* out, size_t out_sz)
{
    labelled(g.crit_label[0] ? g.crit_label : "Chance to crit:", g.crit, out, out_sz);
    add(out, out_sz, "", ".");
}

static void mod_text(const InfoMod* m, char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s %s", m->label, m->value);
}

// "Aim +65%, Full cover -40%."
static void mods_line(int k, char* out, size_t out_sz)
{
    char piece[96];
    size_t used = strlen(out);
    for (int i = 0; i < g.nmods[k]; i++) {
        mod_text(&g.mods[k][i], piece, sizeof piece);
        add(out, out_sz, i ? ", " : " ", piece);
    }
    if (g.nmods[k] && strlen(out) > used) add(out, out_sz, "", ".");
}

void info_summary(char* out, size_t out_sz)
{
    char piece[INFO_TEXT];
    _snprintf_s(out, out_sz, _TRUNCATE, "Target information.");
    add(out, out_sz, " ", g.who);
    stats_line(piece, sizeof piece);
    add(out, out_sz, " ", piece);
    add(out, out_sz, " ", g.weapons);
    if (has_shot()) {
        hit_line(piece, sizeof piece);
        add(out, out_sz, " ", piece);
        mods_line(0, out, out_sz);
        if (has_crit()) {
            crit_line(piece, sizeof piece);
            add(out, out_sz, " ", piece);
            mods_line(1, out, out_sz);
        }
    }
    for (int i = 0; i < INFO_LISTS; i++) {
        if (!g.lists[i].n) continue;
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s, %d.",
                    g.lists[i].title[0] ? g.lists[i].title : "List", g.lists[i].n);
        add(out, out_sz, " ", piece);
    }
}

// ---- the list ---------------------------------------------------------------

// Walks the lines in order, writing line `want` into `out`; returns how many
// there are. One walk for both counting and fetching keeps the two in step.
static int lines(int want, char* out, size_t out_sz)
{
    int n = 0;
    char piece[INFO_TEXT];
#define LINE(text) do { if (n == want) copy(out, out_sz, (text)); n++; } while (0)
    if (g.who[0]) LINE(g.who);
    stats_line(piece, sizeof piece);
    if (piece[0]) LINE(piece);
    if (g.weapons[0]) LINE(g.weapons);
    if (has_shot()) {
        hit_line(piece, sizeof piece);
        if (piece[0]) LINE(piece);
        for (int i = 0; i < g.nmods[0]; i++) {
            mod_text(&g.mods[0][i], piece, sizeof piece);
            LINE(piece);
        }
        if (has_crit()) {
            crit_line(piece, sizeof piece);
            LINE(piece);
            for (int i = 0; i < g.nmods[1]; i++) {
                mod_text(&g.mods[1][i], piece, sizeof piece);
                LINE(piece);
            }
        }
    }
    for (int l = 0; l < INFO_LISTS; l++) {
        const InfoList* L = &g.lists[l];
        if (!L->n) continue;
        // The heading names what is under it: "Abilities, 1." alone sounded
        // like the end of the screen, and Mind Merge was one press away
        // (2026-09-27). The lines below still give each description.
        int w = _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s, %d:",
                            L->title[0] ? L->title : "List", L->n);
        for (int i = 0; i < L->n && w > 0 && (size_t)w < sizeof piece; i++) {
            int more = _snprintf_s(piece + w, sizeof piece - (size_t)w, _TRUNCATE, "%s %s",
                                   i ? "," : "", L->name[i]);
            if (more < 0) break;
            w += more;
        }
        if (w > 0 && (size_t)w < sizeof piece)
            _snprintf_s(piece + w, sizeof piece - (size_t)w, _TRUNCATE, ".");
        LINE(piece);
        for (int i = 0; i < L->n; i++) {
            _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s%s%s", L->name[i],
                        L->desc[i][0] ? ": " : "", L->desc[i]);
            LINE(piece);
        }
    }
#undef LINE
    return n;
}

int info_line_count(void)
{
    char none[1];
    return lines(-1, none, sizeof none);
}

void info_line(int i, char* out, size_t out_sz)
{
    out[0] = 0;
    lines(i, out, out_sz);
}

void info_open(void)  { g_open = 1; g_at = -1; }
void info_close(void) { g_open = 0; }
int  info_is_open(void) { return g_open; }

void info_step(int dir, char* out, size_t out_sz)
{
    int n = info_line_count();
    char line[INFO_TEXT];
    out[0] = 0;
    if (n == 0) return;
    if (g_at >= n) g_at = n - 1;
    int to = g_at + dir;
    const char* edge = "";
    if (to < 0) { to = 0; edge = "Top. "; }
    else if (to >= n) { to = n - 1; edge = "End. "; }
    g_at = to;
    info_line(g_at, line, sizeof line);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s", edge, line);
}

void info_current(char* out, size_t out_sz)
{
    out[0] = 0;
    if (g_at < 0) { info_summary(out, out_sz); return; }
    info_line(g_at, out, out_sz);
}
