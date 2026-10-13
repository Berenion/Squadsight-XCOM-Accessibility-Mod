// The key list on Ctrl + /. See keys.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "keys.h"
#include "controls.h"
#include "menus.h"
#include "abar.h"
#include "help.h"
#include "history.h"
#include "input.h"
#include "names.h"
#include "cursor.h"
#include "objects.h"
#include "game.h"
#include "strings.h"
#include "speech.h"
#include "log.h"
#include "ue3.h"

// The key is taken by where it is, not by what it types (input_key_at): scan
// 0x35, left of right Shift, / on a US layout, - on a German one. UE3 calls
// it Slash (or Minus, ...), and no input section of DefaultInput.ini binds
// any of them outside the edit boxes' alias lists, so Ctrl and it never
// become an InputEvent and nothing has to be swallowed. Numpad / is the
// same scan code with the extended bit, a different virtual key
// (VK_DIVIDE), and stays the options menu's.
#define KEYS_SCAN 0x35

// ---- the game's keys ---------------------------------------------------------
//
// The game's Controls screen (UIKeybindingsPCScreen) lists its actions from
// XComKeybindingData: a label per action, localized
// (m_arrTacticalBindableLabels, m_arrGeneralBindableLabels), and the command
// each action is bound by, from a native table (InitializeBindableCommandsMap).
// A rebinding moves the command to another key's KeyBind, so the action is
// found by its command, and the key is whatever PlayerInput.Bindings holds it
// under now.
//
// The table, read from XComEW.exe: each action is set with the default
// binding's whole command ("Y_Key_Press | onrelease Y_Key_Release"). Matched
// here on the first word, the press. The index is the action's place in
// TacticalBindableCommands, which indexes the labels -- and the two builds
// number it apart: EU has Hunker Down at 25, before the abilities, and no
// Toggle Overhead Camera at all (decompiled XComKeybindingData, EU and EW).
typedef struct { const char* cmd; int ew, eu; } TacticalCmd;
static const TacticalCmd TACTICAL_CMDS[] = {
    { "SpaceBar_Key_Press",        0,  0 },   // eTBC_EnterShotHUD_Confirm
    { "Escape_Key_Press",          1,  1 },   // eTBC_Pause
    { "Right_Mouse_Button_Press",  2,  2 },   // eTBC_Path
    { "V_Key_Press",               3,  3 },   // eTBC_Interact
    { "X_Key_Press",               4,  4 },   // eTBC_SwapWeapon
    { "Backspace_Key_Press",       5,  5 },   // eTBC_EndTurn
                                              // eTBC_Chat (J): multiplayer only
    { "F1_Key_Press",              7,  7 },   // eTBC_MoreInfo
    { "Home_Key_Press",            8,  8 },   // eTBC_CamCenterOnActiveUnit
    { "W_Key_Press",               9,  9 },   // eTBC_CamMoveUp
    { "S_Key_Press",              10, 10 },   // eTBC_CamMoveDown
    { "A_Key_Press",              11, 11 },   // eTBC_CamMoveLeft
    { "D_Key_Press",              12, 12 },   // eTBC_CamMoveRight
    { "Q_Key_Press",              13, 13 },   // eTBC_CamRotateLeft
    { "E_Key_Press",              14, 14 },   // eTBC_CamRotateRight
    { "T_Key_Press",              15, 15 },   // eTBC_CamZoomIn
    { "G_Key_Press",              16, 16 },   // eTBC_CamZoomOut
    { "Middle_Mouse_Button_Press", 17, 17 },  // eTBC_CamFreeZoom
    { "Z_Key_Press",              18, 18 },   // eTBC_CamToggleZoomLevel
    { "F_Key_Press",              19, 19 },   // eTBC_CursorUp
    { "C_Key_Press",              20, 20 },   // eTBC_CursorDown
    { "Tab_Key_Press",            21, 21 },   // eTBC_NextUnit
    { "Left_Shift_Key_Press",     22, 22 },   // eTBC_PrevUnit
    { "Y_Key_Press",              23, 23 },   // eTBC_AbilityOverwatch
    { "R_Key_Press",              24, 24 },   // eTBC_AbilityReload
    { "K_Key_Press",              45, 25 },   // eTBC_AbilityHunkerDown
    { "N1_Key_Press",             25, 26 },   // eTBC_Ability1
    { "N2_Key_Press",             26, 27 },
    { "N3_Key_Press",             27, 28 },
    { "N4_Key_Press",             28, 29 },
    { "N5_Key_Press",             29, 30 },
    { "N6_Key_Press",             30, 31 },
    { "N7_Key_Press",             31, 32 },
    { "N8_Key_Press",             32, 33 },
    { "N9_Key_Press",             33, 34 },
    { "N0_Key_Press",             34, 35 },   // eTBC_Ability0
    { "F2_Key_Press",             35, 36 },   // eTBC_Target1
    { "F3_Key_Press",             36, 37 },
    { "F4_Key_Press",             37, 38 },
    { "F5_Key_Press",             38, 39 },
    { "F6_Key_Press",             39, 40 },
    { "F7_Key_Press",             40, 41 },
    { "F8_Key_Press",             41, 42 },
    { "F9_Key_Press",             42, 43 },   // eTBC_Target8
    { "F10_Key_Press",            43, 44 },   // eTBC_QuickSave
    { "O_Key_Press",              44, -1 },   // eTBC_ToggleOverheadCam, EW only
};
#define TACTICAL_N ((int)(sizeof TACTICAL_CMDS / sizeof *TACTICAL_CMDS))

