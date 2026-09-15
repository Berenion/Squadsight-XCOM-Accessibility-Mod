// The help bar, read back on request.  See help.h for why this is the list
// worth reading.

#include "help.h"
#include "input.h"
#include <string.h>
#include <stdio.h>

// A screen publishes through one or two bars (UIShellDifficulty keeps
// m_kHelpBar and m_kHelpBar2, which arrive as UINavigationHelp_0 and _1) and
// some screens publish through themselves instead, so the table is keyed by
// whichever object made the call.
#define HELP_BARS 8

typedef struct {
    char label[HELP_MAX_LABEL];
    char icon[64];
    int  cmd;
    int  disabled;
} Entry;

typedef struct {
    void*     obj;
    ULONGLONG touched;
    int       count;
    Entry     entries[HELP_MAX_ENTRIES];
} Bar;

static Bar g_bars[HELP_BARS];

// Bars published this far apart belong to different screens.  The test is
// relative -- how far one bar is from the newest, not how old either is --
// so a screen sat on for ten minutes still reads both of its bars, while the
// screen underneath it does not contribute.
#define HELP_WINDOW_MS 1500

static Bar* bar_for(void* obj, int create)
{
    Bar* oldest = &g_bars[0];
    for (int i = 0; i < HELP_BARS; i++) {
        if (g_bars[i].obj == obj) return &g_bars[i];
        if (g_bars[i].touched < oldest->touched) oldest = &g_bars[i];
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof *oldest);
    oldest->obj = obj;
    return oldest;
}

void help_reset(void)
{
    memset(g_bars, 0, sizeof g_bars);
}

void help_clear(void* obj)
{
    if (!obj) return;
    Bar* b = bar_for(obj, 1);
    ULONGLONG when = b->touched;
    memset(b, 0, sizeof *b);
    b->obj = obj;
    // A cleared bar keeps its place in time: the screen is about to refill it,
    // and forgetting when it was last seen would make it look older than the
    // bar beside it.
    b->touched = when;
}

// Every glyph UI_FxsGamepadIcons defines, and the command each one fires.
// The movement glyphs (d-pad, sticks) map to nothing on purpose: they
// describe how to move around a screen, not something the screen will do.
static const struct { const char* icon; int cmd; } g_icons[] = {
    { "Icon_A_X",          FXS_BUTTON_A        },
    { "Icon_B_CIRCLE",     FXS_BUTTON_B        },
    { "Icon_X_SQUARE",     FXS_BUTTON_X        },
    { "Icon_Y_TRIANGLE",   FXS_BUTTON_Y        },
    { "Icon_START",        FXS_BUTTON_START    },
    { "Icon_BACK_SELECT",  FXS_BUTTON_SELECT   },
    { "Icon_LB_L1",        FXS_BUTTON_LBUMPER  },
    { "Icon_RB_R1",        FXS_BUTTON_RBUMPER  },
    { "Icon_LT_L2",        FXS_BUTTON_LTRIGGER },
    { "Icon_RT_R2",        FXS_BUTTON_RTRIGGER },
    { "Icon_KEY_TAB",      FXS_KEY_TAB         },
};

int help_icon_cmd(const char* icon)
{
    if (!icon || !*icon) return 0;
    for (int i = 0; i < (int)(sizeof g_icons / sizeof *g_icons); i++)
        if (strcmp(icon, g_icons[i].icon) == 0) return g_icons[i].cmd;
    return 0;
}

void help_set(void* obj, int slot, const char* label, const char* icon,
              int disabled)
{
    if (!obj || slot < 0 || slot >= HELP_MAX_ENTRIES) return;

    Bar* b = bar_for(obj, 1);
    b->touched = GetTickCount64();

    Entry* e = &b->entries[slot];
    if (!label || !*label) {            // the screen emptying that slot
        memset(e, 0, sizeof *e);
    } else {
        strncpy_s(e->label, sizeof e->label, label, _TRUNCATE);
        strncpy_s(e->icon, sizeof e->icon, icon ? icon : "", _TRUNCATE);
        e->cmd = help_icon_cmd(e->icon);
        e->disabled = disabled != 0;
    }
    if (slot >= b->count) b->count = slot + 1;
}

