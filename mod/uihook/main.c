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
#include <stdlib.h>

#include "ue3.h"
#include "natives.h"
#include "names.h"
#include "speech.h"
#include "focus.h"
#include "dialog.h"
#include "help.h"
#include "shot.h"
#include "cursor.h"
#include "props.h"
#include "input.h"
#include "../../tools/vendor/MinHook/include/MinHook.h"

#define MAX_NATIVES 8192
#define MAX_STR     4096
#define MAX_FIELDS  64
#define MAX_ELEMS   64      // array elements inspected per property

static FILE*            g_log;
static CRITICAL_SECTION g_lock;
static volatile LONG    g_calls;
static int              g_speak = 1;

// Consecutive duplicates are collapsed: the UI re-sends the same string on
// every refresh, which would otherwise bury the interesting transitions --
// and would make the speech unusable.
static char g_last[MAX_STR + 256];
static long g_repeat;
static ULONGLONG g_repeat_since;
static char g_last_spoken[MAX_STR];

static void emit(const char* line)
{
    if (!g_log) return;
    fputs(line, g_log);
    fflush(g_log);
    OutputDebugStringA(line);
}

static void logf_(const char* fmt, ...)
{
    if (!g_log) return;
    char line[MAX_STR + 256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);

    // Compare past the "[N] " counter, otherwise every line is unique and the
    // collapsing never fires.
    const char* key = line;
    if (key[0] == '[') {
        const char* b = strchr(key, ']');
        if (b) key = b + 1;
    }

    EnterCriticalSection(&g_lock);
    if (strcmp(key, g_last) == 0) {
        g_repeat++;
        // Flush periodically. Holding the count until a *different* line
        // arrives makes a live tail look frozen -- which is exactly how this
        // looked when the main menu was repeating one call.
        ULONGLONG now = GetTickCount64();
        if (now - g_repeat_since > 1000) {
            char note[64];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "      ... repeated %ld times\n", g_repeat);
            emit(note);
            g_repeat = 0;
            g_repeat_since = now;
        }
    } else {
        if (g_repeat) {
            char note[64];
            _snprintf_s(note, sizeof note, _TRUNCATE,
                        "      ... repeated %ld more times\n", g_repeat);
            emit(note);
            g_repeat = 0;
        }
        g_repeat_since = GetTickCount64();
        emit(line);
        strcpy_s(g_last, sizeof g_last, key);
    }
    LeaveCriticalSection(&g_lock);
}

// A pointer is only dereferenced after VirtualQuery says the whole range is
// committed and readable -- this runs on the game's own UI thread and a stray
// read would take the process down with it.  Non-static: names.c uses it too.
//
// The range is rejected outright if it wraps the address space. A garbage
// pointer near the top -- 0xFFFFFFFB, as an ASValue array's Data -- made
// `cur + n` overflow to a small number, the loop below never ran, and the
// range was declared readable without one query. Both capture faults on the
// second mission run were exactly this, in read_array and read_fstring.
static int range_wraps(const void* p, size_t n)
{
    return n > (size_t)UINTPTR_MAX - (uintptr_t)p;
}

