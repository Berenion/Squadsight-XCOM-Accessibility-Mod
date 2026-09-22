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

#define SOLDIER_TEXT 384

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
