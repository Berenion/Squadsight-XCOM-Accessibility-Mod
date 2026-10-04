// The soldier customisation screen, in words. See customize.h.
//
// The whole workflow, from the decompiled script (EW):
//
//   UISoldierCustomize.OnUnrealCommand -> OnSpinnerIncrease / Decrease
//     -> XGCustomizeUI.OnInputPadRight / Left -> AdvanceFeature(widget, dir)
//     -> AdvanceRace / AdvanceHairColor / ...: m_kPawn.SetX(new index), and
//        the soldier's kAppearance updated to match
//     -> UpdateView -> UISoldierCustomize.UpdateData: XGCustomizeUI
//        .UpdateMainMenu builds each spinner's strHelp as ("" $ index + 1),
//        then UIWidgetHelper.RefreshAllWidgets -> RefreshSpinner
//        -> SetSpinnerLabel(Index, title), SetSpinnerValue(Index, strHelp)
//
// So by the time SetSpinnerValue reaches Flash the pawn already holds the new
// appearance, and the description is read straight off it. The spinner
// indices are XGCustomizeUI's own SPINNER_* constants: three name buttons
// take 0-2, the spinners follow from 3.
//
// What each number indexes:
//   race          XComHumanPawn.m_kAppearance.iRace, ECharacterRace
//                 (Caucasian, African, Asian, Hispanic -- the game has no
//                 localised names for them; the spinner shows 1-4)
//   head          the place of iHead in PossibleHeads
//   skin colour   iSkinColor, an entry of the head's skin palette:
//                 HeadContent.SkinPalette names one of CaucasianSkin,
//                 AfricanSkin, HispanicSkin, AsianSkin (UpdateSkinMaterial)
//   hair          the place of iHaircut in PossibleHairs, shifted by one:
//                 iHaircut -1 is no hair, shown as 1 (AdvanceHair runs from
//                 -1, UpdateMainMenu adds 2)
//   hair colour   iHairColor, an entry of the HairColor palette -- always
//                 that one: UpdateHairMaterial asks GetColorPalette(0)
//   facial hair   iFacialHair, an index into XComContentManager
//                 .FacialHairPresets, whose first entry has an empty mask
//                 (DefaultContent.ini: MaskARGB=0xFF000000) -- no beard
//   armour deco   the place of iArmorDeco in PossibleArmorKits, whose first
//                 entry is always -1, the armour as issued
//                 (XComHumanPawn.UpdatePossibleContent)
//   armour tint   iArmorTint, an entry of the ArmorTint palette; -1 is the
//                 armour's own colour, which the game already calls Standard
//
// The palettes are XComLinearColorPalette archetypes ("UnitPalettes.HairColor"
// and so on, DefaultContent.ini ColorPaletteInfo), found by name in the object
// table once and kept while they live. GetColorPalette is a native the script
// calls with a frame of its own, so the table walk is the plainer way in.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "customize.h"
#include "colors.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "names.h"
#include "objects.h"
#include "ue3.h"

// XGCustomizeUI's SPINNER_* constants.
#define W_RACE       5
#define W_HEAD       6
#define W_SKIN       7
#define W_HAIR       8
#define W_HAIRCOLOR  9
#define W_FACIAL    10
#define W_DECO      11
#define W_TINT      12

// XComContentManager.EColorPalette, by the archetype names DefaultContent.ini
// gives them.
#define PAL_HAIR   0
#define PAL_SKIN0  4     // CaucasianSkin .. AsianSkin are 4..7
#define PAL_TINT   9
#define PAL_COUNT 10
static const char* const k_pal_names[PAL_COUNT] = {
    "HairColor", "ShirtColor", "PantsColor", "FormalClothesColor",
    "CaucasianSkin", "AfricanSkin", "HispanicSkin", "AsianSkin",
    "EyeColor", "ArmorTint",
};

