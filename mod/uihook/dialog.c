// The dialogue box, composed into one announcement.  See dialog.h for why it
// cannot go through the general text path.

#include "dialog.h"
#include "strings.h"
#include <string.h>
#include <stdio.h>

// Only one box is ever being drawn: UIDialogueBox is a single screen that
// keeps a stack of TDialogueBoxData and redraws itself from the top of it, so
// there is one object and one set of fields to hold.
static struct {
    void* obj;
    char  severity[64];               // "Warning" / "Alert" (translated), or empty
    char  title[DIALOG_MAX_TEXT];
    char  body[DIALOG_MAX_TEXT];
    char  accept[DIALOG_MAX_TEXT];
    char  cancel[DIALOG_MAX_TEXT];
    int   announced;
    char  said[DIALOG_MAX_TEXT];      // what was last announced, verbatim
} g;

void dialog_reset(void)
{
    memset(&g, 0, sizeof g);
}

int dialog_is_box(const char* obj_name)
{
    // The instance carries the game's own suffix -- "UIDialogueBox_0" -- so
    // this is a prefix test, as everywhere else the object name is matched.
    return obj_name && strncmp(obj_name, "UIDialogueBox", 13) == 0;
}

// Appends one piece of an announcement.  Bounds are tracked by hand: strcat_s
// does not truncate, it calls the CRT invalid-parameter handler, which
// __fastfail()s past SEH and takes the game with it.
//
// DIALOG_MAX_TEXT is the speech queue's own limit, so a box longer than it is
// cut here, at a space, rather than by the speech thread mid-word: the
// beginning of a question is worth hearing, half a word is not.  It was 512
// and a research unlock could reach it; at 4096 the case is defensive.
static void join(char* out, size_t out_sz, const char* piece)
{
    if (!piece || !*piece) return;

    size_t used = strlen(out);
    const char* sep = "";
    if (used) {
        // The game's strings carry their own punctuation -- "Do you want to
        // exit and discard your changes?" ends in a question mark, and so
        // does the title "KEEP SETTINGS?" -- so a full stop is added only
        // where one is missing, rather than read out as "changes? ."
        char prev = out[used - 1];
        sep = (prev == '.' || prev == '?' || prev == '!' ||
               prev == ':' || prev == ',' || prev == ';') ? " " : ". ";
    }

    size_t sep_len = strlen(sep);
    if (used + sep_len + 1 >= out_sz) return;   // no room for even a word

    size_t room = out_sz - 1 - used - sep_len;
    size_t want = strlen(piece);
    if (want > room) {
        while (room && piece[room] != ' ') room--;
        while (room && piece[room - 1] == ' ') room--;
        if (!room) return;
        want = room;
    }

    memcpy(out + used, sep, sep_len);
    memcpy(out + used + sep_len, piece, want);
    out[used + sep_len + want] = 0;
}

// "Enter: EXIT WITHOUT CHANGES".  The label alone says what the answer does
// and nothing about how to give it, which is exactly the half a player who
// cannot see the two buttons is missing.
//
// The keys are the ones UIDialogueBox.OnUnrealCommand actually listens for:
//
//     case FXS_KEY_ENTER(511), FXS_BUTTON_A(300), FXS_KEY_SPACEBAR(513): accept
//     case FXS_KEY_ESCAPE(510), FXS_BUTTON_B(301), mouse back(405):      cancel
//
// so naming Enter and Escape is a statement about this build, not a guess.
static void join_answer(char* out, size_t out_sz, const char* key,
                        const char* label)
{
    if (!label || !*label) return;      // an answer the box does not offer
    char line[DIALOG_MAX_TEXT];
    _snprintf_s(line, sizeof line, _TRUNCATE, "%s: %s", key, label);
    join(out, out_sz, line);
}

