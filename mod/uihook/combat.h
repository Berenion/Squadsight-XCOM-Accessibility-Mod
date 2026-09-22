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
// DamageDisplay. Returns 0 when there is nothing to say.
int combat_describe(const char* who, const char* text, int damage,
                    char* out, size_t out_sz);

// The hit points a damaged unit's flag shows afterwards: "2 of 8 HP left." or
// "No HP left." Returns 0 when the flag shows none (-1: "show enemy health"
// is off, which is the screen saying nothing).
int combat_hp(int hp, int hp_max, char* out, size_t out_sz);