static const char* const k_races[4] = { "Caucasian", "African", "Asian", "Hispanic" };

// XComLinearColorPaletteEntry: Primary then Secondary, each a LinearColor of
// four floats.
typedef struct { float r, g, b, a; } Linear;
typedef struct { Linear primary, secondary; } PaletteEntry;

static FieldSlot g_owner, g_soldier, g_pawn, g_heads, g_hairs, g_kits;
static FieldSlot g_nhaircol, g_ntint, g_headc, g_skinpal, g_entries, g_presets;

// ---- the pawn's appearance ---------------------------------------------------

enum { A_HEAD, A_RACE, A_HAIRCUT, A_HAIRCOLOR, A_FACIAL, A_SKIN, A_DECO, A_TINT, A_COUNT };
static const char* const k_app_members[A_COUNT] = {
    "iHead", "iRace", "iHaircut", "iHairColor", "iFacialHair", "iSkinColor",
    "iArmorDeco", "iArmorTint",
};
static uint32_t g_app_off;
static uint32_t g_member_off[A_COUNT];
static int      g_app_known;   // 1 found, -1 not to be found, 0 not asked

static int appearance(void* pawn, int out[A_COUNT])
{
    if (!g_app_known) {
        const void* st = field_struct(pawn, "m_kAppearance", &g_app_off);
        g_app_known = st ? 1 : -1;
        for (int i = 0; st && i < A_COUNT; i++)
            if (!struct_member(st, k_app_members[i], &g_member_off[i], NULL))
                g_app_known = -1;
        logf_("customize: TAppearance %s (m_kAppearance at +0x%X)\n",
              g_app_known > 0 ? "found" : "NOT FOUND -- spinners left as numbers",
              g_app_off);
    }
    if (g_app_known < 0) return 0;
    const uint8_t* base = (const uint8_t*)pawn + g_app_off;
    for (int i = 0; i < A_COUNT; i++) {
        if (!readable(base + g_member_off[i], sizeof(int32_t))) return 0;
        out[i] = *(const int32_t*)(base + g_member_off[i]);
    }
    return 1;
}

static int array_len(void* obj, const char* name, FieldSlot* slot)
{
    const void* v;
    if (!field_ptr(obj, name, slot, sizeof(FArray), &v)) return -1;
    int n = ((const FArray*)v)->Num;
    return n >= 0 && n < 4096 ? n : -1;
}

static int int_field(void* obj, const char* name, FieldSlot* slot)
{
    const void* v;
    return field_ptr(obj, name, slot, sizeof(int32_t), &v) ? *(const int32_t*)v : -1;
}

static void* object_field(void* obj, const char* name, FieldSlot* slot)
{
    const void* v;
    if (!obj || !field_ptr(obj, name, slot, sizeof(void*), &v)) return NULL;
    void* o = *(void* const*)v;
    return o && unit_is_live(o) ? o : NULL;
}

// ---- the palettes ------------------------------------------------------------

static void* g_pal[PAL_COUNT];
static void* g_content;          // XComContentManager, for FacialHairPresets
static int   g_pal_logged[PAL_COUNT];

typedef struct { int found; } PalWalk;

static int pal_visit(void* obj, int which, int idx, void* ctx)
{
    (void)idx;
    PalWalk* w = (PalWalk*)ctx;
    if (which == 1) { if (!g_content) g_content = obj; return 1; }
    char name[64];
    if (!object_name(obj, name, sizeof name)) return 1;
    for (int i = 0; i < PAL_COUNT; i++)
        if (!g_pal[i] && strcmp(name, k_pal_names[i]) == 0) { g_pal[i] = obj; w->found++; }
    return 1;
}

