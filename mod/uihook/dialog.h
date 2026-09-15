#pragma once
#include <windows.h>

// Composing a modal prompt out of the calls that draw it.
//
// A dialogue box is the one piece of UI that has to be read the moment it
// appears.  It takes the keyboard away from whatever was on screen, it offers
// two answers, and there is nothing to navigate: no cursor ever moves inside
// it, so the focus table -- which exists to turn a later index back into a
// label -- has nothing to resolve and no second chance to speak.
//
// XCOM draws one across five calls in a fixed order, and not one of them says
// anything on its own (UIDialogueBox.Realize):
//
//     AS_SetStyleNormal()                 which of the four looks it wears
//     AS_SetTitle(kData.strTitle)         often empty
//     AS_SetText(kData.strText)           the question
//     AS_SetHelp(0, strAccept, icon)      what Enter does
//     AS_SetHelp(1, strCancel, icon)      what Escape does
//
// Put through the general text path they cancel each other out.  Each lone
// line replaces the one still waiting to be spoken, and the two help calls
// carry an index, so they land in the focus table as though they were a menu
// -- and the indexed path opens by dropping whatever was pending.  The result
// was silence: the options screen's "Do you want to exit and discard your
// changes?" announced neither the question, nor its two answers, nor the fact
// that a prompt had appeared at all.
//
// So the calls are accumulated here instead and spoken once, as one sentence,
// when the last of them arrives.

#define DIALOG_MAX_TEXT 512

// What to do with the composed text.
#define DIALOG_IGNORED (-1) // not one of the box's own setters; not handled
#define DIALOG_SILENT 0     // recorded, nothing to say yet
#define DIALOG_SPEAK  1     // a box just appeared -- say all of it
#define DIALOG_UPDATE 2     // a box already announced changed -- let it settle

// True when `obj_name` names a dialogue box, e.g. "UIDialogueBox_0".  The
// progress and input dialogues are different classes with different setters
// and are not this.
int dialog_is_box(const char* obj_name);

// Feeds one call.  `fn` is the calling function's name, `slot` the index the
// call carried or -1 when it carried none, and `text` its first string that
// is not an asset reference (or "" when it carried no text -- an empty help
// label still matters, because it says that answer does not exist).
//
// Returns DIALOG_IGNORED for anything that is not one of the box's setters,
// so that its other traffic (Show, OnInit) keeps taking the general path.
// Otherwise DIALOG_SILENT, or one of the speak codes with `out` filled.
int dialog_note(void* obj, const char* fn, int slot, const char* text,
                char* out, size_t out_sz);

// Forgets the box being accumulated.  Used by the offline checks, so that one
// case cannot leak into the next.
void dialog_reset(void);
