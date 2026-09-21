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
#include "scan.h"
#include "objects.h"
#include "help.h"
#include "shot.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"
#include "sonar.h"
#include "audio.h"
#include "learn.h"
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

// The log is flushed per line on purpose: it is the one part of this mod its
// user cannot read, so it has to survive a crash and stay readable while the
// game runs. What was not on purpose was OutputDebugStringA beside it --
// every call takes the machine-wide DBWinMutex, and the battle hooks emit a
// line per flag per frame, so the game's UI thread was queueing on a global
// lock thousands of times a mission to write to a debugger nobody had
// attached. The log file says everything the debug channel did.
static void emit(const char* line)
{
    if (!g_log) return;
    fputs(line, g_log);
    fflush(g_log);
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

static void unit_note(void* flag, const char* name, const char* nick);

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
    // A unit's name, over its head. Each argument arrives twice -- as the
    // parameter and again in the ASValue array -- and an empty nickname not
    // at all, so a second string equal to the first is the name repeated.
    if (strncmp(obj_name, "UIUnitFlag_", 11) == 0 && strcmp(fn_name, "SetNames") == 0 &&
        p->nstrings > 0) {
        const char* nick = p->nstrings > 1 && strcmp(p->strings[1], p->strings[0]) != 0
                               ? p->strings[1] : "";
        unit_note(object, p->strings[0], nick);
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
// sliders.
static int       g_walls_on = 1;
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

// ---- what is on the tile ---------------------------------------------------
//
// The game's own answers, asked of its C++ directly: see tile.h for why the
// vtable, and how the slots are found. These are pure queries, called on the
// game thread from inside one of its own natives, which is where the script
// would have called them from.
typedef int (__fastcall* TileCoverFn)(void* self, void* edx, float x, float y,
                                      float z, TileCoverPoint* out);
typedef int (__fastcall* TileTestFn)(void* self, void* edx, int x, int y, int z);
typedef int (__fastcall* UnitTestFn)(void* self, void* edx);
typedef int (__fastcall* CursorFloorFn)(void* self, void* edx, float x, float y, float z);

static int       g_tile_slot_cover = -1, g_tile_slot_smoke = -1, g_tile_slot_poison = -1;
static int       g_tile_slot_occupied = -1; // XComWorldData.IsTileOccupied
static int       g_unit_slot_visible = -1;  // XGUnitNativeBase.IsAliveAndVisible
static int       g_cursor_slot_floor = -1;  // XCom3DCursor.WorldZToCursorFloor
static uint8_t*  g_image_lo;            // the game's image, to check a vtable entry
static uint8_t*  g_image_hi;            // points into it before calling it
static void*     g_path_pawn;           // the pathing pawn that built the last path
static void*     g_reach_pawn;          // the pawn the path offsets were resolved on
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
static char      g_step_who[TILE_MAX_TEXT];   // units on the tile, found on arrival

// How long after a tile's first path it is described. None: the next frame.
// It was 200 ms while "Dash" came from DestinationReachability, which the
// dash rebuild (ChangeDashState) changes a tick later. The path's own cost
// needs no such wait -- the pathfinder builds the whole path at once, past
// the dash limit included (costs of 54 against a MaxPathCost of 24).
#define TILE_DESCRIBE_DELAY_MS 0
// The soldier's own tile has no verdict to wait for, only the floor search,
// which on level ground settles on its first frame.
#define OWN_TILE_DELAY_MS 60

// A virtual function of `obj`, or NULL when the slot is unknown or the entry
// does not point into the game's image.
static void* tile_vfn(void* obj, int slot)
{
    if (slot < 0 || !obj || !readable(obj, sizeof(void*))) return NULL;
    uint8_t* vt = *(uint8_t**)obj;
    if (!readable(vt + slot, sizeof(void*))) return NULL;
    uint8_t* fn = *(uint8_t**)(vt + slot);
    if (fn < g_image_lo || fn >= g_image_hi) return NULL;
    return fn;
}

// Where a field lookup's answer is kept, for one call site.
//
// Both answers, deliberately. A miss is the expensive one: field_find walks
// every child of every class up the chain -- XGUnit alone declares over 700
// members -- decoding a name for each, and gives up only at Object. Keeping
// only the hit meant that a class *without* the field paid that walk on every
// single call, and a mission spent 311 of them on one lookup.
//
// Two classes rather than one, because the classes alternate. The unit flags
// are walked in a row, and the two whose class has no m_kUnit sit among
// fourteen whose class does; a single slot would have each of them evicting
// the other, which is how the miss got expensive in the first place.
typedef struct {
    void*    on;        // the class the offset was found on
    void*    absent;    // a class since proved not to have the field at all
    uint32_t off;
} FieldSlot;

// An object's field, by name: the offset is looked up again whenever the
// object's class is not one this slot has already decided. An offset belongs
// to the class, so that is once per class, not once per unit per key press.
static int field_ptr(void* obj, const char* name, FieldSlot* slot,
                     size_t size, const void** out)
{
    if (!obj) return 0;
    uint32_t class_off = props_class_offset();
    if (!class_off || !readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    void* cls = *(void* const*)((const uint8_t*)obj + class_off);
    if (!cls || cls == slot->absent) return 0;
    if (cls != slot->on) {
        if (!object_field_offset(obj, name, &slot->off)) {
            slot->absent = cls;
            // A class that cannot be read is not a missing field, it is a
            // dead object, and the answer is to stop holding the pointer --
            // which is whoever is holding it to say, not this. Saying it here
            // filled a log with "on an unreadable class" and named neither
            // the object nor anything that could be done about it.
            char cls_name[128];
            if (object_class_name(obj, cls_name, sizeof cls_name))
                logf_("field: no %s on %s\n", name, cls_name);
            return 0;
        }
        slot->on = cls;
    }
    const uint8_t* v = (const uint8_t*)obj + slot->off;
    if (!readable(v, size)) return 0;
    *out = v;
    return 1;
}

// The soldier's own tile, from the pawn the cursor is chained to. `z` gets the
// pawn's height, which stands in for a cursor's: both are compared with the
// floor through NAV_CURSOR_LIFT.
//
// Needed because the cursor is not the soldier. In mouse mode it follows the
// mouse every frame, and when the soldier changes the camera pans while the
// mouse stays put, so the cursor lands wherever the mouse now points -- in one
// run, three soldiers in a row began at the map's northern edge, rows 54 to
// 60 of 61, where no path went anywhere.
static FieldSlot g_soldier_loc;

static int soldier_tile(const CursorGrid* g, int* tx, int* ty, float* z)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn ||
        !field_ptr(pawn, "Location", &g_soldier_loc, 3 * sizeof(float), &v))
        return 0;
    const float* loc = (const float*)v;
    *tx = cursor_tile_axis(loc[0], g->min_x, CURSOR_TILE);
    *ty = cursor_tile_axis(loc[1], g->min_y, CURSOR_TILE);
    *z = loc[2];
    return 1;
}

// How far the path just built goes, against how far the soldier may go:
//   0 a standard move, 1 a dash, 2 past this turn's reach, -1 unreadable.
// *turns_out gets how many turns the path takes (tile_turns).
//
// DestinationReachability was the first try and said "dash" on every tile:
// SetActive(kUnit, bCanDash) sets it to 1 for any soldier who *can* dash. The
// second -- the path's XComPath.Cost over XComPathingPawn.StandardMoveLength,
// the test the tutorial's "Dashing!" makes -- was right up to the dash limit
// and then called everything beyond it a dash as well: the pathfinder builds
// paths far past the limit (costs of 54 against a standard move of 12). The
// limit is the one XGUnit.SetDashing applies: twice the standard move, and
// only while m_iMovesActionsPerformed is 0. MaxPathCost is the pawn's current
// allowance, which is either of those depending on which the path last asked
// for, so it only ever raises the limit.
static uint32_t g_path_off, g_std_off, g_maxcost_off, g_cost_off;
static void*    g_cost_class_path;
static FieldSlot g_gameunit, g_moves;

static int tile_dash(int* cost_out, int* std_out, int* max_out, int* moves_out,
                     int* turns_out)
{
    *cost_out = *std_out = *max_out = *moves_out = -1;
    *turns_out = 0;
    void* pawn = g_path_pawn;
    if (!pawn) return -1;
    if (pawn != g_reach_pawn) {
        if (!object_field_offset(pawn, "Path", &g_path_off) ||
            !object_field_offset(pawn, "StandardMoveLength", &g_std_off) ||
            !object_field_offset(pawn, "MaxPathCost", &g_maxcost_off))
            return -1;
        g_reach_pawn = pawn;
    }
    const uint8_t* p = (const uint8_t*)pawn;
    if (!readable(p + g_path_off, sizeof(void*)) || !readable(p + g_std_off, 4) ||
        !readable(p + g_maxcost_off, 4))
        return -1;
    void* path = *(void**)(p + g_path_off);
    *std_out = *(const int32_t*)(p + g_std_off);
    *max_out = *(const int32_t*)(p + g_maxcost_off);
    if (!path) return -1;
    if (path != g_cost_class_path) {
        if (!object_field_offset(path, "Cost", &g_cost_off)) return -1;
        g_cost_class_path = path;
    }
    if (!readable((const uint8_t*)path + g_cost_off, 4)) return -1;
    *cost_out = *(const int32_t*)((const uint8_t*)path + g_cost_off);
    if (*std_out <= 0 || *cost_out < 0) return -1;

    // Moves already made this turn, off the soldier: ChainedPawn.m_kGameUnit.
    void* soldier = NULL;
    const void* v;
    if (cursor_chained_pawn(&soldier) && soldier &&
        field_ptr(soldier, "m_kGameUnit", &g_gameunit, sizeof(void*), &v)) {
        void* unit = *(void* const*)v;
        if (field_ptr(unit, "m_iMovesActionsPerformed", &g_moves, 4, &v))
            *moves_out = *(const int32_t*)v;
    }
    int limit = *moves_out == 0 ? 2 * *std_out : *std_out;
    if (*max_out > limit) limit = *max_out;

    *turns_out = tile_turns(*cost_out, limit, *std_out);
    if (*cost_out > limit) return 2;
    return *cost_out > *std_out;
}

// ---- who is where ----------------------------------------------------------
//
// Every unit has a flag over its head, and UIUnitFlag.SetNames(unitName,
// unitNickName) arrives through the text hooks once per flag: a soldier's
// surname and nickname, an alien's or civilian's name. The flag also holds
// its unit (UIUnitFlag.m_kUnit, an XGUnit), whose m_kPawn has the Location.
// So the table is kept by flag object, as focus.c keeps its lists, and the
// position is read when it is asked for -- units move, flags do not change.
//
// Only what a sighted player could see is ever said. The flag hides itself
// unless m_kUnit.IsVisible(), and the native IsAliveAndVisible is that test
// with the dead left out, asked of the unit through its vtable like the tile
// queries. A unit it cannot be asked about counts as unseen.
#define UNIT_MAX 64

typedef struct {
    void* flag;
    char  name[64];
    char  nick[64];
} UnitName;

static UnitName  g_units[UNIT_MAX];
static int       g_nunits;
static FieldSlot g_flag_unit, g_unit_pawn, g_pawn_loc;

// A flag that has stopped being one. Its slot is left empty rather than
// closed up, because everything that walks this table walks it by index and
// an empty slot is skipped for nothing -- field_ptr answers a null object
// without reading anything.
static void unit_forget(UnitName* u)
{
    logf_("units: the flag for %s is gone -- dropped\n",
          u->name[0] ? u->name : "someone");
    u->flag = NULL;
    u->name[0] = 0;
    u->nick[0] = 0;
}

static void unit_note(void* flag, const char* name, const char* nick)
{
    if (!flag) return;
    int i, free_slot = -1;
    for (i = 0; i < g_nunits && g_units[i].flag != flag; i++)
        if (!g_units[i].flag && free_slot < 0) free_slot = i;
    if (i == g_nunits) {
        // A mission's worth of flags is dropped as its units die, so the
        // emptied slots are where the next mission's go. Without this a long
        // session would fill the table with the dead and stop noticing the
        // living.
        if (free_slot >= 0) i = free_slot;
        else if (g_nunits < UNIT_MAX) g_nunits++;
        else return;
    }
    g_units[i].flag = flag;
    strncpy_s(g_units[i].name, sizeof g_units[i].name, name, _TRUNCATE);
    strncpy_s(g_units[i].nick, sizeof g_units[i].nick, nick, _TRUNCATE);
}

typedef struct {
    const UnitName* who;
    void*  unit;
    void*  pawn;
    float  loc[3];
    int    friendly;
} UnitSeen;

// The player a unit belongs to (XGUnit.m_kPlayer).
static FieldSlot g_player;

static void* unit_player(void* unit)
{
    const void* v;
    if (!field_ptr(unit, "m_kPlayer", &g_player, sizeof(void*), &v))
        return NULL;
    return *(void* const*)v;
}

// The player the soldier being moved belongs to: ChainedPawn.m_kGameUnit.
static FieldSlot g_squad_unit;

static void* squad_player(void)
{
    void* pawn = NULL;
    const void* v;
    if (!cursor_chained_pawn(&pawn) || !pawn ||
        !field_ptr(pawn, "m_kGameUnit", &g_squad_unit, sizeof(void*), &v))
        return NULL;
    return unit_player(*(void* const*)v);
}

// A flag's unit, if it is alive and in sight: its pawn, where it stands, and
// whether it is on the side of the soldier being moved.
//
// The side is the unit's player, compared with the soldier's. The flag's own
// m_bIsFriendly was the first try and put Chryssalids in the squad: this
// session never found UBoolProperty::BitMask ("no BitMask -- bools read as a
// whole dword"), and that bool shares its dword with m_bIsDead, m_bIsSelected
// and the rest, so any of them set read as friendly.
static int unit_seen(UnitName* u, void* squad, UnitSeen* out)
{
    const void* v;
    if (!u->flag) return 0;
    if (!field_ptr(u->flag, "m_kUnit", &g_flag_unit, sizeof(void*), &v)) {
        // A flag whose class has no m_kUnit is not a flag any more. Flags
        // are destroyed with their units -- eleven Chryssalids and zombies
        // died over one mission, and loading a save replaced the squad's four
        // as well -- and the engine hands the memory straight on, so what is
        // left behind reads as an AudioComponent, or as a class pointer that
        // is not readable at all. The table held sixteen of them, and every
        // pass over it paid a failed class-chain walk for each. Dropping the
        // entry is the answer; the question does not get better with age.
        unit_forget(u);
        return 0;
    }
    void* unit = *(void* const*)v;
    UnitTestFn visible = (UnitTestFn)tile_vfn(unit, g_unit_slot_visible);
    if (!visible || !visible(unit, NULL)) return 0;
    if (!field_ptr(unit, "m_kPawn", &g_unit_pawn, sizeof(void*), &v))
        return 0;
    void* pawn = *(void* const*)v;
    if (!field_ptr(pawn, "Location", &g_pawn_loc, 3 * sizeof(float), &v))
        return 0;
    out->who = u;
    out->unit = unit;
    out->pawn = pawn;
    memcpy(out->loc, v, 3 * sizeof(float));
    out->friendly = squad && unit_player(unit) == squad;
    return 1;
}

// What the squad can see: every enemy in any living squad member's
// XGUnitNativeBase.m_arrVisibleEnemies.
//
// IsAliveAndVisible alone let unrevealed pods through -- the radar listed
// Chryssalids 29 tiles north that no one had met. The game's own minimap
// draws enemies from the active soldier's m_arrVisibleEnemies
// (UITacticalHUD_Radar.UpdateBlips), and targeting from the squad's; the
// union across the squad is what a sighted player could have on screen.
#define SEEN_MAX 128

typedef struct {
    void* unit[SEEN_MAX];
    int   n;
} SeenSet;

static FieldSlot g_visen;

static void squad_sight(void* squad, SeenSet* set)
{
    set->n = 0;
    if (!squad) return;
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || !s.friendly) continue;
        const void* v;
        if (!field_ptr(s.unit, "m_arrVisibleEnemies", &g_visen,
                       sizeof(FArray), &v))
            continue;
        const FArray* a = (const FArray*)v;
        if (a->Num <= 0 || a->Num > SEEN_MAX ||
            !readable(a->Data, (size_t)a->Num * sizeof(void*)))
            continue;
        void* const* e = (void* const*)a->Data;
        for (int k = 0; k < a->Num; k++) {
            int j;
            for (j = 0; j < set->n && set->unit[j] != e[k]; j++) {}
            if (j == set->n && set->n < SEEN_MAX) set->unit[set->n++] = e[k];
        }
    }
}

