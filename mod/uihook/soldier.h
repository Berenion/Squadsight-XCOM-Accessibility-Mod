#pragma once
#include <stddef.h>

// The selected soldier, as the tactical HUD shows them.
//
// Two things draw a soldier. The stats panel in the corner,
// UITacticalHUD_SoldierStatsContainer.SetStats(kActiveUnit), sends the full
// name, the nickname, a rank icon ("rank0".."rank7", "shiv1".."shiv3"), a
// class icon ("sniper", "heavy", "support", "assault", "mech", with "_psi" or
// "_gene" added), whether they lead the squad, whether a promotion waits, and
// their aim. SetStats is sent whenever the selection changes, by any means --
// a key, a click, a turn starting, a death -- which is what makes it the place
// to notice a switch. The soldier's flag draws the rest: hit points, the pips
// for actions left (UIUnitFlag.RealizeMoves, friendly units only), and a
// bonus and a penalty marker (ShowBuff / ShowDebuff, on or off; the flag says
// that there are some, not which).

#define SOLDIER_TEXT 640

// Critically wounded, as the flag draws it (UIUnitFlag.RealizeCriticallyWounded
// -> SetCriticallyWounded(bleeding, turns)): bleeding out with turns left, or
// stabilised.
#define SOLDIER_WOUND_NONE       0
#define SOLDIER_BLEEDING         1
#define SOLDIER_STABILISED       2
#define SOLDIER_WEAPON_TEXT 256

// One of the HUD's two weapon panels, as UITacticalHUD_WeaponPanel.
// SetWeaponAndAmmo sends it: the weapon's type ("_LMG", its EItemType less
// "eItem"), then either its ammo (a percentage: 100 is full, each shot takes
// its cost off, and 1 left is rounded to 0) or, for a weapon that overheats,
// its overheat chance; the ammo the selected ability would spend from it, 0
// when the ability uses another weapon; whether it overheats; and whether it
// is empty and can be reloaded.
typedef struct {
    int  set;           // a weapon is in this panel
    char type[48];      // "_LMG"
    int  value;         // ammo percent, or overheat chance
    int  cost;          // ammo percent one shot costs, 0 unknown
    int  overheat;      // `value` is an overheat chance
    int  reload;        // empty, and a reload would refill it
} SoldierWeapon;

typedef struct {
    char name[64];      // "URSULA WRIGHT", as the panel sends it
    char nick[64];      // "'Disco'", or ""
    char rank[16];      // "rank3", "shiv2", or ""
    char cls[32];       // "heavy", "mech_gene", or ""
    int  leader;        // squad leader: 1, 0, -1 unknown
    int  promotion;     // a promotion waits: 1, 0, -1 unknown
    int  aim;           // -1 unknown
    int  hp, hp_max;    // from the flag; -1 unknown
    int  actions;       // from the flag; -1 unknown
    int  buff, debuff;  // from the flag: 1, 0, -1 unknown
    int  panicked;      // the flag's EKG: 1, 0, -1 unknown
    int  wounded;       // SOLDIER_WOUND_*
    int  bleed_turns;   // turns before bleeding out, while SOLDIER_BLEEDING
    char weapon[SOLDIER_WEAPON_TEXT];   // the equipped weapon: "LMG, 1 shot left."
    char weapons[SOLDIER_WEAPON_TEXT];  // it, then the other: "... Rocket Launcher, full."
} SoldierState;

// Clears to "all unknown".
void soldier_clear(SoldierState* s);

// Reads SetStats' strings into `s`: the name first, then the nickname if it
// is quoted, the rank and class icons wherever they fall. An empty nickname
// does not arrive at all, so the order cannot be trusted past the name.
void soldier_from_stats(SoldierState* s, const char* const* strings, int n);

// "Ursula Wright, 'Disco'. 11 of 11 HP. 2 actions." -- said on a switch.
void soldier_brief(const SoldierState* s, char* out, size_t out_sz);

// Everything the HUD shows: "Ursula Wright, 'Disco'. Sergeant, heavy. Squad
// leader. 11 of 11 HP. 2 actions. Aim 69. Promotion available. Has bonuses."
void soldier_full(const SoldierState* s, char* out, size_t out_sz);

// "URSULA O'REILLY" -> "Ursula O'Reilly": capitals would be spelt out.
void soldier_title_case(const char* in, char* out, size_t out_sz);

// "_RocketLauncher" -> "Rocket Launcher", "_LMG" -> "LMG".
void soldier_weapon_words(const char* type, char* out, size_t out_sz);

// Whether the equipped weapon's name, as the HUD shows it ("Rocket
// Launcher"), is the weapon of this type ("_RocketLauncher"): the same
// letters, ignoring case, spaces and punctuation.
int soldier_weapon_is(const char* name, const char* type);

// Whether the panel's type is a MEC's primary weapon: the three items with
// eWP_Mec in DefaultGameCore.ini (eItem_Chaingun, _Railgun, _ParticleBeam).
int soldier_weapon_is_mec(const char* type);

// Looser: every word of the name appears in the type, in any order.
int soldier_weapon_like(const char* name, const char* type);

// "LMG, 1 shot left." / "full." / "34% ammo." / "empty, reload needed." /
// "12% overheat chance." -- `name` is what to call it.
void soldier_weapon_text(const char* name, const SoldierWeapon* w,
                         char* out, size_t out_sz);

// Both panels in words: the equipped weapon into `active` (named as the HUD
// names it, `active_name`), and every weapon, equipped first, into `all`.
void soldier_weapons(const char* active_name, const SoldierWeapon* w, int n,
                     char* active, size_t active_sz, char* all, size_t all_sz);

// The same, told which panel is the equipped one (`active_index`, -1 when
// not known). The name is the game's, in the player's language, and the
// panels' types are the item's English enum name ("_LMG"), so matching the
// two works only in English; the index, where main.c could read it from the
// game, holds in any.
void soldier_weapons_at(const char* active_name, int active_index, const SoldierWeapon* w,
                        int n, char* active, size_t active_sz, char* all, size_t all_sz);

// One squad member's state as their flag shows it, for the squad list:
// "2 actions, 11 of 11 HP", "no actions left, 5 of 10 HP, panicked",
// "bleeding out, 2 turns left", "stabilised". Unknown parts are left out.
void soldier_squad_words(int actions, int hp, int hp_max, int panicked, int wounded,
                         int bleed_turns, char* out, size_t out_sz);

// "rank3" -> "Sergeant", "shiv2" -> "SHIV", anything else "".
const char* soldier_rank_word(const char* rank);

// "heavy" -> "heavy", "mech_psi_gene" -> "MEC trooper, psionic, gene modded".
void soldier_class_words(const char* cls, char* out, size_t out_sz);

// Where the game's own words come from: main.c hands soldier.c a function
// that reads them in the player's language (game_loc). Returns 0 when it has
// none, and soldier.c says the English.
enum { SOLDIER_LOC_RANK, SOLDIER_LOC_CLASS, SOLDIER_LOC_SHIV };
typedef int (*SoldierLocFn)(int kind, int index, char* out, size_t out_sz);
void soldier_set_loc(SoldierLocFn fn);
