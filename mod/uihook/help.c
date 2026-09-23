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
    // Not the game's: main.c gives it to "Previous soldier", whose mouse-mode
    // bar sends a frame number and no glyph.
    { "Icon_KEY_LEFT_SHIFT", FXS_KEY_LEFT_SHIFT },
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
        case FXS_KEY_TAB:        return "Tab";
        case FXS_KEY_LEFT_SHIFT: return "Left Shift";
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

// One thing the player could choose, whether or not a key reaches it.  The
// list is snapshotted when the menu opens: a screen that republishes its bar
// while the menu is up must not move the highlight out from under the hand on
// the arrow key.
typedef struct {
    char text[HELP_MAX_LABEL + 64];   // "START GAME: 3"
    int  cmd;
    int  disabled;
} Choice;

#define HELP_MAX_CHOICES (HELP_BARS * HELP_MAX_ENTRIES)

static Choice g_choices[HELP_MAX_CHOICES];
static int    g_nchoices;

// Gathers what `screen` offers: what its bars advertise, then the keys this
// mod adds that the bars never mentioned.
static int collect(const char* screen)
{
    g_nchoices = 0;

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

    for (int i = 0; i < n; i++) {
        for (int s = 0; s < current[i]->count; s++) {
            const Entry* e = &current[i]->entries[s];
            if (!e->label[0]) continue;
            if (g_nchoices >= HELP_MAX_CHOICES) break;
            const char* key = key_for(screen, e->cmd);
            Choice* c = &g_choices[g_nchoices++];
            _snprintf_s(c->text, sizeof c->text, _TRUNCATE, "%s: %s%s",
                        e->label, key ? key : "no key",
                        e->disabled ? ", unavailable" : "");
            c->cmd = e->cmd;
            c->disabled = e->disabled;
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
        if (g_nchoices >= HELP_MAX_CHOICES) break;

        int already = 0;
        for (int j = 0; j < g_nchoices; j++)
            if (g_choices[j].cmd == cmd) { already = 1; break; }
        if (already) continue;

        const char* name = input_cmd_name(key);
        if (!name || !what) continue;
        Choice* c = &g_choices[g_nchoices++];
        _snprintf_s(c->text, sizeof c->text, _TRUNCATE, "%s: %s", what, name);
        c->cmd = cmd;
        c->disabled = 0;
    }

    return g_nchoices;
}

int help_announce(const char* screen, char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = 0;

    int found = collect(screen);
    for (int i = 0; i < found; i++)
        append(out, out_sz, g_choices[i].text);

    if (!found)
        strncpy_s(out, out_sz, "This screen lists no commands.", _TRUNCATE);
    return found;
}

// ------------------------------------------------------------- the menu --

static int  g_open;
static int  g_cursor;
static char g_screen[128];

int help_menu_is_open(void) { return g_open; }

void help_menu_close(void)
{
    g_open = 0;
    g_cursor = 0;
    g_screen[0] = 0;
}

int help_menu_open(const char* screen, char* out, size_t out_sz)
{
    int found = help_announce(screen, out, out_sz);
    if (!found) {
        help_menu_close();
        return 0;
    }
    g_open = 1;
    g_cursor = 0;
    strncpy_s(g_screen, sizeof g_screen, screen ? screen : "", _TRUNCATE);
    return found;
}

// Says where the highlight is.  The position is worth stating: without the
// row of glyphs on screen there is nothing else to say how far through the
// list this is.
static void say_cursor(char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s. %d of %d",
                g_choices[g_cursor].text, g_cursor + 1, g_nchoices);
}

int help_menu_key(const char* screen, int cmd, int* fire,
                  char* out, size_t out_sz)
{
    if (!g_open || !out || out_sz == 0) return HELP_MENU_PASS;
    out[0] = 0;

    // The list belongs to one screen. If the player has left it -- by firing
    // something that moved them, or by any route the hook did not see -- the
    // menu is stale and must not swallow that screen's keys.
    if (!screen || strcmp(screen, g_screen) != 0) {
        help_menu_close();
        return HELP_MENU_PASS;
    }

    switch (cmd) {
        // Any axis moves: up and left go back, down and right go on. Which
        // one a screen would have used does not matter, because none of this
        // reaches the screen.
        case FXS_DPAD_UP:    case FXS_ARROW_UP:    case FXS_LSTICK_UP:
        case FXS_DPAD_LEFT:  case FXS_ARROW_LEFT:  case FXS_LSTICK_LEFT:
            g_cursor = (g_cursor + g_nchoices - 1) % g_nchoices;
            say_cursor(out, out_sz);
            return HELP_MENU_SPEAK;

        case FXS_DPAD_DOWN:  case FXS_ARROW_DOWN:  case FXS_LSTICK_DOWN:
        case FXS_DPAD_RIGHT: case FXS_ARROW_RIGHT: case FXS_LSTICK_RIGHT:
            g_cursor = (g_cursor + 1) % g_nchoices;
            say_cursor(out, out_sz);
            return HELP_MENU_SPEAK;

        case FXS_BUTTON_A: case FXS_KEY_ENTER: case FXS_KEY_SPACEBAR: {
            const Choice* c = &g_choices[g_cursor];
            if (c->disabled) {
                _snprintf_s(out, out_sz, _TRUNCATE, "%s is unavailable", c->text);
                return HELP_MENU_SPEAK;
            }
            if (!c->cmd) {
                // A glyph with no command behind it -- movement, or one this
                // table does not know. Saying so beats a keypress that
                // silently does nothing.
                _snprintf_s(out, out_sz, _TRUNCATE,
                            "%s cannot be pressed from here", c->text);
                return HELP_MENU_SPEAK;
            }
            if (fire) *fire = c->cmd;
            help_menu_close();
            return HELP_MENU_FIRE;
        }

        case FXS_BUTTON_B: case FXS_KEY_ESCAPE:
            help_menu_close();
            strncpy_s(out, out_sz, "Menu closed", _TRUNCATE);
            return HELP_MENU_SPEAK;

        case FXS_KEY_0:
            help_menu_close();
            strncpy_s(out, out_sz, "Menu closed", _TRUNCATE);
            return HELP_MENU_SPEAK;

        default:
            // Everything else is swallowed rather than passed on. While the
            // menu is up the player is talking to it, and a key that reached
            // the screen underneath would act unseen. Escape is always the
            // way out.
            return HELP_MENU_QUIET;
    }
}
