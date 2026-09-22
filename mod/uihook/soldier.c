// The selected soldier, in words. See soldier.h.

#include "soldier.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void soldier_clear(SoldierState* s)
{
    memset(s, 0, sizeof *s);
    s->leader = s->promotion = -1;
    s->aim = -1;
    s->hp = s->hp_max = -1;
    s->actions = -1;
    s->buff = s->debuff = -1;
}

static int is_rank(const char* t)
{
    return (strncmp(t, "rank", 4) == 0 || strncmp(t, "shiv", 4) == 0) &&
           isdigit((unsigned char)t[4]);
}

static int is_class(const char* t)
{
    static const char* base[] = { "none", "sniper", "heavy", "support", "assault", "mech" };
    for (int i = 0; i < (int)(sizeof base / sizeof base[0]); i++) {
        size_t n = strlen(base[i]);
        if (strncmp(t, base[i], n) == 0 && (t[n] == 0 || t[n] == '_')) return 1;
    }
    return 0;
}

void soldier_from_stats(SoldierState* s, const char* const* strings, int n)
{
    s->name[0] = s->nick[0] = s->rank[0] = s->cls[0] = 0;
    for (int i = 0; i < n; i++) {
        const char* t = strings[i];
        if (!t || !*t) continue;
        if (!s->name[0]) {
            strncpy_s(s->name, sizeof s->name, t, _TRUNCATE);
        } else if (t[0] == '\'' && !s->nick[0]) {
            strncpy_s(s->nick, sizeof s->nick, t, _TRUNCATE);
        } else if (is_rank(t) && !s->rank[0]) {
            strncpy_s(s->rank, sizeof s->rank, t, _TRUNCATE);
        } else if (is_class(t) && !s->cls[0]) {
            strncpy_s(s->cls, sizeof s->cls, t, _TRUNCATE);
        }
    }
}

void soldier_title_case(const char* in, char* out, size_t out_sz)
{
    size_t n = 0;
    int start = 1;          // at the start of a word
    int after_mark = 0;     // just after O' or D' at a word's start
    size_t word_len = 0;
    for (; in[n] && n + 1 < out_sz; n++) {
        unsigned char c = (unsigned char)in[n];
        if (isalpha(c)) {
            out[n] = (char)((start || after_mark) ? toupper(c) : tolower(c));
            start = 0;
            after_mark = 0;
            word_len++;
        } else {
            out[n] = (char)c;
            // O'Reilly, D'Arcy: a single letter and an apostrophe begin a name.
            after_mark = c == '\'' && word_len == 1;
            start = c == ' ' || c == '-';
            if (start) word_len = 0;
        }
    }
    out[n] = 0;
}

const char* soldier_rank_word(const char* r)
{
    static const char* ranks[] = {
        "Rookie", "Squaddie", "Corporal", "Sergeant",
        "Lieutenant", "Captain", "Major", "Colonel",
    };
    if (strncmp(r, "shiv", 4) == 0) return "SHIV";
    if (strncmp(r, "rank", 4) != 0) return "";
    int i = atoi(r + 4);
    return i >= 0 && i < (int)(sizeof ranks / sizeof ranks[0]) ? ranks[i] : "";
}

// "heavy" -> "heavy", "mech_psi_gene" -> "MEC trooper, psionic, gene modded".
void soldier_class_words(const char* c, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!*c || strncmp(c, "none", 4) == 0) return;
    const char* base = c;
    char head[16];
    size_t n = strcspn(c, "_");
    if (n >= sizeof head) n = sizeof head - 1;
    memcpy(head, base, n);
    head[n] = 0;
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s",
                strcmp(head, "mech") == 0 ? "MEC trooper" : head,
                strstr(c, "_psi") ? ", psionic" : "",
                strstr(c, "_gene") ? ", gene modded" : "");
}

// Appends ". piece" (or "piece" at the start); bounded by hand.
static void add(char* out, size_t out_sz, const char* piece)
{
    if (!piece || !*piece) return;
    size_t used = strlen(out);
    _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s", used ? " " : "", piece);
}

static void who(const SoldierState* s, char* out, size_t out_sz)
{
    char name[64];
    soldier_title_case(s->name, name, sizeof name);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s.", name, s->nick[0] ? ", " : "", s->nick);
}

static void hp_and_actions(const SoldierState* s, char* out, size_t out_sz)
{
    char piece[64];
    if (s->hp >= 0 && s->hp_max > 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%d of %d HP.", s->hp, s->hp_max);
        add(out, out_sz, piece);
    }
    if (s->actions == 0)
        add(out, out_sz, "No actions left.");
    else if (s->actions == 1)
        add(out, out_sz, "1 action.");
    else if (s->actions > 1) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%d actions.", s->actions);
        add(out, out_sz, piece);
    }
}

