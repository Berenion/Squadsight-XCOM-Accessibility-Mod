// The selected soldier, in words. See soldier.h.

#include "soldier.h"
#include "strings.h"
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The game's own words for a rank, a class and the S.H.I.V., in the player's
// language, from main.c (soldier_set_loc); the English below when it has none
// to give, as in the offline checks.
static SoldierLocFn g_loc;

void soldier_set_loc(SoldierLocFn fn) { g_loc = fn; }

static const char* loc_or(int kind, int index, const char* english)
{
    static __declspec(thread) char buf[4][96];
    static __declspec(thread) int next;
    char* b = buf[next++ & 3];
    if (g_loc && g_loc(kind, index, b, sizeof buf[0]) && b[0]) return b;
    return english;
}

void soldier_clear(SoldierState* s)
{
    memset(s, 0, sizeof *s);
    s->leader = s->promotion = -1;
    s->aim = -1;
    s->hp = s->hp_max = -1;
    s->actions = -1;
    s->buff = s->debuff = -1;
    s->panicked = -1;
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
    // In UTF-16, so that a name in the game's language -- Cyrillic, Polish,
    // an umlaut -- has its letters' case changed too, not only ASCII's.
    static __declspec(thread) wchar_t w[1024];
    if (!out || !out_sz) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, in ? in : "", -1, w, 1024);
    if (len <= 0) { out[0] = 0; return; }
    len--;
    int start = 1;          // at the start of a word
    int after_mark = 0;     // just after O' or D' at a word's start
    int word_len = 0;
    for (int n = 0; n < len; n++) {
        wchar_t c = w[n];
        if (IsCharAlphaW(c)) {
            if (start || after_mark) CharUpperBuffW(&w[n], 1);
            else CharLowerBuffW(&w[n], 1);
            start = 0;
            after_mark = 0;
            word_len++;
        } else {
            // O'Reilly, D'Arcy: a single letter and an apostrophe begin a name.
            after_mark = c == '\'' && word_len == 1;
            start = c == ' ' || c == '-';
            if (start) word_len = 0;
        }
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_sz, NULL, NULL) <= 0) {
        // Too long for `out`: what fits, at a whole character.
        size_t k = 0;
        int i = 0;
        for (; i < len; i++) {
            char one[8];
            int b = WideCharToMultiByte(CP_UTF8, 0, &w[i], 1, one, sizeof one, NULL, NULL);
            if (b <= 0 || k + (size_t)b + 1 > out_sz) break;
            memcpy(out + k, one, (size_t)b);
            k += (size_t)b;
        }
        out[k] = 0;
    }
}

// The rank icon's name ("rank3", "shiv1") as the game words the rank: its
// m_aRankNames, by the icon's number, which is the ESoldierRanks the icon is
// drawn for.
const char* soldier_rank_word(const char* r)
{
    static const char* ranks[] = {
        "Rookie", "Squaddie", "Corporal", "Sergeant",
        "Lieutenant", "Captain", "Major", "Colonel",
    };
    if (strncmp(r, "shiv", 4) == 0) return loc_or(SOLDIER_LOC_SHIV, 0, "SHIV");
    if (strncmp(r, "rank", 4) != 0) return "";
    int i = atoi(r + 4);
    if (i < 0 || i >= (int)(sizeof ranks / sizeof ranks[0])) return "";
    return loc_or(SOLDIER_LOC_RANK, i, ranks[i]);
}

