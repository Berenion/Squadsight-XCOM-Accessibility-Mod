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

// ---- the promotion tree ----------------------------------------------------
//
// UISoldierPromotion draws a grid: one rank per "column" (Squaddie to
// Colonel, 7; the psi tree has 3), and one or two abilities in each (rows 0
// and 1; Squaddie and Major have one). UpdateAbilityData sends, per rank:
//
//     AS_SetAbilityIcon(int column, int Row, string iconLabel, bool isHighlighted)
//         -- "unknown" above the soldier's rank; isHighlighted is HasPerk,
//            the ability already chosen
//     AS_SetColumnData(int ColumnIndex, string Label, int State)
//         -- 0 earned, 1 the rank to choose now, 2 a later rank also waiting
//            for a choice, 3 not reached yet
//
// and every move (RealizeSelected) sends
//
//     AS_SetSelectedIcon(int column, int Row)
//     AS_SetAbilityDescription(string abilityName, string abilityDescription)
//
// Up and down change rank (clamped, not wrapped); right picks row 0 and left
// row 1 (XGSoldierUI.OnPromotionRight / Left), so an option is named by the
// key that reaches it. Enter raises a confirm dialogue, read like any other.

#define HQ_PROMO_COLS 8

// AS_InitializeTree: a new tree, and its title said before the first move.
void hq_promo_reset(const char* title);
void hq_promo_icon(int col, int row, const char* icon, int chosen);
void hq_promo_column(int col, const char* label, int state);
void hq_promo_select(int col, int row);

// The line for the move just made, from the description that ends it:
//     "HEAVY ABILITIES. SQUADDIE, earned. Fire Rocket, chosen. Fire a rocket..."
//     "CORPORAL, choose now. Shredder Rocket, right. ..."
// The rank is said only when the move changed rank.
void hq_promo_describe(const char* name, const char* desc, char* out, size_t out_sz);

// An abduction site's details (UIMissionControl_AbductionSelection):
//
//     AS_SetHeaderLabels(sitesLabel, panicLabel, difficultyLabel,
//                        rewardLabel, confirmLabel)
//     AS_SetData(countryName, int panicLevel, difficulty, reward)
//
// sent after every move, once the widget helper has named the city. Panic
// is XGCountry.GetPanicBlocks(), clamped to 1..5. The country is left out:
// the city's own label already names it. "PANIC: 2 of 5. MISSION
// DIFFICULTY: Easy. REWARD: Scientists: 4"
void hq_abduction_line(const char* panic_label, int panic,
                       const char* diff_label, const char* diff,
                       const char* reward_label, const char* reward,
                       char* out, size_t out_sz);

// ---- the mission's end (UIMissionSummary_Factors) ---------------------------
//
// The factor table reaches Flash as one string, rows split by ';' and each
// row's six fields by ',' (UIMissionSummary_Factors.SetData):
//
//     factor, icon, "RESULTS:", result, "RATING:", rating
//
// with the two labels empty when their value is. "Aliens Killed,_deadAliens,
// RESULTS:,4,RATING:,Excellent!" -> "Aliens Killed: 4, Excellent!". Rows are
// joined with ". ". Returns how many rows it read.
int hq_summary_factors(const char* raw, char* out, size_t out_sz);

// ---- the base's status (Delete at the base) ---------------------------------
//
// The strategy HUD states the base's position in three panels, and nothing
// read any of them:
//
//     UIStrategyHUD.ClearResources, then AS_AddResource("CREDITS: \xC2\xA7265")
//         per resource -- credits, monthly income, Meld (EW), and so on
//     UIStrategyComponent_Clock.AS_SetDateTime("1 March", "2015", "1", "22")
//         -- the date, the year, the hour and the minutes
//     UIStrategyComponent_EventList.UpdateData, then per event
//         AS_AddEvent("Council Report", "Days", "31", "_endOfMonth")
//
// Kept here and said together on request. Written from the game's thread and
// read from the key thread, so every call takes one lock.

void hq_status_resources_clear(void);
void hq_status_resource(const char* text);
void hq_status_date(const char* day_month, const char* year, const char* hour,
                    const char* minute);
// Events are kept per list: the HUD draws two (UIStrategyComponent_EventList
// _0, "NEXT EVENT", one item; _1, "UPCOMING EVENTS", all of them), and
// whichever refreshed last would otherwise win. The fuller one is read.
void hq_status_events_clear(const void* list);
void hq_status_event(const void* list, const char* title, const char* unit,
                     const char* count);

// "1 March 2015, 1:22. CREDITS: 265. MONTHLY: +165. MELD: 0. Council Report,
// 31 days." Returns 0 when nothing has been seen yet.
int  hq_status_line(char* out, size_t out_sz);

// ---- the squad for a mission (UISquadSelect_SquadList) ------------------------
//
//     AS_SetUnitInfo(int Index, int Status, unitName, unitNickName, classDesc,
//                    classLabel, item1, item2, promote)
//
// unitName carries an abbreviated rank ("RK. WHITE"), classLabel is the
// class's icon ("none", "heavy") and is left out, and the items are image
// paths ("img:///...InventoryIcons.Inv_FragGrenade"), named from the path.
// "Squaddie VARGAS, Heavy, Frag Grenade, promotion"
void hq_squad_row(const char* name, const char* nick, const char* class_desc,
                  const char* item1, const char* item2, const char* promote,
                  char* out, size_t out_sz);

// An inventory image path as the item's name: "...Inv_FragGrenade" ->
// "Frag Grenade", "...Inv_AssaultRifleModern" -> "Assault Rifle Modern".
// Empty when the path is not an inventory icon.
void hq_item_from_image(const char* path, char* out, size_t out_sz);
