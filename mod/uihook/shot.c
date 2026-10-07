// The shot readout.  See shot.h for the burst this composes.

#include "shot.h"
#include <string.h>
#include <stdio.h>

static struct {
    int  available;
    char name[SHOT_MAX_TEXT];
    char chance[64];
    char crit[64];
    char weapon[SHOT_MAX_TEXT];
    char target[SHOT_MAX_TEXT];
    char self[SHOT_MAX_TEXT];     // shot_set_self
    char said[SHOT_MAX_TEXT];     // what was last announced, verbatim
    char said_weapon[SHOT_MAX_TEXT];
    char said_target[SHOT_MAX_TEXT];
    int  brief;                   // shot_set_brief
} g = { 1 };    // available until told otherwise, as shot_reset leaves it;
                // the DLL never calls shot_reset, so this is its start state

void shot_reset(void)
{
    memset(&g, 0, sizeof g);
    g.available = 1;
}

void shot_set_brief(int on)
{
    g.brief = on;
}

void shot_forget_said(void)
{
    g.said[0] = 0;
    g.said_target[0] = 0;
}

int shot_is_panel(const char* obj_name)
{
    return obj_name && strncmp(obj_name, "UITacticalHUD_InfoPanel", 23) == 0;
}

// Bounds by hand: strcat_s does not truncate, it calls the CRT
// invalid-parameter handler, which __fastfail()s past SEH.
static void join(char* out, size_t out_sz, const char* piece)
{
    if (!piece || !*piece) return;
    size_t used = strlen(out);
    const char* sep = used ? ". " : "";
    size_t want = strlen(sep) + strlen(piece);
    if (used + want + 1 > out_sz) return;
    memcpy(out + used, sep, strlen(sep));
    memcpy(out + used + strlen(sep), piece, strlen(piece));
    out[used + want] = 0;
}

static void set(char* dst, size_t sz, const char* text)
{
    strncpy_s(dst, sz, text ? text : "", _TRUNCATE);
}

// "to hit" + "73%" -> "73% to hit".  The game sends the label first because
// its layout puts the number in a big box with the words beside it; spoken,
// the number comes first or the sentence reads backwards.
static void set_measure(char* dst, size_t sz, const char* label, const char* value)
{
    if (!label || !*label) { dst[0] = 0; return; }
    if (!value || !*value) { set(dst, sz, label); return; }
    _snprintf_s(dst, sz, _TRUNCATE, "%s %s", value, label);
}

// "0%", "0", "0.0 %": a number that is zero, whatever the panel dressed it in.
// Anything without a digit is not a zero -- an empty value is handled apart.
static int is_zero_percent(const char* v)
{
    int digits = 0;
    for (; v && *v; v++) {
        if (*v >= '1' && *v <= '9') return 0;
        if (*v == '0') digits++;
        else if (*v != '.' && *v != '%' && *v != ' ') return 0;
    }
    return digits > 0;
}

void shot_set_target(const char* text)
{
    set(g.target, sizeof g.target, text);
}

void shot_set_self(const char* text)
{
    set(g.self, sizeof g.self, text);
}

// The flag's shield, in words. The flag draws one of four; an EU cover point
// that is neither high nor low sends an empty string, which is left unsaid.
static const char* cover_words(const char* shield)
{
    if (!shield) return NULL;
    if (strcmp(shield, "_highCover") == 0) return "High cover";
    if (strcmp(shield, "_lowCover") == 0)  return "Low cover";
    // Hunker Down (TakeCover), or a unit whose cover is always the best.
    if (strcmp(shield, "_megaCover") == 0) return "Hunkered down";
    if (strcmp(shield, "_none") == 0)      return "No cover";
    return NULL;
}

void shot_describe_target(const ShotTarget* t, char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!t || !t->name || !t->name[0]) return;

    char piece[SHOT_MAX_TEXT];

    // The place in the cycle only when there is a cycle: "1 of 1" says
    // nothing a lone name does not.
    if (t->count > 1 && t->index >= 0 && t->index < t->count)
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s, %d of %d",
                    t->name, t->index + 1, t->count);
    else
        set(piece, sizeof piece, t->name);
    join(out, out_sz, piece);

    // The game sets the flanked state only on a unit that is in cover, so
    // it qualifies the cover rather than standing alone.
    const char* cover = cover_words(t->cover);
    if (cover) {
        if (t->flanked == 1 && strcmp(t->cover, "_none") != 0)
            _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s, flanked", cover);
        else
            set(piece, sizeof piece, cover);
        join(out, out_sz, piece);
    }

    // -1 is the flag hiding an enemy's health (the "show enemy health"
    // option off); nothing is drawn, so nothing is said.
    if (t->hp >= 0 && t->hp_max > 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%d of %d HP", t->hp, t->hp_max);
        join(out, out_sz, piece);
    }
}

