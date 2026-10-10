#pragma once
#include <stddef.h>

// What happens in a fight, as the game floats it over the battlefield.
//
// Every floating combat text -- damage numbers, "CRITICAL!", "Panicked!",
// "Poisoned!", "Overwatch", "Stealth!" -- goes through one function,
// UIWorldMessageMgr.Message(text, location, ...), and ends in
// CreateNewMessage or UpdateExistingMessageContents, both of which reach
// Flash through the Invoke the mod already reads. Message itself decides
// whether the local player may see it (ShouldDisplayAndBroadcastMessage), so
// what arrives here is what the screen shows, and nothing more.
//
// A damage number is a bare figure followed by an icon, which the screen
// explains by its colour and its picture: "6 <img Damage_red>", or
// "6 <img Icon_CRIT_HTML> CRITICAL!". Markup is stripped before it gets
// here, so it arrives as "6" or "6 CRITICAL!", and it is told apart from
// other messages by coming through UIWorldMessageMgr.DamageDisplay.

#define COMBAT_MAX_TEXT 256

// "Chryssalid, 6 damage." / "Chryssalid, 6 damage, critical." /
// "Kwan, Panicked!" -- `who` is the unit the message floats over, "" when it
// is not known, and is then left out. `damage` when the message came through
// DamageDisplay. `crit_word` is the game's own mark of a critical hit in the
// player's language (XGUnit.m_sCriticalHitDamageDisplay), NULL when unknown.
// Returns 0 when there is nothing to say.
int combat_describe(const char* who, const char* text, int damage, const char* crit_word,
                    char* out, size_t out_sz);

// Whose turn it is, from the turn banner (UITurnOverlay).
//
// XComPresentationLayer.UIEndTurn is the one place a turn changes hands on
// screen. An alien turn runs ShowAlienTurn, which pulses "ALIEN ACTIVITY"
// and darkens the screen's edges. The player's turn runs HideAlienTurn, which
// takes both away again; only multiplayer pulses "YOUR TURN". Another player's
// turn runs PulseOtherTurn. Each of those reaches Flash by the name of the
// function, and the banner's three texts arrive once, at load, in
// SetDisplayText(alien, xcom, other), which is where the words come from, so
// a translated game says them in its own language.
//
// Feed every UITurnOverlay call here. `strings` are the call's strings in
// order. Returns 1 when `out` holds something to say: "Alien activity.",
// "Your turn.", "Exalt turn." -- said once per change of hands.
int combat_turn(const char* fn, const char* const* strings, int nstrings,
                char* out, size_t out_sz);

// Forgets whose turn it was and the banner's texts. For the offline checks,
// and a new mission.
void combat_turn_reset(void);

// The hit points a damaged unit's flag shows afterwards: "2 of 8 HP left." or
// "No HP left." Returns 0 when the flag shows none (-1: "show enemy health"
// is off, which is the screen saying nothing).
int combat_hp(int hp, int hp_max, char* out, size_t out_sz);

// What a hovered unit's name is followed by: its hit points as the flag shows
// them, and for a seen enemy whether it is on overwatch -- ", 3 of 4 HP, on
// overwatch". Empty when there is neither (the flag shows no HP with the
// game's "show enemy health" off, and then nothing is said of it).
void combat_unit_state(int hp, int hp_max, int overwatch, char* out, size_t out_sz);
