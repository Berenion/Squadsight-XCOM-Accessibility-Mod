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
//
// A nickname in single quotes, as the game's own XGStrategySoldier.GetName
// (eNameType_Nick) gives it: quoted unless it already starts or ends with a
// quote. The strategy screens are sent it that way, and quoting it again read
// "''D.O.A.''" (2026-09-27). Empty for an empty nickname.
void hq_nick_quoted(const char* nick, char* out, size_t out_sz);

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
// Events are kept per list: two panels draw them (the base screen's "NEXT
// EVENT" and Mission Control's "UPCOMING EVENTS"). UpdateData sends every
// event to both -- the first only shows one -- and each redraws only while
// shown, so the list drawn last is the one read. Reading the fuller one read a
// list drawn before the council report, after it.
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

// ---- the Situation Room (Delete in the room) ---------------------------------
//
// UISituationRoom's main view is a display with no cursor: its arrows all go
// to the facility submenu. What it shows, from MainSituationViewState's
// PushedState (UpdateData, RealizeMap, RealizeObjectives):
//
//     AS_SetCountryInfo(int iIndex, countryName, cash, int panicLevel,
//                       bool bIsActive)
//         -- one per Council nation, 16, in SortSitCountries' order. cash is
//            "+\xC2\xA7" "100" when the country funds XCOM or has satellite
//            coverage, else "". panicLevel is XGCountry.GetPanicBlocks(),
//            1..5. bIsActive is false once the country has left XCOM.
//     AS_SetTickerText(title, txt) -- "WORLD NEWS", and the items joined
//            by "//", each coloured by its own state
//     AS_SetDoomLevel(int Level) -- World().m_iNumCountriesLost. The limit
//            (LOSE_CONDITION_NUM_DESERTERS) is never sent.
//     UIObjectivesScreen.AS_SetSmallBody(brief) / AS_SetLargeBody(large)
//         -- the brief is the sub-objectives as bullets, joined by <br>; the
//            large one each with its in-depth text, or "NONE".
//
// Only the panic level told a sighted player anything the names did not, and
// it was a number, so it never showed. Kept here and opened as a list
// (history_page_open) by Delete while the room is up.

#define HQ_SIT_COUNTRIES 24
#define HQ_SIT_NEWS      16
#define HQ_SIT_LINES     (HQ_SIT_COUNTRIES + HQ_SIT_NEWS + 4)
#define HQ_SIT_TEXT      1024

void hq_sit_country(int index, const char* name, const char* cash, int panic, int active);
// The ticker's whole text, items split on "//".
void hq_sit_news(const char* title, const char* text);
void hq_sit_doom(int lost);
void hq_sit_objectives(const char* brief, const char* large);

// The room as list entries, in the order the screen reads: countries lost,
// then each country, the news, the objectives. Returns how many.
//     "Countries lost: 0"
//     "UNITED STATES, panic 2 of 5"
//     "GERMANY, panic 1 of 5, funding +100"
//     "NIGERIA, panic 5 of 5, left XCOM"
//     "WORLD NEWS: Activists in China say aliens are real ..."
//     "OBJECTIVES: NONE"
int  hq_sit_lines(char lines[][HQ_SIT_TEXT], int max);

// ---- Build Facilities (the base's cross-section) -----------------------------
//
// A grid 7 tiles wide; level 0 is the surface and never drawn, so the cursor
// moves over levels 1..4 (XGBuildUI.GetTilesWide/High, OnCursor*). Every
// move runs XGBuildUI.UpdateView and then the screen's GoToView(0):
//
//     UIBuildFacilities.UpdateData
//         AS_UpdateFacilityCard(x, y, sName, icon, bDisabled, adjLeft, adjTop)
//             -- once per terrain tile, then once per facility, which draws
//                over the tile under it. icon is GetFacilityLabel's name:
//                Rock, RockSteam, Excavated, BeingExcavated, Construction,
//                or the facility's ("OfficerTrainingSchool"). sName is
//                label "<br>" counter: "STEAM", "Excavating<br>5 days", the
//                facility's name. The tile under the cursor has its STEAM
//                label taken off (RemoveTerrainTileLabelAtIndex).
//     UIBuildFacilities.UpdateCursor
//         AS_SetCursor(x, y, text, iUIState)
//             -- what Enter does and its cost ("EXCAVATE<br>COST: ..."), or
//                why it cannot: "Disabled for Tutorial", "Requires an access
//                lift", ... In the tutorial every tile but an empty,
//                excavated one off the lift's column is disabled.
//
// Nothing said where the cursor was or what was under it. The cards are kept
// and the cursor's tile is said with the cursor's text.

