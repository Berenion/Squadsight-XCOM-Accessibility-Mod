#pragma once
#include <windows.h>

// Giving the keyboard access to actions the game only bound to a gamepad.
//
// Three screens in the shell put real functionality behind a controller
// button and never provided a keyboard equivalent:
//
//     UIShellDifficulty     case 302 (X)  -> OnToggleAdvancedOptions()
//                           case 303 (Y)  -> OnShowGameplayToggles()
//     UIOptionsPCScreen     case 330 (LB) -> previous tab
//                           case 331 (RB) -> next tab
//
// so Ironman, the tutorial toggle, Second Wave and every options tab past the
// first are unreachable without a controller.  That the omission is an
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

// Returns the command to substitute for `cmd` on `screen`, or 0 to leave it
// alone.  `screen` is the object name the frame reports, e.g.
// "UIShellDifficulty_0", so it is matched by prefix.
int input_remap(const char* screen, int cmd);

// Names a command for the log, e.g. "X" or "RB"; NULL if it is not one we
// deal in.
const char* input_cmd_name(int cmd);