// GeneralBindableCommands: what [Engine.PlayerInput] binds for every screen
// outside a mission.
static const char* const GENERAL_CMDS[] = {
    "Enter_Key_Press",              // eGBC_Confirm
    "Escape_Key_Press",             // eGBC_Cancel
    "Arrow_Up",                     // eGBC_NavigateUp
    "Arrow_Down",                   // eGBC_NavigateDown
    "Arrow_Left",                   // eGBC_NavigateLeft
    "Arrow_Right",                  // eGBC_NavigateRight
};
#define GENERAL_N ((int)(sizeof GENERAL_CMDS / sizeof *GENERAL_CMDS))

// LocalizedKeyNames, in the enum's order: the UE3 key names the game words
// itself (m_arrLocalizedKeyNames). Any other key -- a letter, F5 -- is said
// by its name, as XComKeybindingData.GetKeyString does.
static const char* const KEY_NAMES[] = {
    "Tab", "Home", "CapsLock", "Enter", "Escape", "End", "Delete", "BackSpace",
    "Insert", "NumLock", "SpaceBar", "LeftShift", "RightShift", "LeftControl",
    "RightControl", "LeftAlt", "RightAlt", "RightMouseButton", "LeftMouseButton",
    "MiddleMouseButton", "MouseScrollUp", "MouseScrollDown", "ThumbMouseButton",
    "ThumbMouseButton2", "Up", "Down", "Left", "Right", "ScrollLock", "LeftAccent",
    "LeftBracket", "RightBracket", "PageUp", "PageDown", "Divide", "Multiply",
    "Subtract", "Semicolon", "Underscore", "Equals", "Add", "Tilde", "Quote",
    "Slash", "Backslash", "Comma", "Period", "Decimal", "Pause",
};
#define KEY_NAMES_N ((int)(sizeof KEY_NAMES / sizeof *KEY_NAMES))


// The keys Squadsight takes in a mission, unmodified, from whatever the game
// bound them to (README, "Some of the game's keys are taken over"): Home and
// End are the scanner's, Delete and Insert the mod's, and Space is numpad 5's
// stand-in (nav_keys_swallow keeps it from the game while the HUD has the
// keys) -- the 2026-10-13 (19:26) log listed it under "Open shot hud /
// confirm action", which only Enter still is. Not listed under the
// game's actions, which would be telling the player a key does what it no
// longer does.
static int taken_in_mission(const char* key, uint32_t bits)
{
    if (bits & (KB_CONTROL | KB_SHIFT | KB_ALT)) return 0;
    return !_stricmp(key, "Home") || !_stricmp(key, "End") ||
           !_stricmp(key, "Delete") || !_stricmp(key, "Insert") ||
           !_stricmp(key, "SpaceBar");
}

