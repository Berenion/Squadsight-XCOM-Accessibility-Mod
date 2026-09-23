#pragma once
#include <stddef.h>
#include "abar.h"

// The headquarters' facility menu: Research, Engineering, Barracks, Hangar,
// Situation Room -- the row along the bottom of the base view, walked with
// left and right.
//
// UIStrategyHUD_FacilityMenu.UpdateData publishes it once, one call per
// facility:
//
//     AS_AddMenuOption(int Id, string displayTxt, bool ShowAlert)
//
// where displayTxt is the name through GetHTMLColoredText(name, iState): a
// facility that cannot be entered (iState 1, IsDisabled -- every one but the
// Barracks early in the tutorial's campaign) is drawn grey, and that colour
// is the only thing that says so. ShowAlert is the "needs attention" mark.
//
// Every later update is a command stream instead, Invoke(
// "BatchUpdateListItems", ...), carrying for each facility that changed:
//
//     null, <Id as a number>, then pairs of <function name, value>:
//       "SetAlert"       bool
//       "SetButtonText"  the recoloured name
//
// the same shape the ability bar uses (abar.h), so its values are reused.
// Read flat, as the general path did, it joined "SetAlert, SetButtonText,
// BARRACKS" into the Barracks' label.

#define HQ_FACILITIES 16

void hq_facility_reset(void);

// One AS_AddMenuOption. `name` has had its markup stripped; `unavailable`
// comes from the colour it was drawn in.
void hq_facility_set(int id, const char* name, int unavailable, int alert);

// One BatchUpdateListItems stream. Returns a bitmask of the facilities it
// changed (bit n for Id n), or -1 when the stream did not parse, in which
// case nothing is kept.
int  hq_facility_feed(const AbarValue* v, int n);

// What the menu says for a facility: "BARRACKS", "HANGAR, unavailable",
// "RESEARCH, needs attention". 0 for an Id never published.
int  hq_facility_label(int id, char* out, size_t out_sz);

// The icon a mouse-mode PC help bar draws in place of a glyph. The strategy
// screens call SetButtonType("XComButtonIconPC") and then AddLeftHelp with
// the *frame number* as the label and no icon:
//
//     UINavigationHelp.AddBackButton         4   back
//     UIMissionControl                       2   hologlobe
//     UIStrategyHUD.Touch_ShowPauseButtonHelp 5  pause
//     UIBuildItem                            3   accept
//     UISoldier* screens                     0 / 1  previous / next soldier
//
// so the list 0 read said "4: no key. 2: no key". Returns what the frame
// means, or NULL when `label` is not one of these frames.
const char* hq_pc_icon_label(const char* label);

// One row of a soldier list (UISoldierListBase.UpdateDisplay), in words:
//
//     AS_AddSoldierWithNickname(_name, _nickname, _className, _status,
//         bool _disabled, bool _promotable, bool _psiPromotable,
//         _rankLabel, _classLabel, _flagLabel [, Medals on EW])
//
// The rank is an icon frame, "rank0" .. "rank7" ("shiv1".. for a SHIV), so
// it is turned into its word here; the class and flag labels are icon frames
// too and are left out, since _className already says the class. `nick` and
// `cls` may be empty (a rookie has neither). The result:
//     "Squaddie Kelly Hudson 'Disco', Assault, Available, promotion"
void hq_soldier_row(const char* name, const char* nick, const char* cls,
                    const char* status, const char* rank_label, int disabled,
                    int promotable, char* out, size_t out_sz);

// The count label, "3/4", as a heading: "3 of 4 soldiers available". Returns
// 0 when `label` is not of that shape.
int  hq_soldier_count(const char* label, char* out, size_t out_sz);

// An item card's text, readied to be spoken: the flavour text is a run of
// bullets, "· Light armor · XCOM soldiers will appreciate it...", and a
// screen reader names the dot. Each bullet becomes a sentence break, a
// leading one goes, and a doubled full stop is closed up. In place.
void hq_card_clean(char* s);
