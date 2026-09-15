// Which keys stand in for which gamepad buttons, and on which screens.

#include "input.h"
#include <string.h>

// UI_FxsInput constants, by name so the table below reads as the game's own.
#define FXS_BUTTON_X        302
#define FXS_BUTTON_Y        303
#define FXS_BUTTON_LBUMPER  330
#define FXS_BUTTON_RBUMPER  331

#define FXS_KEY_TAB         571
#define FXS_KEY_1           612
#define FXS_KEY_2           613

typedef struct {
    const char* screen;   // matched by prefix; the game appends _0, _1, ...
    int         from;     // the key actually pressed
    int         to;       // the command the screen is waiting for
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
    { "UIShellDifficulty", FXS_KEY_1,   FXS_BUTTON_X       },
    { "UIShellDifficulty", FXS_KEY_2,   FXS_BUTTON_Y       },

    // Every options tab after the first is behind the bumpers.  Tab cycles
    // forward and wraps, and is already proven to arrive; 1 goes back.
    { "UIOptionsPCScreen", FXS_KEY_TAB, FXS_BUTTON_RBUMPER },
    { "UIOptionsPCScreen", FXS_KEY_1,   FXS_BUTTON_LBUMPER },
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

const char* input_cmd_name(int cmd)
{
    switch (cmd) {
        case FXS_BUTTON_X:       return "X";
        case FXS_BUTTON_Y:       return "Y";
        case FXS_BUTTON_LBUMPER: return "LB";
        case FXS_BUTTON_RBUMPER: return "RB";
        case FXS_KEY_TAB:        return "Tab";
        case FXS_KEY_1:          return "1";
        case FXS_KEY_2:          return "2";
        default:                 return NULL;
    }
}
