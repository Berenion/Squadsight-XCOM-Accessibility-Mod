// Which keys stand in for which gamepad buttons, and on which screens.

#include "input.h"
#include <string.h>

// The UI_FxsInput constants live in input.h, so that help.c can name the same
// commands coming the other way -- off a screen's help bar.

typedef struct {
    const char* screen;   // matched by prefix; the game appends _0, _1, ...
    int         from;     // the key actually pressed
    int         to;       // the command the screen is waiting for
    const char* what;     // what it does, for the list 0 reads out
} Remap;

// Only a handful of keys exist in a menu at all, and which ones is decided by
// the ini *section*, not by the key.  XComInput.ini binds every letter --
//
//     [XComGame.XComTacticalInput]
//     Bindings=(Name="X", Command="X_Key_Press | onrelease X_Key_Release")
//
// -- but that class is only live during a mission.  In the shell the active
// set is [Engine.PlayerInput], which binds the arrows (with WASD as aliases
// for them, not as letters), Enter, Space, Escape, Tab, LeftShift, F1, F12,
// the digits, and the gamepad.  Nothing else.
//
// So a letter key in a menu is not merely unbound: it never becomes a UI
// command, and remapping it is dead code.  That is how both earlier attempts
// failed, first with Q and E, then with X and Y -- each time reading a
// binding that was real but belonged to the tactical class.  Confirmed live:
// on the difficulty screen only 500-503, 510, 511 and 571 ever arrive.
//
// The digits are the free ones.  N1_Key_Press sends 612 and no shell screen
// listens for it.
static const Remap g_remaps[] = {
    // Ironman and the tutorial toggle live behind X; Second Wave behind Y.
    { "UIShellDifficulty", FXS_KEY_1,   FXS_BUTTON_X,       "Advanced options" },
    { "UIShellDifficulty", FXS_KEY_2,   FXS_BUTTON_Y,       "Second Wave" },

    // Starting the game is behind Start, which no keyboard has.  EW moved it
    // there: EU confirms on `case 300: case 511: OnDifficultyConfirm()`, and
    // EW's switch has neither, only `case 321`.  So on EW the difficulty
    // screen could be read and set and never left -- Enter and Space are
    // taken by the checkbox under the cursor (OnUnrealCommand_Checkbox calls
    // ToggleCheckbox for 300, 511 and 513, and the widget helper gets the
    // command first), and the only other way through is clicking the help
    // bar with the mouse.
    //
    // Harmless on EU, where 321 matches no case at all and 3 does nothing
    // either way, so the table stays one list rather than growing a build
    // column for a single row.
    { "UIShellDifficulty", FXS_KEY_3,   FXS_BUTTON_START,   "Start the game" },

    // Every options tab after the first is behind the bumpers.  Tab cycles
    // forward and wraps, and is already proven to arrive; 1 goes back.
    { "UIOptionsPCScreen", FXS_KEY_TAB, FXS_BUTTON_RBUMPER, "Next tab" },
    { "UIOptionsPCScreen", FXS_KEY_1,   FXS_BUTTON_LBUMPER, "Previous tab" },

    // Saving is behind X, and only behind X (UIOptionsPCScreen, case 302 ->
    // SaveAndExit).  Escape is not an alternative: it runs
    // IgnoreChangesAndExit, whose prompt offers "EXIT WITHOUT CHANGES" and
    // "BACK TO OPTIONS" and nothing else.  Without this the keyboard can
    // change a setting and be told, correctly, that its only two choices are
    // to throw the change away or go back and look at it again.
    { "UIOptionsPCScreen", FXS_KEY_2,   FXS_BUTTON_X,       "Save changes and exit" },
};

int input_remap(const char* screen, int cmd)
{
    if (!screen) return 0;
    for (int i = 0; i < (int)(sizeof g_remaps / sizeof *g_remaps); i++) {
        const Remap* r = &g_remaps[i];
        if (r->from != cmd) continue;
        if (strncmp(screen, r->screen, strlen(r->screen)) == 0) return r->to;
    }
    return 0;
}

int input_added_key(const char* screen, int index, int* key, int* cmd,
                    const char** what)
{
    if (!screen || index < 0) return 0;
    for (int i = 0; i < (int)(sizeof g_remaps / sizeof *g_remaps); i++) {
        const Remap* r = &g_remaps[i];
        if (strncmp(screen, r->screen, strlen(r->screen)) != 0) continue;
        if (index--) continue;
        if (key)  *key  = r->from;
        if (cmd)  *cmd  = r->to;
        if (what) *what = r->what;
        return 1;
    }
    return 0;
}

int input_key_for(const char* screen, int cmd)
{
    if (!screen || !cmd) return 0;
    for (int i = 0; i < (int)(sizeof g_remaps / sizeof *g_remaps); i++) {
        const Remap* r = &g_remaps[i];
        if (r->to != cmd) continue;
        if (strncmp(screen, r->screen, strlen(r->screen)) == 0) return r->from;
    }
    return 0;
}

const char* input_cmd_name(int cmd)
{
    switch (cmd) {
        case FXS_BUTTON_X:       return "X";
        case FXS_BUTTON_Y:       return "Y";
        case FXS_BUTTON_START:   return "Start";
        case FXS_BUTTON_LBUMPER: return "LB";
        case FXS_BUTTON_RBUMPER: return "RB";
        case FXS_KEY_TAB:        return "Tab";
        case FXS_KEY_1:          return "1";
        case FXS_KEY_2:          return "2";
        case FXS_KEY_3:          return "3";
        default:                 return NULL;
    }
}