static int seen_has(const SeenSet* set, const void* unit)
{
    for (int j = 0; j < set->n; j++)
        if (set->unit[j] == unit) return 1;
    return 0;
}

static void unit_label(const UnitName* u, char* out, size_t out_sz)
{
    _snprintf_s(out, out_sz, _TRUNCATE, u->nick[0] ? "%s, %s" : "%s", u->name, u->nick);
}

// "Wright, Disco. Sectoid." -- everyone in sight whose pawn stands on
// (tx, ty). With a floor known, a unit on another storey of the same column
// is left out: a pawn's origin is its middle, about one floor above its feet.
// *mine is set when one of them is the soldier being moved.
static void units_on_tile(int tx, int ty, int have_floor, float floor,
                          char* out, size_t out_sz, int* mine)
{
    size_t used = 0;
    out[0] = 0;
    if (mine) *mine = 0;
    CursorGrid g;
    if (!cursor_grid(&g)) return;
    void* soldier = NULL;
    cursor_chained_pawn(&soldier);
    // Someone not on the squad is named only once the squad has seen them,
    // or stepping onto a hidden alien's tile would give it away.
    void* squad = squad_player();
    static SeenSet sight;
    squad_sight(squad, &sight);
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;
        if (!s.friendly && !seen_has(&sight, s.unit)) continue;
        if (cursor_tile_axis(s.loc[0], g.min_x, CURSOR_TILE) != tx ||
            cursor_tile_axis(s.loc[1], g.min_y, CURSOR_TILE) != ty)
            continue;
        if (have_floor && (s.loc[2] < floor - 32.0f || s.loc[2] > floor + 192.0f)) {
            logf_("tile: %s stands in this column at %.1f, not on floor %.1f\n",
                  g_units[i].name, s.loc[2], floor);
            continue;
        }
        if (mine && s.pawn == soldier) *mine = 1;
        char label[160];
        unit_label(&g_units[i], label, sizeof label);
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s.",
                            used ? " " : "", label);
        if (w < 0) break;
        used += (size_t)w;
    }
}