// The palettes, found in one walk of the object table (tens of milliseconds)
// the first time the screen asks, and again only if one has gone.
static void palettes_find(void)
{
    int stale = !g_content || !unit_is_live(g_content);
    for (int i = 0; i < PAL_COUNT && !stale; i++)
        if (i == PAL_HAIR || i == PAL_TINT || (i >= PAL_SKIN0 && i < PAL_SKIN0 + 4))
            if (!g_pal[i] || !unit_is_live(g_pal[i])) stale = 1;
    if (!stale) return;

    static ULONGLONG tried_at;
    ULONGLONG now = GetTickCount64();
    if (tried_at && now - tried_at < 2000) return;   // not on every redraw
    tried_at = now;

    memset(g_pal, 0, sizeof g_pal);
    g_content = NULL;
    const char* const names[2] = { "XComLinearColorPalette", "XComContentManager" };
    const void* classes[2];
    objects_classes(names, classes, 2);
    PalWalk w = { 0 };
    int n = classes[0] ? objects_each(classes, classes[1] ? 2 : 1, pal_visit, &w) : -1;
    logf_("customize: %d palette%s found (%s) in a walk of %d, content manager %s\n",
          w.found, w.found == 1 ? "" : "s", classes[0] ? "by name" : "no palette class",
          n, g_content ? "found" : "not found");
}

static const PaletteEntry* palette_entry(int pal, int index, int* count)
{
    *count = 0;
    void* p = pal >= 0 && pal < PAL_COUNT ? g_pal[pal] : NULL;
    const void* v;
    if (!p || !field_ptr(p, "Entries", &g_entries, sizeof(FArray), &v)) return NULL;
    const FArray* arr = (const FArray*)v;
    if (arr->Num <= 0 || arr->Num > 256 ||
        !readable(arr->Data, (size_t)arr->Num * sizeof(PaletteEntry)))
        return NULL;
    *count = arr->Num;
    const PaletteEntry* e = (const PaletteEntry*)arr->Data;

    // Once per palette: every entry as the colour a picker would show, and the
    // name it is given -- the evidence for tuning colors.c against the game.
    if (!g_pal_logged[pal]) {
        g_pal_logged[pal] = 1;
        char line[2048];
        size_t used = (size_t)_snprintf_s(line, sizeof line, _TRUNCATE,
                                          "customize: palette %s, %d entries:",
                                          k_pal_names[pal], arr->Num);
        for (int i = 0; i < arr->Num && used < sizeof line - 64; i++) {
            char nm[48];
            color_name(e[i].primary.r, e[i].primary.g, e[i].primary.b, pal == PAL_HAIR,
                       nm, sizeof nm);
            int w = _snprintf_s(line + used, sizeof line - used, _TRUNCATE,
                                " %d #%02x%02x%02x %s;", i + 1,
                                color_srgb8(e[i].primary.r), color_srgb8(e[i].primary.g),
                                color_srgb8(e[i].primary.b), nm);
            if (w > 0) used += (size_t)w;
        }
        logf_("%s\n", line);
    }
    return index >= 0 && index < arr->Num ? &e[index] : NULL;
}

// ---- the descriptions --------------------------------------------------------

