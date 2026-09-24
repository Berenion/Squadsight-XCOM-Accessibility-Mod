#pragma once
#include <windows.h>

// Remembers what each UI object most recently put on screen, so that a later
// selection index can be turned back into the label it refers to.
//
// This is needed because XCOM never re-sends text when the selection moves.
// A screen publishes its labels once:
//
//     UIFinalShell.SetText()      -> ["SINGLE PLAYER", "MULTIPLAYER", ...]
//
// and every subsequent cursor move carries nothing but a number:
//
//     UIShell.RealizeSelected()   -> m_iCurrentSelection
//
// Narrating text as it passes therefore reads out a whole menu at build time
// and stays silent while the player actually navigates it -- precisely
// backwards.  Keeping the list lets the index be resolved instead.

#define FOCUS_MAX_LABELS 256
#define FOCUS_MAX_LABEL  256

// Starts a fresh list for `obj` (a screen republishing its contents).
void focus_begin(void* obj);

// Appends one label to `obj`'s current list.
void focus_add(void* obj, const char* text);

// Places a label at a specific slot, growing the list to fit.  Several of the
// game's setters carry their own index --
//     AS_SetCheckboxLabel(int Index, string strText)
//     AS_AddListItem(int Id, string Desc, ...)
// -- which is better than inferring order from arrival, because labels are
// interleaved with other traffic and can be refreshed one at a time.
void focus_set(void* obj, int index, const char* text);

// A settings widget names itself and states its value through two separate
// calls that carry the *same* index, because both describe one control:
//
//     SetSpinnerLabel(int Index, string strText)    -> "Mode:"
//     SetSpinnerValue(int Index, string StrValue)   -> "Fullscreen"
//
// Filing both as plain labels let the second overwrite the first, leaving
// "Fullscreen" with nothing to say which setting it belonged to.  They are
// kept apart and joined only when the slot is read.
#define FOCUS_PART_LABEL 0
#define FOCUS_PART_VALUE 1

// Returns 1 when this overwrote a *different* value that was already there,
// which is how a genuine change is told from a screen populating itself. A
// checkbox being flipped has to be spoken; the same checkbox being redrawn
// with the state it already had must not be.
int focus_set_part(void* obj, int index, int part, const char* text);

// A container widget holds its own items, so an index can mean "which widget"
// or "which item inside one widget", and both appear:
//
//     SetListOptions(int Index, array<string> arrLabels)   the items
//     SetListSelection(int Index, int iSelection)          which item
//
// The items are kept per (object, slot) rather than flattened into the slot,
// which is what made EU's difficulty screen read as one run-on string.
#define FOCUS_MAX_OPTIONS 64

void focus_options_begin(void* obj, int slot);
void focus_options_add(void* obj, int slot, const char* text);
int  focus_option_at(void* obj, int slot, int index, char* out, size_t out_sz);

// The screen that publishes a list and the screen that announces the choice
// are not always the same object -- EU's difficulty list belongs to the widget
// helper while AS_SetCurrentDifficultyMarker(int Index) fires on the shell.
// Only one option list is ever being navigated at a time, so the most recent
// one resolves those.
int  focus_recent_option_at(int index, char* out, size_t out_sz);

// Resolves an index against `obj`'s list.  Returns 0 if unknown.
int  focus_label_at(void* obj, int index, char* out, size_t out_sz);

// How many labels are currently held for `obj`.
int  focus_count(void* obj);

// A screen whose list has a panel beside it, describing the item under the
// cursor, sends that panel as a call of its own with no index:
//
//     UIContinentSelect.AS_UpdateInfo(string continentName, string bonusName,
//                                     string infoText)
//
// (the first argument is misnamed: it carries "CONTINENT BONUS:", and the
// continent's own name is only ever in the list).  Several strings in one
// call look exactly like a screen publishing a list, so it used to replace
// the list -- the five continents became "CONTINENT BONUS:", the bonus and
// its description, and moving the cursor read those back by position.  The
// panel is kept beside the list instead, with when it arrived, and said
// after the label of the item it describes.
#define FOCUS_MAX_DETAIL 2048

void focus_set_detail(void* obj, const char* text);
int  focus_detail(void* obj, char* out, size_t out_sz, ULONGLONG* at);

// A screen's heading ("IDENTIFY BASE LOCATION:"), kept to be said before the
// first item the cursor lands on.  Taken once: every later move is the
// player walking the list, and hearing the heading again would be noise.
// Returns 0, and arms nothing, when the object already had this heading: a
// screen redrawing itself re-sends it.
int  focus_set_title(void* obj, const char* text);
int  focus_take_title(void* obj, char* out, size_t out_sz, ULONGLONG* at);

// Joins a panel's strings into one line, dropping empties and repeats (the
// frame walk sees a parameter and its copy).  Returns how many were kept.
int  focus_join_detail(const char* const* parts, int n, char* out, size_t out_sz);

// "IDENTIFY BASE LOCATION: EUROPE. CONTINENT BONUS: ...": title, label and
// panel, any of which may be empty.  A panel that opens with the label (the
// item's name as its heading) has it dropped, so the name is said once.
void focus_compose(const char* title, const char* label, const char* detail,
                   char* out, size_t out_sz);