int readable(const void* p, size_t n)
{
    if (!p || range_wraps(p, n)) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* cur = (const uint8_t*)p;
    const uint8_t* end = cur + n;
    while (cur < end) {
        if (!VirtualQuery(cur, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return 0;
        if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE ||
              prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
            return 0;
        cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

// Same guard as readable(), but for the one place this DLL writes into the
// game: rewriting an input command in the caller's frame.  A local that is not
// in writable memory means the frame is not what it appears to be, and the
// write is abandoned rather than forced.
static int writable(const void* p, size_t n)
{
    if (!p || range_wraps(p, n)) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* cur = (const uint8_t*)p;
    const uint8_t* end = cur + n;
    while (cur < end) {
        if (!VirtualQuery(cur, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & PAGE_GUARD) return 0;
        DWORD prot = mbi.Protect & 0xFF;
        if (!(prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY))
            return 0;
        cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

// Reads an FString.  Num counts the terminating NUL.
static int read_fstring(const FString* s, char* out, size_t out_sz)
{
    if (!readable(s, sizeof *s)) return 0;
    if (s->Num < 2 || s->Num > MAX_STR) return 0;
    if (s->Max < s->Num) return 0;
    if (!readable(s->Data, (size_t)s->Num * sizeof(wchar_t))) return 0;
    if (s->Data[s->Num - 1] != 0) return 0;

    for (int i = 0; i < s->Num - 1; i++) {
        wchar_t c = s->Data[i];
        if (c == 0) return 0;
        if (c < 32 && c != '\n' && c != '\t' && c != '\r') return 0;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, s->Data, s->Num - 1,
                                out, (int)out_sz - 1, NULL, NULL);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

// Flash markup leaks into a lot of these strings; a screen reader should not
// read tags aloud.
static void strip_markup(char* s)
{
    char* w = s;
    int depth = 0;
    for (char* r = s; *r; r++) {
        if (*r == '<') { depth++; continue; }
        if (*r == '>') { if (depth) depth--; continue; }
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
#define ASVALUE_N      8
#define ASVALUE_S      12

#define AS_NUMBER 2
#define AS_STRING 3

typedef struct {
    char  strings[FOCUS_MAX_LABELS][FOCUS_MAX_LABEL];
    int   nstrings;
    float numbers[8];
    int   nnumbers;
    int   bools[8];
    int   nbools;
} Payload;

static void payload_add_string(Payload* p, const char* s)
{
    if (p->nstrings >= FOCUS_MAX_LABELS) return;
    strncpy_s(p->strings[p->nstrings], FOCUS_MAX_LABEL, s, _TRUNCATE);
    p->nstrings++;
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

// Remembers which (object, function) last spoke, so that a screen publishing
// one row per call can be told apart from a genuine announcement.
static void*     g_last_obj;
static char      g_last_fn[128];
static ULONGLONG g_last_at;


// Which call the capture under way belongs to, for the fault report. Not
// cleared on the way out: an exception filter runs before the unwind, but the
// handler that logs runs after it, and it needs this still in place.
static __declspec(thread) char tls_where[256];

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
    }

    // Payload is ~64KB. Putting that on the game's own thread stack, inside
    // a script VM that is already deep, is asking for trouble; it lives in
    // thread-local storage instead.
    static __declspec(thread) Payload tls_payload;

    Payload* p = &tls_payload;
    p->nstrings = 0;
    p->nnumbers = 0;
    p->nbools   = 0;

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
                    strip_markup(val);
                    if (*val) payload_add_string(p, val);
                } else {
                    read_array((const FArray*)slot, p);
                }
            }
        }
        prop = next;
    }

    // The shot about to be taken. Its panel states one thing per call and
    // says nothing on any of them, so the burst is composed and spoken once.
    // shot.c holds the order it depends on.
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

        char say[SHOT_MAX_TEXT];
        if (shot_note(fn_name, a, b, flag, say, sizeof say)) {
            logf_("[%ld] %s %s.%s  SHOT \"%s\"\n", n, tag, obj_name, fn_name, say);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say_now(say);
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
            int slot = (int)p->numbers[0];
            int disabled = p->nbools ? p->bools[0] : 0;
            help_set(object, slot, label, icon, disabled);
            logf_("[%ld] %s %s.%s  HELP %d = \"%s\" on %s%s\n", n, tag, obj_name,
                  fn_name, slot, label, *icon ? icon : "(no icon)",
                  disabled ? " (disabled)" : "");
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
        joined[0] = 0;
        for (int i = 0; i < p->nstrings; i++) {
            if (looks_like_asset(p->strings[i])) continue;
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
            logf_("[%ld] %s %s.%s  SLOT %d %s \"%s\"%s\n", n, tag, obj_name, fn_name,
                  idx, part == FOCUS_PART_VALUE ? "value =" : "label =", joined,
                  changed ? "  (changed)" : "");
            if (changed) speak_slot(object, idx);
        }
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
        int idx = (int)p->numbers[0];
        char label[FOCUS_MAX_LABEL];
        if (focus_label_at(object, idx, label, sizeof label)) {
            logf_("[%ld] %s %s.%s  FOCUS %d -> \"%s\"\n",
                  n, tag, obj_name, fn_name, idx, label);
            speech_cancel_pending();
            if (g_speak && !muted()) speech_say(label);
        } else {
            logf_("[%ld] %s %s.%s  FOCUS %d unresolved\n",
                  n, tag, obj_name, fn_name, idx);
        }
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

// What a fault was doing, taken in the exception filter while the record is
// still available: the code address, as module+offset so it can be found in a
// disassembly of this DLL, and the address it tried to read or write.
typedef struct {
    DWORD     code;
    void*     at;
    ULONG_PTR access;     // 0 read, 1 write, 8 execute
    ULONG_PTR addr;
    int       has_addr;
} Fault;

static int fault_note(EXCEPTION_POINTERS* ep, Fault* f)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    f->code = r->ExceptionCode;
    f->at = r->ExceptionAddress;
    f->has_addr = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                  r->NumberParameters >= 2;
    f->access = f->has_addr ? r->ExceptionInformation[0] : 0;
    f->addr = f->has_addr ? r->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void fault_log(const char* prefix, const Fault* f, const char* where)
{
    char mod[MAX_PATH] = "?";
    uintptr_t rva = (uintptr_t)f->at;
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)f->at, &m) &&
        GetModuleFileNameA(m, mod, sizeof mod)) {
        rva -= (uintptr_t)m;
        char* slash = strrchr(mod, '\\');
        if (slash) memmove(mod, slash + 1, strlen(slash + 1) + 1);
    }
    char access[48] = "";
    if (f->has_addr)
        _snprintf_s(access, sizeof access, _TRUNCATE, ", %s %p",
                    f->access == 1 ? "writing" : f->access == 8 ? "executing" : "reading",
                    (void*)f->addr);
    logf_("%s faulted (0x%08lx) at %s+0x%X%s%s%s\n", prefix, f->code, mod,
          (unsigned)rva, access, where && *where ? ", in " : "", where ? where : "");
}

// MinHook needs a distinct trampoline per target, so each native gets its own
// thunk; they all funnel into capture().
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
static int rewrite_cmd(LONG n, void* stack)
{
    if (!readable(stack, 0x20)) return DELIVER;

    void* node      = *(void**)((uint8_t*)stack + FFRAME_NODE);
    void* object    = *(void**)((uint8_t*)stack + FFRAME_OBJECT);
    uint8_t* locals = *(uint8_t**)((uint8_t*)stack + FFRAME_LOCALS);
    if (!locals || !props_ready()) return DELIVER;

    char screen[128] = "?";
    object_name(object, screen, sizeof screen);

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

static void cursor_watch(void* self)
{
    cursor_seen(self);

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

static int arm(const NativeEntry* tbl, int n, HMODULE mod,
               const char* name, void* thunk, void** orig)
{
    void* target = natives_find(tbl, n, name);
    if (!target) {
        logf_("  %-38s NOT FOUND\n", name);
        return 0;
    }
    logf_("  %-38s %p (rva %p)\n", name, target,
          (void*)((uint8_t*)target - (uint8_t*)mod));

    MH_STATUS st = MH_CreateHook(target, thunk, orig);
    if (st != MH_OK) { logf_("    MH_CreateHook failed: %d\n", st); return 0; }
    st = MH_EnableHook(target);
    if (st != MH_OK) { logf_("    MH_EnableHook failed: %d\n", st); return 0; }
    return 1;
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
    InitializeCriticalSection(&g_lock);
    _set_invalid_parameter_handler(on_invalid_parameter);

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0;
    strcat_s(path, MAX_PATH, "xcom_uihook.log");
    // NB: fopen_s opens exclusively, which would lock the log for the whole
    // session; _SH_DENYWR keeps it readable while the game runs.
    g_log = _fsopen(path, "w", _SH_DENYWR);

    HMODULE mod = GetModuleHandleA(NULL);
    logf_("xcom_uihook: module base %p\n", (void*)mod);

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

    NativeEntry* tbl = (NativeEntry*)malloc(sizeof(NativeEntry) * MAX_NATIVES);
    if (!tbl) { logf_("FATAL: out of memory\n"); return 1; }

    int n = natives_scan(mod, tbl, MAX_NATIVES);
    logf_("natives: %d discovered\n", n);
    if (n == 0) {
        logf_("FATAL: native table not found -- unexpected build?\n");
        free(tbl);
        return 1;
    }

    if (MH_Initialize() != MH_OK) {
        logf_("FATAL: MH_Initialize\n");
        free(tbl);
        return 1;
    }

    int armed = 0;
    armed += arm(tbl, n, mod, "UGFxMoviePlayerexecActionScriptVoid",
                 (LPVOID)hook_movie_asvoid, (LPVOID*)&g_orig_movie_asvoid);
    armed += arm(tbl, n, mod, "UGFxObjectexecActionScriptVoid",
                 (LPVOID)hook_object_asvoid, (LPVOID*)&g_orig_object_asvoid);
    armed += arm(tbl, n, mod, "AUI_FxsPanelexecInvoke",
                 (LPVOID)hook_panel_invoke, (LPVOID*)&g_orig_panel_invoke);

    // Not a text source: this one gives the keyboard the actions the game
    // bound only to a gamepad. Counted separately so that its failure cannot
    // be mistaken for a text hook failing, and so that losing it leaves the
    // rest of the mod working.
    int input_armed = arm(tbl, n, mod,
                          "AUI_FxsPanelexecCheckInputIsReleaseOrDirectionRepeat",
                          (LPVOID)hook_checkinput, (LPVOID*)&g_orig_checkinput);

    // Nor is this one: it exists to be handed the battle cursor, which a
    // native's `self` gives for free. Counted separately again -- it only
    // matters inside a mission, and its absence must not look like the text
    // hooks failing.
    int cursor_armed = arm(tbl, n, mod, "AXCom3DCursorexecGetCursorMode",
                           (LPVOID)hook_cursormode, (LPVOID*)&g_orig_cursormode);

    // The grid's origin. Without it the cursor still reads, but not as a tile.
    int grid_armed = arm(tbl, n, mod, "UXComWorldDataexecGetWorldData",
                         (LPVOID)hook_worlddata, (LPVOID*)&g_orig_worlddata);

    free(tbl);
    if (!armed) { logf_("FATAL: nothing armed\n"); return 1; }

    logf_("%d/3 text hooks armed, key remap %s, cursor watch %s, grid %s"
          " -- navigate the UI to produce traffic\n---\n",
          armed, input_armed ? "on" : "OFF", cursor_armed ? "on" : "OFF",
          grid_armed ? "on" : "OFF");

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
    }
    return TRUE;
}
