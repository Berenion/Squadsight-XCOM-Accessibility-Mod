// Floating combat text, in words. See combat.h.

#include "combat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Case-insensitive strstr, ASCII only.
static const char* find_ci(const char* hay, const char* needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return hay;
    }
    return NULL;
}

int combat_describe(const char* who, const char* text, int damage,
                    char* out, size_t out_sz)
{
    out[0] = 0;
    if (!text) return 0;
    while (*text == ' ') text++;
    if (!*text) return 0;
    const char* sep = who && *who ? ", " : "";
    if (!who) who = "";

    if (damage) {
        // The figure, then whatever the screen put beside it. Only CRITICAL!
        // is known to be there; anything else is kept as it came.
        char* end;
        long n = strtol(text, &end, 10);
        if (end != text) {
            while (*end == ' ') end++;
            int crit = find_ci(end, "critical") != NULL;
            _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%ld damage%s.", who, sep, n,
                        crit ? ", critical" : "");
            return 1;
        }
    }

    // Anything else is said as it is written, with a full stop if it ends in
    // none, so the speech does not run it into what follows.
    size_t len = strlen(text);
    char last = text[len - 1];
    int stop = last == '.' || last == '!' || last == '?';
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s%s", who, sep, text, stop ? "" : ".");
    return 1;
}

// ---- the turn --------------------------------------------------------------

enum { TURN_UNKNOWN, TURN_XCOM, TURN_ALIEN, TURN_OTHER };

static struct {
    int  whose;
    char text[4][64];           // indexed by the enum; the banner's own words
} t;

void combat_turn_reset(void)
{
    memset(&t, 0, sizeof t);
}

// "ALIEN ACTIVITY" -> "Alien activity.": capitals would be spelt out.
static void sentence(const char* in, char* out, size_t out_sz)
{
    size_t n = 0;
    for (; in[n] && n + 2 < out_sz; n++)
        out[n] = (char)(n == 0 ? toupper((unsigned char)in[n])
                               : tolower((unsigned char)in[n]));
    out[n] = 0;
    if (n && out[n - 1] != '.' && out[n - 1] != '!' && n + 1 < out_sz) {
        out[n] = '.';
        out[n + 1] = 0;
    }
}

int combat_turn(const char* fn, const char* const* strings, int nstrings,
                char* out, size_t out_sz)
{
    out[0] = 0;
    if (!fn) return 0;
    if (strcmp(fn, "SetDisplayText") == 0) {
        // (alien, xcom, other), in that order, each a separate argument; an
        // empty one does not arrive at all, so only a full set is taken.
        // Sent from the banner's OnInit, once a mission: a new mission starts
        // with nobody's turn known, or one that ended on the aliens' turn
        // would swallow the next mission's first "Alien activity".
        t.whose = TURN_UNKNOWN;
        if (nstrings >= 3) {
            strncpy_s(t.text[TURN_ALIEN], sizeof t.text[0], strings[0], _TRUNCATE);
            strncpy_s(t.text[TURN_XCOM], sizeof t.text[0], strings[1], _TRUNCATE);
            strncpy_s(t.text[TURN_OTHER], sizeof t.text[0], strings[2], _TRUNCATE);
        }
        return 0;
    }

    int now;
    if (strcmp(fn, "ShowAlienTurn") == 0 || strcmp(fn, "PulseAlienTurn") == 0)
        now = TURN_ALIEN;
    else if (strcmp(fn, "HideAlienTurn") == 0 || strcmp(fn, "HideOtherTurn") == 0 ||
             strcmp(fn, "PulseXComTurn") == 0 || strcmp(fn, "ShowXComTurn") == 0)
        now = TURN_XCOM;
    else if (strcmp(fn, "PulseOtherTurn") == 0 || strcmp(fn, "ShowOtherTurn") == 0)
        now = TURN_OTHER;
    else
        return 0;               // HideXComTurn is only the pulse fading
    if (now == t.whose) return 0;
    t.whose = now;

    static const char* fallback[] = { "", "Your turn", "Alien activity", "Opponent's turn" };
    sentence(t.text[now][0] ? t.text[now] : fallback[now], out, out_sz);
    return out[0] != 0;
}

int combat_hp(int hp, int hp_max, char* out, size_t out_sz)
{
    out[0] = 0;
    if (hp < 0 || hp_max <= 0) return 0;
    if (hp == 0)
        _snprintf_s(out, out_sz, _TRUNCATE, "No HP left.");
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%d of %d HP left.", hp, hp_max);
    return 1;
}
