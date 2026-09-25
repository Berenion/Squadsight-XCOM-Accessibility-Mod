#pragma once
#include <windows.h>

// Giving the keyboard access to actions the game only bound to a gamepad.
//
// Three screens in the shell put real functionality behind a controller
// button and never provided a keyboard equivalent:
//
//     UIShellDifficulty     case 302 (X)     -> OnToggleAdvancedOptions()
//                           case 303 (Y)     -> OnShowGameplayToggles()
//                           case 321 (Start) -> OnDifficultyConfirm()   (EW)
//     UIOptionsPCScreen     case 330 (LB)    -> previous tab
//                           case 331 (RB)    -> next tab
//                           case 302 (X)     -> SaveAndExit()
//
// so Ironman, the tutorial toggle, Second Wave, every options tab past the
// first, saving the options at all, and on EW starting the game once the
// difficulty is chosen, are unreachable without a controller.  That the omission is an
// oversight rather than a policy is visible two cases further down the same
// switch, where cancel is bound to both the B button and Escape.
//
// Nothing here synthesises input.  Each of these handlers opens with
//
//     if(CheckInputIsReleaseOrDirectionRepeat(Cmd, Arg)!) return true;
//
// which is native, so the hook catches a keypress the game is *already*
// dispatching and rewrites the integer before the switch below reads it.
// That is why no gamepad has to be connected and why the Input Device setting
// does not matter: the event exists either way, only its label changes.
//
// The mapping is per screen because the same keys are already spoken for
// elsewhere -- Q and E cycle targets on the tactical HUD, and remapping them
// everywhere would break something that works today.

// UI_FxsInput's own constants, shared because help.c has to name the same
// commands from the other direction -- the gamepad glyph a screen publishes.
#define FXS_BUTTON_A        300
#define FXS_BUTTON_B        301
#define FXS_BUTTON_X        302
#define FXS_BUTTON_Y        303
#define FXS_BUTTON_SELECT   320
#define FXS_BUTTON_START    321
#define FXS_BUTTON_LBUMPER  330
#define FXS_BUTTON_RBUMPER  331
#define FXS_BUTTON_LTRIGGER 332
#define FXS_BUTTON_RTRIGGER 333

#define FXS_DPAD_UP         350
#define FXS_DPAD_RIGHT      352
#define FXS_DPAD_DOWN       354
#define FXS_DPAD_LEFT       356

#define FXS_LSTICK_UP       370
#define FXS_LSTICK_DOWN     371
#define FXS_LSTICK_LEFT     372
#define FXS_LSTICK_RIGHT    373

#define FXS_ARROW_UP        500
#define FXS_ARROW_RIGHT     501
#define FXS_ARROW_DOWN      502
#define FXS_ARROW_LEFT      503

#define FXS_KEY_ESCAPE      510
#define FXS_KEY_ENTER       511
#define FXS_KEY_LEFT_SHIFT  514
#define FXS_KEY_SPACEBAR    513
#define FXS_KEY_TAB         571
#define FXS_KEY_F1          600
#define FXS_KEY_1           612
#define FXS_KEY_2           613
#define FXS_KEY_3           614
#define FXS_KEY_4           615
#define FXS_KEY_6           617
#define FXS_KEY_7           618
#define FXS_KEY_0           621

#define FXS_ACTION_PRESS    1
#define FXS_ACTION_RELEASE  32

// Returns the command to substitute for `cmd` on `screen`, or 0 to leave it
// alone.  `screen` is the object name the frame reports, e.g.
// "UIShellDifficulty_0", so it is matched by prefix.
int input_remap(const char* screen, int cmd);

// The other direction: which key reaches `cmd` on `screen`, or 0 when no key
// does.  The help bar names a command by its gamepad glyph, and what a player
// on a keyboard needs to hear is which key stands in for it -- or that none
// does, which is the whole point of listing them.
int input_key_for(const char* screen, int cmd);

// Walks the keys this table adds on `screen`, so that they can be read out
// beside the ones the screen advertises.  Returns 0 once `index` is past the
// end.
//
// They have to be listed separately because the help bar does not mention
// them: it shows what the *game* offers, and the game never offered these.
// UIShellDifficulty.UpdateButtonHelp publishes Second Wave and Start and
// nothing about the advanced options behind X, and the options tabs go
// through AS_SetTabHelp, which carries no glyph at all.  `what` is a phrase
// rather than a label for the same reason: there was no label to take.
int input_added_key(const char* screen, int index, int* key, int* cmd,
                    const char** what);

// Names a command for the log, e.g. "X" or "RB"; NULL if it is not one we
// deal in.
const char* input_cmd_name(int cmd);
