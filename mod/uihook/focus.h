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

// Resolves an index against `obj`'s list.  Returns 0 if unknown.
int  focus_label_at(void* obj, int index, char* out, size_t out_sz);

// How many labels are currently held for `obj`.
int  focus_count(void* obj);