// What to press on a keyboard to reach `cmd`, or NULL when nothing does.
//
// A and B are the two the keyboard already has, everywhere in the shell: the
// screens bind them in the same case as Enter and Escape --
//
//     case 301: case 510: case 405:  OnUCancel();
//
// -- so naming them costs nothing and saves listing a command as unreachable
// when it is the one thing every player can already do.  Everything else has
// a key only where input.c puts one, and saying so is the point of the list.
static const char* key_for(const char* screen, int cmd)
{
    switch (cmd) {
        case FXS_BUTTON_A:     return "Enter";
        case FXS_KEY_ENTER:    return "Enter";
        case FXS_KEY_SPACEBAR: return "Space";
        case FXS_BUTTON_B:     return "Escape";
        case FXS_KEY_ESCAPE:   return "Escape";
        default: break;
    }
    return input_cmd_name(input_key_for(screen, cmd));
}

static void append(char* out, size_t out_sz, const char* piece)
{
    size_t used = strlen(out);
    const char* sep = used ? ". " : "";
    size_t want = strlen(sep) + strlen(piece);
    if (used + want + 1 > out_sz) return;   // bounds by hand: strcat_s kills
    memcpy(out + used, sep, strlen(sep));
    memcpy(out + used + strlen(sep), piece, strlen(piece));
    out[used + want] = 0;
}

int help_announce(const char* screen, char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = 0;

    ULONGLONG newest = 0;
    for (int i = 0; i < HELP_BARS; i++)
        if (g_bars[i].obj && g_bars[i].count && g_bars[i].touched > newest)
            newest = g_bars[i].touched;

    // The current screen's bars, oldest first, so the list comes out in the
    // order the screen published it rather than the order the table happens
    // to hold. At most HELP_BARS of them, so an insertion sort is the whole
    // algorithm.
    Bar* current[HELP_BARS];
    int  n = 0;
    for (int i = 0; newest && i < HELP_BARS; i++) {
        Bar* b = &g_bars[i];
        if (!b->obj || !b->count) continue;
        if (newest - b->touched > HELP_WINDOW_MS) continue;
        int at = n++;
        while (at > 0 && current[at - 1]->touched > b->touched) {
            current[at] = current[at - 1];
            at--;
        }
        current[at] = b;
    }

    int found = 0;
    int listed[HELP_BARS * HELP_MAX_ENTRIES];
    int nlisted = 0;

    for (int i = 0; i < n; i++) {
        for (int s = 0; s < current[i]->count; s++) {
            const Entry* e = &current[i]->entries[s];
            if (!e->label[0]) continue;
            const char* key = key_for(screen, e->cmd);
            char line[HELP_MAX_LABEL + 64];
            _snprintf_s(line, sizeof line, _TRUNCATE, "%s: %s%s",
                        e->label, key ? key : "no key",
                        e->disabled ? ", unavailable" : "");
            append(out, out_sz, line);
            if (e->cmd && nlisted < (int)(sizeof listed / sizeof *listed))
                listed[nlisted++] = e->cmd;
            found++;
        }
    }

    // Then the keys the mod adds that the screen said nothing about.  The bar
    // is what the *game* offers, and the game never offered these: the
    // advanced options behind X are absent from
    // UIShellDifficulty.UpdateButtonHelp, and the options tabs go through
    // AS_SetTabHelp, which carries no glyph.  Without this the list is
    // confidently incomplete, which is worse than short.
    //
    // An added key whose command the bar already named is not repeated --
    // "SECOND WAVE: 2" has said it.
    for (int i = 0;; i++) {
        int key = 0, cmd = 0;
        const char* what = NULL;
        if (!input_added_key(screen, i, &key, &cmd, &what)) break;

        int already = 0;
        for (int j = 0; j < nlisted; j++)
            if (listed[j] == cmd) { already = 1; break; }
        if (already) continue;

        const char* name = input_cmd_name(key);
        if (!name || !what) continue;
        char line[HELP_MAX_LABEL + 64];
        _snprintf_s(line, sizeof line, _TRUNCATE, "%s: %s", what, name);
        append(out, out_sz, line);
        found++;
    }

    if (!found)
        strncpy_s(out, out_sz, "This screen lists no commands.", _TRUNCATE);
    return found;
}
