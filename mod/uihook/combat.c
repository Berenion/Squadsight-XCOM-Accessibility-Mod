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