void soldier_weapon_words(const char* type, char* out, size_t out_sz)
{
    size_t n = 0;
    const char* t = type && *type == '_' ? type + 1 : (type ? type : "");
    for (size_t i = 0; t[i] && n + 2 < out_sz; i++) {
        unsigned char c = (unsigned char)t[i];
        // A capital after a small letter starts a word: RocketLauncher.
        if (i && isupper(c) && islower((unsigned char)t[i - 1])) out[n++] = ' ';
        out[n++] = c == '_' ? ' ' : (char)c;
    }
    out[n] = 0;
}

static void squash(const char* in, char* out, size_t out_sz)
{
    size_t n = 0;
    for (; in && *in && n + 1 < out_sz; in++)
        if (isalnum((unsigned char)*in)) out[n++] = (char)tolower((unsigned char)*in);
    out[n] = 0;
}

int soldier_weapon_is(const char* name, const char* type)
{
    char a[64], b[64];
    squash(name, a, sizeof a);
    squash(type, b, sizeof b);
    return a[0] && strcmp(a, b) == 0;
}

int soldier_weapon_like(const char* name, const char* type)
{
    char t[64], word[64];
    squash(type, t, sizeof t);
    int words = 0;
    while (name && *name) {
        while (*name && !isalnum((unsigned char)*name)) name++;
        size_t n = 0;
        while (*name && isalnum((unsigned char)*name) && n + 1 < sizeof word)
            word[n++] = (char)tolower((unsigned char)*name++);
        word[n] = 0;
        if (!n) break;
        if (!strstr(t, word)) return 0;
        words++;
    }
    return words > 0;
}

void soldier_weapon_text(const char* name, const SoldierWeapon* w, char* out, size_t out_sz)
{
    char what[64];
    out[0] = 0;
    if (!w || !w->set) return;
    if (w->overheat)
        _snprintf_s(what, sizeof what, _TRUNCATE, "%d%% overheat chance", w->value);
    else if (w->value <= 0)
        _snprintf_s(what, sizeof what, _TRUNCATE, "%s", w->reload ? "empty, reload needed" : "empty");
    else if (w->cost > 0) {
        int shots = w->value / w->cost;
        if (shots < 1) shots = 1;
        _snprintf_s(what, sizeof what, _TRUNCATE, "%d shot%s left", shots, shots == 1 ? "" : "s");
    } else if (w->value >= 100)
        _snprintf_s(what, sizeof what, _TRUNCATE, "full");
    else
        _snprintf_s(what, sizeof what, _TRUNCATE, "%d%% ammo", w->value);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s.", name, what);
}

void soldier_weapons(const char* active_name, const SoldierWeapon* w, int n,
                     char* active, size_t active_sz, char* all, size_t all_sz)
{
    active[0] = all[0] = 0;
    // The name as shown and the type seldom agree letter for letter: "Laser
    // Rifle" is _LaserAssaultRifle, "Light Plasma Rifle" _PlasmaLightRifle.
    // Exactly, then every word of the name somewhere in the type, then the
    // primary weapon.
    int first = -1;
    for (int i = 0; i < n && first < 0; i++)
        if (w[i].set && soldier_weapon_is(active_name, w[i].type)) first = i;
    for (int i = 0; i < n && first < 0; i++)
        if (w[i].set && soldier_weapon_like(active_name, w[i].type)) first = i;
    if (first < 0 && active_name && active_name[0] && n > 0 && w[0].set) first = 0;
    char piece[SOLDIER_WEAPON_TEXT], words[64];
    if (first >= 0) {
        soldier_weapon_text(active_name, &w[first], active, active_sz);
        add(all, all_sz, active);
    }
    for (int i = 0; i < n; i++) {
        if (i == first || !w[i].set) continue;
        soldier_weapon_words(w[i].type, words, sizeof words);
        soldier_weapon_text(words, &w[i], piece, sizeof piece);
        add(all, all_sz, piece);
    }
}

void soldier_brief(const SoldierState* s, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!s->name[0]) return;
    who(s, out, out_sz);
    hp_and_actions(s, out, out_sz);
    add(out, out_sz, s->weapon);
}

void soldier_full(const SoldierState* s, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!s->name[0]) return;
    who(s, out, out_sz);

    char cls[64], piece[96];
    soldier_class_words(s->cls, cls, sizeof cls);
    const char* rank = soldier_rank_word(s->rank);
    if (*rank || *cls) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s%s%s.", rank,
                    *rank && *cls ? ", " : "", cls);
        add(out, out_sz, piece);
    }
    if (s->leader == 1) add(out, out_sz, "Squad leader.");
    hp_and_actions(s, out, out_sz);
    add(out, out_sz, s->weapons);
    if (s->aim >= 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "Aim %d.", s->aim);
        add(out, out_sz, piece);
    }
    if (s->promotion == 1) add(out, out_sz, "Promotion available.");
    if (s->buff == 1) add(out, out_sz, "Has bonuses.");
    if (s->debuff == 1) add(out, out_sz, "Has penalties.");
}
