// The turn counters a mission puts top right (counters.h).
//
// One class draws all of them, UISpecialMissionHUD_TurnCounter, for three
// owners:
//   - a mission script's own timer: SeqAct_DisplayUISpecialMissionTimer runs
//     XComPresentationLayer.UITimerMessage(label, turns, colour, show), which
//     sets the HUD's m_kGenericTurnCounter and shows or hides it. The
//     2026-10-01 (13:56) log had it as "Turns until Air Strike:" / "8" on the
//     mission after the ship's transponder, and nothing said it;
//   - UISpecialMissionHUD_CapturePointStats, one per XComCapturePointVolume:
//     "ENCODER" / "TRANSMITTER", a sub-label ("Hack in Progress", "Lost"),
//     and TurnsUntilCaptured;
//   - UISpecialMissionHUD_MeldStats, one per canister: "MELD", "RECOVERABLE",
//     the turns until it is destroyed. The scanner's Meld category already
//     asks the canisters themselves, so these are marked and left to it there.
//
// Each Set* keeps its text in the panel (m_sLabel, m_sSubLabel, m_sCounter,
// wrapped in GetHTMLColoredText's font tag) as well as sending it to Flash,
// and Hide / Show set b_IsVisible -- so what is on screen is read off the
// panel when asked, rather than pieced together from the calls. The panels
// are found from the calls themselves: every one draws (OnInit re-sends all
// of its text, so a loaded save does too), and main.c hands each to
// counters_note.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "counters.h"
#include "game.h"
#include "strings.h"
#include "log.h"
#include "cursor.h"
#include "props.h"
#include "menus.h"
#include "ue3.h"

static void*     g_panels[COUNTERS_MAX];
static int       g_npanels;
static FieldSlot g_f_label, g_f_sub, g_f_counter;

void counters_note(void* panel)
{
    for (int i = 0; i < g_npanels; i++)
        if (g_panels[i] == panel) return;
    // A panel gone with its mission makes room; the list is short.
    int w = 0;
    for (int i = 0; i < g_npanels; i++)
        if (unit_is_live(g_panels[i])) g_panels[w++] = g_panels[i];
    g_npanels = w;
    if (g_npanels == COUNTERS_MAX) return;
    g_panels[g_npanels++] = panel;
    logf_("counters: panel %p is counter %d\n", panel, g_npanels);
}

// A bool of the panel's, 0 when it cannot be read.
static int panel_bool(void* panel, const char* name)
{
    if (!props_mask_offset()) return 0;
    const void* prop = object_field_prop(panel, name);
    int b = 0;
    if (!prop || !props_read_object_bool(prop, (const uint8_t*)panel, &b)) return 0;
    return b != 0;
}

// One of the panel's strings, markup out, the label's colon off, and a word
// in capitals ("ENCODER", "HACK IN PROGRESS") put in lower case after its
// first letter, which a speech synthesiser can otherwise spell out.
static void panel_text(void* panel, const char* name, FieldSlot* slot, char* out, size_t out_sz)
{
    // Read whole, then stripped, then cut to size: the colour tag is longer
    // than the number it wraps, and the first run read m_sCounter into 16
    // bytes -- "Turns until Air Strike, <font color='#E." -- a tag cut off
    // before its '>' that strip_markup could not know for one.
    char raw[512];
    const void* v;
    out[0] = 0;
    if (!field_ptr(panel, name, slot, sizeof(FString), &v) ||
        !read_fstring((const FString*)v, raw, sizeof raw))
        return;
    strip_markup(raw);
    size_t n = strlen(raw);
    while (n && (raw[n - 1] == ' ' || raw[n - 1] == ':')) raw[--n] = 0;
    // In the game's language: not ASCII, so through text_*.
    if (text_has_lower(raw)) strncpy_s(out, out_sz, raw, _TRUNCATE);
    else text_sentence_case(raw, out, out_sz);
}

static int read_one(void* panel, Counter* c)
{
    if (!unit_is_live(panel) || !object_is_a(panel, "UISpecialMissionHUD_TurnCounter"))
        return 0;
    if (!panel_bool(panel, "b_IsVisible")) return 0;
    char sub[64], count[16];
    panel_text(panel, "m_sLabel", &g_f_label, c->label, sizeof c->label);
    panel_text(panel, "m_sSubLabel", &g_f_sub, sub, sizeof sub);
    panel_text(panel, "m_sCounter", &g_f_counter, count, sizeof count);
    if (!c->label[0] && !count[0]) return 0;
    // MeldStats' m_strMeldLabel, with the canister's icon after it -- read
    // in the game's language ("MELD" in English), not matched as English.
    char meld[64];
    if (!game_loc("UISpecialMissionHUD_MeldStats", "m_strMeldLabel", 0, meld, sizeof meld))
        strcpy_s(meld, sizeof meld, "Meld");
    c->meld = text_find_ci(c->label, meld) == c->label;
    // An expired counter keeps its last number under the sub-label that says
    // why ("Lost", "Collected"); the Meld tutorial's shows the infinity sign.
    const char* value = count;
    if (panel_bool(panel, "m_bExpired")) value = sub[0] ? "" : T(COUNTER_EXPIRED);
    else if (panel_bool(panel, "m_bInfinity")) value = T(COUNTER_NO_LIMIT);
    _snprintf_s(c->detail, sizeof c->detail, _TRUNCATE, "%s%s%s", sub,
                sub[0] && value[0] ? ", " : "", value);
    return 1;
}

int counters_read(Counter* out, int max)
{
    int n = 0;
    for (int i = 0; i < g_npanels && n < max; i++) {
        int ok = 0;
        GUARDED("counters: read", ok = read_one(g_panels[i], &out[n]), ok = 0);
        if (ok) n++;
    }
    return n;
}

void counters_text(char* out, size_t out_sz)
{
    Counter c[COUNTERS_MAX];
    int n = counters_read(c, COUNTERS_MAX);
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        int w = _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s%s%s%s.", used ? " " : "",
                            c[i].label, c[i].label[0] && c[i].detail[0] ? ", " : "",
                            c[i].detail);
        if (w < 0) break;
        used += (size_t)w;
    }
    static char said[256];
    if (strncmp(said, out, sizeof said - 1) != 0) {
        strncpy_s(said, sizeof said, out, _TRUNCATE);
        logf_("counters: %d of %d on screen -> \"%s\"\n", n, g_npanels, out);
    }
}
