#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ue3.h"

// Ctrl + / (keys.c): the keys that work where the player is, as a list on
// Insert's machinery (history.h, a page) -- Squadsight's first, then the
// game's, read from its live bindings in the game's own words.

// From review_pump, on any screen: opens the list, or closes any list open.
void keys_poll(void);

// Input.uc's KeyBind, a native struct: Name, Command, then the bools as one
// bitfield in declaration order (Control, Shift, Alt, ...). 24 bytes.
typedef struct {
    FName    Name;
    FString  Command;
    uint32_t bits;
} KeyBind;
#define KB_CONTROL 1u
#define KB_SHIFT   2u
#define KB_ALT     4u

// A binding's key as the game's Controls screen words it ("CTRL + F5",
// "NUM 4"), in the game's language. 0, with `out` empty, for no key (None).
int keys_bind_words(const KeyBind* kb, char* out, size_t out_sz);
