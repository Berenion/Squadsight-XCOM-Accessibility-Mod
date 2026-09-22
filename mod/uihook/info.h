#pragma once
#include <stddef.h>

// The game's own unit information screen, TARGET INFORMATION, in words.
//
// F1 (or L3) opens it: UITacticalHUD.OnUnrealCommand(600 or 311) ->
// OpenGermanMode. While aiming it describes the target and the shot; otherwise
// the selected soldier. It is drawn once, in one burst, and nothing on it is
// ever selected, so the burst is kept here, said once as a summary, and can
// then be walked line by line.
//
//   UIUnitGermanMode.SetSoldierInformation(name, nick, class, rank, promote)
//                   .SetAlienInformation(name, friendly, exalt)
//                   .SetUnitStats("Health: 11", "Will: 50", "Offense: 69", "Defense: 0")
//   UIUnitGermanMode_PerkList (x3).SetTitle("ABILITIES" / "BONUSES" / "PENALTIES")
//                                  .AddPerk(name, description, icon)
//   UIUnitGermanMode_ShotInfo.SetShotInfo("", "", "", "", "")      -- cleared first
//                            .AddRegularItem("Aim", "+65%")         -- hit modifiers
//                            .AddCritItem("Flanked", "+50%")        -- crit modifiers
//                            .SetShotInfo("FIRE", "72%", "Chance to Hit:", "10%", "Chance to Crit:")
//
// The shot half is only sent while a targeting action is up, and an argument
// the game left empty is passed as "" -- the caller passes arguments by
// position, never by shape, because a 10% hit and a 10% crit look the same.

#define INFO_TEXT  1024
#define INFO_LISTS 3

// Which of the screen's calls this is. The frame the hook sees is the script
// wrapper, so the names arrive as "AS_SetShotInfo"; the bare Flash name is
// accepted too. The first run matched only the bare names and took nothing.
typedef enum {
    INFO_NONE = 0,      // not content: scrolling, allegiance, the close button
    INFO_SOLDIER,       // UIUnitGermanMode_N.SetSoldierInformation
    INFO_ALIEN,         //                   .SetAlienInformation
    INFO_STATS,         //                   .SetUnitStats
    INFO_TITLE,         // UIUnitGermanMode_PerkList_N.SetTitle
    INFO_PERK,          //                            .AddPerk
    INFO_SHOT,          // UIUnitGermanMode_ShotInfo_N.SetShotInfo
    INFO_HIT_MOD,       //                            .AddRegularItem
    INFO_CRIT_MOD       //                            .AddCritItem
} InfoCall;

InfoCall info_call(const char* obj_name, const char* fn_name);

// Forgets the last screen.
void info_reset(void);

void info_soldier(const char* name, const char* nick, const char* cls,
                  const char* rank, int promotion);
void info_alien(const char* name);
void info_stats(const char* const* stats, int n);

// The soldier's weapons and their ammo, as the HUD's weapon panels show them:
// "LMG, 1 shot left. Rocket Launcher, full." The screen itself does not draw
// them, but the HUD under it does, all the time, so it is said with the stats.
void info_weapons(const char* text);

// A perk list: `list` is which of the three panels, as the caller tells them
// apart (by object). Lists with nothing in them are left out of everything.
void info_list_title(int list, const char* title);
void info_list_add(int list, const char* name, const char* desc);

// SetShotInfo. All empty is the clear that opens the shot's burst, and drops
// the modifiers of any earlier one.
void info_shot(const char* name, const char* hit, const char* hit_label,
               const char* crit, const char* crit_label);
void info_modifier(int crit, const char* label, const char* value);

// Whether anything has arrived since the reset.
int info_has_content(void);

// Said when the screen has finished drawing: "Target information. Muton.
// Fire. Chance to Hit: 72%. Aim +65%, Full cover -40%. Chance to Crit: 10%.
// Abilities, 2."
void info_summary(char* out, size_t out_sz);

// ---- the screen as a list ---------------------------------------------------

// The lines, built from what has arrived so far, so a late call is not lost.
int  info_line_count(void);
void info_line(int i, char* out, size_t out_sz);

// Opens before the first line: Down reads the first.
void info_open(void);
void info_close(void);
int  info_is_open(void);

// Moves by `dir` and says the line, with "Top." / "End." in front at either end.
void info_step(int dir, char* out, size_t out_sz);
void info_current(char* out, size_t out_sz);
