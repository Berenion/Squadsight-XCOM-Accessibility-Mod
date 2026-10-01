#pragma once
#include <stddef.h>

// The turn counters top right of a mission's HUD: "Turns until Air Strike: 8",
// a capture point's "Encoder, Hack in progress, 3", a Meld canister's.
// Every one is a UISpecialMissionHUD_TurnCounter, and main.c hands each one
// it sees drawing to counters_note; what they say is read off the panels.

#define COUNTERS_MAX 8

typedef struct {
    char label[64];     // "Turns until Air Strike"
    char detail[96];    // "8", "Hack in progress, 3", "Expired"
    int  meld;          // a Meld canister's, which the scanner has already
} Counter;

// A counter panel the capture saw drawing.
void counters_note(void* panel);

// The counters on screen now, in the order they were first seen; how many.
int counters_read(Counter* out, int max);

// The counters on screen as sentences, "Turns until Air Strike, 8." --
// empty when there are none.
void counters_text(char* out, size_t out_sz);