// The answers are laid down first and the question is fitted around them.
// They are short, and they are the half a listener cannot guess -- a body
// long enough to fill the buffer would otherwise crowd them out and leave a
// prompt that asks a question and never says how to answer it.
static void compose(char* out, size_t out_sz)
{
    char answers[DIALOG_MAX_TEXT];
    answers[0] = 0;
    join_answer(answers, sizeof answers, T(KEY_ENTER), g.accept);
    join_answer(answers, sizeof answers, T(KEY_ESCAPE), g.cancel);

    size_t reserve = strlen(answers);
    if (reserve) reserve += 2;          // the ". " that will introduce them
    size_t room = (reserve + 1 < out_sz) ? out_sz - reserve : out_sz;

    out[0] = 0;
    join(out, room, g.severity);
    join(out, room, g.title);
    join(out, room, g.body);
    join(out, out_sz, answers);
}

// The size is passed rather than assumed: the severity field is a word long,
// not a sentence, and telling a bounds-checked call otherwise defeats it.
static void set(char* dst, size_t sz, const char* text)
{
    strncpy_s(dst, sz, text ? text : "", _TRUNCATE);
}

int dialog_note(void* obj, const char* fn, int slot, const char* text,
                char* out, size_t out_sz)
{
    if (!out || out_sz == 0) return DIALOG_IGNORED;
    out[0] = 0;
    if (!obj || !fn) return DIALOG_IGNORED;
    if (!text) text = "";

    if (obj != g.obj) {
        dialog_reset();
        g.obj = obj;
    }

    // Matched on a substring, because the same setter reaches the hook
    // AS_-prefixed or bare depending on which native carried it.
    if (strstr(fn, "SetStyle")) {
        // Realize() always sets a style, and sets it first, so this is where
        // a box begins.  Clearing `said` here is what lets the same prompt be
        // announced again when the player raises it a second time, while a
        // bare refresh of the help line (RefreshNavigationHelp is also a
        // watch-variable callback on m_bMouseIsActive, and fires whenever the
        // mouse wakes up) stays silent.
        void* keep = g.obj;
        dialog_reset();
        g.obj = keep;
        if (strstr(fn, "Warning")) set(g.severity, sizeof g.severity, T(DIALOG_WARNING));
        else if (strstr(fn, "Alert")) set(g.severity, sizeof g.severity, T(DIALOG_ALERT));
        return DIALOG_SILENT;
    }

    if (strstr(fn, "SetTitle")) {
        set(g.title, sizeof g.title, text);
        return DIALOG_SILENT;
    }

    if (strstr(fn, "SetText")) {
        set(g.body, sizeof g.body, text);
        // UIOptionsPCScreen.KeepResolutionCountdown drives
        // UIDialogueBox.UpdateDialogText once a second, which re-sends the
        // body and nothing else.  A box that has already been announced
        // therefore reports just the line that moved -- repeating the title
        // and both answers every second would bury the countdown it exists to
        // convey.  It settles first, so that a body replaced twice in quick
        // succession is only spoken once.
        if (g.announced && *text) {
            strncpy_s(out, out_sz, text, _TRUNCATE);
            // What the box now reads as, so that a help refresh arriving
            // afterwards still recognises it as unchanged.
            compose(g.said, sizeof g.said);
            return DIALOG_UPDATE;
        }
        return DIALOG_SILENT;
    }

    if (strstr(fn, "SetHelp")) {
        if (slot == 0) { set(g.accept, sizeof g.accept, text); return DIALOG_SILENT; }
        if (slot != 1) return DIALOG_IGNORED;
        set(g.cancel, sizeof g.cancel, text);

        // Slot 1 is the end of the box: RefreshNavigationHelp sends both
        // slots unconditionally and Realize() calls it last, in both builds.
        // An empty label is still the end -- SaveProfileFailedDialog offers
        // no cancel, and waiting for text that never comes would lose the
        // whole announcement.
        char full[DIALOG_MAX_TEXT];
        compose(full, sizeof full);
        if (!full[0]) return DIALOG_SILENT;

        // A help refresh with nothing new to say must not talk over the
        // screen underneath.
        if (g.announced && strcmp(full, g.said) == 0) return DIALOG_SILENT;

        strncpy_s(out, out_sz, full, _TRUNCATE);
        set(g.said, sizeof g.said, full);
        g.announced = 1;
        return DIALOG_SPEAK;
    }

    return DIALOG_IGNORED;
}
