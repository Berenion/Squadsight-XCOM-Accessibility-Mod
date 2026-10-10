// Every council country's panic, for Delete while an abduction site is being
// chosen.
//
// The choice (UIMissionControl_AbductionSelection) shows one country per
// site, but a site left unhelped raises panic in its country and across its
// whole continent (XGStrategyAI.ApplyMissionPanic: PANIC_ABDUCTION_COUNTRY_*
// on the country, PANIC_ABDUCTION_CONTINENT_* through XGContinent.AddPanic),
// so what matters is every country beside it. The Situation
// Room draws them (hq_sit_*), but only while the player is in it, so on the
// way to an abduction its numbers are a visit old, or were never drawn at
// all this session. They are read from the game's own actors instead:
//
//     XGContinent: m_strName, m_eContinent, array<int> m_arrCountries
//     XGCountry:   m_kTCountry (TCountry: iEnum, strName, ...), m_iPanic,
//                  m_bSecretPact (left XCOM), m_bSatellite
//
// and panic in blocks as the game shows it, XGCountry.GetPanicBlocks():
// m_iPanic + 1, -1 meaning 5, clamped to 1..5. The choice of countries and
// their order are the Situation Room's (XGSituationRoomUI.SortSitCountries):
// council members only (TCountry.bCouncilMember -- the continents also list
// the 16 that are not, and the 2026-10-03 (12:50) log read all 32), the
// continents in the order North America, South America, Africa, Europe, Asia
// (enum 0, 1, 4, 2, 3), each continent's countries in its m_arrCountries
// order. Both builds declare the same fields.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "countries.h"
#include "strings.h"
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "props.h"
#include "objects.h"
#include "ue3.h"

#define MAX_CONT  8
#define MAX_CTRY  32

typedef struct {
    void* obj[MAX_CONT];
    int   ncont;
    void* ctry[MAX_CTRY];
    int   nctry;
} Found;

static int visit(void* obj, int which, int idx, void* ctx)
{
    (void)idx;
    Found* f = (Found*)ctx;
    if (which == 0 && f->nctry < MAX_CTRY) f->ctry[f->nctry++] = obj;
    if (which == 1 && f->ncont < MAX_CONT) f->obj[f->ncont++] = obj;
    return 1;
}

static FieldSlot g_panic, g_tcountry, g_cname, g_cenum, g_ccountries;
static uint32_t  g_tc_off, g_enum_off, g_name_off, g_council_off;
static const void* g_council_prop;
static int       g_tc_known;
static int       g_logged;

// TCountry's iEnum and strName, found once by name.
static int tcountry_layout(void* country)
{
    if (g_tc_known) return g_tc_known > 0;
    const void* st = field_struct(country, "m_kTCountry", &g_tc_off);
    if (st && struct_member(st, "iEnum", &g_enum_off, NULL) &&
        struct_member(st, "strName", &g_name_off, NULL)) {
        g_tc_known = 1;
        if (!struct_member(st, "bCouncilMember", &g_council_off, &g_council_prop))
            g_council_prop = NULL;
        logf_("countries: TCountry at +0x%X, iEnum +0x%X, strName +0x%X, "
              "bCouncilMember %s\n", g_tc_off, g_enum_off, g_name_off,
              g_council_prop ? "found" : "NOT FOUND -- every country listed");
    } else {
        g_tc_known = -1;
        logf_("countries: no TCountry layout on XGCountry\n");
    }
    return g_tc_known > 0;
}

static int country_bool(void* country, const char* name)
{
    // Without the bit mask both flags would read as the dword they share.
    if (!props_mask_offset()) return 0;
    const void* prop = object_field_prop(country, name);
    int b = 0;
    if (!prop || !props_read_object_bool(prop, (const uint8_t*)country, &b)) return 0;
    return b != 0;
}

// Whether a country sits on the council: TCountry.bCouncilMember, a bit in
// a dword that holds the struct's other bools too. Taken as a member when it
// cannot be read, so a failure lists too many rather than none.
static int council_member(const void* country)
{
    uint32_t moff = props_mask_offset();
    if (!g_council_prop || !moff || !readable((const uint8_t*)g_council_prop + moff, 4))
        return 1;
    uint32_t mask = *(const uint32_t*)((const uint8_t*)g_council_prop + moff);
    const uint8_t* word = (const uint8_t*)country + g_tc_off + g_council_off;
    if (!mask || !readable(word, sizeof(uint32_t))) return 1;
    return (*(const uint32_t*)word & mask) != 0;
}

