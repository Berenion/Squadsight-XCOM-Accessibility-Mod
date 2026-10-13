// The game's Controls screen from the keyboard. See controls.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "controls.h"
#include "keys.h"
#include "input.h"
#include "cursor.h"
#include "props.h"
#include "names.h"
#include "game.h"
#include "strings.h"
#include "speech.h"
#include "log.h"
#include "ue3.h"

// ---- calling the screen's script ---------------------------------------------
//
// UObject::ProcessEvent(UFunction*, void* Parms, void* Result), a virtual at
// +0x10C in both builds: every native event wrapper is FindFunctionChecked
// then `mov edx, [vtable + 0x10C]; push eax; call edx` -- 976 of the 988
// callers of FindFunctionChecked in XComEW.exe, 965 of 978 in XComGame.exe.
// It runs the function on the caller's thread, so it is only ever called from
// a hook the game's thread is in (rewrite_cmd).
//
// The parameters are laid out at their own UProperty offsets in a buffer of
// ours. ProcessEvent copies them into the frame and destroys only the locals
// past the parameters, so what the buffer points at -- the args array here --
// stays ours and is never freed by the game.
#define PROCESS_EVENT_VSLOT (0x10C / 4)
typedef void (__fastcall* ProcessEventFn)(void* self, void* edx, void* func, void* parms,
                                          void* result);

#define MOUSE_CLICK 391     // OnMouseEvent's Cmd for a click (case 391)

// The function's first two parameters' offsets; 0 when it has fewer.
static int param_offsets(const void* func, uint32_t* first, uint32_t* second)
{
    const void* prop = NULL;
    if (readable((const uint8_t*)func + USTRUCT_CHILDREN, sizeof(void*)))
        prop = *(const void* const*)((const uint8_t*)func + USTRUCT_CHILDREN);
    int found = 0;
    for (int guard = 0; prop && guard < 64 && found < 2; guard++) {
        if (!readable(prop, 0x68)) return 0;
        uint32_t flags = *(const uint32_t*)((const uint8_t*)prop + UPROPERTY_FLAGS);
        uint32_t off   = *(const uint32_t*)((const uint8_t*)prop + UPROPERTY_OFFSET);
        if ((flags & CPF_PARM) && !(flags & CPF_RETURNPARM)) {
            if (found == 0) *first = off; else *second = off;
            found++;
        }
        prop = *(const void* const*)((const uint8_t*)prop + UFIELD_NEXT);
    }
    return found == 2;
}

static void fstring_of(FString* s, wchar_t* buf, size_t buf_n, const char* text)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, buf, (int)buf_n);
    if (n <= 0) { buf[0] = 0; n = 1; }
    s->Data = buf;
    s->Num = n;
    s->Max = n;
}

// screen.OnMouseEvent(391, [index, slot, callback]): what a click on
// `callback` sends. OnMouseEvent reads args from the end -- the callback
// object last, "primarySlot" or not before it, the slot's index before that
// -- so the same three serve a slot, a tab and a button.
static int click(void* screen, const char* callback, int index, int secondary)
{
    const void* func = object_field_prop(screen, "OnMouseEvent");
    uint32_t cmd_off = 0, args_off = 0;
    if (!func || !param_offsets(func, &cmd_off, &args_off) ||
        cmd_off > 0xF0 || args_off > 0xF0) {
        logf_("controls: no OnMouseEvent to call (%p)\n", func);
        return 0;
    }
    static wchar_t texts[3][32];
    static FString args[3];
    char index_text[16];
    _snprintf_s(index_text, sizeof index_text, _TRUNCATE, "%d", index);
    fstring_of(&args[0], texts[0], 32, index_text);
    fstring_of(&args[1], texts[1], 32, secondary ? "secondarySlot" : "primarySlot");
    fstring_of(&args[2], texts[2], 32, callback);

    uint8_t parms[0x100];
    memset(parms, 0, sizeof parms);
    *(int32_t*)(parms + cmd_off) = MOUSE_CLICK;
    FArray* arr = (FArray*)(parms + args_off);
    arr->Data = args;
    arr->Num = 3;
    arr->Max = 3;

    void** vtable = *(void***)screen;
    if (!readable(vtable + PROCESS_EVENT_VSLOT, sizeof(void*))) return 0;
    ProcessEventFn pe = (ProcessEventFn)vtable[PROCESS_EVENT_VSLOT];
    logf_("controls: click %s (slot %d, %s)\n", callback, index,
          secondary ? "secondary" : "primary");
    pe(screen, NULL, (void*)func, parms, NULL);
    return 1;
}