#define HQ_BASE_W 7
#define HQ_BASE_H 5

void hq_base_card(int x, int y, const char* name, const char* icon);
// What to say for the cursor at x, y with its text (line breaks already
// stops, markup stripped). "Excavated. EXCAVATE... Level 2, column 4."
void hq_base_cursor(int x, int y, const char* text, char* out, size_t out_sz);

// ---- choosing a country on the Situation Room's map --------------------------
//
// Launch Satellite (UISituationRoom's SatelliteState) and EW's covert ops
// (InfiltratorState) move a selection over the countries, and the only name
// for it is on UISituationRoomHUD. Each move runs the state's
// RealizeSelected:
//
//     SetTargetCountry(iEnum); UpdateHUD();
//         UISituationRoomHUD.AS_SetContinentInfo(name, bodyText, 0)
//             -- satellite: the continent and its bonus per satellite count,
//                one per line; covert ops: name "" and the clues instead
//         UISituationRoomHUD.AS_SetLaunchButton(icon, msg, bool enabled)
//         UISituationRoomHUD.AS_SetAccuseButton(icon, msg, bool enabled)
//         UISituationRoomHUD.AS_SetCountryInfo(name, bodyText, int panic)
//             -- satellite: "+\xC2\xA7" "180 per month", and on a new line why
//                no satellite can go there when the launch button is off
//         UISituationRoom.AS_SetSatellites(available, label, inOrbit, max, label)
//     Invoke("SetSelected", [index])  -- -1 when the state is left
//
// The index is the country's place on the map; the general path looked it up
// in the room's own slot table and read "_hq" or "Available, In Orbit".
// Everything the HUD drew is kept, and said at SetSelected.

void hq_sat_country(const char* name, const char* body, int panic);
void hq_sat_continent(const char* name, const char* body);
// which: 0 the launch button (Enter), 1 accuse.
void hq_sat_button(int which, const char* label, int enabled);
void hq_sat_count(int available, int in_orbit, int max);
// The state was left: the next entry says the satellite count again.
void hq_sat_reset(void);
// What to say for the country now selected. The continent is said only when
// it changed since the last time; the satellite count only on entering.
//     "Satellites: 1 available, 1 of 2 in orbit. UNITED STATES, panic 1 of
//      5. +180 per month. Enter: LAUNCH SATELLITE. North America: ..."
// Returns 0, with nothing written, when no country is known.
int  hq_sat_say(char* out, size_t out_sz);

// ---- a day passing (the geoscape's clock) ------------------------------------
//
// UIStrategyComponent_Clock.UpdateData sends AS_SetDateTime whenever the game's
// minute changes, rate-limited to 30 a second (m_fUpdateRate 0.033), and only
// while the clock is visible and focused -- so while time is running on the
// geoscape, and scanning above all, it is a steady stream. The first two
// strings are the date ("1 March", "2015" in English; the year first in
// Korean and Japanese), so the pair is compared whole, whatever the language.
//
// A day has passed when the date changes between two updates less than
// HQ_DAY_GAP_MS apart. A load, a return from a mission or the clock coming
// back after another screen all arrive after a longer silence, and are a
// jump rather than time passing: no tick.
#define HQ_DAY_GAP_MS 3000
int hq_day_passed(const char* date_a, const char* date_b, unsigned long long now_ms);

// ---- Mission Control's notices ----------------------------------------------
//
// XGMissionControlUI.AddNotice puts a short line on top of m_arrNotices --
// an item built, new scientists or engineers, an excavation finished, a
// soldier back from the infirmary -- and each lapses after its fTimer (4).
// UIMissionControl.UpdateNotices sends the whole list on every refresh, newest
// first, as one string: each notice followed by "\n", Invoke(
// "DisplayNotifications", [displayString]). Nothing read it.
//
// Feeds one such string (raw, the "\n"s kept) and returns how many of its
// lines were not in the one before -- the new notices -- joined in `out`, the
// oldest first. A soldier's abbreviated rank at the start of a line becomes
// the word: "Rk. Christophe Leroy has returned to active duty." ->
// "Rookie Christophe Leroy has returned to active duty."
#define HQ_NOTICES 16
int hq_notices_new(const char* raw, char* out, size_t out_sz);