// A key as the Controls screen names it: "CTRL + F5", "NUM 4", "Spacebar".
static void key_words(const char* key, uint32_t bits, char* out, size_t out_sz)
{
    char name[64] = "";
    static const char* const DIGITS[] = {
        "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine",
    };
    for (int d = 0; d < 10 && !name[0]; d++) {
        if (!_stricmp(key, DIGITS[d])) {
            _snprintf_s(name, sizeof name, _TRUNCATE, "%d", d);
        } else if (!_strnicmp(key, "NumPad", 6) && !_stricmp(key + 6, DIGITS[d])) {
            char num[32];
            if (!game_loc("XComKeybindingData", "m_strNumpadKeyPreprocessor", 0, num, sizeof num))
                strcpy_s(num, sizeof num, "NUM");
            _snprintf_s(name, sizeof name, _TRUNCATE, "%s %d", num, d);
        }
    }
    for (int i = 0; i < KEY_NAMES_N && !name[0]; i++)
        if (!_stricmp(key, KEY_NAMES[i]))
            game_loc("XComKeybindingData", "m_arrLocalizedKeyNames", i, name, sizeof name);
    if (!name[0]) strncpy_s(name, sizeof name, key, _TRUNCATE);

    out[0] = 0;
    size_t len = 0;
    static const struct { uint32_t bit; const char* field; } MODS[] = {
        { KB_CONTROL, "m_strControlKeyPreprocessor" },
        { KB_ALT,     "m_strAltKeyPreprocessor" },
        { KB_SHIFT,   "m_strShiftKeyPreprocessor" },
    };
    for (int m = 0; m < 3; m++) {
        if (!(bits & MODS[m].bit)) continue;
        char mod[32];
        if (!game_loc("XComKeybindingData", MODS[m].field, 0, mod, sizeof mod)) continue;
        _snprintf_s(out + len, out_sz - len, _TRUNCATE, "%s + ", mod);
        len = strlen(out);
    }
    _snprintf_s(out + len, out_sz - len, _TRUNCATE, "%s", name);
}

int keys_bind_words(const KeyBind* kb, char* out, size_t out_sz)
{
    char key[64];
    out[0] = 0;
    if (!readable(kb, sizeof *kb) || !name_to_string(&kb->Name, key, sizeof key) ||
        !strcmp(key, "None"))
        return 0;
    key_words(key, kb->bits, out, out_sz);
    return 1;
}

// Whether a binding's command begins with `cmd` as its first word.
static int command_is(const char* command, const char* cmd)
{
    size_t n = strlen(cmd);
    return !_strnicmp(command, cmd, n) && (command[n] == 0 || command[n] == ' ');
}

// One line per action with a keyboard key: "Overwatch: Y". Mouse buttons are
// left out, and in a mission the keys the mod took; an action left with no
// key is not listed. Returns how many lines were added.
static int game_lines(void* input, int mission, char (*lines)[HISTORY_PAGE_TEXT], int max)
{
    static FieldSlot slot;
    const void* v = NULL;
    if (!field_ptr(input, "Bindings", &slot, sizeof(FArray), &v)) return 0;
    const FArray* arr = (const FArray*)v;
    if (arr->Num <= 0 || arr->Num > 1024 ||
        !readable(arr->Data, (size_t)arr->Num * sizeof(KeyBind))) {
        logf_("keys: Bindings unreadable (%d entries at %p)\n", arr->Num, arr->Data);
        return 0;
    }
    const KeyBind* binds = (const KeyBind*)arr->Data;

    int ncmds = mission ? TACTICAL_N : GENERAL_N;
    const char* labels = mission ? "m_arrTacticalBindableLabels" : "m_arrGeneralBindableLabels";
    int n = 0, unread = 0;
    for (int a = 0; a < ncmds && n < max; a++) {
        const char* cmd = mission ? TACTICAL_CMDS[a].cmd : GENERAL_CMDS[a];
        int label_at = !mission ? a : game_is_ew() ? TACTICAL_CMDS[a].ew : TACTICAL_CMDS[a].eu;
        if (label_at < 0) continue;
        char keys[256] = "";
        size_t len = 0;
        for (int b = 0; b < arr->Num; b++) {
            char command[256], key[64];
            if (!read_fstring(&binds[b].Command, command, sizeof command)) continue;
            if (!command_is(command, cmd)) continue;
            if (!name_to_string(&binds[b].Name, key, sizeof key)) continue;
            if (text_find_ci(key, "Mouse")) continue;
            if (mission && taken_in_mission(key, binds[b].bits)) continue;
            char words[96];
            key_words(key, binds[b].bits, words, sizeof words);
            if (len) {
                _snprintf_s(keys + len, sizeof keys - len, _TRUNCATE, "%s", T(KEYS_OR));
                len = strlen(keys);
            }
            _snprintf_s(keys + len, sizeof keys - len, _TRUNCATE, "%s", words);
            len = strlen(keys);
        }
        if (!keys[0]) continue;
        char label[128], said[128];
        if (!game_loc("XComKeybindingData", labels, label_at, label, sizeof label)) { unread++; continue; }
        text_sentence_case(label, said, sizeof said);
        _snprintf_s(lines[n], HISTORY_PAGE_TEXT, _TRUNCATE, "%s: %s.", said, keys);
        n++;
    }
    logf_("keys: %d of the game's %s actions from %d bindings%s\n", n,
          mission ? "tactical" : "general", arr->Num,
          unread ? ", some labels unread" : "");
    return n;
}

// ---- the list ------------------------------------------------------------------