// "heavy" -> "Heavy", "mech_psi_gene" -> "MEC Trooper, psionic, gene modded":
// the class icon's name as the game names the class (m_aSoldierClassNames,
// by ESoldierClass), then what the icon adds.
void soldier_class_words(const char* c, char* out, size_t out_sz)
{
    static const struct { const char* icon; int cls; const char* english; } k[] = {
        { "sniper", 1, "Sniper" }, { "heavy", 2, "Heavy" }, { "support", 3, "Support" },
        { "assault", 4, "Assault" }, { "mech", 6, "MEC Trooper" },
    };
    out[0] = 0;
    if (!*c || strncmp(c, "none", 4) == 0) return;
    char head[16];
    size_t n = strcspn(c, "_");
    if (n >= sizeof head) n = sizeof head - 1;
    memcpy(head, c, n);
    head[n] = 0;
    const char* name = head;
    for (int i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
        if (strcmp(head, k[i].icon) == 0) name = loc_or(SOLDIER_LOC_CLASS, k[i].cls, k[i].english);
    size_t used = 0;
    _snprintf_s(out, out_sz, _TRUNCATE, "%s", name);
    used = strlen(out);
    if (strstr(c, "_psi")) tfmt_cat(out, out_sz, &used, SOLDIER_PSIONIC);
    if (strstr(c, "_gene")) tfmt_cat(out, out_sz, &used, SOLDIER_GENE_MODDED);
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
    char piece[128];
    // A soldier bleeding out has no actions and no hit points worth saying.
    if (s->wounded == SOLDIER_BLEEDING) {
        if (s->bleed_turns > 0)
            tpfmt(piece, sizeof piece, SOLDIER_BLEEDING_TURNS, s->bleed_turns, s->bleed_turns);
        else
            strncpy_s(piece, sizeof piece, T(SOLDIER_BLEEDING_OUT), _TRUNCATE);
        add(out, out_sz, piece);
        return;
    }
    if (s->wounded == SOLDIER_STABILISED) {
        add(out, out_sz, T(SOLDIER_STABILISED_SAID));
        return;
    }
    if (s->panicked == 1) add(out, out_sz, T(SOLDIER_PANICKED));
    if (s->hp >= 0 && s->hp_max > 0) {
        tfmt(piece, sizeof piece, SOLDIER_HP, s->hp, s->hp_max);
        add(out, out_sz, piece);
    }
    if (s->actions == 0)
        add(out, out_sz, T(SOLDIER_NO_ACTIONS));
    else if (s->actions > 0) {
        tpfmt(piece, sizeof piece, SOLDIER_ACTIONS, s->actions, s->actions);
        add(out, out_sz, piece);
    }
}

static void comma(char* out, size_t out_sz, const char* piece)
{
    size_t used = strlen(out);
    if (piece && *piece)
        _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s", used ? ", " : "", piece);
}

void soldier_squad_words(int actions, int hp, int hp_max, int panicked, int wounded,
                         int bleed_turns, char* out, size_t out_sz)
{
    char piece[128];
    out[0] = 0;
    if (wounded == SOLDIER_BLEEDING) {
        if (bleed_turns > 0)
            tpfmt(piece, sizeof piece, SQUAD_BLEEDING_TURNS, bleed_turns, bleed_turns);
        else
            strncpy_s(piece, sizeof piece, T(SQUAD_BLEEDING_OUT), _TRUNCATE);
        comma(out, out_sz, piece);
        return;
    }
    if (wounded == SOLDIER_STABILISED) { comma(out, out_sz, T(SQUAD_STABILISED)); return; }
    if (actions == 0) comma(out, out_sz, T(SQUAD_NO_ACTIONS));
    else if (actions > 0) {
        tpfmt(piece, sizeof piece, SQUAD_ACTIONS, actions, actions);
        comma(out, out_sz, piece);
    }
    if (hp >= 0 && hp_max > 0) {
        tfmt(piece, sizeof piece, TXT_HP_OF, hp, hp_max);
        comma(out, out_sz, piece);
    }
    if (panicked == 1) comma(out, out_sz, T(SQUAD_PANICKED));
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
    // Made from the item's English type name: a translation can give it (@).
    const char* named = strings_phrase(out);
    if (named != out) strncpy_s(out, out_sz, named, _TRUNCATE);
}

static void squash(const char* in, char* out, size_t out_sz)
{
    size_t n = 0;
    for (; in && *in && n + 1 < out_sz; in++)
        if (isalnum((unsigned char)*in)) out[n++] = (char)tolower((unsigned char)*in);
    out[n] = 0;
}

int soldier_weapon_is_mec(const char* type)
{
    static const char* const mec[] = { "_Chaingun", "_Railgun", "_ParticleBeam" };
    for (int i = 0; type && i < (int)(sizeof mec / sizeof mec[0]); i++)
        if (strcmp(type, mec[i]) == 0) return 1;
    return 0;
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
    char what[160];
    out[0] = 0;
    if (!w || !w->set) return;
    if (w->overheat)
        tfmt(what, sizeof what, WEAPON_OVERHEAT, w->value);
    else if (w->value <= 0)
        strncpy_s(what, sizeof what, T(w->reload ? WEAPON_EMPTY_RELOAD : WEAPON_EMPTY), _TRUNCATE);
    else if (soldier_weapon_is_mec(w->type)) {
        // A MEC's weapon is said as the panel's percentage, never in shots:
        // a shot is 50, a reaction shot 33, Collateral Damage 100, and
        // Expanded Storage cuts each (GetAmmoCost, read from the exe), so a
        // count would be wrong as often as right. The ability menu says what
        // each ability costs.
        if (w->value >= 100)
            strncpy_s(what, sizeof what, T(WEAPON_FULL), _TRUNCATE);
        else
            tfmt(what, sizeof what, WEAPON_AMMO, w->value);
    } else if (w->cost > 0) {
        // Less than one shot's cost is no shot: XGUnit's ammo check refuses
        // a fire when GetRemainingAmmo() < GetAmmoCost(). A MEC weapon's
        // shot costs 50 but its reaction shot 33 (the native
        // GetReactionAmmoCost), so after one the Railgun can sit at 17, and
        // that was said as "1 shot left" though Fire would not go
        // (2026-10-07).
        int shots = w->value / w->cost;
        if (shots < 1)
            tfmt(what, sizeof what, WEAPON_NOT_ENOUGH, w->value);
        else
            tpfmt(what, sizeof what, WEAPON_SHOTS_LEFT, shots, shots);
    } else if (w->value >= 100)
        strncpy_s(what, sizeof what, T(WEAPON_FULL), _TRUNCATE);
    else
        tfmt(what, sizeof what, WEAPON_AMMO, w->value);
    _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s.", name, what);
}

void soldier_weapons(const char* active_name, const SoldierWeapon* w, int n,
                     char* active, size_t active_sz, char* all, size_t all_sz)
{
    soldier_weapons_at(active_name, -1, w, n, active, active_sz, all, all_sz);
}

void soldier_weapons_at(const char* active_name, int active_index, const SoldierWeapon* w,
                        int n, char* active, size_t active_sz, char* all, size_t all_sz)
{
    active[0] = all[0] = 0;
    // The game's own word on which panel is equipped, when there is one.
    // Otherwise the name as shown and the type, which seldom agree letter
    // for letter: "Laser Rifle" is _LaserAssaultRifle, "Light Plasma Rifle"
    // _PlasmaLightRifle. Exactly, then every word of the name somewhere in
    // the type, then the primary weapon.
    int first = active_index >= 0 && active_index < n && w[active_index].set ? active_index : -1;
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

    char cls[160], piece[256];
    soldier_class_words(s->cls, cls, sizeof cls);
    const char* rank = soldier_rank_word(s->rank);
    if (*rank || *cls) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s%s%s.", rank,
                    *rank && *cls ? ", " : "", cls);
        add(out, out_sz, piece);
    }
    if (s->leader == 1) add(out, out_sz, T(SOLDIER_LEADER));
    hp_and_actions(s, out, out_sz);
    add(out, out_sz, s->weapons);
    if (s->aim >= 0) {
        tfmt(piece, sizeof piece, SOLDIER_AIM, s->aim);
        add(out, out_sz, piece);
    }
    if (s->promotion == 1) add(out, out_sz, T(INFO_PROMOTION));
    if (s->buff == 1) add(out, out_sz, T(SOLDIER_BONUSES));
    if (s->debuff == 1) add(out, out_sz, T(SOLDIER_PENALTIES));
}