// ---- Engineering -------------------------------------------------------------
//
// The staff count the facility shows along the top, sent to the strategy HUD
// on entering Research, Engineering, the Foundry and the MEC screens, and
// sent empty on leaving:
//
//     UIStrategyHUD.AS_SetHumanResources(resourceName, Amount)
//
// Two strings and no index, which the general path took for a two-item list.
// Kept for Delete's status line, after the date: "ENGINEERS: 10". Empty
// clears it.
void hq_status_human(const char* label, const char* value);

// A cost or duration panel, from its raw text (markup still in), in words.
// The game draws each requirement in its own colour and a missing one in
// red, which is the only place Build Items says what is short:
//
//     UIBuildItem.AS_UpdateInfo(itemName, infoText, descText, imgPath)
//         infoText = colored("Cost:") $ " " $ colored(req) $ "<br>" ...
//     UIManufacturing.AS_UpdateInfo(duration $ "\n" $ cost, notes, imgPath)
//
// A line break (<br>, \n) is a sentence break, "\xC2\xA7" "50" is "50 credits",
// and a red requirement is followed by "(not enough)" -- a red run ending in
// a full stop or "!" is a sentence ("Insufficient funds.") and is left as
// it is. "Cost: 50 credits (not enough), 10 Alloys".
void hq_cost_text(const char* raw, char* out, size_t out_sz);

// One row of Build Items (UIBuildItem.UpdateLayout), sent as pairs in one
// Invoke("BatchAddOptions", [label, quantity, label, quantity, ...]). The
// label is the item's name coloured by state: red when it cannot be built
// now (iState 1: the cost is not met). The quantity is how many are in
// storage, under the column heading from AS_SetLabels ("BUILT"). Left out
// when none. "LASER RIFLE, BUILT: 2", "ARC THROWER, unavailable".
void hq_build_row(const char* raw_label, int quantity, const char* qty_label,
                  char* out, size_t out_sz);

// One order in the build queue (UIStrategyHUD_BuildQueue.UpdateData):
//
//     AS_AddProjectToQueue(desc, engineers "", qty, eta, int uistate)
//
// desc "Laser Rifle (2)" or "Foundry: <project>"; qty "1/2" -- built of
// ordered -- or "---" for a Foundry project; eta "3 Days", "12 Hours", or
// "--" when no engineer is on it. "Laser Rifle (2), 1 of 2 done, 3 Days";
// "Foundry: SHIV Suppression, no engineers".
void hq_queue_row(const char* desc, const char* qty, const char* eta,
                  char* out, size_t out_sz);

// The queue as the game last drew it, for Delete in Engineering. The title
// is AS_SetQueueTitle's first string: "CURRENT PROJECTS", or "NO CURRENT
// PROJECTS" when it is empty.
#define HQ_QUEUE 16
void hq_queue_clear(void);
void hq_queue_title(const char* title);
void hq_queue_add(const char* row);

// Engineering as list entries: the queue's title, each order, then the
// base's status line, which carries the staff ("ENGINEERS: 10"). Returns
// how many.
int  hq_eng_lines(char lines[][HQ_SIT_TEXT], int max);

// ---- a research report (UIScienceLabs) ----------------------------------------
//
// The archives (Research, "ACCESS RESEARCH ARCHIVES") and the report shown
// when research finishes are one screen. RealizeReport sends
//
//     AS_SetReportTitles(TitleText, codename $ "\n" $ date)
//     AS_SetReportItem(subject, Notes, imagePath)
//     AS_ClearResults(), then AS_AddResults(line) per result, coloured
//
// on arrival (the subject empty when the list is up) and on opening an entry.
// The report itself has no cursor: up and down scroll it
// (AS_ScrollResearchUp / Down). Nothing read any of it; the notes were in the
// log, cut at 256 characters.
//
// Kept here and said whole on arrival; up and down then walk it a piece at a
// time. The pieces: "Weapon Fragments", each sentence of the notes, each
// result, then "Research Report. Codename: Sagaris. March, 2015". Game thread
// only.
#define HQ_REPORT_PIECES 64
#define HQ_REPORT_TEXT   1024

void hq_report_titles(const char* title, const char* sub);
void hq_report_item(const char* subject, const char* notes);
void hq_report_results_clear(void);
void hq_report_result(const char* text);

// Whether a report is up to be read: a subject has been sent.
int  hq_report_ready(void);

// The report whole: "Weapon Fragments. <notes> Results: S.C.O.P.E. available
// for manufacture. Research Report. Codename: Sagaris. March, 2015."
void hq_report_text(char* out, size_t out_sz);

// The pieces, as above. Returns how many.
int  hq_report_pieces(char pieces[][HQ_REPORT_TEXT], int max);
