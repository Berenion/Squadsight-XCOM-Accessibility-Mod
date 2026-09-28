// XCOM EU/EW accessibility hook: capture UI text on its way to Scaleform and
// speak it.
//
// All of the game's on-screen text reaches Flash through a small number of
// natives that share a useful property: the text is already evaluated and
// sitting in the *calling* UnrealScript function's frame by the time the
// native runs.
//
//   GFxMoviePlayer.ActionScriptVoid  -- 85% of text.  The call site passes
//       only a movieclip path and the native reads the rest off the caller:
//
//           simulated function AS_SetText(string DisplayText)
//           { manager.ActionScriptVoid(string(GetMCPath()) $ ".SetText"); }
//
//   UI_FxsPanel.Invoke               -- the remaining 15%.  Its arguments are
//       an explicit ASValue array, which the caller fills from locals:
//
//           simulated function SetText()            // no parameters at all
//           {
//               myValue.S = m_sSinglePlayer; myArray.AddItem(myValue);
//               myValue.S = m_sMultiplayer;  myArray.AddItem(myValue);
//               Invoke("SetDisplay", myArray);
//           }
//
// So one capture routine serves every target: read the caller's frame, never
// Stack.Code.  Re-stepping the bytecode would re-evaluate argument
// expressions and double any side effects they carry; reading the frame
// cannot.
//
// The frame is decoded by walking the caller's UProperty chain -- the same
// walk the engine's own ActionScript marshaller performs -- so each value is
// read at its declared offset and reported with its declared name.  That
// includes locals, not just parameters: the example above shows why a
// parameter-only filter would report "no string params" for a whole menu.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <share.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include "ue3.h"
#include "natives.h"
#include "names.h"
#include "speech.h"
#include "focus.h"
#include "dialog.h"
#include "scan.h"
#include "objects.h"
#include "help.h"
#include "shot.h"
#include "combat.h"
#include "history.h"
#include "soldier.h"
#include "info.h"
#include "sight.h"
#include "mission.h"
#include "abar.h"
#include "hq.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "sonar.h"
#include "audio.h"
#include "learn.h"
#include "settings.h"
#include "mouse.h"
#include "props.h"
#include "input.h"
#include "log.h"
#include "game.h"
#include "units.h"
#include "report.h"
#include "where.h"
#include "world.h"

#define MAX_NATIVES 8192
#define MAX_STR     4096
#define MAX_FIELDS  64
#define MAX_ELEMS   64      // array elements inspected per property

static volatile LONG    g_calls;
static int              g_speak = 1;

// Consecutive duplicates are collapsed: the UI re-sends the same string on
// every refresh, which would otherwise bury the interesting transitions --
// and would make the speech unusable.
static char g_last_spoken[MAX_STR];

// Calls that arrive every frame and that nothing here reads: where each
// flag, message and the reticle sit on screen, and the radar's blips. They
// were most of the log -- UIUnitFlag.SetPosition alone was 16,243 of 37,275
// call lines on 2026-09-25 -- and buried the calls around them. Counted,
// and the counts logged every LOG_FRAME_MS instead. Only the log line is
// skipped: the call is handled as before.
#define LOG_FRAME_MS 10000
static const char* const k_frame_calls[][2] = {
    { "UIUnitFlag_",          "SetPosition" },
    { "UITacticalHUD_Radar_", "AS_UpdateBlips" },
    { "UIWorldMessageMgr_",   "UpdateMessageLocation" },
    { "UITargetingReticle_",  "SetLoc" },
};
#define LOG_FRAME_KINDS ((int)(sizeof k_frame_calls / sizeof k_frame_calls[0]))

static int log_frame_call(const char* obj, const char* fn)
{
    static long      counts[LOG_FRAME_KINDS];
    static ULONGLONG since;
    int hit = -1;
    for (int i = 0; i < LOG_FRAME_KINDS && hit < 0; i++)
        if (strcmp(fn, k_frame_calls[i][1]) == 0 &&
            strncmp(obj, k_frame_calls[i][0], strlen(k_frame_calls[i][0])) == 0)
            hit = i;
    if (hit < 0) return 0;
    counts[hit]++;
    ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (now - since >= LOG_FRAME_MS) {
        char line[256];
        size_t used = 0;
        line[0] = 0;
        for (int i = 0; i < LOG_FRAME_KINDS; i++) {
            if (!counts[i]) continue;
            // "UIUnitFlag.SetPosition": the prefix without its underscore.
            int w = _snprintf_s(line + used, sizeof line - used, _TRUNCATE, "%s%.*s.%s %ld",
                                used ? ", " : "",
                                (int)strlen(k_frame_calls[i][0]) - 1, k_frame_calls[i][0],
                                k_frame_calls[i][1], counts[i]);
            if (w < 0) break;
            used += (size_t)w;
            counts[i] = 0;
        }
        logf_("log: per-frame calls, not logged, over %u s: %s\n",
              (unsigned)((now - since) / 1000), line);
        since = now;
    }
    return 1;
}

// Flash markup leaks into a lot of these strings; a screen reader should not
// read tags aloud.
static void strip_markup(char* s)
{
    char* w = s;
    int depth = 0;
    int brk = 0;
    for (char* r = s; *r; r++) {
        // A line break is a pause, not nothing: the unlock popup is
        // strName $ "<br><br>" $ strDescription $ "<br><br>" $ strHelp
        // (XComPresentationLayerBase), and dropping the tags read
        // "LaboratoryEach laboratory ...". A stop where the line had none.
        // Written once the tag has closed: by then the tag's four or more
        // characters are behind the reader, so the two written cannot
        // overtake it.
        if (*r == '<' && !depth && (r[1] == 'b' || r[1] == 'B') &&
            (r[2] == 'r' || r[2] == 'R') && (r[3] == '>' || r[3] == '/' || r[3] == ' '))
            brk = 1;
        if (*r == '<') { depth++; continue; }
        if (*r == '>') {
            if (depth) depth--;
            if (!depth && brk) {
                brk = 0;
                char* b = w;
                while (b > s && b[-1] == ' ') b--;
                if (b > s && !strchr(".!?:;,", b[-1])) *w++ = '.';
                *w++ = ' ';
            }
            continue;
        }
        if (!depth) *w++ = *r;
    }
    *w = 0;

    char* out = s;
    int space = 0;
    for (char* r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!space && out != s) *out++ = ' ';
            space = 1;
        } else {
            *out++ = (char)c;
            space = 0;
        }
    }
    while (out > s && out[-1] == ' ') out--;
    *out = 0;
}

// A sum as the game draws it, "§50", as said: "50 credits". The section sign
// (U+00A7) reads "section". "-" and "" are left as they are.
static void money_text(const char* in, char* out, size_t out_sz)
{
    size_t w = 0;
    for (const char* r = in ? in : ""; *r && w + 1 < out_sz; r++) {
        if ((unsigned char)r[0] == 0xC2 && (unsigned char)r[1] == 0xA7) { r++; continue; }
        out[w++] = *r;
    }
    out[w] = 0;
    const char* d = (out[0] == '+' || out[0] == '-') ? out + 1 : out;
    if (*d >= '0' && *d <= '9')
        strncat_s(out, out_sz, strcmp(d, "1") == 0 ? " credit" : " credits", _TRUNCATE);
}

// Plenty of these "strings" are asset references rather than prose --
// "Icon_B_CIRCLE", "img:///UILibrary_MapImages.Command1".  They belong in the
// log, because they identify the call, but reading them aloud is noise.
static int looks_like_asset(const char* s)
{
    if (strncmp(s, "img:", 4) == 0) return 1;
    if (strncmp(s, "Icon_", 5) == 0) return 1;
    if (strstr(s, "://")) return 1;

    // package.asset style: dotted, and no spaces anywhere.
    int dot = 0;
    for (const char* p = s; *p; p++) {
        if (*p == ' ') return 0;
        if (*p == '.') dot = 1;
    }
    return dot;
}

// A settings widget states its name and its value through two calls carrying
// the same index, so they must be filed as different parts of one control
// (UIWidgetHelper.uc):
//
//     SetSpinnerLabel(int Index, string strText)     "Mode:"
//     SetSpinnerValue(int Index, string StrValue)    "Fullscreen"
//     SetComboboxLabel(int Index, string strText)    "Resolution:"
//     SetComboboxText(int Index, string strText)     "1920 x 1080"
//
// Matched on a substring because the same setters also arrive AS_-prefixed.
static int is_value_fn(const char* fn)
{
    return strstr(fn, "SpinnerValue") != NULL ||
           strstr(fn, "ComboboxText") != NULL;
}

// A container widget publishes every choice it offers in one call:
//
//     SetDropdownOptions(int Index, array<string> arrLabels)
//     SetListOptions(int Index, array<string> arrLabels)
//
// That is the contents of one widget, not the widget's own text, so filing it
// as a label joins the lot into a single slot. EU's difficulty screen is a
// list where EW's is a row of checkboxes, and it read as one run-on string:
// "Easy, Normal, Classic, Impossible, Easy,0 ; Normal,1 ; ..." -- the trailing
// part being the raw data string Flash is handed.
static int is_option_list_fn(const char* fn)
{
    return strstr(fn, "DropdownOptions") != NULL ||
           strstr(fn, "ListOptions")     != NULL;
}

#define LIST_WINDOW_MS 400   // repeats closer than this are one list
#define SETTLE_MS      250   // how long a lone line waits to see if more follow
#define TITLE_FRESH_MS 2000  // a heading older than this belongs to no move

// A screen's heading, sent on a call of its own with one string:
//
//     UIContinentSelect.AS_SetTitle(string displayString)
//     UIMissionControl_MissionList.AS_SetTitle(...)
//
// Matched on the ending, since the name comes both AS_-prefixed and bare. A
// title call carrying two strings (UIStrategyComponent_EventList sends
// "UPCOMING EVENTS" and "DAYS", a heading per column) is not taken here.
static int is_title_fn(const char* fn)
{
    size_t n = strlen(fn);
    return n >= 8 && strcmp(fn + n - 8, "SetTitle") == 0;
}

// The last move said, so a panel arriving just after it can follow it.
static void*     g_focus_obj;
static int       g_focus_idx = -1;
static ULONGLONG g_focus_at;
static int       g_focus_had_panel;

// When a screen last dispatched a key (rewrite_cmd). A selection that names
// the item already said, with no key behind it, is a screen redrawing:
// UIMissionControl_MissionList.RealizeSelected re-sends "0" on every
// geoscape refresh, three times in the first seconds at the base.
static ULONGLONG g_ui_key_at;

// Keys still owed to a mod menu that just closed: the key that closed it has
// events left to come, and they must not reach whatever is underneath.
static ULONGLONG g_menu_grace_until;
#define REDRAW_QUIET_MS 1000
#define NARRATIVE_REPEAT_MS 30000  // a comm-link line re-sent within this is one line

// The base's facility menu, once it has published.
static void*     g_hq_menu;

// The soldier a soldier screen is about, from its header panels.
static char      g_soldier_info[256];

// A soldier's medals are drawn as icons, AS_SetMedals("defender,honor")
// (UIUtilities.GetMedalLabels), and medals can be renamed. So each icon's name
// starts as the game's default (XGFacility_Barracks.m_arrMedalNames) and takes
// whatever the Medals screen last showed for it (UIMedals.AS_SetInfo carries
// both the name and the icon).
static const char* const MEDAL_ICON[] = { "urbancombat", "defender", "international",
                                          "honor", "starofterra" };
static char g_medal_name[5][96] = { "Urban Combat Badge", "Defender's Medal",
                                    "International Service Cross", "Council Medal of Honor",
                                    "Star of Terra" };

static void medal_learn(const char* icon, const char* name)
{
    if (!icon || !name || !name[0]) return;
    for (int i = 0; i < 5; i++)
        if (strcmp(icon, MEDAL_ICON[i]) == 0)
            strncpy_s(g_medal_name[i], sizeof g_medal_name[i], name, _TRUNCATE);
}

// "defender,honor" -> "Medals: Defender's Medal, Council Medal of Honor".
static void medal_line(const char* icons, char* out, size_t out_sz)
{
    out[0] = 0;
    char buf[256];
    strncpy_s(buf, sizeof buf, icons ? icons : "", _TRUNCATE);
    size_t used = 0;
    char* ctx = NULL;
    for (char* t = strtok_s(buf, ",", &ctx); t; t = strtok_s(NULL, ",", &ctx)) {
        const char* name = t;
        for (int i = 0; i < 5; i++) if (strcmp(t, MEDAL_ICON[i]) == 0) name = g_medal_name[i];
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s",
                            used ? ", " : "Medals: ", name);
        if (w < 0) break;
        used += (size_t)w;
    }
}
static char      g_soldier_stats[128];
static ULONGLONG g_soldier_info_at;
#define SOLDIER_INFO_FRESH_MS 3000

// The soldier summary's heading. Opening it draws the header first and the
// menu after; every refresh -- back from Loadout, Tab, Left Shift -- does
// UpdateData (menu, selection) and then UpdatePanels (header), in both
// builds. So the soldier is said when it changes, from whichever comes
// second: kept as the one last said, and forgotten on leaving the summary.
static char      g_summary_said[256];
static char      g_summary_dropship[64];   // "ON MISSION", or empty
static void*     g_summary_obj;
static int       g_summary_titled;         // this pass put the soldier in the heading
static ULONGLONG g_summary_at;
#define SUMMARY_HEADER_MS 1000

// The debrief page last said, for Up and Down to say again where the page
// gives them nothing to do (rewrite_cmd); 0 when they are the page's own.
static char      g_debrief_page[4096];
static int       g_debrief_rereads;

// The dialogue box last said, for Up and Down to say again (rewrite_cmd).
static char      g_dialog_said[DIALOG_MAX_TEXT];

// The mission summary on screen, once it has drawn; see msum_screen_up.
static void*     g_msum_screen;

// The ship summary's two buttons (UIShipSummary.AS_SetWeaponHelp): EDIT
// LOADOUT and DISMISS SHIP, with whether each is disabled. Filed by the help
// path, which takes every *Help call before the screen's own handler runs.
static char      g_ship_btn[2][FOCUS_MAX_LABEL];
static int       g_ship_btn_off[2];

// The labs with soldier slots, as last drawn. See slots_select.
#define SLOT_ROWS 8
static void*     g_slots_obj;
static char      g_slots_title[96];
static char      g_slots_row[SLOT_ROWS][FOCUS_MAX_LABEL];
static int       g_slots_button[SLOT_ROWS];
static int       g_slots_n, g_slots_sel = -1;
static void      slots_select(LONG n, int i, const char* lead);

// The end-of-month report (UIWorldReport, then UIEndOfMonthReport). See the
// handler in capture_body. The summary page is kept a line at a time for the
// arrows, which the report itself ignores: UIEndOfMonthReport hands every key
// to UIWorldReport, whose OnUnrealCommand acts on Enter, Space and A only.
#define EOM_LINES 48
#define EOM_TEXT  512
static char      g_eom[EOM_LINES][EOM_TEXT];
static int       g_eom_n;               // lines kept; 0 until the summary is drawn
static int       g_eom_head;            // how many of them the arrival said
static int       g_eom_at;              // the line the arrows last said
static int       g_eom_said;            // the summary has been said on arrival
static char      g_eom_page[EOM_TEXT * 2]; // the decryption or defections page, said again
static int       g_eom_link;            // this report's decryption page is up

// Up or Down on the report. The summary a line at a time, "Top." and "End."
// at either end; before it is drawn, the page on screen again -- the
// decryption or the countries that have withdrawn.
static void eom_walk(LONG n, const char* screen, int down)
{
    const char* say = g_eom_page;
    char line[EOM_TEXT + 8];
    if (g_eom_said && g_eom_n) {
        const char* edge = "";
        g_eom_at += down ? 1 : -1;
        if (g_eom_at >= g_eom_n) { g_eom_at = g_eom_n - 1; edge = "End. "; }
        if (g_eom_at < 0)        { g_eom_at = 0;           edge = "Top. "; }
        _snprintf_s(line, sizeof line, _TRUNCATE, "%s%s", edge, g_eom[g_eom_at]);
        say = line;
    }
    if (!say[0]) return;
    logf_("[%ld] Input        %s  REPORT %d \"%s\"\n", n, screen, g_eom_at, say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

// When each layer's HUD last drew. See capture_body.
static volatile ULONGLONG g_seen_strategy_at;
static volatile ULONGLONG g_seen_tactical_at;
// When the Situation Room last drew or took a key, and when the player was
// last seen somewhere else. See sitroom_up.
static volatile ULONGLONG g_sitroom_at;
static volatile ULONGLONG g_sitroom_left_at;
// The same for Engineering and the screens it opens (eng_up), whose build
// queue Delete reads there.
static volatile ULONGLONG g_eng_at;
static volatile ULONGLONG g_eng_left_at;
// When Build Items and an order last drew. The strategy HUD's help bar and a
// widget helper belong to no screen by name, so a call arriving within a
// moment of one of these is taken as that screen's.
static ULONGLONG g_builditem_at;
static ULONGLONG g_man_at;
#define ENG_SAME_DRAW_MS 100

// The hiring screen as it last drew. See the handler in capture_body.
static struct {
    void*     obj;
    ULONGLONG at;             // its last call, to claim the widget helper's
    int       fresh;          // nothing said yet on this visit
    char      title[96], cost[128], cap[128], confirm[48], count[32];
} g_hire;

// "Hiring Cost: §10." as said: "Hiring Cost: 10 credits." and "13/70" as
// "13 of 70".
static void hire_text(const char* in, char* out, size_t out_sz)
{
    size_t w = 0;
    for (const char* r = in; *r && w + 12 < out_sz; r++) {
        if ((unsigned char)r[0] == 0xC2 && (unsigned char)r[1] == 0xA7) {
            r += 2;
            const char* d = r;
            while (*r >= '0' && *r <= '9' && w + 12 < out_sz) out[w++] = *r++;
            w += (size_t)_snprintf_s(out + w, out_sz - w, _TRUNCATE,
                                     r - d == 1 && *d == '1' ? " credit" : " credits");
            r--;
            continue;
        }
        if (*r == '/' && r > in && r[-1] >= '0' && r[-1] <= '9' && r[1] >= '0' && r[1] <= '9') {
            w += (size_t)_snprintf_s(out + w, out_sz - w, _TRUNCATE, " of ");
            continue;
        }
        out[w++] = *r;
    }
    out[w] = 0;
}

// The finance statement (UIBaseFinances), a line at a time for the arrows,
// which the screen ignores: only Enter (nothing) and Escape (leave) have
// cases. See the handler in capture_body.
#define FIN_LINES 16
static char g_fin[FIN_LINES][512];
static int  g_fin_n, g_fin_at;
static char g_fin_said[FIN_LINES * 128];

static void fin_walk(LONG n, const char* screen, int down)
{
    if (!g_fin_n) return;
    const char* edge = "";
    g_fin_at += down ? 1 : -1;
    if (g_fin_at >= g_fin_n) { g_fin_at = g_fin_n - 1; edge = "End. "; }
    if (g_fin_at < 0)        { g_fin_at = 0;           edge = "Top. "; }
    char say[600];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s", edge, g_fin[g_fin_at]);
    logf_("[%ld] Input        %s  FINANCES %d \"%s\"\n", n, screen, g_fin_at, say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

// Whether the build queue is taking the arrows (Review an order): only then
// is its selection a move to say.
static int       g_queue_editing;
// The order open in UIManufacturing, as last said (see capture_body).
static struct {
    void* obj;
    int   said;
    char  title[128], eng[128], qty_label[64], qty[32], rush[96];
    char  said_rush[96], said_qty[96], said_eng[128];
    char  said_info[1024], said_notes[1024];
} g_man;

// The loadout's inventory list and its last selection (loadout_leave_locker).
static void*     g_loadout_inv;          // the inventory list, as focus keys it
static int       g_loadout_inv_idx = -1; // its last selection
static void*     g_loadout_side;         // the list the cursor was last said in

// A screen redrawing because of a key the game itself ignored has nothing to
// say, and saying it anyway talks over the answer.
//
// UIShellDifficulty.OnUnrealCommand ends with RefreshDescInfo() -- outside the
// switch, so it runs for *every* command, handled or not -- and that resends
// the current difficulty's description. Pressing the help key therefore
// produced the list and then, a moment later, the description again:
//
//     [82] Input   UIShellDifficulty_0  HELP MENU (3) "SECOND WAVE: 2. ..."
//     [83] Input   UIShellDifficulty_0  cmd 621
//     [86] ASVoid  UIShellDifficulty_0.AS_SetDifficultyDesc  "For players ..."
//
// So the hook goes quiet for a moment after answering a key of its own. The
// window is lifted by the next command rather than only by time, because the
// player pressing something else means they have moved on and whatever that
// key redraws is news again.
static ULONGLONG g_quiet_until;
#define QUIET_MS 1000

// The key whose events carry a menu choice already made, and the command it
// was told to mean.  See rewrite_cmd.
static int g_fired_key;
static int g_fired_cmd;

// What a swallowed command is turned into.  Every FXS range ends well below
// this -- controller 379, mouse 424, keyboard 700 -- so no switch anywhere has
// a case for it and no range test claims it.  Rewriting the command is the
// mechanism already proven by the key remaps; forcing the native's answer to
// false is cleaner in principle but has to reach a result pointer this code
// does not own, so both are applied and the log says which took.
#define CMD_INERT 900

// rewrite_cmd answers one question: should this command reach the screen?
#define DELIVER   0
#define SUPPRESS  1

static int muted(void)
{
    return g_quiet_until && GetTickCount64() < g_quiet_until;
}

// Says a slot's full text -- "Enable Ironman?: checked" -- after a short wait.
//
// A control that changes while the cursor sits on it has to announce itself,
// because nothing else will: flipping a checkbox does not move the selection,
// so RealizeSelected never fires and the change was silent. The wait matters
// because a screen redrawing itself can pass through intermediate states, and
// each new value cancels the last, so only the settled one is spoken.
static void speak_slot(void* obj, int idx)
{
    char text[FOCUS_MAX_LABEL];
    if (g_speak && !muted() && focus_label_at(obj, idx, text, sizeof text) && text[0])
        speech_say_after(text, SETTLE_MS);
}

// Which item of a container widget is chosen:
//
//     SetListSelection(int Index, int iSelection)        (both)
//     SetComboboxValue(int Index, int iSelectionIndex)   (both)
//
// Two integers, so the first picks the widget and the second the item inside
// it -- unlike every other setter here, where the leading int is the slot and
// nothing follows.
static int is_inner_selection_fn(const char* fn)
{
    return strstr(fn, "ListSelection")   != NULL ||
           strstr(fn, "ComboboxValue")   != NULL ||
           strstr(fn, "DropdownSelection") != NULL;
}

// A screen announcing which item of a list is current, where the list was
// published by a different object:
//
//     AS_SetCurrentDifficultyMarker(int Index)    on UIShellDifficulty
//
// while the items came from UIWidgetHelper.SetListOptions. One integer, no
// text, and the index is into the list rather than into any slot table.
static int is_marker_fn(const char* fn)
{
    return strstr(fn, "Marker") != NULL;
}

// A checkbox's state arrives without any text of its own:
//
//     SetCheckboxValue(int Index, bool bChecked)      (EU)
//     AS_SetCheckboxValue(int Index, bool bChecked)   (EW)
//
// Matched on a substring because both builds route the same logical setter
// through a different native, one AS_-prefixed and one not.
static int is_checkbox_state_fn(const char* fn)
{
    return strstr(fn, "CheckboxValue") != NULL;
}

// A slider states where it sits as a bare number, on a call of its own:
//
//     SetSliderValue(int Index, int iValue)            (UIWidgetHelper.uc)
//
// and the number is a percentage every time.  OnUnrealCommand_Slider clamps
// iValue to 0..100 in both builds, and every slider is filled from a
// percentage: the three volumes are stored 0..100, edge-scroll speed is
// m_fScrollSpeed * 100, and gamma is GetGammaPercentage(), which normalises
// 1.7..2.7 onto the same scale.  "Every" is exact rather than hopeful --
// UIOptionsPCScreen is the only caller of NewSlider() in either build -- so
// "50 percent" describes the slider and not merely the number on it.
//
// Its neighbour must not be mistaken for it -- it arrives one call later,
// carrying the same slot and a number that is not the value:
//
//     SetSliderMouseWheelStep(int Index, int iValue)   the step, 10
//
// A value is not necessarily a multiple of that step, and reading one like
// "29 percent" is not a fault to be rounded away.  OnUnrealCommand_Slider
// adds or subtracts the step and clamps, never snapping, while
// ProcessMouseEvent_Slider's default case assigns the dragged position
// straight in -- so one click on the bar leaves an offset that every later
// keypress carries: 29, 39, 49.  Only the clamps at 0 and 100 clear it.
static int is_slider_value_fn(const char* fn)
{
    return strstr(fn, "SliderValue") != NULL;
}

// Does a text-free call carrying a number mean "the cursor moved"?
//
// Most of them do not.  The widget setters take the *widget's* index, not the
// selection -- UIWidgetHelper.uc:
//
//     simulated function SetReadOnly(int Index, bool bReadOnly)
//     private final simulated function SetSliderValue(int Index, int iValue)
//     private final simulated function SetSpinnerArrows(int Index, bool bCanSpin)
//
// and each runs once per widget as a screen is built, so reading them as
// cursor moves narrates the whole screen on arrival -- the exact behaviour
// focus tracking exists to prevent.  Measured on the options screens: 274 such
// calls against 52 real moves.
//
// The genuine signal is the ActionScript method `SetSelected`, but that name
// lives in the native's own argument, which is still unevaluated bytecode at
// hook entry -- reading it would mean stepping Stack.Code and re-running the
// argument expressions.  The callers are used instead: every UnrealScript
// function that sends SetSelected was enumerated from the decompiled source of
// both builds, and they are named consistently.
//
// Arity is not the test.  RealizeSelected() takes nothing and reads
// m_iCurrentWidget, but UILoadGame.SetSelected(int iTarget) and
// UIProtoWidget_Menu.SetSelected(int iItem) are equally genuine and do take
// the index as a parameter.
static int is_selection_fn(const char* fn)
{
    return strncmp(fn, "RealizeSelected", 15) == 0 ||   // + _Inventory, _Locker, Tab
           strncmp(fn, "AS_SetSelected",  14) == 0 ||   // + MainMenu, SubMenu, Tab, Category, Icon
           strncmp(fn, "SetSelected",     11) == 0 ||   // + BoundsCheck, MenuOption, Item
           strcmp (fn, "Deselect")        == 0 ||
           strcmp (fn, "SelectNextMenu")  == 0 ||
           strcmp (fn, "SelectPrevMenu")  == 0 ||
           // The pause menu is the one screen that does not spell it as a
           // variant of SetSelected. UIPauseMenu.SetSelected(int iTarget)
           // stores the index and delegates to AS_Selected(int iTarget),
           // which is the frame that reaches the native -- so the name the
           // hook sees is AS_Selected, and the prefixes above all miss it.
           // Exactly matched rather than prefixed: AS_Selected is declared
           // once in each build, and only on UIPauseMenu.
           strcmp (fn, "AS_Selected")     == 0;
}


// UnrealScript's ASValue is a 24-byte { int Type; int B; float N; FString S },
// which the observed array offsets confirm: the main menu's labels landed at
// byte 12, 36, 60, 84, 108 -- stride 24, string at +12.  Type at +0 says which
// member is live (2 = AS_Number, 3 = AS_String), so these arrays are parsed
// rather than swept, and a selection index is as readable as a label.
#define ASVALUE_STRIDE 24
#define ASVALUE_TYPE   0
#define ASVALUE_B      4
#define ASVALUE_N      8
#define ASVALUE_S      12

#define AS_NUMBER 2
#define AS_STRING 3
#define AS_BOOL   4

typedef struct {
    char  strings[FOCUS_MAX_LABELS][FOCUS_MAX_LABEL];
    int   nstrings;
    float numbers[8];
    int   nnumbers;
    int   bools[8];
    int   nbools;
    // Bools found inside an ASValue array, apart from the parameter bools
    // above: everything that reads `bools` was written expecting parameters
    // only, and a setter passes the same flag both ways.
    int   abools[8];
    int   nabools;
    // The colour each string was drawn in, 0xRRGGBB, or -1: see font_hue.
    int   hues[FOCUS_MAX_LABELS];
} Payload;

static void payload_add_string(Payload* p, const char* s)
{
    if (p->nstrings >= FOCUS_MAX_LABELS) return;
    strncpy_s(p->strings[p->nstrings], FOCUS_MAX_LABEL, s, _TRUNCATE);
    p->hues[p->nstrings] = -1;
    p->nstrings++;
}

// The colour a string's markup gives it, read before strip_markup throws the
// markup away. For some lists the colour is the only thing that says an item
// cannot be chosen: UIContinentSelect.UpdateLayout sends every continent with
// the same iState of 0 and marks a locked one only by drawing it through
// GetHTMLColoredText(label, 3) -- "<font color='#EE1C25'>ASIA</font>".
static int font_hue(const char* raw)
{
    const char* c = strstr(raw, "color='#");
    if (!c) return -1;
    unsigned v = 0;
    c += 8;
    for (int i = 0; i < 6; i++, c++) {
        int d = (*c >= '0' && *c <= '9') ? *c - '0'
              : (*c >= 'A' && *c <= 'F') ? *c - 'A' + 10
              : (*c >= 'a' && *c <= 'f') ? *c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + (unsigned)d;
    }
    return (int)v;
}

// Whether a list item drawn in `hue` is one the game will refuse.
//
// The continent screen, in the tutorial's campaign: XGContinentUI.
// UpdateMainMenu enables only North America and Europe when ISCONTROLLED(),
// giving the other three iState 1, and UpdateLayout draws those in state 3's
// red. Enter on one runs OnChooseCont, which plays the bad sound and nothing
// else -- no text anywhere says why. Scoped to the one screen: red elsewhere
// is a warning, not a lock.
#define HUE_BAD      0xEE1C25
#define HUE_DISABLED 0x808080   // GetHTMLColoredText's state 1
static int hue_unavailable(const char* obj_name, const char* fn_name, int hue)
{
    return hue == HUE_BAD &&
           strncmp(obj_name, "UIContinentSelect", 17) == 0 &&
           strcmp(fn_name, "AS_AddOption") == 0;
}

// A facility submenu's option says it cannot be chosen through its state,
// which is passed through as a plain number rather than drawn as a colour:
//
//     UIStrategyHUD_FacilitySubMenu.AS_AddOption(int Index, string Text, int State)
//
// from the manager's TMenuOption.iState, where 1 is disabled.
static int state_unavailable(const char* obj_name, const char* fn_name,
                             const Payload* p)
{
    return strncmp(obj_name, "UIStrategyHUD_FSM_", 18) == 0 &&
           strcmp(fn_name, "AS_AddOption") == 0 &&
           p->nnumbers >= 2 && (int)p->numbers[1] == 1;
}

static void payload_add_number(Payload* p, float v)
{
    if (p->nnumbers >= 8) return;
    p->numbers[p->nnumbers++] = v;
}

static void payload_add_bool(Payload* p, int v)
{
    if (p->nbools >= 8) return;
    p->bools[p->nbools++] = v != 0;
}

// Reads a TArray as ASValues when it looks like one, falling back to a sweep
// for plain TArray<string>.
static void read_array(const FArray* a, Payload* out)
{
    if (!readable(a, sizeof *a)) return;
    if (a->Num < 1 || a->Num > 4096) return;
    if (a->Max < a->Num) return;

    int elems = a->Num > FOCUS_MAX_LABELS ? FOCUS_MAX_LABELS : a->Num;
    const uint8_t* base = (const uint8_t*)a->Data;

    if (readable(base, (size_t)elems * ASVALUE_STRIDE)) {
        int typed = 0;
        for (int i = 0; i < elems; i++) {
            int32_t ty = *(const int32_t*)(base + (size_t)i * ASVALUE_STRIDE + ASVALUE_TYPE);
            if (ty >= 0 && ty <= 4) typed++;
        }
        // Every element carrying a valid ASType is strong evidence; a random
        // struct array will not satisfy it.
        if (typed == elems) {
            char val[MAX_STR];
            for (int i = 0; i < elems; i++) {
                const uint8_t* e = base + (size_t)i * ASVALUE_STRIDE;
                int32_t ty = *(const int32_t*)(e + ASVALUE_TYPE);
                if (ty == AS_STRING) {
                    if (read_fstring((const FString*)(e + ASVALUE_S), val, sizeof val)) {
                        strip_markup(val);
                        if (*val) payload_add_string(out, val);
                    }
                } else if (ty == AS_NUMBER) {
                    payload_add_number(out, *(const float*)(e + ASVALUE_N));
                } else if (ty == AS_BOOL && out->nabools < 8) {
                    out->abools[out->nabools++] = *(const int32_t*)(e + ASVALUE_B) != 0;
                }
            }
            return;
        }
    }

    // Not an ASValue array: sweep for FStrings inside the array's own memory.
    size_t span = (size_t)elems * 16;
    if (!readable(base, span)) return;
    char val[MAX_STR];
    for (size_t off = 0; off + sizeof(FString) <= span; off += 4) {
        if (read_fstring((const FString*)(base + off), val, sizeof val)) {
            strip_markup(val);
            if (*val) payload_add_string(out, val);
            off += sizeof(FString) - 4;
        }
    }
}

// ---- the ability bar -------------------------------------------------------
//
// PopulateFlash's arrUpdateAbilitiesData is a command stream (abar.h), and
// read_array flattens it into strings and numbers, which loses both the
// order across types and the nulls that mark where each slot begins. So the
// frame is walked again here for the one array, and it is handed over whole.
//
// The array is found by what it is -- the frame's ArrayProperty whose every
// element carries a valid ASValue type -- not by its name.
#define ABAR_STREAM_MAX 512
// Room for a name in its <font color='#...'> wrapper: Build Items sends its
// labels coloured (UIBuildItem.UpdateLayout), 29 characters of markup.
#define ABAR_TEXT       128

static void*     g_abar_obj;            // the container the bar was kept for
static int       g_abar_logged_fail;

// The frame's command stream, typed: the first ArrayProperty whose every
// element carries a valid ASValue type. Returns how many values, or -1 when
// there is none. The strings live in static storage until the next call.
static int asvalues_from_frame(void* stack, AbarValue** out)
{
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!node || !locals) return -1;

    static AbarValue vals[ABAR_STREAM_MAX];
    static char      text[ABAR_STREAM_MAX][ABAR_TEXT];

    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) break;
        uint32_t off = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next   = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        char cls[64];
        if (off < 0x1000 && object_class_name(prop, cls, sizeof cls) &&
            strcmp(cls, "ArrayProperty") == 0) {
            const FArray* a = (const FArray*)(locals + off);
            if (readable(a, sizeof *a) && a->Num > 0 && a->Num <= ABAR_STREAM_MAX &&
                a->Max >= a->Num &&
                readable(a->Data, (size_t)a->Num * ASVALUE_STRIDE)) {
                const uint8_t* base = (const uint8_t*)a->Data;
                int ok = 1;
                for (int i = 0; i < a->Num && ok; i++) {
                    const uint8_t* e = base + (size_t)i * ASVALUE_STRIDE;
                    AbarValue* v = &vals[i];
                    v->type = *(const int32_t*)(e + ASVALUE_TYPE);
                    v->n = 0; v->b = 0; v->s = NULL;
                    switch (v->type) {
                    case ABAR_NULL: break;
                    case ABAR_NUMBER: v->n = *(const float*)(e + ASVALUE_N); break;
                    case ABAR_BOOL:   v->b = *(const int32_t*)(e + ASVALUE_B) != 0; break;
                    case ABAR_STRING:
                        // An empty FString is a real value here ("" hotkey);
                        // read_fstring refuses it, so it stands as "".
                        if (!read_fstring((const FString*)(e + ASVALUE_S), text[i], ABAR_TEXT))
                            text[i][0] = 0;
                        v->s = text[i];
                        break;
                    default: ok = 0;
                    }
                }
                if (ok) { *out = vals; return a->Num; }
            }
        }
        prop = next;
    }
    return -1;
}

static int abar_from_frame(void* stack)
{
    AbarValue* vals;
    int n = asvalues_from_frame(stack, &vals);
    return n < 0 ? -1 : abar_feed(vals, n);
}

// The cursor has landed on item `idx` of `object`'s list: say it, with the
// heading the first time the cursor lands after one, and the panel
// describing this item if the move just redrew it.
static void focus_announce(LONG n, const char* tag, const char* obj_name,
                           const char* fn_name, void* object, int idx)
{
    ULONGLONG now = GetTickCount64();
    char label[FOCUS_MAX_LABEL];
    if (!focus_label_at(object, idx, label, sizeof label)) {
        logf_("[%ld] %s %s.%s  FOCUS %d unresolved\n", n, tag, obj_name, fn_name, idx);
        return;
    }
    char title[FOCUS_MAX_LABEL], detail[FOCUS_MAX_DETAIL];
    char say[FOCUS_MAX_LABEL * 2 + FOCUS_MAX_DETAIL];
    ULONGLONG t_at, d_at;
    if (!focus_take_title(object, title, sizeof title, &t_at) ||
        now - t_at > TITLE_FRESH_MS)
        title[0] = 0;
    if (!focus_detail(object, detail, sizeof detail, &d_at) ||
        now - d_at > LIST_WINDOW_MS)
        detail[0] = 0;
    // A redraw says nothing -- unless it carries a panel just sent, which
    // is news (a facility entered: its submenu rides on this focus).
    if (!detail[0] && object == g_focus_obj && idx == g_focus_idx &&
        now - g_ui_key_at > REDRAW_QUIET_MS) {
        logf_("[%ld] %s %s.%s  FOCUS %d (redraw, unchanged)\n",
              n, tag, obj_name, fn_name, idx);
        return;
    }
    focus_compose(title, label, detail, say, sizeof say);
    // Used once: a description sent after the *previous* move could
    // otherwise ride on the next one when the keys come quickly.
    if (detail[0]) focus_set_detail(object, "");
    g_focus_obj = object;
    g_focus_idx = idx;
    g_focus_at = now;
    g_focus_had_panel = detail[0] != 0;
    logf_("[%ld] %s %s.%s  FOCUS %d -> \"%s\"\n", n, tag, obj_name, fn_name, idx, say);
    speech_cancel_pending();
    // A panel makes this long, so the next move must cut it off rather than
    // queue behind it.
    if (g_speak && !muted()) {
        if (detail[0]) speech_say_now(say);
        else speech_say(say);
    }
}

// A panel describing the item under the cursor, sent on a call of its own
// with one string: a facility submenu's AS_SetHelpText, UISoldierSummary's
// AS_SetDescription. Both used to go through the lone-line path, which takes
// one string for a one-item list and clears the list it belongs beside: the
// soldier summary's first move wiped Abilities, Loadout, Customize and
// Dismiss, and every later move said "unresolved" or read a description.
//
// Kept for the next move to say, and said now when it follows the move just
// announced. On a screen that has no list (UIProgressDialogue and
// UISaveExplanationScreen share the name) it is the content, and is said on
// its own as before.
static void panel_note(LONG n, const char* tag, const char* obj_name,
                       const char* fn_name, void* object, const char* text)
{
    focus_set_detail(object, text);
    logf_("[%ld] %s %s.%s  PANEL \"%s\"\n", n, tag, obj_name, fn_name, text);
    if (!*text || !g_speak || muted()) return;
    if (object == g_focus_obj && !g_focus_had_panel &&
        GetTickCount64() - g_focus_at < LIST_WINDOW_MS) {
        g_focus_had_panel = 1;
        focus_set_detail(object, "");
        speech_say(text);
    } else if (focus_count(object) == 0) {
        speech_say_after(text, SETTLE_MS);
    }
}

// The index as a string. Much of the strategy layer sends the selection that
// way -- the Id is text on the Flash side:
//
//     UIStrategyHUD_FacilityMenu.AS_SetFocus(string Id)
//     UIBuildItem / UIFoundry.AS_SetFocus(string Id)
//     RealizeSelected: Invoke("setFocus", [ASValue string])  on the facility
//         submenus, UIChooseTech, UIChooseFacility, the mission list
//
// One string and no number looks exactly like a screen publishing a
// one-item list, which cleared the list it pointed into: the first move
// along the facility menu wiped the five facility names. Taken as an index
// only when every string in the frame is the same integer (the parameter,
// and its copy in the ASValue array), so a real label is never mistaken.
static int string_index(const Payload* p, int* out)
{
    if (p->nnumbers || !p->nstrings) return 0;
    for (int i = 0; i < p->nstrings; i++) {
        const char* s = p->strings[i];
        if (strcmp(s, p->strings[0]) != 0) return 0;
        const char* d = (*s == '-') ? s + 1 : s;
        if (!*d) return 0;
        for (; *d; d++) if (*d < '0' || *d > '9') return 0;
    }
    *out = atoi(p->strings[0]);
    return 1;
}

// Remembers which (object, function) last spoke, so that a screen publishing
// one row per call can be told apart from a genuine announcement.
static void*     g_last_obj;
static char      g_last_fn[128];
static ULONGLONG g_last_at;


// Which call the capture under way belongs to, for the fault report. Not
// cleared on the way out: an exception filter runs before the unwind, but the
// handler that logs runs after it, and it needs this still in place.
static __declspec(thread) char tls_where[256];

static void unit_flag_drew(void* flag, const char* fn, const Payload* p);
static void combat_message(LONG n, void* stack, const Payload* p);
static void announce(const char* text);
static void announce_as(int setting, const char* text);
static void soldier_stats_note(LONG n, const Payload* p);
static int weapon_note(LONG n, const char* obj, const char* fn, const Payload* p);
static void soldier_selected(void* flag);
static void shot_target_now(void* stack);

// Aiming a cursor-moving ability (a rocket, a grenade) rather than choosing a
// move. The game's aim point is the battle cursor's feet
// (ActiveUnit_Firing_WithMoveCharacteristics.PostProcessCheckGameLogic ->
// SetTargetLoc(CURSOR.GetCursorFeetLocation())), and Mouse_CheckForFreeAim
// places the cursor through CursorSetLocation, the same chain movement uses.
// So navigation drives the aim the same way it drives a move, with three
// differences: it starts from the aim rather than the soldier; the tile goes
// in at ProcessChainedDistance, the range leash, rather than after it, so the
// game still clamps the aim to the ability's range; and a step is announced
// when the aim lands, since no path is ever built to judge it by.
static int       g_nav_aim;
static float     g_aim_floor;           // the floor the aim stands on (aim_floor)
static int soldier_aiming(void);
static void nav_stop(const char* why);

// The aiming reticle's last message, and when it was said. See capture_body.
#define RETICLE_REPEAT_MS 3000
static char      g_reticle_said[128];
static ULONGLONG g_reticle_said_at;

static void strip_note(void* strip, const char* fn, const Payload* p);

// A call's string and bool parameters, by position. The payload cannot be
// used for this: it drops empty strings and repeats every argument in the
// ASValue array, so SetShotInfo("", "72%", "Chance to Hit:", "72%", ...)
// could not be told from one with no hit chance. Here an empty or unreadable
// string is kept as "" in its place.
#define FRAME_ARGS     12
#define FRAME_ARG_TEXT 1024
typedef struct {
    char s[FRAME_ARGS][FRAME_ARG_TEXT];
    int  ns;
    int  b[FRAME_ARGS];
    int  nb;
} FrameArgs;

static void frame_args(void* node, uint8_t* locals, FrameArgs* a)
{
    a->ns = a->nb = 0;
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) break;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) && off < 0x1000) {
            PropKind kind = props_kind(prop);
            if (kind == PROP_BOOL) {
                int b = 0;
                if (a->nb < FRAME_ARGS && props_read_bool(prop, locals, &b)) a->b[a->nb++] = b;
            } else if (kind == PROP_UNKNOWN && a->ns < FRAME_ARGS) {
                char* s = a->s[a->ns++];
                if (read_fstring((const FString*)(locals + off), s, FRAME_ARG_TEXT))
                    strip_markup(s);
                else
                    s[0] = 0;
            }
        }
        prop = next;
    }
}

// The call's `index`th string parameter, whole (frame_args keeps 1024
// characters). Markup is stripped. Empty when there is no such parameter.
static void frame_string(void* node, uint8_t* locals, int index, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    int at = 0;
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) && off < 0x1000 &&
            props_kind(prop) == PROP_UNKNOWN) {
            if (at++ == index) {
                if (read_fstring((const FString*)(locals + off), out, out_sz)) strip_markup(out);
                else out[0] = 0;
                return;
            }
        }
        prop = next;
    }
}

// A string local or parameter of the call, by name, as it is: no markup
// stripped, line breaks kept. Returns 0 when there is none.
static int frame_local_raw(void* node, uint8_t* locals, const char* name, char* out,
                           size_t out_sz)
{
    out[0] = 0;
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return 0;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        if (!(flags & CPF_RETURNPARM) && off < 0x1000 && props_kind(prop) == PROP_UNKNOWN) {
            char pname[64];
            object_name(prop, pname, sizeof pname);
            if (strcmp(pname, name) == 0)
                return read_fstring((const FString*)(locals + off), out, out_sz);
        }
        prop = next;
    }
    return 0;
}

// A float parameter of the call, by name. The payload keeps int parameters
// and ASValue numbers only, so UIBuildFacilities' float xloc and yloc never
// reached it. Returns 0 when there is none.
static int frame_float(void* node, uint8_t* locals, const char* name, float* out)
{
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return 0;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        if ((flags & CPF_PARM) && off < 0x1000 && props_kind(prop) == PROP_FLOAT) {
            char pname[64];
            object_name(prop, pname, sizeof pname);
            if (strcmp(pname, name) == 0) {
                *out = *(const float*)(locals + off);
                return 1;
            }
        }
        prop = next;
    }
    return 0;
}

// A string parameter by name with its line breaks read as stops, the way
// strip_markup reads <br>: "+§100 per month\nNo satellites available" would
// otherwise run together. Markup stripped.
static void frame_lines(void* node, uint8_t* locals, const char* name, char* out,
                        size_t out_sz)
{
    static char raw[FRAME_ARG_TEXT];
    out[0] = 0;
    if (!frame_local_raw(node, locals, name, raw, sizeof raw)) return;
    size_t w = 0;
    for (const char* r = raw; *r && w + 5 < out_sz; r++) {
        if (*r == '\n') { memcpy(out + w, "<br>", 4); w += 4; }
        else out[w++] = *r;
    }
    out[w] = 0;
    strip_markup(out);
}

static void info_note(LONG n, void* object, const char* obj_name, const char* fn_name,
                      void* node, uint8_t* locals);
static ULONGLONG g_info_due;            // when to say the summary, 0 for not yet
static void info_settle(void);

// ---- the mission's objectives (mission.h) -----------------------------------
//
// Each call is read by position (frame_args), except the sorted list, which is
// an array of strings and comes through the payload. What changed is said
// MISSION_SETTLE_MS after the last call of a burst, queued and kept.
#define MISSION_SETTLE_MS 500
static ULONGLONG g_mission_due;
static void*     g_mission_panel;       // the UITacticalHUD_ObjectivesList

// Whether the list is on screen: 1, 0, or -1 when that cannot be asked.
//
// The game fills the list on every mission and draws it on few. OnInit hides
// it unless the mission is eMission_Special (11), and only a script's
// SeqAct_ToggleAllMissionObjectives shows it elsewhere -- so on an ordinary
// mission it holds whatever the script put there at the start, never kept up
// to date because nobody sees it. The first run read exactly that, a list the
// player took for stale (2026-09-22). So what is said follows the screen:
// UI_FxsPanel.IsVisible, a native asked through its vtable slot.
static int mission_visible(void)
{
    void* panel = g_mission_panel;
    if (!panel) return -1;
    // Gone with the mission it belonged to: what it held is not on any screen,
    // and must not be read into the next mission, whose list comes later.
    if (!unit_is_live(panel)) {
        logf_("mission: the list's panel is gone -- forgetting its objectives\n");
        g_mission_panel = NULL;
        mission_reset();
        return 0;
    }
    UnitTestFn visible = (UnitTestFn)tile_vfn(panel, g_panel_slot_visible);
    return visible ? visible(panel, NULL) != 0 : -1;
}

// Visibility polled at most this often, and what it was last.
#define MISSION_VIS_MS 250
static ULONGLONG g_mission_vis_at;
static int       g_mission_vis = -1;

static void mission_note(LONG n, void* object, const char* fn_name, void* node,
                         uint8_t* locals, const Payload* p)
{
    // A new HUD is a new mission; the old one's list is not this one's.
    if (g_mission_panel && object != g_mission_panel) {
        logf_("[%ld] mission: a new list panel -- starting afresh\n", n);
        mission_reset();
        g_mission_vis = -1;
    }
    g_mission_panel = object;
    const char* fn = strncmp(fn_name, "AS_", 3) == 0 ? fn_name + 3 : fn_name;
    static FrameArgs a;     // 12 KB: not on the game's stack
    frame_args(node, locals, &a);
    const char* id = a.ns > 0 ? a.s[0] : "";
    if (strcmp(fn, "AddObjective") == 0) {
        mission_add(id, a.ns > 1 ? a.s[1] : "", a.ns > 2 ? a.s[2] : "", a.nb ? a.b[0] : 1);
        logf_("[%ld] mission: add %s \"%s\" \"%s\"%s\n", n, id, a.ns > 1 ? a.s[1] : "",
              a.ns > 2 ? a.s[2] : "", a.nb && !a.b[0] ? " (hint)" : "");
    } else if (strcmp(fn, "CompleteObjective") == 0) {
        mission_complete(id);
        logf_("[%ld] mission: complete %s\n", n, id);
    } else if (strcmp(fn, "FailObjective") == 0) {
        mission_fail(id);
        logf_("[%ld] mission: fail %s\n", n, id);
    } else if (strcmp(fn, "RemoveObjective") == 0) {
        mission_remove(id);
        logf_("[%ld] mission: remove %s\n", n, id);
    } else if (strcmp(fn, "RemoveAllObjectives") == 0) {
        mission_clear();
        logf_("[%ld] mission: clear\n", n);
    } else if (strcmp(fn, "SetSortedList") == 0) {
        const char* ids[MISSION_MAX];
        int k = 0;
        char seen[256] = "";
        for (int i = 0; i < p->nstrings && k < MISSION_MAX; i++) {
            ids[k++] = p->strings[i];
            size_t used = strlen(seen);
            _snprintf_s(seen + used, sizeof seen - used, _TRUNCATE, "%s%s", used ? " " : "",
                        p->strings[i]);
        }
        mission_order(ids, k);
        logf_("[%ld] mission: order %s\n", n, seen);
        return;     // the order alone is not a change worth waiting on
    } else {
        return;
    }
    g_mission_due = GetTickCount64() + MISSION_SETTLE_MS;
}

static void mission_poll(void)
{
    ULONGLONG now = GetTickCount64();
    char say[MISSION_TEXT];

    // Shown mid-mission by a script: what it shows is read then.
    if (g_mission_panel && now - g_mission_vis_at >= MISSION_VIS_MS) {
        g_mission_vis_at = now;
        int vis = mission_visible();
        if (vis != g_mission_vis) {
            logf_("mission: the list is %s\n",
                  vis > 0 ? "shown" : vis == 0 ? "hidden" : "of unknown visibility");
            if (vis > 0 && g_mission_vis == 0 && !g_mission_due) {
                mission_list(say, sizeof say);
                logf_("mission: shown -> \"%s\"\n", say);
                announce_as(SET_OBJECTIVES, say);
            }
            g_mission_vis = vis;
        }
    }

    if (!g_mission_due || now < g_mission_due) return;
    g_mission_due = 0;
    // The changes are taken either way, so a list that is shown later is
    // compared with what it held, not with the start of the mission.
    mission_changes(say, sizeof say);
    if (!say[0]) return;
    int vis = mission_visible();
    logf_("mission: -> \"%s\"%s\n", say, vis == 0 ? "  (hidden -- not said)" : "");
    if (vis != 0) announce_as(SET_OBJECTIVES, say);
}

// ---- Mission Control alerts ---------------------------------------------------
//
// UIMissionControl_AlertBase.OnInit fills a simple alert (title, text, one
// button) and then Invoke("AlertFullyLoaded"); the OnInit the log shows is
// that. The alerts with several buttons -- research, engineering and the
// Foundry finishing, a facility built, a UFO, terror, the alien base, EXALT
// -- are UIMissionControl_AlertWithMultipleButtons, whose OnInit calls
// super.OnInit() *first* and only then UpdateData(): at AlertFullyLoaded they
// are still empty. Said then, the research alert of 2026-09-24 was
// ALERT "". Such an alert is held (g_alert_due) and said once its burst is
// over: at the first call on anything else, or the first key.
//
// ScienceAlert.UpdateData (EW, and EU the same) sends
//     AS_SetTitle(name), AS_SetSubTitle("RESEARCH COMPLETE"), AS_SetText(text),
//     AS_SetButtonData(i, label, bool disabled) per reply,
//     AS_SetImage(path)
// and the engineering and Foundry ones AS_SetRebates(label, lines) as well.
// The image was taken for a screen publishing a one-item list, and replaced
// the buttons with its path.
//
// A facility built (UIMissionControl_FacilityBuiltAlert) sends
// AS_SetFacilityImageLabel("AlienContainment") after its buttons, which did
// the same: "Options: AlienContainment", and moves unresolved. And nothing
// else draws after it -- the first of each facility plays its cinematic --
// so the held alert waited for a key (log of 2026-09-25, 22:45). A held
// alert is now also said once its calls have stopped for ALERT_SETTLE_MS
// (alert_settle, on review_pump's thread; g_alert_lock keeps the two apart).
#define ALERT_SETTLE_MS 250
static CRITICAL_SECTION g_alert_lock;
static void* volatile g_alert_due;      // the alert waiting to be said
static volatile ULONGLONG g_alert_note_at; // its last call
static char  g_alert_title[FOCUS_MAX_LABEL];
static char  g_alert_text[MAX_STR];
static char  g_alert_sub[FOCUS_MAX_LABEL];
static char  g_alert_rebates[MAX_STR];

static void alert_say(LONG n, const char* tag, const char* obj_name, void* object)
{
    static char say[MAX_STR * 2 + FOCUS_MAX_LABEL * 8];
    const char* parts[4] = { g_alert_title, g_alert_sub, g_alert_text, g_alert_rebates };
    focus_join_detail(parts, 4, say, sizeof say);
    g_alert_title[0] = g_alert_text[0] = g_alert_sub[0] = g_alert_rebates[0] = 0;
    int nb = focus_count(object);
    for (int i = 0; i < nb; i++) {
        char label[FOCUS_MAX_LABEL];
        if (!focus_label_at(object, i, label, sizeof label)) continue;
        size_t used = strlen(say);
        const char* lead = !used ? "Options: "
                         : say[used - 1] == '.' ? " Options: " : ". Options: ";
        _snprintf_s(say + used, sizeof say - used, _TRUNCATE, "%s%s",
                    i == 0 ? lead : ", ", label);
    }
    logf_("[%ld] %s %s  ALERT \"%s\"\n", n, tag, obj_name, say);
    speech_cancel_pending();
    announce(say);
    // The first button is the selected one; a move off it and back must
    // still be said.
    g_focus_obj = object;
    g_focus_idx = 0;
}

// Says a held alert, when `object` is not it: its burst is over.
static void alert_flush(LONG n, const char* tag, void* object)
{
    if (!g_alert_due || object == g_alert_due) return;
    EnterCriticalSection(&g_alert_lock);
    void* due = g_alert_due;
    g_alert_due = NULL;
    if (due && due != object) {
        char name[128] = "?";
        object_name(due, name, sizeof name);
        alert_say(n, tag, name, due);
    }
    LeaveCriticalSection(&g_alert_lock);
}

// Says a held alert whose calls have stopped. From review_pump.
static void alert_settle(void)
{
    if (!g_alert_due || GetTickCount64() - g_alert_note_at < ALERT_SETTLE_MS) return;
    EnterCriticalSection(&g_alert_lock);
    void* due = g_alert_due;
    if (due && GetTickCount64() - g_alert_note_at >= ALERT_SETTLE_MS) {
        g_alert_due = NULL;
        char name[128] = "?";
        object_name(due, name, sizeof name);
        alert_say(g_calls, "Settled     ", name, due);
    }
    LeaveCriticalSection(&g_alert_lock);
}

static int alert_note_locked(LONG n, const char* tag, const char* obj_name, const char* fn_name,
                             void* object, void* node, uint8_t* locals, const Payload* p);

// One call on an alert. Returns 1 when it has been dealt with.
static int alert_note(LONG n, const char* tag, const char* obj_name, const char* fn_name,
                      void* object, void* node, uint8_t* locals, const Payload* p)
{
    EnterCriticalSection(&g_alert_lock);
    int done = alert_note_locked(n, tag, obj_name, fn_name, object, node, locals, p);
    if (object == g_alert_due) g_alert_note_at = GetTickCount64();
    LeaveCriticalSection(&g_alert_lock);
    return done;
}

static int alert_note_locked(LONG n, const char* tag, const char* obj_name, const char* fn_name,
                             void* object, void* node, uint8_t* locals, const Payload* p)
{
    if (strcmp(fn_name, "AS_SetTitle") == 0) {
        frame_string(node, locals, 0, g_alert_title, sizeof g_alert_title);
        return 1;
    }
    if (strcmp(fn_name, "AS_SetText") == 0) {
        frame_string(node, locals, 0, g_alert_text, sizeof g_alert_text);
        return 1;
    }
    if (strcmp(fn_name, "AS_SetSubTitle") == 0) {
        frame_string(node, locals, 0, g_alert_sub, sizeof g_alert_sub);
        return 1;
    }
    if (strcmp(fn_name, "AS_SetRebates") == 0) {
        static char label[FOCUS_MAX_LABEL], lines[MAX_STR];
        frame_string(node, locals, 0, label, sizeof label);
        frame_string(node, locals, 1, lines, sizeof lines);
        const char* parts[2] = { label, lines };
        focus_join_detail(parts, 2, g_alert_rebates, sizeof g_alert_rebates);
        return 1;
    }
    // A UFO's particulars (UIMissionControl_UFOAlert.UpdateData): each is
    // AS_SetContact / AS_SetLocation / AS_SetSize / AS_SetClass(label,
    // data), and they went nowhere -- the alert said its title and its
    // buttons. Added to the text as "CONTACT: Small, LOCATION: ...".
    if ((strcmp(fn_name, "AS_SetContact") == 0 || strcmp(fn_name, "AS_SetLocation") == 0 ||
         strcmp(fn_name, "AS_SetSize") == 0 || strcmp(fn_name, "AS_SetClass") == 0)) {
        static FrameArgs a;
        frame_args(node, locals, &a);
        const char* label = a.ns > 0 ? a.s[0] : "";
        const char* data = a.ns > 1 ? a.s[1] : "";
        if (!data[0]) return 1;
        size_t ll = strlen(label), used = strlen(g_alert_text);
        _snprintf_s(g_alert_text + used, sizeof g_alert_text - used, _TRUNCATE, "%s%s%s%s",
                    used ? ". " : "", label, !ll ? "" : label[ll - 1] == ':' ? " " : ": ", data);
        return 1;
    }
    if (strcmp(fn_name, "AS_SetImage") == 0 ||
        strcmp(fn_name, "AS_SetFacilityImageLabel") == 0) return 1;
    if (strcmp(fn_name, "AS_SetButtonText") == 0 && p->nstrings) {
        focus_set(object, 0, p->strings[0]);
        return 1;
    }
    // (int index, string label, bool disabled): a reply that cannot be
    // chosen yet plays the bad sound on Enter.
    if (strcmp(fn_name, "AS_SetButtonData") == 0 && p->nnumbers && p->nstrings) {
        char label[FOCUS_MAX_LABEL];
        _snprintf_s(label, sizeof label, _TRUNCATE, "%s%s", p->strings[0],
                    p->nbools && p->bools[0] ? ", unavailable" : "");
        focus_set(object, (int)p->numbers[0], label);
        return 1;
    }
    if (strcmp(fn_name, "OnInit") == 0) {
        // Anything a previous alert at this address left is not this one's.
        g_alert_due = NULL;
        if (g_alert_title[0] || g_alert_text[0]) {
            alert_say(n, tag, obj_name, object);
        } else {
            focus_begin(object);
            g_alert_due = object;
            logf_("[%ld] %s %s.%s  ALERT held until its data is in\n", n, tag, obj_name,
                  fn_name);
        }
        return 1;
    }
    return 0;
}

// EW's unlock notice, UISpecialUnlockDialogue: "NEW GENE MOD AVAILABLE" or
// "NEW MEC AVAILABLE", raised by research (Meld Recombination unlocks three
// gene mods and a MEC at once). It said nothing -- no handler, and the
// general path found no list in it. Realize draws the last queued unlock:
//     AS_SetTitle(title)
//     AS_SetRequirement(GetHTMLColoredText("REQUIRES GENETICS LAB",
//                                          has the lab ? 0 : 3))
//     AS_SetGeneModImage(path), AS_SetGeneModData(name, summary, icon)
//  or AS_SetMechImage(path), AS_SetMechData(mec, weapon0, summary0, icon0,
//                                           weapon1, summary1, icon1, "OR")
//     AS_SetButtonData("ACCEPT", icon)
// -- the button last, so that is when it is said. Enter, Space or Escape
// (OnUnrealCommand: 300, 511, 513, 301, 510, 405) drops that one and Realize
// draws the next, which is said the same way. While a dialogue box is up
// Realize only hides, and it runs again when the box closes.
static char g_unlock_title[FOCUS_MAX_LABEL];
static char g_unlock_req[FOCUS_MAX_LABEL];
static char g_unlock_body[MAX_STR];

static void unlock_note(LONG n, const char* tag, const char* obj_name, const char* fn_name,
                        void* node, uint8_t* locals, const Payload* p)
{
    static FrameArgs a;
    if (strcmp(fn_name, "AS_SetTitle") == 0) {
        frame_string(node, locals, 0, g_unlock_title, sizeof g_unlock_title);
    } else if (strcmp(fn_name, "AS_SetRequirement") == 0) {
        // Red when the facility is not built yet; the plain text says so
        // only by its colour.
        char req[FOCUS_MAX_LABEL];
        frame_string(node, locals, 0, req, sizeof req);
        int missing = p->nstrings && p->hues[0] == HUE_BAD;
        _snprintf_s(g_unlock_req, sizeof g_unlock_req, _TRUNCATE, "%s%s", req,
                    missing && req[0] ? ", not built yet" : "");
    } else if (strcmp(fn_name, "AS_SetGeneModData") == 0) {
        frame_args(node, locals, &a);
        const char* parts[2] = { a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "" };
        focus_join_detail(parts, 2, g_unlock_body, sizeof g_unlock_body);
    } else if (strcmp(fn_name, "AS_SetMechData") == 0) {
        frame_args(node, locals, &a);
        const char* g = a.ns > 0 ? a.s[0] : "";
        char w0[MAX_STR], w1[MAX_STR];
        const char* p0[2] = { a.ns > 1 ? a.s[1] : "", a.ns > 2 ? a.s[2] : "" };
        const char* p1[2] = { a.ns > 4 ? a.s[4] : "", a.ns > 5 ? a.s[5] : "" };
        focus_join_detail(p0, 2, w0, sizeof w0);
        focus_join_detail(p1, 2, w1, sizeof w1);
        const char* parts[4] = { g, w0, a.ns > 7 ? a.s[7] : "", w1 };
        focus_join_detail(parts, 4, g_unlock_body, sizeof g_unlock_body);
    } else if (strcmp(fn_name, "AS_SetButtonData") == 0) {
        char button[FOCUS_MAX_LABEL], key[FOCUS_MAX_LABEL + 16];
        frame_string(node, locals, 0, button, sizeof button);
        _snprintf_s(key, sizeof key, _TRUNCATE, "%s%s", button[0] ? "Enter: " : "", button);
        static char say[MAX_STR + FOCUS_MAX_LABEL * 4];
        const char* parts[4] = { g_unlock_title, g_unlock_body, g_unlock_req, key };
        focus_join_detail(parts, 4, say, sizeof say);
        g_unlock_title[0] = g_unlock_req[0] = g_unlock_body[0] = 0;
        logf_("[%ld] %s %s.%s  UNLOCK \"%s\"\n", n, tag, obj_name, fn_name, say);
        speech_cancel_pending();
        announce(say);
    }
    // The images, and anything else, are not for saying.
}

static void capture_body(const char* tag, LONG n, void* stack)
{
    if (!readable(stack, 0x20)) {
        logf_("[%ld] %s unreadable frame %p\n", n, tag, stack);
        return;
    }

    void* node      = *(void**)((uint8_t*)stack + FFRAME_NODE);
    void* object    = *(void**)((uint8_t*)stack + FFRAME_OBJECT);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);

    char fn_name[128] = "?", obj_name[128] = "?";
    object_name(node, fn_name, sizeof fn_name);
    object_name(object, obj_name, sizeof obj_name);
    _snprintf_s(tls_where, sizeof tls_where, _TRUNCATE, "%s.%s", obj_name, fn_name);

    // Deferred until a real frame is in hand: the probe needs a UStruct whose
    // children are known to be properties, and a called function is exactly
    // that. Reported once so the log says whether scalars are being read.
    if (!props_ready()) {
        char why[160];
        if (props_init(node, why, sizeof why)) logf_("props: %s\n", why);
    } else if (!props_mask_offset() && props_learn_mask(node)) {
        logf_("props: BitMask +0x%X, learned from %s\n", props_mask_offset(), fn_name);
    }

    // Payload is ~64KB. Putting that on the game's own thread stack, inside
    // a script VM that is already deep, is asking for trouble; it lives in
    // thread-local storage instead.
    static __declspec(thread) Payload tls_payload;

    Payload* p = &tls_payload;
    p->nstrings = 0;
    p->nnumbers = 0;
    p->nbools   = 0;
    p->nabools  = 0;

    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);

    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) break;

        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);

        // Locals count as much as parameters: UIFinalShell.SetText takes no
        // arguments and builds the whole main menu out of locals.
        if (locals && (flags & CPF_RETURNPARM) == 0 && off < 0x1000) {
            const void* slot = locals + off;
            PropKind kind = props_kind(prop);

            // A plain scalar is taken only from a *parameter*. Locals would
            // otherwise hand over loop counters: UIFinalShell.SetText builds
            // the main menu from locals, and its counter would be read as a
            // slot number, joining all five entries into one. Every setter
            // that matters declares its index as an argument, so the
            // restriction costs nothing.
            if ((flags & CPF_PARM) && (kind == PROP_INT || kind == PROP_BYTE)) {
                if (readable(slot, sizeof(int32_t)))
                    payload_add_number(p, (float)*(const int32_t*)slot);
            } else if ((flags & CPF_PARM) && kind == PROP_BOOL) {
                int b;
                if (props_read_bool(prop, locals, &b)) payload_add_bool(p, b);
            } else {
                char val[MAX_STR];
                if (read_fstring((const FString*)slot, val, sizeof val)) {
                    int hue = font_hue(val);
                    strip_markup(val);
                    if (*val) {
                        payload_add_string(p, val);
                        p->hues[p->nstrings - 1] = hue;
                    }
                } else {
                    read_array((const FArray*)slot, p);
                }
            }
        }
        prop = next;
    }

    // A multi-button alert held for its data is said once anything else
    // draws. See alert_note.
    alert_flush(n, tag, object);

    // EW's Meld counters and the arrow at a canister are redrawn every frame
    // (UISpecialMissionHUD_MeldStats subscribes UpdatePanel to the UI
    // update): 9,110 of the Gateway run's 10,807 lines. Nothing here reads
    // them -- the scanner's Meld category asks the canisters themselves --
    // so a call is logged only when it is not one of the last few sent.
    if (strncmp(obj_name, "UISpecialMissionHUD_TurnCounter", 31) == 0 ||
        strncmp(obj_name, "UISpecialMissionHUD_Arrows", 26) == 0) {
        static struct { void* obj; char fn[48]; char last[160]; } s_seen[24];
        static int s_next;
        char now[160];
        size_t used = 0;
        now[0] = 0;
        for (int i = 0; i < p->nstrings && used + 1 < sizeof now; i++) {
            int k = _snprintf_s(now + used, sizeof now - used, _TRUNCATE, "%s|", p->strings[i]);
            used = k < 0 ? sizeof now - 1 : used + (size_t)k;
        }
        for (int i = 0; i < p->nnumbers && used + 1 < sizeof now; i++) {
            int k = _snprintf_s(now + used, sizeof now - used, _TRUNCATE, "%g|", p->numbers[i]);
            used = k < 0 ? sizeof now - 1 : used + (size_t)k;
        }
        // Keyed on the whole call: two arrows alternate on one object, and
        // either would count as a change every frame against the other.
        for (int i = 0; i < 24; i++)
            if (s_seen[i].obj == object && strcmp(s_seen[i].fn, fn_name) == 0 &&
                strcmp(s_seen[i].last, now) == 0)
                return;
        int slot = s_next;
        s_next = (s_next + 1) % 24;
        s_seen[slot].obj = object;
        strncpy_s(s_seen[slot].fn, sizeof s_seen[slot].fn, fn_name, _TRUNCATE);
        strncpy_s(s_seen[slot].last, sizeof s_seen[slot].last, now, _TRUNCATE);
    }

    // Which layer the player is in, for keys read off the game's thread:
    // Delete means the selected soldier in a mission and the base's status
    // at the base, and the key thread cannot ask the game which it is.
    if (strncmp(obj_name, "UIStrategyHUD", 13) == 0 ||
        strncmp(obj_name, "UIStrategyComponent", 19) == 0)
        g_seen_strategy_at = GetTickCount64();
    else if (strncmp(obj_name, "UITacticalHUD", 13) == 0)
        g_seen_tactical_at = GetTickCount64();

    // The base's status panels. See hq.h. Kept for Delete; they were never
    // said, and a lone resource line risked being spoken as an announcement.
    // UIStrategyHUD_0 itself, not its panels (UIStrategyHUD_FacilityMenu_0
    // and the rest share the prefix): the name's next character is a digit.
    if (strncmp(obj_name, "UIStrategyHUD_", 14) == 0 &&
        obj_name[14] >= '0' && obj_name[14] <= '9') {
        if (strcmp(fn_name, "ClearResources") == 0) { hq_status_resources_clear(); return; }
        if (strcmp(fn_name, "AS_SetHumanResources") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            hq_status_human(a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "");
            logf_("[%ld] %s %s.%s  STAFF \"%s\" \"%s\"\n", n, tag, obj_name, fn_name,
                  a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_AddResource") == 0 && p->nstrings) {
            hq_status_resource(p->strings[0]);
            return;
        }
    }
    if (strncmp(obj_name, "UIStrategyComponent_Clock", 25) == 0 &&
        strcmp(fn_name, "AS_SetDateTime") == 0) {
        static FrameArgs a;
        frame_args(node, locals, &a);
        hq_status_date(a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "",
                       a.ns > 2 ? a.s[2] : "", a.ns > 3 ? a.s[3] : "");
        // A tick as each day passes (hq_day_passed), so time moving on the
        // geoscape -- scanning above all -- is heard without asking.
        if (hq_day_passed(a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "", GetTickCount64())) {
            logf_("[%ld] %s %s.%s  DAY \"%s %s\"%s\n", n, tag, obj_name, fn_name,
                  a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "",
                  settings_get(SET_DAYS) ? "" : " (tick off)");
            if (settings_get(SET_DAYS)) audio_cue(HEART_TICK, 1);
        }
        return;
    }
    if (strncmp(obj_name, "UIStrategyComponent_EventList", 29) == 0) {
        if (strcmp(fn_name, "UpdateData") == 0) { hq_status_events_clear(object); return; }
        if (strcmp(fn_name, "AS_AddEvent") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            hq_status_event(object, a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "",
                            a.ns > 2 ? a.s[2] : "");
            // Logged, so a run shows which panel drew what and when: the
            // status reads the list drawn last.
            logf_("[%ld] %s %s.%s  EVENT \"%s, %s %s\"\n", n, tag, obj_name, fn_name,
                  a.ns > 0 ? a.s[0] : "", a.ns > 2 ? a.s[2] : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
    }

    // ---- Engineering ---------------------------------------------------------
    //
    // Its submenu is a facility submenu like any other. What it adds is read
    // here, from the game's full workflow (UIStrategyHUD_FSM_Engineering,
    // UIBuildItem, UIManufacturing, UIStrategyHUD_BuildQueue, and
    // XGEngineeringUI / XGManufacturingUI behind them). See hq.h.
    if (strncmp(obj_name, "UIStrategyHUD_FSM_Engineering", 29) == 0 ||
        strncmp(obj_name, "UIBuildItem", 11) == 0 ||
        strncmp(obj_name, "UIManufacturing", 15) == 0 ||
        strncmp(obj_name, "UIFoundry", 9) == 0)
        g_eng_at = GetTickCount64();

    // The interception (UIInterceptionEngagement): a fight played back in
    // real time from a script the game has already rolled. Nothing in it was
    // said -- ship 0 is the UFO, 1 the interceptor -- and everything it shows
    // comes through these calls:
    //     AS_SetHP(ship, hp, bool initialization, weaponID)
    //         -- initialization once per ship in OnInit, the full hull; then
    //            once per hit as the damage lands (Playback)
    //     AS_SetAimButton(label, state) / AS_SetDodgeButton(label, state) /
    //     AS_SetTrackButton(label, trackingText, state)
    //         -- 0 available, 2 none left, 3 not researched
    //     AS_BeginIntroSequence() -- the link comes up; the fight follows
    //     AS_SetEnemyEscapeTimer(tenths) -- "CONTACT LOSS IN", each tenth
    //     AS_DisplayEffectEvent(type, description, bool enabled, data)
    //         -- 0 aim, 1 dodge, 2 track, on when used and off when spent
    //     AS_SetAbortLabel("ABORTING...") / ("ABORTED")
    //     AS_ShowResults(report, battleResult, leaveLabel)
    // and AS_AttackEvent / AS_MovementEvent for every shot, which the hits
    // already cover. The keys are input.c's 1 to 4.
    if (strncmp(obj_name, "UIInterceptionEngagement", 24) == 0) {
        static int hp[2], hp_max[2], ability[3] = { 3, 3, 3 }, secs_said = -1;
        static char title[128];
        static const char* const names[3] = { "Aim", "Dodge", "Track" };
        char say[FRAME_ARG_TEXT + 256];
        say[0] = 0;
        int now = 1;                    // said at once, cutting what was said
        if (strcmp(fn_name, "AS_SetResultsTitleLabels") == 0) {
            frame_string(node, locals, 0, title, sizeof title);
            hp[0] = hp[1] = hp_max[0] = hp_max[1] = 0;
            ability[0] = ability[1] = ability[2] = 3;
            secs_said = -1;
        } else if (strcmp(fn_name, "AS_SetHP") == 0 && p->nnumbers >= 2) {
            int ship = (int)p->numbers[0], v = (int)p->numbers[1];
            if (ship < 0 || ship > 1) return;
            if (p->nbools && p->bools[0]) {
                hp[ship] = hp_max[ship] = v;
            } else if (v != hp[ship]) {
                hp[ship] = v;
                int pct = hp_max[ship] > 0 ? (v * 100 + hp_max[ship] - 1) / hp_max[ship] : 0;
                if (v <= 0)
                    strcpy_s(say, sizeof say, ship == 0 ? "UFO down." : "Interceptor shot down.");
                else if (ship == 0)
                    _snprintf_s(say, sizeof say, _TRUNCATE, "UFO hit, %d%%.", pct);
                else
                    _snprintf_s(say, sizeof say, _TRUNCATE, "We're hit, %d%%.", pct);
            }
        } else if ((strcmp(fn_name, "AS_SetAimButton") == 0 ||
                    strcmp(fn_name, "AS_SetDodgeButton") == 0 ||
                    strcmp(fn_name, "AS_SetTrackButton") == 0) && p->nnumbers) {
            int k = fn_name[6] == 'A' ? 0 : fn_name[6] == 'D' ? 1 : 2;
            ability[k] = (int)p->numbers[0];
        } else if (strcmp(fn_name, "AS_BeginIntroSequence") == 0) {
            size_t w = 0;
            w += _snprintf_s(say, sizeof say, _TRUNCATE, "%s. UFO %d, interceptor %d.",
                             title[0] ? title : "Interception", hp_max[0], hp_max[1]);
            int any = 0;
            for (int k = 0; k < 3; k++) {
                if (ability[k] != 0) continue;
                w += _snprintf_s(say + w, sizeof say - w, _TRUNCATE, "%s%d %s",
                                 any ? ", " : " ", k + 1, names[k]);
                any = 1;
            }
            _snprintf_s(say + w, sizeof say - w, _TRUNCATE, "%s4 Abort.", any ? ", " : " ");
        } else if (strcmp(fn_name, "AS_SetEnemyEscapeTimer") == 0 && p->nnumbers) {
            // Whole seconds, rounded up: 4.3 s left is "5" until it is 4.
            int secs = ((int)p->numbers[0] + 9) / 10;
            static const int marks[] = { 10, 5, 3, 2, 1 };
            if (secs_said < 0) {
                _snprintf_s(say, sizeof say, _TRUNCATE, "Contact loss in %d seconds.", secs);
                now = 0;
            } else if (secs > secs_said) {
                secs_said = secs;       // tracking ended and the clock was reset
            } else {
                for (int i = 0; i < 5; i++)
                    if (secs <= marks[i] && secs_said > marks[i]) {
                        _snprintf_s(say, sizeof say, _TRUNCATE, "%d.", secs);
                        break;
                    }
            }
            if (secs_said < 0 || say[0]) secs_said = secs;
        } else if (strcmp(fn_name, "AS_DisplayEffectEvent") == 0 && p->nnumbers) {
            int k = (int)p->numbers[0];
            if (p->nbools && p->bools[0]) {
                frame_lines(node, locals, "effectDescription", say, sizeof say);
            } else if (k >= 0 && k < 3) {
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s over.", names[k]);
            }
        } else if (strcmp(fn_name, "AS_SetAbortLabel") == 0) {
            char label[64];
            frame_string(node, locals, 0, label, sizeof label);
            if (_strnicmp(label, "ABORTING", 8) == 0) strcpy_s(say, sizeof say, label);
        } else if (strcmp(fn_name, "AS_ShowResults") == 0) {
            char report[FRAME_ARG_TEXT], leave[64];
            frame_lines(node, locals, "report", report, sizeof report);
            frame_string(node, locals, 1, leave, sizeof leave);
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s", report,
                        leave[0] ? " Enter: " : "", leave, leave[0] ? "." : "");
        } else if (strcmp(fn_name, "AS_AttackEvent") == 0 ||
                   strcmp(fn_name, "AS_MovementEvent") == 0) {
            return;                     // every shot; the hits are said
        }
        if (say[0]) {
            logf_("[%ld] %s %s.%s  INTERCEPT \"%s\"\n", n, tag, obj_name, fn_name, say);
            if (g_speak && !muted()) {
                if (now) { speech_cancel_pending(); speech_say_now(say); }
                else speech_say(say);
            }
        } else if (strcmp(fn_name, "AS_SetEnemyEscapeTimer") != 0) {
            logf_("[%ld] %s %s.%s  INTERCEPT (kept)\n", n, tag, obj_name, fn_name);
        }
        return;
    }

    // Build Facilities: the base's grid. See hq.h. The cards are kept, and
    // the cursor's tile said with the cursor's text. OnInit and GoToView(0)
    // both draw the cursor on the way in, so a second drawing of the same
    // tile and text straight after the first is not said again.
    if (strncmp(obj_name, "UIBuildFacilities", 17) == 0) {
        static FrameArgs a;
        float fx, fy;
        int at = frame_float(node, locals, "xloc", &fx) && frame_float(node, locals, "yloc", &fy);
        if (strcmp(fn_name, "AS_UpdateFacilityCard") == 0 && at) {
            frame_args(node, locals, &a);
            hq_base_card((int)fx, (int)fy, a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetCursor") == 0 && at) {
            static char text[FRAME_ARG_TEXT], say[FRAME_ARG_TEXT + 256], said[sizeof say];
            static ULONGLONG said_at;
            frame_lines(node, locals, "DisplayText", text, sizeof text);
            hq_base_cursor((int)fx, (int)fy, text, say, sizeof say);
            ULONGLONG t = GetTickCount64();
            if (strcmp(say, said) == 0 && t - said_at < 500) {
                logf_("[%ld] %s %s.%s  BASE cursor redrawn\n", n, tag, obj_name, fn_name);
                said_at = t;
                return;
            }
            strncpy_s(said, sizeof said, say, _TRUNCATE);
            said_at = t;
            logf_("[%ld] %s %s.%s  BASE cursor -> \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The build queue, drawn beside Engineering and the Foundry. UpdateData
    // clears it (Invoke "clear", the only call in that frame) and adds one
    // order per AS_AddProjectToQueue, with no index; the selection is
    // Invoke("setSelected", [string index]) from RealizeSelected. Left to the
    // general path, the four column headings of AS_SetQueueTitle were a list,
    // every order replaced the one before, the order's state (3 or 4) was
    // taken for its slot, and a lone "NO CURRENT PROJECTS" was said out of
    // nowhere at the base.
    if (strncmp(obj_name, "UIStrategyHUD_BuildQueue", 24) == 0) {
        if (strcmp(fn_name, "UpdateData") == 0) {
            focus_begin(object);
            hq_queue_clear();
            logf_("[%ld] %s %s.%s  QUEUE cleared\n", n, tag, obj_name, fn_name);
            return;
        }
        if (strcmp(fn_name, "AS_AddProjectToQueue") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            char row[FOCUS_MAX_LABEL];
            hq_queue_row(a.ns > 0 ? a.s[0] : "", a.ns > 2 ? a.s[2] : "",
                         a.ns > 3 ? a.s[3] : "", row, sizeof row);
            focus_add(object, row);
            hq_queue_add(row);
            logf_("[%ld] %s %s.%s  QUEUE %d = \"%s\"\n", n, tag, obj_name, fn_name,
                  focus_count(object) - 1, row);
            return;
        }
        if (strcmp(fn_name, "AS_SetQueueTitle") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            hq_queue_title(a.ns > 0 ? a.s[0] : "");
            logf_("[%ld] %s %s.%s  QUEUE title \"%s\"\n", n, tag, obj_name, fn_name,
                  a.ns > 0 ? a.s[0] : "");
            return;
        }
        // What the queue offers, into 0's list. Y (303) on the Engineering
        // submenu starts Review an order when there is one
        // (UIStrategyHUD_FSM_Engineering, case 303), and 6 sends Y there.
        // With a gamepad the game names it with its glyph; in mouse mode it
        // says "CLICK TO EDIT" with none, or "QUEUE LOCKED" while it cannot
        // be edited (an order open, the Foundry, the queue already active).
        // Only the English lock is told apart: elsewhere a locked queue is
        // listed too, and 6 on it does nothing.
        if (strcmp(fn_name, "AS_SetHelp") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            const char* label = a.ns > 0 ? a.s[0] : "";
            const char* icon  = a.ns > 1 ? a.s[1] : "";
            if (!*label || _stricmp(label, "QUEUE LOCKED") == 0)
                help_set(object, 0, "", "", 0);
            else
                help_set(object, 0, *icon ? label : "Review an order", "Icon_Y_TRIANGLE", 0);
            logf_("[%ld] %s %s.%s  QUEUE help \"%s\" on %s\n", n, tag, obj_name, fn_name,
                  label, *icon ? icon : "(no icon)");
            return;
        }
        // Review an order: the queue takes the arrows until Enter opens the
        // order or Escape gives them back. It starts with nothing selected
        // (DeactivateEditing left -1), so the first Down lands on the first
        // order and the first Up on the last.
        if (strcmp(fn_name, "ActivateEditing") == 0) {
            g_queue_editing = 1;
            char say[128];
            _snprintf_s(say, sizeof say, _TRUNCATE, "Review an order, %d in the queue",
                        focus_count(object));
            logf_("[%ld] %s %s.%s  QUEUE reviewing \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strcmp(fn_name, "DeactivateEditing") == 0 || strcmp(fn_name, "OnAccept") == 0) {
            g_queue_editing = 0;
            logf_("[%ld] %s %s.%s  QUEUE reviewing ends\n", n, tag, obj_name, fn_name);
            return;
        }
        // Every redraw selects the first order when there is one; only a
        // move while reviewing is news.
        if (strcmp(fn_name, "RealizeSelected") == 0) {
            int idx;
            if (!g_queue_editing || !string_index(p, &idx) || idx < 0) {
                logf_("[%ld] %s %s.%s  QUEUE selection (not reviewing)\n",
                      n, tag, obj_name, fn_name);
                return;
            }
            focus_announce(n, tag, obj_name, fn_name, object, idx);
            return;
        }
    }

    // The Officer Training School (UIOTS). DrawTable sends AS_Clear, then per
    // upgrade
    //     AS_AddOption(int i, tactic, cost, bool disabled)
    // and selects one; each selection (RealizeSelected) is
    //     AS_SetListSelection(string i)   -- the index as a STRING
    //     AS_SetHelp(why it is locked, or "")
    //     AS_SetPurchaseButtonText        -- mouse mode only
    //     AS_UpdateInfo(image label, description)
    // An upgrade (XGOTSUI.BuildTableItem) is bought -- disabled, cost
    // "PURCHASED" -- locked by a rank not reached -- disabled, the help says
    // which -- or dear: not disabled, only its cost drawn red, and Enter
    // still opens the confirm dialogue before the bad sound. On the general
    // path the list was slots, the moves said nothing and the panel read the
    // image label: "_squadSizeI. Squad size increased to 5 soldiers."
    // Up/Down wrap, Enter buys (a dialogue confirms), Escape leaves; after a
    // purchase the table is drawn again.
    if (strncmp(obj_name, "UIOTS", 5) == 0) {
        #define OTS_ROWS 16
        static char s_title[96], s_help[512];
        static char s_row[OTS_ROWS][FOCUS_MAX_LABEL];
        static int  s_n, s_sel, s_fresh;
        if (strcmp(fn_name, "AS_SetTitles") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            return;
        }
        if (strcmp(fn_name, "AS_Clear") == 0) {
            s_n = 0;
            s_fresh = 1;
            return;
        }
        if (strcmp(fn_name, "AS_AddOption") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= OTS_ROWS) return;
            static FrameArgs a;
            static char raw[FRAME_ARG_TEXT];
            frame_args(node, locals, &a);
            // The cost as drawn: red is XGOTSUI's state 3, not enough money.
            int dear = frame_local_raw(node, locals, "sRequirement", raw, sizeof raw) &&
                       font_hue(raw) == HUE_BAD;
            int disabled = p->nbools && p->bools[0];
            char cost[64];
            size_t w = 0;
            const char* c = a.ns > 1 ? a.s[1] : "";
            for (; *c && w + 1 < sizeof cost; c++) {          // the section sign, C2 A7
                if ((unsigned char)c[0] == 0xC2 && (unsigned char)c[1] == 0xA7) { c++; continue; }
                cost[w++] = *c;
            }
            cost[w] = 0;
            int bought = disabled && cost[0] && !(cost[0] >= '0' && cost[0] <= '9');
            _snprintf_s(s_row[i], FOCUS_MAX_LABEL, _TRUNCATE, "%s, %s%s%s", a.s[0],
                        bought ? "purchased" : cost, !bought && cost[0] ? " credits" : "",
                        bought ? "" : disabled ? ", locked"
                                   : dear ? ", not enough credits" : "");
            if (i >= s_n) s_n = i + 1;
            return;
        }
        if (strcmp(fn_name, "AS_SetListSelection") == 0) {
            char id[16];
            frame_string(node, locals, 0, id, sizeof id);
            s_sel = atoi(id);
            s_help[0] = 0;
            return;
        }
        if (strcmp(fn_name, "AS_SetHelp") == 0) {
            frame_string(node, locals, 0, s_help, sizeof s_help);
            return;
        }
        if (strcmp(fn_name, "AS_SetPurchaseButtonText") == 0) return;
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            char desc[1024];
            frame_string(node, locals, 1, desc, sizeof desc);
            char say[2048];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s.%s%s %s%s",
                        s_fresh && s_title[0] ? s_title : "", s_fresh && s_title[0] ? ". " : "",
                        s_sel >= 0 && s_sel < s_n ? s_row[s_sel] : "?",
                        s_help[0] ? " " : "", s_help, desc,
                        s_fresh ? " Enter purchases." : "");
            s_fresh = 0;
            logf_("[%ld] %s %s.%s  OTS %d \"%s\"\n", n, tag, obj_name, fn_name, s_sel, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The Gray Market (UIGreyMarket). Every change redraws all of it
    // (UpdateData):
    //     AS_SetTitle; AS_SetHeader(i, text) x4 -- "In Storage", "", "Sell",
    //         and the sale's total ("§0")
    //     AS_AddItem(int i, storage, name, price, sell, total, int, bool canSell)
    //         per item; storage is what stays after the sale, "-" for none;
    //         sell "-" until one is marked, total "" until then
    //     AS_SetListSelection(int i)
    //     AS_UpdateInfo(Desc, image): "NAME\nNOT RESEARCHED\nCannot sell this
    //         item\nsummary", the middle two only when they apply
    // Up/Down move (OnHighlightUp/Down), Right marks one more to sell
    // (OnSellItem), Left takes one back (OnReturnItem), Enter completes the
    // sale at once -- no dialogue -- and Escape leaves, handing everything
    // back. A refused change plays the bad sound and draws nothing. On the
    // general path the rows were slots and every move said "ITEM n
    // unresolved". So each draw is said by what changed: the item on a move;
    // the count and the total on Right or Left, which change the item's stock
    // with its count; the sale on Enter, which empties the total and leaves
    // the stock as it was.
    if (strncmp(obj_name, "UIGreyMarket", 12) == 0) {
        #define GREY_ROWS 48
        typedef struct { char name[96], store[16], price[32], sell[16], total[32]; int can; } GreyRow;
        static GreyRow s_row[GREY_ROWS];
        static GreyRow s_prev;                  // the selected row as last said
        static char s_title[96], s_head_total[32], s_prev_total[32];
        static int  s_n, s_sel = -1;
        static void* s_obj;                     // a new screen is a new visit
        if (object != s_obj) {
            s_obj = object;
            s_n = 0;
            s_sel = -1;
            memset(&s_prev, 0, sizeof s_prev);
            s_prev_total[0] = 0;
        }
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            return;
        }
        if (strcmp(fn_name, "AS_SetHeader") == 0 && p->nnumbers) {
            if ((int)p->numbers[0] == 3) {
                char t[64];
                frame_string(node, locals, 0, t, sizeof t);
                money_text(t, s_head_total, sizeof s_head_total);
            }
            return;
        }
        if (strcmp(fn_name, "AS_AddItem") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= GREY_ROWS) return;
            static FrameArgs a;
            frame_args(node, locals, &a);
            GreyRow* r = &s_row[i];
            strncpy_s(r->store, sizeof r->store, a.ns > 0 ? a.s[0] : "", _TRUNCATE);
            strncpy_s(r->name, sizeof r->name, a.ns > 1 ? a.s[1] : "", _TRUNCATE);
            money_text(a.ns > 2 ? a.s[2] : "", r->price, sizeof r->price);
            strncpy_s(r->sell, sizeof r->sell, a.ns > 3 ? a.s[3] : "", _TRUNCATE);
            money_text(a.ns > 4 ? a.s[4] : "", r->total, sizeof r->total);
            r->can = a.nb > 0 ? a.b[a.nb - 1] : 1;
            if (i >= s_n) s_n = i + 1;
            return;
        }
        if (strcmp(fn_name, "AS_SetListSelection") == 0 && p->nnumbers) {
            s_sel = (int)p->numbers[0];
            return;
        }
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            // The description a line at a time; its first line is the name,
            // which the item already says.
            static char raw[FRAME_ARG_TEXT];
            char info[1024] = "";
            if (!frame_local_raw(node, locals, "Desc", raw, sizeof raw))
                frame_string(node, locals, 0, raw, sizeof raw);
            const GreyRow* r = s_sel >= 0 && s_sel < s_n ? &s_row[s_sel] : NULL;
            int line = 0;
            for (char* part = raw; part && *part; line++) {
                char* nl = strchr(part, '\n');
                if (nl) *nl = 0;
                strip_markup(part);
                size_t pl = strlen(part);
                if ((line > 0 || !r) && pl) {
                    size_t u = strlen(info);
                    _snprintf_s(info + u, sizeof info - u, _TRUNCATE, "%s%s%s", u ? " " : "",
                                part, strchr(".!?", part[pl - 1]) ? "" : ".");
                }
                part = nl ? nl + 1 : NULL;
            }

            int marked = r && r->sell[0] && strcmp(r->sell, "-") != 0;
            char item[512] = "";
            if (r)
                _snprintf_s(item, sizeof item, _TRUNCATE, "%s, %s in storage%s%s%s%s%s%s%s.",
                            r->name, strcmp(r->store, "-") == 0 ? "none" : r->store,
                            r->can && r->price[0] ? ", " : "", r->can ? r->price : "",
                            r->can && r->price[0] ? " each" : "",
                            marked ? ", selling " : "", marked ? r->sell : "",
                            marked ? " for " : "", marked ? r->total : "");

            int fresh = !s_prev.name[0];
            int same = r && strcmp(r->name, s_prev.name) == 0;
            int counted = same && strcmp(r->sell, s_prev.sell) != 0 &&
                          strcmp(r->store, s_prev.store) != 0;
            int sold = !counted && s_prev_total[0] && strcmp(s_prev_total, s_head_total) != 0 &&
                       (!s_head_total[0] || strncmp(s_head_total, "0 ", 2) == 0);
            char say[2048] = "";
            if (fresh)
                _snprintf_s(say, sizeof say, _TRUNCATE,
                            "%s%s%s %s Right sells one more, Left takes one back, "
                            "Enter completes the sale.",
                            s_title, s_title[0] ? ". " : "", item, info);
            else if (sold)
                _snprintf_s(say, sizeof say, _TRUNCATE, "Sold for %s. %s", s_prev_total, item);
            else if (counted && marked)
                _snprintf_s(say, sizeof say, _TRUNCATE, "Selling %s, %s. Total %s.",
                            r->sell, r->total, s_head_total);
            else if (counted)
                _snprintf_s(say, sizeof say, _TRUNCATE, "None selling. Total %s.",
                            s_head_total[0] ? s_head_total : "0 credits");
            else if (!same)
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s %s", item, info);
            if (r) s_prev = *r;
            else memset(&s_prev, 0, sizeof s_prev);
            if (!s_prev.name[0]) strcpy_s(s_prev.name, sizeof s_prev.name, "-");
            strncpy_s(s_prev_total, sizeof s_prev_total, s_head_total, _TRUNCATE);
            logf_("[%ld] %s %s.%s  GREY %d \"%s\"\n", n, tag, obj_name, fn_name, s_sel, say);
            if (say[0]) {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
    }

    // Medals (UIMedals, Barracks -> Medals). One screen, several views, each
    // opened with AS_SetDisplayMode(view) (GoToView):
    //   0  the medals: AS_SetTitle("CURRENT MEDALS"), then per medal
    //      AS_SetInfo(i, name, status, bool locked, icon) -- a locked one is
    //      only "LOCKED". No selection is sent until a move.
    //   1  one medal: AS_SetEditingTitle("EDIT MEDAL", name, "Awards
    //      remaining: 1"), AS_SetEditingButton(i, text, bool enabled) x3 --
    //      RENAME MEDAL, ASSIGN POWER (or POWER ASSIGNED), AWARD MEDAL --
    //      AS_SetEditingHelp (the power, or why it cannot be awarded yet),
    //      AS_SetFocus(i).
    //   2  its power: AS_SetTitle, AS_SetPowerInfo(i, description, image) x2,
    //      AS_SetPowerButton x2 (both "ASSIGN THIS POWER"), AS_SetFocus(i).
    //   3  the soldier list to award it (UISoldierList_AssignMedal), 4 its
    //      name (UIInputDialogue) -- screens of their own.
    // Up/Down move in 0 and 1, Left/Right in 2; Enter opens, picks, assigns
    // (a warning dialogue first); Escape goes back a view. Every move is
    // AS_SetFocus(i) as a bare number, which nothing resolved.
    if (strncmp(obj_name, "UIMedals", 8) == 0) {
        #define MEDAL_ROWS 12
        static int  s_view, s_fresh;
        static char s_title[96], s_row[MEDAL_ROWS][FOCUS_MAX_LABEL];
        static int  s_nrow;
        static char s_edit[3][96], s_btn[3][FOCUS_MAX_LABEL], s_help[512];
        static int  s_btn_on[3];
        static char s_power[2][512];
        static int  s_sel0;                     // the medal selected, kept across views
        static void* s_obj;
        if (object != s_obj) { s_obj = object; s_sel0 = 0; }
        if (strcmp(fn_name, "AS_SetDisplayMode") == 0 && p->nnumbers) {
            s_view = (int)p->numbers[0];
            s_fresh = 1;
            if (s_view == 0) s_nrow = 0;
            return;
        }
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            return;
        }
        if (strcmp(fn_name, "AS_SetInfo") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= MEDAL_ROWS) return;
            static FrameArgs a;
            frame_args(node, locals, &a);
            int locked = a.nb > 0 && a.b[0];
            if (!locked && a.ns > 2) medal_learn(a.s[2], a.s[0]);
            if (locked)
                strcpy_s(s_row[i], FOCUS_MAX_LABEL, "Locked");
            else
                _snprintf_s(s_row[i], FOCUS_MAX_LABEL, _TRUNCATE, "%s%s%s", a.s[0],
                            a.ns > 1 && a.s[1][0] ? ", " : "", a.ns > 1 ? a.s[1] : "");
            if (i >= s_nrow) s_nrow = i + 1;
            // Nothing follows the last medal and nothing is selected, so the
            // arrival is said once the rows stop: each one puts it back.
            if (s_view == 0 && s_fresh && i >= s_sel0) {
                char say[FOCUS_MAX_LABEL + 256];
                _snprintf_s(say, sizeof say, _TRUNCATE,
                            "%s%s%s. Up and Down choose, Enter opens a medal.",
                            s_title, s_title[0] ? ". " : "", s_row[s_sel0]);
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_after(say, SETTLE_MS);
            }
            return;
        }
        if (strcmp(fn_name, "AS_SetEditingTitle") == 0) {
            for (int i = 0; i < 3; i++) frame_string(node, locals, i, s_edit[i], sizeof s_edit[i]);
            return;
        }
        if (strcmp(fn_name, "AS_SetEditingButton") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= 3) return;
            frame_string(node, locals, 0, s_btn[i], sizeof s_btn[i]);
            s_btn_on[i] = !p->nbools || p->bools[0];
            return;
        }
        if (strcmp(fn_name, "AS_SetEditingHelp") == 0) {
            frame_string(node, locals, 0, s_help, sizeof s_help);
            return;
        }
        if (strcmp(fn_name, "AS_SetPowerInfo") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= 2) return;
            frame_string(node, locals, 0, s_power[i], sizeof s_power[i]);
            return;
        }
        if (strcmp(fn_name, "AS_SetEditingImage") == 0 || strcmp(fn_name, "AS_SetPowerButton") == 0)
            return;
        if (strcmp(fn_name, "AS_SetFocus") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            char say[2048] = "";
            if (s_view == 0 && i >= 0 && i < s_nrow) {
                s_sel0 = i;
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s.", s_row[i]);
            } else if (s_view == 1 && i >= 0 && i < 3) {
                char btn[FOCUS_MAX_LABEL + 16];
                _snprintf_s(btn, sizeof btn, _TRUNCATE, "%s%s.", s_btn[i],
                            s_btn_on[i] ? "" : ", unavailable");
                if (s_fresh)
                    _snprintf_s(say, sizeof say, _TRUNCATE,
                                "%s. %s. %s. %s%s%s Up and Down choose, Enter picks.",
                                s_edit[0], s_edit[1], s_edit[2], s_help, s_help[0] ? " " : "",
                                btn);
                else
                    strcpy_s(say, sizeof say, btn);
            } else if (s_view == 2 && i >= 0 && i < 2) {
                if (s_fresh)
                    _snprintf_s(say, sizeof say, _TRUNCATE,
                                "%s. Left and Right choose, Enter assigns. Power 1: %s Power 2: "
                                "%s On power %d.",
                                s_title, s_power[0], s_power[1], i + 1);
                else
                    _snprintf_s(say, sizeof say, _TRUNCATE, "Power %d: %s", i + 1, s_power[i]);
            }
            s_fresh = 0;
            logf_("[%ld] %s %s.%s  MEDALS view %d, %d \"%s\"\n", n, tag, obj_name, fn_name,
                  s_view, i, say);
            if (say[0]) {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
    }

    // A text box (UIInputDialogue): a medal's name, a soldier's. OnInit sends
    // SetData(title, maxChars, the text as it stands) and nothing else; the
    // typing goes to Flash's own text field, and the game reads it back only
    // on Enter (GetInputText). So what is asked, and what it holds now.
    if (strncmp(obj_name, "UIInputDialogue", 15) == 0 && strcmp(fn_name, "SetData") == 0) {
        char title[256], text[256], say[640];
        frame_string(node, locals, 0, title, sizeof title);
        frame_string(node, locals, 1, text, sizeof text);
        _snprintf_s(say, sizeof say, _TRUNCATE,
                    "%s %s%s%s Type, then Enter accepts, Escape cancels.", title,
                    text[0] ? "Now: " : "", text, text[0] ? "." : "");
        logf_("[%ld] %s %s.%s  INPUT \"%s\"\n", n, tag, obj_name, fn_name, say);
        speech_cancel_pending();
        if (g_speak && !muted()) speech_say_now(say);
        return;
    }

    // The XCOM Database (UITellMeMore, the pause menu's "XCOM Database"). A
    // list of sections, each followed by its topics while it is open:
    //     AS_SetTitle("XCOM Database"), AS_SetAudioButtonText, AS_ClearList,
    //     Invoke("clear") -- logged under UpdateLayout --, then
    //     AS_AddOption(i, label, 0) per row: a section drawn in
    //         GetHTMLColoredText's state 2 (green), a topic in 0
    //     and a selection (RealizeSelected):
    //     AS_SetAudioButtonText("Play Audio" / "Stop Audio" on a topic,
    //         "Expand Menu" / "Collapse Menu" on a section)
    //     AS_UpdateInfo(topic, text, image) -- empty on a section
    //     Invoke("setFocus", "i") -- the index as a string, logged under
    //         RealizeSelected
    // Up/Down wrap, Enter opens or closes a section (the list is drawn again)
    // or plays a topic's narration, Escape leaves. It opens on the section of
    // the facility it was called from. On the general path the audio
    // button's one string was taken for a list and replaced the topics, so
    // every move said "FOCUS n unresolved" or the button's label.
    if (strncmp(obj_name, "UITellMeMore", 12) == 0) {
        #define TMM_ROWS 256
        static char s_title[96], s_btn[64], s_topic[256], s_text[4096];
        static char s_row[TMM_ROWS][96];
        static unsigned char s_head[TMM_ROWS];
        static int  s_n, s_fresh;
        static void* s_obj;
        if (object != s_obj) { s_obj = object; s_fresh = 1; s_n = 0; }
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            return;
        }
        if (strcmp(fn_name, "AS_SetAudioButtonText") == 0) {
            frame_string(node, locals, 0, s_btn, sizeof s_btn);
            return;
        }
        if (strcmp(fn_name, "AS_SetBackButtonText") == 0) return;
        if (strcmp(fn_name, "AS_ClearList") == 0 || strcmp(fn_name, "UpdateLayout") == 0) {
            s_n = 0;
            return;
        }
        if (strcmp(fn_name, "AS_AddOption") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= TMM_ROWS) return;
            static char raw[FRAME_ARG_TEXT];
            frame_string(node, locals, 0, s_row[i], sizeof s_row[i]);
            s_head[i] = frame_local_raw(node, locals, "sLabel", raw, sizeof raw) &&
                        font_hue(raw) == 0x5CD16C;
            if (i >= s_n) s_n = i + 1;
            return;
        }
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            frame_string(node, locals, 0, s_topic, sizeof s_topic);
            frame_string(node, locals, 1, s_text, sizeof s_text);
            return;
        }
        if (strcmp(fn_name, "RealizeSelected") == 0) {
            int i;
            if (!string_index(p, &i) || i < 0 || i >= s_n) return;
            // "Expand Menu" means the section is shut.
            int shut = strstr(s_btn, "Expand") != NULL;
            static char say[4096 + 512];
            if (s_head[i])
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s, %s.%s", s_fresh ? s_title : "",
                            s_fresh && s_title[0] ? ". " : "", s_row[i],
                            shut ? "closed" : "open",
                            s_fresh ? " Up and Down choose, Enter opens or closes a section, "
                                      "and on a topic plays its narration." : "");
            else
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s. %s%s", s_fresh ? s_title : "",
                            s_fresh && s_title[0] ? ". " : "",
                            s_topic[0] ? s_topic : s_row[i], s_text,
                            s_fresh ? " Up and Down choose, Enter plays the narration." : "");
            s_fresh = 0;
            logf_("[%ld] %s %s.%s  TMM %d \"%.200s\"\n", n, tag, obj_name, fn_name, i, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The finance statement (UIBaseFinances, Situation Room -> View XCOM
    // Finances). UpdateData draws it whole, twice on arrival:
    //     AS_SetTitle("CASH FLOW STATEMENT")
    //     AS_SetNet("NET MONTHLY INCOME: +§265")
    //     AS_SetSection(0, "GROSS MONTHLY INCOME", "+§375", "", "")
    //     AS_SetSection(i, heading, total, labels, values) per section, the
    //         items as "2x Interceptor\n1x Skyranger\n" beside "-§40\n-§20\n"
    // and takes no arrows. Nothing was said (log of 2026-09-25). Each line is
    // kept -- a section with its items paired to their costs -- the whole
    // statement is said once the calls stop, and the arrows walk it
    // (fin_walk).
    if (strncmp(obj_name, "UIBaseFinances", 14) == 0) {
        static char s_netline[256];
        static void* s_obj;                 // a new visit is said again
        if (object != s_obj) { s_obj = object; g_fin_said[0] = 0; }
        char t[512];
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, t, sizeof t);
            g_fin_n = 0;
            g_fin_at = -1;
            if (t[0]) _snprintf_s(g_fin[g_fin_n++], sizeof g_fin[0], _TRUNCATE, "%s.", t);
            return;
        }
        if (strcmp(fn_name, "AS_SetNet") == 0) {
            frame_string(node, locals, 0, t, sizeof t);
            hire_text(t, s_netline, sizeof s_netline);
            if (s_netline[0] && g_fin_n < FIN_LINES)
                _snprintf_s(g_fin[g_fin_n++], sizeof g_fin[0], _TRUNCATE, "%s.", s_netline);
            return;
        }
        if (strcmp(fn_name, "AS_SetSection") == 0) {
            char head[128], total[128], sum[128];
            frame_string(node, locals, 0, head, sizeof head);
            frame_string(node, locals, 1, t, sizeof t);
            hire_text(t, total, sizeof total);
            if (!head[0] || g_fin_n >= FIN_LINES) return;      // an empty section
            static char labels[FRAME_ARG_TEXT], values[FRAME_ARG_TEXT];
            if (!frame_local_raw(node, locals, "Label", labels, sizeof labels)) labels[0] = 0;
            if (!frame_local_raw(node, locals, "Value", values, sizeof values)) values[0] = 0;
            char* line = g_fin[g_fin_n++];
            size_t used = (size_t)_snprintf_s(line, sizeof g_fin[0], _TRUNCATE, "%s: %s.", head,
                                              total);
            char* lctx = NULL;
            char* vctx = NULL;
            char* l = strtok_s(labels, "\n", &lctx);
            char* v = strtok_s(values, "\n", &vctx);
            for (; l && used < sizeof g_fin[0]; l = strtok_s(NULL, "\n", &lctx),
                                                v = v ? strtok_s(NULL, "\n", &vctx) : NULL) {
                strip_markup(l);
                sum[0] = 0;
                if (v) { strip_markup(v); hire_text(v, sum, sizeof sum); }
                int w = _snprintf_s(line + used, sizeof g_fin[0] - used, _TRUNCATE, " %s%s%s.",
                                    l, sum[0] ? ": " : "", sum);
                if (w < 0) break;
                used += (size_t)w;
            }
            // Said once the draw stops: each section puts it back.
            static char say[FIN_LINES * 128];
            say[0] = 0;
            size_t u = 0;
            for (int i = 0; i < g_fin_n; i++) {
                int w = _snprintf_s(say + u, sizeof say - u, _TRUNCATE, "%s%s", u ? " " : "",
                                    g_fin[i]);
                if (w < 0) break;
                u += (size_t)w;
            }
            if (strcmp(say, g_fin_said) != 0) {
                strncpy_s(g_fin_said, sizeof g_fin_said, say, _TRUNCATE);
                if (u < sizeof say)
                    _snprintf_s(say + u, sizeof say - u, _TRUNCATE,
                                " Up and Down read it a line at a time. Escape: back.");
                logf_("[%ld] %s %s.%s  FINANCES \"%s\"\n", n, tag, obj_name, fn_name, say);
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_after(say, SETTLE_MS);
            }
            return;
        }
    }

    // The labs with soldier slots. See slots_select.
    if (strncmp(obj_name, "UIGeneLab", 9) == 0 || strncmp(obj_name, "UIPsiLabs", 9) == 0 ||
        strncmp(obj_name, "UICyberneticsLab", 16) == 0) {
        if (strcmp(fn_name, "AS_SetTitleLabels") == 0) {
            frame_string(node, locals, 0, g_slots_title, sizeof g_slots_title);
            return;
        }
        if (strcmp(fn_name, "AS_ClearSoldiers") == 0) {
            g_slots_obj = object;
            g_slots_n = 0;
            return;
        }
        if (strcmp(fn_name, "AS_AddSlot") == 0) {
            if (g_slots_n >= SLOT_ROWS) return;
            static FrameArgs a;
            frame_args(node, locals, &a);
            for (int i = 0; i < a.ns; i++) strip_markup(a.s[i]);
            const char* name = a.ns > 0 ? a.s[0] : "";
            const char* status = a.ns > 1 ? a.s[1] : "";
            const char* button = a.ns > 2 ? a.s[2] : "";
            int off = a.nb > 0 && a.b[0];
            _snprintf_s(g_slots_row[g_slots_n], FOCUS_MAX_LABEL, _TRUNCATE, "%s%s%s.%s%s%s",
                        strcmp(name, "(EMPTY)") == 0 ? "Empty" : name, status[0] ? ", " : "",
                        status, button[0] ? " Enter: " : "", button,
                        button[0] ? (off ? ", unavailable." : ".") : "");
            g_slots_button[g_slots_n] = button[0] != 0;
            g_slots_n++;
            return;
        }
        // The end of every draw. The game's own choice when it made one;
        // otherwise the first slot with a button, as the game does without a
        // mouse. The arrival names the lab.
        if (strcmp(fn_name, "AS_SetSelected") == 0) {
            int i;
            if (!string_index(p, &i) || i < 0 || i >= g_slots_n) {
                i = 0;
                for (int k = 0; k < g_slots_n; k++) if (g_slots_button[k]) { i = k; break; }
            }
            char lead[160];
            _snprintf_s(lead, sizeof lead, _TRUNCATE, "%s%s", g_slots_title,
                        g_slots_title[0] ? ". Up and Down choose a slot. " : "");
            slots_select(n, i, lead);
            return;
        }
    }

    // A soldier's gene mods (UISoldierGeneMods), from the Genetics Lab or, to
    // look only, from the soldier's own screen. Five rows, the body parts
    // (AS_SetRowData(i, "BRAIN", locked)), two mods each; the arrows move the
    // selection natively, and each move (RealizeSelected) sends
    //     AS_SetSelectedIcon(row, col), AS_SetDescription(name, text),
    //     AS_SetImplantButtonHelp("SELECT" / "REMOVE" / "INSUFFICIENT
    //         RESOURCES", icon) -- empty for a locked or installed mod --
    //     AS_SetRequirements(meld, cash) -- empty the same way, and last.
    // Enter ticks or unticks the mod; Y opens the confirm dialogue, and Y
    // cannot be pressed in the headquarters, so 1 stands in for it (input.c).
    if (strncmp(obj_name, "UISoldierGeneMods", 17) == 0) {
        static char s_title[64], s_rowname[5][48], s_name[96], s_desc[1024], s_button[96];
        static int  s_row = -1, s_col = -1, s_said_row = -1, s_said_col = -1, s_fresh;
        static char s_said_button[96];
        if (strcmp(fn_name, "AS_InitializeTree") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            s_fresh = 1;
            s_said_row = s_said_col = -1;
            return;
        }
        if (strcmp(fn_name, "AS_SetRowData") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i >= 0 && i < 5) frame_string(node, locals, 0, s_rowname[i], sizeof s_rowname[i]);
            return;
        }
        if (strcmp(fn_name, "AS_SetSelectedIcon") == 0 && p->nnumbers >= 2) {
            s_row = (int)p->numbers[0];
            s_col = (int)p->numbers[1];
            return;
        }
        if (strcmp(fn_name, "AS_SetDescription") == 0) {
            frame_string(node, locals, 0, s_name, sizeof s_name);
            frame_string(node, locals, 1, s_desc, sizeof s_desc);
            return;
        }
        if (strcmp(fn_name, "AS_SetImplantButtonHelp") == 0) {
            frame_string(node, locals, 0, s_button, sizeof s_button);
            return;
        }
        if (strcmp(fn_name, "AS_SetRequirements") == 0) {
            char meld[64], cash[64], cost[160] = "";
            frame_string(node, locals, 0, meld, sizeof meld);
            frame_string(node, locals, 1, cash, sizeof cash);
            // The Meld is an injected icon and then the number, and the
            // stripped text came back empty ("Costs  Meld", 2026-09-27): the
            // number is taken from the raw text, its last run of digits.
            if (!meld[0]) {
                static char raw[FRAME_ARG_TEXT];
                static int raw_logged;
                if (frame_local_raw(node, locals, "meldLabel", raw, sizeof raw)) {
                    if (!raw_logged) {
                        raw_logged = 1;
                        logf_("[%ld] GENEMODS meld raw \"%.200s\"\n", n, raw);
                    }
                    const char* end = NULL;
                    for (const char* c = raw; *c; c++)
                        if (*c >= '0' && *c <= '9' && !(c[1] >= '0' && c[1] <= '9')) end = c;
                    if (end) {
                        const char* start = end;
                        while (start > raw && start[-1] >= '0' && start[-1] <= '9') start--;
                        size_t len = (size_t)(end - start + 1);
                        if (len < sizeof meld) { memcpy(meld, start, len); meld[len] = 0; }
                    }
                }
            }
            if (meld[0] || cash[0]) {
                char c[64];
                hire_text(cash, c, sizeof c);
                _snprintf_s(cost, sizeof cost, _TRUNCATE, " Costs %s Meld, %s.", meld, c);
            }
            const char* state = strstr(s_button, "REMOVE") ? "Chosen."
                              : strstr(s_button, "INSUFFICIENT") ? "Not enough resources."
                              : s_button[0] ? "Not chosen." : "";
            char say[2048];
            if (s_row == s_said_row && s_col == s_said_col && !s_fresh) {
                // Enter on the same mod: only whether it is now chosen.
                if (strcmp(s_button, s_said_button) == 0) return;
                strcpy_s(say, sizeof say, state);
            } else {
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s, mod %d of 2. %s%s%s%s%s",
                            s_fresh ? s_title : "", s_fresh && s_title[0] ? ". " : "",
                            s_row != s_said_row && s_row >= 0 && s_row < 5 ? s_rowname[s_row] : "",
                            s_row != s_said_row ? ". " : "", s_name, s_col + 1,
                            state, state[0] ? " " : "", s_desc, cost,
                            s_fresh ? " Up and Down choose the body part, Left and Right the "
                                      "mod, Enter chooses it, 1 confirms." : "");
            }
            s_fresh = 0;
            s_said_row = s_row;
            s_said_col = s_col;
            strcpy_s(s_said_button, sizeof s_said_button, s_button);
            logf_("[%ld] %s %s.%s  GENEMODS %d, %d \"%s\"\n", n, tag, obj_name, fn_name, s_row,
                  s_col, say);
            if (say[0]) {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
        if (strcmp(fn_name, "AS_SetCalloutImage") == 0 || strcmp(fn_name, "AS_SetIcon") == 0)
            return;
    }

    // Build Items. UpdateLayout sends the heading and column labels
    // (AS_SetLabels(title, "ITEM", "BUILT")), the tabs' states, clears the
    // list (Invoke "clear") and fills it in one Invoke("BatchAddOptions",
    // [label, quantity, ...]); RealizeSelected then sends the item's panel
    // (AS_UpdateInfo) and the selection as text (AS_SetFocus("3")). Left
    // and right change tab, which runs all of that and then RealizeSelected
    // once more.
    if (strncmp(obj_name, "UIBuildItem", 11) == 0) {
        static char s_qty_label[64];
        g_builditem_at = GetTickCount64();
        if (strcmp(fn_name, "AS_SetLabels") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            if (a.ns > 0 && a.s[0][0]) focus_set_title(object, a.s[0]);
            strncpy_s(s_qty_label, sizeof s_qty_label, a.ns > 2 ? a.s[2] : "", _TRUNCATE);
            logf_("[%ld] %s %s.%s  BUILD title \"%s\", count \"%s\"\n", n, tag, obj_name,
                  fn_name, a.ns > 0 ? a.s[0] : "", s_qty_label);
            return;
        }
        // The tab's index, which the general path took for a move to the
        // item at that index.
        if (strcmp(fn_name, "AS_SetTabState") == 0 ||
            strcmp(fn_name, "AS_SetSelectedCategory") == 0)
            return;
        if (strcmp(fn_name, "AS_SetConfirmButton") == 0) {
            if (p->nstrings) help_set(object, 0, p->strings[0], "Icon_A_X", 0);
            return;
        }
        if (strcmp(fn_name, "UpdateLayout") == 0) {
            AbarValue* v;
            int nv = asvalues_from_frame(stack, &v);
            focus_begin(object);
            if (nv <= 0) {
                logf_("[%ld] %s %s.%s  BUILD cleared\n", n, tag, obj_name, fn_name);
                return;
            }
            int k = 0;
            for (int i = 0; i + 1 < nv; i++) {
                if (v[i].type != ABAR_STRING || v[i + 1].type != ABAR_NUMBER) continue;
                char row[FOCUS_MAX_LABEL];
                hq_build_row(v[i].s, (int)v[i + 1].n, s_qty_label, row, sizeof row);
                focus_set(object, k, row);
                logf_("[%ld] %s %s.%s  BUILD %d = \"%s\"\n", n, tag, obj_name, fn_name, k, row);
                k++;
                i++;
            }
            return;
        }
        // EU sends the rows one at a time instead, after the same clear:
        // AS_AddOption(int iIndex, string sLabel, bool IsDisabled, int
        // iQuantity), the label coloured the same way.
        if (strcmp(fn_name, "AS_AddOption") == 0 && p->nnumbers >= 2) {
            static char raw[FRAME_ARG_TEXT];
            char row[FOCUS_MAX_LABEL];
            frame_local_raw(node, locals, "sLabel", raw, sizeof raw);
            hq_build_row(raw, (int)p->numbers[1], s_qty_label, row, sizeof row);
            focus_set(object, (int)p->numbers[0], row);
            logf_("[%ld] %s %s.%s  BUILD %d = \"%s\"\n", n, tag, obj_name, fn_name,
                  (int)p->numbers[0], row);
            return;
        }
        // The item's panel: its name (the label already says it), the cost
        // from the raw text so a requirement short is marked, and the
        // description.
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            static char raw[FRAME_ARG_TEXT], cost[FRAME_ARG_TEXT];
            static FrameArgs a;
            static char detail[FOCUS_MAX_DETAIL];
            frame_local_raw(node, locals, "infoText", raw, sizeof raw);
            hq_cost_text(raw, cost, sizeof cost);
            frame_args(node, locals, &a);
            const char* parts[2];
            int np = 0;
            if (cost[0]) parts[np++] = cost;
            if (a.ns > 2 && a.s[2][0]) parts[np++] = a.s[2];
            focus_join_detail(parts, np, detail, sizeof detail);
            focus_set_detail(object, detail);
            logf_("[%ld] %s %s.%s  PANEL \"%s\"\n", n, tag, obj_name, fn_name, detail);
            if (object == g_focus_obj && !g_focus_had_panel && detail[0] &&
                GetTickCount64() - g_focus_at < LIST_WINDOW_MS) {
                g_focus_had_panel = 1;
                if (g_speak && !muted()) speech_say(detail);
            }
            return;
        }
        // A tab change selects twice, the second time with the same item and
        // the same panel; said again, it cut the first off, heading and all.
        if (strcmp(fn_name, "AS_SetFocus") == 0) {
            static void*     s_obj;
            static int       s_idx = -1;
            static char      s_said[FOCUS_MAX_LABEL + FOCUS_MAX_DETAIL];
            static ULONGLONG s_at;
            int idx;
            if (!string_index(p, &idx) || idx < 0) return;
            static char label[FOCUS_MAX_LABEL], detail[FOCUS_MAX_DETAIL];
            static char key[FOCUS_MAX_LABEL + FOCUS_MAX_DETAIL];
            ULONGLONG d_at, t = GetTickCount64();
            if (!focus_label_at(object, idx, label, sizeof label)) label[0] = 0;
            if (!focus_detail(object, detail, sizeof detail, &d_at)) detail[0] = 0;
            _snprintf_s(key, sizeof key, _TRUNCATE, "%s|%s", label, detail);
            if (object == s_obj && idx == s_idx && t - s_at < 1500 &&
                strcmp(key, s_said) == 0) {
                focus_set_detail(object, "");
                logf_("[%ld] %s %s.%s  FOCUS %d (said, same again)\n",
                      n, tag, obj_name, fn_name, idx);
                return;
            }
            s_obj = object;
            s_idx = idx;
            s_at = t;
            strncpy_s(s_said, sizeof s_said, key, _TRUNCATE);
            focus_announce(n, tag, obj_name, fn_name, object, idx);
            return;
        }
    }

    // Hiring: soldiers (UIHiring_Barracks, Barracks -> Hire Soldiers) and
    // interceptors (UIHiring_Hangar, an empty hangar slot). Every change
    // redraws it all (UpdateData):
    //     AS_UpdateInfo("Hiring Cost:<br>§10<br>", "Barracks Capacity:<br>13/70")
    //     AS_SetTitle("HIRE SOLDIERS"); AS_SetIcon
    //     the count, a spinner on the screen's UIWidgetHelper:
    //         SetSpinnerValue("1"), SetSpinnerArrows
    // Up and Down go to the spinner first and raise or lower the count
    // (OnIncreaseQuantity / OnDecreaseQuantity); Enter hires, Escape cancels.
    // On the general path only the first "1" was said, and the count went to
    // 8 and the cost to §80 without a word (log of 2026-09-25). As for an
    // order (below), a helper call within a moment of a hiring draw is the
    // hiring's; the draw is said at its spinner value, whole on arrival and
    // then when the count changes.
    if (strncmp(obj_name, "UIWidgetHelper", 14) == 0 && g_hire.at &&
        GetTickCount64() - g_hire.at < ENG_SAME_DRAW_MS) {
        if (strstr(fn_name, "SpinnerValue") && p->nstrings) {
            char count[32];
            strncpy_s(count, sizeof count, p->strings[0], _TRUNCATE);
            char say[1024] = "";
            if (g_hire.fresh)
                _snprintf_s(say, sizeof say, _TRUNCATE,
                            "%s%s%s. %s %s Up and Down change the number, Enter: %s, "
                            "Escape: cancel.",
                            g_hire.title, g_hire.title[0] ? ". " : "", count, g_hire.cost,
                            g_hire.cap, g_hire.confirm[0] ? g_hire.confirm : "confirm");
            else if (strcmp(count, g_hire.count) != 0)
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s. %s %s", count, g_hire.cost,
                            g_hire.cap);
            g_hire.fresh = 0;
            strncpy_s(g_hire.count, sizeof g_hire.count, count, _TRUNCATE);
            logf_("[%ld] %s %s.%s  HIRE \"%s\"\n", n, tag, obj_name, fn_name, say);
            if (say[0]) {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
        // The arrows and the helper's own focus: nothing to say apart.
        if (strstr(fn_name, "Spinner") || strcmp(fn_name, "RealizeSelected") == 0) return;
    }
    if (strncmp(obj_name, "UIHiring", 8) == 0) {
        g_hire.at = GetTickCount64();
        if (object != g_hire.obj) {
            memset(&g_hire, 0, sizeof g_hire);
            g_hire.obj = object;
            g_hire.fresh = 1;
            g_hire.at = GetTickCount64();
        }
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            char t[256];
            frame_string(node, locals, 0, t, sizeof t);
            hire_text(t, g_hire.cost, sizeof g_hire.cost);
            frame_string(node, locals, 1, t, sizeof t);
            hire_text(t, g_hire.cap, sizeof g_hire.cap);
            // The cost ends in a <br>, and so a stop; the capacity does not.
            size_t cl = strlen(g_hire.cap);
            if (cl && !strchr(".!?", g_hire.cap[cl - 1]))
                strncat_s(g_hire.cap, sizeof g_hire.cap, ".", _TRUNCATE);
            return;
        }
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, g_hire.title, sizeof g_hire.title);
            return;
        }
        if (strcmp(fn_name, "AS_SetMouseConfirmText") == 0) {
            frame_string(node, locals, 0, g_hire.confirm, sizeof g_hire.confirm);
            return;
        }
        if (strcmp(fn_name, "AS_SetIcon") == 0) return;
    }

    // An order (UIManufacturing): a new one from Build Items, or one already
    // in the queue from Review an order. The screen is not a list. Every
    // redraw -- arriving, the quantity changing, rush toggled -- goes
    // UpdateData (the buttons through SetHelp), RefreshDisplay (AS_SetTitle,
    // AS_SetEngineerLine, the quantity spinner through the widget helper,
    // AS_SetQuantityLine for an order already placed), then AS_UpdateInfo
    // (the duration and cost, the notes). So the order is gathered and said
    // at AS_UpdateInfo: whole on arrival, then only what changed.
    //
    // The quantity is a spinner on a UIWidgetHelper, which by name belongs
    // to no screen. Its calls come in the same redraw as the order's, so a
    // helper call within a moment of one is the order's. Up and down move
    // it (the spinner is vertical), and so do left and right.
    if (strncmp(obj_name, "UIWidgetHelper", 14) == 0 && g_man_at &&
        GetTickCount64() - g_man_at < ENG_SAME_DRAW_MS) {
        if (strstr(fn_name, "SpinnerValue") && p->nstrings) {
            strncpy_s(g_man.qty, sizeof g_man.qty, p->strings[0], _TRUNCATE);
            logf_("[%ld] %s %s.%s  ORDER quantity \"%s\"\n", n, tag, obj_name, fn_name,
                  g_man.qty);
        }
        return;
    }
    if (strncmp(obj_name, "UIManufacturing", 15) == 0) {
        g_man_at = GetTickCount64();
        if (object != g_man.obj) {
            memset(&g_man, 0, sizeof g_man);
            g_man.obj = object;
        }
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_args(node, locals, &a);
            strncpy_s(g_man.title, sizeof g_man.title, a.ns > 0 ? a.s[0] : "", _TRUNCATE);
            return;
        }
        if (strcmp(fn_name, "AS_SetEngineerLine") == 0) {
            frame_args(node, locals, &a);
            _snprintf_s(g_man.eng, sizeof g_man.eng, _TRUNCATE, "%s %s",
                        a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetQuantityLine") == 0) {
            frame_args(node, locals, &a);
            strncpy_s(g_man.qty_label, sizeof g_man.qty_label, a.ns > 0 ? a.s[0] : "", _TRUNCATE);
            strncpy_s(g_man.qty, sizeof g_man.qty, a.ns > 1 ? a.s[1] : "", _TRUNCATE);
            return;
        }
        // Rush construction's button says whether it is on ("Rush
        // Construction YES"), so it is kept as part of the order; the call
        // goes on to the help bar below.
        if (strcmp(fn_name, "AS_SetHelp") == 0 && p->nnumbers && (int)p->numbers[0] == 2) {
            frame_args(node, locals, &a);
            strncpy_s(g_man.rush, sizeof g_man.rush, a.ns > 0 ? a.s[0] : "", _TRUNCATE);
        }
        if (strcmp(fn_name, "AS_UpdateInfo") == 0) {
            static char raw[FRAME_ARG_TEXT], info[FRAME_ARG_TEXT], notes[FRAME_ARG_TEXT];
            static char say[FOCUS_MAX_DETAIL];
            frame_local_raw(node, locals, "infoText", raw, sizeof raw);
            hq_cost_text(raw, info, sizeof info);
            frame_args(node, locals, &a);
            strncpy_s(notes, sizeof notes, a.ns > 1 ? a.s[1] : "", _TRUNCATE);
            char qty[96];
            if (g_man.qty[0])
                _snprintf_s(qty, sizeof qty, _TRUNCATE, "%s %s",
                            g_man.qty_label[0] ? g_man.qty_label : "QUANTITY:", g_man.qty);
            else
                qty[0] = 0;
            const char* parts[6];
            int np = 0;
            int first = !g_man.said;
            if (first && g_man.title[0]) parts[np++] = g_man.title;
            if (!first && g_man.rush[0] && strcmp(g_man.rush, g_man.said_rush) != 0)
                parts[np++] = g_man.rush;
            if (qty[0] && (first || strcmp(qty, g_man.said_qty) != 0)) parts[np++] = qty;
            if (g_man.eng[0] && (first || strcmp(g_man.eng, g_man.said_eng) != 0))
                parts[np++] = g_man.eng;
            if (info[0] && (first || strcmp(info, g_man.said_info) != 0)) parts[np++] = info;
            if (notes[0] && (first || strcmp(notes, g_man.said_notes) != 0)) parts[np++] = notes;
            focus_join_detail(parts, np, say, sizeof say);
            g_man.said = 1;
            strncpy_s(g_man.said_rush, sizeof g_man.said_rush, g_man.rush, _TRUNCATE);
            strncpy_s(g_man.said_qty, sizeof g_man.said_qty, qty, _TRUNCATE);
            strncpy_s(g_man.said_eng, sizeof g_man.said_eng, g_man.eng, _TRUNCATE);
            strncpy_s(g_man.said_info, sizeof g_man.said_info, info, _TRUNCATE);
            strncpy_s(g_man.said_notes, sizeof g_man.said_notes, notes, _TRUNCATE);
            logf_("[%ld] %s %s.%s  ORDER %s\"%s\"\n", n, tag, obj_name, fn_name,
                  first ? "" : "change ", say);
            if (say[0]) {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
    }

    // A country chosen on the room's map (Launch Satellite, covert ops). See
    // hq.h. The HUD beside the map is the only place the country is named;
    // what it draws is kept, and said when the map's selection moves.
    if (strncmp(obj_name, "UISituationRoomHUD_", 19) == 0) {
        static FrameArgs a;
        static char body[FRAME_ARG_TEXT];
        if (strcmp(fn_name, "AS_SetCountryInfo") == 0) {
            frame_args(node, locals, &a);
            frame_lines(node, locals, "bodyText", body, sizeof body);
            hq_sat_country(a.ns > 0 ? a.s[0] : "", body, p->nnumbers ? (int)p->numbers[0] : 0);
            return;
        }
        if (strcmp(fn_name, "AS_SetContinentInfo") == 0) {
            frame_args(node, locals, &a);
            frame_lines(node, locals, "bodyText", body, sizeof body);
            hq_sat_continent(a.ns > 0 ? a.s[0] : "", body);
            return;
        }
        if (strcmp(fn_name, "AS_SetLaunchButton") == 0 ||
            strcmp(fn_name, "AS_SetAccuseButton") == 0) {
            frame_args(node, locals, &a);
            hq_sat_button(fn_name[5] == 'A', a.ns > 1 ? a.s[1] : "", a.nb > 0 && a.b[0]);
            return;
        }
    }
    if (strncmp(obj_name, "UISituationRoom_", 16) == 0 &&
        obj_name[16] >= '0' && obj_name[16] <= '9') {
        g_sitroom_at = GetTickCount64();
        if (strcmp(fn_name, "AS_SetSatellites") == 0 && p->nnumbers >= 3) {
            hq_sat_count((int)p->numbers[0], (int)p->numbers[1], (int)p->numbers[2]);
            return;
        }
        if (strcmp(fn_name, "RealizeSelected") == 0) {
            int idx = p->nnumbers ? (int)p->numbers[0] : -1;
            if (idx < 0) {
                hq_sat_reset();
                logf_("[%ld] %s %s.%s  MAP left\n", n, tag, obj_name, fn_name);
                return;
            }
            static char say[2 * HQ_SIT_TEXT];
            if (!hq_sat_say(say, sizeof say)) {
                logf_("[%ld] %s %s.%s  MAP %d, no country drawn\n", n, tag, obj_name, fn_name,
                      idx);
                return;
            }
            logf_("[%ld] %s %s.%s  MAP %d -> \"%s\"\n", n, tag, obj_name, fn_name, idx, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The Situation Room's display. See hq.h. Kept for Delete; nothing else
    // is changed, so the countries are still filed as the slots the satellite
    // view's SetSelected moves over. The ticker and the objectives are read
    // from the frame whole: the payload keeps 256 characters, and the ticker
    // is every headline in one string.
    if ((strncmp(obj_name, "UISituationRoom_", 16) == 0 &&
         obj_name[16] >= '0' && obj_name[16] <= '9') ||
        strncmp(obj_name, "UIObjectivesScreen_", 19) == 0) {
        g_sitroom_at = GetTickCount64();
        if (strcmp(fn_name, "AS_SetCountryInfo") == 0 && p->nnumbers >= 2) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            hq_sit_country((int)p->numbers[0], a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "",
                           (int)p->numbers[1], a.nb > 0 ? a.b[0] : 1);
        } else if (strcmp(fn_name, "AS_SetTickerText") == 0) {
            static char title[64], text[MAX_STR];
            frame_string(node, locals, 0, title, sizeof title);
            frame_string(node, locals, 1, text, sizeof text);
            hq_sit_news(title, text);
        } else if (strcmp(fn_name, "AS_SetDoomLevel") == 0 && p->nnumbers >= 1) {
            hq_sit_doom((int)p->numbers[0]);
        } else if (strcmp(fn_name, "AS_SetSmallBody") == 0 ||
                   strcmp(fn_name, "AS_SetLargeBody") == 0) {
            static char body[MAX_STR];
            frame_string(node, locals, 0, body, sizeof body);
            if (fn_name[6] == 'S') hq_sit_objectives(body, NULL);
            else                   hq_sit_objectives(NULL, body);
        }
    }
    // Back at the facility row: out of the room, whatever drew last.
    if (strncmp(obj_name, "UIStrategyHUD_FacilityMenu", 26) == 0 &&
        strcmp(fn_name, "OnReceiveFocus") == 0)
        g_sitroom_left_at = g_eng_left_at = GetTickCount64();

    // The mission's objectives. See mission.h. Kept, and said once a burst
    // of changes is over (mission_poll).
    if (g_mission_due) mission_poll();
    if (strncmp(obj_name, "UITacticalHUD_ObjectivesList", 28) == 0) {
        mission_note(n, object, fn_name, node, locals, p);
        return;
    }

    // The unit information screen (F1). See info.h. Its calls are read by
    // position, and nothing is said until the burst is over (info_settle).
    // The summary is due from the cursor's per-frame poll; it is checked
    // here too, since the HUD goes on redrawing while the screen is up, in
    // case the cursor does not.
    if (g_info_due) info_settle();
    if (strncmp(obj_name, "UIUnitGermanMode", 16) == 0) {
        info_note(n, object, obj_name, fn_name, node, locals);
        return;
    }

    // The shot about to be taken. Its panel states one thing per call and
    // says nothing on any of them, so the burst is composed and spoken once.
    // shot.c holds the order it depends on.
    // A unit's name, over its head. Each argument arrives twice -- as the
    // parameter and again in the ASValue array -- and an empty nickname not
    // at all, so a second string equal to the first is the name repeated.
    if (strncmp(obj_name, "UIUnitFlag_", 11) == 0 && strcmp(fn_name, "SetNames") == 0 &&
        p->nstrings > 0) {
        const char* nick = p->nstrings > 1 && strcmp(p->strings[1], p->strings[0]) != 0
                               ? p->strings[1] : "";
        unit_note(object, p->strings[0], nick);
    }
    if (strncmp(obj_name, "UIUnitFlag_", 11) == 0)
        unit_flag_drew(object, fn_name, p);
    // The selected soldier's panel. See soldier.h. Not spoken as it passes:
    // soldier_poll announces a switch once the flags have caught up.
    if (strncmp(obj_name, "UITacticalHUD_SoldierStatsContainer", 35) == 0 &&
        strcmp(fn_name, "SetStats") == 0) {
        soldier_stats_note(n, p);
        return;
    }
    // The weapon panels: the equipped weapon and the ammo each has. Kept for
    // the soldier's readouts, not said as they pass (weapon_note).
    if (weapon_note(n, obj_name, fn_name, p)) return;
    // Whose turn it is. See combat_turn in combat.h.
    if (strncmp(obj_name, "UITurnOverlay", 13) == 0) {
        const char* strs[8];
        int ns = 0;
        for (int i = 0; i < p->nstrings && ns < 8; i++) strs[ns++] = p->strings[i];
        char say[80];
        if (combat_turn(fn_name, strs, ns, say, sizeof say)) {
            logf_("[%ld] %s %s.%s  TURN \"%s\"\n", n, tag, obj_name, fn_name, say);
            announce_as(SET_TURN, say);
        } else {
            logf_("[%ld] %s %s.%s  (turn banner)\n", n, tag, obj_name, fn_name);
        }
        return;
    }
    // The message ticker along the top of the screen: "Sq. O'Reilly takes a
    // reaction shot!", "Corporal Hudson has earned a promotion!". Everything
    // goes through UIMessageMgr.Message, which has already decided whether
    // the local player may see it, then UIMessageMgr_Container.Message, whose
    // CreateMessageBox hands Flash (id, title, icon, pulse). The id is
    // "default<n>" or the caller's own; the title is the text.
    if (strncmp(obj_name, "UIMessageMgr_Container", 22) == 0 &&
        strcmp(fn_name, "CreateMessageBox") == 0) {
        const char* title = NULL;
        for (int i = 1; i < p->nstrings; i++)
            if (p->strings[i][0] && strcmp(p->strings[i], p->strings[0]) != 0 &&
                !looks_like_asset(p->strings[i])) {
                title = p->strings[i];
                break;
            }
        if (title) {
            logf_("[%ld] %s %s.%s  TICKER \"%s\"\n", n, tag, obj_name, fn_name, title);
            announce_as(SET_TICKER, title);
        }
        return;
    }

    // Floating combat text. See combat.h.
    if (strncmp(obj_name, "UIWorldMessageMgr", 17) == 0 &&
        (strcmp(fn_name, "CreateNewMessage") == 0 ||
         strcmp(fn_name, "UpdateExistingMessageContents") == 0)) {
        combat_message(n, stack, p);
        return;
    }
    if (strncmp(obj_name, "UISightlineHUD_SightlineContainer", 33) == 0)
        strip_note(object, fn_name, p);

    // The ability bar, kept for numpad . to read. Nothing is said as it
    // passes: it is rebuilt on every soldier switch, move and target change.
    if (strncmp(obj_name, "UITacticalHUD_AbilityContainer", 30) == 0) {
        if (object != g_abar_obj) {
            abar_reset();
            g_abar_obj = object;
        }
        if (strstr(fn_name, "SetNumActiveAbilities") && p->nnumbers > 0) {
            abar_set_count((int)p->numbers[0]);
        } else if (strcmp(fn_name, "PopulateFlash") == 0) {
            int got = abar_from_frame(stack);
            if (got < 0 && !g_abar_logged_fail) {
                g_abar_logged_fail = 1;
                logf_("[%ld] abar: PopulateFlash's stream did not parse -- the bar "
                      "will be out of date\n", n);
            } else if (got >= 0) {
                char bar[1024];
                abar_describe(bar, sizeof bar);
                logf_("[%ld] abar: %d slot%s updated -> \"%s\"\n", n, got,
                      got == 1 ? "" : "s", bar);
            }
            return;
        }
    }

    // Targeting lowered, by Escape or by the shot being taken. Without this,
    // picking the same ability again after a cancel was dropped as a repeat
    // and said nothing.
    // UITacticalHUD is the only class with this function, in both builds.
    if (strcmp(fn_name, "LowerTargetSystem") == 0) {
        shot_forget_said();
        g_reticle_said[0] = 0;
        // An aim has nothing left to point once targeting is down.
        if (g_nav_aim) nav_stop("targeting lowered");
    }

    // The aiming reticle's message: "Shot is blocked." is the one warning the
    // game gives before a free-aimed shot goes into a wall, and it is drawn
    // nowhere else. UITargetingReticle.UpdateShotData sends it when the
    // blocked state changes, and OnInit sends an empty one while the reticle
    // builds, so an empty message is not "clear" and is not spoken. The same
    // message is said again only after a pause, so the re-send that follows
    // OnInit is not heard twice.
    if (strncmp(obj_name, "UITargetingReticle", 18) == 0 &&
        strcmp(fn_name, "SetCursorMessage") == 0) {
        const char* msg = p->nstrings ? p->strings[0] : "";
        ULONGLONG t = GetTickCount64();
        if (*msg && (strcmp(msg, g_reticle_said) != 0 ||
                     t - g_reticle_said_at > RETICLE_REPEAT_MS)) {
            strncpy_s(g_reticle_said, sizeof g_reticle_said, msg, _TRUNCATE);
            g_reticle_said_at = t;
            logf_("[%ld] %s %s.%s  RETICLE \"%s\"\n", n, tag, obj_name, fn_name, msg);
            if (g_speak && !muted()) speech_say(msg);
            return;
        }
    }

    if (shot_is_panel(obj_name)) {
        // Each argument arrives twice -- once as the parameter, once inside
        // the ASValue array built from it -- so the first occurrences are
        // taken and the repeats dropped. Empty arguments never arrive at all:
        // read_fstring rejects a zero-length string, which is how
        // SetShotChance("", "") reads as a call with no text.
        const char* a = "";
        const char* b = "";
        for (int i = 0; i < p->nstrings; i++) {
            if (looks_like_asset(p->strings[i])) continue;
            if (!*a) { a = p->strings[i]; continue; }
            if (strcmp(p->strings[i], a) == 0) continue;
            b = p->strings[i];
            break;
        }
        int flag = p->nbools ? p->bools[0] : -1;

        // Who the shot is at, looked up as the burst ends, since that is the
        // one call made from inside Update and so the one with the soldier
        // in reach.
        if (strstr(fn_name, "UpdateLayout")) shot_target_now(stack);

        char say[SHOT_MAX_TEXT];
        shot_set_brief(g_nav_aim);
        if (shot_note(fn_name, a, b, flag, say, sizeof say)) {
            logf_("[%ld] %s %s.%s  SHOT \"%s\"\n", n, tag, obj_name, fn_name, say);
            // While the numpad moves an aim, the chance follows the step that
            // moved it, and must not cut its coordinates off.
            if (g_nav_aim) {
                if (g_speak && !muted()) speech_say(say);
            } else {
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
        } else if (*a) {
            logf_("[%ld] %s %s.%s  SHOT held \"%s\"%s%s\n", n, tag, obj_name,
                  fn_name, a, *b ? " / " : "", b);
        } else {
            logf_("[%ld] %s %s.%s  SHOT held (no text)\n", n, tag, obj_name, fn_name);
        }
        return;
    }

    // The help bar: the screen's own list of what it can do, kept so that a
    // key can read it back.  It is not spoken as it passes -- a screen
    // publishes it on arrival and on every refresh, and narrating that would
    // bury whatever the player was actually doing.  help.c says why this is
    // the list worth having.
    //
    // The dialogue box publishes through the same AS_SetHelp and is excluded
    // above, on purpose: it announces its own two answers with their keys as
    // part of the prompt, which is more use than an entry in a list the
    // player would have to go and ask for.
    if (!dialog_is_box(obj_name) && strstr(fn_name, "Help")) {
        if (strstr(fn_name, "Clear")) {
            help_clear(object);
            logf_("[%ld] %s %s.%s  HELP cleared\n", n, tag, obj_name, fn_name);
            return;
        }
        if (p->nnumbers) {
            // A glyph name identifies the button; the other string is the
            // label.  AS_SetTabHelp carries neither an index nor a glyph and
            // falls through to the general path, which is right: the tabs it
            // describes are already on Tab and 1.
            const char* label = "";
            const char* icon  = "";
            for (int i = 0; i < p->nstrings; i++) {
                if (strncmp(p->strings[i], "Icon_", 5) == 0) {
                    if (!*icon) icon = p->strings[i];
                } else if (!*label && !looks_like_asset(p->strings[i])) {
                    label = p->strings[i];
                }
            }
            // A mouse-mode PC bar's frame number, standing in for a glyph.
            // Back and Accept keep their keys, and so do the soldier
            // screens' Previous and Next soldier: Summary, Loadout,
            // Promotion, Customize and Gene Mods all take `case 514`
            // (Left Shift) and `case 571` (Tab), in both builds. The rest
            // are mouse buttons.
            const char* pc = !*icon ? hq_pc_icon_label(label) : NULL;
            if (pc) {
                label = pc;
                if (strcmp(pc, "Back") == 0) icon = "Icon_B_CIRCLE";
                else if (strcmp(pc, "Accept") == 0) icon = "Icon_A_X";
                // On Build Items frame 3 is the item card, not Accept:
                // UIBuildItem adds it with OnMouseAccept, which opens the
                // card, as F1 does (case 600). Enter is MANUFACTURE, which
                // the screen's own confirm button names.
                if (strcmp(pc, "Accept") == 0 &&
                    GetTickCount64() - g_builditem_at < ENG_SAME_DRAW_MS) {
                    label = "Details";
                    icon = "Icon_KEY_F1";
                }
                else if (strcmp(pc, "Previous soldier") == 0) icon = "Icon_KEY_LEFT_SHIFT";
                else if (strcmp(pc, "Next soldier") == 0) icon = "Icon_KEY_TAB";
            }
            int slot = (int)p->numbers[0];
            int disabled = p->nbools ? p->bools[0] : 0;
            help_set(object, slot, label, icon, disabled);
            if (slot >= 0 && slot < 2 && strcmp(fn_name, "AS_SetWeaponHelp") == 0 &&
                strncmp(obj_name, "UIShipSummary", 13) == 0) {
                strncpy_s(g_ship_btn[slot], sizeof g_ship_btn[slot], label, _TRUNCATE);
                g_ship_btn_off[slot] = disabled;
            }
            logf_("[%ld] %s %s.%s  HELP %d = \"%s\" on %s%s\n", n, tag, obj_name,
                  fn_name, slot, label, *icon ? icon : "(no icon)",
                  disabled ? " (disabled)" : "");
            return;
        }
    }

    // A screen that draws its own two buttons instead of using a help bar:
    //
    //     UIContinentSelect.AS_SetAcceptButton(string Text, string iconLabel)
    //     UIContinentSelect.AS_SetBackButton(string Text, string iconLabel)
    //
    // Neither name says "Help", so the buttons never reached the list 0
    // reads, and on the continent screen -- where Enter picks the base and
    // nothing asks again -- 0 read the tactical HUD's leftover bar instead.
    // Filed as the screen's own bar, accept first. An empty back button is
    // the game switching Escape off (m_bDisableCancel at a campaign's start),
    // and clearing the slot says exactly that. Exact names: both are declared
    // on UIContinentSelect alone, in both builds (EU has no back button).
    if (strcmp(fn_name, "AS_SetAcceptButton") == 0 ||
        strcmp(fn_name, "AS_SetBackButton") == 0) {
        const char* label = "";
        const char* icon  = "";
        for (int i = 0; i < p->nstrings; i++) {
            if (strncmp(p->strings[i], "Icon_", 5) == 0) {
                if (!*icon) icon = p->strings[i];
            } else if (!*label && !looks_like_asset(p->strings[i])) {
                label = p->strings[i];
            }
        }
        int slot = fn_name[6] == 'A' ? 0 : 1;
        help_set(object, slot, label, icon, 0);
        logf_("[%ld] %s %s.%s  HELP %d = \"%s\" on %s\n", n, tag, obj_name,
              fn_name, slot, label, *icon ? icon : "(no icon)");
        return;
    }

    // The panel beside a list, describing the item under the cursor. See
    // focus_set_detail. Kept, not filed as a list: it replaced the list it
    // describes. Said with the item's label when the cursor moves; if it
    // arrives just after the move was said, it follows on its own.
    // The strings are read from the frame whole: the payload keeps 256
    // characters, and a research project's description stopped mid-word
    // ("... ways to improve the sold").
    // The facility list's panel (UIChooseFacility.UpdateInfoPanelData):
    // AS_UpdateInfo(techName, infoText, descText, imageLabel). imageLabel is
    // GetFacilityLabel's "AlienContainment", which looks_like_asset cannot
    // tell from a word and was read aloud; descText is why it cannot be
    // built ("Disabled for Tutorial") "\n" the summary, which ran together;
    // infoText's "\xC2\xA7" "85" is a sum. The name is the list's label.
    if (strncmp(obj_name, "UIChooseFacility", 16) == 0 &&
        strcmp(fn_name, "AS_UpdateInfo") == 0) {
        static char raw[FRAME_ARG_TEXT], cost[FRAME_ARG_TEXT], desc[FRAME_ARG_TEXT];
        static char detail[FOCUS_MAX_DETAIL];
        frame_local_raw(node, locals, "infoText", raw, sizeof raw);
        hq_cost_text(raw, cost, sizeof cost);
        frame_lines(node, locals, "descText", desc, sizeof desc);
        const char* parts[2];
        int np = 0;
        if (cost[0]) parts[np++] = cost;
        if (desc[0]) parts[np++] = desc;
        focus_join_detail(parts, np, detail, sizeof detail);
        focus_set_detail(object, detail);
        logf_("[%ld] %s %s.%s  PANEL \"%s\"\n", n, tag, obj_name, fn_name, detail);
        if (object == g_focus_obj && !g_focus_had_panel && detail[0] &&
            GetTickCount64() - g_focus_at < LIST_WINDOW_MS) {
            g_focus_had_panel = 1;
            if (g_speak && !muted()) speech_say(detail);
        }
        return;
    }
    if (strcmp(fn_name, "AS_UpdateInfo") == 0 && p->nstrings) {
        static FrameArgs a;
        frame_args(node, locals, &a);
        const char* parts[FRAME_ARGS];
        int np = 0;
        for (int i = 0; i < a.ns; i++)
            if (a.s[i][0] && !looks_like_asset(a.s[i])) parts[np++] = a.s[i];
        static char detail[FOCUS_MAX_DETAIL];
        focus_join_detail(parts, np, detail, sizeof detail);
        focus_set_detail(object, detail);
        logf_("[%ld] %s %s.%s  PANEL \"%s\"\n", n, tag, obj_name, fn_name, detail);
        if (object == g_focus_obj && !g_focus_had_panel && detail[0] &&
            GetTickCount64() - g_focus_at < LIST_WINDOW_MS) {
            g_focus_had_panel = 1;
            if (g_speak && !muted()) speech_say(detail);
        }
        return;
    }

    // The base's facility menu. See hq.h: its first publish is one call per
    // facility, its updates a command stream, and a facility's name is
    // composed with what its colour and alert say before it is filed.
    if (strncmp(obj_name, "UIStrategyHUD_FacilityMenu", 26) == 0) {
        if (object != g_hq_menu) { hq_facility_reset(); g_hq_menu = object; }
        // A move. The labels are put back from hq.c's own copy first: the
        // slot table can reclaim this object while the player is elsewhere
        // in the base, and the menu never republishes them on its return.
        if (strcmp(fn_name, "AS_SetFocus") == 0) {
            for (int id = 0; id < HQ_FACILITIES; id++) {
                char label[FOCUS_MAX_LABEL];
                if (hq_facility_label(id, label, sizeof label))
                    focus_set(object, id, label);
            }
        }
        if (strcmp(fn_name, "AS_AddMenuOption") == 0 && p->nnumbers && p->nstrings) {
            int id = (int)p->numbers[0];
            const char* name = "";
            int grey = 0;
            for (int i = 0; i < p->nstrings; i++)
                if (!looks_like_asset(p->strings[i])) {
                    name = p->strings[i];
                    grey = p->hues[i] == HUE_DISABLED;
                    break;
                }
            hq_facility_set(id, name, grey, p->nbools ? p->bools[0] : 0);
            char label[FOCUS_MAX_LABEL];
            if (hq_facility_label(id, label, sizeof label))
                focus_set(object, id, label);
            logf_("[%ld] %s %s.%s  FACILITY %d = \"%s\"\n", n, tag, obj_name,
                  fn_name, id, label);
            return;
        }
        if (strcmp(fn_name, "UpdateData") == 0) {
            AbarValue* vals;
            int nv = asvalues_from_frame(stack, &vals);
            int touched = nv < 0 ? -1 : hq_facility_feed(vals, nv);
            if (touched < 0) {
                logf_("[%ld] %s %s.%s  FACILITY update did not parse\n",
                      n, tag, obj_name, fn_name);
                return;
            }
            for (int id = 0; id < HQ_FACILITIES; id++) {
                char label[FOCUS_MAX_LABEL];
                if (!(touched & (1 << id)) ||
                    !hq_facility_label(id, label, sizeof label)) continue;
                focus_set(object, id, label);
                logf_("[%ld] %s %s.%s  FACILITY %d now \"%s\"\n", n, tag,
                      obj_name, fn_name, id, label);
            }
            return;
        }
    }

    // What the game says to the player in words: Central and the other
    // advisors (UINarrativeCommLink: AS_SetTitle the speaker, AS_SetText the
    // line) and the tutorial's instruction box (UIStrategyTutorialBox:
    // "Select the Barracks"). Both used to go through the lone-line path,
    // whose one pending slot the next line overwrites: at the Barracks the
    // box's "Select "View Soldiers"" was replaced five calls later by
    // Central's line, so one of the two was never heard. Queued as events
    // instead, and kept for the Insert list. The comm link re-sends a line it
    // is still showing, so the same line is said once.
    if ((strncmp(obj_name, "UINarrativeCommLink", 19) == 0 ||
         strncmp(obj_name, "UIStrategyTutorialBox", 21) == 0) &&
        (strcmp(fn_name, "AS_SetText") == 0 || strcmp(fn_name, "AS_SetTitle") == 0)) {
        static char s_speaker[64];
        static char s_said[MAX_STR];
        static ULONGLONG s_said_at;
        const char* text = p->nstrings ? p->strings[0] : "";
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            strncpy_s(s_speaker, sizeof s_speaker, text, _TRUNCATE);
            logf_("[%ld] %s %s.%s  SPEAKER \"%s\"\n", n, tag, obj_name, fn_name, text);
            return;
        }
        if (!*text) return;
        char say[MAX_STR + 80];
        if (obj_name[2] == 'N' && s_speaker[0])
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s: %s", s_speaker, text);
        else
            strncpy_s(say, sizeof say, text, _TRUNCATE);
        ULONGLONG t = GetTickCount64();
        if (strcmp(say, s_said) == 0 && t - s_said_at < NARRATIVE_REPEAT_MS) {
            logf_("[%ld] %s %s.%s  NARRATIVE repeated\n", n, tag, obj_name, fn_name);
            return;
        }
        strncpy_s(s_said, sizeof s_said, say, _TRUNCATE);
        s_said_at = t;
        logf_("[%ld] %s %s.%s  NARRATIVE \"%s\"\n", n, tag, obj_name, fn_name, say);
        announce_as(obj_name[2] == 'N' ? SET_NARRATIVE : -1, say);
        return;
    }

    // A soldier list: the Barracks' View Soldiers, and every other screen
    // built on UISoldierListBase. UpdateDisplay clears the list, adds one
    // row per soldier with no index, then sends the column headings and the
    // count, and selects through Invoke("SetSelected", [string index]) --
    // which the string index resolves (string_index). Left to the general
    // path, every row was "a screen publishing its contents" and replaced
    // the one before, and the headings and "3/4" replaced the lot.
    if (strstr(obj_name, "UISoldierList")) {
        if (strcmp(fn_name, "UpdateDisplay") == 0 && !p->nstrings) {
            focus_begin(object);                 // Invoke("ClearSoldierList")
            logf_("[%ld] %s %s.%s  SOLDIERS cleared\n", n, tag, obj_name, fn_name);
            return;
        }
        int nick = strcmp(fn_name, "AS_AddSoldierWithNickname") == 0;
        if (nick || strcmp(fn_name, "AS_AddSoldier") == 0) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            // By position, empties kept: name, [nickname,] class, status,
            // rank label; then the bools disabled, promotable.
            int k = nick ? 1 : 0;
            if (a.ns < 4 + k) {
                logf_("[%ld] %s %s.%s  SOLDIER row did not read (%d strings)\n",
                      n, tag, obj_name, fn_name, a.ns);
                return;
            }
            char row[FOCUS_MAX_LABEL];
            hq_soldier_row(a.s[0], nick ? a.s[1] : "", a.s[1 + k], a.s[2 + k],
                           a.s[3 + k], a.nb > 0 && a.b[0], a.nb > 1 && a.b[1],
                           row, sizeof row);
            focus_add(object, row);
            logf_("[%ld] %s %s.%s  SOLDIER %d = \"%s\"\n", n, tag, obj_name,
                  fn_name, focus_count(object) - 1, row);
            return;
        }
        if (strcmp(fn_name, "AS_SetCountLabel") == 0) {
            char head[96];
            if (p->nstrings && hq_soldier_count(p->strings[0], head, sizeof head)) {
                focus_set_title(object, head);
                logf_("[%ld] %s %s.%s  HEADING \"%s\"\n", n, tag, obj_name, fn_name, head);
            }
            return;
        }
        if (strcmp(fn_name, "AS_SetTitleLabels") == 0) {
            logf_("[%ld] %s %s.%s  (column headings)\n", n, tag, obj_name, fn_name);
            return;
        }
    }

    // The loadout: two lists on one screen, the soldier's inventory and the
    // locker of what can go in the slot under the cursor.
    //
    //     UpdateInventoryList: Invoke("ClearInventoryList"), then per slot
    //         AS_AddInventoryItem(int Type, string Title, string imgLabel,
    //                             int numEquipableItems, GFxObject mecIcons)
    //     UpdateLockerList:    Invoke("ClearLockerList"), then per item
    //         AS_AddLockerItem(string Title, string Count, string imgLabel,
    //                          bool isLocked, bool showItemCard,
    //                          string lockedDescription, GFxObject mecIcons)
    //     AS_SetSelectedIndex_InventoryList(int) / _LockerList(int)
    //
    // Up/down move within a list, right goes into the locker and left back,
    // Enter equips. Both lists landed in the screen's one slot table, so the
    // inventory read icon paths and the locker said "Body Armor" wherever
    // the cursor was. Kept as two lists -- the locker under the screen's
    // address plus one, which no object can have -- and the list is named
    // when the cursor crosses into the other.
    if (strncmp(obj_name, "UISoldierLoadout", 16) == 0) {
        void* inv = object;
        void* locker = (uint8_t*)object + 1;
        static FrameArgs a;
        if (strcmp(fn_name, "UpdateInventoryList") == 0 ||
            strcmp(fn_name, "UpdateLockerList") == 0) {
            focus_begin(fn_name[6] == 'I' ? inv : locker);
            return;
        }
        if (strcmp(fn_name, "AS_AddInventoryItem") == 0 ||
            strcmp(fn_name, "AS_AddLockerItem") == 0) {
            int in_locker = fn_name[6] == 'L';
            frame_args(node, locals, &a);
            char row[FOCUS_MAX_LABEL];
            if (!in_locker) {
                strncpy_s(row, sizeof row, a.ns ? a.s[0] : "", _TRUNCATE);
            } else {
                // Title, Count ("x3", or "" for infinite), imgLabel,
                // lockedDescription; the bools isLocked, showItemCard.
                const char* count = a.ns > 1 ? a.s[1] : "";
                const char* why = a.ns > 3 ? a.s[3] : "";
                int is_locked = a.nb > 0 && a.b[0];
                _snprintf_s(row, sizeof row, _TRUNCATE, "%s%s%s%s%s",
                            a.ns ? a.s[0] : "", *count ? " " : "", count,
                            is_locked ? ", " : "",
                            is_locked ? (*why ? why : "unavailable") : "");
            }
            focus_add(in_locker ? locker : inv, row);
            logf_("[%ld] %s %s.%s  %s %d = \"%s\"\n", n, tag, obj_name, fn_name,
                  in_locker ? "LOCKER" : "INVENTORY",
                  focus_count(in_locker ? locker : inv) - 1, row);
            return;
        }
        if (strncmp(fn_name, "AS_SetSelectedIndex_", 20) == 0 && p->nnumbers) {
            int in_locker = fn_name[20] == 'L';
            void* list = in_locker ? locker : inv;
            int idx = (int)p->numbers[0];
            char label[FOCUS_MAX_LABEL], say[FOCUS_MAX_LABEL + 64];
            if (!focus_label_at(list, idx, label, sizeof label)) {
                logf_("[%ld] %s %s.%s  FOCUS %d unresolved\n", n, tag, obj_name, fn_name, idx);
                return;
            }
            char title[FOCUS_MAX_LABEL];
            ULONGLONG t_at;
            int head = focus_take_title(inv, title, sizeof title, &t_at) &&
                       GetTickCount64() - t_at < TITLE_FRESH_MS;
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s",
                        head ? title : "", head ? ". " : "",
                        g_loadout_side != list ? (in_locker ? "Locker. " : "Inventory. ") : "",
                        label);
            g_loadout_side = list;
            if (!in_locker) { g_loadout_inv = object; g_loadout_inv_idx = idx; }
            logf_("[%ld] %s %s.%s  FOCUS %d -> \"%s\"\n", n, tag, obj_name, fn_name, idx, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetScreenTitle") == 0 && p->nstrings) {
            focus_set_title(inv, p->strings[0]);
            return;
        }
        // The screen's own buttons, into its help bar for 0. Details is the
        // item card, which F1 reaches (case 600); Remove is X, which no
        // keyboard key sends in the headquarters -- 1 stands in (input.c).
        if (strcmp(fn_name, "AS_SetLockerButtonHelp") == 0 && p->nstrings >= 2) {
            help_set(object, 0, p->strings[0], "Icon_A_X", 0);
            if (p->nstrings >= 3) help_set(object, 1, "DETAILS: F1", "", 0);
            return;
        }
        if (strcmp(fn_name, "AS_SetRemoveInventorySlotButtonHelp") == 0 && p->nstrings) {
            help_set(object, 2, p->strings[0], "Icon_X_SQUARE", 0);
            return;
        }
        if (strcmp(fn_name, "AS_SetListTitles") == 0) return;
    }

    // A button moving between choices: (int buttonIndex, bool bFocus), sent
    // twice per move -- the old button off, the new one on. Declared the same
    // way on the Mission Control alerts, the council's requests and missions,
    // and the infiltrator mission, in both builds. Only the "on" call is the
    // move.
    if (strcmp(fn_name, "AS_SetButtonFocus") == 0 && p->nnumbers && p->nbools) {
        if (p->bools[0])
            focus_announce(n, tag, obj_name, fn_name, object, (int)p->numbers[0]);
        return;
    }

    // Mission Control's notices: "Rk. Christophe Leroy has returned to active
    // duty.", an item built, new scientists. See hq_notices_new. The whole
    // list comes on every refresh, so only the lines new since the last one
    // are said. Read from the local, raw: the notices are divided by "\n",
    // which strip_markup would make a space.
    if (strncmp(obj_name, "UIMissionControl_", 17) == 0 && obj_name[17] >= '0' &&
        obj_name[17] <= '9' && strcmp(fn_name, "UpdateNotices") == 0) {
        static char raw[MAX_STR], say[MAX_STR];
        if (frame_local_raw(node, locals, "displayString", raw, sizeof raw) &&
            hq_notices_new(raw, say, sizeof say) > 0) {
            logf_("[%ld] %s %s.%s  NOTICE \"%s\"\n", n, tag, obj_name, fn_name, say);
            announce(say);
        }
        return;
    }

    // A Mission Control alert: "ALIEN ABDUCTIONS REPORTED!", a UFO, a
    // finished project. Its title and text are single strings and its
    // buttons indexed labels, so on the general path the buttons cancelled
    // the title before it was said, and with the mouse active nothing is
    // selected on arrival -- the alert said nothing at all, and up/down on it
    // were silent. It is said whole, as an event (alert_say), once it is
    // filled -- see alert_note for when that is.
    // Scrambling interceptors: the UFO alert's ShipSelection state
    // (UIMissionControl_UFORadarContactAlert) lists the squadron in the alert
    // itself --
    //     global.UpdateData()  -- the title and particulars again
    //     AS_AddShip(name, weapon, status, icon, bool disabled) per jet
    //     AS_ActivateShipList(launchLabel)
    //     AS_SetShipFocus(old, false); AS_SetShipFocus(new, true)
    // with no index on AddShip, so the rows are counted from the title. Up
    // and down wrap; Enter launches the focused jet, or plays the bad sound
    // on a disabled one. The title and particulars sent again are not a new
    // alert, and are dropped.
    if (strncmp(obj_name, "UIMissionControl_UFORadarContactAlert", 37) == 0) {
        static char ships[8][FOCUS_MAX_LABEL];
        static int nships;
        static ULONGLONG listed_at;
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetTitle") == 0) nships = 0;
        if (strcmp(fn_name, "AS_AddShip") == 0) {
            frame_args(node, locals, &a);
            if (nships < 8) {
                _snprintf_s(ships[nships], sizeof ships[nships], _TRUNCATE, "%s, %s, %s%s",
                            a.ns > 0 ? a.s[0] : "", a.ns > 1 ? a.s[1] : "",
                            a.ns > 2 ? a.s[2] : "", a.nb > 0 && a.b[0] ? ", unavailable" : "");
                logf_("[%ld] %s %s.%s  SHIP %d = \"%s\"\n", n, tag, obj_name, fn_name, nships,
                      ships[nships]);
                nships++;
            }
            return;
        }
        if (strcmp(fn_name, "AS_ActivateShipList") == 0) {
            char label[128], say[FOCUS_MAX_LABEL * 8 + 160];
            frame_string(node, locals, 0, label, sizeof label);
            g_alert_due = NULL;
            g_alert_title[0] = g_alert_text[0] = g_alert_sub[0] = g_alert_rebates[0] = 0;
            size_t w = 0;
            w += _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%d interceptor%s", label,
                             label[0] ? ". " : "", nships, nships == 1 ? "" : "s");
            for (int i = 0; i < nships && w < sizeof say; i++)
                w += _snprintf_s(say + w, sizeof say - w, _TRUNCATE, "%s%s",
                                 i ? ". " : ": ", ships[i]);
            _snprintf_s(say + w, sizeof say - w, _TRUNCATE, ".");
            logf_("[%ld] %s %s.%s  SHIPS \"%s\"\n", n, tag, obj_name, fn_name, say);
            listed_at = GetTickCount64();
            speech_cancel_pending();
            announce(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetShipFocus") == 0) {
            if (!p->nnumbers || !p->nbools || !p->bools[0]) return;
            int i = (int)p->numbers[0];
            if (i < 0 || i >= nships) return;
            logf_("[%ld] %s %s.%s  SHIP focus %d -> \"%s\"\n", n, tag, obj_name, fn_name, i,
                  ships[i]);
            // Straight after the list, the first row is selected for the
            // player: the list has just named it, and cutting it off would
            // lose the rest.
            if (GetTickCount64() - listed_at < 500) return;
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(ships[i]);
            return;
        }
        if (strcmp(fn_name, "AS_DeactivateShipList") == 0) return;
        // The particulars sent again for the ship list: kept out of the next
        // alert (AS_ActivateShipList clears them).
    }
    if (strncmp(obj_name, "UIMissionControl_", 17) == 0 && strstr(obj_name, "Alert")) {
        if (alert_note(n, tag, obj_name, fn_name, object, node, locals, p)) return;
    }
    if (strncmp(obj_name, "UISpecialUnlockDialogue", 23) == 0 &&
        strncmp(fn_name, "AS_", 3) == 0) {
        unlock_note(n, tag, obj_name, fn_name, node, locals, p);
        return;
    }

    // A council mission ("COUNCIL MISSION. GATEWAY. The latest reports..."),
    // EW's covert op (UIInfiltratorMission) and a council request: each
    // screen's UpdateData sends its whole text in one call, then its two
    // buttons (NUM_BUTTONS, both builds):
    //     AS_OpenMissionRequest(Title, subtitle, DescriptionText, reward,
    //                           topSecretLabel)
    //     AS_OpenSalesRequest(Title, subtitle, requestLabel, requestData,
    //         storageLabel, storageData, timeLabel, timeData,
    //         DescriptionText, reward, imagePath, float, topSecretLabel)
    //     AS_SetButtonData(int, label, bool disabled)  x2
    // and a request fulfilled one call with its one button:
    //     AS_OpenRequestCompleteDialog(Title, subtitle, Description,
    //                                  rewards, buttonLabel)
    // The text went nowhere; the log of 2026-09-25 heard only LAUNCH MISSION
    // and NOT NOW. Said like an alert (alert_say) once the second button is
    // in. "Not now" is disabled in the tutorial (ISCONTROLLED), and says so.
    if (strncmp(obj_name, "UIFundingCouncil", 16) == 0 ||
        strncmp(obj_name, "UIInfiltratorMission", 20) == 0) {
        int mission = strcmp(fn_name, "AS_OpenMissionRequest") == 0;
        int sales = strcmp(fn_name, "AS_OpenSalesRequest") == 0;
        int done = strcmp(fn_name, "AS_OpenRequestCompleteDialog") == 0;
        if (mission || sales || done) {
            static FrameArgs a;
            frame_args(node, locals, &a);
            focus_begin(object);
            g_alert_due = NULL;
            strncpy_s(g_alert_title, sizeof g_alert_title, a.ns > 0 ? a.s[0] : "", _TRUNCATE);
            strncpy_s(g_alert_sub, sizeof g_alert_sub, a.ns > 1 ? a.s[1] : "", _TRUNCATE);
            g_alert_rebates[0] = 0;
            static char req[3][FRAME_ARG_TEXT + 64];
            const char* parts[6];
            int np = 0;
            if (sales) {
                // "REQUESTED: 2 Sectoid Corpses", the storage and the time
                // left; the description and the reward after.
                for (int i = 0; i < 3; i++) {
                    const char* label = a.ns > 2 + 2 * i ? a.s[2 + 2 * i] : "";
                    const char* data = a.ns > 3 + 2 * i ? a.s[3 + 2 * i] : "";
                    size_t ll = strlen(label);
                    _snprintf_s(req[i], sizeof req[i], _TRUNCATE, "%s%s%s", label,
                                !ll || !data[0] ? "" : label[ll - 1] == ':' ? " " : ": ", data);
                }
                parts[np++] = a.ns > 8 ? a.s[8] : "";
                for (int i = 0; i < 3; i++) parts[np++] = req[i];
                parts[np++] = a.ns > 9 ? a.s[9] : "";
            } else {
                parts[np++] = a.ns > 2 ? a.s[2] : "";
                parts[np++] = a.ns > 3 ? a.s[3] : "";
            }
            focus_join_detail(parts, np, g_alert_text, sizeof g_alert_text);
            logf_("[%ld] %s %s.%s  REQUEST \"%s\" \"%s\" \"%s\"\n", n, tag, obj_name, fn_name,
                  g_alert_title, g_alert_sub, g_alert_text);
            if (done) {
                if (a.ns > 4 && a.s[4][0]) focus_set(object, 0, a.s[4]);
                alert_say(n, tag, obj_name, object);
            }
            return;
        }
        if (strcmp(fn_name, "AS_SetButtonData") == 0 && p->nnumbers && p->nstrings) {
            alert_note(n, tag, obj_name, fn_name, object, node, locals, p);
            if ((int)p->numbers[0] == 1 && (g_alert_title[0] || g_alert_text[0]))
                alert_say(n, tag, obj_name, object);
            return;
        }
    }

    // The research archives, and the report shown when research finishes:
    // one screen, UIScienceLabs. See hq_report_*. The list is
    //     AS_ClearArchives(), AS_SetArchiveTitle("ARCHIVES"),
    //     AS_AddOption(int i, label, bool), AS_SetListSelection(int i)
    // with no second int, so the general path took SetListSelection for a
    // container's (widget, item) pair and read "All" from another screen's
    // list. The report is gathered and said whole when the list is put away
    // (AS_EnableArchives(false), the last call of GoToView(3) and of OnInit
    // straight into a report); up and down, which only scroll it, walk it.
    if (strncmp(obj_name, "UIScienceLabs", 13) == 0) {
        static char s_rep[HQ_REPORT_PIECES][HQ_REPORT_TEXT];
        static int  s_rep_n, s_rep_at;
        if (strcmp(fn_name, "AS_ClearArchives") == 0) { focus_begin(object); return; }
        if (strcmp(fn_name, "AS_SetArchiveTitle") == 0) {
            char t[FOCUS_MAX_LABEL];
            frame_string(node, locals, 0, t, sizeof t);
            if (t[0]) focus_set_title(object, t);
            return;
        }
        if (strcmp(fn_name, "AS_SetTopSecretText") == 0) return;
        if (strcmp(fn_name, "AS_AddOption") == 0 && p->nnumbers && p->nstrings) {
            focus_set(object, (int)p->numbers[0], p->strings[0]);
            return;
        }
        if (strcmp(fn_name, "AS_SetListSelection") == 0 && p->nnumbers) {
            focus_announce(n, tag, obj_name, fn_name, object, (int)p->numbers[0]);
            return;
        }
        if (strcmp(fn_name, "AS_SetReportTitles") == 0) {
            static char t[512], sub[512];
            frame_string(node, locals, 0, t, sizeof t);
            // Raw: the codename and the date are split by "\n".
            if (!frame_local_raw(node, locals, "subTitleText", sub, sizeof sub))
                frame_string(node, locals, 1, sub, sizeof sub);
            hq_report_titles(t, sub);
            return;
        }
        if (strcmp(fn_name, "AS_SetReportItem") == 0) {
            static char subject[512], notes[MAX_STR];
            frame_string(node, locals, 0, subject, sizeof subject);
            frame_string(node, locals, 1, notes, sizeof notes);
            hq_report_item(subject, notes);
            return;
        }
        if (strcmp(fn_name, "AS_ClearResults") == 0) { hq_report_results_clear(); return; }
        if (strcmp(fn_name, "AS_AddResults") == 0) {
            char r[512];
            frame_string(node, locals, 0, r, sizeof r);
            hq_report_result(r);
            return;
        }
        if (strcmp(fn_name, "AS_EnableArchives") == 0) {
            s_rep_n = 0;
            if (p->nbools && !p->bools[0] && hq_report_ready()) {
                static char say[MAX_STR + 2048];
                hq_report_text(say, sizeof say);
                s_rep_n = hq_report_pieces(s_rep, HQ_REPORT_PIECES);
                s_rep_at = -1;
                logf_("[%ld] %s %s.%s  REPORT %d pieces \"%s\"\n", n, tag, obj_name, fn_name,
                      s_rep_n, say);
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(say);
            }
            return;
        }
        int down = strcmp(fn_name, "AS_ScrollResearchDown") == 0;
        if (down || strcmp(fn_name, "AS_ScrollResearchUp") == 0) {
            if (!s_rep_n) return;
            const char* edge = "";
            s_rep_at += down ? 1 : -1;
            if (s_rep_at >= s_rep_n) { s_rep_at = s_rep_n - 1; edge = "End. "; }
            if (s_rep_at < 0)        { s_rep_at = 0;           edge = "Top. "; }
            char say[HQ_REPORT_TEXT + 8];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s", edge, s_rep[s_rep_at]);
            logf_("[%ld] %s %s.%s  REPORT %d \"%s\"\n", n, tag, obj_name, fn_name, s_rep_at,
                  say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // An item card (F1 on the loadout, and wherever else UIItemCards opens):
    // its name, its stats and its paragraphs, each on a call of its own, and
    // nothing ever read them -- the title was held and cancelled by the stat
    // slots after it. UIItemCards.OnInit fills the card and then calls
    // AS_InitializationComplete, so the card is gathered call by call and
    // said whole there. By position (frame_args):
    //     AS_SetCardTitle(Title)
    //     AS_SetStatData(int statIndex, statLabel, statVal, optional statDiff)
    //     AS_AddSimpleTextCardData(Text)
    //     AS_AddTacticalInfoCardData / AS_AddAbilitiesCardData /
    //     AS_AddPerksCardData(Title, text)
    if (strncmp(obj_name, "UIItemCards", 11) == 0) {
        static char s_card[2048];
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetHelp") == 0 && p->nstrings) {
            const char* icon = p->nstrings > 1 ? p->strings[1] : "";
            help_set(object, 0, p->strings[0], icon, 0);
            return;
        }
        if (strcmp(fn_name, "AS_InitializationComplete") == 0) {
            logf_("[%ld] %s %s.%s  CARD \"%s\"\n", n, tag, obj_name, fn_name, s_card);
            if (s_card[0]) {
                history_add(s_card);
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(s_card);
            }
            s_card[0] = 0;
            return;
        }
        int title = strcmp(fn_name, "AS_SetCardTitle") == 0;
        int stat = strcmp(fn_name, "AS_SetStatData") == 0;
        int para = strncmp(fn_name, "AS_Add", 6) == 0 && strstr(fn_name, "CardData");
        if (title || stat || para) {
            frame_args(node, locals, &a);
            char piece[1200];
            piece[0] = 0;
            if (title && a.ns)
                strncpy_s(piece, sizeof piece, a.s[0], _TRUNCATE);
            else if (stat && a.ns >= 2 && a.s[0][0])
                _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s %s%s%s", a.s[0], a.s[1],
                            a.ns > 2 && a.s[2][0] ? " " : "", a.ns > 2 ? a.s[2] : "");
            else if (para && strstr(fn_name, "SimpleText") && a.ns)
                strncpy_s(piece, sizeof piece, a.s[0], _TRUNCATE);
            else if (para && a.ns >= 2 && a.s[1][0])
                _snprintf_s(piece, sizeof piece, _TRUNCATE, "%s: %s", a.s[0], a.s[1]);
            hq_card_clean(piece);
            if (title) s_card[0] = 0;
            if (piece[0]) {
                size_t used = strlen(s_card);
                _snprintf_s(s_card + used, sizeof s_card - used, _TRUNCATE, "%s%s",
                            used ? ". " : "", piece);
            }
            return;
        }
    }

    // An abduction site's details, after the widget helper has named the
    // city. See hq_abduction_line. Filed as a slot before, and never said:
    // the player heard "CHICAGO, UNITED STATES" and nothing of its panic,
    // difficulty or reward, which are the whole of the choice.
    if (strncmp(obj_name, "UIMissionControl_AbductionSelection", 35) == 0) {
        static char s_labels[3][48];
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetHeaderLabels") == 0) {
            frame_args(node, locals, &a);
            for (int i = 0; i < 3; i++)
                strncpy_s(s_labels[i], sizeof s_labels[i], a.ns > i + 1 ? a.s[i + 1] : "",
                          _TRUNCATE);
            return;
        }
        if (strcmp(fn_name, "AS_SetData") == 0 && p->nnumbers) {
            frame_args(node, locals, &a);
            char say[512];
            hq_abduction_line(s_labels[0], (int)p->numbers[0], s_labels[1],
                              a.ns > 1 ? a.s[1] : "", s_labels[2], a.ns > 2 ? a.s[2] : "",
                              say, sizeof say);
            logf_("[%ld] %s %s.%s  SITE \"%s\"\n", n, tag, obj_name, fn_name, say);
            // With the city the widget helper has just named, as one line
            // that interrupts: said after it, a quick run of presses queued
            // a city-and-details pair per press.
            char city[FOCUS_MAX_LABEL], both[FOCUS_MAX_LABEL + 512];
            if (g_focus_obj && GetTickCount64() - g_focus_at < LIST_WINDOW_MS &&
                focus_label_at(g_focus_obj, g_focus_idx, city, sizeof city))
                _snprintf_s(both, sizeof both, _TRUNCATE, "%s. %s", city, say);
            else
                strncpy_s(both, sizeof both, say, _TRUNCATE);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(both);
            return;
        }
    }

    // The loading briefing before a mission (UIBriefing): the operation and
    // its place, the intel, the objectives as bullets and a tip, then
    // "LOADING..." and, once the map is in, "READY TO ENGAGE". Several
    // strings per call, so each went into a list and nothing was said. The
    // pieces are gathered -- StartBriefing sends them twice, the second time
    // final -- and said as one event at "LOADING...", which follows the last
    // of them; "READY TO ENGAGE" is said when it comes, since Enter then
    // starts the mission.
    if (strncmp(obj_name, "UIBriefing", 10) == 0) {
        static char s_where[256], s_intel[1024], s_goals[512], s_tip[512];
        char* dst = NULL;
        size_t dst_sz = 0;
        if (strcmp(fn_name, "AS_SetMissionInfo") == 0) { dst = s_where; dst_sz = sizeof s_where; }
        else if (strcmp(fn_name, "AS_SetIntel") == 0)  { dst = s_intel; dst_sz = sizeof s_intel; }
        else if (strcmp(fn_name, "AS_SetObjectives") == 0) { dst = s_goals; dst_sz = sizeof s_goals; }
        else if (strcmp(fn_name, "AS_SetTip") == 0)    { dst = s_tip;   dst_sz = sizeof s_tip; }
        if (dst) {
            const char* parts[8];
            int np = 0;
            for (int i = 0; i < p->nstrings && np < 8; i++)
                if (!looks_like_asset(p->strings[i])) parts[np++] = p->strings[i];
            focus_join_detail(parts, np, dst, dst_sz);
            hq_card_clean(dst);
            return;
        }
        if (strcmp(fn_name, "AS_SetLoadingMessage") == 0 && p->nstrings) {
            const char* msg = p->strings[0];
            char say[2560];
            if (s_where[0] || s_intel[0]) {
                _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s%s%s", s_where,
                            s_intel[0] ? ". " : "", s_intel, s_goals[0] ? ". " : "", s_goals,
                            s_tip[0] ? ". " : "", s_tip);
                s_where[0] = s_intel[0] = s_goals[0] = s_tip[0] = 0;
                logf_("[%ld] %s %s.%s  BRIEFING \"%s\"\n", n, tag, obj_name, fn_name, say);
                announce(say);
            }
            // "LOADING..." is what the briefing is said under; the rest --
            // "READY TO ENGAGE" -- is news.
            if (!strstr(msg, "...")) {
                logf_("[%ld] %s %s.%s  BRIEFING \"%s\"\n", n, tag, obj_name, fn_name, msg);
                announce(msg);
            }
            return;
        }
        if (strcmp(fn_name, "StartBriefing") == 0) return;
    }

    // The end of a mission (UIMissionSummary). Its factor panel builds first,
    // the whole table in one string (hq_summary_factors); then the screen's
    // OnInit sends the header -- result, operation, mission type, time,
    // place -- through Invoke("SetMissionInfo"), and ends with
    // AS_SetButtonHelp(CONTINUE, icon). Every piece went into a list and
    // nothing was said: the screen was silent. Gathered, and said whole at
    // the button, which is last. Only the factor page is ever shown -- in
    // both builds XGSummaryUI.OnNextView does nothing from view 0 and
    // OnUnrealCommand never pages -- and the header's rating and influence
    // are never filled, so arrive empty.
    if (strncmp(obj_name, "UIMissionSummary", 16) == 0) {
        static char s_factors[1024], s_head[512];
        if (strncmp(obj_name, "UIMissionSummary_Factors", 24) == 0) {
            if (strcmp(fn_name, "SetData") == 0) {
                const char* raw = "";
                for (int i = 0; i < p->nstrings; i++)
                    if (strchr(p->strings[i], ',') && strlen(p->strings[i]) > strlen(raw))
                        raw = p->strings[i];
                hq_summary_factors(raw, s_factors, sizeof s_factors);
                logf_("[%ld] %s %s.%s  SUMMARY factors \"%s\"\n", n, tag, obj_name,
                      fn_name, s_factors);
            }
            return;
        }
        // The unshown pages and the ticker: UIMissionSummary_Artifacts_0 and
        // the rest. The screen itself is UIMissionSummary_0 -- a digit after
        // the underscore -- and must not be caught here.
        if (obj_name[16] == '_' && !(obj_name[17] >= '0' && obj_name[17] <= '9')) return;
        if (strcmp(fn_name, "OnInit") == 0 && p->nstrings) {
            g_msum_screen = object;
            size_t used = 0;
            s_head[0] = 0;
            for (int i = 0; i < p->nstrings; i++) {
                int dup = 0;
                for (int j = 0; j < i; j++)
                    if (strcmp(p->strings[i], p->strings[j]) == 0) dup = 1;
                if (dup || looks_like_asset(p->strings[i])) continue;
                // "Mission Completed!" carries its own stop.
                const char* sep = !used ? ""
                                : strchr(".!?", s_head[used - 1]) ? " " : ". ";
                int w = _snprintf_s(s_head + used, sizeof s_head - used, _TRUNCATE, "%s%s",
                                    sep, p->strings[i]);
                if (w < 0) break;
                used += (size_t)w;
            }
            return;
        }
        if (strcmp(fn_name, "AS_SetButtonHelp") == 0) {
            const char* label = "";
            const char* icon = "";
            for (int i = 0; i < p->nstrings; i++) {
                if (strncmp(p->strings[i], "Icon_", 5) == 0) { if (!*icon) icon = p->strings[i]; }
                else if (!*label) label = p->strings[i];
            }
            if (*label) help_set(object, 0, label, *icon ? icon : "Icon_A_X", 0);
            char say[2048];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s", s_head,
                        s_head[0] && s_factors[0] ? ". " : "", s_factors,
                        *label ? ". Enter: " : "", label);
            s_head[0] = s_factors[0] = 0;
            logf_("[%ld] %s %s.%s  SUMMARY \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            announce(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetMissionStatus") == 0) return;
    }

    // The debrief back at the base (UIDebrief): one screen, a page at a time
    // -- the soldiers, then the science (research and artifacts), and after
    // other missions the council's report or a covert operative. Every row
    // went into a list the cursor never visits, so the only thing heard was
    // the last soldier's slot changing; the loot's quantity is an int
    // parameter and was dropped altogether. Each page is gathered from its
    // setters and said whole at its AS_Show*Debrief, which comes last. By
    // position (frame_args), the same in EU and EW:
    //     AS_SetTitles(debrief, operation, soldierTitle, scienceTitle,
    //                  councilTitle, covertTitle, covertSubTitle)
    //     AS_SetLabels(kills, missions, active, wounded, days, kia, continue, ...)
    //     AS_SetSoldier(int slot, portrait, flag, rank, class, name, nick,
    //                   int kills, int killsThisMission, int missions,
    //                   int promoteRank, promoteText, classPromoteText, status,
    //                   bool isDead, bool psiPromoted)
    //     AS_SetShiv(int slot, name, int kills, int killsThisMission,
    //                int missions, bool isAlive, status, rankIcon)
    //     AS_SetCovertSoldier -- AS_SetSoldier without the slot
    //     AS_AddListHeader(int id, text)
    //     AS_AddScienceResearch(int id, title, description, image)
    //     AS_AddScienceItem(int id, description, int amount, image)
    //     AS_SetCouncilInfo(text, rewards, panic)
    //     AS_SetCovertInfo(bool success, feedback, clue)
    // Up and Down pick among the promoted soldiers (AS_SetSoldierSelection)
    // on the soldier page and scroll the science page (AS_ScrollUp/Down);
    // both are followed and the line under them said.
    if (strncmp(obj_name, "UIDebrief", 9) == 0) {
        static char s_op[128], s_title[5][128], s_continue[64] = "CONTINUE";
        static char s_page[3072], s_council[1536], s_covert[1536];
        static char s_lines[32][384];
        static int  s_nlines, s_line = -1, s_promoted, s_building;
        static FrameArgs a;
        char row[FOCUS_MAX_LABEL];

        if (strcmp(fn_name, "AS_SetTitles") == 0) {
            frame_args(node, locals, &a);
            strncpy_s(s_op, sizeof s_op, a.ns > 1 ? a.s[1] : "", _TRUNCATE);
            for (int i = 0; i < 5; i++)
                strncpy_s(s_title[i], sizeof s_title[i], a.ns > i + 2 ? a.s[i + 2] : "",
                          _TRUNCATE);
            return;
        }
        if (strcmp(fn_name, "AS_SetLabels") == 0) {
            frame_args(node, locals, &a);
            if (a.ns > 6 && a.s[6][0])
                strncpy_s(s_continue, sizeof s_continue, a.s[6], _TRUNCATE);
            return;
        }
        int soldier = strcmp(fn_name, "AS_SetSoldier") == 0;
        int covert = strcmp(fn_name, "AS_SetCovertSoldier") == 0;
        int shiv = strcmp(fn_name, "AS_SetShiv") == 0;
        if (soldier || covert || shiv) {
            frame_args(node, locals, &a);
            int k = covert ? 0 : 1;               // the numbers after the slot
            int kills = p->nnumbers > k ? (int)p->numbers[k] : 0;
            int missions = p->nnumbers > k + 2 ? (int)p->numbers[k + 2] : 0;
            const char *name, *nick = "", *cls = "", *status, *promo = "", *cpromo = "";
            if (shiv) {
                name = a.ns > 0 ? a.s[0] : "";
                status = a.ns > 1 ? a.s[1] : "";
            } else {
                cls = a.ns > 3 ? a.s[3] : "";
                name = a.ns > 4 ? a.s[4] : "";
                nick = a.ns > 5 ? a.s[5] : "";
                promo = a.ns > 6 ? a.s[6] : "";
                cpromo = a.ns > 7 ? a.s[7] : "";
                status = a.ns > 8 ? a.s[8] : "";
            }
            // The class is an icon name, "heavy" or "none"; it is left out
            // when the promotion already names it ("Class Assigned: Sniper").
            char cls_word[32] = "";
            if (*cls && strcmp(cls, "none") != 0 && !*cpromo) {
                strncpy_s(cls_word, sizeof cls_word, cls, _TRUNCATE);
                cls_word[0] = (char)toupper((unsigned char)cls_word[0]);
            }
            // The name already carries the nickname ("Cpl. Christophe 'D.O.A.'
            // Leroy"). The nickname slot is not one: XGDebriefUI fills it only
            // when a nickname was just earned, with m_strEarnedNickName --
            // "Earned Nickname: 'D.O.A.'" -- so it is news, said with the
            // promotions, not quoted after the name as it was.
            _snprintf_s(row, sizeof row, _TRUNCATE,
                        "%s%s%s, %s%s%d kill%s, %d mission%s%s%s%s%s%s%s", name,
                        *cls_word ? ", " : "", cls_word, status, *status ? ", " : "",
                        kills, kills == 1 ? "" : "s", missions, missions == 1 ? "" : "s",
                        *promo ? ". " : "", promo, *cpromo ? ". " : "", cpromo,
                        *nick ? ". " : "", nick);
            if (*promo) s_promoted = 1;
            if (covert) {
                strncpy_s(s_covert, sizeof s_covert, row, _TRUNCATE);
            } else {
                if (!s_building) { s_building = 1; s_page[0] = 0; s_promoted = *promo != 0; }
                int slot = p->nnumbers ? (int)p->numbers[0] : 0;
                focus_set(object, slot, row);
                size_t used = strlen(s_page);
                _snprintf_s(s_page + used, sizeof s_page - used, _TRUNCATE, "%s%s",
                            used ? ". " : "", row);
            }
            logf_("[%ld] %s %s.%s  DEBRIEF soldier \"%s\"\n", n, tag, obj_name, fn_name, row);
            return;
        }
        if (strcmp(fn_name, "AS_SetSoldierSelection") == 0) {
            int idx = p->nnumbers ? (int)p->numbers[0] : -1;
            // Sent while the page builds too, before anything is said.
            if (idx >= 0 && !s_building && focus_label_at(object, idx, row, sizeof row)) {
                logf_("[%ld] %s %s.%s  DEBRIEF selected %d \"%s\"\n", n, tag, obj_name,
                      fn_name, idx, row);
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say_now(row);
            }
            return;
        }
        if (strcmp(fn_name, "AS_AddListHeader") == 0 ||
            strcmp(fn_name, "AS_AddScienceResearch") == 0 ||
            strcmp(fn_name, "AS_AddScienceItem") == 0) {
            frame_args(node, locals, &a);
            int id = p->nnumbers ? (int)p->numbers[0] : s_nlines;
            if (id == 0) s_nlines = 0;
            if (fn_name[6] == 'L')
                strncpy_s(row, sizeof row, a.ns ? a.s[0] : "", _TRUNCATE);
            else if (fn_name[13] == 'R')
                _snprintf_s(row, sizeof row, _TRUNCATE, "%s%s%s", a.ns ? a.s[0] : "",
                            a.ns > 1 && a.s[1][0] ? ": " : "", a.ns > 1 ? a.s[1] : "");
            else
                _snprintf_s(row, sizeof row, _TRUNCATE, "%s, %d", a.ns ? a.s[0] : "",
                            p->nnumbers > 1 ? (int)p->numbers[1] : 0);
            if (id >= 0 && id < 32) {
                strncpy_s(s_lines[id], sizeof s_lines[id], row, _TRUNCATE);
                if (id >= s_nlines) s_nlines = id + 1;
            }
            logf_("[%ld] %s %s.%s  DEBRIEF line %d \"%s\"\n", n, tag, obj_name, fn_name,
                  id, row);
            return;
        }
        if (strcmp(fn_name, "AS_SetCouncilInfo") == 0) {
            frame_args(node, locals, &a);
            _snprintf_s(s_council, sizeof s_council, _TRUNCATE, "%s%s%s%s%s",
                        a.ns ? a.s[0] : "", a.ns > 1 && a.s[1][0] ? ". " : "",
                        a.ns > 1 ? a.s[1] : "", a.ns > 2 && a.s[2][0] ? ". " : "",
                        a.ns > 2 ? a.s[2] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetCovertInfo") == 0) {
            frame_args(node, locals, &a);
            size_t used = strlen(s_covert);
            _snprintf_s(s_covert + used, sizeof s_covert - used, _TRUNCATE, "%s%s%s%s",
                        a.ns && a.s[0][0] ? ". " : "", a.ns ? a.s[0] : "",
                        a.ns > 1 && a.s[1][0] ? ". " : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_ScrollUp") == 0 || strcmp(fn_name, "AS_ScrollDown") == 0) {
            if (!s_nlines) return;
            int down = fn_name[9] == 'D';
            s_line += down ? 1 : -1;
            if (s_line < 0) s_line = 0;
            if (s_line >= s_nlines) s_line = s_nlines - 1;
            const char* say = s_lines[s_line][0] ? s_lines[s_line] : "blank";
            logf_("[%ld] %s %s.%s  DEBRIEF line %d -> \"%s\"\n", n, tag, obj_name, fn_name,
                  s_line, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strncmp(fn_name, "AS_Show", 7) == 0 && strstr(fn_name, "Debrief")) {
            char say[4096];
            const char* title = "";
            const char* body = "";
            char science[3072];
            science[0] = 0;
            help_clear(object);
            help_set(object, 0, s_continue, "Icon_A_X", 0);
            if (strstr(fn_name, "Soldier")) {
                title = s_title[0];
                body = s_page;
                s_building = 0;
                if (s_promoted) help_set(object, 1, "PROMOTE", "Icon_Y_TRIANGLE", 0);
            } else if (strstr(fn_name, "Science")) {
                title = s_title[1];
                for (int i = 0; i < s_nlines; i++) {
                    if (!s_lines[i][0]) continue;
                    size_t used = strlen(science);
                    // A header ends in a colon and leads its items.
                    const char* sep = !used ? "" : science[used - 1] == ':' ? " " : ". ";
                    _snprintf_s(science + used, sizeof science - used, _TRUNCATE, "%s%s",
                                sep, s_lines[i]);
                }
                body = science;
                s_line = -1;
            } else if (strstr(fn_name, "Council")) {
                title = s_title[2];
                body = s_council;
            } else if (strstr(fn_name, "Covert")) {
                title = s_title[3];
                body = s_covert;
            }
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s%s%s%s", s_op,
                        s_op[0] && title[0] ? ". " : "", title,
                        (s_op[0] || title[0]) && body[0] ? ". " : "", body,
                        s_promoted && strstr(fn_name, "Soldier")
                            ? ". Up and Down pick a promoted soldier, 1 promotes" : "",
                        ". Enter: ", s_continue);
            // The science page scrolls on the arrows and a soldier page with
            // a promotion moves between the promoted; everywhere else they do
            // nothing (UIDebrief.OnPressUp/Down), so they say the page again.
            strncpy_s(g_debrief_page, sizeof g_debrief_page, say, _TRUNCATE);
            g_debrief_rereads = !strstr(fn_name, "Science") &&
                                !(strstr(fn_name, "Soldier") && s_promoted);
            logf_("[%ld] %s %s.%s  DEBRIEF \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            announce(say);
            return;
        }
    }

    // The end-of-month report. XGWorldReportUI has three views, and both
    // screens are up for all of them:
    //   0  UIWorldReport: AS_SetDecryptingText("Transmitting encrypted
    //      data...", "Transmission Decoded!") at OnInit;
    //   1  UIWorldReport: AS_SetText(the countries that have withdrawn),
    //      only when some have;
    //   2  UIEndOfMonthReport.UpdateData, all in one pass, then Show():
    //        AS_UpdateHeader(Title, Desc, rewards, gradeLabel, grade)
    //        AS_UpdateBar(int cont, continentName, rewards, bonus, int, bool withdrawn)
    //        AS_UpdateCountry(int cont, int country, countryName, int panicLevel,
    //                         satelliteinfo)
    // Enter, Space or A advances each (UIWorldReport.OnUnrealCommand ->
    // OnAdvance); nothing else does anything. On the general path every
    // string was filed as a slot and nothing was said (log of 2026-09-26).
    // panicLevel is GetPanicBlocks, 1 to 5, the bars the screen draws; -1 for
    // a country that has withdrawn, whose satelliteinfo says so.
    if (strncmp(obj_name, "UIWorldReport", 13) == 0) {
        if (strcmp(fn_name, "AS_SetDecryptingText") == 0) {
            char status[EOM_TEXT], ready[EOM_TEXT];
            frame_string(node, locals, 0, status, sizeof status);
            frame_string(node, locals, 1, ready, sizeof ready);
            g_eom_n = g_eom_head = g_eom_said = 0;      // a new report
            g_eom_link = 1;
            g_eom_at = -1;
            _snprintf_s(g_eom_page, sizeof g_eom_page, _TRUNCATE, "%s%s%s Enter: Next.",
                        status, status[0] && ready[0] ? " " : "", ready);
            logf_("[%ld] %s %s.%s  REPORT \"%s\"\n", n, tag, obj_name, fn_name, g_eom_page);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(g_eom_page);
            return;
        }
        if (strcmp(fn_name, "AS_SetText") == 0) {
            char t[EOM_TEXT];
            frame_string(node, locals, 0, t, sizeof t);
            // GoToView(0) sends the link status here too, before OnInit
            // sends it again with "decoded"; only the defections are news.
            if (!t[0] || !g_eom_link || strncmp(g_eom_page, t, strlen(t)) == 0) return;
            _snprintf_s(g_eom_page, sizeof g_eom_page, _TRUNCATE, "%s Enter: Next.", t);
            logf_("[%ld] %s %s.%s  REPORT \"%s\"\n", n, tag, obj_name, fn_name, g_eom_page);
            history_add(t);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(g_eom_page);
            return;
        }
        if (strcmp(fn_name, "AS_HideDecrypting") == 0) return;
    }
    if (strncmp(obj_name, "UIEndOfMonthReport", 18) == 0) {
        if (strcmp(fn_name, "AS_UpdateHeader") == 0) {
            char title[EOM_TEXT], act[EOM_TEXT], label[64], grade[64], raw[EOM_TEXT];
            frame_string(node, locals, 0, title, sizeof title);
            frame_string(node, locals, 1, act, sizeof act);
            frame_string(node, locals, 3, label, sizeof label);
            frame_string(node, locals, 4, grade, sizeof grade);
            g_eom_n = 0;
            g_eom_said = 0;
            _snprintf_s(g_eom[g_eom_n++], EOM_TEXT, _TRUNCATE, "%s%s%s%s%s.", title,
                        grade[0] ? ". " : "", label, label[0] && grade[0] ? ": " : "", grade);
            if (act[0]) _snprintf_s(g_eom[g_eom_n++], EOM_TEXT, _TRUNCATE, "%s.", act);
            // Funding and specialists, one line each: "\n" between them.
            if (!frame_local_raw(node, locals, "rewards", raw, sizeof raw))
                frame_string(node, locals, 2, raw, sizeof raw);
            for (char* part = raw; part && *part && g_eom_n < EOM_LINES; ) {
                char* nl = strchr(part, '\n');
                if (nl) *nl = 0;
                strip_markup(part);
                if (part[0]) _snprintf_s(g_eom[g_eom_n++], EOM_TEXT, _TRUNCATE, "%s.", part);
                part = nl ? nl + 1 : NULL;
            }
            g_eom_head = g_eom_n;
            return;
        }
        if (strcmp(fn_name, "AS_UpdateBar") == 0) {
            if (g_eom_n >= EOM_LINES) return;
            char name[128], rewards[256], bonus[256];
            frame_string(node, locals, 0, name, sizeof name);
            frame_string(node, locals, 1, rewards, sizeof rewards);
            frame_string(node, locals, 2, bonus, sizeof bonus);
            // The bonus comes in quotes: "Expert Knowledge".
            char* b = bonus;
            size_t bl = strlen(b);
            if (bl >= 2 && b[0] == '"' && b[bl - 1] == '"') { b[bl - 1] = 0; b++; }
            int withdrawn = p->nbools && p->bools[0];
            _snprintf_s(g_eom[g_eom_n++], EOM_TEXT, _TRUNCATE, "%s%s%s%s%s%s.", name,
                        rewards[0] ? ", " : "", rewards, b[0] ? ", " : "", b,
                        withdrawn ? ", withdrawn" : "");
            return;
        }
        if (strcmp(fn_name, "AS_UpdateCountry") == 0) {
            if (g_eom_n >= EOM_LINES) return;
            char name[128], info[128], panic[32] = "";
            frame_string(node, locals, 0, name, sizeof name);
            frame_string(node, locals, 1, info, sizeof info);
            int blocks = p->nnumbers >= 3 ? (int)p->numbers[2] : -1;
            if (blocks > 0) _snprintf_s(panic, sizeof panic, _TRUNCATE, ", panic %d of 5", blocks);
            _snprintf_s(g_eom[g_eom_n++], EOM_TEXT, _TRUNCATE, "%s%s%s%s.", name,
                        info[0] ? ", " : "", info, panic);
            return;
        }
        // UpdateData's last step is Show(); the earlier Show, at OnInit,
        // comes with nothing kept yet.
        if (strcmp(fn_name, "Show") == 0 && g_eom_n && !g_eom_said) {
            g_eom_said = 1;
            g_eom_link = 0;
            g_eom_at = g_eom_head - 1;      // Down starts at the first continent
            static char say[EOM_LINES * EOM_TEXT / 4];
            say[0] = 0;
            size_t used = 0;
            for (int i = 0; i < g_eom_head; i++) {
                int w = _snprintf_s(say + used, sizeof say - used, _TRUNCATE, "%s%s",
                                    used ? " " : "", g_eom[i]);
                if (w < 0) break;
                used += (size_t)w;
            }
            history_add(say);
            if (used < sizeof say)
                _snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                            " Up and Down read the continents and countries. Enter: Carry On.");
            for (int i = 0; i < g_eom_n; i++)
                logf_("[%ld] %s %s.%s  REPORT line %d \"%s\"\n", n, tag, obj_name, fn_name,
                      i, g_eom[i]);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The hangar's ship list (UIShipList). UpdateData sends Invoke("ClearAll")
    // -- logged under UpdateData, the caller -- then per continent its ships,
    //     AS_AddShip(int cont, shipName, WeaponType, Status, Help, int State)
    // (State -1 is the empty slot), its orders,
    //     AS_AddPendingShip(int cont, infoTxt, statusTxt, Help, int ShipType)
    // and last its title, AS_SetContinentTitle(int cont, "Europe (2/4)"). A
    // move is AS_SetSelection(int cont, int row), which nothing resolved: the
    // rows were filed as slots of one list, each continent over the last.
    // Keys (OnUnrealCommand): Up/Down move, and past a continent's last row
    // into the next; Enter opens a ship, orders on the empty slot, cancels on
    // an order; X transfers a ready ship (AS_InitializeShipTransfer, then the
    // arrows pick a hangar and Enter confirms); F1 is the item card; Escape.
    if (strncmp(obj_name, "UIShipList", 10) == 0) {
        #define SHIP_CONTS 8
        #define SHIP_ROWS  8
        static char s_row[SHIP_CONTS][SHIP_ROWS][FOCUS_MAX_LABEL];
        static int  s_nrow[SHIP_CONTS];
        static char s_cont[SHIP_CONTS][96];
        static int  s_said_cont = -1, s_fresh;
        static FrameArgs a;
        if (strcmp(fn_name, "UpdateData") == 0) {          // Invoke("ClearAll")
            memset(s_nrow, 0, sizeof s_nrow);
            s_fresh = 1;
            s_said_cont = -1;
            return;
        }
        int add = strcmp(fn_name, "AS_AddShip") == 0;
        if (add || strcmp(fn_name, "AS_AddPendingShip") == 0) {
            int c = p->nnumbers ? (int)p->numbers[0] : -1;
            if (c < 0 || c >= SHIP_CONTS || s_nrow[c] >= SHIP_ROWS) return;
            frame_args(node, locals, &a);
            for (int i = 0; i < a.ns; i++) strip_markup(a.s[i]);
            char* row = s_row[c][s_nrow[c]++];
            int empty = add && p->nnumbers >= 2 && (int)p->numbers[1] == -1;
            if (empty)
                _snprintf_s(row, FOCUS_MAX_LABEL, _TRUNCATE, "Empty slot");
            else if (add)       // name, weapon, status
                _snprintf_s(row, FOCUS_MAX_LABEL, _TRUNCATE, "%s%s%s%s%s", a.s[0],
                            a.ns > 1 && a.s[1][0] ? ", " : "", a.ns > 1 ? a.s[1] : "",
                            a.ns > 2 && a.s[2][0] ? ", " : "", a.ns > 2 ? a.s[2] : "");
            else                // "Interceptor Purchase", "Ready in 3 day(s)"
                _snprintf_s(row, FOCUS_MAX_LABEL, _TRUNCATE, "%s%s%s", a.s[0],
                            a.ns > 1 && a.s[1][0] ? ", " : "", a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetContinentTitle") == 0) {
            int c = p->nnumbers ? (int)p->numbers[0] : -1;
            if (c < 0 || c >= SHIP_CONTS) return;
            frame_string(node, locals, 0, s_cont[c], sizeof s_cont[c]);
            return;
        }
        if (strcmp(fn_name, "AS_InitializeShipTransfer") == 0) {
            const char* say = "Transfer. Up and Down choose a hangar, Enter transfers there, "
                              "Escape cancels.";
            logf_("[%ld] %s %s.%s  SHIPS \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            s_said_cont = -1;                   // the hangar's name goes with the first move
            return;
        }
        if (strcmp(fn_name, "AS_SetSelection") == 0 && p->nnumbers >= 2) {
            int c = (int)p->numbers[0], r = (int)p->numbers[1];
            if (c < 0 || c >= SHIP_CONTS || r < 0 || r >= s_nrow[c]) return;
            char say[FOCUS_MAX_LABEL * 3];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s.%s",
                        s_fresh ? "Ship list. " : "",
                        c != s_said_cont ? s_cont[c] : "", c != s_said_cont ? ". " : "",
                        s_row[c][r],
                        s_fresh ? " Enter opens a ship, 1 transfers it, F1 for more "
                                  "information." : "");
            s_said_cont = c;
            s_fresh = 0;
            logf_("[%ld] %s %s.%s  SHIP %d, %d \"%s\"\n", n, tag, obj_name, fn_name, c, r, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // One ship (UIShipSummary). UpdateData sends AS_SetShipName,
    // AS_SetWeaponLabel, AS_SetWeaponName, AS_SetShipStatus, AS_SetKills,
    // AS_SetWeaponImage; UpdateButtonHelp the two buttons,
    //     AS_SetWeaponHelp(int i, label, icon, bool IsDisabled)
    // -- EDIT LOADOUT (not while the ship is busy), DISMISS SHIP -- and a move
    // is AS_SetWeaponButtonFocus(int i, bool focused): off the old, on the
    // new. Up and Down wrap between the two, Enter presses one, F1 is the
    // weapon's card. The selection starts at -1 with a mouse about, and Enter
    // there presses Dismiss (behind a dialogue), so the arrival says to pick.
    if (strncmp(obj_name, "UIShipSummary", 13) == 0) {
        static char s_name[96], s_wlabel[64], s_weapon[128], s_status[96], s_kills[96];
        char (*s_btn)[FOCUS_MAX_LABEL] = g_ship_btn;
        int* s_off = g_ship_btn_off;
        char* slot = strcmp(fn_name, "AS_SetShipName") == 0 ? s_name
                   : strcmp(fn_name, "AS_SetWeaponLabel") == 0 ? s_wlabel
                   : strcmp(fn_name, "AS_SetWeaponName") == 0 ? s_weapon
                   : strcmp(fn_name, "AS_SetShipStatus") == 0 ? s_status
                   : strcmp(fn_name, "AS_SetKills") == 0 ? s_kills : NULL;
        if (slot) {
            size_t sz = slot == s_name ? sizeof s_name : slot == s_wlabel ? sizeof s_wlabel
                      : slot == s_weapon ? sizeof s_weapon : slot == s_status ? sizeof s_status
                      : sizeof s_kills;
            frame_string(node, locals, 0, slot, sz);
            if (slot != s_kills) return;
            // The kills are the last line UpdateData writes that says anything.
            char btns[2 * FOCUS_MAX_LABEL + 64] = "";
            for (int i = 0; i < 2; i++) {
                if (!s_btn[i][0]) continue;
                size_t u = strlen(btns);
                _snprintf_s(btns + u, sizeof btns - u, _TRUNCATE, "%s%s%s",
                            u ? ", " : " Buttons: ", s_btn[i], s_off[i] ? ", unavailable" : "");
            }
            char say[1024];
            _snprintf_s(say, sizeof say, _TRUNCATE,
                        "%s. %s %s. %s. %s.%s%s Up and Down choose, Enter presses.",
                        s_name, s_wlabel, s_weapon, s_status, s_kills, btns,
                        btns[0] ? "." : "");
            logf_("[%ld] %s %s.%s  SHIP \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetWeaponButtonFocus") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (!p->nbools || !p->bools[0] || i < 0 || i >= 2 || !s_btn[i][0]) return;
            char say[FOCUS_MAX_LABEL + 16];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s.", s_btn[i],
                        s_off[i] ? ", unavailable" : "");
            logf_("[%ld] %s %s.%s  SHIP button %d \"%s\"\n", n, tag, obj_name, fn_name, i, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetWeaponImage") == 0) return;
    }

    // A ship's weapons (UIShipLoadout, EDIT LOADOUT). OnInit: AS_SetTitle,
    // AS_SetListLabels(weaponLabel, quantityLabel); UpdateData: Invoke("clear")
    // and AS_AddWeapon(name, count, bool Disabled) per weapon; each selection
    // (RealizeSelected): AS_SetStatData(i, label, value) x5 -- hit chance,
    // range, fire rate, damage, armour penetration -- AS_SetSelected(i),
    // AS_SetWeaponName, AS_SetWeaponImage, AS_SetWeaponDescription last.
    // Up/Down move, Enter equips (a dialogue confirms; a disabled one only
    // plays the bad sound), F1 the card, Escape.
    if (strncmp(obj_name, "UIShipLoadout", 13) == 0) {
        #define LOADOUT_ROWS 12
        static char s_title[96], s_qty[48];
        static char s_row[LOADOUT_ROWS][FOCUS_MAX_LABEL];
        static int  s_n, s_sel = -1, s_fresh;
        static char s_stat[5][96];
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetTitle") == 0) {
            frame_string(node, locals, 0, s_title, sizeof s_title);
            return;
        }
        if (strcmp(fn_name, "AS_SetListLabels") == 0) {
            frame_string(node, locals, 1, s_qty, sizeof s_qty);
            return;
        }
        if (strcmp(fn_name, "UpdateData") == 0) {          // Invoke("clear")
            s_n = 0;
            s_fresh = 1;
            return;
        }
        if (strcmp(fn_name, "AS_AddWeapon") == 0) {
            if (s_n >= LOADOUT_ROWS) return;
            frame_args(node, locals, &a);
            int off = p->nbools && p->bools[0];
            _snprintf_s(s_row[s_n++], FOCUS_MAX_LABEL, _TRUNCATE, "%s%s%s%s%s%s", a.s[0],
                        a.ns > 1 && a.s[1][0] ? ", " : "", s_qty[0] && a.ns > 1 && a.s[1][0] ? s_qty : "",
                        s_qty[0] && a.ns > 1 && a.s[1][0] ? " " : "", a.ns > 1 ? a.s[1] : "",
                        off ? ", unavailable" : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetStatData") == 0 && p->nnumbers) {
            int i = (int)p->numbers[0];
            if (i < 0 || i >= 5) return;
            frame_args(node, locals, &a);
            _snprintf_s(s_stat[i], sizeof s_stat[i], _TRUNCATE, "%s %s", a.s[0],
                        a.ns > 1 ? a.s[1] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetSelected") == 0 && p->nnumbers) {
            s_sel = (int)p->numbers[0];
            return;
        }
        if (strcmp(fn_name, "AS_SetWeaponName") == 0 || strcmp(fn_name, "AS_SetWeaponImage") == 0)
            return;
        if (strcmp(fn_name, "AS_SetWeaponDescription") == 0) {
            char desc[1024];
            frame_string(node, locals, 0, desc, sizeof desc);
            char say[2048];
            _snprintf_s(say, sizeof say, _TRUNCATE,
                        "%s%s%s. %s. %s. %s. %s. %s. %s%s", s_fresh ? s_title : "",
                        s_fresh && s_title[0] ? ". " : "",
                        s_sel >= 0 && s_sel < s_n ? s_row[s_sel] : "?",
                        s_stat[0], s_stat[1], s_stat[2], s_stat[3], s_stat[4], desc,
                        s_fresh ? " Enter equips." : "");
            s_fresh = 0;
            logf_("[%ld] %s %s.%s  LOADOUT \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
    }

    // The squad for a mission. See hq_squad_row. Each slot is either a
    // soldier (AS_SetUnitInfo with a status) or empty (status -1, then
    // AS_SetAddUnitText with "ADD UNIT" and a "+", or the Officer Training
    // School hint for a slot not yet bought). On the general path a soldier
    // read "RK. WHITE, none" -- rank abbreviated, the class's icon name, and
    // no gear.
    if (strncmp(obj_name, "UISquadSelect_SquadList", 23) == 0) {
        static FrameArgs a;
        if (strcmp(fn_name, "AS_SetUnitInfo") == 0 && p->nnumbers >= 2) {
            int idx = (int)p->numbers[0];
            if ((int)p->numbers[1] == -1) return;          // empty: the add text names it
            frame_args(node, locals, &a);
            char row[FOCUS_MAX_LABEL];
            hq_squad_row(a.s[0], a.ns > 1 ? a.s[1] : "", a.ns > 2 ? a.s[2] : "",
                         a.ns > 4 ? a.s[4] : "", a.ns > 5 ? a.s[5] : "",
                         a.ns > 6 ? a.s[6] : "", row, sizeof row);
            focus_set(object, idx, row);
            logf_("[%ld] %s %s.%s  SQUAD %d = \"%s\"\n", n, tag, obj_name, fn_name, idx, row);
            return;
        }
        if (strcmp(fn_name, "AS_SetAddUnitText") == 0 && p->nnumbers) {
            frame_args(node, locals, &a);
            if (!a.ns || !a.s[0][0]) return;               // a soldier's slot
            char row[FOCUS_MAX_LABEL];
            int add = a.ns > 1 && strcmp(a.s[1], "+") == 0;
            _snprintf_s(row, sizeof row, _TRUNCATE, "%s%s", add ? "Empty slot, " : "Locked: ",
                        a.s[0]);
            focus_set(object, (int)p->numbers[0], row);
            logf_("[%ld] %s %s.%s  SQUAD %d = \"%s\"\n", n, tag, obj_name, fn_name,
                  (int)p->numbers[0], row);
            return;
        }
        // (icon0, EDIT UNIT, icon1, CLEAR UNIT), into 0's list.
        //
        // Sent once, at OnInit. Back from a soldier, OnReceiveFocus redraws
        // the slots (UpdateDisplay) and the screen's bar (UpdateButtonHelp)
        // but not this, so 0 -- which lists only bars published within
        // HELP_WINDOW_MS of the newest -- had dropped Edit and Clear unit
        // (log of 2026-09-25, 23:22: "BACK TO BRIEFING. MAKE ITEMS
        // AVAILABLE. LAUNCH MISSION", and no way to hear that 2 clears a
        // slot). The pair is kept and published again with each redraw.
        static void* help_obj;
        static char help_s[4][FOCUS_MAX_LABEL];
        int sent = strcmp(fn_name, "AS_SetUnitHelp") == 0;
        if (sent) {
            frame_args(node, locals, &a);
            help_obj = object;
            for (int i = 0; i < 4; i++)
                strncpy_s(help_s[i], sizeof help_s[i], i < a.ns ? a.s[i] : "", _TRUNCATE);
            logf_("[%ld] %s %s.%s  SQUAD help \"%s\" on %s, \"%s\" on %s\n", n, tag, obj_name,
                  fn_name, help_s[1], help_s[0], help_s[3], help_s[2]);
        }
        if (sent || (strcmp(fn_name, "UpdateDisplay") == 0 && object == help_obj)) {
            if (help_s[1][0]) help_set(object, 0, help_s[1], help_s[0], 0);
            if (help_s[3][0]) help_set(object, 1, help_s[3], help_s[2], 0);
        }
        if (sent) return;
    }

    // The promotion tree. See hq.h: the grid is kept from its per-rank
    // calls, and each move is said from the description that ends it. On
    // the general path each icon name ("FireRocket", "unknown") was filed as
    // a label and every move read a rank or an icon name.
    if (strncmp(obj_name, "UISoldierPromotion", 18) == 0) {
        static FrameArgs a;
        if (strcmp(fn_name, "AS_InitializeTree") == 0) {
            frame_args(node, locals, &a);
            hq_promo_reset(a.ns ? a.s[0] : "");
            logf_("[%ld] %s %s.%s  PROMOTION tree \"%s\"\n", n, tag, obj_name, fn_name,
                  a.ns ? a.s[0] : "");
            return;
        }
        if (strcmp(fn_name, "AS_SetAbilityIcon") == 0 && p->nnumbers >= 2) {
            frame_args(node, locals, &a);
            hq_promo_icon((int)p->numbers[0], (int)p->numbers[1], a.ns ? a.s[0] : "",
                          a.nb && a.b[0]);
            return;
        }
        if (strcmp(fn_name, "AS_SetColumnData") == 0 && p->nnumbers >= 2) {
            frame_args(node, locals, &a);
            hq_promo_column((int)p->numbers[0], a.ns ? a.s[0] : "", (int)p->numbers[1]);
            return;
        }
        if (strcmp(fn_name, "AS_SetSelectedIcon") == 0 && p->nnumbers >= 2) {
            hq_promo_select((int)p->numbers[0], (int)p->numbers[1]);
            return;
        }
        if (strcmp(fn_name, "AS_SetAbilityDescription") == 0) {
            frame_args(node, locals, &a);
            char say[1400];
            hq_promo_describe(a.ns ? a.s[0] : "", a.ns > 1 ? a.s[1] : "", say, sizeof say);
            logf_("[%ld] %s %s.%s  PROMOTION \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
            return;
        }
        if (strcmp(fn_name, "AS_SetSoldierStats") == 0) return;
    }

    // A facility submenu's line of help for the option under the cursor,
    // sent by RealizeSelected just before it moves the focus:
    //
    //     UIStrategyHUD_FacilitySubMenu.AS_SetHelpText(string displayString)
    //
    // Kept as that option's panel, said after its label. Declared on that
    // class alone, in both builds.
    if ((strcmp(fn_name, "AS_SetHelpText") == 0 ||
         strcmp(fn_name, "AS_SetDescription") == 0) && p->nstrings <= 1 && !p->nnumbers) {
        panel_note(n, tag, obj_name, fn_name, object, p->nstrings ? p->strings[0] : "");
        return;
    }

    // Who a soldier screen is about. UIStrategyComponent_SoldierInfo and
    // _SoldierStats draw the header over the soldier summary, promotion and
    // loadout screens, and nothing said it: several strings and no index, so
    // each went into a list of its own that no key ever read. Kept, and given
    // to the soldier summary as its heading when it builds, so opening a
    // soldier says who it is before the menu. By position (frame_args):
    //     (_name, _nickname, _status, _flagIcon, _classLabel, _classText,
    //      _rankLabel, _rankText, bool _showPromoteIcon, _missions, _kills)
    //     (_health, _will, _defense, _offense)
    // the same in both builds.
    if (strncmp(obj_name, "UIStrategyComponent_Soldier", 27) == 0) {
        static FrameArgs a;
        frame_args(node, locals, &a);
        if (strcmp(fn_name, "AS_SetSoldierInformation") == 0 && a.ns >= 10) {
            char who[160], quoted[96];
            hq_nick_quoted(a.s[1], quoted, sizeof quoted);   // sent already quoted
            if (quoted[0])
                _snprintf_s(who, sizeof who, _TRUNCATE, "%s %s %s", a.s[7], a.s[0], quoted);
            else
                _snprintf_s(who, sizeof who, _TRUNCATE, "%s %s", a.s[7], a.s[0]);
            _snprintf_s(g_soldier_info, sizeof g_soldier_info, _TRUNCATE,
                        "%s%s%s%s. %s. %s. %s", who, a.s[5][0] ? ", " : "", a.s[5],
                        a.nb && a.b[0] ? ", promotion" : "", a.s[2], a.s[8], a.s[9]);
            g_soldier_stats[0] = 0;
            g_soldier_info_at = GetTickCount64();
            logf_("[%ld] %s %s.%s  SOLDIER INFO \"%s\"\n", n, tag, obj_name, fn_name,
                  g_soldier_info);
            return;
        }
        // Between the two: the medals, as icons. Said with the soldier.
        if (strcmp(fn_name, "AS_SetMedals") == 0) {
            char line[256];
            medal_line(a.ns > 0 ? a.s[0] : "", line, sizeof line);
            if (line[0] && g_soldier_info[0]) {
                size_t u = strlen(g_soldier_info);
                _snprintf_s(g_soldier_info + u, sizeof g_soldier_info - u, _TRUNCATE, ". %s",
                            line);
            }
            logf_("[%ld] %s %s.%s  SOLDIER MEDALS \"%s\"\n", n, tag, obj_name, fn_name, line);
            return;
        }
        if (strcmp(fn_name, "AS_SetSoldierStats") == 0 && a.ns >= 4) {
            _snprintf_s(g_soldier_stats, sizeof g_soldier_stats, _TRUNCATE,
                        "%s. %s. %s. %s", a.s[0], a.s[1], a.s[2], a.s[3]);
            logf_("[%ld] %s %s.%s  SOLDIER STATS \"%s\"\n", n, tag, obj_name, fn_name,
                  g_soldier_stats);
            // The header after a summary refresh: said after the item, and
            // only when it is another soldier than the one last said.
            if (g_summary_obj && g_soldier_info[0] &&
                GetTickCount64() - g_summary_at < SUMMARY_HEADER_MS &&
                strcmp(g_soldier_info, g_summary_said) != 0) {
                char head[FOCUS_MAX_LABEL];
                _snprintf_s(head, sizeof head, _TRUNCATE, "%s. %s%s%s", g_soldier_info,
                            g_soldier_stats, g_summary_dropship[0] ? ". " : "",
                            g_summary_dropship);
                strncpy_s(g_summary_said, sizeof g_summary_said, g_soldier_info, _TRUNCATE);
                logf_("[%ld] %s %s.%s  SUMMARY soldier \"%s\"\n", n, tag, obj_name,
                      fn_name, head);
                if (g_speak && !muted()) speech_say(head);
            }
            return;
        }
    }
    // The soldier summary building: its heading is the soldier, when the
    // header came first (opening it) and is not the soldier already said.
    // A refresh finds the previous header here -- after Tab, the previous
    // soldier's -- and leaves it to the header that follows.
    if (strncmp(obj_name, "UISoldierSummary", 16) == 0 &&
        strcmp(fn_name, "UpdateData") == 0) {
        g_summary_obj = object;
        g_summary_at = GetTickCount64();
        g_summary_dropship[0] = 0;
        g_summary_titled = g_soldier_info[0] &&
                           GetTickCount64() - g_soldier_info_at < SOLDIER_INFO_FRESH_MS &&
                           strcmp(g_soldier_info, g_summary_said) != 0;
        if (g_summary_titled) {
            char head[FOCUS_MAX_LABEL];
            _snprintf_s(head, sizeof head, _TRUNCATE, "%s%s%s", g_soldier_info,
                        g_soldier_stats[0] ? ". " : "", g_soldier_stats);
            focus_set_title(object, head);
            strncpy_s(g_summary_said, sizeof g_summary_said, g_soldier_info, _TRUNCATE);
        }
    }
    // EW's "ON MISSION" marker, sent after the menu with one string (or
    // none). On the lone-line path it cleared Abilities, Loadout, Customize
    // and Dismiss and became item 0, so the first said "ON MISSION" and the
    // rest were unresolved. It belongs to the soldier, so it joins the
    // heading -- this pass's, or the header's still to come. Alone it said
    // "ON MISSION. LOADOUT" on the way back from Loadout.
    if (strncmp(obj_name, "UISoldierSummary", 16) == 0 &&
        strcmp(fn_name, "AS_SetInDropship") == 0) {
        const char* label = p->nstrings ? p->strings[0] : "";
        strncpy_s(g_summary_dropship, sizeof g_summary_dropship, label, _TRUNCATE);
        if (*label && g_summary_titled) {
            char head[FOCUS_MAX_LABEL];
            _snprintf_s(head, sizeof head, _TRUNCATE, "%s%s%s. %s", g_soldier_info,
                        g_soldier_stats[0] ? ". " : "", g_soldier_stats, label);
            focus_set_title(object, head);
        }
        logf_("[%ld] %s %s.%s  IN DROPSHIP \"%s\"\n", n, tag, obj_name, fn_name, label);
        return;
    }

    // A selection sent as text. See string_index.
    {
        int idx;
        if ((is_selection_fn(fn_name) || strcmp(fn_name, "AS_SetFocus") == 0) &&
            string_index(p, &idx)) {
            if (idx < 0)
                logf_("[%ld] %s %s.%s  FOCUS none\n", n, tag, obj_name, fn_name);
            else
                focus_announce(n, tag, obj_name, fn_name, object, idx);
            return;
        }
    }

    // A modal prompt is composed rather than narrated call by call.  It
    // arrives unasked, takes the keyboard from whatever was underneath it,
    // and has no cursor to move, so nothing will read it a second time --
    // and the calls that draw it cancel each other on the general path.
    // dialog.c holds the reasoning and the ordering it depends on.
    if (dialog_is_box(obj_name)) {
        // The help calls carry the button's icon beside its label
        // ("EXIT WITHOUT CHANGES", "Icon_A_X"); the label is what is wanted.
        const char* text = "";
        for (int i = 0; i < p->nstrings; i++) {
            if (looks_like_asset(p->strings[i])) continue;
            text = p->strings[i];
            break;
        }
        int slot = p->nnumbers ? (int)p->numbers[0] : -1;

        char say[DIALOG_MAX_TEXT];
        int what = dialog_note(object, fn_name, slot, text, say, sizeof say);
        if (what != DIALOG_IGNORED) {
            if (what == DIALOG_SPEAK || what == DIALOG_UPDATE)
                strncpy_s(g_dialog_said, sizeof g_dialog_said, say, _TRUNCATE);
            if (what == DIALOG_SPEAK) {
                logf_("[%ld] %s %s.%s  DIALOG says \"%s\"\n",
                      n, tag, obj_name, fn_name, say);
                // Ahead of anything a screen redrawing underneath has left
                // waiting: the prompt is what the keyboard is now attached to.
                speech_cancel_pending();
                if (g_speak && !muted()) speech_say(say);
            } else if (what == DIALOG_UPDATE) {
                logf_("[%ld] %s %s.%s  DIALOG update \"%s\"\n",
                      n, tag, obj_name, fn_name, say);
                if (g_speak && !muted()) speech_say_after(say, SETTLE_MS);
            } else if (*text) {
                logf_("[%ld] %s %s.%s  DIALOG held \"%s\"\n",
                      n, tag, obj_name, fn_name, text);
            } else {
                logf_("[%ld] %s %s.%s  DIALOG held (no text)\n",
                      n, tag, obj_name, fn_name);
            }
            return;
        }
    }

    // A checkbox reports its state separately from its name:
    //
    //     SetCheckboxLabel(int Index, string strText)   "Enable Ironman?"
    //     SetCheckboxValue(int Index, bool bChecked)
    //
    // The second carries no text, so it logged as "(no text)" and the state
    // was never spoken -- the box flipped and nothing said so. It is filed as
    // the control's value, beside the name the label call stored.
    if (is_checkbox_state_fn(fn_name) && p->nnumbers && p->nbools) {
        int idx = (int)p->numbers[0];
        if (idx >= 0 && idx < FOCUS_MAX_LABELS) {
            const char* state = p->bools[0] ? "checked" : "unchecked";
            int changed = focus_set_part(object, idx, FOCUS_PART_VALUE, state);
            logf_("[%ld] %s %s.%s  SLOT %d value = \"%s\"%s\n",
                  n, tag, obj_name, fn_name, idx, state,
                  changed ? "  (changed)" : "");
            if (changed) speak_slot(object, idx);
        }
        return;
    }

    // Where a slider sits. Two plain integers and no text at all, so it used
    // to log as "NUMS 3, 49" and go no further: moving a slider was silent,
    // and landing on one read "Music volume:" with no position -- while the
    // spinner beside it, whose value happens to be a string, read
    // "Shadows: Medium". The value is filed as this control's, beside the
    // name SetSliderLabel stored, exactly as a checkbox's state is.
    //
    // The frame carries the pair twice, once as the parameters and once as
    // the ASValue array built from them, so the parameters are taken: they
    // are first, and they are what the function was called with.
    if (is_slider_value_fn(fn_name) && p->nnumbers >= 2 && !p->nstrings) {
        int idx = (int)p->numbers[0];
        if (idx >= 0 && idx < FOCUS_MAX_LABELS) {
            char value[32];
            _snprintf_s(value, sizeof value, _TRUNCATE, "%d percent",
                        (int)p->numbers[1]);
            int changed = focus_set_part(object, idx, FOCUS_PART_VALUE, value);
            logf_("[%ld] %s %s.%s  SLOT %d value = \"%s\"%s\n",
                  n, tag, obj_name, fn_name, idx, value,
                  changed ? "  (changed)" : "");
            // Only a change speaks. A slider is republished whole every time
            // its screen redraws, and RefreshSlider sends the label, the
            // value and the step together, so an arrival that says nothing
            // new must stay quiet or every redraw would read the tab aloud.
            if (changed) speak_slot(object, idx);
        }
        return;
    }

    // Which item of a container widget is current. The items were published
    // separately, so the index resolves against them rather than the slots.
    if (p->nnumbers && !p->nstrings &&
        (is_inner_selection_fn(fn_name) || is_marker_fn(fn_name))) {
        char item[FOCUS_MAX_LABEL];
        int got;
        int idx;
        if (is_inner_selection_fn(fn_name) && p->nnumbers >= 2) {
            idx = (int)p->numbers[1];
            got = focus_option_at(object, (int)p->numbers[0], idx,
                                  item, sizeof item);
        } else {
            // A marker names the choice without naming the list, so it is the
            // list currently being navigated.
            idx = (int)p->numbers[0];
            got = focus_recent_option_at(idx, item, sizeof item);
        }
        if (got) {
            logf_("[%ld] %s %s.%s  ITEM %d -> \"%s\"\n",
                  n, tag, obj_name, fn_name, idx, item);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say(item);
        } else {
            logf_("[%ld] %s %s.%s  ITEM %d unresolved\n",
                  n, tag, obj_name, fn_name, idx);
        }
        return;
    }

    if (!p->nstrings && !p->nnumbers) {
        logf_("[%ld] %s %s.%s (no text)\n", n, tag, obj_name, fn_name);
        return;
    }

    // The help bar's button style, "XComButtonIconPC", sent around every
    // redraw of it. A lone string, so the general path below held it as a
    // possible announcement, and on the ship list nothing came after to
    // cancel it: it was said after every move (log of 2026-09-27).
    if (strcmp(fn_name, "AS_SetButtonType") == 0) return;

    if (!log_frame_call(obj_name, fn_name))
        for (int i = 0; i < p->nstrings; i++)
            logf_("[%ld] %s %s.%s  \"%s\"\n", n, tag, obj_name, fn_name, p->strings[i]);

    ULONGLONG now = GetTickCount64();
    // Compare against the last call that actually carried text, not the last
    // call of any kind.  Screens interleave their labels with other traffic --
    //     AS_SetCheckboxLabel "Tutorial"
    //     AS_SetCheckboxValue      (no text)
    //     AS_SetCheckboxStyle      (no text)
    //     AS_SetCheckboxLabel "Ironman"
    // and treating those as a break started a new list per label, leaving one
    // entry behind and resolving every index to the last thing seen.
    int continues_list = (object == g_last_obj) &&
                         (strcmp(fn_name, g_last_fn) == 0) &&
                         (now - g_last_at < LIST_WINDOW_MS);

    if (p->nstrings && p->nnumbers && p->numbers[0] >= 0 && p->numbers[0] < FOCUS_MAX_LABELS) {
        // The call carries its own slot number, so place the label there
        // rather than inferring order from arrival:
        //     AS_SetCheckboxLabel(int Index, string strText)
        //     AS_AddListItem(int Id, string Desc, ...)
        // Refreshing one row no longer disturbs the rest of the list.
        int idx = (int)p->numbers[0];
        // Bounds tracked by hand. strcat_s does NOT truncate: on a full
        // destination it invokes the CRT invalid-parameter handler, which
        // __fastfail()s and takes the process with it -- past SEH, so the
        // handler above cannot catch it. SetDropdownOptions ships twenty
        // resolution strings and overran this.
        char joined[FOCUS_MAX_LABEL];
        size_t used = 0;
        int locked = 0;
        joined[0] = 0;
        for (int i = 0; i < p->nstrings; i++) {
            if (looks_like_asset(p->strings[i])) continue;
            // An icon id riding beside the label: the mission list sends
            // AS_AddOption(..., "SCAN FOR ACTIVITY", "_ScanForUFO"). Always a
            // leading underscore and no space. Dropped here rather than in
            // looks_like_asset, because the shot readout reads "_lowCover".
            if (p->strings[i][0] == '_' && !strchr(p->strings[i], ' ')) continue;
            if (hue_unavailable(obj_name, fn_name, p->hues[i])) locked = 1;
            if (state_unavailable(obj_name, fn_name, p)) locked = 1;
            // The frame walk sees every property, so a setter's parameter and
            // the local it was copied into both arrive -- one call, the same
            // text twice, joined into "Mode:, Mode:". Repeats within a single
            // call are always that artefact, never two real labels, because
            // this path is building one label out of one call.
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (strcmp(p->strings[i], p->strings[j]) == 0) { dup = 1; break; }
            if (dup) continue;
            size_t want = strlen(p->strings[i]);
            size_t sep = used ? 2 : 0;
            if (used + sep + want >= sizeof joined) break;
            if (sep) { memcpy(joined + used, ", ", 2); used += 2; }
            memcpy(joined + used, p->strings[i], want);
            used += want;
            joined[used] = 0;
        }
        if (locked && joined[0]) {
            const char* tag = ", unavailable";
            if (used + strlen(tag) < sizeof joined) {
                memcpy(joined + used, tag, strlen(tag) + 1);
                used += strlen(tag);
            }
        }
        // Drop any lone line still waiting to be spoken before deciding what
        // this call should say; otherwise the cancel below would swallow the
        // announcement this very call schedules.
        speech_cancel_pending();

        if (is_option_list_fn(fn_name)) {
            // The items belong to the widget at `idx`, not to the screen's
            // slot list. The trailing entry is the raw string Flash is handed
            // ("Easy,0 ; Normal,1 ; ...") and is not one of the choices.
            focus_options_begin(object, idx);
            int kept = 0;
            for (int i = 0; i < p->nstrings; i++) {
                if (looks_like_asset(p->strings[i])) continue;
                if (strchr(p->strings[i], ';') && strchr(p->strings[i], ',')) continue;
                focus_options_add(object, idx, p->strings[i]);
                kept++;
            }
            logf_("[%ld] %s %s.%s  OPTIONS slot %d, %d items\n",
                  n, tag, obj_name, fn_name, idx, kept);
        } else if (joined[0]) {
            int part = is_value_fn(fn_name) ? FOCUS_PART_VALUE : FOCUS_PART_LABEL;
            int changed = focus_set_part(object, idx, part, joined);
            // Entering a facility publishes its submenu and then re-sends
            // the facility menu's focus. The submenu starts on its first
            // option and, in mouse mode, never sends a selection of its own
            // (FacilitySubMenu.OnFlashCommand realizes only without a
            // mouse) -- so the first option rides along as the facility's
            // panel: "BARRACKS, needs attention. VIEW SOLDIERS".
            if (idx == 0 && g_hq_menu && part == FOCUS_PART_LABEL &&
                strncmp(obj_name, "UIStrategyHUD_FSM_", 18) == 0)
                focus_set_detail(g_hq_menu, joined);
            logf_("[%ld] %s %s.%s  SLOT %d %s \"%s\"%s\n", n, tag, obj_name, fn_name,
                  idx, part == FOCUS_PART_VALUE ? "value =" : "label =", joined,
                  changed ? "  (changed)" : "");
            if (changed) speak_slot(object, idx);
        }
    } else if (p->nstrings == 1 && is_title_fn(fn_name)) {
        // A heading. Kept apart from the list, which it used to open -- and
        // opening a list means clearing it, so a screen retitling itself
        // wiped what it had published. Still said on its own if nothing
        // follows; a list arriving cancels that, and the heading is then
        // said before the first item the cursor lands on.
        if (focus_set_title(object, p->strings[0]) &&
            !looks_like_asset(p->strings[0]) && !muted())
            speech_say_after(p->strings[0], SETTLE_MS);
    } else if (p->nstrings) {
        // No index given. Several strings at once is a screen publishing its
        // contents; repeated single-string calls are the same thing spread
        // out. Either way it is a list, and a list must not read itself
        // aloud -- it is recorded so a later index can be resolved.
        if (p->nstrings > 1) {
            focus_begin(object);
            for (int i = 0; i < p->nstrings; i++) focus_add(object, p->strings[i]);
            speech_cancel_pending();
        } else if (continues_list) {
            focus_add(object, p->strings[0]);
            speech_cancel_pending();
        } else {
            // Might be an announcement, might be the first row of a list.
            // Hold it briefly; a second call will cancel it.
            focus_begin(object);
            focus_add(object, p->strings[0]);
            if (!looks_like_asset(p->strings[0]) && !muted())
                speech_say_after(p->strings[0], SETTLE_MS);
        }
    } else if (p->nnumbers && is_selection_fn(fn_name) && focus_count(object) > 0) {
        // No text, just an index, from a function that actually moves the
        // selection: XCOM never re-sends the label, so resolve it from what
        // the screen published.
        focus_announce(n, tag, obj_name, fn_name, object, (int)p->numbers[0]);
    } else if (p->nnumbers) {
        // Numbers nobody claimed. Now that plain ints are readable these are
        // no longer invisible, and dropping them silently hid the very call
        // that names EU's difficulty -- so they are logged even when they say
        // nothing, because that is the evidence the next screen is read from.
        char nums[64];
        size_t used = 0;
        nums[0] = 0;
        for (int i = 0; i < p->nnumbers && used + 12 < sizeof nums; i++)
            used += (size_t)_snprintf_s(nums + used, sizeof nums - used, _TRUNCATE,
                                        i ? ", %d" : "%d", (int)p->numbers[i]);
        if (!log_frame_call(obj_name, fn_name))
            logf_("[%ld] %s %s.%s  NUMS %s\n", n, tag, obj_name, fn_name, nums);
    }

    if (p->nstrings) {
        g_last_obj = object;
        strncpy_s(g_last_fn, sizeof g_last_fn, fn_name, _TRUNCATE);
        g_last_at = now;
    }
}

// The re-entrancy guard, which covers a hooked native being reached from
// inside another one. It is cleared in a termination handler because a fault
// does not leave through any return: the thunk's __except catches it, and a
// guard cleared by hand at each exit stayed set -- after one access violation
// while aiming, every later capture on the game thread saw itself as nested
// and returned at once, and the mod went silent for the rest of the session,
// pause menu and dialogue boxes included.
static void capture(const char* tag, LONG n, void* stack)
{
    static __declspec(thread) int tls_busy;
    if (tls_busy) return;
    tls_busy = 1;
    tls_where[0] = 0;
    __try { capture_body(tag, n, stack); }
    __finally { tls_busy = 0; }
}

// Each native gets its own thunk, since each has its own original to call;
// they all funnel into capture().
// Relabels a keypress as the gamepad button a screen is waiting for.
//
// Runs on CheckInputIsReleaseOrDirectionRepeat, which every shell screen calls
// as the first line of its OnUnrealCommand, so the rewrite lands before the
// switch below reads Cmd. The parameter is found the same way text is: by
// walking the caller's UProperty chain and matching the declared name, rather
// than assuming it is first in the frame.
// Returns 1 when the command must not reach the screen at all.  The caller
// delivers that by forcing this native's own answer to false: every screen
// opens its handler with
//
//     if(CheckInputIsReleaseOrDirectionRepeat(Cmd, Arg)!) return true;
//
// so a false answer makes the screen report the key as handled and do nothing
// with it -- before the switch, and before the refresh at the end. That is a
// cleaner swallow than rewriting the command to a code nothing matches, which
// would still run the rest of the handler.
static void hq_locked_note(LONG n, void* sub, const char* screen);
static void loadout_leave_locker(LONG n, void* loadout, const char* screen);

// Whether a key reaching `screen` says the player is in the Situation Room or
// somewhere else. Every key in the room passes through its own screens
// (UIObjectivesScreen first, then the strategy HUD and the facility menu,
// then UIStrategyHUD_FSM_SituationRoom; UISituationRoom itself in the
// satellite view), and the strategy HUD and the facility menu see every key
// at the base as well, so those two say nothing either way. Any other
// screen -- another facility, the Gray Market, a dialogue -- means the
// player has left the room's main view.
static void sitroom_key_reached(const char* screen)
{
    if (strncmp(screen, "UISituationRoom", 15) == 0 ||
        strncmp(screen, "UIObjectivesScreen", 18) == 0 ||
        strncmp(screen, "UIStrategyHUD_FSM_SituationRoom", 31) == 0) {
        g_sitroom_at = GetTickCount64();
        return;
    }
    if ((strncmp(screen, "UIStrategyHUD_", 14) == 0 && screen[14] >= '0' && screen[14] <= '9') ||
        strncmp(screen, "UIStrategyHUD_FacilityMenu", 26) == 0 ||
        strncmp(screen, "UIStrategyHUD_BuildQueue", 24) == 0)
        return;
    g_sitroom_left_at = GetTickCount64();
}

// Whether a key reaching `screen` says the player is in Engineering -- its
// submenu, Build Items, an order, the Foundry -- or somewhere else. The
// strategy HUD, the facility menu and the build queue see every key at the
// base, so they say nothing either way.
static void eng_key_reached(const char* screen)
{
    if (strncmp(screen, "UIStrategyHUD_FSM_Engineering", 29) == 0 ||
        strncmp(screen, "UIBuildItem", 11) == 0 ||
        strncmp(screen, "UIManufacturing", 15) == 0 ||
        strncmp(screen, "UIFoundry", 9) == 0) {
        g_eng_at = GetTickCount64();
        return;
    }
    if ((strncmp(screen, "UIStrategyHUD_", 14) == 0 && screen[14] >= '0' && screen[14] <= '9') ||
        strncmp(screen, "UIStrategyHUD_FacilityMenu", 26) == 0 ||
        strncmp(screen, "UIStrategyHUD_BuildQueue", 24) == 0)
        return;
    g_eng_left_at = GetTickCount64();
}

// Whether Delete should read Engineering's queue: as sitroom_up.
static int eng_up(void)
{
    return g_eng_at && g_eng_at > g_eng_left_at &&
           g_seen_strategy_at && g_seen_tactical_at <= g_seen_strategy_at;
}

// Whether Delete should open the Situation Room rather than the base's
// status: the room drew or took a key more recently than the player was seen
// anywhere else, and the base more recently than a mission.
static int sitroom_up(void)
{
    return g_sitroom_at && g_sitroom_at > g_sitroom_left_at &&
           g_seen_strategy_at && g_seen_tactical_at <= g_seen_strategy_at;
}

// Up and Down in a lab with soldier slots. See slots_select.
static void slots_walk(LONG n, int down);

static int rewrite_cmd(LONG n, void* stack)
{
    g_ui_key_at = GetTickCount64();
    if (!readable(stack, 0x20)) return DELIVER;

    void* node      = *(void**)((uint8_t*)stack + FFRAME_NODE);
    void* object    = *(void**)((uint8_t*)stack + FFRAME_OBJECT);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !props_ready()) return DELIVER;

    char screen[128] = "?";
    object_name(object, screen, sizeof screen);
    sitroom_key_reached(screen);
    eng_key_reached(screen);
    alert_flush(n, "Input       ", object);

    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);

    // Both integers are wanted: the command, and the action mask beside it.
    // Both are taken by position, because the shape is constant where the
    // names are not: OnUnrealCommand(int Cmd, int Arg) on the difficulty
    // screen, (int ucmd, int Actionmask) on the pause menu, and two more
    // spellings besides. Every one of the 22 callers of this native --
    // checked against both decompiles -- declares exactly two int parameters
    // with the command first, so position says what a name cannot.
    //
    // Matching the name "Cmd" is what cost the pause menu: it declares `ucmd`,
    // so the walk found nothing, logged "no Cmd parameter found", and dropped
    // every key on the one full menu a mission can reach.
    int32_t* cmd_slot = NULL;
    int mask = 0;
    int have_mask = 0;

    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return DELIVER;

        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        void* next     = *(void**)((uint8_t*)prop + UFIELD_NEXT);

        // The return value carries CPF_Parm as well. OnUnrealCommand returns
        // bool, which classifies as PROP_BOOL and would be passed over anyway,
        // but a walk that takes the first int it meets should say which ints
        // it means rather than lean on that.
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000) {
            int32_t* slot = (int32_t*)(locals + off);
            if (!cmd_slot) {
                // Writing into the caller's frame, so the page must be
                // writable as well as readable; a local that lives in
                // read-only memory would mean this is not the frame we think
                // it is.
                if (!writable(slot, sizeof *slot)) return DELIVER;
                cmd_slot = slot;
            } else if (readable(slot, sizeof *slot)) {
                mask = *slot;
                have_mask = 1;
                break;
            }
        }
        prop = next;
    }

    if (!cmd_slot) {
        // Reached only if the caller declared no int parameter at all, which
        // would mean this native was reached from something that is not an
        // OnUnrealCommand. Worth saying so: every key on that screen would
        // silently do nothing.
        logf_("[%ld] Input        %s  no command parameter found\n", n, screen);
        return DELIVER;
    }

    int cmd = *cmd_slot;
    const char* from_name = input_cmd_name(cmd);

    // A single keystroke arrives here several times -- press, hold, release --
    // and the native this hook runs on exists precisely to filter those, which
    // it does *after* us.  Anything that speaks or moves has to filter them
    // itself, or one press acts three times.
    int press = (!have_mask || (mask & FXS_ACTION_PRESS));

    // The rest of the keystroke that fired something from the menu.
    //
    // One keystroke reaches this native several times and the screen acts on
    // only one of them -- not the first, and not the one carrying the press
    // bit. The log says so plainly: of the four calls behind a single Enter,
    // the native answered false to three and true to the last.
    //
    //     [100] MENU fires X(302) in place of 511 (mask 1)
    //     [100] after rewrite: native said 0  -- the screen will ignore this
    //     [101] swallow: native said 0
    //     [102] swallow: native said 0
    //     [103] swallow: native said 1        <- the one that would have acted
    //
    // So a choice is carried by the *whole* keystroke rather than by the
    // event the menu happened to act on. That is what the key remaps have
    // always done -- they rewrite every call, which is precisely why they
    // work -- and firing now does the same. It also keeps the trailing
    // events from arriving as a bare Enter, which is how firing "advanced
    // options" used to tick the difficulty checkbox underneath.
    if (g_fired_key) {
        if (cmd == g_fired_key) {
            *cmd_slot = g_fired_cmd;
            // The keystroke ends at the release; anything else pending would
            // hijack the next press of the same key.
            if (have_mask && (mask & FXS_ACTION_RELEASE)) g_fired_key = 0;
            return DELIVER;
        }
        g_fired_key = 0;               // a different key: the moment has passed
    }

    // While the menu is up the player is talking to it, not to the screen.
    if (help_menu_is_open()) {
        char say[512];
        int fire = 0;
        int what = press ? help_menu_key(screen, cmd, &fire, say, sizeof say)
                         : HELP_MENU_QUIET;
        switch (what) {
            case HELP_MENU_SPEAK:
                logf_("[%ld] Input        %s  MENU \"%s\"\n", n, screen, say);
                if (g_speak) speech_say_now(say);
                *cmd_slot = CMD_INERT;
                // The screen still runs its handler to the end on an inert
                // command, and UIShellDifficulty refreshes its description
                // there, so keep the quiet window alive while the menu is up.
                g_quiet_until = GetTickCount64() + QUIET_MS;
                return SUPPRESS;
            case HELP_MENU_QUIET:
                *cmd_slot = CMD_INERT;
                g_quiet_until = GetTickCount64() + QUIET_MS;
                return SUPPRESS;
            case HELP_MENU_FIRE:
                // The player's own keypress carries the command in. Nothing
                // is synthesised: this is the rewrite the table does, with
                // the target chosen a moment ago instead of years ago.
                *cmd_slot = fire;
                g_fired_key = cmd;
                g_fired_cmd = fire;
                logf_("[%ld] Input        %s  MENU fires %s(%d) in place of %d\n",
                      n, screen,
                      input_cmd_name(fire) ? input_cmd_name(fire) : "?",
                      fire, cmd);
                g_quiet_until = 0;
                return DELIVER;
            default:
                // Stale -- the screen changed under it. Fall through and treat
                // the key as the screen's own.
                logf_("[%ld] Input        %s  MENU closed, screen changed\n",
                      n, screen);
                break;
        }
    }

    // Insert's list (review_pump) has the arrows, Enter and Escape while it is
    // open, and for a moment after it closes, so the Escape that closed it
    // does not also back out of the screen underneath.
    if ((history_is_open() || GetTickCount64() < g_menu_grace_until) &&
        ((cmd >= 500 && cmd <= 503) || cmd == 510 || cmd == 511)) {
        *cmd_slot = CMD_INERT;
        return SUPPRESS;
    }

    // Up or Down on a dialogue box says it again. It is read once, as it
    // appears, often straight after something else -- the unlock popup lands
    // on the end of the council's report -- and nothing else will ever read
    // it. UIDialogueBox.OnUnrealCommand has no case for either arrow in
    // either build, so the key goes on to be ignored.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        dialog_is_box(screen) && g_dialog_said[0]) {
        logf_("[%ld] Input        %s  DIALOG again \"%s\"\n", n, screen, g_dialog_said);
        if (g_speak) speech_say_now(g_dialog_said);
    }

    // Up or Down on a debrief page they do nothing on says the page again.
    // The council's report can be buried by the unlock popup that opens on
    // top of it: re-reading that popup cut the report off, and once the
    // popup closed nothing would read the page.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        strncmp(screen, "UIDebrief", 9) == 0 && g_debrief_rereads && g_debrief_page[0]) {
        logf_("[%ld] Input        %s  DEBRIEF again\n", n, screen);
        if (g_speak) speech_say_now(g_debrief_page);
    }

    // Up or Down on the end-of-month report walks it. See eom_walk. Both
    // screens take every key; the outer one is answered, the inner is not.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        strncmp(screen, "UIEndOfMonthReport", 18) == 0)
        eom_walk(n, screen, cmd == FXS_ARROW_DOWN);

    // Up or Down in a lab with soldier slots moves the selection the screen
    // itself never moves from the keyboard. See slots_select.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        (strncmp(screen, "UIGeneLab", 9) == 0 || strncmp(screen, "UIPsiLabs", 9) == 0 ||
         strncmp(screen, "UICyberneticsLab", 16) == 0))
        slots_walk(n, cmd == FXS_ARROW_DOWN);

    // Up or Down on the finance statement walks it. See fin_walk.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        strncmp(screen, "UIBaseFinances", 14) == 0)
        fin_walk(n, screen, cmd == FXS_ARROW_DOWN);

    // Left in the loadout's locker. See loadout_leave_locker.
    if (cmd == FXS_ARROW_LEFT && strncmp(screen, "UISoldierLoadout", 16) == 0)
        loadout_leave_locker(n, object, screen);

    // Up and down on a facility submenu the tutorial has locked. See
    // hq_locked_note. Only observed: the key goes on to the screen, which
    // plays its sound and ignores it.
    if (press && (cmd == FXS_ARROW_UP || cmd == FXS_ARROW_DOWN) &&
        strncmp(screen, "UIStrategyHUD_FSM_", 18) == 0)
        hq_locked_note(n, object, screen);

    // The key that opens the menu, and reads the whole list on the way in so
    // that one press still answers "what can I do here".
    //
    // It is swallowed, so the screen never sees 621: the shell has no case for
    // it, but UITacticalHUD_AbilityContainer spends 619, 620 and 621 on
    // DirectPickAbility, and this key has to mean the same thing everywhere.
    if (cmd == FXS_KEY_0) {
        if (press) {
            char say[512];
            int count = help_menu_open(screen, say, sizeof say);
            logf_("[%ld] Input        %s  MENU open (%d) \"%s\"\n",
                  n, screen, count, say);
            if (g_speak) speech_say_now(say);
        }
        *cmd_slot = CMD_INERT;
        // An inert command still runs the handler to its end, and
        // UIShellDifficulty refreshes its description there. See muted().
        g_quiet_until = GetTickCount64() + QUIET_MS;
        return SUPPRESS;
    }

    // Anything else means the player has moved on, and what it redraws is
    // worth hearing again -- so the quiet window ends here rather than only
    // when it times out.
    g_quiet_until = 0;

    // Leaving the soldier summary: the next visit names its soldier again,
    // even the same one.
    if (strncmp(screen, "UISoldierSummary", 16) == 0 &&
        (cmd == FXS_KEY_ESCAPE || cmd == FXS_BUTTON_B)) {
        g_summary_said[0] = 0;
        g_summary_obj = NULL;
    }

    int to = input_remap(screen, cmd);
    if (to) {
        *cmd_slot = to;
        const char* to_name = input_cmd_name(to);
        logf_("[%ld] Input        %s  %s(%d) -> %s(%d)\n", n, screen,
              from_name ? from_name : "?", cmd,
              to_name ? to_name : "?", to);
    } else {
        // Every command is logged, not only the remapped ones. With only the
        // remaps visible there was no way to tell a key that never arrived
        // from one arriving under a code we did not expect -- which is
        // exactly the question Q raised.
        logf_("[%ld] Input        %s  cmd %d%s%s\n", n, screen, cmd,
              from_name ? " = " : "", from_name ? from_name : "");
    }
    return DELIVER;
}

#define THUNK(id, tag)                                                        \
    static ExecFn g_orig_##id;                                                \
    static void __fastcall hook_##id(void* self, void* edx,                   \
                                     void* stack, void* result)               \
    {                                                                         \
        LONG n = InterlockedIncrement(&g_calls);                              \
        Fault f;                                                              \
        __try { capture(tag, n, stack); }                                     \
        __except (fault_note(GetExceptionInformation(), &f)) {                \
            char prefix[64];                                                  \
            _snprintf_s(prefix, sizeof prefix, _TRUNCATE,                     \
                        "[%ld] %s capture", n, tag);                          \
            fault_log(prefix, &f, tls_where);                                 \
        }                                                                     \
        g_orig_##id(self, edx, stack, result);                                \
    }

// The cursor, watched rather than driven -- for now.
//
// AXCom3DCursor::GetCursorMode is native and the cursor's own Tick calls it
// every frame, so `self` hands the object over for nothing. Identifying it is
// the whole point: the grid cannot be navigated by relabelling a command,
// because the cursor is flown rather than stepped, so the next piece has to
// write a position into this object -- and that is worth proving readable,
// against a real mission, before anything writes.
//
// The position is read four times a second rather than every frame. The read
// itself is twelve bytes at a fixed offset, but it is guarded by VirtualQuery
// like every other read into the game, and that is a syscall on the game's
// own thread.
#define CURSOR_WATCH_MS 250

static ULONGLONG g_cursor_at;
static float g_cursor_last[3];
static CursorGrid g_grid_last;

// ---- numpad navigation -----------------------------------------------------
//
// The keys are read here rather than through the game's input, because the
// numpad is unbound in [XComGame.XComTacticalInput]: an unbound key never
// becomes an InputEvent, so no hook on the input path would ever see it. That
// also means the game does nothing with them, so nothing has to be swallowed.
// They are polled on the cursor's per-frame native, so they are live exactly
// while a battle cursor exists, and only while the game has the foreground.
//
// Num Lock must be on. With it off, Windows reports numpad 8 as the Up arrow
// -- which pans the camera -- and NVDA's desktop layout takes the numpad for
// its own review commands.
//
// A move is not written into the cursor. In mouse mode Mouse_CheckForPathing
// puts the cursor under the mouse on every frame, so a written Location would
// last one frame. Instead the target is handed to hook_validpos below, which
// substitutes it where the game turns that frame's pick into a cursor position
// -- so the game's own validation, floor snap and path preview run on it.
//
// A tap on a direction steps one tile. Holding it glides, tile after tile,
// until it is let go -- see "holding a direction" below, next to nav_press.

static int       g_numpad_down[10];
static int       g_radar_down[2];       // numpad +, numpad -
static int       g_walls_down;          // numpad *
// Numpad * turns the wall field off and on. A sound that never stops and
// cannot be stopped is a trap, and the player who wants the words without it
// -- or who is working next to someone -- has no other way out. Like the
// digits and the radar keys, numpad * is bound to nothing in a mission:
// DefaultInput.ini mentions Multiply only in the alias lists of edit boxes and
// sliders. The switch is the options menu's (settings.h, SET_FIELD), so the
// choice is saved and either key reaches it.
static void*     g_nav_cursor;          // the cursor navigation began on
static void*     g_nav_pawn;            // ChainedPawn when navigation began
static POINT     g_nav_mouse;           // where the mouse was, to notice it moving
static float     g_nav_world[3];        // the target, in world units, for the hook
static int       g_nav_live;            // the hook substitutes while this is set
static ULONGLONG g_nav_key_at;          // last direction key
static ULONGLONG g_nav_placed_at;       // last frame the hook substituted
static int       g_nav_parked;          // mouse already moved to the centre once

// Heights and reachability. The cursor must be put at the floor's height or
// the game builds no path to it, and whether a tile can be reached at all is
// known only from the path the game builds. Both are settled per tile by the
// phases in nav.h -- floor search, then probing heights against the
// pathfinder -- which main.c drives from three hooks: the pick asks
// navh_query_z, hook_floorz reports the search, and hook_computepath reports
// the path.
//
// The evidence behind the phases, from the logs:
//   - getValidLocation adds exactly 64 to every placement (NAVH_LIFT).
//   - GetFloorZForPosition returns the height it was given when it finds no
//     floor (XGUnit.IsAttemptingToHover tests `FloorZ != PathDestination.Z`),
//     so a search must never start from its own last answer: that climbed 64
//     a step.
//   - It looks down a limited way, and never found a floor below -129 from
//     any start -- which left two soldiers unable to move by numpad at all.
//   - The ground estimate at the start is the cursor less 64. The soldier
//     pawn's Location.Z less CollisionHeight read 215.8 for two soldiers on
//     different ground, so it is not used.
#define NAV_CURSOR_LIFT NAVH_LIFT
static int       g_nav_path_tile[2] = { -1, -1 };     // the tile being decided
static NavHeightPhase g_nav_phase_logged = (NavHeightPhase)-1;
static int       g_nav_tile_logged[2] = { -1, -1 };

static int game_has_focus(void)
{
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// The tile the cursor stands on, by the native's own arithmetic.
static int cursor_tile(const CursorGrid* g, int* tx, int* ty, float* z)
{
    float x, y, cz;
    if (!cursor_position(&x, &y, &cz)) return 0;
    *tx = cursor_tile_axis(x, g->min_x, CURSOR_TILE);
    *ty = cursor_tile_axis(y, g->min_y, CURSOR_TILE);
    if (z) *z = cz;
    return 1;
}

// ---- a step, waiting to be said ---------------------------------------------
//
// What is on the tile is worked out in report.c; this is the step that waits
// for it, and when it gives up waiting.

static ULONGLONG g_tile_due;            // when the target tile is to be described
static int       g_tile_due_at[2];
static int       g_tile_due_dash;       // whether its path says anything about it

// A step is announced once, when what is on the tile is known, with the
// coordinates last: "Ellis. Low cover south. 44, 12." Until then the step's
// coordinates, and anyone found standing there, wait here. If nothing decides
// the tile in STEP_FALLBACK_MS, what is known is said anyway.
#define STEP_FALLBACK_MS 1500
static int       g_step_pending;
static ULONGLONG g_step_deadline;

// How many times the game has computed a path since the current step began.
// Counted for every caller, not only the navigated tile, because the question
// it answers is whether the game is pathing at all.
static volatile LONG g_path_calls;
// The fallback speaks the coordinates and gives up on the description -- but
// the description is not always gone, only late. The pathfinder does not run
// while a soldier is walking, and in the 2026-09-20 log a step taken during a
// move had its path built one line *after* the deadline had already said
// "44, 6.", so "Low cover west." was worked out and thrown away. When that
// happens the tile is remembered here and the missing half is said on its own
// when it turns up; the coordinates are not repeated, having just been heard.
static int       g_step_late;
static int       g_step_late_at[2];
static char      g_step_coords[NAV_MAX_TEXT];
// Who stands in the target's column, found on arrival. A step does not know
// its floor yet then, so they are worded when the step is said, against the
// floor it settled on: "Godongwana, one floor up." (step_who).
static ColumnUnit g_step_units[COLUMN_UNITS];
static int        g_step_nunits;
static char      g_step_note[160];              // said first: "Floor 2." after F / C
static char      g_step_where[64];             // then "Inside building, floor 2 of 3."
// The floor F / C put the target on, until the next step. The tile does not
// change, so a path the game already had for it -- on the floor it was just
// taken off -- is still reported, and was taken as proof of that floor: the
// first runs of F went 212.6 -> 466.2 and settled straight back on 212.6.
static int       g_floor_hold;
static float     g_floor_hold_z;
// How far a path's end, or a pick's floor, may sit from the held floor and
// still be on it: half a storey, well clear of the floors either side.
#define FLOOR_HOLD_SLACK 96.0f

// How long after a tile's first path it is described. None: the next frame.
// It was 200 ms while "Dash" came from DestinationReachability, which the
// dash rebuild (ChangeDashState) changes a tick later. The path's own cost
// needs no such wait -- the pathfinder builds the whole path at once, past
// the dash limit included (costs of 54 against a MaxPathCost of 24).
#define TILE_DESCRIBE_DELAY_MS 0
// The soldier's own tile has no verdict to wait for, only the floor search,
// which on level ground settles on its first frame.
#define OWN_TILE_DELAY_MS 60

// ---- headquarters screens the keyboard needs help with ----------------------

// The labs with soldier slots: the Genetics Lab (UIGeneLab), the Psi Labs
// (UIPsiLabs) and the Cybernetics Lab (UICyberneticsLab), all UISoldierSlots.
// Its OnUnrealCommand has cases for Enter and Escape and none for an arrow:
// the selection follows the mouse, or is set to the first slot with a button
// only when there is no mouse. With one, m_iCurrentSelection stayed -1 -- the
// log of 2026-09-27 has "SetSelected FOCUS none" and a dozen arrow presses
// that did nothing -- and XGGeneLabUI.OnChooseSlot(-1) plays the bad sound,
// so a soldier could not be put into the lab from the keyboard at all. So the
// mod keeps the slots as drawn (AS_ClearSoldiers, AS_AddSlot(name, status,
// buttonLabel, disabled)), walks them on Up and Down, and writes its choice
// into the screen's m_iCurrentSelection, which Enter then acts on.
static FieldSlot g_slots_cur;

// Puts the choice into the game and says it; `lead` goes in front.
static void slots_select(LONG n, int i, const char* lead)
{
    if (!g_slots_obj || i < 0 || i >= g_slots_n) return;
    g_slots_sel = i;
    const void* v;
    if (field_ptr(g_slots_obj, "m_iCurrentSelection", &g_slots_cur, sizeof(int32_t), &v) &&
        writable(v, sizeof(int32_t)))
        *(int32_t*)v = i;
    char say[FOCUS_MAX_LABEL + 256];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%sSlot %d of %d: %s", lead ? lead : "", i + 1,
                g_slots_n, g_slots_row[i]);
    logf_("[%ld] SLOTS %d \"%s\"\n", n, i, say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

static void slots_walk(LONG n, int down)
{
    if (!g_slots_n) return;
    int i = g_slots_sel + (down ? 1 : -1);
    if (i >= g_slots_n) i = 0;
    if (i < 0) i = g_slots_n - 1;
    slots_select(n, i, "");
}


// Up and down on a facility submenu while the tutorial has locked them. The
// submenu's OnUnrealCommand plays MenuSelectCue and returns when the
// headquarters input has m_bDisableLeftStick or m_bDisableDPad set -- which
// the tutorial's campaign does, to make the player pick the one option it
// wants. All the player heard was the sound, over and over, with nothing to
// say the list was not moving or which option Enter would pick. The flags
// are read (submenu -> controllerRef -> PlayerInput), not guessed from the
// silence, and the option under the cursor is m_iCurrentSelection.
static FieldSlot g_sub_ctrl, g_ctrl_input, g_sub_sel;
static void hq_locked_note(LONG n, void* sub, const char* screen)
{
    const void* v;
    if (!field_ptr(sub, "controllerRef", &g_sub_ctrl, sizeof(void*), &v)) return;
    void* ctrl = *(void* const*)v;
    if (!ctrl || !field_ptr(ctrl, "PlayerInput", &g_ctrl_input, sizeof(void*), &v)) return;
    const uint8_t* input = *(const uint8_t* const*)v;
    if (!input) return;
    static const char* const flags[] = { "m_bDisableDPad", "m_bDisableLeftStick" };
    int locked = 0;
    for (int i = 0; i < 2 && !locked; i++) {
        const void* prop = object_field_prop(input, flags[i]);
        int b = 0;
        if (prop && props_read_object_bool(prop, input, &b) && b) locked = 1;
    }
    if (!locked) return;
    int sel = 0;
    if (field_ptr(sub, "m_iCurrentSelection", &g_sub_sel, sizeof(int32_t), &v))
        sel = *(const int32_t*)v;
    char label[FOCUS_MAX_LABEL], say[FOCUS_MAX_LABEL + 64];
    if (focus_label_at(sub, sel, label, sizeof label))
        // The option named first: it is the one Enter picks, not a locked
        // one. "Locked by the tutorial. LAUNCH SATELLITE" was heard as the
        // option being blocked (2026-09-25).
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s. The tutorial holds the cursor here.", label);
    else
        strncpy_s(say, sizeof say, "The tutorial holds the cursor here.", _TRUNCATE);
    logf_("[%ld] Input        %s  LOCKED \"%s\"\n", n, screen, say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

// A bool inside a struct field of an object: `owner.field.member`, e.g.
// XGSoldierUI.m_kLocker.bIsSelected. Returns a pointer to the dword holding
// the bit and its mask, or NULL -- and says, once, which step failed.
//
// UStructProperty::Struct is not at a known offset: the first guess, the
// bool's BitMask offset, found nothing live. So a short window past
// UProperty's own fields is searched for the one pointer whose class is
// ScriptStruct, and the answer is kept.
static uint32_t g_struct_ptr_off;
static int      g_struct_logged;
static uint32_t* struct_bool(void* owner, const char* field, const char* member,
                             uint32_t* mask_out)
{
    const char* why = NULL;
    uint32_t* result = NULL;
    const uint8_t* sp = (const uint8_t*)object_field_prop(owner, field);
    void* st = NULL;
    uint32_t moff = 0;
    if (!sp) why = "no such field";
    if (!why && !g_struct_ptr_off) {
        for (uint32_t off = UPROPERTY_OFFSET + 4; off <= 0x90 && !g_struct_ptr_off; off += 4) {
            if (!readable(sp + off, sizeof(void*))) break;
            void* cand = *(void* const*)(sp + off);
            char cls[64];
            if (cand && readable(cand, 0x40) &&
                object_class_name(cand, cls, sizeof cls) && strcmp(cls, "ScriptStruct") == 0)
                g_struct_ptr_off = off;
        }
        if (!g_struct_ptr_off) why = "no ScriptStruct pointer on the field";
        else logf_("struct: UStructProperty::Struct at +0x%X\n", g_struct_ptr_off);
    }
    if (!why) {
        st = *(void* const*)(sp + g_struct_ptr_off);
        // The struct's own bool members teach the mask if nothing has yet.
        if (!props_mask_offset() && props_learn_mask(st))
            logf_("props: BitMask +0x%X, learned from %s\n", props_mask_offset(), field);
        moff = props_mask_offset();
        if (!moff) why = "no bool mask offset";
    }
    if (!why) {
        uint32_t field_off = *(const uint32_t*)(sp + UPROPERTY_OFFSET);
        void* m = readable((uint8_t*)st + USTRUCT_CHILDREN, sizeof(void*))
                      ? *(void**)((uint8_t*)st + USTRUCT_CHILDREN) : NULL;
        why = "no such member";
        for (int guard = 0; m && guard < MAX_FIELDS; guard++) {
            char name[64];
            if (!readable(m, moff + sizeof(uint32_t))) { why = "member unreadable"; break; }
            if (object_name(m, name, sizeof name) && strcmp(name, member) == 0) {
                uint32_t off = *(const uint32_t*)((const uint8_t*)m + UPROPERTY_OFFSET);
                uint32_t mask = *(const uint32_t*)((const uint8_t*)m + moff);
                uint32_t* word = (uint32_t*)((uint8_t*)owner + field_off + off);
                if (!mask || (mask & (mask - 1))) why = "member is not a one-bit bool";
                else if (!writable(word, sizeof *word)) why = "member not writable";
                else {
                    why = NULL;
                    *mask_out = mask;
                    result = word;
                    if (!g_struct_logged)
                        logf_("struct: %s.%s at +0x%X+0x%X, mask 0x%X\n",
                              field, member, field_off, off, mask);
                    g_struct_logged = 1;
                }
                break;
            }
            m = *(void**)((uint8_t*)m + UFIELD_NEXT);
        }
    }
    if (why) logf_("struct: %s.%s not found -- %s\n", field, member, why);
    return result;
}

// Left in the loadout's locker closed the whole screen. UISoldierLoadout
// sends left to OnCancel() while the locker is selected, and OnCancel calls
// XGSoldierUI.OnLeaveGear(manager.IsMouseActive()) -- which steps back to the
// inventory only when that is false. On a PC the mouse is active, so the
// step back is the gamepad's alone, and the keyboard's left and Escape both
// leave. The mouse's own way back is a click on the inventory list, and all
// that click does to the state is clear m_kLocker.bIsSelected. So that is
// what left does: the flag is cleared before the screen reads the key, the
// screen's case then finds the locker not selected and does nothing, and the
// inventory item the cursor returns to is said. Escape still leaves, as the
// game intends.
static FieldSlot g_loadout_mgr;
static void loadout_leave_locker(LONG n, void* loadout, const char* screen)
{
    const void* v;
    if (!field_ptr(loadout, "m_kLocalMgr", &g_loadout_mgr, sizeof(void*), &v)) {
        logf_("[%ld] Input        %s  no m_kLocalMgr -- left will leave\n", n, screen);
        return;
    }
    void* mgr = *(void* const*)v;
    if (!mgr) {
        logf_("[%ld] Input        %s  m_kLocalMgr is empty -- left will leave\n", n, screen);
        return;
    }
    uint32_t mask = 0;
    uint32_t* word = mgr ? struct_bool(mgr, "m_kLocker", "bIsSelected", &mask) : NULL;
    if (!word) {
        logf_("[%ld] Input        %s  locker flag not found -- left will leave\n", n, screen);
        return;
    }
    if (!(*word & mask)) return;            // in the inventory already
    *word &= ~mask;
    g_loadout_side = loadout;               // "Inventory." is said here, once
    char label[FOCUS_MAX_LABEL], say[FOCUS_MAX_LABEL + 16];
    if (g_loadout_inv == loadout && g_loadout_inv_idx >= 0 &&
        focus_label_at(loadout, g_loadout_inv_idx, label, sizeof label))
        _snprintf_s(say, sizeof say, _TRUNCATE, "Inventory. %s", label);
    else
        strncpy_s(say, sizeof say, "Inventory.", _TRUNCATE);
    logf_("[%ld] Input        %s  LOCKER left -> \"%s\"\n", n, screen, say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

// ---- the target ------------------------------------------------------------
//
// What a flag draws besides its name, kept for the shot readout. Both calls
// carry the displayed value in the ASValue array they build, and that is the
// value taken -- not the parameters, which for hit points are the raw figure
// before the game divides it down and before "show enemy health" hides it.
//
//     SetHitPoints(int _currentHP, int _maxHP)   array: current, max; -1, -1 hidden
//     RealizeCover()                             array: "_highCover".., flanked
//
// Parameters come first in a frame's property chain and the array is a local,
// so SetHitPoints reads as four numbers and the last two are the ones drawn.
// The unit a damage number just floated over, whose flag is redrawn with its
// new hit points a moment later (UIUnitFlag.SetHitPoints, four calls after the
// message in the run of 2026-09-22). Those are said as the damage's second
// half: "Chryssalid, 6 damage." then "2 of 8 HP left."
//
// A damage number with nobody to name -- a Chryssalid's claws raise it from
// XComWeaponComponent_Melee.CustomFire, where no frame's object is a unit --
// is held for COMBAT_NAME_WAIT_MS and given to the first flag whose hit points
// drop in that time: "Hudson, 6 damage. 4 of 10 HP left." Said as it is if
// none does. And a named hit that no redraw follows may be the last one: the
// flag of the dead is never redrawn. combat_poll asks the unit, and says
// "down" if the game no longer counts it alive and visible.
#define COMBAT_HP_WAIT_MS   2000
#define COMBAT_NAME_WAIT_MS 1000
static const UnitName* g_hurt;
static void*           g_hurt_flag;     // g_hurt's flag, in case the slot is reused
static ULONGLONG       g_hurt_at;
static char            g_unnamed[COMBAT_MAX_TEXT];
static ULONGLONG       g_unnamed_at;

static void unit_flag_drew(void* flag, const char* fn, const Payload* p)
{
    if (strcmp(fn, "SetHitPoints") == 0) {
        if (p->nnumbers < 4) return;
        UnitName* u = unit_entry(flag);
        if (!u) return;
        int was = u->hp;
        u->hp = (int)p->numbers[2];
        u->hp_max = (int)p->numbers[3];
        ULONGLONG now = GetTickCount64();
        if (g_unnamed[0] && now - g_unnamed_at <= COMBAT_NAME_WAIT_MS && u->name[0] &&
            u->hp >= 0 && was >= 0 && u->hp < was) {
            char label[160], hp[64] = "", say[COMBAT_MAX_TEXT + 224];
            unit_label(u, label, sizeof label);
            combat_hp(u->hp, u->hp_max, hp, sizeof hp);
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s, %s%s%s", label, g_unnamed,
                        hp[0] ? " " : "", hp);
            g_unnamed[0] = 0;
            logf_("combat: the damage was %s's -> \"%s\"\n", u->name, say);
            announce_as(SET_COMBAT, say);
            return;
        }
        if (u == g_hurt && u->hp != was && now - g_hurt_at <= COMBAT_HP_WAIT_MS) {
            g_hurt = NULL;
            char say[64], label[160];
            if (combat_hp(u->hp, u->hp_max, say, sizeof say)) {
                logf_("combat: %s now %d of %d -> \"%s\"\n", u->name, u->hp, u->hp_max, say);
                if (g_speak && !muted()) speech_say(say);
                // Kept with the hit it belongs to: "Chryssalid, 4 damage. 4 of
                // 8 HP left." reads as one event in the list.
                unit_label(u, label, sizeof label);
                history_extend(label, say);
            }
        }
        return;
    }
    // The action pips: RealizeMoves sends SetMoves(n), n being
    // GetRemainingActions for a friendly unit with moves left and 0 otherwise.
    if (strcmp(fn, "RealizeMoves") == 0) {
        UnitName* u = unit_entry(flag);
        if (u && p->nnumbers > 0) u->moves = (int)p->numbers[p->nnumbers - 1];
        return;
    }
    // The turn ending greys every friendly flag out and empties its pips
    // (UIUnitFlag.EndTurn: SetDisabled, then SetMoves(0)).
    if (strcmp(fn, "EndTurn") == 0) {
        UnitName* u = unit_entry(flag);
        if (u) u->moves = 0;
        return;
    }
    // The panic readout: RealizeEKG sends SetEKGState(1) when the unit
    // panics and SetEKGState(0) when it stops -- only ever on a change.
    if (strcmp(fn, "RealizeEKG") == 0) {
        UnitName* u = unit_entry(flag);
        if (u && p->nnumbers > 0) u->panicked = p->numbers[p->nnumbers - 1] != 0;
        return;
    }
    // Critically wounded: SetCriticallyWounded(bleeding, turns), sent for a
    // soldier whose m_iCriticalWoundCounter is running; bleeding false is one
    // who has been stabilised.
    if (strcmp(fn, "RealizeCriticallyWounded") == 0) {
        UnitName* u = unit_entry(flag);
        if (!u) return;
        int bleeding = p->nabools ? p->abools[0] : (p->nbools ? p->bools[0] : 1);
        u->wounded = bleeding ? SOLDIER_BLEEDING : SOLDIER_STABILISED;
        u->bleed_turns = p->nnumbers > 0 ? (int)p->numbers[p->nnumbers - 1] : 0;
        return;
    }
    // The selection marker: UIUnitFlag.Update runs SetSelected(true) and
    // ShowExtension on the new active unit's flag. It reaches Flash once per
    // change of selection, after the flags exist -- which the panel's
    // SetStats does not wait for at a mission's start -- so it is what
    // announces a switch (soldier_poll).
    if (strcmp(fn, "ShowExtension") == 0) {
        soldier_selected(flag);
        return;
    }
    // The bonus and penalty markers: whether there are any, not which.
    if (strcmp(fn, "ShowBuff") == 0 || strcmp(fn, "ShowDebuff") == 0) {
        UnitName* u = unit_entry(flag);
        if (!u || !p->nbools) return;
        if (fn[4] == 'B') u->buff = p->bools[0] != 0;
        else u->debuff = p->bools[0] != 0;
        return;
    }
    if (strcmp(fn, "RealizeCover") == 0) {
        UnitName* u = unit_entry(flag);
        if (!u) return;
        u->cover[0] = 0;
        for (int i = 0; i < p->nstrings; i++)
            if (p->strings[i][0] == '_') {
                strncpy_s(u->cover, sizeof u->cover, p->strings[i], _TRUNCATE);
                break;
            }
        u->flanked = p->nabools ? p->abools[0] : -1;
    }
}

// One floating combat message, said. Which unit it floats over is found from
// the calls that raised it, not from its position: DamageDisplay and the rest
// are called from inside the unit's own functions (XGUnit.OnTakeDamage ->
// UIWorldMessageMgr.DamageDisplay -> Message -> CreateNewMessage), so a frame
// above has that XGUnit as its object. DamageDisplay in the chain is what
// makes it a damage number.
#define COMBAT_CHAIN_DEPTH 10
#define COMBAT_REPEAT_MS   500

static void combat_message(LONG n, void* stack, const Payload* p)
{
    // The text is whichever string is not the message's id. The pool is built
    // at mission start with ids and no text ("worldMessageBox0".."7"), and
    // the cursor's own help ("cursorHelp_Dashing", "Dashing!") is left to
    // navigation, which already says "Dash".
    const char* text = NULL;
    for (int i = 0; i < p->nstrings; i++) {
        const char* s = p->strings[i];
        if (strncmp(s, "cursorHelp", 10) == 0) return;
        if (!s[0] || strncmp(s, "worldMessageBox", 15) == 0 || looks_like_asset(s)) continue;
        if (!text) text = s;
    }
    if (!text) return;

    int damage = 0;
    UnitName* who = NULL;
    char chain[256] = "";
    size_t used = 0;
    void* frame = stack;
    for (int depth = 0; frame && depth < COMBAT_CHAIN_DEPTH; depth++) {
        if (!readable(frame, FFRAME_PREVIOUS + sizeof(void*))) break;
        char name[128];
        if (!object_name(*(void**)((uint8_t*)frame + FFRAME_NODE), name, sizeof name)) break;
        if (used < sizeof chain) {
            int w = _snprintf_s(chain + used, sizeof chain - used, _TRUNCATE,
                                depth ? " <- %s" : "%s", name);
            if (w > 0) used += (size_t)w;
        }
        if (strcmp(name, "DamageDisplay") == 0) damage = 1;
        void* obj = *(void**)((uint8_t*)frame + FFRAME_OBJECT);
        if (!who && obj) who = unit_by_unit(obj);
        // "Missed!" comes from XGAction_Fire, not from a unit, and floats
        // where the shot went; the action's target is who was missed.
        if (!who && obj) {
            static FieldSlot missed;
            char oname[128];
            const void* v;
            if (object_name(obj, oname, sizeof oname) &&
                strncmp(oname, "XGAction_Fire", 13) == 0 &&
                field_ptr(obj, "m_kTargetedEnemy", &missed, sizeof(void*), &v) &&
                *(void* const*)v)
                who = unit_by_unit(*(void* const*)v);
        }
        frame = *(void**)((uint8_t*)frame + FFRAME_PREVIOUS);
    }

    char label[160] = "";
    if (who) unit_label(who, label, sizeof label);
    char say[COMBAT_MAX_TEXT];
    if (!combat_describe(label, text, damage, say, sizeof say)) return;

    // Message can be asked twice for one event (an update of a message that
    // is already up); the same words twice in a moment are one event.
    static char said[COMBAT_MAX_TEXT];
    static ULONGLONG said_at;
    ULONGLONG now = GetTickCount64();
    if (strcmp(say, said) == 0 && now - said_at < COMBAT_REPEAT_MS) return;
    strncpy_s(said, sizeof said, say, _TRUNCATE);
    said_at = now;

    logf_("[%ld] combat: \"%s\"  (%s)%s\n", n, say, chain,
          damage && !who ? " -- held for the flag it lands on" : "");
    if (damage && !who) {
        // Only a figure: without one ("Missed!" over nobody known) there is
        // no hit-point drop to wait for.
        if (isdigit((unsigned char)say[0])) {
            if (g_unnamed[0]) announce_as(SET_COMBAT, g_unnamed);
            strncpy_s(g_unnamed, sizeof g_unnamed, say, _TRUNCATE);
            g_unnamed_at = now;
            return;
        }
    }
    announce_as(SET_COMBAT, say);
    if (damage && who) {
        g_hurt = who;
        g_hurt_flag = who->flag;
        g_hurt_at = now;
    }
}

// An event, said and kept (history.h): combat, whose turn it is, the ticker.
// Queued, never interrupting, since events come in bursts. Kept even when the
// mod is muted, so what was missed can still be read back.
static void announce(const char* text)
{
    if (!text || !*text) return;
    history_add(text);
    if (g_speak && !muted()) speech_say(text);
}

// An announcement the options menu can silence (settings.h). Switched off it
// is still kept for Insert, so nothing is lost -- only not said as it comes.
// -1 is a line no setting covers.
static void announce_as(int setting, const char* text)
{
    if (!text || !*text) return;
    if (setting < 0 || settings_get(setting)) { announce(text); return; }
    history_add(text);
    logf_("options: %s is off -- kept for Insert, not said\n", settings_name(setting));
}

// Every frame, from the cursor's per-frame native: the combat lines that wait.
static void combat_poll(void)
{
    ULONGLONG now = GetTickCount64();
    if (g_unnamed[0] && now - g_unnamed_at > COMBAT_NAME_WAIT_MS) {
        logf_("combat: no flag took the damage -- \"%s\" said as it is\n", g_unnamed);
        announce_as(SET_COMBAT, g_unnamed);
        g_unnamed[0] = 0;
    }
    if (g_hurt && now - g_hurt_at > COMBAT_HP_WAIT_MS) {
        const UnitName* u = g_hurt;
        void* flag = g_hurt_flag;
        g_hurt = NULL;
        char name[64];
        strncpy_s(name, sizeof name, u->name, _TRUNCATE);
        Fault f;
        int gone = 0;
        __try { gone = unit_gone(u, flag); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("combat: gone", &f, NULL);
        }
        if (gone && name[0]) {
            char say[96];
            _snprintf_s(say, sizeof say, _TRUNCATE, "%s down.", name);
            logf_("combat: no redraw after the hit, and %s is gone -> \"%s\"\n", name, say);
            announce_as(SET_COMBAT, say);
        }
    }
}

// Who the shot being announced is aimed at, handed to shot.c.
//
// UpdateLayout is called from UITacticalHUD_InfoPanel.Update(XGUnit kUnit,
// XGAbility kAbility) and from nowhere else, in either build, so the frame
// above it holds the soldier as its first parameter. From there it is fields
// all the way: the soldier's m_kCurrAction is the targeting action
// (XGAction_Targeting in EW, XGAction_Fire in EU, the same field names in
// both), whose m_kTargetedEnemy is what Tab cycles through
// m_arrInteractionList_ConstrainedByAbilities. By the time Tab re-runs Update
// the new target is already set: NextTarget assigns it, then
// UITacticalHUD.SelectNextTarget rebuilds the ability menu, which is what
// calls Update.
//
// The parameter is taken by position, not by name, for the reason the traps
// section of the handoff gives.
static FieldSlot g_curr_action, g_targeted, g_target_list;
static void*     g_shot_node;           // Update, once confirmed
static uint32_t  g_shot_unit_off;       // its first parameter
static int       g_shot_node_logged;
static char      g_shot_target_logged[SHOT_MAX_TEXT];

static void shot_target_now(void* stack)
{
    shot_set_target("");
    if (!readable((uint8_t*)stack + FFRAME_PREVIOUS, sizeof(void*))) return;
    void* frame = *(void**)((uint8_t*)stack + FFRAME_PREVIOUS);
    if (!frame || !readable(frame, FFRAME_PREVIOUS + sizeof(void*))) return;
    void* node = *(void**)((uint8_t*)frame + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)frame + FFRAME_LOCALS);
    if (!node || !locals) return;

    if (node != g_shot_node) {
        char name[128];
        uint32_t off = 0;
        int found = 0;
        if (object_name(node, name, sizeof name) && strcmp(name, "Update") == 0) {
            void* prop = NULL;
            if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
                prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
            for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
                if (!readable(prop, 0x68)) break;
                uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
                if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
                    off = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
                    found = off < 0x1000;
                    break;
                }
                prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
            }
        }
        if (!found) {
            if (!g_shot_node_logged) {
                g_shot_node_logged = 1;
                logf_("shot: the frame above UpdateLayout is %s, not Update(kUnit, ..) "
                      "-- the target will not be named\n",
                      object_name(node, name, sizeof name) ? name : "unreadable");
            }
            return;
        }
        g_shot_node = node;
        g_shot_unit_off = off;
        logf_("shot: soldier is Update's first parameter, +0x%X\n", off);
    }

    if (!readable(locals + g_shot_unit_off, sizeof(void*))) return;
    void* unit = *(void**)(locals + g_shot_unit_off);
    const void* v;
    if (!unit || !unit_is_live(unit) ||
        !field_ptr(unit, "m_kCurrAction", &g_curr_action, sizeof(void*), &v))
        return;
    void* action = *(void* const*)v;
    if (!action || !unit_is_live(action) ||
        !field_ptr(action, "m_kTargetedEnemy", &g_targeted, sizeof(void*), &v))
        return;
    void* target = *(void* const*)v;
    // Hunker Down, Reload and Overwatch aim at the soldier using them, and
    // naming the soldier back to the player says nothing. Stabilize and
    // Revive aim at somebody else, who is worth naming.
    if (!target || target == unit) return;

    UnitName* u = unit_by_unit(target);
    if (!u) return;

    ShotTarget t;
    char label[160];
    unit_label(u, label, sizeof label);
    t.name = label;
    t.cover = u->cover;
    t.flanked = u->flanked;
    t.hp = u->hp;
    t.hp_max = u->hp_max;
    t.index = -1;
    t.count = 0;
    if (field_ptr(action, "m_arrInteractionList_ConstrainedByAbilities",
                  &g_target_list, sizeof(FArray), &v)) {
        const FArray* a = (const FArray*)v;
        if (a->Num > 0 && a->Num <= SEEN_MAX &&
            readable(a->Data, (size_t)a->Num * sizeof(void*))) {
            void* const* e = (void* const*)a->Data;
            for (int k = 0; k < a->Num; k++)
                if (e[k] == target) { t.index = k; break; }
            if (t.index >= 0) t.count = a->Num;
        }
    }

    char text[SHOT_MAX_TEXT];
    shot_describe_target(&t, text, sizeof text);
    shot_set_target(text);
    if (strcmp(text, g_shot_target_logged) != 0) {
        strncpy_s(g_shot_target_logged, sizeof g_shot_target_logged, text, _TRUNCATE);
        logf_("shot: target \"%s\" (cover %s, flanked %d, hp %d/%d, %d of %d)\n",
              text, u->cover[0] ? u->cover : "?", u->flanked, u->hp, u->hp_max,
              t.index + 1, t.count);
    }
}

// The target strip: the row of enemy icons, UISightlineHUD_SightlineContainer.
// Its m_arrEnemies is the list it draws, in the order it draws them -- the
// soldier's visible enemies, or everything in squadsight range for a sniper
// with the upgrade, less the critically wounded -- and each icon is marked by
// index with AS_SetFlanked(TargetIndex, isFlanked), which is
// IsFlankedBy(the active soldier). The mark is kept on the enemy's flag entry
// rather than by index, because the indices are re-dealt on every change.
static void*     g_strip;
static FieldSlot g_strip_enemies;

// The strip's list, or 0 when it cannot be read.
static int strip_enemies(void* const** out)
{
    const void* v;
    if (!g_strip || !unit_is_live(g_strip) ||
        !field_ptr(g_strip, "m_arrEnemies", &g_strip_enemies, sizeof(FArray), &v))
        return 0;
    const FArray* a = (const FArray*)v;
    if (a->Num <= 0 || a->Num > SEEN_MAX ||
        !readable(a->Data, (size_t)a->Num * sizeof(void*)))
        return 0;
    *out = (void* const*)a->Data;
    return a->Num;
}

static void strip_note(void* strip, const char* fn, const Payload* p)
{
    g_strip = strip;
    if (!strstr(fn, "SetFlanked") || p->nnumbers < 1 || p->nbools < 1) return;
    void* const* e;
    int n = strip_enemies(&e);
    int id = (int)p->numbers[0];
    if (id < 0 || id >= n) return;
    UnitName* u = unit_by_unit(e[id]);
    if (u) u->strip_flanked = p->bools[0];
}

// ---- the walls around a tile -----------------------------------------------
//
// Walls in XCOM sit between tiles, not on them: that is why cover is named by
// side -- COVER_North is a wall on this tile's northern edge -- and why
// IsTileOccupied, which asks whether a tile is filled with solid stuff, finds
// pillars and trucks but walks straight through a partition. So the scan asks
// both questions of every tile within range:
//
//   its cover bits, each of which is a wall face on one of its edges, and
//   IsTileOccupied, which is a solid object standing on the tile itself.
//
// Every face found is one emitter in the field (sonar.h): a wall face sounds
// from the edge it is on, a solid tile from its own middle. Nothing in range
// is open ground, and is silence.
//
// Each wall is read once, not twice. A wall between two tiles shows in one's
// north bit and the other's south, so only each tile's own north and east
// faces are taken; the other two belong to its neighbours and are picked up
// when the scan reaches them. Reading all four would count every wall twice,
// which would make a partition as loud as two walls and a corner louder than
// either.
//
// Two limits worth naming. A tile whose cover frame is turned 45 degrees
// (COVER_Diagonal) describes its corners rather than its sides, so it has
// nothing to say about any of the four and its cover bits are passed over -- a
// diagonal wall is heard only as whatever solid stands behind it. And every
// tile in range is asked about at the floor height of the tile being listened
// from, because that is the only floor the mod knows; where the ground changes
// level within range the game answers for whatever tile it finds there
// instead, which the check on the cover point's own coordinates below turns
// into silence rather than into a wall that is not there.
//
// The game's compass is the mirror of the mod's (see tile.h): its East is this
// mod's west. So the two faces read off each tile are the game's North bit --
// the mod's north -- and its West bit, which is the mod's east.
// Only the two bits that say a wall is there. The matching low-cover bits are
// deliberately not read: low cover blocks the way as surely as a wall and the
// field says so, and whether it is waist high is said in words (tile_describe)
// rather than folded into a level that has to carry distance.
#define WALL_N_BIT      TILE_COVER_N
#define WALL_E_BIT      TILE_COVER_W

// How far the scan reaches, in whole tiles: everything the range can hear.
#define WALL_TILES ((int)SONAR_RANGE)

static int walls_scan(const CursorGrid* g, int tx, int ty, float floor,
                      SonarField* out)
{
    void* world = cursor_world();
    sonar_field_clear(out);
    if (!world) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;
    TileTestFn occupied = (TileTestFn)tile_vfn(world, g_tile_slot_occupied);

    float z = floor + 4.0f;
    int tz = cursor_tile_axis(z, g->min_z, 64.0f);

    for (int dy = -WALL_TILES; dy <= WALL_TILES; dy++) {
        for (int dx = -WALL_TILES; dx <= WALL_TILES; dx++) {
            int x = tx + dx, y = ty + dy;
            if (x < 0 || y < 0 || x >= g->num_x || y >= g->num_y) {
                // Off the map. The edge stops a soldier as surely as a wall
                // does, and a player walking towards it should hear it
                // coming, so the tile that is not there sounds as solid.
                sonar_block(out, (float)dx, (float)dy);
                continue;
            }

            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            float wx = g->min_x + ((float)x + 0.5f) * CURSOR_TILE;
            float wy = g->min_y + ((float)y + 0.5f) * CURSOR_TILE;
            // An answer about some other tile is an answer about some other
            // floor, and is worth less than no answer at all.
            if (cover(world, NULL, wx, wy, z, &cp) && cp.x == x && cp.y == y &&
                !(cp.flags & TILE_COVER_DIAGONAL)) {
                if (cp.flags & WALL_N_BIT)
                    sonar_face(out, SONAR_AXIS_NS, (float)dx, (float)dy + 0.5f);
                if (cp.flags & WALL_E_BIT)
                    sonar_face(out, SONAR_AXIS_EW, (float)dx + 0.5f, (float)dy);
            }

            // A solid tile sounds from where it stands, and has no side to it.
            // The tile being listened from is not one of them -- sonar_block
            // drops one with no bearing -- which is right: what fills the
            // cursor's own tile is not a wall around it.
            if (occupied && occupied(world, NULL, x, y, tz))
                sonar_block(out, (float)dx, (float)dy);
        }
    }
    sonar_field_finish(out);
    return 1;
}

// The field, handed to the mixer. Silent where the scan cannot run at all --
// no world data, no cover slot -- because a field meaning "the mod could not
// ask" would be indistinguishable from one meaning "open".
//
// The tile listened from is listen_tile's, below. A scan is two questions of
// the game about each of the (2 * SONAR_RANGE + 1)^2 tiles in range, so it is rescanned when that tile
// changes and at most every WALLS_SCAN_MS; while the tile does not change, only
// every WALLS_IDLE_MS, which is there to catch a wall being blown up rather
// than to track the player. Between scans the mixer's own glide carries the
// level, so a tile crossed faster than the scan rate loses nothing but detail.
#define WALLS_SCAN_MS   40
#define WALLS_IDLE_MS  250
#define WALLS_LOG_MS   400

static SonarField g_walls_field;        // the last scan, renewed every frame
static int        g_walls_have;          // a tile has been scanned
static int        g_walls_tile[2];
static ULONGLONG  g_walls_at;
static ULONGLONG  g_walls_logged;

// Where the field and the hearts listen from:
//   - the navigation target while one is held;
//   - the cursor while aiming, since the cursor is the aim;
//   - otherwise the selected soldier.
// Not the cursor when nothing is held: in mouse mode it sits under the mouse,
// and the mouse is parked mid-window (mouse.h), which in the 2026-09-25 log
// was 24 tiles from the soldier after every switch -- 28, 35 for Vargas on
// 28, 11. The cursor is the fallback only when the soldier cannot be read.
// The soldier's Location.Z runs a few units above the cursor's resting height
// (83.1 against 80.0 on the same tile), so it takes the same lift off; the
// cursor's own height is used when the two share a tile, as nav_press does.
static int listen_tile(const CursorGrid* g, int* tx, int* ty, float* floor)
{
    if (nav_active() && nav_target(tx, ty)) {
        *floor = navh_ground();
        return 1;
    }
    float z;
    int have_cursor = cursor_tile(g, tx, ty, &z);
    if (!soldier_aiming()) {
        int sx, sy;
        float sz;
        if (soldier_tile(g, &sx, &sy, &sz) &&
            sx >= 0 && sy >= 0 && sx < g->num_x && sy < g->num_y) {
            if (!have_cursor || sx != *tx || sy != *ty) z = sz;
            *tx = sx;
            *ty = sy;
            *floor = z - NAV_CURSOR_LIFT;
            return 1;
        }
    }
    if (!have_cursor) return 0;
    *floor = z - NAV_CURSOR_LIFT;
    return 1;
}

static void walls_quiet(void)
{
    audio_field_off();
    g_walls_have = 0;
}

static void walls_poll(void)
{
    CursorGrid g;
    int tx, ty;
    float floor;

    // Switched off from here or from the options menu: quiet once, on the
    // change, since the menu's thread must not touch the field itself.
    static int was_on = 1;
    int on = settings_get(SET_FIELD);
    if (!on && was_on) walls_quiet();
    was_on = on;
    if (!on || !audio_available()) return;
    if (!cursor_grid(&g)) { walls_quiet(); return; }

    if (!listen_tile(&g, &tx, &ty, &floor)) { walls_quiet(); return; }

    ULONGLONG now = GetTickCount64();
    int same = g_walls_have && tx == g_walls_tile[0] && ty == g_walls_tile[1];
    if (g_walls_have &&
        now - g_walls_at < (ULONGLONG)(same ? WALLS_IDLE_MS : WALLS_SCAN_MS)) {
        // The scan is what is throttled, not the field: the mixer lets an
        // unrenewed field lapse (audio.h), so the last one has to be handed
        // over again every frame to say it still holds.
        audio_field(&g_walls_field);
        return;
    }
    g_walls_at = now;

    Fault flt;
    __try {
        if (!walls_scan(&g, tx, ty, floor, &g_walls_field)) { walls_quiet(); return; }
    }
    __except (fault_note(GetExceptionInformation(), &flt)) {
        fault_log("walls: scan", &flt, NULL);
        walls_quiet();
        return;
    }
    audio_field(&g_walls_field);
    g_walls_have = 1;
    g_walls_tile[0] = tx;
    g_walls_tile[1] = ty;

    // On a change of tile, and rate limited: a glide crosses twenty tiles a
    // second and a line for each would bury everything else in the log.
    if (!same && now - g_walls_logged >= WALLS_LOG_MS) {
        g_walls_logged = now;
        logf_("walls: %d, %d floor %.1f -- W %.2f N %.2f S %.2f E %.2f\n",
              tx, ty, floor,
              g_walls_field.level[SONAR_W], g_walls_field.level[SONAR_N],
              g_walls_field.level[SONAR_S], g_walls_field.level[SONAR_E]);
    }
}

// Heartbeats (heart.h), allies' and seen enemies', heard from the tile the
// field listens from. The units are read at most every HEARTS_SCAN_MS --
// unit_seen calls into the game for each flag -- and what was read is handed
// to the mixer every frame, as the field is, so it lapses when this stops
// being called.
#define HEARTS_SCAN_MS 150
#define HEARTS_MAX     32

// Every 5 s in a mission, a line saying how the frames went: how many there
// were, and the worst the wall field and the hearts each took of one on the
// game's thread. Written after a report of slowdowns while moving with beeps
// on, when the log had no timings to tell a slow frame from a slow decision.
#define PERF_MS 5000

static void perf_note(long long walls_ticks, long long hearts_ticks)
{
    static LARGE_INTEGER freq;
    static ULONGLONG since;
    static int frames;
    static long long walls_max, hearts_max;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    ULONGLONG now = GetTickCount64();
    if (!since || now - since > 4 * PERF_MS) {
        // First frame, or back from a stretch with no mission: start afresh
        // rather than average the gap in.
        since = now;
        frames = 0;
        walls_max = hearts_max = 0;
    }
    frames++;
    if (walls_ticks > walls_max) walls_max = walls_ticks;
    if (hearts_ticks > hearts_max) hearts_max = hearts_ticks;
    if (now - since < PERF_MS) return;
    double ms = 1000.0 / (double)freq.QuadPart;
    static unsigned walks_seen;
    logf_("perf: %.1f frames a second; worst frame's walls %.2f ms, hearts %.2f ms; "
          "%u field walks\n",
          frames * 1000.0 / (double)(now - since), walls_max * ms, hearts_max * ms,
          g_field_walks - walks_seen);
    walks_seen = g_field_walks;
    since = now;
    frames = 0;
    walls_max = hearts_max = 0;
}

// Door and window sounds (SET_DOORS, SET_WINDOWS): every door and window
// within DOOR_RANGE tiles of where the field listens from, one sound per
// doorway or window, taking turns in the mixer, in one round together. The
// doors are the scanner's (world_refresh), refreshed every DOORS_SCAN_MS --
// doors do not move -- and the first refresh of a mission is the object walk
// the scanner would otherwise make on its first press.
#define DOOR_RANGE      10
#define DOORS_SCAN_MS   1000
#define DOOR_ALONE_S    2.7f    // a door with no other near knocks this often
#define DOORS_MAX       64

// One entry per doorway or window tile, kept for the mission: its address is
// the id in the mixer, so it keeps its place in the round. A double door is
// two actors on one tile, and is one entry. [2] is its HEART_* kind.
static int   g_door_tile[DOORS_MAX][3];
static int   g_door_n;
static void* g_door_map;        // the cursor the table was built for
static int   g_door_near[DOORS_MAX];    // this refresh's doors within range
static int   g_door_near_n;

// Below the scanner, whose world list it reads.
static void doors_refresh(const CursorGrid* g, int tx, int ty, int doors, int windows);

// "Follow one soldier" (settings.h, SET_HEART_SOLO): the soldier last picked
// in the scanner, by the name the scanner gives them (unit_label). Set by
// scan_say_selected, on the same thread as hearts_poll.
static char g_heart_follow[SCAN_NAME];

static void hearts_poll(void)
{
    static const void* ids[HEARTS_MAX];
    static HeartSound  sounds[HEARTS_MAX];
    static int         n = -1;          // -1: nothing read yet
    static ULONGLONG   at;
    static int         was_on = 1;
    static int         logged = -1;

    int allies = settings_get(SET_HEARTS) && audio_hearts_available(HEART_ALLY);
    int solo = settings_get(SET_HEART_SOLO);
    int aliens = settings_get(SET_ALIENS) && audio_hearts_available(HEART_ALIEN);
    int doors = settings_get(SET_DOORS) && audio_hearts_available(HEART_DOOR);
    int windows = settings_get(SET_WINDOWS) && audio_hearts_available(HEART_WINDOW);
    int on = allies || aliens || doors || windows;
    if (!on && was_on) audio_hearts_off();
    was_on = on;
    if (!on) return;

    CursorGrid g;
    int tx, ty;
    if (!cursor_grid(&g)) { audio_hearts_off(); return; }
    float floor;
    if (!listen_tile(&g, &tx, &ty, &floor)) { audio_hearts_off(); return; }

    ULONGLONG now = GetTickCount64();
    if (n >= 0 && now - at < HEARTS_SCAN_MS) {
        audio_hearts(ids, sounds, n);
        return;
    }
    at = now;

    void* squad = squad_player();
    if (!squad) { audio_hearts_off(); n = -1; return; }
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);

    Fault flt;
    int k = 0, seen_aliens = 0, near_doors = 0, near_windows = 0;
    static SeenSet sight;
    static ULONGLONG doors_at;
    __try {
        if (!doors && !windows) g_door_near_n = 0;
        else if (!doors_at || now - doors_at >= DOORS_SCAN_MS) {
            doors_at = now;
            doors_refresh(&g, tx, ty, doors, windows);
        }
        for (int d = 0; d < g_door_near_n && k < HEARTS_MAX; d++) {
            int* t = g_door_tile[g_door_near[d]];
            // Checked here too, so a switch in the menu is heard at once
            // rather than at the next refresh.
            if (t[2] == HEART_DOOR ? !doors : !windows) continue;
            heart_sound(t[0] - tx, t[1] - ty, -1, -1, 0, SOLDIER_WOUND_NONE, &sounds[k]);
            sounds[k].kind = t[2];
            sounds[k].period = DOOR_ALONE_S;
            if (t[2] == HEART_DOOR) near_doors++; else near_windows++;
            ids[k++] = t;
        }
        // Enemies only while a squad member sees them: the radar's rule.
        if (aliens) squad_sight(squad, &sight);
        for (int i = 0; i < g_nunits && k < HEARTS_MAX; i++) {
            UnitSeen s;
            if (!unit_seen(&g_units[i], squad, &s)) continue;
            if (s.friendly ? !allies : (!aliens || !seen_has(&sight, s.unit))) continue;
            const UnitName* u = &g_units[i];
            int followed = 0;
            if (s.friendly && solo) {
                char label[SCAN_NAME];
                unit_label(u, label, sizeof label);
                if (!g_heart_follow[0] || strcmp(label, g_heart_follow) != 0) continue;
                followed = 1;
            }
            int dx = cursor_tile_axis(s.loc[0], g.min_x, CURSOR_TILE) - tx;
            int dy = cursor_tile_axis(s.loc[1], g.min_y, CURSOR_TILE) - ty;
            // The selected soldier is where the player is listening from
            // until they navigate away; then their heart marks the spot.
            // One the player chose to follow is heard even there.
            if (s.pawn == soldier && !dx && !dy && !followed) continue;
            if (s.friendly) {
                heart_sound(dx, dy, u->hp, u->hp_max, u->panicked, u->wounded, &sounds[k]);
            } else {
                heart_sound(dx, dy, u->hp, u->hp_max, 0, SOLDIER_WOUND_NONE, &sounds[k]);
                sounds[k].kind = HEART_ALIEN;
                seen_aliens++;
            }
            ids[k++] = s.unit;
        }
    }
    __except (fault_note(GetExceptionInformation(), &flt)) {
        fault_log("hearts: squad", &flt, NULL);
        audio_hearts_off();
        n = -1;
        return;
    }
    n = k;
    audio_hearts(ids, sounds, n);
    int sig = ((n * 64 + seen_aliens) * 64 + near_doors) * 64 + near_windows;
    if (sig != logged) {
        logged = sig;
        logf_("hearts: %d sounding: %d aliens, %d doors, %d windows\n", n, seen_aliens,
              near_doors, near_windows);
    }
}

// Says the pending step: anyone standing there, `body`, then the
// coordinates. Once per step.
// The floor the step stands on, as far as it is known: the aim's, or the
// ground the height search settled on.
static float step_floor(void)
{
    return g_nav_aim ? g_aim_floor : navh_ground();
}


// The units in the target's column, worded against the step's floor:
// "Godongwana, one floor up." With `same_only`, only those on that floor --
// the ones that can be standing in the way. Inside a building the floors are
// its own storeys (where_levels_between); elsewhere, storeys of 192.
static void step_who(int same_only, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    float floor = step_floor();
    int tx = 0, ty = 0;
    int have_tile = nav_target(&tx, &ty);
    for (int i = 0; i < g_step_nunits; i++) {
        int dz = scan_storey_diff(g_step_units[i].feet, floor);
        int levels;
        if (have_tile && dz && where_levels_between(tx, ty, floor, g_step_units[i].feet, &levels))
            dz = levels;
        if (same_only && dz) continue;
        char piece[TILE_MAX_TEXT];
        scan_unit_floor_text(g_step_units[i].label, dz, piece, sizeof piece);
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s",
                            used ? " " : "", piece);
        if (w < 0) break;
        used += (size_t)w;
    }
}

static int nav_step_say(const char* body)
{
    if (!g_step_pending) return 0;
    g_step_pending = 0;
    char who[TILE_MAX_TEXT];
    step_who(0, who, sizeof who);
    char say[TILE_MAX_TEXT + TILE_MAX_TEXT + NAV_MAX_TEXT + sizeof g_step_note +
             sizeof g_step_where];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s%s%s%s%s.", g_step_note,
                g_step_note[0] ? " " : "", g_step_where,
                g_step_where[0] ? " " : "", who,
                who[0] && body[0] ? " " : "", body,
                who[0] || body[0] ? " " : "", g_step_coords);
    g_step_note[0] = 0;
    g_step_where[0] = 0;
    logf_("nav: said \"%s\"\n", say);
    speech_say_now(say);
    return 1;
}

// Why a tile no path reaches is refused, from the game's own tile flags (see
// tile.h). The floor is not known -- that is what failed -- so every layer the
// height probe tried is asked about: the ground's layer and three either side,
// which is PROBE_HEIGHTS' span. `say` stays "No path." whenever the game
// cannot be asked.
#define REFUSAL_LAYERS 3

// The game's own flags for the layers of a tile's column, `span` either side
// of the ground's layer: floor, a valid destination, occupied. Returns how
// many layers were asked, or -1 when the game cannot be asked; `seen` gets
// them for the log, "7:FD-" per layer, and `mid` the ground's layer.
#define QUICK_LAYERS 6

static int tile_layers(int tx, int ty, float ground, int span, TileLayerFlags* layers,
                       int max, char* seen, size_t seen_sz, int* mid_out)
{
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return -1;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    PositionTestFn standable = (PositionTestFn)tile_vfn(world, g_tile_slot_standable);
    TileTestFn occupied = (TileTestFn)tile_vfn(world, g_tile_slot_occupied);
    if (!on_floor || !standable) return -1;

    // The layer is found as tile_report finds it, from the floor plus 4.
    int mid = cursor_tile_axis(ground + 4.0f, g.min_z, 64.0f);
    if (mid_out) *mid_out = mid;
    size_t used = 0;
    int n = 0;
    seen[0] = 0;
    for (int tz = mid - span; tz <= mid + span && n < max; tz++) {
        if (tz < 0 || (g.num_z > 0 && tz >= g.num_z)) continue;
        // The middle of the layer: the natives make a tile of it themselves,
        // and the middle is as far as can be from either edge's rounding.
        float pos[3] = {
            g.min_x + ((float)tx + 0.5f) * CURSOR_TILE,
            g.min_y + ((float)ty + 0.5f) * CURSOR_TILE,
            g.min_z + ((float)tz + 0.5f) * 64.0f,
        };
        TileLayerFlags* l = &layers[n++];
        l->floor = on_floor(world, NULL, pos) != 0;
        l->destination = standable(world, NULL, pos) != 0;
        l->occupied = occupied ? occupied(world, NULL, tx, ty, tz) != 0 : 0;
        l->below = tz < mid;
        // "7:FD-" -- floor, destination, occupied, per layer, for the log.
        int w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, "%s%d:%c%c%c",
                            used ? " " : "", tz, l->floor ? 'F' : '-',
                            l->destination ? 'D' : '-', l->occupied ? 'O' : '-');
        if (w > 0) used += (size_t)w;
    }
    return n;
}

static void tile_refusal_probe(int tx, int ty, float ground, char* say, size_t say_sz)
{
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(TILE_REFUSE_NO_PATH));
    TileLayerFlags layers[2 * REFUSAL_LAYERS + 1];
    char seen[(2 * REFUSAL_LAYERS + 1) * 16];
    int mid;
    int n = tile_layers(tx, ty, ground, REFUSAL_LAYERS, layers,
                        2 * REFUSAL_LAYERS + 1, seen, sizeof seen, &mid);
    if (n < 0) return;
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(tile_refusal(layers, n)));
    logf_("nav: %d, %d refused, ground %.1f (layer %d), layers %s -> \"%s\"\n",
          tx, ty, ground, mid, seen, say);
}

// A step onto a tile nothing can stand on, decided the moment it lands. The
// height search needs a path answer per height, and the game gives about ten
// a second: on 48, 6 (2026-09-23), where every height failed, the search and
// the probe never finished inside STEP_FALLBACK_MS, and all four visits were
// 1.5 s of silence and then the bare coordinates, "No path" never said. The
// tile's flags say the same thing at once: if no layer within QUICK_LAYERS of
// the ground -- further than the search reaches -- is a place a move may end
// (IsPositionOnFloorAndValidDestination), no height will build a path. Only
// then is the step decided here; a tile with any destination on it searches
// as ever, so nothing reachable is refused early.
static int tile_blocked_now(int tx, int ty, float ground, char* say, size_t say_sz)
{
    TileLayerFlags layers[2 * QUICK_LAYERS + 1];
    char seen[(2 * QUICK_LAYERS + 1) * 16];
    int mid;
    int n = tile_layers(tx, ty, ground, QUICK_LAYERS, layers, 2 * QUICK_LAYERS + 1,
                        seen, sizeof seen, &mid);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) if (layers[i].destination) return 0;
    _snprintf_s(say, say_sz, _TRUNCATE, "%s", tile_refusal_text(tile_refusal(layers, n)));
    logf_("nav: %d, %d decided on arrival, ground %.1f (layer %d), layers %s -> \"%s\"\n",
          tx, ty, ground, mid, seen, say);
    return 1;
}

// The floor an aim lands on at a tile. The aim is the cursor's feet, and
// nav_aim_substitute puts in a height as well as the tile -- only X and Y went
// in at first, and every aim of the 2026-09-22 run sat at 259.2, the height of
// the soldier on the roof who was lent to it, three storeys above the
// Chryssalid it was fired at. So the height is the tile's own floor: the first
// layer at or below the aim's current floor that the game marks as floor
// (IsPositionOnFloor), which steps off a roof onto the ground and stays on a
// roof walked along; failing that, the nearest above, a few layers up.
//
// The layer only says a floor is somewhere inside it: 64 units. The exact
// height is then asked of GetFloorZForPosition from the layer's top, which
// searches down and finds it (floors were found from 64 above in the height
// probe, never from 247). The layer's bottom stands in if that cannot be
// called: 0 lies in layer -1..63 on the map with Min.Z -193. `from` is
// returned when the game cannot be asked or finds nothing.
#define AIM_FLOOR_UP 3

static float aim_floor(const CursorGrid* g, int tx, int ty, float from)
{
    void* world = cursor_world();
    if (!world) return from;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    if (!on_floor) return from;
    int start = cursor_tile_axis(from + 4.0f, g->min_z, 64.0f);
    float pos[3] = {
        g->min_x + ((float)tx + 0.5f) * CURSOR_TILE,
        g->min_y + ((float)ty + 0.5f) * CURSOR_TILE,
        0.0f,
    };
    Fault f;
    __try {
        for (int tz = start; tz >= 0; tz--) {
            if (g->num_z > 0 && tz >= g->num_z) continue;
            pos[2] = g->min_z + ((float)tz + 0.5f) * 64.0f;
            if (on_floor(world, NULL, pos))
                return aim_floor_exact(world, pos, g->min_z + (float)tz * 64.0f);
        }
        for (int tz = start + 1; tz <= start + AIM_FLOOR_UP; tz++) {
            if (tz < 0 || (g->num_z > 0 && tz >= g->num_z)) break;
            pos[2] = g->min_z + ((float)tz + 0.5f) * 64.0f;
            if (on_floor(world, NULL, pos))
                return aim_floor_exact(world, pos, g->min_z + (float)tz * 64.0f);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: aim floor", &f, NULL);
    }
    return from;
}

// A tile no path reaches. A unit standing on it is the likeliest reason, and
// worth more than the verdict: when one was found on arrival, its name is the
// whole answer. Otherwise the tile's flags say why.
static void nav_say_no_path(int tx, int ty)
{
    char here[TILE_MAX_TEXT];
    step_who(1, here, sizeof here);
    logf_("nav: %d, %d unreachable%s\n", tx, ty, here[0] ? " -- occupied" : "");
    if (here[0]) {
        nav_step_say("");
        return;
    }
    if (!g_step_pending) return;    // already said: nothing to ask the game for
    char why[48];
    Fault f;
    __try { tile_refusal_probe(tx, ty, navh_ground(), why, sizeof why); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: refusal", &f, NULL);
        _snprintf_s(why, sizeof why, _TRUNCATE, "%s", tile_refusal_text(TILE_REFUSE_NO_PATH));
    }
    nav_step_say(why);
}

// ---- the radar -------------------------------------------------------------
//
// Numpad + lists the enemies in sight, numpad - the rest of the squad, each
// by offset from the soldier being moved, nearest first: "Chryssalid, 2
// north, 5 east." Offsets are tiles in the numpad's directions, so the answer
// is also the way there. The minimap does the same job for a sighted player,
// and draws from the same condition: a contact is on it while it is in sight.
static void radar(int friendly)
{
    CursorGrid g;
    int sx, sy;
    float sz;
    if (!cursor_grid(&g) || !soldier_tile(&g, &sx, &sy, &sz)) {
        speech_say_now("No soldier.");
        return;
    }
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);
    void* squad = squad_player();
    if (!squad) {
        logf_("radar: the soldier's player is unreadable\n");
        speech_say_now("No soldier.");
        return;
    }

    // Measured from the tile being navigated to, so the offsets are the keys
    // to press from where the player is now; from the soldier until a step
    // has been taken. The soldier is left out only when measuring from their
    // own tile -- away from it, where they stand is worth hearing too.
    int ox = sx, oy = sy;
    nav_target(&ox, &oy);
    int from_soldier = ox == sx && oy == sy;
    static SeenSet sight;
    if (!friendly) squad_sight(squad, &sight);

    // The squad list is also the squad at a glance: each soldier with what
    // their flag shows over their head -- the action pips, hit points, panic,
    // bleeding out -- so who can still act is heard without switching to
    // each in turn. The selected soldier is in it too, "here".
    static char labels[UNIT_MAX][256];
    TileContact c[UNIT_MAX];
    int n = 0;
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || s.friendly != friendly ||
            (!friendly && from_soldier && s.pawn == soldier) ||
            (!friendly && !seen_has(&sight, s.unit)))
            continue;
        unit_label(&g_units[i], labels[n], sizeof labels[n]);
        if (friendly) {
            const UnitName* u = &g_units[i];
            char state[128];
            soldier_squad_words(u->moves, u->hp, u->hp_max, u->panicked, u->wounded,
                                u->bleed_turns, state, sizeof state);
            if (state[0]) {
                size_t used = strlen(labels[n]);
                _snprintf_s(labels[n] + used, sizeof labels[n] - used, _TRUNCATE, ", %s", state);
            }
        }
        c[n].name = labels[n];
        c[n].dx = cursor_tile_axis(s.loc[0], g.min_x, CURSOR_TILE) - ox;
        c[n].dy = cursor_tile_axis(s.loc[1], g.min_y, CURSOR_TILE) - oy;
        n++;
    }
    static char say[2048];
    tile_contacts(c, n,
                  !friendly ? "No enemies in sight." : "No squad in sight.",
                  say, sizeof say);
    logf_("radar: %s from %d, %d%s%s: %s\n", friendly ? "squad" : "enemies", ox, oy,
          from_soldier ? " (the soldier)" : " (the target)",
          friendly ? "" : sight.n ? "" : ", the squad sees no one", say);
    speech_say_now(say);
}

// Sets the slots from the thunks, once, at startup.
static void tile_arm(const NativeEntry* tbl, int n, HMODULE mod)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)mod + dos->e_lfanew);
    g_image_lo = (uint8_t*)mod;
    g_image_hi = (uint8_t*)mod + nt->OptionalHeader.SizeOfImage;

    // Named without the C++ class prefix: natives_find_class tries both, so
    // an Actor that the decompile's `// Export` comment calls a UObject --
    // XGUnitNativeBase is one -- cannot cost a live run to notice.
    static const struct { const char* name; int* slot; void** direct; } want[] = {
        { "XComWorldDataexecGetCoverPoint",          &g_tile_slot_cover,    NULL },
        { "XComWorldDataexecTileContainsSmoke",      &g_tile_slot_smoke,    NULL },
        { "XComWorldDataexecTileContainsPoison",     &g_tile_slot_poison,   NULL },
        { "XComWorldDataexecIsTileOccupied",         &g_tile_slot_occupied, NULL },
        { "XComWorldDataexecIsPositionOnFloor",      &g_tile_slot_onfloor,  NULL },
        { "XComWorldDataexecGetFloorZForPosition",   &g_tile_slot_floorz,   NULL },
        { "XComWorldDataexecIsPositionOnFloorAndValidDestination",
                                              &g_tile_slot_standable, NULL },
        { "XComWorldDataexecCanSeeActorToTile",      &g_world_slot_seetile, NULL },
        { "XGUnitNativeBaseexecIsFlankingCoverPoint", &g_unit_slot_flanking,
                                              &g_unit_fn_flanking },
        { "XGUnitNativeBaseexecIsPointWithinFiringRange", &g_unit_slot_range, NULL },
        { "XGUnitNativeBaseexecIsFlankedBy_EnemyAtLocation", &g_unit_slot_flankedby, NULL },
        { "XGUnitNativeBaseexecIsAliveAndVisible",   &g_unit_slot_visible,  NULL },
        { "XGUnitNativeBaseexecIsAlive",             &g_unit_slot_alive,    NULL },
        { "XGUnitNativeBaseexecIsInOverwatch",       &g_unit_slot_overwatch, NULL },
        { "UI_FxsPanelexecIsVisible",                &g_panel_slot_visible, NULL },
        { "XCom3DCursorexecWorldZToCursorFloor",     &g_cursor_slot_floor,  NULL },
        { "VolumeexecEncompassesPoint",              &g_volume_slot_encompass,
                                              &g_volume_fn_encompass },
    };
    for (int i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
        const uint8_t* code = (const uint8_t*)natives_find_class(tbl, n, want[i].name);

        // The window was 0x180, which is long enough for a thunk that decodes
        // three ints and not obviously for one that decodes seven arguments
        // including two Vectors by value. Widening it cannot cost anything:
        // the scan stops at the thunk's own `ret 8` whatever the window says.
        size_t win = 0x400;
        if (code && !readable(code, win)) win = 0x180;
        *want[i].slot = code && readable(code, win) ? tile_vtable_slot(code, win) : -1;

        if (*want[i].slot >= 0) {
            logf_("  %-38s vtable +0x%X\n", want[i].name, *want[i].slot);
            continue;
        }

        // No slot to find: the thunk may be calling the implementation
        // outright, which is what a `final` script function compiles to.
        const uint8_t* direct = code && readable(code, win)
            ? tile_direct_target(code, win, g_image_lo, g_image_hi) : NULL;
        if (direct && want[i].direct) {
            *want[i].direct = (void*)direct;
            logf_("  %-38s direct %p (rva %08X) -- not virtual\n",
                  want[i].name, direct, (unsigned)(direct - g_image_lo));
            continue;
        }
        logf_("  %-38s vtable slot NOT FOUND\n", want[i].name);

        // Two things produce that, and they want different answers: a thunk
        // longer than the window, and a native the compiler called DIRECTLY
        // because the script declared it `final` -- a final function is not
        // virtual, so there is no slot to find and never will be. Guessing
        // between them costs a run each; the bytes say which in one.
        if (!code || !readable(code, 0x80)) continue;
        for (int off = 0; off < 0x80; off += 32) {
            char hex[3 * 32 + 1];
            for (int b = 0; b < 32; b++)
                _snprintf_s(hex + b * 3, 4, _TRUNCATE, "%02X ", code[off + b]);
            logf_("      +%03X  %s\n", off, hex);
        }
    }
}

// Defined with the pick, below: the interface lent to the mouse's own pick.
static void nav_forget_interface(void);

// ---- a step up or down a floor ---------------------------------------------
//
// "You just stepped from the roof to the ground": a cue when a described step
// stands on a different floor from the one before -- rising for up, falling
// for down, once a storey (192 units, the game's floor), so a roof two
// storeys up is two. Less than a third of a storey (a kerb, a ramp) is
// nothing. Only a described tile has a settled floor, so a glide is heard
// where it stops, against where it started; a tile no path reaches has no
// floor and sounds nothing.
#define HEIGHT_MIN      64.0f
#define HEIGHT_STOREY   192.0f
#define HEIGHT_MAX_CUES 4

static int   g_floor_prev_ok;
static float g_floor_prev;

static void height_step(int tx, int ty, float floor)
{
    if (g_floor_prev_ok && settings_get(SET_STEPS)) {
        float dz = floor - g_floor_prev;
        float up = dz < 0.0f ? -dz : dz;
        if (up >= HEIGHT_MIN) {
            int n = (int)(up / HEIGHT_STOREY + 0.5f);
            if (n < 1) n = 1;
            if (n > HEIGHT_MAX_CUES) n = HEIGHT_MAX_CUES;
            audio_cue(dz > 0.0f ? HEART_STEP_UP : HEART_STEP_DOWN, n);
            logf_("height: %d, %d floor %.1f from %.1f -- %d %s\n", tx, ty, floor,
                  g_floor_prev, n, dz > 0.0f ? "up" : "down");
        }
    }
    g_floor_prev = floor;
    g_floor_prev_ok = 1;
}

// ---- letting go of the target, the mouse, confirming ------------------------

static void nav_stop(const char* why)
{
    if (!nav_active()) return;
    g_floor_prev_ok = 0;
    nav_end();
    nav_forget_interface();
    // The field is not silenced. With no target held it simply goes back to
    // listening from the selected soldier (listen_tile) -- the walls are
    // still there, and the player has not stopped needing to hear them.
    g_nav_live = 0;
    g_nav_aim = 0;
    g_nav_parked = 0;
    g_tile_due = 0;
    g_step_pending = 0;
    g_step_late = 0;
    logf_("nav: released (%s)\n", why);
}

// The mouse picks nothing while it rests on the HUD or off the map, and then
// Mouse_CheckForPathing never reaches the placement at all -- a key would move
// the target and nothing would follow. Moving the mouse to the middle of the
// game window puts it over the battlefield. Done once per navigation, and only
// after a key has gone unanswered, so a mouse already over the map is left
// where it is.
static int mouse_to_centre(POINT* c)
{
    HWND w = GetForegroundWindow();
    RECT r;
    // A minimised window is still in front for a moment on the way back, and
    // its "centre" is off the screen at -32000; SetCursorPos then pins the
    // mouse to a corner. Not parked, so the next poll tries again.
    if (!w || IsIconic(w) || !GetClientRect(w, &r) ||
        r.right <= r.left || r.bottom <= r.top) return 0;
    c->x = (r.right - r.left) / 2;
    c->y = (r.bottom - r.top) / 2;
    if (!ClientToScreen(w, c)) return 0;
    SetCursorPos(c->x, c->y);
    GetCursorPos(&g_nav_mouse);
    return 1;
}

static void nav_park_mouse(void)
{
    POINT c;
    if (!mouse_to_centre(&c)) return;
    g_nav_parked = 1;
    logf_("nav: no placement since the key; mouse moved to the window centre "
          "(%ld, %ld)\n", c.x, c.y);
}

// With the mouse blocked (mouse.h) it stays wherever it was left, and where it
// was left may be the HUD, where the game will not path, or a screen edge,
// where the camera scrolls for as long as it rests there. So once the block
// takes hold in a mission -- and again each time the game comes back to the
// front, since the mouse is free while it is away -- it is put in the middle
// of the window, over the battlefield. g_nav_mouse follows, so a held target
// does not read this as the mouse moving.
static int g_mouse_parked;

static void mouse_hold_poll(void)
{
    if (!mouse_blocking()) { g_mouse_parked = 0; return; }
    if (g_mouse_parked) return;
    POINT c;
    if (!mouse_to_centre(&c)) return;
    g_mouse_parked = 1;
    logf_("mouse: blocked, parked at the window centre (%ld, %ld); %u device events "
          "swallowed so far\n", c.x, c.y, mouse_swallowed());
}

// Numpad 0. The game moves a soldier from the path it has already built out
// to the cursor, and a right click is the mouse-mode way to ask for that:
// RMouse's release runs ClickToPath -> PerformPath. Sent as a click rather
// than called, because nothing in this DLL calls into script.
static ULONGLONG g_nav_confirm_at;      // opens nav_watch_input's window

static void nav_confirm(void)
{
    g_nav_confirm_at = GetTickCount64();
    INPUT in[2];
    ZeroMemory(in, sizeof in);
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    UINT sent = SendInput(2, in, sizeof(INPUT));
    int tx = -1, ty = -1;
    nav_target(&tx, &ty);
    logf_("nav: confirm -- right click sent (%u of 2), target %d, %d\n",
          sent, tx, ty);
}

// ---- holding a direction ---------------------------------------------------
//
// A tap steps one tile and says what is on it. Holding the key glides: after
// NAV_HOLD_MS the step repeats, quickening from NAV_GLIDE_MS to
// NAV_GLIDE_FAST_MS over NAV_GLIDE_RAMP steps, and carries on until the key is
// let go. It is meant to feel like the camera under WASD -- a way to cross the
// map or sweep a room -- and it is the field (walls_poll) that makes it worth
// having: the walls swell and fade as the cursor passes them, so a glide down
// a corridor is heard as a corridor.
//
// Nothing is spoken while it runs. A description costs a path from the game
// and a floor search, and twenty of them a second would arrive long after the
// player had stopped and would say the wrong tiles when they did. So the glide
// announces exactly one tile: the one it ends on.
//
// The repeat is driven from here rather than from Windows' own key repeat. The
// numpad is unbound in the mission, so no key message for it ever reaches the
// game and there is nothing to read a repeat off; GetAsyncKeyState says only
// that the key is down now.
#define NAV_HOLD_MS       260   // held this long before it starts to repeat
// The first repeat and where it settles, per speed in the options menu
// (settings.h, GLIDE_*). Normal is the speed verified live.
static const int NAV_GLIDE_MS[3]      = { 170, 110, 75 };
static const int NAV_GLIDE_FAST_MS[3] = {  85,  50, 30 };
#define NAV_GLIDE_RAMP     10   // repeats spent getting there

static int       g_glide_digit;      // the direction being held, 0 for none
static int       g_glide_steps;      // repeats taken, for the ramp
static ULONGLONG g_glide_next;       // when the next step is due
static ULONGLONG g_numpad_at[10];    // when each key last went down

static int glide_interval(int steps)
{
    int sp = settings_get(SET_GLIDE);
    if (steps >= NAV_GLIDE_RAMP) return NAV_GLIDE_FAST_MS[sp];
    return NAV_GLIDE_MS[sp] +
           (NAV_GLIDE_FAST_MS[sp] - NAV_GLIDE_MS[sp]) * steps / NAV_GLIDE_RAMP;
}

// What a step arrives to: who is standing on the tile, the floor under it, the
// path the game builds to it, and in the end the announcement. Skipped on
// every step of a glide and run once on the tile it stops on.
static void nav_arrive(int tx, int ty)
{
    g_tile_due = 0;
    g_step_note[0] = 0;
    g_step_where[0] = 0;
    g_floor_hold = 0;
    navh_begin_tile();
    g_nav_path_tile[0] = tx;
    g_nav_path_tile[1] = ty;
    g_nav_phase_logged = (NavHeightPhase)-1;

    // Who stands there is found now, on arrival, and not from the tile's
    // verdict: another unit's tile gets only "No path", and the soldier's
    // own tile gets no verdict at all -- the game builds no path to
    // within 64 units of the soldier (XGAction_Path.DoPathingTick).
    // Every storey of the column: which floor the step is on is not known
    // until it settles, so step_who words them then.
    int mine = 0;
    Fault f;
    __try {
        g_step_nunits = units_in_column(tx, ty, g_step_units, COLUMN_UNITS);
        for (int i = 0; i < g_step_nunits; i++) mine |= g_step_units[i].mine;
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("tile: units", &f, NULL);
        g_step_nunits = 0;
    }
    nav_describe(tx, ty, g_step_coords, sizeof g_step_coords);
    g_step_late = 0;
    g_step_pending = 1;
    g_step_deadline = GetTickCount64() + STEP_FALLBACK_MS;
    g_path_calls = 0;
    // An aim is announced when it lands (nav_aim_landed): no path is built
    // to it, and the soldier's own tile is nothing special to a rocket.
    if (g_nav_aim) return;
    // Nothing can stand here: say so now rather than after the search times
    // out. A tile with a unit on it is left to the search, whose verdict
    // names them.
    if (!mine && !g_step_nunits) {
        char why[48];
        int blocked = 0;
        __try { blocked = tile_blocked_now(tx, ty, navh_ground(), why, sizeof why); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: arrival check", &f, NULL);
            blocked = 0;
        }
        if (blocked) {
            navh_decide_none();
            nav_step_say(why);
            return;
        }
    }
    // With no verdict coming for the soldier's own tile, its cover is
    // described on a timer instead, once the floor search has settled. The
    // same when the soldier has no moves left: XGUnit.AddPathAction gives an
    // idle action instead of a path when GetMoves() is 0 -- after Run and
    // Gun's dash, with only the shot to come -- so no path is ever computed.
    // Every step then waited out STEP_FALLBACK_MS and said its coordinates
    // alone (the 2026-09-28 00:20 log: "nothing decided the tile in 1500 ms
    // (0 path calls)", five steps running).
    int stuck = !mine && soldier_out_of_moves();
    if (stuck)
        logf_("nav: %d, %d described without a path -- the soldier has no moves left\n",
              tx, ty);
    if (mine || stuck) {
        g_tile_due = GetTickCount64() + OWN_TILE_DELAY_MS;
        g_tile_due_at[0] = tx;
        g_tile_due_at[1] = ty;
        g_tile_due_dash = 0;
    }
}

// `gliding` when this is a repeat of a held key rather than a fresh press.
static void nav_press(int digit, int gliding)
{
    CursorGrid g;
    int tx, ty;
    float z;
    if (!cursor_grid(&g) || !cursor_tile(&g, &tx, &ty, &z)) {
        // Said on the press only. A glide that loses the grid under it would
        // otherwise say this twenty times a second.
        if (gliding) return;
        logf_("nav: numpad %d with no grid or cursor yet\n", digit);
        speech_say_now("No map yet.");
        return;
    }

    // Moving and aiming place the cursor for different reasons, so a
    // navigation begun for one is not carried into the other.
    int aiming = soldier_aiming();
    if (nav_active() && aiming != g_nav_aim)
        nav_stop(aiming ? "aiming began" : "aiming ended");

    // Until a step has been taken, "here" is the soldier, not the mouse --
    // except while aiming, where the cursor IS the aim, and the aim is what
    // the player is moving.
    if (!nav_active() && aiming) {
        logf_("nav: aiming -- starting from the aim on %d, %d (z %.1f)\n", tx, ty, z);
    } else if (!nav_active()) {
        int sx, sy;
        float sz;
        if (soldier_tile(&g, &sx, &sy, &sz) &&
            sx >= 0 && sy >= 0 && sx < g.num_x && sy < g.num_y) {
            // The cursor's height is kept when it is on the soldier already:
            // it is the one the floor search is known to work from. An earlier
            // run read this pawn's height as 215.8 for two soldiers on
            // different ground, so it is only a fallback, and the search
            // sweeps several hundred units either way.
            if (sx != tx || sy != ty) {
                logf_("nav: cursor on %d, %d (z %.1f), soldier on %d, %d (z %.1f) -- "
                      "using the soldier\n", tx, ty, z, sx, sy, sz);
                z = sz;
            }
            tx = sx;
            ty = sy;
        } else {
            logf_("nav: soldier's position unreadable, using the cursor's\n");
        }
    }

    if (digit == 5) {
        // Where the cursor actually is, which is what the game accepted --
        // not necessarily the target, if that was somewhere it cannot stand.
        // And what is there, from the cursor's own height: it stands
        // NAVH_LIFT above the floor it was placed on.
        char say[NAV_MAX_TEXT + TILE_MAX_TEXT + 64];
        char what[TILE_MAX_TEXT] = "";
        char coords[NAV_MAX_TEXT];
        nav_describe(tx, ty, coords, sizeof coords);
        logf_("nav: numpad 5 -> cursor on %s\n", coords);
        // The last path is this tile's only while navigating to it, and never
        // on the soldier's own tile: the game builds no path to where the
        // soldier stands, so the one left over belongs to somewhere else.
        int ntx, nty, sx, sy;
        float sz;
        int own = soldier_tile(&g, &sx, &sy, &sz) && sx == tx && sy == ty;
        int path_is_here = nav_target(&ntx, &nty) && ntx == tx && nty == ty && !own;
        Fault f;
        __try {
            if (!tile_report(tx, ty, z - NAV_CURSOR_LIFT, path_is_here, 1, what, sizeof what))
                what[0] = 0;
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("tile: report", &f, NULL);
            what[0] = 0;
        }
        char where[64];
        where_say(tx, ty, z - NAV_CURSOR_LIFT, 1, where, sizeof where);
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s.", where, where[0] ? " " : "",
                    what, what[0] ? " " : "", coords);
        // Nothing is played here. Numpad 5 is the key for "where am I", and
        // the shape of the room is the larger half of that answer -- but the
        // field has been answering it all along, so the key only has to
        // supply the words.
        speech_say_now(say);
        return;
    }
    if (digit == 0) {
        nav_confirm();
        return;
    }

    int dx, dy;
    if (!nav_step_for_digit(digit, &dx, &dy)) return;

    if (!nav_active()) {
        nav_begin(tx, ty);
        g_nav_cursor = cursor_object();
        g_nav_pawn = NULL;
        cursor_chained_pawn(&g_nav_pawn);
        GetCursorPos(&g_nav_mouse);
        g_nav_parked = 0;
        navh_set_ground(z - NAV_CURSOR_LIFT);
        navh_begin_tile();
        g_nav_aim = aiming;
        g_aim_floor = z - NAV_CURSOR_LIFT;
        logf_("nav: begins on %d, %d, ground estimate %.1f%s\n", tx, ty, navh_ground(),
              aiming ? ", aiming" : "");
        // The soldier's floor, so the first step off a roof is heard too.
        g_floor_prev = navh_ground();
        g_floor_prev_ok = 1;
        where_forget();
    }

    char say[NAV_MAX_TEXT + TILE_MAX_TEXT];
    NavGrid ng = { g.num_x, g.num_y };
    int moved = nav_move(&ng, dx, dy, say, sizeof say);
    nav_target(&tx, &ty);
    if (moved && !gliding) {
        nav_arrive(tx, ty);
    } else if (moved) {
        // A glide describes nothing, and must not leave anything half
        // decided behind it either: a description still pending belongs to a
        // tile the cursor has left, and a path verdict is about to arrive for
        // one too.
        g_tile_due = 0;
        g_step_pending = 0;
        g_nav_path_tile[0] = -1;
        g_nav_path_tile[1] = -1;
        // The floor search is not restarted per step. It takes several frames
        // and a glide gives it fifty milliseconds, so restarting it would
        // leave the height permanently unsettled; the ground carries over
        // instead, which is right on the level ground a glide is for. The tile
        // it stops on gets a search of its own, from nav_arrive. Whether the
        // cursor can actually be placed at that height does not hold the glide
        // up: what the field listens from is nav's own target, not the cursor.
    }

    // The middle of the tile; the height comes from navh_query_z each frame.
    // An aim is asked at one height instead: the floor search is driven by
    // path verdicts, aiming builds no paths, and the cursor snaps itself to
    // the floor as it moves (XCom3DCursor's CursorSnapToFloor).
    g_nav_world[0] = g.min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    g_nav_world[1] = g.min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    if (g_nav_aim) {
        float was = g_aim_floor;
        g_aim_floor = aim_floor(&g, tx, ty, g_aim_floor);
        if (g_aim_floor != was)
            logf_("nav: aim floor %.1f -> %.1f at %d, %d\n", was, g_aim_floor, tx, ty);
    }
    // The floor itself, not the cursor's height above it: getValidLocation
    // adds the cursor's collision height to what it is given, as it does for
    // a move, whose pick point is the ground. Given floor + lift, the aim sat
    // 63 units up on flat ground in the run of 2026-09-22.
    g_nav_world[2] = g_nav_aim ? g_aim_floor : navh_query_z();
    g_nav_live = 1;
    g_nav_key_at = GetTickCount64();

    if (!gliding)
        logf_("nav: numpad %d -> target %d, %d  \"%s\"\n", digit, tx, ty, say);
    // A step waits for its description (nav_step_say); only the edge, which
    // moves nothing, is answered at once -- and not while gliding, where it
    // would repeat "Edge" twenty times a second against the side of the map.
    if (!moved && !gliding) speech_say_now(say);
}

// ---- the scanner -----------------------------------------------------------
//
// Ported from the Wasteland 2 accessibility mod, key for key. scan.h holds the
// keys and the reasoning; this is where the lists come from.
//
// Three sources, because XCOM keeps these things in three different places:
//
//   units   the flag table (units.c). A flag exists for every soldier,
//           alien and civilian, and unit_seen has already settled whether it
//           can be seen at all. The side comes from the unit's own m_eTeam
//           rather than from the flag: eTeam_Neutral is a civilian, and the
//           flag's m_bIsFriendly shares a dword with m_bIsDead in this build.
//
//   world   the game's object table, walked in world.c. Doors, windows,
//           panels, ladders, Meld canisters and the radar array are level
//           actors that never pass through the UI, and no native lists them:
//           GetInteractionPoints comes closest but covers only
//           XComInteractiveLevelActor, which a ladder is not.
//
//   climbs  the tiles around the soldier, from the cover flags the mod already
//           asks for. COVER_ClimbOnto_* and COVER_ClimbOver_* live in the same
//           flags word walls_scan reads, so a way up a ledge -- a ramp, a
//           crate, a low wall -- costs one query per tile and nothing else.
//
// Only the units are gated on being seen. XCOM draws the whole map, so a
// sighted player can pick out a door across it, and hiding level actors would
// take away something the screen already gives.

// COVER_ClimbOnto_N..W and COVER_ClimbOver_N..W, from XComWorldData.
#define COVER_CLIMB_ONTO 0x001E0000
#define COVER_CLIMB_OVER 0x01E00000

#define SCAN_CLIMB_RADIUS 12    // tiles each way the climb scan covers
#define SCAN_CLIMB_APART   4    // tiles between two climbs worth naming apart

// Where the scan was measured from, and the grid it was taken on, so Home and
// End answer about the same scan the player has just heard.
static CursorGrid g_scan_grid;
static int        g_scan_from[3];
static float      g_scan_world_z;   // the origin's own height, for tile queries
static int        g_scan_have;

// Fills in an item's tile from its world position. 0 when the grid is unknown.
//
// `lift` is how far the position stands above the floor it is on. A pawn's
// Location is its middle, NAV_CURSOR_LIFT above its feet, and scan_origin
// takes that off the tile the scan is measured from -- so an item that does
// not take it off too reads one storey high. The first run said "Payne, here,
// one floor up" about the very soldier the scan was measured from. A level
// actor's Location is already at its base, so it passes 0 (and the level
// actors are placed by world.c on their own grid).
static int scan_item_at(ScanItem* it, const float* world, float lift)
{
    if (!g_scan_have) return 0;
    memcpy(it->world, world, 3 * sizeof(float));
    it->tx = cursor_tile_axis(world[0], g_scan_grid.min_x, CURSOR_TILE);
    it->ty = cursor_tile_axis(world[1], g_scan_grid.min_y, CURSOR_TILE);
    // Off the grid is not a place the cursor can go, so it is not a place the
    // scanner may offer. A class default object put "Radar array" on tile
    // 65, -12 -- world (0, 0), which is where an object with no position sits
    // -- and Home sent the cursor over the edge of the map after it.
    if (it->tx < 0 || it->ty < 0 ||
        it->tx >= g_scan_grid.num_x || it->ty >= g_scan_grid.num_y)
        return 0;
    float feet[3] = { world[0], world[1], world[2] - lift };
    it->tz = floor_of(feet);
    return 1;
}

// ---- the units -------------------------------------------------------------

// The player the squad belongs to, kept from the last time it could be read.
//
// squad_player goes through the cursor's ChainedPawn, and during a soldier
// switch that is briefly nothing -- at which point every unit reads as not
// friendly and the scan comes back empty. The first run showed it: "Squad, 5
// found" and then, one key later, "No squad", with "nav: released (the soldier
// changed)" between them. The player does not change within a mission, so the
// last one read is the right answer while the cursor is between soldiers.
static void* g_scan_squad;

static void* scan_squad_player(void)
{
    void* p = squad_player();
    if (p) g_scan_squad = p;
    return p ? p : g_scan_squad;
}

static void scan_add_flagless(const SquadSight* sight);

static void scan_add_units(void)
{
    void* squad = scan_squad_player();
    static SquadSight sight;
    squad_sight_take(squad, &sight);

    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        if (!squad_sees(&sight, s.unit, s.loc, s.friendly, s.who->name)) continue;

        ScanItem it;
        memset(&it, 0, sizeof it);
        if (s.friendly) {
            it.kind = SCAN_SQUAD;
        } else if (unit_team(s.unit) == TEAM_NEUTRAL) {
            it.kind = SCAN_CIVILIANS;
        } else {
            it.kind = SCAN_ENEMIES;
            if (unit_overwatch(s.unit))
                strcpy_s(it.detail, sizeof it.detail, "on overwatch");
        }
        unit_label(&g_units[i], it.name, sizeof it.name);
        if (scan_item_at(&it, s.loc, NAV_CURSOR_LIFT)) scan_add(&it);
    }
    if (squad) scan_add_flagless(&sight);
}

// ---- the targets -----------------------------------------------------------
//
// What the soldier can shoot: the target strip's own list (strip_enemies),
// best shot first. Each is said with what the screen offers about it -- the
// hit chance its icon shows under the mouse, the cover shield and hit points
// on its flag, and the strip's flanked and squadsight marks.
//
// The hit chance is the one the strip itself shows. Hovering an icon
// (UISightlineHUD_SightlineContainer.OnMouseEvent, case 392) walks the
// soldier's m_aAbilities for the standard shot (iType 7, eAbility_ShotStandard)
// whose primary target is that enemy and puts its GetUIHitChance on the icon;
// for a standard shot that is GetHitChance, which is m_iHitChance. So the
// same walk is made here with field reads: the ability's m_aTargets[0]
// .m_kTarget stands for GetPrimaryTarget (native, and a standard shot has one
// target), and nothing is called.
static FieldSlot g_nabilities, g_abilities, g_ab_targets, g_ab_chance;
static uint32_t  g_itype_off;           // XGAbility.iType, the same in every subclass
static int       g_itype_have;

#define ABILITY_SHOT_STANDARD 7

static int soldier_chance_at(void* soldier, const void* enemy)
{
    const void* v;
    if (!field_ptr(soldier, "m_iNumAbilities", &g_nabilities, 4, &v)) return -1;
    int n = *(const int32_t*)v;
    if (n <= 0 || n > 64) return -1;
    if (!field_ptr(soldier, "m_aAbilities", &g_abilities, 64 * sizeof(void*), &v))
        return -1;
    void* abilities[64];
    memcpy(abilities, v, (size_t)n * sizeof(void*));
    for (int i = 0; i < n; i++) {
        uint8_t* a = (uint8_t*)abilities[i];
        if (!a || !unit_is_live(a)) continue;
        // One lookup for every ability class: iType is XGAbility's, so it
        // sits at the same offset in all of them, and asking each class in
        // turn would make the field cache walk a class chain per ability.
        if (!g_itype_have) {
            if (!object_field_offset(a, "iType", &g_itype_off)) return -1;
            g_itype_have = 1;
        }
        if (!readable(a + g_itype_off, 4) ||
            *(const int32_t*)(a + g_itype_off) != ABILITY_SHOT_STANDARD)
            continue;
        if (!field_ptr(a, "m_aTargets", &g_ab_targets, sizeof(void*), &v) ||
            *(void* const*)v != enemy)
            continue;
        if (!field_ptr(a, "m_iHitChance", &g_ab_chance, 4, &v)) continue;
        return *(const int32_t*)v;
    }
    return -1;
}

// ---- the selected soldier (soldier.h) ---------------------------------------
//
// The panel (SetStats) says who the soldier is; the flag (ShowExtension) says
// when they became the selection. SetStats cannot be the trigger: at a
// mission's start it is sent while the HUD is still being built, before any
// flag exists, and a switch announced from it came out as a bare name while
// the mission was still loading (2026-09-22). ShowExtension reaches Flash
// once per change of selection, from the new soldier's own flag, after the
// panel has been redrawn -- six switches, six calls, in that log. The panel
// is matched to the flag by surname, so nothing depends on when the cursor's
// ChainedPawn catches up.
#define SOLDIER_SETTLE_MS 100
static SoldierState g_panel;            // the panel as last drawn
static void*        g_selected_flag;    // the flag ShowExtension last marked
static void*        g_soldier_said;     // the flag last announced
static ULONGLONG    g_soldier_due;      // when to announce, 0 for never

// Case-insensitive: does `hay` contain `needle`? "URSULA WRIGHT" / "Wright".
static int contains_ci(const char* hay, const char* needle)
{
    size_t n = strlen(needle);
    if (!n) return 0;
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return 1;
    return 0;
}

static void soldier_stats_note(LONG n, const Payload* p)
{
    SoldierState s;
    soldier_clear(&s);
    const char* strs[8];
    int ns = 0;
    for (int i = 0; i < p->nstrings && ns < 8; i++) strs[ns++] = p->strings[i];
    soldier_from_stats(&s, strs, ns);
    if (p->nabools > 0) s.leader = p->abools[0];
    if (p->nabools > 1) s.promotion = p->abools[1];
    if (p->nnumbers > 0) s.aim = (int)p->numbers[p->nnumbers - 1];
    // The surname-only redraw while the ability menu is up: the same soldier,
    // and the full name is kept.
    if (s.name[0] && contains_ci(g_panel.name, s.name) && strlen(s.name) < strlen(g_panel.name))
        memcpy(s.name, g_panel.name, sizeof s.name);
    g_panel = s;
    logf_("[%ld] soldier: panel \"%s\" %s %s %s, leader %d, promotion %d, aim %d\n", n,
          s.name, s.nick, s.rank, s.cls, s.leader, s.promotion, s.aim);
}

static void soldier_selected(void* flag)
{
    g_selected_flag = flag;
    g_soldier_due = GetTickCount64() + SOLDIER_SETTLE_MS;
}

static UnitName* unit_of_flag(void* flag)
{
    for (int i = 0; i < g_nunits; i++)
        if (flag && g_units[i].flag == flag) return &g_units[i];
    return NULL;
}

// The soldier behind flag entry `u`: the panel if it is theirs, their flag's
// name otherwise, and the flag's hit points, actions and markers.
// ---- the weapon panels (soldier.h) -----------------------------------------
//
// UITacticalHUD_WeaponContainer.SetWeapons draws the active soldier's two
// weapons, each in a UITacticalHUD_WeaponPanel through SetWeaponAndAmmo, and
// names the equipped one with AS_SetWeaponName. It runs on every HUD update
// (UITacticalHUD.Update -> m_kWeaponContainer.Update(true)), so what is kept
// here is the soldier now selected. The panel's ASValue array carries the
// numbers: the type string, the ammo or overheat chance, the ability's cost,
// then the overheat and reload flags.
//
// A shot's cost is only sent while an ability that uses the weapon is
// selected, and GetAmmoCost is native, so each type's cost is learnt as it
// passes and kept for the mission: the smallest seen, since Rapid Fire sends
// double.
#define WEAPON_COSTS 32
static SoldierWeapon g_weapon[2];
static char          g_weapon_name[64];
static struct { char type[48]; int cost; } g_weapon_cost[WEAPON_COSTS];
static int           g_weapon_ncost;

static int weapon_cost(const char* type)
{
    for (int i = 0; i < g_weapon_ncost; i++)
        if (strcmp(g_weapon_cost[i].type, type) == 0) return g_weapon_cost[i].cost;
    return 0;
}

// The costs are kept between sessions, in xcom_uihook_ammo.ini beside the log
// (one "type=cost" per line), so a weapon aimed once is counted in shots from
// the start of every mission after. One file per build, since the log is.
static char g_weapon_cost_path[MAX_PATH];

static void weapon_costs_save(void)
{
    if (!g_weapon_cost_path[0]) return;
    FILE* f = NULL;
    if (fopen_s(&f, g_weapon_cost_path, "w") != 0 || !f) {
        logf_("weapon: could not write %s\n", g_weapon_cost_path);
        return;
    }
    for (int i = 0; i < g_weapon_ncost; i++)
        fprintf(f, "%s=%d\n", g_weapon_cost[i].type, g_weapon_cost[i].cost);
    fclose(f);
}

// Returns 1 when the table changed.
static int weapon_set_cost(const char* type, int cost)
{
    if (!type[0] || cost <= 0) return 0;
    for (int i = 0; i < g_weapon_ncost; i++)
        if (strcmp(g_weapon_cost[i].type, type) == 0) {
            if (cost >= g_weapon_cost[i].cost) return 0;
            g_weapon_cost[i].cost = cost;
            return 1;
        }
    if (g_weapon_ncost >= WEAPON_COSTS) return 0;
    strncpy_s(g_weapon_cost[g_weapon_ncost].type, sizeof g_weapon_cost[0].type, type, _TRUNCATE);
    g_weapon_cost[g_weapon_ncost++].cost = cost;
    return 1;
}

static void weapon_learn_cost(const char* type, int cost)
{
    if (weapon_set_cost(type, cost)) weapon_costs_save();
}

// At startup: the costs already seen in EW's logs (2026-09-22, an LMG's three
// shots and a laser rifle's four), then whatever the file has learnt since.
// The two are EW's only; EU learns its own.
static void weapon_costs_load(const char* dir, int is_ew)
{
    if (is_ew) {
        weapon_set_cost("_LMG", 33);
        weapon_set_cost("_LaserAssaultRifle", 25);
    }
    _snprintf_s(g_weapon_cost_path, sizeof g_weapon_cost_path, _TRUNCATE,
                "%sxcom_uihook_ammo.ini", dir);
    FILE* f = NULL;
    int read = 0;
    if (fopen_s(&f, g_weapon_cost_path, "r") == 0 && f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            int cost = atoi(eq + 1);
            if (line[0] && cost > 0 && cost <= 100) { weapon_set_cost(line, cost); read++; }
        }
        fclose(f);
    }
    logf_("weapon: %d shot cost%s known at startup (%d from %s)\n", g_weapon_ncost,
          g_weapon_ncost == 1 ? "" : "s", read, g_weapon_cost_path);
}

// X switches weapons (X_Key_Press -> CycleWeapons, and the container is
// redrawn). A change of the equipped weapon this soon after X is that switch
// and is said; one without it is a soldier switch, which says the weapon
// itself.
//
// Not the clock alone: X on a soldier who cannot switch (a heavy's rocket
// launcher is not drawn) followed by Tab to a sniper with a pistol out read
// as a switch, and the pistol was said twice (2026-09-22). So X also notes
// who is active and which two weapons they carry, and a change counts only
// for the same soldier with the same two.
#define WEAPON_SWITCH_MS 1500
static ULONGLONG g_weapon_x_at;
static int       g_weapon_x_down;
static void*     g_weapon_x_unit;
static char      g_weapon_x_types[2][48];

static void weapon_x_pressed(void)
{
    g_weapon_x_at = GetTickCount64();
    g_weapon_x_unit = soldier_unit();
    for (int i = 0; i < 2; i++)
        strncpy_s(g_weapon_x_types[i], sizeof g_weapon_x_types[i], g_weapon[i].type, _TRUNCATE);
}

static int weapon_x_same_soldier(void)
{
    for (int i = 0; i < 2; i++)
        if (strcmp(g_weapon_x_types[i], g_weapon[i].type) != 0) return 0;
    void* now = soldier_unit();
    return !g_weapon_x_unit || !now || now == g_weapon_x_unit;
}

static void weapon_words(char* active, size_t active_sz, char* all, size_t all_sz);

static int weapon_note(LONG n, const char* obj, const char* fn, const Payload* p)
{
    if (strncmp(obj, "UITacticalHUD_WeaponContainer", 29) == 0 && strstr(fn, "SetWeaponName")) {
        const char* name = p->nstrings ? p->strings[0] : "";
        if (strcmp(name, g_weapon_name) == 0) return 1;
        strncpy_s(g_weapon_name, sizeof g_weapon_name, name, _TRUNCATE);
        int after_x = g_weapon_x_at && GetTickCount64() - g_weapon_x_at < WEAPON_SWITCH_MS;
        int switched = after_x && weapon_x_same_soldier();
        logf_("[%ld] weapon: equipped \"%s\"%s\n", n, g_weapon_name,
              switched ? " -- after X" : after_x ? " -- after X, but another soldier" : "");
        if (switched) {
            g_weapon_x_at = 0;
            char one[SOLDIER_WEAPON_TEXT], all[SOLDIER_WEAPON_TEXT];
            weapon_words(one, sizeof one, all, sizeof all);
            logf_("weapon: switched -> \"%s\"\n", one);
            if (one[0] && g_speak) {
                speech_cancel_pending();
                speech_say_now(one);
            }
        }
        return 1;
    }
    if (strncmp(obj, "UITacticalHUD_WeaponPanel_", 26) != 0 ||
        strcmp(fn, "SetWeaponAndAmmo") != 0)
        return 0;
    SoldierWeapon* w = &g_weapon[atoi(obj + 26) % 2];
    SoldierWeapon was = *w;
    memset(w, 0, sizeof *w);
    if (p->nstrings) strncpy_s(w->type, sizeof w->type, p->strings[0], _TRUNCATE);
    w->set = w->type[0] != 0;
    w->value = p->nnumbers > 0 ? (int)(p->numbers[0] + 0.5f) : 0;
    int cost = p->nnumbers > 1 ? (int)(p->numbers[1] + 0.5f) : 0;
    w->overheat = p->nabools > 0 ? p->abools[0] : 0;
    w->reload = p->nabools > 1 ? p->abools[1] : 0;
    int known = weapon_cost(w->type);
    if (!w->overheat) weapon_learn_cost(w->type, cost);
    // Every HUD update redraws both panels; only a change is worth a line.
    if (memcmp(&was, w, sizeof was) != 0 || weapon_cost(w->type) != known)
        logf_("[%ld] weapon: %s %s %s %d, cost %d (%d known)%s\n", n, obj,
              w->set ? w->type : "(none)", w->overheat ? "overheat" : "ammo", w->value,
              cost, weapon_cost(w->type), w->reload ? ", reload needed" : "");
    return 1;
}

// The two panels in words, with each type's cost as learnt.
static void weapon_words(char* active, size_t active_sz, char* all, size_t all_sz)
{
    SoldierWeapon w[2];
    memcpy(w, g_weapon, sizeof w);
    for (int i = 0; i < 2; i++) w[i].cost = weapon_cost(w[i].type);
    soldier_weapons(g_weapon_name, w, 2, active, active_sz, all, all_sz);
}

static int soldier_state(const UnitName* u, SoldierState* s)
{
    if (!u || !u->name[0]) return 0;
    if (g_panel.name[0] && contains_ci(g_panel.name, u->name)) {
        *s = g_panel;
    } else {
        soldier_clear(s);
        strncpy_s(s->name, sizeof s->name, u->name, _TRUNCATE);
        hq_nick_quoted(u->nick, s->nick, sizeof s->nick);
    }
    s->hp = u->hp;
    s->hp_max = u->hp_max;
    s->actions = u->moves;
    s->buff = u->buff;
    s->debuff = u->debuff;
    s->panicked = u->panicked;
    s->wounded = u->wounded;
    s->bleed_turns = u->bleed_turns;
    // The panels are the active soldier's, redrawn before the flag marks a
    // switch (2026-09-22: SetWeaponAndAmmo at 712, ShowExtension at 743).
    weapon_words(s->weapon, sizeof s->weapon, s->weapons, sizeof s->weapons);
    return 1;
}

// Every frame: a switch, once the flag has marked it.
static void soldier_poll(void)
{
    if (!g_soldier_due || GetTickCount64() < g_soldier_due) return;
    g_soldier_due = 0;
    if (g_selected_flag == g_soldier_said) return;
    SoldierState s;
    if (!soldier_state(unit_of_flag(g_selected_flag), &s)) return;
    g_soldier_said = g_selected_flag;
    char say[SOLDIER_TEXT];
    soldier_brief(&s, say, sizeof say);
    logf_("soldier: selected -> \"%s\"\n", say);
    if (say[0] && g_speak && !muted()) {
        speech_cancel_pending();
        speech_say_now(say);
    }
}

// Delete: everything the HUD shows about the selected soldier.
static void soldier_readout(void)
{
    SoldierState s;
    char say[SOLDIER_TEXT];
    UnitName* u = unit_of_flag(g_selected_flag);
    if (!u) u = unit_by_unit(soldier_unit());
    if (!soldier_state(u, &s)) {
        speech_say_now("No soldier selected.");
        return;
    }
    soldier_full(&s, say, sizeof say);
    logf_("soldier: Delete -> \"%s\"\n", say);
    speech_cancel_pending();
    speech_say_now(say);
}

// Whether the soldier is aiming: their current action is the targeting one
// (XGAction_Targeting in EW; EU aims inside XGAction_Fire, as shot_target_now
// notes). Told by the action object's name, since its class is what differs.
// ---- the unit information screen (F1) ---------------------------------------
//
// See info.h. F1 is the game's own key and reaches the game untouched; what
// is added is hearing the screen it opens, and walking it. The screen is drawn
// in one burst of calls on several objects (the screen, three perk lists, the
// shot panel) whose order is up to Flash, so the summary waits for the burst
// to go quiet. While it is up, numpad 8 / 2 or the arrows walk its lines and
// numpad 5 says the line again; the arrows only scroll the screen itself,
// which consumes every key (eInputState_Consume), so nothing reaches the
// battle. The game closes it -- Escape, F1, Backspace -- and m_kGermanMode
// going to none is how that is noticed.
#define INFO_SETTLE_MS 300
#define INFO_BURST_GAP_MS 1500

static void*     g_info_screen;         // the UIUnitGermanMode the burst came from
static void*     g_info_lists[INFO_LISTS];
static int       g_info_fresh = 1;      // the next content call starts a new screen
static int       g_info_said;           // the summary has been said for this one
static ULONGLONG g_info_last;           // the last content call
static ULONGLONG g_info_opened_at;      // when the summary was said
static FieldSlot g_info_ctrl, g_info_pres, g_info_german;

// Which of the three lists an object is, in the screen's order. They draw
// themselves last first (PENALTIES, BONUSES, ABILITIES in the first run), but
// UIUnitGermanMode.Init spawns them abilities, bonuses, penalties, one after
// another, so their instance numbers run 0, 1, 2 and then 3, 4, 5 for the next
// screen: the remainder is the place. Arrival order only if the name will not
// parse.
static int info_list_slot(void* obj, const char* obj_name)
{
    const char* digits = strrchr(obj_name, '_');
    if (digits && digits[1] >= '0' && digits[1] <= '9') return atoi(digits + 1) % INFO_LISTS;
    for (int i = 0; i < INFO_LISTS; i++) {
        if (g_info_lists[i] == obj) return i;
        if (!g_info_lists[i]) { g_info_lists[i] = obj; return i; }
    }
    return -1;
}

static void info_note(LONG n, void* object, const char* obj_name, const char* fn_name,
                      void* node, uint8_t* locals)
{
    static FrameArgs a;     // 12 KB: not on the game's stack
    frame_args(node, locals, &a);
    const char* s[FRAME_ARGS];
    for (int i = 0; i < FRAME_ARGS; i++) s[i] = i < a.ns ? a.s[i] : "";

    InfoCall call = info_call(obj_name, fn_name);
    int is_screen = call == INFO_SOLDIER || call == INFO_ALIEN || call == INFO_STATS;
    int content = call != INFO_NONE;

    logf_("[%ld] info: %s.%s  \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"  bools %d:%d%s\n", n,
          obj_name, fn_name, s[0], s[1], s[2], s[3], s[4], a.nb, a.nb ? a.b[0] : -1,
          content ? "" : "  (not content)");
    if (!content) return;

    // A new screen. Not while one is open: the scroll calls the arrows make
    // are not content, but a late call from the open screen must not wipe it.
    ULONGLONG now = GetTickCount64();
    if (g_info_fresh || (!info_is_open() && now - g_info_last > INFO_BURST_GAP_MS)) {
        info_reset();
        memset(g_info_lists, 0, sizeof g_info_lists);
        g_info_screen = NULL;
        g_info_fresh = 0;
        g_info_said = 0;
    }
    g_info_last = now;
    if (!g_info_said) g_info_due = now + INFO_SETTLE_MS;
    if (is_screen) g_info_screen = object;

    switch (call) {
    case INFO_SOLDIER: {
        info_soldier(s[0], s[1], s[2], s[3], a.nb ? a.b[0] : -1);
        // Only for the active soldier: the HUD's panels are theirs. F1 opens
        // on the active soldier unless aiming, and then on the target.
        if (!soldier_aiming()) {
            char one[SOLDIER_WEAPON_TEXT], all[SOLDIER_WEAPON_TEXT];
            weapon_words(one, sizeof one, all, sizeof all);
            info_weapons(all);
        }
        break;
    }
    case INFO_ALIEN:    info_alien(s[0]); break;
    case INFO_STATS:    info_stats(s, a.ns < 4 ? a.ns : 4); break;
    case INFO_TITLE:    info_list_title(info_list_slot(object, obj_name), s[0]); break;
    case INFO_PERK:     info_list_add(info_list_slot(object, obj_name), s[0], s[1]); break;
    case INFO_SHOT:     info_shot(s[0], s[1], s[2], s[3], s[4]); break;
    case INFO_HIT_MOD:  info_modifier(0, s[0], s[1]); break;
    case INFO_CRIT_MOD: info_modifier(1, s[0], s[1]); break;
    default: break;
    }
}

// Whether the screen the burst came from is still the one up:
// controllerRef.m_Pres.m_kGermanMode, which State_GermanMode.Deactivate sets
// to none as it removes the screen. Anything unreadable counts as closed.
static int info_screen_up(void)
{
    const void* v;
    void* scr = g_info_screen;
    if (!scr || !unit_is_live(scr)) return 0;
    if (!field_ptr(scr, "controllerRef", &g_info_ctrl, sizeof(void*), &v)) return 0;
    void* pc = *(void* const*)v;
    if (!pc || !unit_is_live(pc) ||
        !field_ptr(pc, "m_Pres", &g_info_pres, sizeof(void*), &v)) return 0;
    void* pres = *(void* const*)v;
    if (!pres || !unit_is_live(pres) ||
        !field_ptr(pres, "m_kGermanMode", &g_info_german, sizeof(void*), &v)) return 0;
    return *(void* const*)v == scr;
}

// Whether the mission summary is still up: m_Pres.m_kMissionSummary, which
// State_MissionSummary sets as it spawns the screen. Its Deactivate quits
// the map, so the presentation layer going is what ends it; anything
// unreadable counts as gone. While it is up the battle is over and the
// numpad has nothing to move -- in the 2026-09-24 log it went on walking the
// cursor under the screen.
static FieldSlot g_msum_ctrl, g_msum_pres, g_msum_field;
static int msum_screen_up(void)
{
    const void* v;
    void* scr = g_msum_screen;
    if (!scr) return 0;
    int up = 0;
    if (unit_is_live(scr) &&
        field_ptr(scr, "controllerRef", &g_msum_ctrl, sizeof(void*), &v)) {
        void* pc = *(void* const*)v;
        if (pc && unit_is_live(pc) &&
            field_ptr(pc, "m_Pres", &g_msum_pres, sizeof(void*), &v)) {
            void* pres = *(void* const*)v;
            up = pres && unit_is_live(pres) &&
                 field_ptr(pres, "m_kMissionSummary", &g_msum_field, sizeof(void*), &v) &&
                 *(void* const*)v == scr;
        }
    }
    if (!up) g_msum_screen = NULL;
    return up;
}

// Every frame: the summary, once the burst has gone quiet.
static void info_settle(void)
{
    if (!g_info_due || GetTickCount64() < g_info_due) return;
    g_info_due = 0;
    if (!info_has_content()) return;
    g_info_said = 1;
    char say[INFO_TEXT * 2];
    info_summary(say, sizeof say);
    int up = info_screen_up();
    logf_("info: summary%s (%d lines) -> \"%s\"\n", up ? "" : " -- but the screen is not up",
          info_line_count(), say);
    if (!up) { g_info_fresh = 1; return; }
    info_open();
    g_info_opened_at = GetTickCount64();
    if (g_speak) {
        speech_cancel_pending();
        speech_say_now(say);
    }
}

#define INFO_KEYS 5
static const int g_info_vk[INFO_KEYS] = {
    VK_NUMPAD8, VK_UP, VK_NUMPAD2, VK_DOWN, VK_NUMPAD5
};
static int g_info_was[INFO_KEYS];

// Returns 1 while the screen is up, having handled its keys.
static int info_poll_body(void)
{
    int now[INFO_KEYS], pressed[INFO_KEYS];
    for (int k = 0; k < INFO_KEYS; k++) {
        now[k] = (GetAsyncKeyState(g_info_vk[k]) & 0x8000) != 0;
        pressed[k] = now[k] && !g_info_was[k];
        g_info_was[k] = now[k];
    }
    if (!info_is_open()) return 0;
    if (!info_screen_up()) {
        info_close();
        g_info_fresh = 1;
        logf_("info: the screen closed, %llu ms after the summary\n",
              GetTickCount64() - g_info_opened_at);
        speech_cancel_pending();
        speech_say_now("Closed.");
        return 0;
    }
    char say[INFO_TEXT * 2];
    if (pressed[0] || pressed[1]) info_step(-1, say, sizeof say);
    else if (pressed[2] || pressed[3]) info_step(1, say, sizeof say);
    else if (pressed[4]) info_current(say, sizeof say);
    else return 1;
    logf_("info: %s at %llu ms -> \"%s\"\n",
          pressed[0] || pressed[1] ? "up" : pressed[2] || pressed[3] ? "down" : "again",
          GetTickCount64() - g_info_opened_at, say);
    speech_cancel_pending();
    speech_say_now(say);
    return 1;
}

static int info_poll(void)
{
    Fault f;
    __try { return info_poll_body(); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("info: poll", &f, NULL);
        info_close();
        g_info_fresh = 1;
        return 0;
    }
}

// ---- enemies coming into sight (sight.h) ------------------------------------
//
// The squad's sight, polled, and what changed said: "Sighted: Sectoid, 5
// north, 3 east." It is the same union of m_arrVisibleEnemies the radar and
// the tile readout use, measured from the soldier. The squad is taken only
// from a human player (XGPlayer, or XGPlayer_MP): the cursor is chained to
// whoever is acting, and in the aliens' turn their sight is the squad's
// soldiers, which is nothing to announce. The last good one is kept through
// that turn, and a different one -- a new mission, a load -- starts afresh.
#define SIGHT_POLL_MS 200
static ULONGLONG g_sight_at;
static void*     g_sight_squad;

static void* sight_squad(void)
{
    void* p = squad_player();
    char cls[64];
    if (p && unit_is_live(p) && object_class_name(p, cls, sizeof cls) &&
        (strcmp(cls, "XGPlayer") == 0 || strcmp(cls, "XGPlayer_MP") == 0)) {
        if (p != g_sight_squad) {
            logf_("sight: the squad is %s %p%s\n", cls, p,
                  g_sight_squad ? " -- a new one, starting afresh" : "");
            sight_reset();
            g_sight_squad = p;
        }
    }
    if (g_sight_squad && !unit_is_live(g_sight_squad)) {
        sight_reset();
        g_sight_squad = NULL;
    }
    return g_sight_squad;
}

// Whether a unit that left the squad's sight is still alive. One that died
// leaves it too, and "Chryssalid down." has already said so. Not knowing
// counts as alive.
static int sight_alive(void* unit)
{
    if (!unit || !unit_is_live(unit)) return 0;
    UnitTestFn alive = (UnitTestFn)tile_vfn(unit, g_unit_slot_alive);
    return alive ? alive(unit, NULL) != 0 : 1;
}

static void sight_poll(void)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_sight_at < SIGHT_POLL_MS) return;
    g_sight_at = now;
    void* squad = sight_squad();
    if (!squad) return;

    static SeenSet seen;
    squad_sight(squad, &seen);
    CursorGrid g;
    int sx = 0, sy = 0;
    float sz;
    int have_pos = cursor_grid(&g) && soldier_tile(&g, &sx, &sy, &sz);

    static SightUnit cur[SIGHT_MAX];
    int n = 0;
    for (int i = 0; i < g_nunits && n < SIGHT_MAX; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || s.friendly || !seen_has(&seen, s.unit))
            continue;
        cur[n].unit = s.unit;
        unit_label(&g_units[i], cur[n].label, sizeof cur[n].label);
        cur[n].has_pos = have_pos;
        cur[n].dx = have_pos ? cursor_tile_axis(s.loc[0], g.min_x, CURSOR_TILE) - sx : 0;
        cur[n].dy = have_pos ? cursor_tile_axis(s.loc[1], g.min_y, CURSOR_TILE) - sy : 0;
        n++;
    }

    static SightEvent ev[SIGHT_MAX];
    int k = sight_step(now, cur, n, ev, SIGHT_MAX);
    if (!k) return;
    // The dead are dropped from what is said, not from what is kept.
    int m = 0;
    for (int i = 0; i < k; i++) {
        if (ev[i].kind == SIGHT_GONE && !sight_alive(ev[i].u.unit)) {
            logf_("sight: %s left sight dead -- not said\n", ev[i].u.label);
            continue;
        }
        ev[m++] = ev[i];
    }
    static char say[SIGHT_TEXT];
    sight_text(ev, m, say, sizeof say);
    if (!say[0]) return;
    logf_("sight: %d in sight -> \"%s\"\n", n, say);
    announce_as(SET_SIGHT, say);
}

static int soldier_aiming(void)
{
    void* unit = soldier_unit();
    const void* v;
    if (!unit || !field_ptr(unit, "m_kCurrAction", &g_curr_action, sizeof(void*), &v))
        return 0;
    void* action = *(void* const*)v;
    char name[128];
    if (!action || !unit_is_live(action) || !object_name(action, name, sizeof name))
        return 0;
    return strncmp(name, "XGAction_Targeting", 18) == 0 ||
           strncmp(name, "XGAction_Fire", 13) == 0;
}

static void scan_add_targets(void)
{
    void* const* list;
    int n = strip_enemies(&list);
    if (n <= 0) return;
    void* enemies[SEEN_MAX];
    memcpy(enemies, list, (size_t)n * sizeof(void*));

    // The soldier's own sight, for squadsight: the strip marks an enemy that
    // is on it but not in the soldier's m_arrVisibleEnemies.
    void* soldier = soldier_unit();
    void* own[SEEN_MAX];
    int nown = -1;
    const void* v;
    if (soldier && field_ptr(soldier, "m_arrVisibleEnemies", &g_visen, sizeof(FArray), &v)) {
        const FArray* a = (const FArray*)v;
        if (a->Num == 0) nown = 0;
        else if (a->Num > 0 && a->Num <= SEEN_MAX &&
                 readable(a->Data, (size_t)a->Num * sizeof(void*))) {
            memcpy(own, a->Data, (size_t)a->Num * sizeof(void*));
            nown = a->Num;
        }
    }

    for (int i = 0; i < n; i++) {
        void* e = enemies[i];
        if (!e || !unit_is_live(e)) continue;
        UnitName* u = unit_by_unit(e);
        if (!u) continue;
        void* pawn = unit_pawn(e);
        if (!pawn || !unit_is_live(pawn) ||
            !field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v))
            continue;
        float loc[3];
        memcpy(loc, v, sizeof loc);

        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_TARGETS;
        unit_label(u, it.name, sizeof it.name);

        int chance = soldier ? soldier_chance_at(soldier, e) : -1;
        int squadsight = 0;
        if (nown >= 0) {
            squadsight = 1;
            for (int k = 0; k < nown; k++)
                if (own[k] == e) { squadsight = 0; break; }
        }
        ShotTarget t = { it.name, u->cover, u->strip_flanked, u->hp, u->hp_max, -1, 0 };
        shot_list_detail(&t, chance, squadsight, it.detail, sizeof it.detail);
        // Best shot first; an enemy with no shot at it goes last, nearest
        // first among themselves.
        it.rank = chance >= 0 ? chance + 1 : 0;
        if (scan_item_at(&it, loc, NAV_CURSOR_LIFT)) scan_add(&it);
    }
}

static void scan_add_world(void)
{
    int n;
    if (!world_refresh(&g_scan_grid)) return;
    const ScanItem* items = world_items(&n);
    for (int i = 0; i < n; i++) scan_add(&items[i]);
}

static void scan_add_flagless(const SquadSight* sight)
{
    static FlaglessUnit found[COLUMN_FLAGLESS];
    int n = flagless_units(1, found, COLUMN_FLAGLESS);
    for (int i = 0; i < n; i++) {
        if (!squad_sees(sight, found[i].unit, found[i].loc, 0, found[i].name)) continue;
        ScanItem it;
        memset(&it, 0, sizeof it);
        it.kind = SCAN_CIVILIANS;
        strcpy_s(it.name, sizeof it.name, found[i].name);
        if (scan_item_at(&it, found[i].loc, NAV_CURSOR_LIFT)) scan_add(&it);
    }
}

// The doors and windows within range of (tx, ty), of the kinds switched on,
// into g_door_near; the table and its reasons are above hearts_poll.
static void doors_refresh(const CursorGrid* g, int tx, int ty, int doors, int windows)
{
    int new_map = g_door_map != cursor_object();
    if (new_map) { g_door_n = 0; g_door_map = cursor_object(); }
    g_door_near_n = 0;
    if (!world_refresh(g)) return;
    int nitems;
    const ScanItem* items = world_items(&nitems);
    // Once a map: how many were placed, and whether the scanner had measured
    // yet. Until 2026-09-28 nothing was placed before it had (world_item_at),
    // so a line with "not yet" and doors in it is the fix working.
    if (new_map) {
        int nd = 0, nw = 0;
        for (int i = 0; i < nitems; i++) {
            if (items[i].kind == SCAN_DOORS) nd++;
            else if (items[i].kind == SCAN_INTERACT && strcmp(items[i].name, "Window") == 0) nw++;
        }
        logf_("doors: %d doors and %d windows placed on this map; the scanner has %s\n",
              nd, nw, g_scan_have ? "measured" : "not yet measured");
    }
    for (int i = 0; i < nitems; i++) {
        const ScanItem* it = &items[i];
        int kind;
        if (it->kind == SCAN_DOORS && doors) kind = HEART_DOOR;
        else if (it->kind == SCAN_INTERACT && windows && strcmp(it->name, "Window") == 0)
            kind = HEART_WINDOW;
        else continue;
        int dx = it->tx - tx, dy = it->ty - ty;
        if (dx * dx + dy * dy > DOOR_RANGE * DOOR_RANGE) continue;
        int k;
        for (k = 0; k < g_door_n; k++)
            if (g_door_tile[k][0] == it->tx && g_door_tile[k][1] == it->ty &&
                g_door_tile[k][2] == kind) break;
        if (k == g_door_n) {
            if (g_door_n >= DOORS_MAX) continue;
            g_door_tile[k][0] = it->tx;
            g_door_tile[k][1] = it->ty;
            g_door_tile[k][2] = kind;
            g_door_n++;
        }
        int dup = 0;
        for (int j = 0; j < g_door_near_n; j++) if (g_door_near[j] == k) dup = 1;
        if (!dup && g_door_near_n < DOORS_MAX) g_door_near[g_door_near_n++] = k;
    }
}

// ---- the ways up -----------------------------------------------------------
// ---- where the tutorial is holding the cursor ------------------------------
//
// The EW tutorial does not merely suggest a move, it refuses every other one,
// and a player who cannot see the pulsing marker has no way to find out
// where. It is one Kismet action: SeqAct_RestrictMovementCursor takes a
// Locator placed in the map, lifts its Z by 24, and hands it to
// XComPathingPawn.SetDirectedTargetPoint, which is what fills vTargetPoint.
// SeqAct_UnrestrictMovementCursor clears it again.
//
// â›” bUseTargetPoint is not read, although it exists. It is a bool among ten
// on that pawn and this build found no UBoolProperty::BitMask ("props: no
// BitMask"), so it would read as whatever its neighbours are. The game does
// not trust it alone either: XComDirectedTacticalExperience.InvalidMovement
// asks for the bool AND for X + Y + Z != 0, and the vector test is the one
// that survives having no mask.
//
// Filed under the objectives, because a tutorial waypoint is where the player
// has to go. Meld canisters were here too, and have their own category now
// (SCAN_MELD): one entry each, placed only once seen.
//
// What is NOT here: SeqAct_RestrictMovementCursorToCover, the tutorial's
// other restriction, which sets bFirstMoveOutOfCover and no position at all.
// That one is a rule and not a place; it has no tile to point at, and its
// flag is a bool with the same mask problem and no vector to fall back on.
static FieldSlot g_soldier_unit, g_path_pawn_field, g_target_point;

// The Locator's own Z, before the game lifted it: scan_item_at takes the lift
// back off to find the floor the marker stands on.
#define TUTORIAL_POINT_LIFT 24.0f

// The evac zone, under Objectives, on its tile nearest where the scan was
// measured from -- so Home takes the cursor to the edge of it that is
// closest, and the offsets say the shortest way in.
static void scan_add_evac(void)
{
    int nx, ny;
    float nz;
    int found = 0;
    Fault f;
    __try { found = evac_nearest(g_scan_from[0], g_scan_from[1], &nx, &ny, &nz); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("scan: evac", &f, NULL);
        found = 0;
    }
    if (!found) return;
    float at[3] = { g_scan_grid.min_x + ((float)nx + 0.5f) * CURSOR_TILE,
                    g_scan_grid.min_y + ((float)ny + 0.5f) * CURSOR_TILE, nz };
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;
    strcpy_s(it.name, sizeof it.name, "Evac zone");
    if (scan_item_at(&it, at, 0.0f)) scan_add(&it);
}

static void scan_add_tutorial(void)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn) return;
    if (!field_ptr(pawn, "m_kGameUnit", &g_soldier_unit, sizeof(void*), &v)) return;
    void* unit = *(void* const*)v;
    if (!unit || !unit_is_live(unit)) return;

    if (!field_ptr(unit, "m_kPathingPawn", &g_path_pawn_field, sizeof(void*), &v)) return;
    void* ppawn = *(void* const*)v;
    if (!ppawn || !unit_is_live(ppawn)) return;

    if (!field_ptr(ppawn, "vTargetPoint", &g_target_point, 3 * sizeof(float), &v)) return;
    const float* pt = (const float*)v;
    if (pt[0] + pt[1] + pt[2] == 0.0f) return;      // the game's own test

    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;
    strncpy_s(it.name, sizeof it.name, "Tutorial target", _TRUNCATE);
    int placed = scan_item_at(&it, pt, TUTORIAL_POINT_LIFT);
    if (placed) scan_add(&it);

    // Logged when the point moves, which is when the tutorial advances a
    // step -- not once per press, and not every frame.
    static float said[3];
    if (pt[0] != said[0] || pt[1] != said[1] || pt[2] != said[2]) {
        memcpy(said, pt, sizeof said);
        logf_("tutorial: movement restricted to (%.0f, %.0f, %.0f)%s\n",
              pt[0], pt[1], pt[2],
              placed ? "" : " -- off the grid, not offered");
        if (placed) logf_("tutorial: that is tile %d, %d, floor %d\n",
                          it.tx, it.ty, it.tz);
    }
}

// ---- the ways up -----------------------------------------------------------
//
// A ramp is not an actor: XComWorldData holds the ways up a ledge as
// COVER_ClimbOnto_* and COVER_ClimbOver_* in the same flags word GetCoverPoint
// already answers with, and a ramp shows as the ClimbOnto that leads onto it.
// So this walks the tiles around the soldier and keeps the ones whose flags
// say a unit can get up there. One query per tile, so it is bounded, and it
// runs only for its own category.

static void scan_add_climbs(void)
{
    void* world_data = cursor_world();
    if (!g_scan_have || !world_data) return;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world_data, g_tile_slot_cover);
    if (!cover) return;

    const CursorGrid* g = &g_scan_grid;
    float z = g_scan_world_z + 4.0f;    // just off the floor, as walls_scan asks
    static int kept[SCAN_MAX][2];
    int nkept = 0;

    for (int dy = -SCAN_CLIMB_RADIUS; dy <= SCAN_CLIMB_RADIUS; dy++) {
        for (int dx = -SCAN_CLIMB_RADIUS; dx <= SCAN_CLIMB_RADIUS; dx++) {
            int x = g_scan_from[0] + dx, y = g_scan_from[1] + dy;
            if (x < 0 || y < 0 || x >= g->num_x || y >= g->num_y) continue;

            float wx = g->min_x + ((float)x + 0.5f) * CURSOR_TILE;
            float wy = g->min_y + ((float)y + 0.5f) * CURSOR_TILE;
            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            // An answer about some other tile is an answer about some other
            // floor, and is worth less than no answer at all -- the same test
            // walls_scan makes.
            if (!cover(world_data, NULL, wx, wy, z, &cp)) continue;
            if (cp.x != x || cp.y != y) continue;

            int onto = (cp.flags & COVER_CLIMB_ONTO) != 0;
            int over = (cp.flags & COVER_CLIMB_OVER) != 0;
            if (!onto && !over) continue;

            // One entry per ledge, not per tile. Climbable cover is
            // everywhere -- the first run found 155 of them within twelve
            // tiles, which is a list nobody can use -- and a wall you can
            // vault is one place, however many tiles long it is. So a tile is
            // kept only when nothing already kept is within SCAN_CLIMB_APART.
            // NB: not `near` -- windows.h still defines that as nothing,
            // and `int near = 0;` compiles to `int = 0;`.
            int crowded = 0;
            for (int k = 0; k < nkept && !crowded; k++) {
                int kx = kept[k][0] - x, ky = kept[k][1] - y;
                if (kx * kx + ky * ky <= SCAN_CLIMB_APART * SCAN_CLIMB_APART)
                    crowded = 1;
            }
            if (crowded) continue;
            if (nkept < SCAN_MAX) {
                kept[nkept][0] = x;
                kept[nkept][1] = y;
                nkept++;
            }

            ScanItem it;
            memset(&it, 0, sizeof it);
            it.kind = SCAN_INTERACT;
            strncpy_s(it.name, sizeof it.name,
                      onto ? "Ledge up" : "Low wall", _TRUNCATE);
            float here[3] = { wx, wy, z };
            if (scan_item_at(&it, here, 0.0f)) scan_add(&it);
        }
    }
}

// ---- building and saying ---------------------------------------------------

// Where the scan is measured from: the soldier being moved, falling back to
// the cursor when there is none. The height convention is nav's own -- a
// pawn's Location is a lift above its feet.
static int scan_origin(CursorGrid* g, int* tx, int* ty, int* tz, float* world_z)
{
    float z;
    if (!cursor_grid(g)) return 0;
    if (!soldier_tile(g, tx, ty, &z) && !cursor_tile(g, tx, ty, &z)) return 0;

    // While a step is being navigated, THAT tile is where the player is, and
    // it is what everything here is measured from: an offset is the keys left
    // to press, so after stepping one north towards a Floater two north the
    // answer has to be one north. The radar has always worked this way; the
    // scanner measured from the soldier and so kept saying two.
    //
    // Only while navigating. Off the numpad the cursor cannot be trusted to
    // say where the player is -- in mouse mode it follows the mouse every
    // frame, and a soldier switch leaves it wherever the mouse happens to
    // point -- which is the same reason navigation itself begins from the
    // soldier rather than from the cursor.
    int ntx, nty;
    if (nav_active() && nav_target(&ntx, &nty)) {
        int cx, cy;
        float cz;
        // The height comes from the cursor, which the mod places on the
        // navigated tile -- but only once it is actually there. A placement
        // that has not landed yet would otherwise hand over a height from
        // the tile the cursor is still on, which on a stairwell is a
        // different floor; the soldier's stands in until it does.
        if (cursor_tile(g, &cx, &cy, &cz) && cx == ntx && cy == nty) z = cz;
        *tx = ntx;
        *ty = nty;
    }

    float feet[3];
    feet[0] = g->min_x + ((float)*tx + 0.5f) * CURSOR_TILE;
    feet[1] = g->min_y + ((float)*ty + 0.5f) * CURSOR_TILE;
    feet[2] = z - NAV_CURSOR_LIFT;
    *tz = floor_of(feet);
    // The climb scan asks the cover native, which wants a world height and
    // not a floor number -- so the height is carried out separately rather
    // than worked back out of a floor that is three grid rows deep.
    if (world_z) *world_z = feet[2];
    return 1;
}

static int scan_rebuild(void)
{
    CursorGrid g;
    int tx, ty, tz;
    // Asked before scan_begin, so a moment when there is no cursor to measure
    // from leaves the list that was there rather than emptying it.
    if (!scan_origin(&g, &tx, &ty, &tz, &g_scan_world_z)) return scan_count();
    g_scan_have = 1;
    g_scan_grid = g;
    g_scan_from[0] = tx;
    g_scan_from[1] = ty;
    g_scan_from[2] = tz;

    scan_begin(tx, ty, tz);
    ScanCategory c = scan_category();
    if (c == SCAN_ALL || c == SCAN_SQUAD || c == SCAN_ENEMIES || c == SCAN_CIVILIANS)
        scan_add_units();
    // Its own category only: "Everything" already has these enemies once,
    // under Enemies.
    if (c == SCAN_TARGETS) scan_add_targets();
    if (c == SCAN_ALL || c == SCAN_DOORS || c == SCAN_OBJECTIVES || c == SCAN_INTERACT ||
        c == SCAN_EXPLOSIVES || c == SCAN_MELD)
        scan_add_world();
    // Three field reads and no walk, so it costs nothing outside a tutorial
    // -- and inside one it is the only objective that matters.
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_tutorial();
    if (c == SCAN_ALL || c == SCAN_OBJECTIVES) scan_add_evac();
    // The climb scan is a query per tile, so it runs only when its own
    // category is showing: "Everything" would pay for it on every press, and
    // a hundred ledges would bury the doors and the people in it anyway.
    if (c == SCAN_INTERACT) scan_add_climbs();
    return scan_end();
}

static void scan_say(const char* what)
{
    logf_("scan: %s\n", what);
    speech_say_now(what);
}

static void scan_say_selected(void)
{
    ScanItem it;
    char say[SCAN_MAX_TEXT];
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    scan_describe(&it, g_scan_from[0], g_scan_from[1], g_scan_from[2],
                  say, sizeof say);
    logf_("scan: %s  [%d of %d, %s]\n", say, scan_index(), scan_count(),
          scan_category_name(scan_category()));
    // A soldier picked here is the one "Follow one soldier" hears. Anything
    // else leaves the one already followed.
    if (it.kind == SCAN_SQUAD && strcmp(it.name, g_heart_follow) != 0) {
        strncpy_s(g_heart_follow, sizeof g_heart_follow, it.name, _TRUNCATE);
        if (settings_get(SET_HEART_SOLO))
            logf_("hearts: following %s\n", g_heart_follow);
    }
    speech_say_now(say);
}

// Page Up and Page Down, with Ctrl for the category and Alt for the storey.
static void scan_press(int dir, int ctrl, int alt)
{
    char say[SCAN_MAX_TEXT];

    if (ctrl) {
        scan_cycle_category(dir);
        int n = scan_rebuild();
        scan_category_text(scan_category(), scan_floor(), n, say, sizeof say);
        scan_say(say);
        return;
    }

    if (alt) {
        scan_cycle_floor(dir, floor_count());
        int n = scan_rebuild();
        char where[48];
        scan_floor_text(scan_floor(), where, sizeof where);
        scan_category_text(scan_category(), scan_floor(), n, say, sizeof say);
        logf_("scan: %s %s\n", where, say);
        speech_say_now(say);
        return;
    }

    scan_rebuild();
    scan_cycle(dir);
    scan_say_selected();
}

// Home: put the cursor on the selection, so the camera goes there and the tile
// describes itself as a step would. Shift+Home goes back to the soldier.
//
// This is the same start nav_press makes on its first key, with one
// difference: the item's own height is a far better ground estimate than the
// cursor's, so the floor search usually settles on its first frame.
static void scan_focus(int tx, int ty, float ground, const char* what)
{
    CursorGrid g;
    if (!cursor_grid(&g)) return;

    nav_begin(tx, ty);
    g_nav_cursor = cursor_object();
    g_nav_pawn = NULL;
    cursor_chained_pawn(&g_nav_pawn);
    GetCursorPos(&g_nav_mouse);
    g_nav_parked = 0;
    navh_set_ground(ground);
    navh_begin_tile();
    // While aiming, Home puts the aim on the selection -- at the floor under
    // it, which for a unit on a roof is the roof. It is also the one way to
    // lift an aim onto a higher floor: numpad steps only ever go down to one.
    g_nav_aim = soldier_aiming();
    if (g_nav_aim) g_aim_floor = aim_floor(&g, tx, ty, ground);
    nav_arrive(tx, ty);

    g_nav_world[0] = g.min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    g_nav_world[1] = g.min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    g_nav_world[2] = g_nav_aim ? g_aim_floor : navh_query_z();
    g_nav_live = 1;
    g_nav_key_at = GetTickCount64();
    logf_("scan: cursor to %s on %d, %d, ground %.1f\n", what, tx, ty, ground);
}

// F and C: the target one storey up or down -- the game's own keys for it
// ("Change Cursor Altitude", also the mouse wheel). In the moving state and
// while aiming a rocket or grenade the game runs XCom3DCursor.AscendFloor /
// DescendFloor on them, which is the next *storey* (WorldZToCursorFloor, 192
// units), snapped to whatever surface it has there. Numpad navigation holds
// the cursor's height itself every frame, so without this the keys moved the
// camera's cut-away and nothing else. They still reach the game, which keeps
// that cut-away in step.
//
// The next floor is the first surface, going the way asked through the grid's
// layers, at least FLOOR_STEP_MIN from where the target stands. A layer's
// surface is asked of GetFloorZForPosition from the layer's top -- the floor at
// or below that point -- with IsPositionOnFloor at its middle as the fallback.
// It used to be the midpoint test alone, and the first surface in a different
// *game storey* (WorldZToCursorFloor): on 2026-09-27 a raised floor at 32 and
// a floor at 129.8 were one storey to the game, so F refused, and on the tile
// beside it the midpoint test found nothing below 129.8, so C refused too --
// "no floors below" from a floor the player had just stepped up to. A step
// or a crate top, closer than FLOOR_STEP_MIN, is still passed over. The
// target is then put there exactly as Home puts it on a scanner item, so a
// move gets its path verdict and an aim its odds, with "Floor N." in front.
#define FLOOR_KEYS 2
#define FLOOR_STEP_MIN 96.0f
static int g_floor_down[FLOOR_KEYS];      // F, C

// `seen` gets what each layer tried answered, for the log: "6:- 5:F0@212.6"
// is no floor on layer 6, a floor at 212.6 in storey 0 on layer 5.
static int floor_probe(const CursorGrid* g, int tx, int ty, float from, int dir,
                       float* out, int* storey, char* seen, size_t seen_sz)
{
    size_t used = 0;
    seen[0] = 0;
    void* world = cursor_world();
    if (!world) return 0;
    PositionTestFn on_floor = (PositionTestFn)tile_vfn(world, g_tile_slot_onfloor);
    FloorZFn floorz = (FloorZFn)tile_vfn(world, g_tile_slot_floorz);
    if (!on_floor) return 0;
    float x = g->min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    float y = g->min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    float here[3] = { x, y, from + 4.0f };
    int cur = floor_of(here);
    int layer = cursor_tile_axis(from + 4.0f, g->min_z, 64.0f);
    int w = _snprintf_s(seen, seen_sz, _TRUNCATE, "from layer %d storey %d:", layer, cur);
    if (w > 0) used = (size_t)w;
    (void)cur;
    // Down asks the game first for the floor at or below a step under the
    // target, which needs no grid layer: a sunken floor can lie under the
    // grid's bottom. The log of 2026-09-27 had one at -59, layer -1; F went up
    // from it to 69, and C, walking layers 1 and 0 and no further, could not
    // find it again.
    if (dir < 0 && floorz) {
        float probe[3] = { x, y, from - FLOOR_STEP_MIN };
        float z = floorz(world, NULL, probe, 1);           // bUnlimitedSearch
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below(raw %.1f)", z);
        if (w > 0) used += (size_t)w;
        if (z != probe[2] && z <= probe[2] + 1.0f && z > probe[2] - 4096.0f) {
            float at[3] = { x, y, z + 4.0f };
            int f = floor_of(at);
            w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below:F%d@%.1f", f, z);
            if (w > 0) used += (size_t)w;
            *out = z;
            *storey = f;
            return 1;
        }
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " below:-");
        if (w > 0) used += (size_t)w;
    }
    // Down starts in the target's own layer: a surface below it there is
    // still further than the step, or not.
    for (int tz = dir < 0 ? layer : layer + dir; tz >= 0 && (g->num_z <= 0 || tz < g->num_z);
         tz += dir) {
        float bottom = g->min_z + (float)tz * 64.0f;
        float top[3] = { x, y, bottom + 63.0f };
        float z = 0.0f;
        int has = 0;
        if (floorz) {
            z = floorz(world, NULL, top, 0);
            has = z != top[2] && z >= bottom - 1.0f && z <= bottom + 63.0f;
        }
        if (!has) {
            float mid[3] = { x, y, bottom + 32.0f };
            if (on_floor(world, NULL, mid)) {
                z = aim_floor_exact(world, mid, bottom);
                has = 1;
            }
        }
        if (!has) {
            w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " %d:-", tz);
            if (w > 0) used += (size_t)w;
            continue;
        }
        float at[3] = { x, y, z + 4.0f };
        int f = floor_of(at);
        w = _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " %d:F%d@%.1f", tz, f, z);
        if (w > 0) used += (size_t)w;
        if ((dir > 0 && z < from + FLOOR_STEP_MIN) || (dir < 0 && z > from - FLOOR_STEP_MIN))
            continue;
        *out = z;
        *storey = f;
        return 1;
    }
    return 0;
}

// The surfaces the cursor has stood on, per tile. GetFloorZForPosition does
// not report every surface the game lets a soldier stand on: on 2026-09-27 the
// cursor settled on 102.2 at 31, 47 (the top of something, found by the game's
// own cursor validation), F went up to 219.3, and neither the direct query nor
// any layer found 102.2 again -- "No floor below" from where the player had
// just been. So every height the search settles on, and every floor F / C
// reaches, is kept, and F / C take the nearest kept one in the way asked when
// it is closer than what the probe found.
#define KNOWN_SURFACES 512
static struct { short tx, ty; float z; } g_known[KNOWN_SURFACES];
static int g_known_n, g_known_next;
static void* g_known_world;           // another map is another set

static void surface_forget_if_new_map(void)
{
    void* w = cursor_world();
    if (w == g_known_world) return;
    g_known_world = w;
    g_known_n = g_known_next = 0;
}

static void surface_note(int tx, int ty, float z)
{
    surface_forget_if_new_map();
    for (int i = 0; i < g_known_n; i++)
        if (g_known[i].tx == tx && g_known[i].ty == ty &&
            g_known[i].z > z - 16.0f && g_known[i].z < z + 16.0f)
            return;
    int slot = g_known_n < KNOWN_SURFACES ? g_known_n++ : g_known_next++ % KNOWN_SURFACES;
    g_known[slot].tx = (short)tx;
    g_known[slot].ty = (short)ty;
    g_known[slot].z = z;
}

static int surface_known(int tx, int ty, float from, int dir, float* out)
{
    surface_forget_if_new_map();
    int found = 0;
    for (int i = 0; i < g_known_n; i++) {
        if (g_known[i].tx != tx || g_known[i].ty != ty) continue;
        float d = (g_known[i].z - from) * (float)dir;
        if (d < FLOOR_STEP_MIN) continue;
        if (!found || d < (*out - from) * (float)dir) { *out = g_known[i].z; found = 1; }
    }
    return found;
}

// The next floor: the probe's answer or a kept surface, whichever is nearer.
static int floor_next(const CursorGrid* g, int tx, int ty, float from, int dir,
                      float* out, int* storey, char* seen, size_t seen_sz)
{
    float zp = 0.0f, zk = 0.0f;
    int sp = 0;
    int probe = floor_probe(g, tx, ty, from, dir, &zp, &sp, seen, seen_sz);
    int known = surface_known(tx, ty, from, dir, &zk);
    size_t used = strlen(seen);
    if (known)
        _snprintf_s(seen + used, seen_sz - used, _TRUNCATE, " known:%.1f", zk);
    if (!probe && !known) return 0;
    if (known && (!probe || (zk - from) * (float)dir < (zp - from) * (float)dir)) {
        float at[3] = { g->min_x + ((float)tx + 0.5f) * CURSOR_TILE,
                        g->min_y + ((float)ty + 0.5f) * CURSOR_TILE, zk + 4.0f };
        *out = zk;
        *storey = floor_of(at);
        return 1;
    }
    *out = zp;
    *storey = sp;
    return 1;
}

// What the game's own F / C made of the key: it still runs AscendFloor /
// DescendFloor on its cursor, and the storey it reached is kept there even
// though navigation sets the cursor's position. Read a moment after the key,
// so the log can say whether the game found a floor where the mod did not.
#define FLOOR_GAME_CHECK_MS 250
static ULONGLONG g_floor_check_at;
static FieldSlot g_cur_requested, g_cur_effective, g_cur_camfloor;

static void floor_game_check(void)
{
    void* cur = cursor_object();
    const void* v;
    int req = -99, eff = -99;
    float cam = -99999.0f;
    if (!cur) return;
    if (field_ptr(cur, "m_iRequestedFloor", &g_cur_requested, sizeof(int32_t), &v))
        req = *(const int32_t*)v;
    if (field_ptr(cur, "m_iLastEffectiveFloorIndex", &g_cur_effective, sizeof(int32_t), &v))
        eff = *(const int32_t*)v;
    // EW only; EU keeps the floor's bounds in other fields.
    if (field_ptr(cur, "m_fLogicalCameraFloorHeight", &g_cur_camfloor, sizeof(float), &v))
        cam = *(const float*)v;
    logf_("nav: the game's own cursor after the key: requested floor %d, reached %d, "
          "camera floor height %.1f\n", req, eff, cam);
}

static void nav_floor(int dir)
{
    CursorGrid g;
    int tx, ty;
    float from, z;
    if (!cursor_grid(&g)) return;
    if (nav_active() && nav_target(&tx, &ty)) {
        from = g_nav_aim ? g_aim_floor : navh_ground();
    } else if (cursor_tile(&g, &tx, &ty, &z)) {
        from = z - NAV_CURSOR_LIFT;
    } else {
        return;
    }
    float to = from;
    int storey = 0, found = 0;
    char seen[512] = "";
    g_floor_check_at = GetTickCount64() + FLOOR_GAME_CHECK_MS;
    Fault f;
    __try { found = floor_next(&g, tx, ty, from, dir, &to, &storey, seen, sizeof seen); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: floor key", &f, NULL);
        found = 0;
    }
    if (!found) {
        logf_("nav: %s at %d, %d from %.1f -- no floor that way (%s)\n",
              dir > 0 ? "F" : "C", tx, ty, from, seen);
        char missed[128] = "", say[160];
        Fault f2;
        __try { floor_missed(tx, ty, from, 0, from, dir, missed, sizeof missed); }
        __except (fault_note(GetExceptionInformation(), &f2)) {
            fault_log("nav: floor missed", &f2, NULL);
            missed[0] = 0;
        }
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s",
                    dir > 0 ? "No floor above here." : "No floor below here.",
                    missed[0] ? " " : "", missed);
        speech_cancel_pending();
        speech_say_now(say);
        return;
    }
    logf_("nav: %s at %d, %d: %.1f -> %.1f, storey %d (%s)\n", dir > 0 ? "F" : "C",
          tx, ty, from, to, storey, seen);
    scan_focus(tx, ty, to, dir > 0 ? "the floor above" : "the floor below");
    // How far: inside a building, in its own floors (asked for 2026-09-27,
    // after "2 storeys up" from a tall ground floor to the one above it);
    // elsewhere, or between two heights on the same floor of it, in storeys
    // of 192 -- not the camera's floor number (tile_height_step).
    int levels = 0;
    if (where_levels_between(tx, ty, from, to, &levels) && levels)
        tile_floor_step(levels, g_step_note, sizeof g_step_note);
    else
        tile_height_step(to - from, g_step_note, sizeof g_step_note);
    {
        char missed[128] = "";
        Fault f2;
        __try { floor_missed(tx, ty, from, 1, to, dir, missed, sizeof missed); }
        __except (fault_note(GetExceptionInformation(), &f2)) {
            fault_log("nav: floor missed", &f2, NULL);
            missed[0] = 0;
        }
        if (missed[0]) {
            size_t used = strlen(g_step_note);
            _snprintf_s(g_step_note + used, sizeof g_step_note - used, _TRUNCATE, " %s", missed);
        }
    }
    g_floor_hold = 1;
    g_floor_hold_z = to;
    surface_note(tx, ty, from);
    surface_note(tx, ty, to);
    // Arrival counted the soldier as on this tile whatever their floor. Their
    // own tile is described on a timer, since no path is built to it -- but
    // on another floor of the column it gets a path like any other.
    int mine = 0;
    for (int i = 0; i < g_step_nunits; i++)
        if (g_step_units[i].mine && scan_storey_diff(g_step_units[i].feet, to) == 0) mine = 1;
    if (!mine) g_tile_due = 0;
}

static void scan_home(int shift)
{
    CursorGrid g;
    char say[SCAN_MAX_TEXT];
    int tx, ty;
    float z;

    if (shift) {
        if (!cursor_grid(&g) || !soldier_tile(&g, &tx, &ty, &z)) {
            scan_say("No soldier.");
            return;
        }
        scan_focus(tx, ty, z - NAV_CURSOR_LIFT, "the soldier");
        return;
    }

    ScanItem it;
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    if (it.unplaced) {
        scan_describe(&it, 0, 0, 0, say, sizeof say);
        scan_say(say);
        return;
    }
    scan_focus(it.tx, it.ty, it.world[2], it.name);
}

// End: how far the selection is and which way. Shift+End answers the same
// about the soldier, measured from wherever the cursor is now -- the way back.
static void scan_distance(int shift)
{
    CursorGrid g;
    char say[SCAN_MAX_TEXT];

    if (shift) {
        int sx, sy, cx, cy;
        float sz, cz;
        if (!cursor_grid(&g) || !soldier_tile(&g, &sx, &sy, &sz) ||
            !cursor_tile(&g, &cx, &cy, &cz)) {
            scan_say("No soldier.");
            return;
        }
        char where[64];
        tile_offset_text(sx - cx, sy - cy, where, sizeof where);
        _snprintf_s(say, sizeof say, _TRUNCATE, "Soldier, %s.", where);
        scan_say(say);
        return;
    }

    ScanItem it;
    if (!scan_selected(&it)) {
        scan_empty_text(scan_category(), say, sizeof say);
        scan_say(say);
        return;
    }
    // Measured afresh from where the player is NOW -- the navigated tile
    // while there is one, the soldier otherwise -- and not from where the
    // scan was taken: the whole point of the key is to ask again after
    // moving, and the answer is the keys still to press.
    int tx, ty, tz;
    if (scan_origin(&g, &tx, &ty, &tz, &g_scan_world_z)) {
        g_scan_grid = g;
        g_scan_from[0] = tx; g_scan_from[1] = ty; g_scan_from[2] = tz;
        g_scan_have = 1;
    }
    scan_describe(&it, g_scan_from[0], g_scan_from[1], g_scan_from[2],
                  say, sizeof say);
    scan_say(say);
}

// The scanner's keys. Page Up and Page Down are unbound in a mission, and Home
// only raises an InputEvent nothing handles, so all three are read the way the
// numpad is. End is the exception -- it is End Turn -- and is swallowed in
// hook_moviecheck, which is the only reason it can be used here.
static int g_scan_down[4];      // Page Up, Page Down, Home, End

static void scan_poll(void)
{
    static const int keys[4] = { VK_PRIOR, VK_NEXT, VK_HOME, VK_END };
    int ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    int shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;

    for (int k = 0; k < 4; k++) {
        int down = (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
        if (down && !g_scan_down[k]) {
            Fault f;
            __try {
                switch (k) {
                case 0: scan_press(-1, ctrl, alt); break;
                case 1: scan_press(+1, ctrl, alt); break;
                case 2: scan_home(shift);          break;
                case 3: scan_distance(shift);      break;
                }
            }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("scan", &f, NULL);
            }
        }
        g_scan_down[k] = down;
    }
}

// ---- the ability menu ------------------------------------------------------
//
// Numpad . opens the ability bar as a menu: numpad 8 / 2 or the Up / Down
// arrows walk it, numpad 5 says the entry again, numpad . or Escape closes
// it. Each entry is the key and name, the status, and the ability's own
// tooltip -- XGAbility.strHelp, what GetHelpText returns and the info panel
// shows while aiming (SetHelp). It is read off the container's
// m_arrAbilities, the same array PopulateFlash drew slot by slot.
//
// Decimal is bound to nothing in a mission -- DefaultInput.ini names it only
// in the edit-box alias lists, as it does * and /. The arrows and Escape are
// bound (Arrow_Up .. InputEvent 500-503, Escape 510), so while the menu is
// open they are kept from the game in hook_moviecheck, the same way End is.
static FieldSlot g_bar_abilities, g_ability_help;

static void abar_help(int index, char* out, size_t out_sz)
{
    out[0] = 0;
    const void* v;
    if (!g_abar_obj || !unit_is_live(g_abar_obj) ||
        !field_ptr(g_abar_obj, "m_arrAbilities", &g_bar_abilities, sizeof(FArray), &v))
        return;
    const FArray* a = (const FArray*)v;
    if (index < 0 || index >= a->Num || a->Num > 64 ||
        !readable(a->Data, (size_t)a->Num * sizeof(void*)))
        return;
    void* ability = ((void* const*)a->Data)[index];
    if (!ability || !unit_is_live(ability) ||
        !field_ptr(ability, "strHelp", &g_ability_help, sizeof(FString), &v))
        return;
    if (read_fstring((const FString*)v, out, out_sz)) strip_markup(out);
}

static void abar_say_entry(void)
{
    char help[512], say[1024];
    int i = abar_menu_index();
    abar_help(i, help, sizeof help);
    if (!abar_entry(i, help, say, sizeof say))
        strncpy_s(say, sizeof say, "No abilities.", _TRUNCATE);
    logf_("abar: %d of %d \"%s\"\n", i + 1, abar_count(), say);
    speech_cancel_pending();
    speech_say_now(say);
}

// The menu's keys, as last seen: numpad ., numpad 8, numpad 2, numpad 5, Up,
// Down, Escape, Enter, Space.
#define MENU_KEYS 9
static const int g_menu_vk[MENU_KEYS] = {
    VK_DECIMAL, VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD5, VK_UP, VK_DOWN, VK_ESCAPE,
    VK_RETURN, VK_SPACE
};
static int g_menu_was[MENU_KEYS];

// Enter uses the ability: it becomes the ability's own number key.
//
// In a mission Enter and Space both raise InputEvent(513) (Spacebar_Key_Press;
// Enter's binding in [XComGame.XComTacticalInput] is the same command), and
// the number keys 1-0 raise 612-621, which
// UITacticalHUD_AbilityContainer.OnUnrealCommand turns into
// DirectPickAbility(0..9) -- on the key's RELEASE, since it returns early for
// anything without mask 32. So nothing is synthesised: the Enter keystroke's
// own two events are rewritten in InputEvent's frame, press and release, from
// 513 to 612 + index, and the game does exactly what that number key does.
//
// The rewrite has to happen before InputEvent looks at Cmd at all, and in EW
// that is earlier than IsAnyMoviePlaying. EW's InputEvent opens with
// PreProcessEventMatching, which drops a release unless m_arrEventTrackers
// holds a press of the same Cmd, and it closes with ActivateTracker(Cmd), which
// records the press under whatever Cmd has become by then. Rewritten at
// IsAnyMoviePlaying, the press was recorded as the number key, Enter's release
// was dropped before the hook ever saw it, and the number key stayed "held":
// InputRepeatTimer sent it again every 0.1 s for the rest of the session. That
// was the stream of 614-616 the pause menu received with nobody pressing
// anything. So the rewrite is done at GetEngine, the first native InputEvent
// calls, in both builds (the copy-protection check); EU matches after
// IsAnyMoviePlaying, but the earlier point is right for it too.
// That means the same thing a second press of a number key means:
// the first selects the ability and raises targeting, and picking the one
// already selected fires it (DirectPickAbility -> OnAccept).
//
// An ability past the tenth has no number key and so cannot be picked this
// way; the menu says so.
#define INPUT_CMD_ENTER     513
#define INPUT_CMD_ABILITY_1 612
#define ABILITY_KEYS        10
#define PICK_WAIT_MS        500
static int       g_pick_index = -1;     // chosen by the poll, for the hook
static ULONGLONG g_pick_until;
static int       g_pick_release_to = -1; // Enter's release, still to rewrite

// The game's side of the same keys: presses are taken while the menu is open,
// and for a moment after it closes, because the frame that closes the menu may
// run before the game's InputEvent for the key that closed it.
//
// Releases are never taken, because the game already drops them. A press
// swallowed at IsAnyMoviePlaying returns before InputEvent's ActivateTracker,
// so no tracker is made, and PreProcessEventMatching drops the release (EW
// before this hook, EU after it). That is also why the Escape that closed the
// menu cannot reach the game alone.
#define MENU_GRACE_MS 150

static void abar_menu_end(const char* why)
{
    abar_menu_close();
    g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
    logf_("abar: menu closed (%s)\n", why);
    speech_cancel_pending();
    speech_say_now("Closed.");
}

// When the menu last had its keys read. The poll runs from the battle
// cursor's per-frame native, so a mission that ends with the menu open stops
// polling it -- and a menu nobody is polling must not go on taking the arrows
// from whatever screen comes next.
#define MENU_STALE_MS 500
static ULONGLONG g_menu_polled_at;

// Returns 1 while the menu is open, having handled its keys.
static int abar_menu_poll(void)
{
    g_menu_polled_at = GetTickCount64();
    int now[MENU_KEYS], pressed[MENU_KEYS];
    for (int k = 0; k < MENU_KEYS; k++) {
        now[k] = (GetAsyncKeyState(g_menu_vk[k]) & 0x8000) != 0;
        pressed[k] = now[k] && !g_menu_was[k];
        g_menu_was[k] = now[k];
    }

    if (!abar_menu_is_open()) {
        if (!pressed[0] || history_is_open()) return 0;
        abar_menu_open();
        logf_("abar: menu opened, %d abilities\n", abar_count());
        if (abar_count() <= 0) {
            abar_menu_close();
            speech_cancel_pending();
            speech_say_now("No abilities.");
            return 0;
        }
        abar_say_entry();
        return 1;
    }

    if (pressed[0]) { abar_menu_end("numpad ."); return 0; }
    if (pressed[6]) { abar_menu_end("Escape"); return 0; }
    if (pressed[7] || pressed[8]) {
        int i = abar_menu_index();
        if (i < 0 || i >= ABILITY_KEYS) {
            logf_("abar: %d has no number key -- cannot be picked\n", i + 1);
            speech_cancel_pending();
            speech_say_now("No key for this ability.");
            return 1;
        }
        // Handed to the hook, which rewrites this same keystroke. Silent: the
        // shot readout names the ability as soon as the game selects it.
        g_pick_index = i;
        g_pick_until = GetTickCount64() + PICK_WAIT_MS;
        abar_menu_close();
        g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
        logf_("abar: Enter picks ability %d\n", i + 1);
        return 0;
    }
    if (pressed[1] || pressed[4]) { abar_menu_step(-1); abar_say_entry(); }
    else if (pressed[2] || pressed[5]) { abar_menu_step(1); abar_say_entry(); }
    else if (pressed[3]) abar_say_entry();
    return 1;
}

// Whether InputEvent(cmd, mask) belongs to the menu and is to be kept from
// the game. Called from hook_moviecheck.
static int abar_menu_swallow(int cmd, int mask)
{
    // Enter is here for an ability with no number key; one that has a key was
    // already rewritten at GetEngine and arrives as that key.
    if (!((cmd >= 500 && cmd <= 503) || cmd == 510 || cmd == INPUT_CMD_ENTER))
        return 0;
    if (!(mask & 1)) return 0;
    ULONGLONG t = GetTickCount64();
    if ((abar_menu_is_open() || history_is_open()) && t - g_menu_polled_at > MENU_STALE_MS) {
        abar_menu_close();
        history_close();
        logf_("abar: menu closed (no longer polled -- the mission ended)\n");
    }
    if (!abar_menu_is_open() && !history_is_open() && t >= g_menu_grace_until) return 0;
    return 1;
}

static int input_event_cmd(void* stack, int* mask_out);
static int32_t* input_event_cmd_slot(void* stack);

// Whether the GetEngine hook has anything to do. It runs on every call to
// GetEngine, from everywhere, so this is the whole cost of the hook almost
// all of the time.
static int abar_pick_pending(void)
{
    return abar_menu_is_open() || g_pick_release_to >= 0 ||
           (g_pick_index >= 0 && GetTickCount64() < g_pick_until);
}

// Rewrites Enter's InputEvent into the chosen ability's number key. Called
// from GetEngine, whose caller is InputEvent when this does anything at all.
static void abar_menu_pick(void* stack)
{
    int mask = 0;
    if (input_event_cmd(stack, &mask) != INPUT_CMD_ENTER) return;
    int32_t* slot = input_event_cmd_slot(stack);
    if (!slot || !writable(slot, sizeof *slot)) return;

    if (mask & 32) {
        if (g_pick_release_to < 0) return;
        *slot = g_pick_release_to;
        logf_("abar: Enter's release -> InputEvent %d, the ability's number key\n",
              g_pick_release_to);
        g_pick_release_to = -1;
        return;
    }
    if (!(mask & 1)) return;

    // The press. The poll may have seen the key first and closed the menu,
    // leaving its choice in g_pick_index; or this may come first, with the
    // menu still open. A press that picks nothing also ends any release still
    // owed, so a release that never came cannot hijack this keystroke's.
    int i = -1;
    if (abar_menu_is_open()) i = abar_menu_index();
    else if (g_pick_index >= 0 && GetTickCount64() < g_pick_until) i = g_pick_index;
    if (i < 0 || i >= ABILITY_KEYS) { g_pick_release_to = -1; return; }

    *slot = INPUT_CMD_ABILITY_1 + i;
    g_pick_release_to = *slot;
    g_pick_index = -1;
    if (abar_menu_is_open()) {
        abar_menu_close();
        g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
        logf_("abar: Enter picks ability %d\n", i + 1);
    }
    logf_("abar: Enter -> InputEvent %d\n", *slot);
}

// Insert: the announcements, as a list (history.h). Opens on the newest; Up
// and Down (numpad 8 / 2 or the arrows) walk it, numpad 5 says the entry
// again, Insert or Escape closes it. The arrows and Escape are bound in a
// mission, so while it is open hook_moviecheck keeps them from the game
// through abar_menu_swallow, exactly as for the ability menu. Insert is not:
// its only binding, Camera FreeCam, is removed with -Bindings in
// [Engine.PlayerInput].
#define REVIEW_KEYS 7
static const int g_review_vk[REVIEW_KEYS] = {
    VK_INSERT, VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD5, VK_UP, VK_DOWN, VK_ESCAPE
};
static int g_review_was[REVIEW_KEYS];

static void review_end(const char* why)
{
    history_close();
    g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
    logf_("review: closed (%s)\n", why);
    speech_cancel_pending();
    speech_say_now("Closed.");
}

// Returns 1 while the list is open, having handled its keys.
static int review_poll(void)
{
    int now[REVIEW_KEYS], pressed[REVIEW_KEYS];
    for (int k = 0; k < REVIEW_KEYS; k++) {
        now[k] = (GetAsyncKeyState(g_review_vk[k]) & 0x8000) != 0;
        pressed[k] = now[k] && !g_review_was[k];
        g_review_was[k] = now[k];
    }
    // A page's lines (history_page_open) are longer than an announcement.
    static char say[HISTORY_PAGE_TEXT + 64];
    if (!history_is_open()) {
        if (!pressed[0]) return 0;
        g_menu_polled_at = GetTickCount64();
        int opened = history_open(say, sizeof say);
        logf_("review: %s \"%s\"\n", opened ? "opened" : "nothing to open", say);
        speech_cancel_pending();
        speech_say_now(say);
        return opened;
    }
    g_menu_polled_at = GetTickCount64();
    if (pressed[0]) { review_end("Insert"); return 0; }
    if (pressed[6]) { review_end("Escape"); return 0; }
    if (pressed[1] || pressed[4]) history_step(-1, say, sizeof say);
    else if (pressed[2] || pressed[5]) history_step(1, say, sizeof say);
    else if (pressed[3]) history_current(say, sizeof say);
    else return 1;
    speech_cancel_pending();
    speech_say_now(say);
    return 1;
}

// Insert has to work everywhere -- the base, the shell, a mission -- and the
// only per-frame hook is the battle cursor's, which exists in a mission alone:
// in the base Insert was never read. So the list is polled on a thread of its
// own, as sound practice is (learn.c). history.c takes a lock, and speech is
// already safe from any thread. The keys are kept from the screen underneath
// by rewrite_cmd in a menu and by hook_moviecheck in a mission.
#define REVIEW_POLL_MS 15
static volatile LONG g_review_stop;
// Delete at the base: the date, the resources and what is coming. The same
// key says everything about the selected soldier in a mission (nav_poll), so
// it acts here only while the strategy HUD has drawn more recently than the
// tactical one.
//
// In the Situation Room it opens the room as a list instead (hq_sit_lines),
// on Insert's machinery, so the same keys walk and close it; Delete closes it
// too. In Engineering it opens the build queue the same way (hq_eng_lines),
// with the status line as its last entry.
static int g_status_was;
static void status_poll(void)
{
    int down = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    int pressed = down && !g_status_was;
    g_status_was = down;
    if (!pressed) return;
    if (history_page_is_open()) { review_end("Delete"); return; }
    if (history_is_open()) return;
    if (!g_seen_strategy_at || g_seen_tactical_at > g_seen_strategy_at) return;
    if (sitroom_up()) {
        typedef char page_fits_room[HQ_SIT_TEXT == HISTORY_PAGE_TEXT ? 1 : -1];
        static char lines[HISTORY_PAGE_MAX][HISTORY_PAGE_TEXT];
        static char say[HISTORY_PAGE_TEXT + 64];
        int n = hq_sit_lines(lines, HISTORY_PAGE_MAX);
        g_menu_polled_at = GetTickCount64();
        int opened = history_page_open("Situation Room", (const char (*)[HISTORY_PAGE_TEXT])lines,
                                       n, say, sizeof say);
        logf_("sitroom: %s, %d entries \"%s\"\n", opened ? "opened" : "nothing to open", n, say);
        speech_cancel_pending();
        if (g_speak) speech_say_now(say);
        return;
    }
    if (eng_up()) {
        static char lines[HISTORY_PAGE_MAX][HISTORY_PAGE_TEXT];
        static char say[HISTORY_PAGE_TEXT + 64];
        int n = hq_eng_lines(lines, HISTORY_PAGE_MAX);
        g_menu_polled_at = GetTickCount64();
        int opened = history_page_open("Engineering", (const char (*)[HISTORY_PAGE_TEXT])lines,
                                       n, say, sizeof say);
        logf_("engineering: %s, %d entries \"%s\"\n", opened ? "opened" : "nothing to open", n, say);
        speech_cancel_pending();
        if (g_speak) speech_say_now(say);
        return;
    }
    char say[1024];
    if (!hq_status_line(say, sizeof say))
        strncpy_s(say, sizeof say, "Nothing known about the base yet.", _TRUNCATE);
    logf_("status: \"%s\"\n", say);
    speech_cancel_pending();
    if (g_speak) speech_say_now(say);
}

static void hooks_sweep_retry(void);

static DWORD WINAPI review_pump(LPVOID unused)
{
    (void)unused;
    while (!g_review_stop) {
        Sleep(REVIEW_POLL_MS);
        if (!game_has_focus()) continue;
        {
            Fault f;
            __try { status_poll(); }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("status: poll", &f, NULL);
            }
        }
        {
            Fault f;
            __try { hooks_sweep_retry(); }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("hooks: late UFunction pass", &f, NULL);
            }
        }
        {
            Fault f;
            __try { alert_settle(); }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("alert: settle", &f, NULL);
            }
        }
        Fault f;
        __try { review_poll(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("review: poll", &f, NULL);
        }
    }
    return 0;
}

// Who an area attack would hit, as the game marks them while it is aimed.
// XGAction_Targeting.DrawSplashRadius, on every update of an aim at a spot
// (a rocket, a grenade, and the other blasts it lists), works out the centre
// and the radius and hands them to the native UpdateShotTargetLocation, which
// marks the actors inside (MarkTargetedActors) and keeps them in the action's
// m_arrMarkedTargets -- the highlight the sighted player sees -- beside the
// radius it used, m_fSplashRadiusCache. So the list is read as marked, not
// worked out again. Said a moment after each numpad aim step, once the game
// has drawn the blast at the new spot, queued behind the step and its odds:
// "In the blast: Sectoid, White." A blast with nobody in it says so.
#define BLAST_DELAY_MS 200
static ULONGLONG g_blast_due;
static FieldSlot g_marked_slot, g_splash_slot;

// Whether a destructible is cover, and how high: 2 high, 1 low, 0 not cover.
// Nothing on the actor says so -- ShouldIgnoreForCover is native and only
// rules things out -- so the game's own cover map is asked, as the tile
// readout asks it: the tiles just outside the object's bounds (its mesh's
// world box, PrimitiveComponent.Bounds), and whether the cover point there
// faces the object. North is +Y and the game's East is -X (see tile.h), so a
// tile on the object's -X side needs the game's West bit.
static FieldSlot g_blast_bounds, g_blast_health, g_blast_smc, g_blast_mesh;
static int destructible_cover(void* a)
{
    const void* v;
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;
    if (!field_ptr(a, "StaticMeshComponent", &g_blast_smc, sizeof(void*), &v) ||
        !*(void* const*)v)
        return 0;
    void* smc = *(void* const*)v;
    if (!field_ptr(smc, "Bounds", &g_blast_bounds, 7 * sizeof(float), &v)) return 0;
    const float* b = (const float*)v;           // Origin, BoxExtent, SphereRadius
    int x0 = cursor_tile_axis(b[0] - b[3] + 8.0f, g.min_x, CURSOR_TILE);
    int x1 = cursor_tile_axis(b[0] + b[3] - 8.0f, g.min_x, CURSOR_TILE);
    int y0 = cursor_tile_axis(b[1] - b[4] + 8.0f, g.min_y, CURSOR_TILE);
    int y1 = cursor_tile_axis(b[1] + b[4] - 8.0f, g.min_y, CURSOR_TILE);
    if (x1 < x0) x1 = x0;                       // thinner than a tile
    if (y1 < y0) y1 = y0;
    if (x1 - x0 > 12) x1 = x0 + 12;             // a long wall: its first stretch
    if (y1 - y0 > 12) y1 = y0 + 12;
    float z = b[2] - b[5] + 4.0f;               // the floor it stands on, as asked of a tile

    int best = 0;
    for (int side = 0; side < 4; side++) {
        int along = side < 2 ? x1 - x0 : y1 - y0;
        for (int i = 0; i <= along; i++) {
            int x, y, bit;
            switch (side) {
            case 0:  x = x0 + i; y = y0 - 1; bit = TILE_COVER_N; break;   // south of it
            case 1:  x = x0 + i; y = y1 + 1; bit = TILE_COVER_S; break;   // north of it
            case 2:  x = x0 - 1; y = y0 + i; bit = TILE_COVER_W; break;   // west of it
            default: x = x1 + 1; y = y0 + i; bit = TILE_COVER_E; break;   // east of it
            }
            if (x < 0 || y < 0 || x >= g.num_x || y >= g.num_y) continue;
            TileCoverPoint cp;
            memset(&cp, 0, sizeof cp);
            float wx = g.min_x + ((float)x + 0.5f) * CURSOR_TILE;
            float wy = g.min_y + ((float)y + 0.5f) * CURSOR_TILE;
            if (!cover(world, NULL, wx, wy, z, &cp) || cp.x != x || cp.y != y ||
                (cp.flags & TILE_COVER_DIAGONAL) || !(cp.flags & bit))
                continue;
            int level = (cp.flags & (bit << 4)) ? 1 : 2;   // the matching low bit
            if (level > best) best = level;
            if (best == 2) return 2;
        }
    }
    return best;
}

static void blast_say(void)
{
    void* unit = soldier_unit();
    const void* v;
    if (!unit || !field_ptr(unit, "m_kCurrAction", &g_curr_action, sizeof(void*), &v)) return;
    void* action = *(void* const*)v;
    char name[64];
    if (!action || !unit_is_live(action) || !object_name(action, name, sizeof name) ||
        strncmp(name, "XGAction_Targeting", 18) != 0)
        return;
    if (!field_ptr(action, "m_fSplashRadiusCache", &g_splash_slot, sizeof(float), &v)) return;
    float radius = *(const float*)v;
    if (!(radius > 0.0f)) return;                 // not an area attack
    if (!field_ptr(action, "m_arrMarkedTargets", &g_marked_slot, sizeof(FArray), &v)) return;
    const FArray* arr = (const FArray*)v;
    int num = arr->Num;
    void* const* data = (void* const*)arr->Data;
    if (num < 0 || num > 64 || (num && !readable(data, (size_t)num * sizeof(void*)))) return;

    // The marked actors are units or their pawns, matched against the units
    // the squad can see, which is what the screen can highlight. The squad's
    // own are said apart -- the first run had the rocket's own heavy in it as
    // "In the blast: Vargas, Dozer.", which also sounded like two people -- so
    // a soldier is named by surname alone there.
    const char* them[TILE_NAMES_MAX];
    const char* ours[TILE_NAMES_MAX];
    int nthem = 0, nours = 0, tthem = 0, tours = 0;
    unsigned char matched[64] = { 0 };
    void* squad = squad_player();
    // MarkTargetedActors marks every unit in the radius, hidden or not, and
    // a hidden one's highlight is never drawn -- so an alien or civilian is
    // named only while the squad sees them. It still counts as matched: it
    // is a unit, and must not be offered again as a destructible below.
    static SquadSight sight;
    squad_sight_take(squad, &sight);
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        int in = 0;
        for (int k = 0; k < num; k++)
            if (data[k] == s.unit || data[k] == s.pawn) { in = 1; matched[k] = 1; }
        if (!in) continue;
        if (!squad_sees(&sight, s.unit, s.loc, s.friendly, s.who->name)) continue;
        if (s.friendly) { if (nours < TILE_NAMES_MAX) ours[nours++] = s.who->name; tours++; }
        else            { if (nthem < TILE_NAMES_MAX) them[nthem++] = s.who->name; tthem++; }
    }
    // And those with no flag: a survivor in the blast went unmentioned,
    // being neither a flag's unit nor a destructible. `them` holds
    // pointers, so the names stay in `found` until the line is built.
    {
        static FlaglessUnit found[COLUMN_FLAGLESS];
        int nf = squad ? flagless_units(0, found, COLUMN_FLAGLESS) : 0;
        for (int i = 0; i < nf; i++) {
            int in = 0;
            for (int k = 0; k < num; k++)
                if (data[k] == found[i].unit || data[k] == found[i].pawn) { in = 1; matched[k] = 1; }
            if (!in || !squad_sees(&sight, found[i].unit, found[i].loc, 0, found[i].name))
                continue;
            if (nthem < TILE_NAMES_MAX) them[nthem++] = found[i].name;
            tthem++;
        }
    }

    // The destructibles. Only what explodes is named -- a car or a gas
    // canister chains on, which is worth hearing on every step; the rest,
    // cover chunks, posters, rubble, is a count. Named after their mesh, as
    // the scanner names them, and told from the rest by the scanner's own
    // list of blast owners (scan_describe_explosive). Already destroyed ones
    // (Health 0, as BeginDestroyed leaves them) are left out. The blast
    // owners are brought up to date first: the scanner and the door sounds
    // are what refresh them otherwise, and either may not have run here.
    {
        CursorGrid bg;
        if (cursor_grid(&bg)) world_refresh(&bg);
    }
    // Three lists: what explodes, and what is high or low cover
    // (destructible_cover). Anything else is a count.
    static char names[3][TILE_NAMES_MAX][SCAN_NAME];
    const char* namep[3][TILE_NAMES_MAX];
    int kept[3] = { 0 }, total[3] = { 0 }, others = 0, wrecked = 0;
    for (int k = 0; k < num; k++) {
        void* a = data[k];
        if (matched[k] || !a || !unit_is_live(a) || !object_is_a(a, "XComDestructibleActor"))
            continue;
        if (field_ptr(a, "Health", &g_blast_health, sizeof(int32_t), &v) &&
            *(const int32_t*)v <= 0) { wrecked++; continue; }
        int explodes = world_explodes(a);
        int list;
        if (explodes) list = 0;
        else {
            int c = destructible_cover(a);
            if (!c) { others++; continue; }
            list = c == 2 ? 1 : 2;
        }
        total[list]++;
        if (kept[list] >= TILE_NAMES_MAX) continue;
        char mesh[SCAN_NAME] = "";
        if (field_ptr(a, "StaticMeshComponent", &g_blast_smc, sizeof(void*), &v) &&
            *(void* const*)v &&
            field_ptr(*(void* const*)v, "StaticMesh", &g_blast_mesh, sizeof(void*), &v) &&
            *(void* const*)v)
            object_name(*(void* const*)v, mesh, sizeof mesh);
        char* out = names[list][kept[list]];
        scan_mesh_words(mesh, list ? "Cover" : "Explosive", out, SCAN_NAME);
        namep[list][kept[list]++] = out;
    }

    // "In the blast: 2 Floaters. Squad in the blast: Vargas. Explodes: Car.
    // High cover: Wall. Low cover: 2 Crates. 8 other objects."
    char say[768], text[256];
    size_t used = 0;
    say[0] = 0;
    if (tthem) {
        tile_names_counted(them, nthem, tthem, text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    "In the blast: %s.", text);
    }
    if (tours && used < sizeof say) {
        tile_names_counted(ours, nours, tours, text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    "%sSquad in the blast: %s.", used ? " " : "", text);
    }
    if (!tthem && !tours) {
        strcpy_s(say, sizeof say, "No one in the blast.");
        used = strlen(say);
    }
    static const char* heads[3] = { "Explodes", "High cover", "Low cover" };
    for (int l = 0; l < 3; l++) {
        if (!total[l] || used >= sizeof say) continue;
        tile_names_counted(namep[l], kept[l], total[l], text, sizeof text);
        used += (size_t)_snprintf_s(say + used, sizeof say - used, _TRUNCATE,
                                    " %s: %s.", heads[l], text);
    }
    if (others && used < sizeof say)
        _snprintf_s(say + used, sizeof say - used, _TRUNCATE, " %d other object%s.", others,
                    others == 1 ? "" : "s");
    logf_("blast: radius %.0f, %d marked, %d of them, %d of ours, %d explode, %d high cover, "
          "%d low cover, %d other objects, %d wrecked -> \"%s\"\n", radius, num, tthem, tours,
          total[0], total[1], total[2], others, wrecked, say);
    if (g_speak) speech_say(say);
}

static void blast_poll(void)
{
    if (!g_blast_due || GetTickCount64() < g_blast_due) return;
    g_blast_due = 0;
    if (!g_nav_aim) return;
    Fault f;
    __try { blast_say(); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("blast: read", &f, NULL);
    }
}

static void nav_poll(void)
{
    // Practice owns the numpad while it is on (learn.h). Navigation stands
    // aside completely rather than filtering the keys one at a time: a held
    // target with nobody reading the keys is a cursor stuck where it was left,
    // so the target is released and the field is left to practice to drive.
    if (learn_active()) {
        if (nav_active()) nav_stop("sound practice");
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        memset(g_scan_down, 0, sizeof g_scan_down);
        g_radar_down[0] = g_radar_down[1] = 0;
        g_walls_down = 0;
        g_glide_digit = 0;
        g_glide_steps = 0;
        g_walls_have = 0;
        return;
    }

    blast_poll();

    if (nav_active()) {
        void* pawn = NULL;
        POINT m;
        if (cursor_object() != g_nav_cursor) {
            nav_stop("the cursor was replaced");
        } else if (cursor_chained_pawn(&pawn) && pawn != g_nav_pawn) {
            nav_stop("the soldier changed");
        } else if (GetCursorPos(&m) &&
                   (labs(m.x - g_nav_mouse.x) > 2 || labs(m.y - g_nav_mouse.y) > 2)) {
            nav_stop("the mouse moved");
        } else if (g_tile_due && GetTickCount64() >= g_tile_due) {
            // A reached tile, described on the frame after its first path
            // (TILE_DESCRIBE_DELAY_MS), outside the pathfinder's own call.
            int tx, ty;
            g_tile_due = 0;
            if (nav_target(&tx, &ty) && tx == g_tile_due_at[0] && ty == g_tile_due_at[1]) {
                char what[TILE_MAX_TEXT] = "";
                Fault f;
                __try {
                    if (!tile_report(tx, ty, navh_ground(), g_tile_due_dash, 0,
                                     what, sizeof what))
                        what[0] = 0;
                }
                __except (fault_note(GetExceptionInformation(), &f)) {
                    fault_log("tile: report", &f, NULL);
                    what[0] = 0;
                }
                height_step(tx, ty, navh_ground());
                // Asked only when it will be said: the crossing is kept as
                // heard, and one worked out for a step nobody hears is lost.
                int late = g_step_late && what[0] &&
                           tx == g_step_late_at[0] && ty == g_step_late_at[1];
                if (g_step_pending || late)
                    where_say(tx, ty, navh_ground(), 0, g_step_where, sizeof g_step_where);
                if (!nav_step_say(what) && late) {
                    g_step_late = 0;
                    char both[sizeof g_step_where + TILE_MAX_TEXT];
                    _snprintf_s(both, sizeof both, _TRUNCATE, "%s%s%s", g_step_where,
                                g_step_where[0] ? " " : "", what);
                    g_step_where[0] = 0;
                    logf_("nav: %d, %d described late -- \"%s\"\n", tx, ty, both);
                    speech_say_now(both);
                }
            }
        } else if (!g_nav_aim && navh_poll(GetTickCount64()) == NAVH_NO_PATH) {
            logf_("nav: %d, %d has no path on its floor %.1f\n",
                  g_nav_path_tile[0], g_nav_path_tile[1], navh_ground());
            nav_say_no_path(g_nav_path_tile[0], g_nav_path_tile[1]);
        } else if (g_step_pending && GetTickCount64() >= g_step_deadline) {
            // How many times the game computed a path while this step was
            // waiting. Zero means the game never tried, which is a different
            // fault from a path that came back late, and the two are not
            // otherwise distinguishable from out here: both look like
            // silence. XGAction_Path.m_bDoPathingTick is what gates it, and
            // Mouse_CheckForPathing clears that whenever the Flash hit test
            // says the mouse was consumed -- so a run of zeroes here should
            // be read next to the "Flash hit test" lines.
            logf_("nav: nothing decided the tile in %d ms (%ld path calls) "
                  "-- saying what is known\n", STEP_FALLBACK_MS, g_path_calls);
            nav_step_say("");
            g_step_late = nav_target(&g_step_late_at[0], &g_step_late_at[1]);
        } else if (!g_nav_parked && g_nav_placed_at < g_nav_key_at &&
                   GetTickCount64() - g_nav_key_at > 300) {
            nav_park_mouse();
        }
    }

    mouse_hold_poll();

    if (!game_has_focus()) {
        // Forget what was held, so a key released while the game was in the
        // background does not read as a fresh press on return.
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        memset(g_scan_down, 0, sizeof g_scan_down);
        g_radar_down[0] = g_radar_down[1] = 0;
        g_walls_down = 0;
        g_glide_digit = 0;
        g_glide_steps = 0;
        // And the walls go quiet: a field playing on over another window is
        // describing a game the player is not looking at.
        walls_quiet();
        return;
    }
    // The ability menu holds the numpad while it is open, as practice does;
    // so does the unit information screen (F1) while it is up.
    // Insert's list is polled on its own thread (review_pump); while it is
    // open the numpad is its, as before.
    // The mission summary: the battle is over.
    if (msum_screen_up()) {
        if (nav_active()) nav_stop("the mission summary");
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        g_glide_digit = 0;
        g_glide_steps = 0;
        return;
    }
    if (info_poll() || abar_menu_poll() || history_is_open()) {
        memset(g_numpad_down, 0, sizeof g_numpad_down);
        g_glide_digit = 0;
        g_glide_steps = 0;
        return;
    }

    ULONGLONG now = GetTickCount64();
    for (int d = 0; d <= 9; d++) {
        int down = (GetAsyncKeyState(VK_NUMPAD0 + d) & 0x8000) != 0;
        if (down && !g_numpad_down[d]) {
            g_numpad_at[d] = now;
            nav_press(d, 0);
        }
        g_numpad_down[d] = down;
    }

    // Which direction is being held. The one pressed most recently wins, so
    // rolling from one key to the next turns the glide instead of arguing
    // with it.
    int held = 0;
    for (int d = 1; d <= 9; d++) {
        int dx, dy;
        if (!g_numpad_down[d] || !nav_step_for_digit(d, &dx, &dy)) continue;
        if (!held || g_numpad_at[d] > g_numpad_at[held]) held = d;
    }

    if (!held) {
        // Let go. The tile it stopped on is the one worth describing, and the
        // only one the glide says anything about.
        if (g_glide_digit && g_glide_steps > 0) {
            int tx, ty;
            logf_("nav: glide of %d step%s ends\n", g_glide_steps,
                  g_glide_steps == 1 ? "" : "s");
            if (nav_active() && nav_target(&tx, &ty)) nav_arrive(tx, ty);
        }
        g_glide_digit = 0;
        g_glide_steps = 0;
    } else if (held != g_glide_digit) {
        int was_gliding = g_glide_digit && g_glide_steps > 0;
        g_glide_digit = held;
        // A fresh hold waits out NAV_HOLD_MS so that a tap is a tap. A turn
        // taken mid-glide does not: the player is already moving and a pause
        // there would read as the key being missed.
        if (!was_gliding) {
            g_glide_steps = 0;
            g_glide_next = g_numpad_at[held] + NAV_HOLD_MS;
        }
    } else if (now >= g_glide_next) {
        nav_press(held, 1);
        if (g_glide_steps < NAV_GLIDE_RAMP) g_glide_steps++;
        g_glide_next = now + glide_interval(g_glide_steps);
    }

    // The radar: numpad + for enemies, numpad - for the squad. Neither key is
    // bound in [XComGame.XComTacticalInput], so, like the digits, they never
    // reach the game and need no swallowing.
    static const int radar_keys[2] = { VK_ADD, VK_SUBTRACT };
    for (int k = 0; k < 2; k++) {
        int down = (GetAsyncKeyState(radar_keys[k]) & 0x8000) != 0;
        if (down && !g_radar_down[k]) {
            Fault f;
            __try { radar(k == 1); }
            __except (fault_note(GetExceptionInformation(), &f)) {
                fault_log("radar", &f, NULL);
            }
        }
        g_radar_down[k] = down;
    }
    // Numpad *: the wall field off and on. Answered in words, because a
    // feature that has just gone quiet cannot announce itself with a sound.
    int walls = (GetAsyncKeyState(VK_MULTIPLY) & 0x8000) != 0;
    if (walls && !g_walls_down) {
        int on = settings_step(SET_FIELD, 1);
        logf_("walls: field %s\n", on ? "on" : "off");
        speech_say_now(on ? "Wall sound on." : "Wall sound off.");
    }
    g_walls_down = walls;

    // F and C: the target a storey up or down. They are the game's own keys
    // for it and reach the game as well; see nav_floor.
    static const int floor_keys[FLOOR_KEYS] = { 'F', 'C' };
    for (int k = 0; k < FLOOR_KEYS; k++) {
        int down = (GetAsyncKeyState(floor_keys[k]) & 0x8000) != 0;
        if (down && !g_floor_down[k]) nav_floor(k == 0 ? 1 : -1);
        g_floor_down[k] = down;
    }
    // M: the mission's objectives, as the HUD lists them. M is bound in no
    // section of DefaultInput.ini or BaseInput.ini, so it never reaches the
    // game.
    static int mission_key_down;
    int mkey = (GetAsyncKeyState('M') & 0x8000) != 0;
    if (mkey && !mission_key_down) {
        char say[MISSION_TEXT];
        mission_list(say, sizeof say);
        if (mission_visible() == 0) {
            logf_("mission: M, the list hidden: \"%s\"\n", say);
            strcpy_s(say, sizeof say, "No objectives on screen.");
        }
        logf_("mission: M -> \"%s\"\n", say);
        speech_cancel_pending();
        speech_say_now(say);
    }
    mission_key_down = mkey;

    // X: the game's weapon switch. Only noted; the change it makes is said
    // when the HUD redraws the equipped weapon (weapon_note).
    int x = (GetAsyncKeyState('X') & 0x8000) != 0;
    if (x && !g_weapon_x_down) weapon_x_pressed();
    g_weapon_x_down = x;

    if (g_floor_check_at && GetTickCount64() >= g_floor_check_at) {
        g_floor_check_at = 0;
        Fault f;
        __try { floor_game_check(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: floor check", &f, NULL);
        }
    }

    // Delete: the selected soldier (soldier.h). Its only binding, Camera
    // Default, is removed with -Bindings in [Engine.PlayerInput], so like
    // Insert it never reaches the game.
    static int soldier_key_down;
    int soldier_key = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
    if (soldier_key && !soldier_key_down) {
        Fault f;
        __try { soldier_readout(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("soldier: readout", &f, NULL);
        }
    }
    soldier_key_down = soldier_key;

    // The scanner: Page Up, Page Down, Home, End. Read here rather than in a
    // poll of its own so it shares the guards this one already applies --
    // practice has taken the keys, the game has the foreground.
    scan_poll();

    // Last, so a step taken this frame is already in the target the field
    // listens from. The hearts listen from the same tile. Both timed for the
    // perf line, which is how a slowdown blamed on the sounds is settled.
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    walls_poll();
    QueryPerformanceCounter(&t1);
    hearts_poll();
    QueryPerformanceCounter(&t2);
    perf_note(t1.QuadPart - t0.QuadPart, t2.QuadPart - t1.QuadPart);
}

static void cursor_watch(void* self)
{
    cursor_seen(self);

    // Every frame, not at the watch's four times a second: a key press lasts
    // a few frames, and a quarter-second poll would drop quick ones.
    if (cursor_resolved()) nav_poll();
    combat_poll();
    {
        Fault f;
        __try { soldier_poll(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("soldier: poll", &f, NULL);
        }
    }
    {
        Fault f;
        __try { mission_poll(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("mission: poll", &f, NULL);
        }
    }
    {
        Fault f;
        __try { sight_poll(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("sight: poll", &f, NULL);
        }
    }
    {
        Fault f;
        __try { info_settle(); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("info: settle", &f, NULL);
        }
    }

    ULONGLONG now = GetTickCount64();
    if (now - g_cursor_at < CURSOR_WATCH_MS) return;
    g_cursor_at = now;

    char why[256];
    int fields = cursor_fields(why, sizeof why);
    if (!fields) {
        // Said once per cursor, not once per frame: g_tried latches inside
        // cursor.c, so a failure reports itself and then stays quiet.
        if (strcmp(why, "already failed") != 0)
            logf_("cursor: %s\n", why);
        return;
    }
    // Success is worth a line too: these are the offsets anything that writes
    // into the cursor will be trusting.
    if (fields == 2) logf_("cursor: %s\n", why);

    // The grid's origin, off XComWorldData. Waiting for GetWorldData to be
    // called is not a failure and is not logged; the tile then reads "?".
    char grid_why[256];
    int grid = cursor_world_fields(grid_why, sizeof grid_why);
    if (grid == 2)
        logf_("grid: %s\n", grid_why);
    else if (!grid && strcmp(grid_why, "already failed") != 0 &&
             strcmp(grid_why, "no world data yet") != 0 &&
             strcmp(grid_why, "UObject::Class not probed yet") != 0)
        logf_("grid: %s\n", grid_why);

    CursorGrid g;
    int have_grid = grid && cursor_grid(&g);
    if (have_grid && memcmp(&g, &g_grid_last, sizeof g) != 0) {
        g_grid_last = g;
        logf_("grid: Min %.1f, %.1f, %.1f  size %d x %d x %d tiles\n",
              g.min_x, g.min_y, g.min_z, g.num_x, g.num_y, g.num_z);
    }

    float x, y, z;
    if (!cursor_position(&x, &y, &z)) return;
    if (x == g_cursor_last[0] && y == g_cursor_last[1] && z == g_cursor_last[2])
        return;
    g_cursor_last[0] = x; g_cursor_last[1] = y; g_cursor_last[2] = z;

    if (!have_grid) {
        logf_("cursor: at %.1f, %.1f, %.1f  tile ? (%s)\n", x, y, z,
              grid ? "grid unreadable" : grid_why);
        return;
    }

    // The native's own arithmetic (see cursor.h): floor((pos - Min) / 96).
    // The fraction is printed so the log can confirm it rather than trust it
    // -- a cursor at rest in the middle of a tile should read +0.50 on both
    // axes, and anything else means Min is not the origin the native uses.
    int tx = cursor_tile_axis(x, g.min_x, CURSOR_TILE);
    int ty = cursor_tile_axis(y, g.min_y, CURSOR_TILE);
    float fx = (x - g.min_x) / CURSOR_TILE - (float)tx;
    float fy = (y - g.min_y) / CURSOR_TILE - (float)ty;
    int off_grid = tx < 0 || ty < 0 || tx >= g.num_x || ty >= g.num_y;
    logf_("cursor: at %.1f, %.1f, %.1f  tile %d, %d (+%.2f, +%.2f)%s\n",
          x, y, z, tx, ty, fx, fy, off_grid ? "  OFF GRID" : "");
}

// Hands over XComWorldData, which owns the grid's origin. The native is static
// and writes the object into Result unconditionally (both builds), so the
// object is read after the original has run. Half the tactical script calls
// this, so the hook does nothing beyond one pointer comparison.
static ExecFn g_orig_worlddata;
static LONG g_worlddata_faulted;
static void __fastcall hook_worlddata(void* self, void* edx,
                                      void* stack, void* result)
{
    g_orig_worlddata(self, edx, stack, result);
    __try { cursor_world_seen(*(void**)result); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_worlddata_faulted, 1))
            logf_("grid: reading GetWorldData's result faulted (0x%08lx)\n",
                  GetExceptionCode());
    }
}

static ExecFn g_orig_cursormode;
static void __fastcall hook_cursormode(void* self, void* edx,
                                       void* stack, void* result)
{
    Fault f;
    __try { cursor_watch(self); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("cursor: watch", &f, NULL);
    }
    g_orig_cursormode(self, edx, stack, result);
}

// Where navigation takes over the cursor.
//
// Every placement of the cursor ends in GetClosestValidCursorPosition, called
// with the position the script wants: from getValidLocation(Vector NewLoc) in
// EW, straight from CursorSetLocation(Vector NewLoc, ...) in EU. Either way the
// position is the *caller's first parameter*, and the native reads it out of
// the caller's frame when it runs -- so writing the target there just before
// the original is called is the whole substitution. What comes back is the
// game's own answer: the nearest position a cursor may occupy.
//
// Only the placement that follows the mouse is taken over. The script frames
// above are walked for Mouse_CheckForPathing, which is declared once per build,
// on XComTacticalInput, and exists only while the soldier is choosing where to
// move. The other callers -- MoveToUnit when the soldier changes, the aiming
// camera -- are left alone.
#define NAV_CHAIN_DEPTH 6

static int g_nav_chain_logged;

static int nav_substitute(void* stack)
{
    char chain[512] = "";
    size_t used = 0;
    int from_mouse = 0;
    void* frame = stack;

    for (int depth = 0; frame && depth < NAV_CHAIN_DEPTH; depth++) {
        if (!readable(frame, FFRAME_PREVIOUS + sizeof(void*))) break;
        void* node = *(void**)((uint8_t*)frame + FFRAME_NODE);
        char name[128];
        if (!object_name(node, name, sizeof name) || !name[0]) break;
        used += (size_t)_snprintf_s(chain + used, sizeof chain - used, _TRUNCATE,
                                    depth ? " <- %s" : "%s", name);
        if (used >= sizeof chain) used = sizeof chain - 1;
        if (strcmp(name, "Mouse_CheckForPathing") == 0) { from_mouse = 1; break; }
        if (strcmp(name, "Mouse_CheckForFreeAim") == 0) {
            // The aim. Its tile was put in at ProcessChainedDistance, ahead
            // of the range leash (nav_aim_substitute); writing it again here,
            // after the leash, would undo the game's clamp. Reported as a
            // placement so what the game made of it is heard.
            static int aim_logged;
            if (!g_nav_aim) return 0;
            if (!aim_logged) {
                aim_logged = 1;
                logf_("nav: aim placement call chain %s\n", chain);
            }
            return 1;
        }
        frame = *(void**)((uint8_t*)frame + FFRAME_PREVIOUS);
    }

    // The chain is the evidence that FFRAME_PREVIOUS is right, so it is put on
    // record the first time navigation meets this native.
    if (!g_nav_chain_logged && from_mouse) {
        g_nav_chain_logged = 1;
        logf_("nav: placement call chain %s\n", chain);
    }
    if (!from_mouse) {
        // And if the mouse's placement is never recognised, the chains that
        // were seen instead are what says why -- a few, not one per frame.
        static int misses;
        if (!g_nav_chain_logged && misses < 3) {
            misses++;
            logf_("nav: not a mouse placement: %s\n", chain);
        }
        return 0;
    }

    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        return 0;

    // The first parameter, by position: NewLoc in both builds.
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            float* v = (float*)(locals + off);
            if (off >= 0x1000 || !writable(v, 3 * sizeof(float))) return 0;
            // A vector the mouse picked is a world position. Anything else in
            // this slot means the frame is not the one described above.
            for (int i = 0; i < 3; i++)
                if (!(v[i] > -1.0e6f && v[i] < 1.0e6f)) return 0;
            // X and Y only. The height is the ground under the target, which
            // hook_floorz has already had the game work out for this frame.
            // Writing the cursor's own height here instead raised it by its
            // collision height on every step -- getValidLocation adds that to
            // whatever comes back -- until it hung hundreds of units in the
            // air and ClickToPath read the move as a hover it could not make.
            v[0] = g_nav_world[0];
            v[1] = g_nav_world[1];
            return 1;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return 0;
}

// Where an aim step landed. The game's range leash may have pulled it short of
// the tile asked for; then the aim IS where it landed, and navigation follows
// it there, so the next step goes on from where the aim really is and the
// coordinates spoken are the ones a shot would go to.
static void nav_aim_landed(const CursorGrid* g, int px, int py)
{
    int tx, ty;
    if (!nav_target(&tx, &ty)) return;
    // Who the blast takes in, once the game has drawn it here (blast_poll).
    g_blast_due = GetTickCount64() + BLAST_DELAY_MS;
    // What the aim hits from here is heard after the coordinates, even when
    // it is what the last tile had: the odds (brief, see shot_set_brief) and
    // whether the shot is blocked. The game resends the blocked message only
    // when it changes, so that one is said only if it does.
    shot_forget_said();
    g_reticle_said[0] = 0;
    if (px == tx && py == ty) {
        nav_step_say("");
        return;
    }
    logf_("nav: aim for %d, %d held at %d, %d -- out of range\n", tx, ty, px, py);
    nav_begin(px, py);
    g_nav_world[0] = g->min_x + ((float)px + 0.5f) * CURSOR_TILE;
    g_nav_world[1] = g->min_y + ((float)py + 0.5f) * CURSOR_TILE;
    g_aim_floor = aim_floor(g, px, py, g_aim_floor);
    g_nav_world[2] = g_aim_floor;
    nav_describe(px, py, g_step_coords, sizeof g_step_coords);
    // Who was found on arrival stands on the tile asked for, not this one.
    g_step_nunits = 0;
    nav_step_say("Out of range.");
}

// What the game made of the target. Logged when the tile changes, so the log
// shows each step landing -- or being moved somewhere else by the validation.
static void nav_placed(const float* v)
{
    g_nav_placed_at = GetTickCount64();
    CursorGrid g;
    int tx, ty;
    if (!cursor_grid(&g) || !readable(v, 3 * sizeof(float))) return;
    int px = cursor_tile_axis(v[0], g.min_x, CURSOR_TILE);
    int py = cursor_tile_axis(v[1], g.min_y, CURSOR_TILE);
    // An aim is announced as it lands, and before the same-tile check below:
    // a step pushed against the range limit lands where the last one did.
    if (g_nav_aim && g_step_pending) nav_aim_landed(&g, px, py);
    if (px == g_nav_tile_logged[0] && py == g_nav_tile_logged[1]) return;
    g_nav_tile_logged[0] = px;
    g_nav_tile_logged[1] = py;
    nav_target(&tx, &ty);
    logf_("nav: placed on %d, %d (%.1f, %.1f, %.1f)%s\n", px, py, v[0], v[1], v[2],
          (px == tx && py == ty) ? "" : "  -- NOT the target");
}

// The ground under the target.
//
// GetAdjustedMousePickPoint -- declared once per build, on XComTacticalInput --
// opens with
//
//     fGroundLocation = kWorldData.GetFloorZForPosition(kHUD.CachedHitLocation);
//
// and then takes the tile and the cursor's height from that same
// CachedHitLocation. The HUD fills it from a mouse trace every frame. Writing
// the target into it just before this native reads it makes the whole pick --
// ground height, tile, snap -- come out for the target instead of the mouse,
// for this frame only; the next trace overwrites it again.
//
// The HUD is found by shape: the local in that frame whose object has a
// property called CachedHitLocation. Found once, then recognised by pointer.
static void*    g_pick_node;
static uint32_t g_pick_hud_local;       // offset of the HUD local in the frame
static void*    g_pick_hud;
static uint32_t g_pick_hit_off;         // CachedHitLocation, on the HUD
static int      g_pick_logged;

// ---- the pick the mouse has to agree to -----------------------------------
//
// For a soldier who is not flying, GetAdjustedMousePickPoint ends
//
//     if(kHUD.CachedMouseInteractionInterface != none) { ... return true; }
//     return false;
//
// and Mouse_CheckForPathing places the cursor only when it returns true. That
// interface is whatever the *mouse's own* trace hit this frame -- an actor
// inside the level volume, standing on a floor, from
// XComTacticalHUD.GetMousePickActor. So when the mouse rests where its ray
// hits nothing worth picking -- over the HUD, off the map, or on the sky after
// the camera panned out from under it -- the pick is refused, and with it the
// whole chain navigation rides on: no CursorSetLocation, no floor snap, no
// GetClosestValidCursorPosition, no path. The target moves and the game never
// hears of it, so every step falls through to STEP_FALLBACK_MS and comes out
// as bare coordinates a second and a half late. In the log of 2026-09-20 that
// started on the first tile after a release and never recovered: thirteen
// steps in a row, each one a wait.
//
// The refusal turns on a comparison against none and nothing else -- nothing
// calls the interface between that test and the placement -- so the last actor
// the mouse really did hit is lent back for exactly that stretch and taken out
// again before anything can read it. Nothing is invented: until the mouse has
// picked something at least once this navigation, the frame is left as it was.
static uint32_t g_pick_iface_off;       // CachedMouseInteractionInterface
static void*    g_pick_iface_seen[2];   // the last one the mouse itself hit
static void*    g_pick_iface_cursor;    // the cursor it was seen under
static void**   g_pick_iface_lent;      // where it was lent, to take back
static int      g_pick_iface_state = -1;

// An UnrealScript interface is two pointers: the object, then its interface
// table. Both are put back, and only if they are still the ones lent.
static void nav_return_interface(void)
{
    void** slot = g_pick_iface_lent;
    if (!slot) return;
    g_pick_iface_lent = NULL;
    if (!writable(slot, 2 * sizeof(void*))) return;
    if (slot[0] != g_pick_iface_seen[0] || slot[1] != g_pick_iface_seen[1]) return;
    slot[0] = NULL;
    slot[1] = NULL;
}

static void nav_return_interface_guarded(void)
{
    Fault f;
    __try { nav_return_interface(); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: interface", &f, NULL);
    }
}

static void nav_lend_interface(void* hud)
{
    nav_return_interface();
    if (!g_pick_iface_off) return;
    void** slot = (void**)((uint8_t*)hud + g_pick_iface_off);
    if (!writable(slot, 2 * sizeof(void*))) return;

    if (slot[0]) {                      // the mouse picked something itself
        // Once: whether a real pick holds (object, object), which is what
        // lending the soldier to an aim assumes (nav_aim_lend).
        static int pair_logged;
        if (!pair_logged) {
            pair_logged = 1;
            logf_("nav: a real pick holds %p, %p (%s)\n", slot[0], slot[1],
                  slot[0] == slot[1] ? "the same object twice" : "two different pointers");
        }
        g_pick_iface_seen[0] = slot[0];
        g_pick_iface_seen[1] = slot[1];
        g_pick_iface_cursor = cursor_object();
        // Only worth a line as the answer to one that said it had stopped.
        if (g_pick_iface_state == 0)
            logf_("nav: the mouse picks the map again\n");
        g_pick_iface_state = 1;
        return;
    }

    // Lending a destroyed actor would be read once, by the next frame's
    // mouse-out, so the remembered one is checked for still being an object
    // before it goes back in -- and for having been seen on this map, since a
    // new mission spawns a new cursor and everything the old one pointed at is
    // gone.
    char name[128];
    int alive = g_pick_iface_seen[0] &&
                g_pick_iface_cursor == cursor_object() &&
                readable(g_pick_iface_seen[0], 0x60) &&
                object_name(g_pick_iface_seen[0], name, sizeof name) && name[0];
    const char* what = "lending back the last actor it hit";
    if (!alive) {
        // Nothing remembered: a fresh map, and with the mouse blocked
        // (mouse.h) the player can no longer move it by hand to give the
        // game something to pick, which was the only way out -- the run of
        // 2026-09-25 after the block went in: every step "nothing decided
        // the tile in 1500 ms". The soldier is lent instead, as nav_aim_lend
        // does for an aim: past the test against none,
        // GetAdjustedMousePickPoint reads only CachedHitLocation, never the
        // interface. Kept as seen, as there, so it is what the next miss
        // lends back; a real pick replaces it.
        void* pawn = NULL;
        if (cursor_chained_pawn(&pawn) && pawn && unit_is_live(pawn)) {
            g_pick_iface_seen[0] = pawn;
            g_pick_iface_seen[1] = pawn;
            g_pick_iface_cursor = cursor_object();
            alive = 1;
            what = "none remembered, lending the soldier";
        } else {
            g_pick_iface_seen[0] = NULL;
            g_pick_iface_seen[1] = NULL;
            what = "none to lend, the game will refuse the placement";
        }
    }
    if (alive) {
        slot[0] = g_pick_iface_seen[0];
        slot[1] = g_pick_iface_seen[1];
        g_pick_iface_lent = slot;
    }
    if (g_pick_iface_state != 0) {
        g_pick_iface_state = 0;
        logf_("nav: the mouse picks nothing -- %s\n", what);
    }
}

// Navigation is over. What was lent goes back, but what was *seen* is kept:
// the mouse having picked the map once is the only thing that lets a later
// navigation start placing straight away, and dropping it per navigation left
// "none to lend, the game will refuse the placement" -- thirteen steps of bare
// coordinates, 1.5 s apart, in the run of 2026-09-20. It is dropped when the
// cursor is replaced instead, which is where it actually stops being valid;
// nav_lend_interface makes that check.
static void nav_forget_interface(void)
{
    nav_return_interface_guarded();
    g_pick_iface_state = -1;
}

static int nav_aim_pick(void* stack)
{
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals) return 0;

    if (node != g_pick_node) {
        char name[128];
        if (!object_name(node, name, sizeof name) ||
            strcmp(name, "GetAdjustedMousePickPoint") != 0)
            return 0;

        // Walk the frame's properties for a local holding a HUD.
        void* prop = NULL;
        if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
            prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
        for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
            if (!readable(prop, 0x68)) return 0;
            uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
            uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
            if (!(flags & CPF_PARM) && off < 0x1000 &&
                readable(locals + off, sizeof(void*))) {
                void* obj = *(void**)(locals + off);
                uint32_t hit;
                if (obj && readable(obj, 0x60) &&
                    object_field_offset(obj, "CachedHitLocation", &hit)) {
                    g_pick_node = node;
                    g_pick_hud_local = off;
                    g_pick_hud = obj;
                    g_pick_hit_off = hit;
                    if (!object_field_offset(obj, "CachedMouseInteractionInterface",
                                             &g_pick_iface_off))
                        g_pick_iface_off = 0;
                    break;
                }
            }
            prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        }
        if (node != g_pick_node) {
            if (!g_pick_logged) {
                g_pick_logged = 1;
                logf_("nav: GetAdjustedMousePickPoint has no HUD local -- "
                      "the ground height will come from the mouse\n");
            }
            return 0;
        }
        logf_("nav: pick HUD local +0x%X, CachedHitLocation +0x%X, "
              "CachedMouseInteractionInterface +0x%X\n",
              g_pick_hud_local, g_pick_hit_off, g_pick_iface_off);
    }

    if (!readable(locals + g_pick_hud_local, sizeof(void*))) return 0;
    void* hud = *(void**)(locals + g_pick_hud_local);
    if (hud != g_pick_hud) {
        uint32_t hit;
        if (!hud || !object_field_offset(hud, "CachedHitLocation", &hit)) return 0;
        g_pick_hud = hud;
        g_pick_hit_off = hit;
        if (!object_field_offset(hud, "CachedMouseInteractionInterface",
                                 &g_pick_iface_off))
            g_pick_iface_off = 0;
    }
    float* v = (float*)((uint8_t*)hud + g_pick_hit_off);
    if (!writable(v, 3 * sizeof(float))) return 0;
    v[0] = g_nav_world[0];
    v[1] = g_nav_world[1];
    v[2] = g_nav_aim ? g_nav_world[2] : navh_query_z();
    g_nav_world[2] = v[2];
    // Last, because it decides whether the game will take any of the above.
    nav_lend_interface(hud);
    return 1;
}

// Says in the log when a tile's height changes phase, once per change.
static void nav_log_phase(void)
{
    NavHeightPhase p = navh_phase();
    if (p == g_nav_phase_logged) return;
    g_nav_phase_logged = p;
    if (p == 2) surface_note(g_nav_path_tile[0], g_nav_path_tile[1], navh_ground());
    static const char* names[] = { "searching", "probing heights", "settled", "no height works" };
    logf_("nav: %d, %d height %s, ground %.1f\n", g_nav_path_tile[0], g_nav_path_tile[1],
          names[p], navh_ground());
}

// The frames above a native, named: "IsAttemptingToHover <- ClickToPath <-
// RMouse". What the confirm diagnostics print.
static void frame_chain(void* stack, int depth, char* out, size_t out_sz)
{
    size_t used = 0;
    out[0] = 0;
    void* frame = stack;
    for (int d = 0; frame && d < depth; d++) {
        if (!readable(frame, FFRAME_PREVIOUS + sizeof(void*))) break;
        char name[128];
        if (!object_name(*(void**)((uint8_t*)frame + FFRAME_NODE), name, sizeof name) ||
            !name[0])
            break;
        used += (size_t)_snprintf_s(out + used, out_sz - used, _TRUNCATE,
                                    d ? " <- %s" : "%s", name);
        if (used >= out_sz) break;
        frame = *(void**)((uint8_t*)frame + FFRAME_PREVIOUS);
    }
}

// Whether a confirm is recent enough for its consequences to be logged.
static int nav_confirm_window(void)
{
    return g_nav_confirm_at && GetTickCount64() - g_nav_confirm_at <= 2000;
}

static ExecFn g_orig_floorz;
static void __fastcall hook_floorz(void* self, void* edx, void* stack, void* result)
{
    Fault f;
    int aimed = 0;
    if (g_nav_live) {
        __try { aimed = nav_aim_pick(stack); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: aim pick", &f, NULL);
        }
    }
    g_orig_floorz(self, edx, stack, result);

    __try {
        float z = *(float*)result;
        // A floor F / C chose is held against the game's own answer for the
        // pick. The game does not report every surface it lets a soldier
        // stand on: on 2026-09-27 C chose 96.0 at 31, 48, this pick answered
        // 220.3, the height search settled on that, and the target went back
        // up without a word -- C was silent and the next C started from 220.3
        // again. Only the pick made for the target (aimed), only while held.
        if (aimed && g_floor_hold && fabsf(z - g_floor_hold_z) > FLOOR_HOLD_SLACK &&
            writable(result, sizeof(float))) {
            static int logged;
            if (!logged) {
                logged = 1;
                logf_("nav: the pick's floor %.1f replaced by the held floor %.1f\n", z,
                      g_floor_hold_z);
            }
            z = g_floor_hold_z;
            *(float*)result = z;
        }
        if (aimed && !g_nav_aim) {
            navh_floor_result(g_nav_world[2], z);
            nav_log_phase();
        }

        // ClickToPath calls IsAttemptingToHover only once it has a path, and
        // that is the only reason this native is called from there: seeing
        // it after a confirm proves the click got through to the move.
        if (nav_confirm_window()) {
            char chain[256];
            frame_chain(stack, 4, chain, sizeof chain);
            if (strncmp(chain, "IsAttemptingToHover", 19) == 0)
                logf_("nav: after confirm, %s -- floor %.1f\n", chain, z);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: floor result", &f, NULL);
    }
}

// Whether the game could build a path to where navigation put the cursor.
// XGAction_Path.Perform_ComputePath(Vector vLoc, ...) asks the pathing pawn's
// native ComputePath2(vLoc, ...) with the cursor's location, and ClickToPath
// moves only along what that produced -- an empty path makes a confirm do
// nothing, silently. The destination is the caller's first parameter, as for
// the placement. Logged when the answer or the tile changes, while navigating.
static int   g_path_logged_ok = -1;
static int   g_path_logged_tile[2] = { -1, -1 };
static float g_path_logged_z;

static void nav_path_result(void* self, void* stack, void* result)
{
    if (!nav_active() || !readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) return;

    float* dest = NULL;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            if (off < 0x1000 && readable(locals + off, 3 * sizeof(float)))
                dest = (float*)(locals + off);
            break;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }

    int ok = *(int32_t*)result != 0;
    CursorGrid g;
    int tx = -1, ty = -1;
    if (dest && cursor_grid(&g)) {
        tx = cursor_tile_axis(dest[0], g.min_x, CURSOR_TILE);
        ty = cursor_tile_axis(dest[1], g.min_y, CURSOR_TILE);
    }
    // After F / C, a path whose end is not on the chosen floor is the one
    // from before the key: the same tile, the old storey.
    int stale = dest && g_floor_hold &&
                fabsf(dest[2] - NAV_CURSOR_LIFT - g_floor_hold_z) > FLOOR_HOLD_SLACK;
    if (dest && !stale && tx == g_nav_path_tile[0] && ty == g_nav_path_tile[1]) {
        g_path_pawn = self;
        NavVerdict v = navh_path_result(dest[2], ok, GetTickCount64());
        nav_log_phase();
        if (v == NAVH_NO_PATH) {
            logf_("nav: %d, %d has no path at any height\n", tx, ty);
            nav_say_no_path(tx, ty);
        } else if (v == NAVH_REACHABLE) {
            logf_("nav: %d, %d reachable, floor %.1f\n", tx, ty, navh_ground());
            g_tile_due = GetTickCount64() + TILE_DESCRIBE_DELAY_MS;
            g_tile_due_at[0] = tx;
            g_tile_due_at[1] = ty;
            g_tile_due_dash = 1;
        }
    }

    // Only the tile being navigated is logged. Every other caller -- the
    // aliens' turn runs ComputePathForAIUnit for each move it considers --
    // filled the log once navigation had been left on.
    if (!dest || tx != g_nav_path_tile[0] || ty != g_nav_path_tile[1]) return;

    // Logged when the answer, the tile or the height changes: probing moves
    // the height with the tile unchanged, and each try is worth a line.
    float dz = dest[2];
    if (ok == g_path_logged_ok && tx == g_path_logged_tile[0] &&
        ty == g_path_logged_tile[1] && dz == g_path_logged_z)
        return;
    g_path_logged_ok = ok;
    g_path_logged_tile[0] = tx;
    g_path_logged_tile[1] = ty;
    g_path_logged_z = dz;

    char name[128] = "?";
    object_name(node, name, sizeof name);
    if (dest)
        logf_("nav: path to %d, %d (%.1f, %.1f, %.1f) %s  [from %s]\n", tx, ty,
              dest[0], dest[1], dest[2], ok ? "built" : "NONE", name);
    else
        logf_("nav: path %s  [from %s, destination unreadable]\n",
              ok ? "built" : "NONE", name);
}

static ExecFn g_orig_computepath;
static void __fastcall hook_computepath(void* self, void* edx, void* stack, void* result)
{
    InterlockedIncrement(&g_path_calls);
    g_orig_computepath(self, edx, stack, result);
    Fault f;
    __try { nav_path_result(self, stack, result); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: path result", &f, NULL);
    }
}

// Whether Flash took a click. InputEvent asks this, through
// TestMouseConsumedByFlash, before a mouse button may reach RMouse.
//
// It is asked several times a frame, so the answer is written when it
// *changes* and once per confirm, rather than every time. One mission left
// 1,370 identical "miss" lines, and each had paid for a frame_chain first --
// five UnrealScript frames walked, a name decoded for each -- to repeat what
// the first line had already said. The chain is now built only for a line
// that is going to be written.
//
// The change is logged whether or not a confirm is recent, because this
// answer is not only about clicks: Mouse_CheckForPathing reads it every frame
// and clears XGAction_Path.m_bDoPathingTick the moment it comes back true,
// which stops the game pathing until a later frame says false again. Cutting
// this line back to the confirm window took away the only evidence of that,
// and a run where nothing was ever pathed could not be told from a run where
// the paths were merely late. A flip is a handful of lines a mission; the
// silence in between is the useful part.
static ULONGLONG g_flash_said_for;
static int       g_flash_last = -1;

static ExecFn g_orig_flashhit;
static void __fastcall hook_flashhit(void* self, void* edx, void* stack, void* result)
{
    g_orig_flashhit(self, edx, stack, result);
    Fault f;
    __try {
        int hit = *(int32_t*)result != 0;
        int changed = hit != g_flash_last;
        int confirmed = nav_confirm_window() && g_flash_said_for != g_nav_confirm_at;
        if (changed || confirmed) {
            g_flash_last = hit;
            if (confirmed) g_flash_said_for = g_nav_confirm_at;
            char chain[256];
            frame_chain(stack, 5, chain, sizeof chain);
            logf_("nav: Flash hit test %s%s: %s\n",
                  hit ? "HIT -- the mouse is on the HUD, pathing stops" : "miss",
                  confirmed ? ", after confirm" : "", chain);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: flash hit", &f, NULL);
    }
}

// What the game did with a confirm. InputEvent calls the native
// XComEngine.IsAnyMoviePlaying on every bound key and mouse button before it
// acts on it, in both builds, so its caller's frame carries (Cmd, Actionmask)
// -- taken by position, as rewrite_cmd does. Logged only for two seconds after
// Numpad 0, so the log shows whether the right click arrived as 405 and in
// what order, without a line per key for the rest of the mission.
// Reads the Cmd off an InputEvent frame, or -1 when this is not one. The
// native XComEngine.IsAnyMoviePlaying is called from InputEvent on every bound
// key, in both builds, so its caller's frame carries (Cmd, Actionmask) -- by
// position, as rewrite_cmd takes them.
static int input_event_cmd(void* stack, int* mask_out)
{
    if (!readable(stack, 0x20)) return -1;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return -1;

    int vals[2], nvals = 0;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS && nvals < 2; guard++) {
        if (!readable(prop, 0x68)) return -1;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000 &&
            readable(locals + off, sizeof(int32_t)))
            vals[nvals++] = *(int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    if (nvals < 1) return -1;
    if (mask_out) *mask_out = nvals == 2 ? vals[1] : 0;
    return vals[0];
}

// Where InputEvent's Cmd lives in its frame -- the first int parameter, as
// input_event_cmd reads it -- so it can be rewritten before InputEvent goes
// on to act on it.
static int32_t* input_event_cmd_slot(void* stack)
{
    if (!readable(stack, 0x20)) return NULL;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return NULL;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return NULL;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000)
            return (int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return NULL;
}

// End is the scanner's "how far, and which way" key, and it is also the
// secondary binding for Backspace_Key_Press -- which is PerformEndTurn. So it
// has to be taken away from the game, and InputEvent offers exactly one place
// to do that:
//
//     if(Class'XComGame.XComEngine'.static.IsAnyMoviePlaying())
//     {
//         return;
//     }
//
// which sits above PreProcessCheckGameLogic and everything that acts on a key.
// Forcing that native true for one call makes InputEvent return, and the turn
// does not end.
//
// Only for End. Cmd 512 is raised by Backspace as well -- that is its primary
// binding -- so the key itself decides: End down and Backspace up, or the
// press is left alone. A player who wants to end the turn with Backspace still
// can, and one who wants to with End can hold Backspace... which is why the
// help says End is the scanner's now.
#define INPUT_CMD_BACKSPACE 512

static int input_is_our_end(int cmd)
{
    if (cmd != INPUT_CMD_BACKSPACE) return 0;
    if (!(GetAsyncKeyState(VK_END) & 0x8000)) return 0;
    if (GetAsyncKeyState(VK_BACK) & 0x8000) return 0;
    return 1;
}

static int g_end_swallowed;

static void nav_watch_input(void* stack)
{
    if (!nav_confirm_window()) return;
    if (!readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    char name[128];
    if (!locals || !object_name(node, name, sizeof name) ||
        strcmp(name, "InputEvent") != 0)
        return;

    int vals[2], nvals = 0;
    void* prop = NULL;
    if (readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS && nvals < 2; guard++) {
        if (!readable(prop, 0x68)) return;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM) &&
            props_kind(prop) == PROP_INT && off < 0x1000 &&
            readable(locals + off, sizeof(int32_t)))
            vals[nvals++] = *(int32_t*)(locals + off);
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    if (nvals == 2)
        logf_("nav: after confirm, InputEvent %d mask %d\n", vals[0], vals[1]);
}

static ExecFn g_orig_moviecheck;
static void __fastcall hook_moviecheck(void* self, void* edx, void* stack, void* result)
{
    Fault f;
    int swallow = 0;
    __try {
        nav_watch_input(stack);
        int mask = 0;
        int cmd = input_event_cmd(stack, &mask);
        swallow = input_is_our_end(cmd);
        if (!swallow && abar_menu_swallow(cmd, mask))
            swallow = 2;
        if (swallow == 1 && !g_end_swallowed) {
            g_end_swallowed = 1;
            logf_("scan: End taken from the game (cmd %d, mask %d) -- "
                  "the turn does not end\n", INPUT_CMD_BACKSPACE, mask);
        }
    }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("nav: watch input", &f, NULL);
    }
    g_orig_moviecheck(self, edx, stack, result);

    // After the original, because what is being replaced is its answer.
    if (swallow) {
        __try {
            if (writable(result, sizeof(int32_t))) *(int32_t*)result = 1;
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("scan: swallow End", &f, NULL);
        }
    }
}

// Engine.GetEngine: the first native InputEvent calls, which is the one point
// early enough to rewrite Enter into an ability's number key (see
// abar_menu_pick). Called from all over the script, so it does nothing unless
// a pick is under way.
static ExecFn g_orig_getengine;
static void __fastcall hook_getengine(void* self, void* edx, void* stack, void* result)
{
    if (abar_pick_pending()) {
        Fault f;
        __try { abar_menu_pick(stack); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("abar: pick", &f, NULL);
        }
    }
    g_orig_getengine(self, edx, stack, result);
}

// The aim's tile, put in ahead of the range leash. CursorSetLocation opens
//
//     NewLoc = ProcessChainedDistance(NewLoc);
//
// in both builds, and the native reads NewLoc out of CursorSetLocation's frame
// when it runs -- so the target written there is what the leash clamps, and
// what comes back is where the game will let the aim go. Only while aiming,
// and only for the placement Mouse_CheckForFreeAim makes.
static int nav_aim_substitute(void* stack)
{
    // The frames above are CursorSetLocation again, then Mouse_CheckForFreeAim:
    // the live cursor is XCom3DCursorMouseForCursorVolumes, whose
    // CursorSetLocation calls its parent's, and it is the parent's that calls
    // the leash. The log of 2026-09-22 printed "CursorSetLocation <-
    // CursorSetLocation <- Mouse_CheckForFreeAim"; looking only one frame up
    // found the first and never substituted.
    if (!readable(stack, FFRAME_PREVIOUS + sizeof(void*))) return 0;
    void* above = *(void**)((uint8_t*)stack + FFRAME_PREVIOUS);
    char name[128];
    int from_aim = 0;
    for (int depth = 0; above && depth < 3; depth++) {
        if (!readable(above, FFRAME_PREVIOUS + sizeof(void*)) ||
            !object_name(*(void**)((uint8_t*)above + FFRAME_NODE), name, sizeof name))
            return 0;
        if (strcmp(name, "Mouse_CheckForFreeAim") == 0) { from_aim = 1; break; }
        if (strcmp(name, "CursorSetLocation") != 0) return 0;
        above = *(void**)((uint8_t*)above + FFRAME_PREVIOUS);
    }
    if (!from_aim) return 0;

    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        return 0;
    void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
    for (int guard = 0; prop && guard < MAX_FIELDS; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(uint32_t*)((uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            float* v = (float*)(locals + off);
            if (off >= 0x1000 || !writable(v, 3 * sizeof(float))) return 0;
            for (int i = 0; i < 3; i++)
                if (!(v[i] > -1.0e6f && v[i] < 1.0e6f)) return 0;
            v[0] = g_nav_world[0];
            v[1] = g_nav_world[1];
            v[2] = g_nav_world[2];      // the tile's floor (aim_floor)
            return 1;
        }
        prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
    }
    return 0;
}

// The pick an aim has to be allowed. Mouse_CheckForFreeAim gives up unless
//
//     MouseTarget = GetMouseInterfaceTarget();   // HUD.CachedMouseInteractionInterface
//     if(MouseTarget == none) return;
//
// and it asks that BEFORE GetAdjustedMousePickPoint, where nav_lend_interface
// works for a move -- so with the mouse over nothing, every aim step went
// nowhere (the first aim of 2026-09-22: five steps, no placement). The lend is
// made one call earlier for an aim: ActiveUnit_Firing_WithMoveCharacteristics.
// PostProcessCheckGameLogic calls the native Engine.GetCurrentWorldInfo (for
// IsPaused) just before Mouse_CheckForFreeAim, and nothing between reads the
// interface. hook_validpos takes it back, as it does for a move.
//
// The HUD is reached by name from the cursor: Pawn.Controller, then
// Controller.myHUD. That works before any move has shown the mod the HUD.
static void*     g_aim_frame_node;      // that PostProcessCheckGameLogic, once seen
static void*     g_aim_frame_miss[16];  // other callers, so their names are not re-read
static int       g_aim_frame_nmiss;
static FieldSlot g_cursor_controller, g_controller_hud;

static void* aim_hud(void)
{
    void* cursor = cursor_object();
    const void* v;
    if (!cursor || !field_ptr(cursor, "Controller", &g_cursor_controller, sizeof(void*), &v))
        return NULL;
    void* controller = *(void* const*)v;
    if (!controller || !unit_is_live(controller) ||
        !field_ptr(controller, "myHUD", &g_controller_hud, sizeof(void*), &v))
        return NULL;
    void* hud = *(void* const*)v;
    return hud && unit_is_live(hud) ? hud : NULL;
}

static void nav_aim_lend(void* stack)
{
    if (!readable(stack, 0x20)) return;
    void* node = *(void**)((uint8_t*)stack + FFRAME_NODE);
    if (node != g_aim_frame_node) {
        for (int i = 0; i < g_aim_frame_nmiss; i++)
            if (g_aim_frame_miss[i] == node) return;
        char name[128];
        if (!object_name(node, name, sizeof name) ||
            strcmp(name, "PostProcessCheckGameLogic") != 0) {
            if (g_aim_frame_nmiss < (int)(sizeof g_aim_frame_miss / sizeof g_aim_frame_miss[0]))
                g_aim_frame_miss[g_aim_frame_nmiss++] = node;
            return;
        }
        // Only aiming's own: the moving state has one of the same name, but
        // this is called only while an aim is being navigated.
        g_aim_frame_node = node;
    }
    void* hud = aim_hud();
    if (!hud) return;
    if (!g_pick_iface_off &&
        !object_field_offset(hud, "CachedMouseInteractionInterface", &g_pick_iface_off))
        return;
    nav_return_interface();
    void** slot = (void**)((uint8_t*)hud + g_pick_iface_off);
    if (!writable(slot, 2 * sizeof(void*))) return;
    if (slot[0]) return;                // the mouse picks something itself

    // Nothing under the mouse, and with a fresh map nothing remembered either
    // (the run of 2026-09-22: "none to lend", parking the mouse did not help,
    // the player had to move it by hand). So the soldier is lent: over a unit
    // pawn, Mouse_CheckForFreeAim does CursorSetLocation(unit.GetLocation()),
    // and nav_aim_substitute puts the aim's tile in place of that location at
    // the leash. IMouseInteractionInterface is a script interface (no native
    // keyword, no VfTable property), so the game reaches it through the object
    // and the pair is (object, object); nav_lend_interface logs what a real
    // pick holds, to confirm it.
    void* pawn = NULL;
    if (!cursor_chained_pawn(&pawn) || !pawn || !unit_is_live(pawn)) return;
    slot[0] = pawn;
    slot[1] = pawn;
    g_pick_iface_seen[0] = pawn;
    g_pick_iface_seen[1] = pawn;
    g_pick_iface_cursor = cursor_object();
    g_pick_iface_lent = slot;
    if (g_pick_iface_state != 2) {
        g_pick_iface_state = 2;
        logf_("nav: the mouse picks nothing -- lending the soldier to the aim\n");
    }
}

static ExecFn g_orig_worldinfo;
static void __fastcall hook_worldinfo(void* self, void* edx, void* stack, void* result)
{
    if (g_nav_live && g_nav_aim) {
        Fault f;
        __try { nav_aim_lend(stack); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: aim lend", &f, NULL);
        }
    }
    g_orig_worldinfo(self, edx, stack, result);
}

static ExecFn g_orig_chained;
static void __fastcall hook_chained(void* self, void* edx, void* stack, void* result)
{
    if (g_nav_live && g_nav_aim) {
        Fault f;
        static int logged;
        __try {
            if (nav_aim_substitute(stack) && !logged) {
                logged = 1;
                logf_("nav: aim tile put in at the range leash\n");
            }
        }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: aim substitute", &f, NULL);
        }
    }
    g_orig_chained(self, edx, stack, result);
}

static ExecFn g_orig_validpos;
static void __fastcall hook_validpos(void* self, void* edx,
                                     void* stack, void* result)
{
    int placed = 0;
    Fault f;
    // Reaching here means the pick was accepted, so whatever was lent to make
    // it accepted has done its work and comes straight back out -- before the
    // native runs, and long before anything else on this frame reads it.
    nav_return_interface_guarded();
    if (g_nav_live) {
        __try { placed = nav_substitute(stack); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: substitute", &f, NULL);
        }
    }
    g_orig_validpos(self, edx, stack, result);
    if (placed) {
        __try { nav_placed((const float*)result); }
        __except (fault_note(GetExceptionInformation(), &f)) {
            fault_log("nav: placed", &f, NULL);
        }
    }
}

// This one rewrites rather than captures, so it gets its own thunk: the work
// has to happen before the native reads its arguments, not alongside it.
static ExecFn g_orig_checkinput;
static void __fastcall hook_checkinput(void* self, void* edx,
                                       void* stack, void* result)
{
    LONG n = InterlockedIncrement(&g_calls);
    int suppress = 0;
    Fault f;
    __try { suppress = rewrite_cmd(n, stack); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        char prefix[64];
        _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "[%ld] Input        rewrite", n);
        fault_log(prefix, &f, NULL);
    }
    g_orig_checkinput(self, edx, stack, result);

    // The native's own answer, overwritten after it has given it: a UBOOL is
    // a 32-bit int, and every screen returns immediately when it is false.
    // The command has been made inert as well, so a refusal here costs the
    // tidiness of the swallow rather than the swallow itself -- but it means
    // this frame is not what it appears to be, which is worth saying.
    if (suppress) {
        if (writable(result, sizeof(int32_t)))
            *(int32_t*)result = 0;
        else
            logf_("[%ld] Input        result %p not writable; the command was "
                  "made inert instead\n", n, result);
    }
}

THUNK(movie_asvoid, "ASVoid/Movie ")
THUNK(object_asvoid, "ASVoid/Object")
THUNK(panel_invoke, "Invoke/Panel ")

// ---- the hooks, by pointer ---------------------------------------------
//
// The game's code is never written to. EW's exe carries Steam's CEG
// anti-tamper, which hashes stretches of its own code and uses the hash to
// compute where it calls next: modified code sends it into a decoy that
// returns to address 0. MinHook's jumps over the start of each native were
// exactly that, and launching a mission from squad select crashed there every
// time from 2026-09-25 23:27 on (eight dumps, one site: EIP 0, ECX on a decoy
// that returns 0x45, EAX 0x45; the mod's call counter stopped on the launch
// key, no mod code on the stack). The same launch with the mod off went on.
//
// Every native hooked here is only ever reached through a pointer the engine
// keeps in data, and those are what change instead:
//
//   - the registration tables in .data ({ "UClassexecName", func }), which a
//     UFunction binds from when its class is linked -- for anything linked
//     after we attach;
//   - GNatives[], for a native with a bytecode index of its own;
//   - UFunction::Func, for every function already linked when we attach.
//
// The first two are found by scanning the exe's writable sections for the
// native's address, the third by walking the object table for UFunctions of
// the native's name holding it. The script VM calls Function->Func (or
// GNatives[i]) on every call, so a swapped pointer sees every call the
// patched code did. The original is the untouched function itself.
#define MAX_HOOKS 24

typedef struct {
    const char* name;       // "UGFxMoviePlayerexecActionScriptVoid"
    const char* fn;         // "ActionScriptVoid", the UFunction's name
    void*       target;     // the native, unmodified
    void*       thunk;      // ours
    int         data_sites; // pointers swapped in writable data
    int         ro_sites;   // seen in read-only data, left alone
    int         func_sites; // UFunctions swapped, or found already ours
} Hook;

static Hook             g_hooks[MAX_HOOKS];
static int              g_nhooks;
static uint32_t         g_func_off;          // UFunction::Func, once seen
static volatile LONG    g_hooks_swept;       // the UFunction pass has run
static volatile LONG    g_hooks_ready;       // registered and data swapped

// Registers a native to be hooked. Returns its handle, or -1 when the game
// does not have it. `*orig` is the native itself, called as before.
static int arm(const NativeEntry* tbl, int n, HMODULE mod,
               const char* name, void* thunk, void** orig)
{
    void* target = natives_find(tbl, n, name);
    if (!target) {
        logf_("  %-38s NOT FOUND\n", name);
        return -1;
    }
    if (g_nhooks >= MAX_HOOKS) {
        logf_("  %-38s no room (MAX_HOOKS)\n", name);
        return -1;
    }
    const char* ex = strstr(name, "exec");
    Hook* h = &g_hooks[g_nhooks];
    h->name = name;
    h->fn = ex ? ex + 4 : name;
    h->target = target;
    h->thunk = thunk;
    *orig = target;
    logf_("  %-38s %p (rva %p)\n", name, target,
          (void*)((uint8_t*)target - (uint8_t*)mod));
    return g_nhooks++;
}

// The exe's data, section by section: writable sections have the pointers
// swapped; read-only ones are only counted, for the log -- a native's address
// there would be a route to it this cannot reach.
static void hooks_swap_data(HMODULE mod)
{
    uint8_t* m = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(m + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD ch = sec[i].Characteristics;
        if (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) continue;
        if (!(ch & IMAGE_SCN_MEM_READ)) continue;
        int writable_sec = (ch & IMAGE_SCN_MEM_WRITE) != 0;
        uint8_t* base = m + sec[i].VirtualAddress;
        size_t size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                              : sec[i].SizeOfRawData;
        if (!readable(base, size)) continue;
        for (size_t off = 0; off + 4 <= size; off += 4) {
            void** slot = (void**)(base + off);
            void* v = *slot;
            for (int k = 0; k < g_nhooks; k++) {
                if (v != g_hooks[k].target) continue;
                if (writable_sec && writable(slot, sizeof *slot)) {
                    InterlockedExchangePointer(slot, g_hooks[k].thunk);
                    g_hooks[k].data_sites++;
                } else {
                    g_hooks[k].ro_sites++;
                }
                break;
            }
        }
    }
}

// One UFunction. The Func offset is not assumed: the first function of a
// hooked name that holds its native somewhere in its body settles it, and
// every later one is read there.
#define UFUNCTION_SCAN_FROM 0x28
#define UFUNCTION_SCAN_TO   0x100

static int hooks_visit_function(void* obj, int which, int idx, void* ctx)
{
    (void)which; (void)idx; (void)ctx;
    char name[128];
    if (!object_name(obj, name, sizeof name)) return 1;
    for (int k = 0; k < g_nhooks; k++) {
        Hook* h = &g_hooks[k];
        if (strcmp(name, h->fn) != 0) continue;
        if (!g_func_off) {
            if (!readable((uint8_t*)obj + UFUNCTION_SCAN_FROM,
                          UFUNCTION_SCAN_TO - UFUNCTION_SCAN_FROM))
                continue;
            for (uint32_t off = UFUNCTION_SCAN_FROM; off < UFUNCTION_SCAN_TO; off += 4) {
                void* v = *(void**)((uint8_t*)obj + off);
                if (v == h->target || v == h->thunk) { g_func_off = off; break; }
            }
            if (!g_func_off) continue;
        }
        void** slot = (void**)((uint8_t*)obj + g_func_off);
        if (!writable(slot, sizeof *slot)) continue;
        if (*slot == h->target) {
            InterlockedExchangePointer(slot, h->thunk);
            h->func_sites++;
        } else if (*slot == h->thunk) {
            h->func_sites++;           // linked after the tables were swapped
        }
        // Two classes can declare natives of one name (ActionScriptVoid on
        // GFxMoviePlayer and GFxObject); the address tells them apart, so the
        // other hooks of this name still get their look.
    }
    return 1;
}

// Returns 1 once the pass has run. Needs the object table; until it is found
// the swapped tables still cover whatever links from then on.
// One pass at a time: init and review_pump both ask, and on 2026-09-27 both
// ran it at once -- two passes, and the second's props line printed garbage.
static SRWLOCK g_sweep_lock = SRWLOCK_INIT;

static int hooks_sweep_locked(const char* when);

static int hooks_sweep_functions(const char* when)
{
    AcquireSRWLockExclusive(&g_sweep_lock);
    int done = hooks_sweep_locked(when);
    ReleaseSRWLockExclusive(&g_sweep_lock);
    return done;
}

static int hooks_sweep_locked(const char* when)
{
    if (g_hooks_swept) return 1;
    // UObject::Class, which the walk below needs, used to be learnt only from
    // the first UI call (props_init) -- and a UI call reaches this DLL only
    // through a hook. Attached late, with every function linked before the
    // tables were swapped, no call ever came: the log of 2026-09-27 stopped
    // at "waits for the first UI call" and the mod never spoke. So it is
    // learnt here from a native's own UFunction, found by name alone: one
    // whose parameters give props_init the fields it validates against.
    if (!g_hooks_ready || !objects_ready()) return 0;
    if (!props_ready()) {
        static const char* const probe[] = {
            "CheckInputIsReleaseOrDirectionRepeat", "GetClosestValidCursorPosition",
            "ComputePath2",
        };
        for (int i = 0; i < 3 && !props_ready(); i++) {
            const void* fn = objects_named(probe[i]);
            char why[160] = "";
            if (fn && props_init(fn, why, sizeof why))
                logf_("props: %s, learnt from %s\n", why, probe[i]);
        }
    }
    if (!props_class_offset()) return 0;
    const void* fn_class = objects_class("Function");
    if (!fn_class) return 0;
    if (objects_each(&fn_class, 1, hooks_visit_function, NULL) < 0) return 0;
    InterlockedExchange(&g_hooks_swept, 1);

    logf_("hooks: UFunction::Func at +0x%X, the pass run %s\n", (unsigned)g_func_off, when);
    for (int k = 0; k < g_nhooks; k++) {
        Hook* h = &g_hooks[k];
        logf_("  %-38s %d in data%s, %d function%s%s\n", h->name, h->data_sites,
              h->ro_sites ? " (+ read-only, left)" : "", h->func_sites,
              h->func_sites == 1 ? "" : "s",
              !h->data_sites && !h->func_sites ? " -- NOT HOOKED" : "");
    }
    return 1;
}

// From review_pump, when the object table was not there at attach.
static void hooks_sweep_retry(void)
{
    if (g_hooks_swept || !g_hooks_ready) return;
    char why[256];
    if (!objects_retry(why, sizeof why)) return;
    if (why[0]) logf_("objects: %s\n", why);
    hooks_sweep_functions("after startup");
}

static void hooks_install(HMODULE mod)
{
    hooks_swap_data(mod);
    InterlockedExchange(&g_hooks_ready, 1);
}

// Whether a hook will see its calls: some pointer to it was swapped.
static int hook_live(int h)
{
    if (h < 0) return 0;
    return g_hooks[h].data_sites > 0 || g_hooks[h].func_sites > 0;
}

static HINSTANCE g_self;

// The CRT's default invalid-parameter handler calls __fastfail, which no
// SEH frame can intercept. Injected into someone else's process, a mistake in
// this DLL should degrade rather than terminate the game, so it is replaced.
static void __cdecl on_invalid_parameter(const wchar_t* expr, const wchar_t* func,
                                         const wchar_t* file, unsigned line,
                                         uintptr_t reserved)
{
    (void)expr; (void)func; (void)file; (void)reserved;
    logf_("CRT invalid parameter at line %u -- call ignored\n", line);
}

static DWORD WINAPI init(LPVOID param)
{
    (void)param;
    InitializeCriticalSection(&g_alert_lock);
    _set_invalid_parameter_handler(on_invalid_parameter);

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(path, MAX_PATH, "xcom_uihook.log");
    log_open(path);

    HMODULE mod = GetModuleHandleA(NULL);
    logf_("xcom_uihook: module base %p\n", (void*)mod);
    {
        // `path` is the log's; its directory is the game's own.
        char dir[MAX_PATH], exe[MAX_PATH];
        strcpy_s(dir, sizeof dir, path);
        char* s = strrchr(dir, '\\');
        if (s) *(s + 1) = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* base = strrchr(exe, '\\');
        weapon_costs_load(dir, _stricmp(base ? base + 1 : exe, "XComEW.exe") == 0);
        char set_why[MAX_PATH + 64];
        settings_load(dir, set_why, sizeof set_why);
        logf_("options: %s\n", set_why);
    }

    char why[256];
    if (names_init(mod, why, sizeof why))
        logf_("names: %s\n", why);
    else
        logf_("names: UNAVAILABLE (%s) -- falling back to raw pointers\n", why);


    char dll_dir[MAX_PATH];
    GetModuleFileNameA(g_self, dll_dir, MAX_PATH);
    slash = strrchr(dll_dir, '\\');
    if (slash) *(slash + 1) = 0;
    speech_init(dll_dir, why, sizeof why);
    logf_("speech: %s\n", why);

    // The wall field's mixer. A machine with no output device loses the field
    // and keeps everything else, so this is reported and never fatal.
    char audio_why[256];
    audio_start(audio_why, sizeof audio_why);
    logf_("audio: %s\n", audio_why);
    {
        // The saved levels, one per sound.
        char levels[256];
        learn_apply_levels(levels, sizeof levels);
        logf_("audio: %s\n", levels);
        // The heartbeats ship beside the DLL, as the NVDA client does.
        // The height cues are made in audio.c and have no file.
        static const char* const BEAT_FILE[HEART_KINDS] = { "ekgbeep.wav", "alienbeat.wav",
                                                             "doorsound.wav",
                                                             "windowsound.wav", NULL, NULL,
                                                             NULL };
        for (int kind = 0; kind < HEART_KINDS; kind++) {
            if (!BEAT_FILE[kind]) continue;
            char beat[MAX_PATH];
            _snprintf_s(beat, sizeof beat, _TRUNCATE, "%s%s", dll_dir, BEAT_FILE[kind]);
            audio_heart_load(kind, beat, audio_why, sizeof audio_why);
            logf_("audio: %s\n", audio_why);
        }
    }

    // Sound practice, on a thread of its own so that it works at the main menu
    // and not only in a mission. It uses the mixer and speech, so it starts
    // after both.
    char learn_why[256];
    learn_start(learn_why, sizeof learn_why);
    logf_("practice: %s\n", learn_why);

    char mouse_why[128];
    mouse_start(mouse_why, sizeof mouse_why);
    logf_("mouse: %s\n", mouse_why);

    if (CreateThread(NULL, 0, review_pump, NULL, 0, NULL))
        logf_("review: Insert polled every %d ms, everywhere\n", REVIEW_POLL_MS);
    else
        logf_("review: no thread (0x%08lx) -- Insert will not work\n", GetLastError());

    NativeEntry* tbl = (NativeEntry*)malloc(sizeof(NativeEntry) * MAX_NATIVES);
    if (!tbl) { logf_("FATAL: out of memory\n"); return 1; }

    int n = natives_scan(mod, tbl, MAX_NATIVES);
    logf_("natives: %d discovered\n", n);
    if (n == 0) {
        logf_("FATAL: native table not found -- unexpected build?\n");
        free(tbl);
        return 1;
    }

    int h_text[3];
    h_text[0] = arm(tbl, n, mod, "UGFxMoviePlayerexecActionScriptVoid",
                    (LPVOID)hook_movie_asvoid, (LPVOID*)&g_orig_movie_asvoid);
    h_text[1] = arm(tbl, n, mod, "UGFxObjectexecActionScriptVoid",
                    (LPVOID)hook_object_asvoid, (LPVOID*)&g_orig_object_asvoid);
    h_text[2] = arm(tbl, n, mod, "AUI_FxsPanelexecInvoke",
                    (LPVOID)hook_panel_invoke, (LPVOID*)&g_orig_panel_invoke);

    // Not a text source: this one gives the keyboard the actions the game
    // bound only to a gamepad. Counted separately so that its failure cannot
    // be mistaken for a text hook failing, and so that losing it leaves the
    // rest of the mod working.
    int h_input = arm(tbl, n, mod,
                          "AUI_FxsPanelexecCheckInputIsReleaseOrDirectionRepeat",
                          (LPVOID)hook_checkinput, (LPVOID*)&g_orig_checkinput);

    // Nor is this one: it exists to be handed the battle cursor, which a
    // native's `self` gives for free. Counted separately again -- it only
    // matters inside a mission, and its absence must not look like the text
    // hooks failing.
    int h_cursor = arm(tbl, n, mod, "AXCom3DCursorexecGetCursorMode",
                           (LPVOID)hook_cursormode, (LPVOID*)&g_orig_cursormode);

    // The grid's origin. Without it the cursor still reads, but not as a tile.
    int h_grid = arm(tbl, n, mod, "UXComWorldDataexecGetWorldData",
                         (LPVOID)hook_worlddata, (LPVOID*)&g_orig_worlddata);

    // Not hooks: the implementations behind the world-data natives, called to
    // say what is on a tile, read out of the natives' own code. Nothing writes
    // to that code any more, so the order no longer matters.
    tile_arm(tbl, n, mod);

    // Where numpad navigation puts its target in front of the game: the
    // position, the ground under it, and a view of what a confirm did.
    int h_nav[5];
    h_nav[0] = arm(tbl, n, mod, "UXComWorldDataexecGetClosestValidCursorPosition",
                   (LPVOID)hook_validpos, (LPVOID*)&g_orig_validpos);
    h_nav[1] = arm(tbl, n, mod, "UXComWorldDataexecGetFloorZForPosition",
                     (LPVOID)hook_floorz, (LPVOID*)&g_orig_floorz);
    h_nav[2] = arm(tbl, n, mod, "UXComEngineexecIsAnyMoviePlaying",
                     (LPVOID)hook_moviecheck, (LPVOID*)&g_orig_moviecheck);
    // Enter in the ability menu. Its loss costs only that, so it is not
    // counted against navigation.
    arm(tbl, n, mod, "UEngineexecGetEngine",
        (LPVOID)hook_getengine, (LPVOID*)&g_orig_getengine);
    // Aiming from the numpad. Its loss costs only that.
    arm(tbl, n, mod, "AXCom3DCursorexecProcessChainedDistance",
        (LPVOID)hook_chained, (LPVOID*)&g_orig_chained);
    arm(tbl, n, mod, "UEngineexecGetCurrentWorldInfo",
        (LPVOID)hook_worldinfo, (LPVOID*)&g_orig_worldinfo);
    h_nav[3] = arm(tbl, n, mod, "UXComInputBaseexecTestHitPointToFlash",
                     (LPVOID)hook_flashhit, (LPVOID*)&g_orig_flashhit);
    h_nav[4] = arm(tbl, n, mod, "AXComPathingPawnexecComputePath2",
                     (LPVOID)hook_computepath, (LPVOID*)&g_orig_computepath);

    free(tbl);

    // The pointers in the exe's data, then every UFunction already linked --
    // which needs the object table, so that probe now comes before the
    // banner. It takes milliseconds; the one that took long enough to trip
    // the launcher's ten-second wait is long gone. The scanner wants the
    // table too: doors, ladders and the Meld are level actors nothing else in
    // this DLL would see. Not found here, it is asked again from review_pump,
    // and the tables swapped now still catch whatever links meanwhile.
    hooks_install(mod);
    if (objects_init(mod, why, sizeof why))
        logf_("objects: %s\n", why);
    else
        logf_("objects: UNAVAILABLE (%s) -- the scanner will have no doors, "
              "and the hooks wait for it\n", why);
    int swept = hooks_sweep_functions("at startup");

    int armed = hook_live(h_text[0]) + hook_live(h_text[1]) + hook_live(h_text[2]);
    int input_armed = hook_live(h_input);
    int cursor_armed = hook_live(h_cursor);
    int grid_armed = hook_live(h_grid);
    int nav_armed = 1;
    for (int i = 0; i < 5; i++) nav_armed &= hook_live(h_nav[i]);
    if (!armed) { logf_("FATAL: nothing armed\n"); return 1; }

    logf_("%d/3 text hooks armed, key remap %s, cursor watch %s, grid %s, nav %s,"
          " scanner %s -- navigate the UI to produce traffic\n---\n",
          armed, input_armed ? "on" : "OFF", cursor_armed ? "on" : "OFF",
          grid_armed ? "on" : "OFF", nav_armed ? "on" : "OFF",
          nav_armed ? "on" : "OFF");
    if (!swept)
        logf_("hooks: the UFunction pass waits for UObject::Class -- no native function to learn it from yet\n");

    // Said aloud, because the log is the one part of this mod its user cannot
    // read.  Now that the launcher attaches during startup rather than on
    // request, this is the only sign that anything happened at all.
    speech_say(armed == 3 && input_armed
               ? "Accessibility mod ready."
               : "Accessibility mod loaded with errors. Check the log.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, init, NULL, 0, NULL);   // stay off the loader lock
    } else if (reason == DLL_PROCESS_DETACH) {
        log_on_exit();
    }
    return TRUE;
}