// Describes tile (tx, ty) with its floor at `floor`. Returns 0 when the game
// could not be asked, leaving `say` empty. `with_dash` is off where the last
// path is not this tile's; `with_who` off where the units were said already.
static int tile_report(int tx, int ty, float floor, int with_dash, int with_who,
                       char* say, size_t say_sz)
{
    say[0] = 0;
    void* world = cursor_world();
    CursorGrid g;
    if (!world || !cursor_grid(&g)) return 0;
    TileCoverFn cover = (TileCoverFn)tile_vfn(world, g_tile_slot_cover);
    if (!cover) return 0;

    // Where XGAction_EndMove asks: the floor under the destination, plus 4.
    float x = g.min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    float y = g.min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    float z = floor + 4.0f;
    TileCoverPoint cp;
    memset(&cp, 0, sizeof cp);
    int has_cover = cover(world, NULL, x, y, z, &cp) != 0;

    // The layer is WORLD_FloorHeight (64) deep, measured from Min.Z as the
    // tile natives do.
    int tz = cursor_tile_axis(z, g.min_z, 64.0f);
    TileTestFn smoke = (TileTestFn)tile_vfn(world, g_tile_slot_smoke);
    TileTestFn poison = (TileTestFn)tile_vfn(world, g_tile_slot_poison);

    TileReport r;
    memset(&r, 0, sizeof r);
    r.cover_flags = has_cover ? cp.flags : 0;
    r.smoke = smoke ? smoke(world, NULL, tx, ty, tz) != 0 : 0;
    r.poison = poison ? poison(world, NULL, tx, ty, tz) != 0 : 0;
    int cost = -1, std = -1, maxc = -1, moves = -1, turns = 0;
    int reach = with_dash ? tile_dash(&cost, &std, &maxc, &moves, &turns) : -1;
    r.dash = reach == 1;
    r.turns = reach == 2 ? turns : 0;

    // Who is standing there comes first: it is what the tile *is*.
    char who[TILE_MAX_TEXT] = "", what[TILE_MAX_TEXT];
    if (with_who) units_on_tile(tx, ty, 1, floor, who, sizeof who, NULL);
    tile_describe(&r, what, sizeof what);
    _snprintf_s(say, say_sz, _TRUNCATE, "%s%s%s", who, who[0] ? " " : "", what);

    // The cover point carries its own tile, which is the check on the one
    // asked about -- and on the layer this file worked out for smoke.
    logf_("tile: %d, %d floor %.1f (layer %d): cover %s flags 0x%05X at %d, %d, %d; "
          "path cost %d, standard move %d, max %d, moves made %d, turns %d, smoke %d, "
          "poison %d -> \"%s\"\n",
          tx, ty, floor, tz, has_cover ? "yes" : "no", (unsigned)cp.flags,
          cp.x, cp.y, cp.z, cost, std, maxc, moves, turns, r.smoke, r.poison, say);
    return 1;
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
// The tile listened from is the navigation target while one is held and the
// cursor's own tile otherwise, so the walls are alive under the mouse as well
// as under the numpad. A scan is two questions of the game about each of the
// (2 * SONAR_RANGE + 1)^2 tiles in range, so it is rescanned when that tile
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

    if (!g_walls_on || !audio_available()) return;
    if (!cursor_grid(&g)) { walls_quiet(); return; }

    if (nav_active() && nav_target(&tx, &ty)) {
        floor = navh_ground();
    } else {
        float z;
        if (!cursor_tile(&g, &tx, &ty, &z)) { walls_quiet(); return; }
        floor = z - NAV_CURSOR_LIFT;
    }

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

// Says the pending step: anyone standing there, `body`, then the
// coordinates. Once per step.
static int nav_step_say(const char* body)
{
    if (!g_step_pending) return 0;
    g_step_pending = 0;
    char say[TILE_MAX_TEXT + TILE_MAX_TEXT + NAV_MAX_TEXT];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s.", g_step_who,
                g_step_who[0] && body[0] ? " " : "", body,
                g_step_who[0] || body[0] ? " " : "", g_step_coords);
    logf_("nav: said \"%s\"\n", say);
    speech_say_now(say);
    return 1;
}

