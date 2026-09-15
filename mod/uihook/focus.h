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
void focus_set_part(void* obj, int index, int part, const char* text);

// Resolves an index against `obj`'s list.  Returns 0 if unknown.
int  focus_label_at(void* obj, int index, char* out, size_t out_sz);

// How many labels are currently held for `obj`.
int  focus_count(void* obj);
