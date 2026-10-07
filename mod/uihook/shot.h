#pragma once
#include <windows.h>

// The shot the player is about to take.
//
// UITacticalHUD_InfoPanel is the one place the game states what a shot is and
// what it will do, and it states it in pieces.  Every change of ability or of
// target runs UITacticalHUD_InfoPanel.Update, which sends a fixed burst and
// always ends with UpdateLayout:
//
//     SetIsAvailable(bool)
//     SetShotName("Standard Shot", "")
//     SetShotChance("to hit", "73%")        omitted for shots with no percentage
//     SetCriticalChance("critical", "20%")  omitted unless the shot shows one
//     SetHelp("Take a standard shot ...")
//     SetWeaponStats("Assault Rifle")
//     UpdateLayout()                        the end, every time
//
// Not one of those says anything on its own, and the general text path would
// treat each as a list of two strings and stay silent -- which is what it did:
// aiming was completely quiet.  So the burst is accumulated and spoken once,
// on UpdateLayout, in the order a person would say it: "Standard Shot. 73% to
// hit. 20% critical."
//
// Update runs whenever the targeting state is touched, not only when it
// changes, so an announcement identical to the last one is dropped. Without
// that, holding a direction would repeat the same shot indefinitely.
//
// What this panel does not say is *who* is being aimed at: the game shows that
// with a reticule and puts the name only on the unit's flag. So the target is
// described from its flag -- what the flag draws, and nothing it does not --
// and handed in with shot_set_target before the burst ends. It leads the
// announcement when it is news, which is what makes Tab audible: two targets
// at the same odds used to be the same announcement, and the second was
// dropped as a repeat.

#define SHOT_MAX_TEXT 512

// Who a shot is aimed at, as the screen shows it.
typedef struct {
    const char* name;       // the flag's SetNames; "" or NULL when unnamed
    const char* cover;      // the flag's SetCover shield: "_highCover", "_lowCover",
                            // "_megaCover", "_none"; "" or NULL when unknown
    int flanked;            // the shield's flanked state; 1, 0, or -1 unknown
    int hp, hp_max;         // as the flag displays them; -1 when it shows none
    int index, count;       // place in the Tab cycle, 0-based; count 0 unknown
} ShotTarget;

// "Sectoid, 2 of 3. Low cover, flanked. 3 of 4 HP."  Anything unknown is
// left out rather than guessed.
void shot_describe_target(const ShotTarget* t, char* out, size_t out_sz);

// One enemy on the target strip, for the scanner's Targets list: "45%, low
// cover, flanked, 8 of 8 HP, squadsight". The name is said by the scanner, so
// it is not repeated here. `chance` is -1 when the soldier has no shot at it,
// and is then left out; `flanked` here is the strip's own mark, flanked by
// this soldier. Only what the strip and the enemy's flag draw.
void shot_list_detail(const ShotTarget* t, int chance, int squadsight,
                      char* out, size_t out_sz);

// The target described for the burst in progress, "" for none. Read by the
// call that ends the burst, so it must be set before that call.
void shot_set_target(const char* text);

// For an ability used on the soldier taking it (eTarget_Self, the Jetboot
// Module, Hunker Down): how to use it, "Used on White. Enter to use", said
// after the ability's name. "" for any other. Set like the target, before the
// burst ends.
void shot_set_self(const char* text);

// True when `obj_name` is the tactical info panel.
int shot_is_panel(const char* obj_name);

// Feeds one call of the burst.  `a` and `b` are the call's first two strings
// with duplicates removed (the frame carries each argument twice: once as the
// parameter and once inside the ASValue array built from it), and `flag` is a
// bool argument or -1 when the call had none.
//
// Returns 1 when `out` holds something to say -- only ever on the call that
// ends the burst.
int shot_note(const char* fn, const char* a, const char* b, int flag,
              char* out, size_t out_sz);

// Forgets what was last announced, so the next burst is spoken even if it is
// the same shot. Called when targeting is lowered (a cancel, or the shot
// taken): picking the same ability again is a new decision, not a repeat.
// The weapon is kept, since it has not changed.
void shot_forget_said(void);

// While the numpad moves a free aim, each step is followed by its odds alone:
// "0% to hit", not "Free Aiming: Fire Rocket. 0% to hit. Rocket Launcher".
// The name is still said for a shot with no odds, since then it is all there
// is. The weapon is left out.
void shot_set_brief(int on);

// Forgets the shot being accumulated.  Used by the offline checks, and when
// the tactical HUD goes away.
void shot_reset(void);
