#pragma once
#include <stddef.h>

// Every council country's panic, read from the game's own XGCountry and
// XGContinent actors rather than from the Situation Room's drawing (hq_sit_*),
// which is only as fresh as the last visit there -- and never drawn at all on
// the way to an abduction, where the choice is which continent to leave to
// panic (countries.c has the reasons).

// One line per continent, in the game's continent order:
//     "North America: United States, panic 2 of 5, satellite. Canada, panic
//      1 of 5. Mexico, panic 5 of 5, left XCOM"
// written to `lines`, `max` lines of `width` bytes each. Returns how many, 0
// when the countries cannot be read (no strategy game loaded, or the fields
// not found -- logged once).
//
// Walks the object table, tens of milliseconds, so it is for a key press, not
// a draw. Call it from under a fault guard: the actors are the game's.
int countries_lines(char* lines, size_t width, int max);
