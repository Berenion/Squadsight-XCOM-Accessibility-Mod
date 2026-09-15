#pragma once
#include <windows.h>

// What a screen says its buttons do, read back on request.
//
// Every shell screen publishes a help bar -- the row of gamepad glyphs and
// labels along the bottom -- and that bar is the game's own inventory of what
// the screen can do.  It reaches Flash through calls the hook already sees:
//
//     UINavigationHelp.AS_AddLeftButtonHelp(int Id, string Label,
//                                           string Icon, bool Disabled)
//     UINavigationHelp.AS_AddRightButtonHelp(...)   and AddCenter
//     UIOptionsPCScreen.AS_SetHelp(int Index, string Label, string Icon)
//
// each carrying a slot, a label, and the name of the glyph that fires it:
//
//     UINavigationHelp_0.AS_AddRightButtonHelp  "START GAME"  "Icon_START"
//
// That line was in the log the whole time the difficulty screen had no
// keyboard way to start a game, with the answer in it, discarded as an asset
// reference.  Reading the bar back turns finding the next such gap from a
// session with the decompiler into a keypress.
//
// The icon names are a closed set (UI_FxsGamepadIcons), so each maps to the
// command it stands for, and input.c says which key -- if any -- reaches that
// command on this screen.  "No key" is the interesting answer.
//
// This is deliberately read-only.  Firing the chosen command is the same
// rewrite input.c already does, with the target picked at runtime rather than
// from a table, but the list has to prove itself correct on real screens
// before anything acts on it.

#define HELP_MAX_LABEL   128
#define HELP_MAX_ENTRIES 16

// Forgets everything `obj` published -- a bar rebuilding itself
// (UINavigationHelp.ClearButtonHelp, which the hook sees as a text-free call).
void help_clear(void* obj);

// Records one entry.  An empty label clears that slot: a screen with nothing
// to put there publishes AS_SetHelp(1, "", "") rather than skipping the call.
// `icon` may be any string; one that is not a gamepad glyph is kept as an
// entry with no command, because a label without a way to reach it is still
// worth hearing. `disabled` marks an action the screen is currently refusing.
void help_set(void* obj, int slot, const char* label, const char* icon,
              int disabled);

// The command a glyph stands for, or 0 when it is not a button -- the stick
// and d-pad glyphs describe movement rather than an action.
int help_icon_cmd(const char* icon);

// Builds what to say for the screen currently taking input, e.g.
//
//     "START GAME: 3. SECOND WAVE: 2. BACK: Escape."
//
// Returns how many entries it found; 0 means the screen advertises nothing
// and `out` says so.  `screen` is the object name the input hook reports, so
// that the key can be looked up per screen.
int help_announce(const char* screen, char* out, size_t out_sz);

// Drops every bar.  Used by the offline checks.
void help_reset(void);