// A tile no path reaches. A unit standing on it is the likeliest reason, and
// worth more than the verdict: when one was found on arrival, its name is the
// whole answer.
static void nav_say_no_path(int tx, int ty)
{
    logf_("nav: %d, %d unreachable%s\n", tx, ty, g_step_who[0] ? " -- occupied" : "");
    nav_step_say(g_step_who[0] ? "" : "No path.");
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

    static char labels[UNIT_MAX][160];
    TileContact c[UNIT_MAX];
    int n = 0;
    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s) || s.friendly != friendly ||
            (from_soldier && s.pawn == soldier) ||
            (!friendly && !seen_has(&sight, s.unit)))
            continue;
        unit_label(&g_units[i], labels[n], sizeof labels[n]);
        c[n].name = labels[n];
        c[n].dx = cursor_tile_axis(s.loc[0], g.min_x, CURSOR_TILE) - ox;
        c[n].dy = cursor_tile_axis(s.loc[1], g.min_y, CURSOR_TILE) - oy;
        n++;
    }
    static char say[2048];
    tile_contacts(c, n,
                  !friendly ? "No enemies in sight." :
                  from_soldier ? "No one else in the squad." : "No squad in sight.",
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

    static const struct { const char* name; int* slot; } want[] = {
        { "UXComWorldDataexecGetCoverPoint",         &g_tile_slot_cover },
        { "UXComWorldDataexecTileContainsSmoke",     &g_tile_slot_smoke },
        { "UXComWorldDataexecTileContainsPoison",    &g_tile_slot_poison },
        { "UXComWorldDataexecIsTileOccupied",        &g_tile_slot_occupied },
        { "AXGUnitNativeBaseexecIsAliveAndVisible",  &g_unit_slot_visible },
        { "AXCom3DCursorexecWorldZToCursorFloor",    &g_cursor_slot_floor },
    };
    for (int i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
        const uint8_t* code = (const uint8_t*)natives_find(tbl, n, want[i].name);
        *want[i].slot = code && readable(code, 0x180) ? tile_vtable_slot(code, 0x180) : -1;
        if (*want[i].slot < 0) logf_("  %-38s vtable slot NOT FOUND\n", want[i].name);
        else                   logf_("  %-38s vtable +0x%X\n", want[i].name, *want[i].slot);
    }
}

