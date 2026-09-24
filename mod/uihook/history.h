#pragma once
#include <stddef.h>

// What has been announced, kept so it can be heard again.
//
// Speech goes by once. A sighted player glances back at the ticker, a flag's
// hit points or the banner; a player listening has only what they caught. So
// every event the mod announces -- combat text, whose turn it is, the message
// ticker -- is also kept here, newest last, and Insert opens it as a list.
//
// Entries are numbered as they arrive and the numbers are never reused, so a
// place in the list stays put while new announcements come in underneath it.

#define HISTORY_MAX  200
#define HISTORY_TEXT 256

// Adds an announcement.
void history_add(const char* text);

// Adds `text` to the newest entry if that entry begins with `prefix`, and as
// an entry of its own otherwise. What a hit's hit points use: "Chryssalid, 4
// damage." becomes "Chryssalid, 4 damage. 4 of 8 HP left.", unless something
// else was announced in between.
void history_extend(const char* prefix, const char* text);

// How many are kept.
int history_count(void);

// Forgets everything. For the offline checks.
void history_reset(void);

// ---- the list --------------------------------------------------------------

// Opens the list on the newest entry. Returns 0, with `out` saying so, when
// there is nothing to show.
int history_open(char* out, size_t out_sz);
void history_close(void);
int history_is_open(void);

// Moves by `dir` (-1 older, +1 newer) and says where it landed: the entry,
// or at either end the entry again with "Oldest." / "Newest." in front.
void history_step(int dir, char* out, size_t out_sz);

// The entry under the cursor, again.
void history_current(char* out, size_t out_sz);

// ---- a page ----------------------------------------------------------------
//
// The same list, over lines handed to it instead of the announcements: what
// a screen shows with no cursor to walk it (the Situation Room). Opens on the
// first line with `title` and the count in front; Up and Down walk it, with
// "Top." / "End." at either end, and history_close or any key that closes
// the list closes it. history_is_open is true while it is up. The lines are
// copied.
#define HISTORY_PAGE_MAX  48
#define HISTORY_PAGE_TEXT 1024
int history_page_open(const char* title, const char (*lines)[HISTORY_PAGE_TEXT], int n,
                      char* out, size_t out_sz);
int history_page_is_open(void);
