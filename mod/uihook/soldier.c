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

void soldier_brief(const SoldierState* s, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!s->name[0]) return;
    who(s, out, out_sz);
    hp_and_actions(s, out, out_sz);
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
    if (s->aim >= 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "Aim %d.", s->aim);
        add(out, out_sz, piece);
    }
    if (s->promotion == 1) add(out, out_sz, "Promotion available.");
    if (s->buff == 1) add(out, out_sz, "Has bonuses.");
    if (s->debuff == 1) add(out, out_sz, "Has penalties.");
}