// The country whose TCountry.iEnum is `e`, or NULL.
static void* country_of(const Found* f, int e)
{
    for (int i = 0; i < f->nctry; i++) {
        const uint8_t* c = (const uint8_t*)f->ctry[i];
        if (readable(c + g_tc_off + g_enum_off, sizeof(int32_t)) &&
            *(const int32_t*)(c + g_tc_off + g_enum_off) == e)
            return f->ctry[i];
    }
    return NULL;
}

static void put(char* out, size_t out_sz, size_t* w, const char* s)
{
    _snprintf_s(out + *w, out_sz - *w, _TRUNCATE, "%s", s);
    *w += strlen(out + *w);
}

int countries_lines(char* lines, size_t width, int max)
{
    static const char* const names[2] = { "XGCountry", "XGContinent" };
    const void* classes[2] = { NULL, NULL };
    objects_classes(names, classes, 2);
    if (!classes[0] || !classes[1]) {
        if (!g_logged++) logf_("countries: no XGCountry / XGContinent class loaded\n");
        return 0;
    }
    static Found f;
    memset(&f, 0, sizeof f);
    if (objects_each(classes, 2, visit, &f) < 0 || !f.nctry || !f.ncont) {
        logf_("countries: %d countries, %d continents found\n", f.nctry, f.ncont);
        return 0;
    }
    if (!tcountry_layout(f.ctry[0])) return 0;

    // Continents in SortSitCountries' order, by rank: a selection sort over
    // at most eight.
    static const int rank[5] = { 0, 1, 3, 4, 2 };   // enum -> place
    int order[MAX_CONT], key[MAX_CONT];
    for (int i = 0; i < f.ncont; i++) {
        const void* v;
        int e = field_ptr(f.obj[i], "m_eContinent", &g_cenum, 1, &v) ? *(const uint8_t*)v : 99;
        order[i] = i;
        key[i] = e < 5 ? rank[e] : 99;
    }
    for (int i = 0; i < f.ncont; i++)
        for (int j = i + 1; j < f.ncont; j++)
            if (key[order[j]] < key[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }

    int n = 0, said = 0;
    for (int k = 0; k < f.ncont && n < max; k++) {
        void* cont = f.obj[order[k]];
        const void* v;
        char cname[64] = "";
        if (field_ptr(cont, "m_strName", &g_cname, sizeof(FString), &v))
            read_fstring((const FString*)v, cname, sizeof cname);
        if (!field_ptr(cont, "m_arrCountries", &g_ccountries, sizeof(FArray), &v)) continue;
        const FArray* arr = (const FArray*)v;
        if (arr->Num <= 0 || arr->Num > MAX_CTRY ||
            !readable(arr->Data, (size_t)arr->Num * sizeof(int32_t)))
            continue;
        char* out = lines + (size_t)n * width;
        size_t w = 0;
        out[0] = 0;
        if (cname[0]) { put(out, width, &w, cname); put(out, width, &w, ": "); }
        int any = 0;
        for (int i = 0; i < arr->Num; i++) {
            void* c = country_of(&f, ((const int32_t*)arr->Data)[i]);
            if (!c || !council_member(c)) continue;
            char name[64] = "", piece[160];
            read_fstring((const FString*)((const uint8_t*)c + g_tc_off + g_name_off),
                         name, sizeof name);
            int panic = 0;
            if (field_ptr(c, "m_iPanic", &g_panic, sizeof(int32_t), &v))
                panic = *(const int32_t*)v;
            int blocks = panic == -1 ? 5 : panic + 1;
            if (blocks < 1) blocks = 1;
            if (blocks > 5) blocks = 5;
            size_t pw = 0;
            piece[0] = 0;
            if (any) { strcpy_s(piece, sizeof piece, ". "); pw = 2; }
            tfmt_cat(piece, sizeof piece, &pw, COUNTRY_PANIC,
                     name[0] ? name : T(COUNTRY_UNKNOWN), blocks);
            if (country_bool(c, "m_bSatellite"))
                tfmt_cat(piece, sizeof piece, &pw, COUNTRY_SATELLITE);
            if (country_bool(c, "m_bSecretPact"))
                tfmt_cat(piece, sizeof piece, &pw, COUNTRY_LEFT);
            put(out, width, &w, piece);
            any = 1;
            said++;
        }
        if (any) n++;
    }
    logf_("countries: %d continents, %d council countries of %d\n", n, said, f.nctry);
    return n;
}
