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
    char said[SHOT_MAX_TEXT];     // what was last announced, verbatim
    char said_weapon[SHOT_MAX_TEXT];
} g = { 1 };    // available until told otherwise, as shot_reset leaves it;
                // the DLL never calls shot_reset, so this is its start state

void shot_reset(void)
{
    memset(&g, 0, sizeof g);
    g.available = 1;
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
        join(core, sizeof core, g.name);
        join(core, sizeof core, g.chance);
        join(core, sizeof core, g.crit);
    }
    if (!core[0]) return 0;

    // The weapon changes far less often than the shot, and reading it every
    // time would bury the number that matters. It is announced when it
    // changes -- which is exactly when switching ability changes the weapon.
    int weapon_is_news = g.weapon[0] && strcmp(g.weapon, g.said_weapon) != 0;

    // Update runs whenever the targeting state is touched, not only when it
    // changes.
    if (!weapon_is_news && strcmp(core, g.said) == 0) return 0;

    char full[SHOT_MAX_TEXT];
    full[0] = 0;
    join(full, sizeof full, core);
    if (weapon_is_news) join(full, sizeof full, g.weapon);

    set(g.said, sizeof g.said, core);
    if (g.weapon[0]) set(g.said_weapon, sizeof g.said_weapon, g.weapon);
    strncpy_s(out, out_sz, full, _TRUNCATE);
    return 1;
}