static int is_number(const char* s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void* customize_pawn(void* helper)
{
    void* screen = object_field(helper, "Owner", &g_owner);
    char cls[64];
    if (!screen || !object_class_name(screen, cls, sizeof cls) ||
        strcmp(cls, "UISoldierCustomize") != 0)
        return NULL;
    void* soldier = object_field(screen, "m_kSoldier", &g_soldier);
    return object_field(soldier, "m_kPawn", &g_pawn);
}

int customize_describe(void* helper, int widget, const char* value,
                       char* out, size_t out_sz)
{
    if (widget < W_RACE || widget > W_TINT || !is_number(value)) return 0;
    void* pawn = customize_pawn(helper);
    int app[A_COUNT];
    if (!pawn || !appearance(pawn, app)) return 0;

    switch (widget) {
    case W_RACE:
        if (app[A_RACE] < 0 || app[A_RACE] > 3) return 0;
        _snprintf_s(out, out_sz, _TRUNCATE, "%s", k_races[app[A_RACE]]);
        return 1;

    case W_HEAD: {
        int n = array_len(pawn, "PossibleHeads", &g_heads);
        if (n <= 0) return 0;
        _snprintf_s(out, out_sz, _TRUNCATE, "%s of %d", value, n);
        return 1;
    }

    case W_HAIR: {
        int n = array_len(pawn, "PossibleHairs", &g_hairs);
        if (n < 0) return 0;
        _snprintf_s(out, out_sz, _TRUNCATE, "%s%s of %d",
                    app[A_HAIRCUT] == -1 ? "bald, " : "", value, n + 1);
        return 1;
    }

    case W_SKIN: {
        palettes_find();
        void* head = object_field(pawn, "HeadContent", &g_headc);
        const void* v;
        int pal = head && field_ptr(head, "SkinPalette", &g_skinpal, 1, &v)
                  ? *(const uint8_t*)v : PAL_SKIN0 + app[A_RACE];
        int n;
        const PaletteEntry* mine = palette_entry(pal, app[A_SKIN], &n);
        if (!mine) return 0;
        // Its place among the palette's tones, lightest first.
        const PaletteEntry* all = mine - app[A_SKIN];
        float my_l = color_lightness(mine->primary.r, mine->primary.g, mine->primary.b);
        int rank = 0;
        for (int i = 0; i < n; i++)
            if (color_lightness(all[i].primary.r, all[i].primary.g, all[i].primary.b) > my_l)
                rank++;
        _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s of %d",
                    color_tone_word(rank, n), value, n);
        return 1;
    }

    case W_HAIRCOLOR: {
        palettes_find();
        int n;
        const PaletteEntry* e = palette_entry(PAL_HAIR, app[A_HAIRCOLOR], &n);
        if (!e) return 0;
        // The spinner counts what the hair allows (NumPossibleHairColors),
        // which is the palette's length unless the hair is a helmet.
        int shown = int_field(pawn, "NumPossibleHairColors", &g_nhaircol);
        char nm[48];
        color_name(e->primary.r, e->primary.g, e->primary.b, 1, nm, sizeof nm);
        _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s of %d", nm, value,
                    shown > 0 ? shown : n);
        return 1;
    }

    case W_FACIAL: {
        palettes_find();
        int n = g_content ? array_len(g_content, "FacialHairPresets", &g_presets) : -1;
        if (n > 0)
            _snprintf_s(out, out_sz, _TRUNCATE, "%s%s of %d",
                        app[A_FACIAL] == 0 ? "none, " : "", value, n);
        else
            _snprintf_s(out, out_sz, _TRUNCATE, "%s%s",
                        app[A_FACIAL] == 0 ? "none, " : "", value);
        return 1;
    }

    case W_DECO: {
        int n = array_len(pawn, "PossibleArmorKits", &g_kits);
        if (n <= 0) return 0;
        _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s of %d",
                    app[A_DECO] == -1 ? "standard" : "decorated", value, n);
        return 1;
    }

    case W_TINT: {
        palettes_find();
        int n;
        const PaletteEntry* e = palette_entry(PAL_TINT, app[A_TINT], &n);
        if (!e) return 0;
        int shown = int_field(pawn, "NumPossibleArmorTints", &g_ntint);
        char a[48], b[48];
        color_name(e->primary.r, e->primary.g, e->primary.b, 0, a, sizeof a);
        color_name(e->secondary.r, e->secondary.g, e->secondary.b, 0, b, sizeof b);
        if (strcmp(a, b) == 0)
            _snprintf_s(out, out_sz, _TRUNCATE, "%s, %s of %d", a, value,
                        shown > 0 ? shown : n);
        else
            _snprintf_s(out, out_sz, _TRUNCATE, "%s with %s, %s of %d", a, b, value,
                        shown > 0 ? shown : n);
        return 1;
    }
    }
    return 0;
}