// Like join, with a comma: the list's pieces are one phrase, not sentences.
static void join_comma(char* out, size_t out_sz, const char* piece)
{
    if (!piece || !*piece) return;
    size_t used = strlen(out);
    _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s", used ? ", " : "", piece);
}

void shot_list_detail(const ShotTarget* t, int chance, int squadsight,
                      char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = 0;
    if (!t) return;
    char piece[64];

    if (chance >= 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%d%%", chance);
        join_comma(out, out_sz, piece);
    }
    const char* cover = cover_words(t->cover);
    if (cover) {
        // Lower case inside a phrase; the words table is written for the
        // start of a sentence.
        set(piece, sizeof piece, cover);
        piece[0] = (char)(piece[0] | 0x20);
        join_comma(out, out_sz, piece);
        if (t->flanked == 1 && strcmp(t->cover, "_none") != 0)
            join_comma(out, out_sz, "flanked");
    }
    if (t->hp >= 0 && t->hp_max > 0) {
        _snprintf_s(piece, sizeof piece, _TRUNCATE, "%d of %d HP", t->hp, t->hp_max);
        join_comma(out, out_sz, piece);
    }
    if (squadsight) join_comma(out, out_sz, "squadsight");
}

int shot_note(const char* fn, const char* a, const char* b, int flag,
              char* out, size_t out_sz)
{
    if (!out || out_sz == 0 || !fn) return 0;
    out[0] = 0;
    if (!a) a = "";
    if (!b) b = "";

    // Matched on a substring, as everywhere else: the same setter can reach
    // the hook with or without an AS_ prefix depending on which native
    // carried it.
    if (strstr(fn, "SetIsAvailable")) {
        if (flag >= 0) g.available = flag;
        return 0;
    }
    if (strstr(fn, "SetShotName")) {
        set(g.name, sizeof g.name, a);
        return 0;
    }
    if (strstr(fn, "SetShotChance")) {
        set_measure(g.chance, sizeof g.chance, a, b);
        return 0;
    }
    if (strstr(fn, "SetCriticalChance")) {
        // The panel sends a crit chance with every shot, and "0% critical"
        // on nearly all of them buries the number that matters. It is said
        // only when there is a chance.
        if (is_zero_percent(b)) g.crit[0] = 0;
        else set_measure(g.crit, sizeof g.crit, a, b);
        return 0;
    }
    if (strstr(fn, "SetWeaponStats")) {
        set(g.weapon, sizeof g.weapon, a);
        return 0;
    }
    if (!strstr(fn, "UpdateLayout")) return 0;

    // The end of the burst.  The shot itself is composed apart from the
    // weapon, because the two are remembered separately: comparing a line
    // that carries the weapon against one that has already dropped it makes
    // every repeat look like a change.
    char core[SHOT_MAX_TEXT];
    core[0] = 0;

    if (!g.available) {
        // Worth saying plainly. The panel says it by greying out, and a shot
        // that cannot be taken is the one thing a player must not learn by
        // pressing fire.
        join(core, sizeof core, "Unavailable");
    } else {
        // Brief: the ability was named when it was picked, and a step of the
        // aim changes only the odds.
        if (!g.brief || (!g.chance[0] && !g.crit[0])) join(core, sizeof core, g.name);
        // Nothing to aim, so the one thing worth knowing is the key.
        join(core, sizeof core, g.self);
        join(core, sizeof core, g.chance);
        join(core, sizeof core, g.crit);
    }
    if (!core[0]) return 0;

    // The weapon changes far less often than the shot, and reading it every
    // time would bury the number that matters. It is announced when it
    // changes -- which is exactly when switching ability changes the weapon.
    int weapon_is_news = !g.brief && g.weapon[0] && strcmp(g.weapon, g.said_weapon) != 0;

    // The target the same way: said when it changes, which is what Tab does,
    // and not again while the player tries abilities against it. Losing the
    // target is not news on its own -- the shot that has none says so.
    int target_is_news = g.target[0] && strcmp(g.target, g.said_target) != 0;

    // Update runs whenever the targeting state is touched, not only when it
    // changes.
    if (!weapon_is_news && !target_is_news && strcmp(core, g.said) == 0) {
        set(g.said_target, sizeof g.said_target, g.target);
        return 0;
    }

    char full[SHOT_MAX_TEXT];
    full[0] = 0;
    // The target first: it is what the key just changed.
    if (target_is_news) join(full, sizeof full, g.target);
    join(full, sizeof full, core);
    if (weapon_is_news) join(full, sizeof full, g.weapon);

    set(g.said, sizeof g.said, core);
    set(g.said_target, sizeof g.said_target, g.target);
    if (g.weapon[0]) set(g.said_weapon, sizeof g.said_weapon, g.weapon);
    strncpy_s(out, out_sz, full, _TRUNCATE);
    return 1;
}
