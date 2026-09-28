// FName resolution for XCOM EU/EW.
//
// UnrealScript names are indices into a global name table, so turning
// Stack.Node into "AS_SetTitle" means finding that table.  Rather than chase
// GNames through call chains -- and rather than bake in an address that would
// differ between EU and EW -- this discovers it by scanning .data for a
// TArray whose first entry resolves to "None".
//
// That validator is exact rather than heuristic: UE3 reserves FName index 0
// for NAME_None in every build, so any candidate that produces "None" at
// index 0 and plausible names at a few other indices is the real table.
//
// The same scan also settles the FNameEntry layout, since the offset at which
// "None" is found is the offset the string lives at.

#include "names.h"
#include <string.h>
#include <stdio.h>

// Candidate offsets of the character data inside FNameEntry.  UE3 puts it
// after { INT Index; FNameEntry* HashNext; }, but licensee builds shuffle in
// extra fields, so the scan tries a range rather than assuming.
static const int kNameOffsets[] = { 0x08, 0x0C, 0x10, 0x14, 0x18 };

static FArray* g_names;      // points at the live TArray, re-read every lookup
static int     g_name_off;

extern int readable(const void* p, size_t n);   // provided by game.c

// UE3 stores a name entry as either ANSI or UTF-16 depending on a flag we do
// not need to locate: for the ASCII names UnrealScript actually uses, a
// second byte of zero is an unambiguous marker of UTF-16.
static int decode_entry(const void* entry, int off, char* out, size_t out_sz)
{
    const uint8_t* p = (const uint8_t*)entry + off;

    // Validate one window up front rather than probing per character: this
    // runs on the game's UI thread and VirtualQuery is a syscall.  Entries
    // near the end of a page may not have the largest window available, so
    // fall back through smaller ones.
    static const size_t windows[] = { 256, 64, 16, 4 };
    size_t avail = 0;
    for (size_t i = 0; i < sizeof windows / sizeof *windows; i++) {
        if (readable(p, windows[i])) { avail = windows[i]; break; }
    }
    if (!avail) return 0;

    if (p[0] == 0) return 0;
    int wide = (p[1] == 0 && p[0] < 0x80);

    size_t limit = avail / (wide ? sizeof(wchar_t) : 1);
    if (limit > out_sz - 1) limit = out_sz - 1;

    if (wide) {
        const wchar_t* w = (const wchar_t*)p;
        size_t n = 0;
        while (n < limit && w[n]) {
            if (w[n] < 32 || w[n] > 0x7E) return 0;
            n++;
        }
        if (n == 0 || n == limit) return 0;      // unterminated within window
        int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, (int)out_sz - 1,
                                    NULL, NULL);
        if (k <= 0) return 0;
        out[k] = 0;
    } else {
        size_t n = 0;
        while (n < limit && p[n]) {
            if (p[n] < 32 || p[n] > 0x7E) return 0;
            n++;
        }
        if (n == 0 || n == limit) return 0;
        memcpy(out, p, n);
        out[n] = 0;
    }
    return 1;
}

// A table is accepted only if index 0 is exactly "None" and a sample of other
// live entries also decode cleanly -- a lone "None" could be coincidence.
static int validate(const FArray* arr, int off)
{
    void** data = (void**)arr->Data;
    char buf[128];

    if (!readable(data, sizeof(void*))) return 0;
    if (!data[0] || !decode_entry(data[0], off, buf, sizeof buf)) return 0;
    if (strcmp(buf, "None") != 0) return 0;

    int good = 0, tried = 0;
    for (int i = 1; i < arr->Num && tried < 16; i += (arr->Num / 16) + 1) {
        tried++;
        if (!readable(&data[i], sizeof(void*))) continue;
        if (!data[i]) continue;
        if (decode_entry(data[i], off, buf, sizeof buf)) good++;
    }
    return good >= 8;
}

int names_init(HMODULE mod, char* why, size_t why_sz)
{
    uint8_t* dbase;
    size_t dsize;
    if (!module_section(mod, ".data", &dbase, &dsize)) {
        _snprintf_s(why, why_sz, _TRUNCATE, "no .data section");
        return 0;
    }

    for (size_t off = 0; off + sizeof(FArray) <= dsize; off += 4) {
        FArray* cand = (FArray*)(dbase + off);
        // The name table is large and grows; anything small is not it.
        if (cand->Num < 2000 || cand->Num > 4000000) continue;
        if (cand->Max < cand->Num || cand->Max > 8000000) continue;
        if (!cand->Data) continue;
        if (!readable(cand->Data, (size_t)16 * sizeof(void*))) continue;

        for (size_t k = 0; k < sizeof kNameOffsets / sizeof *kNameOffsets; k++) {
            if (validate(cand, kNameOffsets[k])) {
                g_names = cand;
                g_name_off = kNameOffsets[k];
                _snprintf_s(why, why_sz, _TRUNCATE,
                            "GNames at %p (data %p, %d names), FNameEntry text +0x%02x",
                            (void*)cand, cand->Data, cand->Num, kNameOffsets[k]);
                return 1;
            }
        }
    }
    _snprintf_s(why, why_sz, _TRUNCATE, "GNames not found in .data");
    return 0;
}

int names_ready(void) { return g_names != NULL; }

int name_to_string(const FName* fn, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!g_names || !fn) return 0;
    if (!readable(fn, sizeof *fn)) return 0;
    if (fn->Index < 0 || fn->Index >= g_names->Num) return 0;

    // Re-read Data every time: the array is reallocated as names are added.
    void** data = (void**)g_names->Data;
    if (!readable(&data[fn->Index], sizeof(void*))) return 0;
    void* entry = data[fn->Index];
    if (!entry) return 0;

    char buf[256];
    if (!decode_entry(entry, g_name_off, buf, sizeof buf)) return 0;

    // A non-zero Number means the engine appended _N to the printed name.
    if (fn->Number)
        _snprintf_s(out, out_sz, _TRUNCATE, "%s_%d", buf, fn->Number - 1);
    else
        _snprintf_s(out, out_sz, _TRUNCATE, "%s", buf);
    return 1;
}

int object_name(const void* obj, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!obj || !readable((const uint8_t*)obj + UOBJECT_NAME, sizeof(FName)))
        return 0;
    return name_to_string((const FName*)((const uint8_t*)obj + UOBJECT_NAME),
                          out, out_sz);
}

void names_find(const char* const* texts, int32_t* out, int n)
{
    for (int i = 0; i < n; i++) out[i] = -1;
    if (!g_names || !texts || n <= 0) return;
    if (!readable(g_names, sizeof *g_names)) return;

    void** data = (void**)g_names->Data;
    int total = g_names->Num;
    int left = n;

    for (int i = 0; i < total && left; i++) {
        if (!readable(&data[i], sizeof(void*))) break;
        void* entry = data[i];
        if (!entry) continue;
        char buf[256];
        if (!decode_entry(entry, g_name_off, buf, sizeof buf)) continue;
        for (int k = 0; k < n; k++) {
            if (out[k] >= 0 || !texts[k]) continue;
            if (strcmp(buf, texts[k]) != 0) continue;
            out[k] = i;
            left--;
            break;
        }
    }
}
