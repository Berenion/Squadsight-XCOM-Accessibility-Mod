#pragma once
#include <stddef.h>

// The soldier customisation screen (Soldier -> Customize), in words. The
// game draws every appearance option as a bare number -- "RACE 3",
// "HAIR COLOR 5" -- through UIWidgetHelper.SetSpinnerValue(Index, "5"); the
// language spinner alone carries a name. This turns the number into what it
// stands for, read from the soldier's own pawn:
//
//     race          "Asian"
//     head          "3 of 8"
//     skin colour   "light, 2 of 5"           (its place by lightness)
//     hair          "bald, 1 of 12" / "4 of 12"
//     hair colour   "dark brown, 3 of 12"     (its palette entry, named)
//     facial hair   "none, 1 of 24" / "6 of 24"
//     armour deco   "standard, 1 of 2" / "decorated, 2 of 2"
//     armour tint   "olive with black, 3 of 20"
//
// Voice and language are left as they are: language is already a name, and a
// new voice plays a sample of itself.

// `helper` is the UIWidgetHelper the call came from, `widget` the spinner's
// index and `value` what the game sent. Writes the description to `out` and
// returns 1 when the helper is the customisation screen's and the spinner is
// one described here; 0 leaves the value as it was. Reads the game's objects:
// call it from under a fault guard.
int customize_describe(void* helper, int widget, const char* value,
                       char* out, size_t out_sz);
