// Floating combat text, in words. See combat.h.

#include "combat.h"
#include "strings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int combat_describe(const char* who, const char* text, int damage, const char* crit_word,
                    char* out, size_t out_sz)
{
    out[0] = 0;
    if (!text) return 0;
    while (*text == ' ') text++;
    if (!*text) return 0;
    const char* sep = who && *who ? ", " : "";
    if (!who) who = "";

    if (damage) {
        // The figure, then whatever the screen put beside it. Only the
        // critical mark is known to be there (XGUnit's
        // m_sCriticalHitDamageDisplay, "CRITICAL!" in English, which main.c
        // reads in the game's language); anything else is kept as it came.
        char* end;
        long n = strtol(text, &end, 10);
        if (end != text) {
            while (*end == ' ') end++;
            int crit = crit_word && *crit_word && text_find_ci(end, crit_word) != NULL;
            if (*who) tfmt(out, out_sz, crit ? COMBAT_WHO_DAMAGE_CRIT : COMBAT_WHO_DAMAGE, who, n);
            else      tfmt(out, out_sz, crit ? COMBAT_DAMAGE_CRIT : COMBAT_DAMAGE, n);
            return 1;
        }
    }

    // Anything else is said as it is written, with a full stop if it ends in
    // none, so the speech does not run it into what follows.
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s%s", who, sep, text);
    text_end_sentence(out, out_sz);
    return 1;
}

// ---- the turn --------------------------------------------------------------

enum { TURN_UNKNOWN, TURN_XCOM, TURN_ALIEN, TURN_OTHER };

static struct {
    int  whose;
    char text[4][128];          // indexed by the enum; the banner's own words
} t;

void combat_turn_reset(void)
{
    memset(&t, 0, sizeof t);
}

// "ALIEN ACTIVITY" -> "Alien activity.": capitals would be spelt out.
static void sentence(const char* in, char* out, size_t out_sz)
{
    text_sentence_case(in, out, out_sz);
    text_end_sentence(out, out_sz);
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

    static const StrId fallback[] = { TXT_EMPTY, COMBAT_YOUR_TURN, COMBAT_ALIEN_TURN,
                                      COMBAT_OPPONENT_TURN };
    sentence(t.text[now][0] ? t.text[now] : T(fallback[now]), out, out_sz);
    return out[0] != 0;
}

int combat_hp(int hp, int hp_max, char* out, size_t out_sz)
{
    out[0] = 0;
    if (hp < 0 || hp_max <= 0) return 0;
    if (hp == 0)
        strncpy_s(out, out_sz, T(COMBAT_NO_HP), _TRUNCATE);
    else
        tfmt(out, out_sz, COMBAT_HP_LEFT, hp, hp_max);
    return 1;
}

void combat_unit_state(int hp, int hp_max, int overwatch, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    if (hp >= 0 && hp_max > 0) {
        tfmt_cat(out, out_sz, &used, COMBAT_UNIT_HP, hp, hp_max);
    }
    if (overwatch && used < out_sz)
        tfmt_cat(out, out_sz, &used, COMBAT_ON_OVERWATCH);
}
