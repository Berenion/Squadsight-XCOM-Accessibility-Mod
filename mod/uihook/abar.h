#pragma once
#include <stddef.h>

// The ability bar: what the soldier can do this turn, kept so a key can read
// it back.
//
// UITacticalHUD_AbilityContainer.PopulateFlash sends the whole bar in one
// call, Invoke("BatchUpdateListItems", arrUpdateAbilitiesData), and the array
// is not a list but a command stream. Each UITacticalHUD_AbilityItem appends,
// for its own slot, only what CHANGED since it last sent anything
// (UpdateData):
//
//     null, <index as a number>, then pairs of <function name, value>:
//       "SetCooldown"     "T-2", or null when there is none
//       "SetIconLabel"    "Standard"               with a new ability type
//       "SetAntennaText"  "FIRE"                   the name; mouse mode only
//       "SetAvailable"    bool
//       "SetCharge"       "x1", or null
//       "SetBGColorLabel" "cyan" / "yellow" / "purple"
//       "SetHotkeyLabel"  "1"                      mouse mode only
//       "SetOverwatchButtonHelp" bool              gamepad only
//
// A slot that did not change sends nothing at all, so the bar can only be
// known by keeping it: a copy per slot, updated by each stream. That is also
// why a value that looks missing is not reset -- the item still holds it,
// and so does this. The items start from their class defaults (available
// false, no cooldown, no charge), and so does this.
//
// A value null in a function's place is that function's argument, not the
// start of a slot, which is what keeps the stream unambiguous: a slot starts
// only where a null is followed by a number and no function name is waiting
// for its argument.
//
// How many slots are live is AS_SetNumActiveAbilities(Len), sent before the
// stream on every PopulateFlash. Slots past it keep their data in the items
// and are not shown.

#define ABAR_SLOTS 15           // PopulateFlash refuses more than this

enum { ABAR_NULL = 1, ABAR_NUMBER = 2, ABAR_STRING = 3, ABAR_BOOL = 4 };

typedef struct {
    int         type;           // ABAR_NULL .. ABAR_BOOL, as ASValue.Type
    float       n;
    int         b;
    const char* s;              // NULL unless a string
} AbarValue;

// Forgets the bar: a new container (a new mission, or a reload) starts again.
void abar_reset(void);

// AS_SetNumActiveAbilities.
void abar_set_count(int n);

// One BatchUpdateListItems stream. Returns how many slots it touched, or -1
// when the stream did not parse -- in which case nothing it said is kept.
int  abar_feed(const AbarValue* v, int n);

// ---- the menu --------------------------------------------------------------
//
// Numpad . opens the bar as a menu of its own, walked one ability at a time.
// Only the state and the words are here; main.c reads the keys, keeps them
// from the game while the menu is open, and fetches each ability's help.

int  abar_count(void);              // live slots

void abar_menu_open(void);          // at the first ability
void abar_menu_close(void);
int  abar_menu_is_open(void);
int  abar_menu_index(void);         // 0-based, -1 when closed or empty

// Moves by `dir` (+1 / -1), wrapping. Returns 0 when there is nothing to move
// through.
int  abar_menu_step(int dir);

// One entry: "2 Headshot, cooldown 2 turns. Fire a precise shot ..." -- the
// key and name, the status, then `help` (the ability's own tooltip, which may
// be empty). Returns 0 for a slot with nothing to say.
int  abar_entry(int index, const char* help, char* out, size_t out_sz);

// "1 Fire. 2 Headshot, cooldown 2 turns. 3 Overwatch. 4 Reload, unavailable.
// 5 Fire Rocket, 1 charge." Returns 0 when there is nothing to say.
int  abar_describe(char* out, size_t out_sz);