// screen.OnCancel(""): what Escape does, called while a slot is waiting so
// that Escape is not taken as the new key (see controls_key).
static int cancel(void* screen)
{
    const void* func = object_field_prop(screen, "OnCancel");
    if (!func) return 0;
    uint8_t parms[0x40];
    memset(parms, 0, sizeof parms);
    void** vtable = *(void***)screen;
    if (!readable(vtable + PROCESS_EVENT_VSLOT, sizeof(void*))) return 0;
    ((ProcessEventFn)vtable[PROCESS_EVENT_VSLOT])(screen, NULL, (void*)func, parms, NULL);
    return 1;
}

// ---- the screen's state ---------------------------------------------------------

// UIKeyBind: UserLabel, PrimaryBind, SecondaryBind, BlockUnbindingPrimaryKey.
typedef struct {
    FString  UserLabel;
    KeyBind  Primary;
    KeyBind  Secondary;
    uint32_t block;
} UIKeyBind;

static FieldSlot g_arr_slot, g_cat_slot, g_bound_slot, g_second_slot;

static const UIKeyBind* rows(void* screen, int* n)
{
    const void* v;
    *n = 0;
    if (!field_ptr(screen, "m_arrBindings", &g_arr_slot, sizeof(FArray), &v)) return NULL;
    const FArray* arr = (const FArray*)v;
    if (arr->Num <= 0 || arr->Num > 128 ||
        !readable(arr->Data, (size_t)arr->Num * sizeof(UIKeyBind)))
        return NULL;
    *n = arr->Num;
    return (const UIKeyBind*)arr->Data;
}

static int screen_bool(void* screen, const char* name)
{
    const void* prop = object_field_prop(screen, name);
    int b = 0;
    if (!prop || !props_read_object_bool(prop, (const uint8_t*)screen, &b)) return -1;
    return b != 0;
}

static int screen_int(void* screen, const char* name, FieldSlot* slot, size_t size)
{
    const void* v;
    if (!field_ptr(screen, name, slot, size, &v)) return -1;
    return size == 1 ? *(const uint8_t*)v : *(const int32_t*)v;
}

// The game's own word for something on the screen, sentence-cased.
static void screen_word(const char* field, char* out, size_t out_sz)
{
    char raw[128];
    if (!game_loc("UIKeybindingsPCScreen", field, 0, raw, sizeof raw)) { out[0] = 0; return; }
    text_sentence_case(raw, out, out_sz);
}

// ---- what is said -----------------------------------------------------------------

static void*     g_screen;      // the screen the cursor belongs to
static int       g_row;
static int       g_secondary;   // the column: 0 primary, 1 secondary
static int       g_eat_cmd;     // the rest of a keystroke already used

static void say(const char* text)
{
    speech_cancel_pending();
    speech_say_now(text);
}

static void bind_words(const KeyBind* kb, char* out, size_t out_sz)
{
    if (!keys_bind_words(kb, out, out_sz)) strncpy_s(out, out_sz, T(CONTROLS_UNBOUND), _TRUNCATE);
}

// "Overwatch. Primary: Y. Secondary: none." -- or, with `column_only`, just
// the column the cursor is on: "Secondary: none."
static void row_words(void* screen, int i, int column_only, char* out, size_t out_sz)
{
    int n;
    const UIKeyBind* r = rows(screen, &n);
    out[0] = 0;
    if (!r || i < 0 || i >= n) {
        strncpy_s(out, out_sz, T(CONTROLS_NOTHING), _TRUNCATE);
        return;
    }
    char label[128] = "", said[128], first[96], second[96], h1[64], h2[64];
    read_fstring(&r[i].UserLabel, label, sizeof label);
    text_sentence_case(label, said, sizeof said);
    bind_words(&r[i].Primary, first, sizeof first);
    bind_words(&r[i].Secondary, second, sizeof second);
    screen_word("m_strPrimaryBindingsColumnHeader", h1, sizeof h1);
    screen_word("m_strSecondaryBindingsColumnHeader", h2, sizeof h2);
    if (column_only)
        _snprintf_s(out, out_sz, _TRUNCATE, "%s: %s.", g_secondary ? h2 : h1,
                    g_secondary ? second : first);
    else
        tfmt(out, out_sz, CONTROLS_ROW, said, h1, first, h2, second);
}

