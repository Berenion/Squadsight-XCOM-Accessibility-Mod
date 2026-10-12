// What the base has to spend, for Delete in Research and Engineering.
//
// The game's own headers show little of it: Engineering's (XGEngineeringUI.
// UpdateHeader) engineers, credits, and elerium and alloys once ever held;
// Research's (XGResearchUI.UpdateHeader) the scientists alone, drawn only on
// the research choice (UIChooseTech, AS_SetHumanResources). Weapon fragments,
// which most research and Foundry projects cost, are on neither; a sighted
// player reads them off a project's cost against the stores. Reported missing
// 2026-10-12. So everything is read from the actors behind those headers:
//
//     XGFacility_Labs:        int m_iNumScientists   (GetAvailableScientists)
//     XGFacility_Engineering: int m_iNumEngineers    (GetNumEngineersAvailable)
//     XGStorage:              array<int> m_arrItems, m_arrClaims, by EItemType
//
// an item available as XGStorage.GetNumItemsAvailable counts it, held less
// claimed (the infinite items are never these). Labelled as the headers label
// them, XGScreenMgr.m_aResourceTypeNames (SCIENTISTS, ENGINEERS, ELERIUM,
// ALLOYS), and the fragments by their item name, XLocalizedData.
// m_aItemNamesPlural (m_aItemNames when that is empty).
// Elerium, alloys and fragments are EItemType 161-163 in EW, 171-173 in EU.
// Both builds declare the same fields.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stores.h"
#include "strings.h"
#include "game.h"
#include "log.h"
#include "objects.h"
#include "ue3.h"

// The live object of each class, the last the walk meets: a save loaded over
// another can leave the old game's actors in the table until they are
// collected, and the newer ones sit later in it.
typedef struct {
    void* obj[3];
    int   count[3];
} Found;

static int visit(void* obj, int which, int idx, void* ctx)
{
    (void)idx;
    Found* f = (Found*)ctx;
    if (which >= 0 && which < 3) { f->obj[which] = obj; f->count[which]++; }
    return 1;
}

static FieldSlot g_sci, g_eng, g_items, g_claims;

static int int_field(void* obj, const char* name, FieldSlot* slot, int* out)
{
    const void* v;
    if (!obj || !field_ptr(obj, name, slot, sizeof(int32_t), &v)) return 0;
    *out = *(const int32_t*)v;
    return 1;
}

// Element `i` of an array<int>, or 0 past its end (an item never held).
static int array_at(const FArray* a, int i)
{
    if (!a || i < 0 || i >= a->Num || !a->Data) return 0;
    const int32_t* p = (const int32_t*)a->Data + i;
    return readable(p, sizeof *p) ? *p : 0;
}

static void put(char* out, size_t out_sz, size_t* w, const char* label, int n)
{
    if (!label[0]) return;
    char piece[128];
    tfmt(piece, sizeof piece, STORES_ITEM, label, n);
    _snprintf_s(out + *w, out_sz - *w, _TRUNCATE, "%s%s.", *w ? " " : "", piece);
    *w += strlen(out + *w);
}

static void label_of(const char* cls, const char* field, int i, char* out, size_t out_sz)
{
    if (!game_loc(cls, field, i, out, out_sz)) {
        out[0] = 0;
        logf_("stores: no %s.%s[%d] -- left out\n", cls, field, i);
    }
}

int stores_line(char* out, size_t out_sz)
{
    static const char* const names[3] = { "XGFacility_Labs", "XGFacility_Engineering", "XGStorage" };
    const void* classes[3] = { NULL, NULL, NULL };
    out[0] = 0;
    objects_classes(names, classes, 3);
    Found f;
    memset(&f, 0, sizeof f);
    if (objects_each(classes, 3, visit, &f) < 0) return 0;

    int sci = 0, eng = 0, have_sci, have_eng;
    have_sci = int_field(f.obj[0], "m_iNumScientists", &g_sci, &sci);
    have_eng = int_field(f.obj[1], "m_iNumEngineers", &g_eng, &eng);
    const FArray* items = NULL;
    const FArray* claims = NULL;
    const void* v;
    if (f.obj[2] && field_ptr(f.obj[2], "m_arrItems", &g_items, sizeof(FArray), &v))
        items = (const FArray*)v;
    if (f.obj[2] && field_ptr(f.obj[2], "m_arrClaims", &g_claims, sizeof(FArray), &v))
        claims = (const FArray*)v;

    int base = game_is_ew() ? 161 : 171;
    int amount[3];
    for (int i = 0; i < 3; i++)
        amount[i] = array_at(items, base + i) - array_at(claims, base + i);

    char label[64];
    size_t w = 0;
    if (have_sci) {
        label_of("XGScreenMgr", "m_aResourceTypeNames", 4, label, sizeof label);
        put(out, out_sz, &w, label, sci);
    }
    if (have_eng) {
        label_of("XGScreenMgr", "m_aResourceTypeNames", 3, label, sizeof label);
        put(out, out_sz, &w, label, eng);
    }
    if (items) {
        label_of("XGScreenMgr", "m_aResourceTypeNames", 1, label, sizeof label);
        put(out, out_sz, &w, label, amount[0]);
        label_of("XGScreenMgr", "m_aResourceTypeNames", 2, label, sizeof label);
        put(out, out_sz, &w, label, amount[1]);
        // The plural, as a heading over a count: the 2026-10-12 log said
        // "Weapon Fragment: 55" with m_aItemNames.
        if (!game_loc("XLocalizedData", "m_aItemNamesPlural", base + 2, label, sizeof label) || !label[0])
            label_of("XLocalizedData", "m_aItemNames", base + 2, label, sizeof label);
        put(out, out_sz, &w, label, amount[2]);
    }
    logf_("stores: labs %d (%d), engineering %d (%d), storage %d (%d items) -- \"%s\"\n",
          f.count[0], sci, f.count[1], eng, f.count[2], items ? items->Num : -1, out);
    return w > 0;
}