// Defined with the pick, below: the interface lent to the mouse's own pick.
static void nav_forget_interface(void);

static void nav_stop(const char* why)
{
    if (!nav_active()) return;
    nav_end();
    nav_forget_interface();
    // The field is not silenced. With no target held it simply goes back to
    // listening from wherever the cursor is, which is where the mouse has
    // just put it -- the walls are still there, and the player has not
    // stopped needing to hear them.
    g_nav_live = 0;
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
static void nav_park_mouse(void)
{
    HWND w = GetForegroundWindow();
    RECT r;
    if (!w || !GetClientRect(w, &r)) return;
    POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
    if (!ClientToScreen(w, &c)) return;
    SetCursorPos(c.x, c.y);
    GetCursorPos(&g_nav_mouse);
    g_nav_parked = 1;
    logf_("nav: no placement since the key; mouse moved to the window centre "
          "(%ld, %ld)\n", c.x, c.y);
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
#define NAV_GLIDE_MS      110   // the first repeat
#define NAV_GLIDE_FAST_MS  50   // where it settles
#define NAV_GLIDE_RAMP     10   // repeats spent getting there

static int       g_glide_digit;      // the direction being held, 0 for none
static int       g_glide_steps;      // repeats taken, for the ramp
static ULONGLONG g_glide_next;       // when the next step is due
static ULONGLONG g_numpad_at[10];    // when each key last went down

static int glide_interval(int steps)
{
    if (steps >= NAV_GLIDE_RAMP) return NAV_GLIDE_FAST_MS;
    return NAV_GLIDE_MS +
           (NAV_GLIDE_FAST_MS - NAV_GLIDE_MS) * steps / NAV_GLIDE_RAMP;
}

// What a step arrives to: who is standing on the tile, the floor under it, the
// path the game builds to it, and in the end the announcement. Skipped on
// every step of a glide and run once on the tile it stops on.
static void nav_arrive(int tx, int ty)
{
    g_tile_due = 0;
    navh_begin_tile();
    g_nav_path_tile[0] = tx;
    g_nav_path_tile[1] = ty;
    g_nav_phase_logged = (NavHeightPhase)-1;

    // Who stands there is found now, on arrival, and not from the tile's
    // verdict: another unit's tile gets only "No path", and the soldier's
    // own tile gets no verdict at all -- the game builds no path to
    // within 64 units of the soldier (XGAction_Path.DoPathingTick).
    int mine = 0;
    Fault f;
    __try { units_on_tile(tx, ty, 0, 0.0f, g_step_who, sizeof g_step_who, &mine); }
    __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("tile: units", &f, NULL);
        g_step_who[0] = 0;
    }
    nav_describe(tx, ty, g_step_coords, sizeof g_step_coords);
    g_step_late = 0;
    g_step_pending = 1;
    g_step_deadline = GetTickCount64() + STEP_FALLBACK_MS;
    g_path_calls = 0;
    // With no verdict coming for the soldier's own tile, its cover is
    // described on a timer instead, once the floor search has settled.
    if (mine) {
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

    // Until a step has been taken, "here" is the soldier, not the mouse.
    if (!nav_active()) {
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
        char say[NAV_MAX_TEXT + TILE_MAX_TEXT];
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
        _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s.", what, what[0] ? " " : "", coords);
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
        logf_("nav: begins on %d, %d, ground estimate %.1f\n", tx, ty, navh_ground());
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
    g_nav_world[0] = g.min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    g_nav_world[1] = g.min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    g_nav_world[2] = navh_query_z();
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
//   units   the flag table above (g_units). A flag exists for every soldier,
//           alien and civilian, and unit_seen has already settled whether it
//           can be seen at all. The side comes from the unit's own m_eTeam
//           rather than from the flag: eTeam_Neutral is a civilian, and the
//           flag's m_bIsFriendly shares a dword with m_bIsDead in this build.
//
//   world   the game's object table (objects.h). Doors, windows, panels,
//           ladders, Meld canisters and the radar array are level actors that
//           never pass through the UI, and no native lists them:
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

// XComInteractiveLevelActor.IconSocket: how the level designer classified it.
// XGDOOR_Icon 0, XGWINDOW_Icon 1, XGBUTTON_Icon 2. Read rather than asking the
// native IsDoor(), because this is a plain byte on the actor and calling into
// script is something this DLL does not do.
#define ICON_WINDOW  1
#define ICON_BUTTON  2

// COVER_ClimbOnto_N..W and COVER_ClimbOver_N..W, from XComWorldData.
#define COVER_CLIMB_ONTO 0x001E0000
#define COVER_CLIMB_OVER 0x01E00000

#define SCAN_CLIMB_RADIUS 12    // tiles each way the climb scan covers
#define SCAN_CLIMB_APART   4    // tiles between two climbs worth naming apart
#define TEAM_NEUTRAL      1     // Object.ETeam.eTeam_Neutral -- a civilian

static FieldSlot g_icon, g_ladder_loc, g_ilact_loc;
static FieldSlot g_meld_loc, g_meld_turns, g_team;

// Where the scan was measured from, and the grid it was taken on, so Home and
// End answer about the same scan the player has just heard.
static CursorGrid g_scan_grid;
static int        g_scan_from[3];
static float      g_scan_world_z;   // the origin's own height, for tile queries
static int        g_scan_have;

// A unit's team, from XGUnitNativeBase.m_eTeam (Object.ETeam, one byte).
static int unit_team(void* unit)
{
    const void* v;
    if (!field_ptr(unit, "m_eTeam", &g_team, 1, &v)) return 0;
    return *(const uint8_t*)v;
}

// An actor's Location, through the same field walk everything else uses.
static int actor_location(void* actor, FieldSlot* slot, float* out)
{
    const void* v;
    if (!field_ptr(actor, "Location", slot, 3 * sizeof(float), &v)) return 0;
    memcpy(out, v, 3 * sizeof(float));
    return 1;
}

// ---- what a floor is -------------------------------------------------------
//
// Not a row of the world grid. XComWorldData steps its Z axis by
// WORLD_FloorHeight, 64 units, and a map 18 of those tall was announced as
// having eighteen floors -- on a building with two. A *floor*, as the game and
// the player mean it, is XCom3DCursor.CURSOR_OUTDOOR_FLOOR_HEIGHT: 192 units,
// three grid rows. The first run had a soldier on 259.1 and another on 533.4
// called five floors apart; they are one.
//
// The game will answer this itself -- WorldZToCursorFloor is native on the
// cursor, and takes the whole position, so it can tell an indoor floor from
// the ground outside it. That is the answer used. The division is only the
// fallback for a build where the thunk does not have the shape tile_vtable_slot
// reads, and it is the same division the constant describes.
#define CURSOR_FLOOR_HEIGHT 192.0f

static int floor_of(const float* world)
{
    void* cur = cursor_object();
    CursorFloorFn fn = (CursorFloorFn)tile_vfn(cur, g_cursor_slot_floor);
    if (fn) return fn(cur, NULL, world[0], world[1], world[2]);
    CursorGrid g;
    if (!cursor_grid(&g)) return 0;
    return cursor_tile_axis(world[2], g.min_z, CURSOR_FLOOR_HEIGHT);
}

// How many floors the map has. XCom3DCursorForCursorVolumes works m_iMaxFloor
// out from the cursor volumes the level was built with, so it is the map's own
// count; the grid's height in floors is the fallback.
static FieldSlot g_maxfloor;

static int floor_count(void)
{
    const void* v;
    void* cur = cursor_object();
    if (cur && field_ptr(cur, "m_iMaxFloor", &g_maxfloor,
                         sizeof(int32_t), &v)) {
        int n = *(const int32_t*)v + 1;         // m_iMaxFloor is the top index
        if (n > 0 && n < 64) return n;
    }
    CursorGrid g;
    if (!cursor_grid(&g)) return 1;
    int n = (int)((float)g.num_z * 64.0f / CURSOR_FLOOR_HEIGHT);
    return n > 0 ? n : 1;
}

// Fills in an item's tile from its world position. 0 when the grid is unknown.
//
// `lift` is how far the position stands above the floor it is on. A pawn's
// Location is its middle, NAV_CURSOR_LIFT above its feet, and scan_origin
// takes that off the tile the scan is measured from -- so an item that does
// not take it off too reads one storey high. The first run said "Payne, here,
// one floor up" about the very soldier the scan was measured from. A level
// actor's Location is already at its base, so it passes 0.
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

static void scan_add_units(void)
{
    void* squad = scan_squad_player();
    static SeenSet sight;
    squad_sight(squad, &sight);

    for (int i = 0; i < g_nunits; i++) {
        UnitSeen s;
        if (!unit_seen(&g_units[i], squad, &s)) continue;

        ScanItem it;
        memset(&it, 0, sizeof it);
        if (s.friendly) {
            it.kind = SCAN_SQUAD;
        } else if (unit_team(s.unit) == TEAM_NEUTRAL) {
            // A civilian is on nobody's side, so no one holds them in
            // m_arrVisibleEnemies. IsAliveAndVisible -- which unit_seen has
            // already asked -- is the whole gate, the same one the squad gets.
            it.kind = SCAN_CIVILIANS;
        } else {
            // Aliens go through the squad's own sight, as the radar does:
            // IsAliveAndVisible alone let unrevealed pods through.
            if (!seen_has(&sight, s.unit)) continue;
            it.kind = SCAN_ENEMIES;
        }
        unit_label(&g_units[i], it.name, sizeof it.name);
        if (scan_item_at(&it, s.loc, NAV_CURSOR_LIFT)) scan_add(&it);
    }
}

// ---- the level actors, kept between key presses ----------------------------
//
// A full walk of the object table is 175,000 entries and tens of milliseconds
// -- one run measured 78 ms, five frames, with the game thread stopped for
// all of it -- and doing one per key press was felt as lag. The first attempt
// at that was a three-second cache, which only moved the stall around: every
// press more than three seconds after the last one paid for it again, which
// is most presses.
//
// So the walk is done once per mission and then kept up to date instead. What
// is kept is the *actors*, not the finished items: what the scanner says
// about one -- its tile, and a Meld canister's countdown -- is worked out
// again on every press, so nothing here is ever stale.
//
// Keeping up to date is two cheap things. Actors that have gone are dropped,
// because a door can be blown off its hinges and a canister can expire, and
// objects_still asks the table rather than trusting the pointer. Then the
// walk resumes where it stopped, over whatever the mission has added since,
// which is usually nothing. A different cursor or a different grid is a
// different map, and starts again from the beginning.
typedef struct {
    void* actor;
    int   idx;      // its slot in the object table, for objects_still
    int   kind;     // 0 interactive, 1 ladder, 2 Meld canister
} WorldActor;

static WorldActor g_wactors[SCAN_MAX];
static int        g_wactor_n;
static int        g_world_next;     // where the last walk stopped
static int        g_world_have;
static void*      g_world_cursor;
static CursorGrid g_world_grid;

static ScanItem   g_world[SCAN_MAX];
static int        g_world_n;

static void world_keep(const ScanItem* it)
{
    if (g_world_n < SCAN_MAX) g_world[g_world_n++] = *it;
}


static void scan_describe_interactive(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);

    const void* v;
    int icon = 0;
    if (field_ptr(actor, "IconSocket", &g_icon, 1, &v))
        icon = *(const uint8_t*)v;

    // The radar array is the objective on the missions that have one, and it
    // is an interactive actor like any other -- so it is named and filed
    // before the icon gets a say.
    if (object_is_a(actor, "XComRadarArrayActor")) {
        it.kind = SCAN_OBJECTIVES;
        strncpy_s(it.name, sizeof it.name, "Radar array", _TRUNCATE);
    } else if (icon == ICON_WINDOW) {
        it.kind = SCAN_INTERACT;
        strncpy_s(it.name, sizeof it.name, "Window", _TRUNCATE);
    } else if (icon == ICON_BUTTON) {
        it.kind = SCAN_INTERACT;
        strncpy_s(it.name, sizeof it.name, "Panel", _TRUNCATE);
    } else {
        it.kind = SCAN_DOORS;
        strncpy_s(it.name, sizeof it.name, "Door", _TRUNCATE);
    }

    float world[3];
    if (!actor_location(actor, &g_ilact_loc, world)) return;
    if (scan_item_at(&it, world, 0.0f)) world_keep(&it);
}

static void scan_describe_ladder(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_INTERACT;
    strncpy_s(it.name, sizeof it.name, "Ladder", _TRUNCATE);
    float world[3];
    if (!actor_location(actor, &g_ladder_loc, world)) return;
    if (scan_item_at(&it, world, 0.0f)) world_keep(&it);
}

static void scan_describe_meld(void* actor)
{
    ScanItem it;
    memset(&it, 0, sizeof it);
    it.kind = SCAN_OBJECTIVES;

    // How long it lasts is the whole decision about a canister, and it is an
    // int -- unlike m_bCollected, which is a bool, and a bool in this build
    // shares its dword with its neighbours (props: "no BitMask"). So a
    // collected canister is not filtered out; one whose timer has run out is.
    const void* v;
    int turns = -1;
    if (field_ptr(actor, "m_iTurnsUntilDestroyed", &g_meld_turns, sizeof(int32_t), &v))
        turns = *(const int32_t*)v;

    if (turns == 0) return;                     // its timer has run out
    if (turns > 0)
        _snprintf_s(it.name, sizeof it.name, _TRUNCATE,
                    "Meld canister, %d turn%s left", turns, turns == 1 ? "" : "s");
    else
        strncpy_s(it.name, sizeof it.name, "Meld canister", _TRUNCATE);

    float world[3];
    if (!actor_location(actor, &g_meld_loc, world)) return;
    if (scan_item_at(&it, world, 0.0f)) world_keep(&it);
}

// What the scanner would say about each actor it is holding, worked out
// afresh: the tiles are relative to a grid, and a canister's countdown is
// relative to the turn.
static void scan_world_items(void)
{
    g_world_n = 0;
    for (int i = 0; i < g_wactor_n; i++) {
        switch (g_wactors[i].kind) {
        case 0:  scan_describe_interactive(g_wactors[i].actor); break;
        case 1:  scan_describe_ladder(g_wactors[i].actor);      break;
        default: scan_describe_meld(g_wactors[i].actor);        break;
        }
    }
}

// The walk hands back an index into the class list it was given; this carries
// the mapping across it, since a visitor gets no state of its own beyond ctx
// and this keeps the call cheap.
static const int* g_scan_kinds;

static int scan_collect_world(void* actor, int which, int idx, void* ctx)
{
    (void)ctx;
    if (g_wactor_n >= SCAN_MAX) return 0;
    g_wactors[g_wactor_n].actor = actor;
    g_wactors[g_wactor_n].idx   = idx;
    g_wactors[g_wactor_n].kind  = g_scan_kinds[which];
    g_wactor_n++;
    return 1;
}

// The three classes, resolved together, because finding a class by name costs
// a pass over the whole table. After the first scan of a mission they come
// from the cache. `map` receives the kind each entry of `use` stands for.
static int scan_world_classes(const void** use, int* map)
{
    static const char* const names[] = {
        "XComInteractiveLevelActor", "XComLadder", "XComMeldContainerActor",
    };
    const void* cls[3];
    objects_classes(names, cls, 3);

    int n = 0;
    for (int i = 0; i < 3; i++)
        if (cls[i]) { use[n] = cls[i]; map[n] = i; n++; }
    return n;
}

static void scan_world_full(const void* const* use, int nclasses)
{
    g_wactor_n = 0;
    g_world_next = 0;
    if (objects_each_from(use, nclasses, 0, &g_world_next,
                          scan_collect_world, NULL) < 0) {
        logf_("scan: the object table could not be read this press\n");
        return;
    }

    unsigned ms;
    int entries;
    objects_last_walk(&ms, &entries);
    logf_("scan: full object walk %d entries in %u ms, %d actors kept\n",
          entries, ms, g_wactor_n);
}

static void scan_world_catch_up(const void* const* use, int nclasses)
{
    int had = g_wactor_n;

    int keep = 0;
    for (int i = 0; i < g_wactor_n; i++)
        if (objects_still(g_wactors[i].actor, g_wactors[i].idx, use, nclasses))
            g_wactors[keep++] = g_wactors[i];
    g_wactor_n = keep;

    int was = g_world_next;
    objects_each_from(use, nclasses, g_world_next, &g_world_next,
                      scan_collect_world, NULL);

    // Silent when nothing has changed, which is the usual answer and the
    // whole point of not walking the table again.
    int gone = had - keep, found = g_wactor_n - keep;
    if (gone || found)
        logf_("scan: %d gone, %d new over %d entries added since\n",
              gone, found, g_world_next - was);
}

static void scan_add_world(void)
{
    g_world_n = 0;
    if (!objects_ready()) {
        // The probe at startup runs while the game is still in its shell,
        // where the object table can be too small to recognise. In a mission
        // it is not, so it is worth another look -- and the answer is logged
        // whichever way it goes, once.
        char why[256];
        int got = objects_retry(why, sizeof why);
        if (why[0]) logf_("objects: %s%s\n", got ? "" : "still unavailable -- ", why);
        if (!got) return;
    }

    const void* use[3];
    int map[3];
    int nclasses = scan_world_classes(use, map);
    if (!nclasses) return;
    g_scan_kinds = map;

    // A different cursor, or a different grid, is a different map: what was
    // found last time belongs to one that is gone. So is a table that has
    // shrunk below where the last walk stopped -- it cannot be the table
    // those indices were taken from, and resuming into it would count
    // everything a second time.
    int same_map = g_world_have && g_world_cursor == cursor_object() &&
                   memcmp(&g_world_grid, &g_scan_grid, sizeof g_world_grid) == 0 &&
                   g_world_next <= objects_count();
    if (same_map) scan_world_catch_up(use, nclasses);
    else          scan_world_full(use, nclasses);

    g_world_have = 1;
    g_world_cursor = cursor_object();
    g_world_grid = g_scan_grid;

    scan_world_items();
    for (int i = 0; i < g_world_n; i++) scan_add(&g_world[i]);
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
    if (soldier_tile(g, tx, ty, &z) || cursor_tile(g, tx, ty, &z)) {
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
    return 0;
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
    if (c == SCAN_ALL || c == SCAN_DOORS || c == SCAN_OBJECTIVES || c == SCAN_INTERACT)
        scan_add_world();
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
    nav_arrive(tx, ty);

    g_nav_world[0] = g.min_x + ((float)tx + 0.5f) * CURSOR_TILE;
    g_nav_world[1] = g.min_y + ((float)ty + 0.5f) * CURSOR_TILE;
    g_nav_world[2] = navh_query_z();
    g_nav_live = 1;
    g_nav_key_at = GetTickCount64();
    logf_("scan: cursor to %s on %d, %d, ground %.1f\n", what, tx, ty, ground);
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
    // Measured afresh from where the soldier is now, not from where the scan
    // was taken: the point of the key is to ask again after moving.
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
                if (!nav_step_say(what) && g_step_late && what[0] &&
                    tx == g_step_late_at[0] && ty == g_step_late_at[1]) {
                    g_step_late = 0;
                    logf_("nav: %d, %d described late -- \"%s\"\n", tx, ty, what);
                    speech_say_now(what);
                }
            }
        } else if (navh_poll(GetTickCount64()) == NAVH_NO_PATH) {
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
        g_walls_on = !g_walls_on;
        if (!g_walls_on) walls_quiet();
        logf_("walls: field %s\n", g_walls_on ? "on" : "off");
        speech_say_now(g_walls_on ? "Wall sound on." : "Wall sound off.");
    }
    g_walls_down = walls;

    // The scanner: Page Up, Page Down, Home, End. Read here rather than in a
    // poll of its own so it shares the guards this one already applies --
    // practice has taken the keys, the game has the foreground.
    scan_poll();

    // Last, so a step taken this frame is already in the target the field
    // listens from.
    walls_poll();
}

static void cursor_watch(void* self)
{
    cursor_seen(self);

    // Every frame, not at the watch's four times a second: a key press lasts
    // a few frames, and a quarter-second poll would drop quick ones.
    if (cursor_resolved()) nav_poll();

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
    if (alive) {
        slot[0] = g_pick_iface_seen[0];
        slot[1] = g_pick_iface_seen[1];
        g_pick_iface_lent = slot;
    } else {
        g_pick_iface_seen[0] = NULL;
        g_pick_iface_seen[1] = NULL;
    }
    if (g_pick_iface_state != 0) {
        g_pick_iface_state = 0;
        logf_("nav: the mouse picks nothing -- %s\n",
              alive ? "lending back the last actor it hit" :
                      "none to lend, the game will refuse the placement");
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
    v[2] = navh_query_z();
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
        if (aimed) {
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
    if (dest && tx == g_nav_path_tile[0] && ty == g_nav_path_tile[1]) {
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
        swallow = input_is_our_end(input_event_cmd(stack, &mask));
        if (swallow && !g_end_swallowed) {
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

    // The wall field's mixer. A machine with no output device loses the field
    // and keeps everything else, so this is reported and never fatal.
    char audio_why[256];
    audio_start(audio_why, sizeof audio_why);
    logf_("audio: %s\n", audio_why);

    // Sound practice, on a thread of its own so that it works at the main menu
    // and not only in a mission. It uses the mixer and speech, so it starts
    // after both.
    char learn_why[256];
    learn_start(learn_why, sizeof learn_why);
    logf_("practice: %s\n", learn_why);

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

    // Where numpad navigation puts its target in front of the game: the
    // position, the ground under it, and a view of what a confirm did.
    int nav_armed = arm(tbl, n, mod, "UXComWorldDataexecGetClosestValidCursorPosition",
                        (LPVOID)hook_validpos, (LPVOID*)&g_orig_validpos);
    nav_armed &= arm(tbl, n, mod, "UXComWorldDataexecGetFloorZForPosition",
                     (LPVOID)hook_floorz, (LPVOID*)&g_orig_floorz);
    nav_armed &= arm(tbl, n, mod, "UXComEngineexecIsAnyMoviePlaying",
                     (LPVOID)hook_moviecheck, (LPVOID*)&g_orig_moviecheck);
    nav_armed &= arm(tbl, n, mod, "UXComInputBaseexecTestHitPointToFlash",
                     (LPVOID)hook_flashhit, (LPVOID*)&g_orig_flashhit);
    nav_armed &= arm(tbl, n, mod, "AXComPathingPawnexecComputePath2",
                     (LPVOID)hook_computepath, (LPVOID*)&g_orig_computepath);

    // Not hooks: the implementations behind three world-data natives, called
    // to say what is on a tile.
    tile_arm(tbl, n, mod);

    free(tbl);
    if (!armed) { logf_("FATAL: nothing armed\n"); return 1; }

    logf_("%d/3 text hooks armed, key remap %s, cursor watch %s, grid %s, nav %s,"
          " scanner %s -- navigate the UI to produce traffic\n---\n",
          armed, input_armed ? "on" : "OFF", cursor_armed ? "on" : "OFF",
          grid_armed ? "on" : "OFF", nav_armed ? "on" : "OFF",
          nav_armed ? "on" : "OFF");

    // Said aloud, because the log is the one part of this mod its user cannot
    // read.  Now that the launcher attaches during startup rather than on
    // request, this is the only sign that anything happened at all.
    speech_say(armed == 3 && input_armed
               ? "Accessibility mod ready."
               : "Accessibility mod loaded with errors. Check the log.");

    // The object table, for the scanner: doors, ladders and the Meld are level
    // actors that nothing else in this DLL would ever see. Last, and after the
    // banner, because nothing the arming does not need belongs in front of it:
    // the launcher waits ten seconds for that line and then tells the player
    // the mod did not arm. An early version of this probe took longer than
    // that, and the report was "attached but did not arm" about a mod that had
    // armed perfectly. Not a failure if it is missing either -- the scanner
    // still has the units, and scan_add_world asks again in a mission.
    if (objects_init(mod, why, sizeof why))
        logf_("objects: %s\n", why);
    else
        logf_("objects: UNAVAILABLE (%s) -- the scanner will have no doors\n", why);
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