static void say_row(LONG n, void* screen, int column_only)
{
    char text[512];
    row_words(screen, g_row, column_only, text, sizeof text);
    logf_("[%ld] controls: row %d \"%s\"\n", n, g_row, text);
    say(text);
}

static void say_tab(LONG n, void* screen)
{
    int count, cat = screen_int(screen, "m_eBindingCategory", &g_cat_slot, 1);
    rows(screen, &count);
    char tab[96], text[640];
    screen_word(cat == 1 ? "m_strTacticalBindingsCategoryLabel"
                         : "m_strGeneralBindingsCategoryLabel", tab, sizeof tab);
    size_t len = 0;
    text[0] = 0;
    tpfmt_cat(text, sizeof text, &len, CONTROLS_TAB, count, tab, count);
    char row[512];
    row_words(screen, g_row, 0, row, sizeof row);
    _snprintf_s(text + len, sizeof text - len, _TRUNCATE, " %s", row);
    logf_("[%ld] controls: tab %d \"%s\"\n", n, cat, text);
    say(text);
}

// The presentation layer spawns the screen once and keeps it
// (XComPresentationLayerBase, m_kPCKeybindings), so being alive says nothing:
// it is up while its b_IsVisible is set (Show / Hide, as mission_visible).
int controls_up(void)
{
    void* s = g_screen;
    if (!s || !unit_is_live(s)) return 0;
    return screen_bool(s, "b_IsVisible") == 1;
}

static void adopt(void* object)
{
    if (object == g_screen) return;
    g_screen = object;
    g_row = 0;
    g_secondary = 0;
    g_eat_cmd = 0;
}

// ---- the keys -------------------------------------------------------------------

#define CMD_TAB 571

