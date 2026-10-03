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
// Reading the list is half of it; the menu below is the other half, because
// most of what the list names has no key to press.

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

// ------------------------------------------------------------- the menu --
//
// Reading the list out is half the job; the other half is that most of what
// it names cannot be pressed.  So the same key opens a menu over the list:
// arrows move a highlight, Enter runs the highlighted command, Escape closes.
//
// Nothing here synthesises input.  Firing is the rewrite input.c already
// does, with the target chosen at runtime instead of from a table -- the
// player's own Enter keypress is what carries the command into the screen.
// That is why the menu can only act while a screen is dispatching a key, and
// why it needs no gamepad and no new native.

// What the caller should do with a command offered to the open menu.
#define HELP_MENU_PASS    0   // not the menu's: let it reach the screen
#define HELP_MENU_SPEAK   1   // swallow it, and say `out`
#define HELP_MENU_QUIET   2   // swallow it, say nothing
#define HELP_MENU_FIRE    3   // let it through as `fire` instead

// Opens the menu on `screen` and fills `out` with the whole list, as before:
// one press still answers "what can I do here".  Returns 0 (and does not
// open) when the screen offers nothing.
int  help_menu_open(const char* screen, char* out, size_t out_sz);

int  help_menu_is_open(void);

// The screen the open menu belongs to; "" when none is open.
const char* help_menu_screen(void);

// Closes the menu.  Silent: the caller decides whether that needs saying.
void help_menu_close(void);

// Offers one command to the open menu.  `fire` receives the command to
// substitute when the answer is HELP_MENU_FIRE.
//
// `screen` is checked against the one the menu opened on: a menu left open
// when the screen changed answers PASS and closes itself, because its list
// describes somewhere the player is no longer standing.
int  help_menu_key(const char* screen, int cmd, int* fire,
                   char* out, size_t out_sz);