static const StrId MISSION_KEYS[] = {
    KEYS_STEP, KEYS_DIAGONAL, KEYS_HERE, KEYS_GO,
    KEYS_SCAN_ITEM, KEYS_SCAN_CATEGORY, KEYS_SCAN_STOREY, KEYS_SCAN_GO, KEYS_SCAN_BACK,
    KEYS_SCAN_FAR, KEYS_SCAN_WAY_BACK,
    KEYS_SOLDIER, KEYS_SQUAD, KEYS_ENEMIES, KEYS_OBJECTIVES, KEYS_ABILITIES,
    KEYS_TARGET_INFO, KEYS_HISTORY, KEYS_WALLS, KEYS_OPTIONS, KEYS_THIS,
};
static const StrId MENU_KEYS[] = {
    KEYS_BUTTONS, KEYS_DIGITS, KEYS_BASE, KEYS_HISTORY, KEYS_OPTIONS, KEYS_THIS,
};

static int add(char (*lines)[HISTORY_PAGE_TEXT], int n, const char* text)
{
    if (n >= HISTORY_PAGE_MAX) return n;
    strncpy_s(lines[n], HISTORY_PAGE_TEXT, text, _TRUNCATE);
    return n + 1;
}

static void keys_open(void)
{
    // In a mission when the last key the game took went to the tactical
    // input (hud_has_keys' object). Before any key has reached the game,
    // or once that object is gone, it is the menus' list.
    void* input = nav_input_object();
    char cls[64] = "";
    if (input && (!unit_is_live(input) || !object_class_name(input, cls, sizeof cls)))
        input = NULL;
    int mission = input && !strcmp(cls, "XComTacticalInput");

    // No input object yet: the mod's own keys never pass through InputEvent,
    // so Ctrl + / can be the first key of a mission, and the base's input
    // object is gone by then -- the 2026-10-13 (19:11) log: "menu list (input
    // unknown)" in a mission. Which HUD drew last says where the player is,
    // and the class default object holds the same config Bindings as any
    // instance (rebinding saves to the config and reloads it).
    if (!input) {
        mission = layer_tactical();
        const char* cdo = mission ? "Default__XComTacticalInput"
                                  : "Default__XComHeadquartersInput";
        input = (void*)objects_named(cdo);
        if (!input && !mission) input = (void*)objects_named(cdo = "Default__XComShellInput");
        strcpy_s(cls, sizeof cls, input ? cdo : "unknown");
    }

    static char lines[HISTORY_PAGE_MAX][HISTORY_PAGE_TEXT];
    static char say[HISTORY_PAGE_TEXT + 64];
    int n = add(lines, 0, T(KEYS_MOD));
    const StrId* mod = mission ? MISSION_KEYS : MENU_KEYS;
    int nmod = mission ? (int)(sizeof MISSION_KEYS / sizeof *MISSION_KEYS)
                       : (int)(sizeof MENU_KEYS / sizeof *MENU_KEYS);
    // On the game's Controls screen, its keys first: they are the mod's too.
    if (!mission && controls_up()) n = add(lines, n, T(CONTROLS_HINT));
    for (int i = 0; i < nmod; i++) n = add(lines, n, T(mod[i]));

    int game = 0;
    if (input && n + 1 < HISTORY_PAGE_MAX) {
        GUARDED("keys: bindings",
                game = game_lines(input, mission, lines + n + 1, HISTORY_PAGE_MAX - n - 1),
                game = 0);
        if (game) {
            strncpy_s(lines[n], HISTORY_PAGE_TEXT, T(KEYS_GAME), _TRUNCATE);
            n += 1 + game;
        }
    }

    menu_polled();
    int opened = history_page_open(T(KEYS_TITLE), (const char (*)[HISTORY_PAGE_TEXT])lines,
                                   n, say, sizeof say);
    logf_("keys: %s, %s list (input %s), %d entries, %d of them the game's\n",
          opened ? "opened" : "nothing to open", mission ? "mission" : "menu",
          cls[0] ? cls : "unknown", n, game);
    speech_cancel_pending();
    speech_say_now(say);
}

static int g_was;

void keys_poll(void)
{
    int down = input_key_at_down(KEYS_SCAN, "the key left of right Shift") &&
               (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    int pressed = down && !g_was;
    g_was = down;
    if (!pressed) return;
    if (history_is_open()) { review_end("Ctrl + slash"); return; }
    // Another of the mod's menus has the arrows; opening over it would leave
    // both reading the same keys.
    if (abar_menu_is_open() || help_menu_is_open()) {
        logf_("keys: Ctrl + slash with another menu open -- not opened\n");
        return;
    }
    keys_open();
}