int controls_key(LONG n, void* object, const char* screen, int cmd, int mask, int have_mask)
{
    if (strncmp(screen, "UIKeybindingsPCScreen", 21) != 0) return 0;
    adopt(object);
    int press = !have_mask || (mask & FXS_ACTION_PRESS);
    int release = have_mask && (mask & FXS_ACTION_RELEASE);

    // The rest of a keystroke the mod already acted on.
    if (g_eat_cmd && cmd == g_eat_cmd) {
        if (release) g_eat_cmd = 0;
        return 1;
    }
    g_eat_cmd = 0;

    // A slot waiting for its key takes the next key's RELEASE (RawInputHandler
    // returns at once for anything else), Escape's included -- so Escape would
    // become the new key, not a way out. Its press comes through here first:
    // cancelled now, as OnCancel does for the gamepad's B, and the release
    // kept from the screen, which would otherwise leave it.
    int waiting = screen_bool(object, "m_bAwaitingInputForBind");
    if (waiting == 1) {
        if (cmd == FXS_KEY_ESCAPE && press) {
            cancel(object);
            g_eat_cmd = cmd;
            logf_("[%ld] controls: Escape -- the slot no longer waits\n", n);
            say(T(CONTROLS_KEPT));
            return 1;
        }
        return 0;           // the screen ignores it; the key is the new binding
    }

    int count;
    rows(object, &count);
    switch (cmd) {
    case FXS_ARROW_UP:
    case FXS_ARROW_DOWN:
        if (press) {
            int to = g_row + (cmd == FXS_ARROW_DOWN ? 1 : -1);
            if (to < 0 || to >= count) {
                char text[560], row[512];
                row_words(object, g_row, 0, row, sizeof row);
                _snprintf_s(text, sizeof text, _TRUNCATE, "%s %s",
                            T(to < 0 ? TXT_TOP : TXT_END), row);
                say(text);
            } else {
                g_row = to;
                say_row(n, object, 0);
            }
        }
        return 1;
    case FXS_ARROW_LEFT:
    case FXS_ARROW_RIGHT:
        if (press) {
            g_secondary = cmd == FXS_ARROW_RIGHT;
            say_row(n, object, 1);
        }
        return 1;
    case FXS_KEY_ENTER:
    case FXS_KEY_SPACEBAR:
        // On the release, not the press: started on the press, the slot
        // would be waiting when this same key's release reaches
        // RawInputHandler, and Enter would be bound.
        if (release && g_row < count) {
            click(object, "bgHighlight", g_row, g_secondary);
            char press_key[96], text[640], row[512];
            screen_word("m_strPressKeyLabel", press_key, sizeof press_key);
            row_words(object, g_row, 0, row, sizeof row);
            _snprintf_s(text, sizeof text, _TRUNCATE, "%s %s", row, press_key);
            logf_("[%ld] controls: slot %d %s waits for a key (now %d)\n", n, g_row,
                  g_secondary ? "secondary" : "primary",
                  screen_bool(object, "m_bAwaitingInputForBind"));
            say(text);
        }
        return 1;
    case CMD_TAB:
        if (press) {
            int cat = screen_int(object, "m_eBindingCategory", &g_cat_slot, 1);
            click(object, cat == 1 ? "tab1_background" : "tab2_background", 0, 0);
            g_row = 0;
            say_tab(n, object);
        }
        return 1;
    case FXS_KEY_1:
        if (press && g_row < count) {
            const UIKeyBind* r = rows(object, &count);
            if (r && !g_secondary && r[g_row].block) {
                say(T(CONTROLS_CANT_CLEAR));
            } else {
                click(object, "clearBindingButton", g_row, g_secondary);
                say_row(n, object, 1);
            }
        }
        return 1;
    case FXS_KEY_2:
        // The game asks first (DisplayConfirmResetBindingsDialog); the dialog
        // is read as any other.
        if (press) click(object, "theResetToDefaultsButton", 0, 0);
        return 1;
    case FXS_KEY_3:
        if (press) click(object, "theAcceptButton", 0, 0);
        return 1;
    case FXS_KEY_0:
        // The screen's buttons are Flash's and never reach a help bar, so 0's
        // menu (help.c) found the Options screen's bar still underneath and
        // offered its SAVE CHANGES AND EXIT -- which this screen ignores. The
        // 2026-10-13 (19:07) log: the player chose it twice, then Escape
        // discarded every change. So 0 here names this screen's own buttons
        // with the keys that press them, then the rest of its keys.
        if (press) {
            char save[64], reset[64], back[64], text[1024];
            screen_word("m_strSaveAndExitButtonLabel", save, sizeof save);
            screen_word("m_strResetBindingsButtonLabel", reset, sizeof reset);
            screen_word("m_strCancelButtonLabel", back, sizeof back);
            _snprintf_s(text, sizeof text, _TRUNCATE, "%s: 3. %s: 2. %s: %s. %s",
                        save, reset, back, T(KEY_ESCAPE), T(CONTROLS_HINT));
            logf_("[%ld] controls: 0 \"%s\"\n", n, text);
            say(text);
        }
        return 1;
    }
    return 0;
}

// ---- what the screen draws ------------------------------------------------------

int controls_capture(LONG n, void* object, const char* obj_name, const char* fn_name)
{
    if (strncmp(obj_name, "UIKeybindingsPCScreen", 21) != 0) return 0;
    adopt(object);
    // The title is the first thing OnInit draws: the screen is new.
    if (!strcmp(fn_name, "AS_SetTitle")) {
        g_row = 0;
        g_secondary = 0;
        char title[128], text[768];
        screen_word("m_strTitle", title, sizeof title);
        _snprintf_s(text, sizeof text, _TRUNCATE, "%s. %s", title, T(CONTROLS_HINT));
        logf_("[%ld] controls: opened \"%s\"\n", n, text);
        say(text);
        return 1;
    }
    // A key just bound, by RawInputHandler or the conflict dialog's yes: the
    // slot that was waiting, with its new key.
    if (!strcmp(fn_name, "AS_SetNewKey")) {
        int slot = screen_int(object, "m_iKeySlotBeingBound", &g_bound_slot, 4);
        int second = screen_bool(object, "m_bSecondaryKeyBeingBound");
        if (slot >= 0) {
            g_row = slot;
            if (second >= 0) g_secondary = second;
        }
        say_row(n, object, 0);
        return 1;
    }
    // A list drawn afresh (a tab, a reset): the cursor stays if it still fits.
    if (!strcmp(fn_name, "AS_ClearBindingsList")) {
        int count;
        rows(object, &count);
        if (g_row >= count) g_row = 0;
    }
    logf_("[%ld] controls: %s (not said)\n", n, fn_name);
    return 1;
}
