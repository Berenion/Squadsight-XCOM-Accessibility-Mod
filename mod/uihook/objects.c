// Walking UObject::GObjObjects. See objects.h.

#include "objects.h"
#include "names.h"
#include "cursor.h"
#include "props.h"
#include "ue3.h"
#include <stdio.h>
#include <string.h>

extern int readable(const void* p, size_t n);   // provided by main.c

// Candidate offsets of UObject::Index. UE3 puts it just past ObjectFlags,
// which is a QWORD at 0x04, so 0x0C is the expected answer -- but the probe
// tries a window rather than assuming, exactly as names.c does for the name
// text and props.c does for the class pointer.
// Widened after the first run found nothing: UObject's front is vtable,
// ObjectFlags (a QWORD), then a run of engine bookkeeping, and licensee builds
// shuffle it. Name is known to be at 0x2C and Class at 0x34, so Index is below
// that; everything from just past the flags to just below Name is tried.
// EW keeps it at 0x20, which is where the probe found it; the rest are tried
// after, because EU is a different build and this file does not assume.
// UObject's front is vtable, ObjectFlags (a QWORD), then engine bookkeeping;
// Name is at 0x2C and Class at 0x34, so Index is somewhere below that.
static const uint32_t kIndexOffsets[] = {
    0x20, 0x0C, 0x08, 0x10, 0x14, 0x18, 0x1C, 0x24, 0x28,
};

static FArray*  g_objs;
static uint32_t g_index_off;
static unsigned g_walk_ms;
static int      g_walk_entries;
static HMODULE  g_mod;              // kept so the probe can be run again
static DWORD    g_last_probe;

#define RETRY_EVERY_MS 5000

// ---- reading a table this large --------------------------------------------
//
// readable() is a VirtualQuery per call, and the walk makes one per object. A
// hundred thousand syscalls on the game's thread is a stutter, so the regions
// VirtualQuery has already approved are kept: the objects come out of a few
// heap regions, and after the first object in one the rest are a pair of
// comparisons. Rejections are not cached -- a region that is not there now may
// be mapped later, and a wrong "no" would quietly lose objects.

#define REGION_CACHE 8

typedef struct { const uint8_t* lo, *hi; } Region;
static Region g_regions[REGION_CACHE];
static int    g_region_next;
static int    g_region_hot;         // the one the last call was answered from

static void region_forget(void)
{
    memset(g_regions, 0, sizeof g_regions);
    g_region_next = 0;
    g_region_hot = 0;
}

static int region_ok(const void* p, size_t n)
{
    if (!p) return 0;
    const uint8_t* lo = (const uint8_t*)p;
    const uint8_t* hi = lo + n;
    if (hi < lo) return 0;                          // wrapped

    // The hot slot before the rest. A walk of the table comes out of a
    // handful of heap regions and hits the same one thousands of times in a
    // row, so the answer should cost one comparison rather than eight.
    if (g_regions[g_region_hot].lo &&
        lo >= g_regions[g_region_hot].lo && hi <= g_regions[g_region_hot].hi)
        return 1;

    for (int i = 0; i < REGION_CACHE; i++)
        if (g_regions[i].lo && lo >= g_regions[i].lo && hi <= g_regions[i].hi) {
            g_region_hot = i;
            return 1;
        }

    if (!readable(p, n)) return 0;

    // Keep the whole region the range fell in, not just the range.
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
        const uint8_t* rlo = (const uint8_t*)mbi.BaseAddress;
        const uint8_t* rhi = rlo + mbi.RegionSize;
        if (hi <= rhi) {                            // one region covers it
            g_regions[g_region_next].lo = rlo;
            g_regions[g_region_next].hi = rhi;
            g_region_hot = g_region_next;
            g_region_next = (g_region_next + 1) % REGION_CACHE;
        }
    }
    return 1;
}

// ---- finding the table -----------------------------------------------------
//
// The invariant the engine keeps: an object's Index is where it sits in the
// table. That is what lets an object be freed by index, and no other array of
// pointers in .data can satisfy it by accident.
//
// The first version of this probe cost the launcher its patience: it sampled
// every candidate array once per candidate index offset -- nine passes, each a
// VirtualQuery and a name decode per entry -- and took longer than the ten
// seconds the launcher waits for the arming banner. The game was fine; the
// launcher reported "attached but did not arm" because the banner had not been
// written yet.
//
// So the sample is taken once per candidate and every offset is tested against
// it. That is the whole difference: an object already proven readable for 0x60
// bytes can have any offset below 0x60 read out of it without asking Windows
// again, so testing nine offsets costs nine integer comparisons rather than
// nine passes.

#define SAMPLE_MAX 64

typedef struct { int index; void* obj; } Sample;

// Up to SAMPLE_MAX live entries, spread across the array rather than taken off
// the front, where the engine's own intrinsics sit and a wrong offset can match
// a small integer by luck. `named` counts how many resolved to a name.
//
// Gives up early on an array whose first few live entries are not objects at
// all, so a stray run of plausible numbers in .data costs eight reads and not
// sixty-four.
static int collect(const FArray* arr, Sample* out, int* named)
{
    void** data = (void**)arr->Data;
    int n = 0;
    *named = 0;
    int step = arr->Num / SAMPLE_MAX;
    if (step < 1) step = 1;

    for (int i = 0; i < arr->Num && n < SAMPLE_MAX; i += step) {
        if (!readable(&data[i], sizeof(void*))) return n;
        void* obj = data[i];
        if (!obj) continue;                  // a freed slot; not evidence
        if (!readable(obj, 0x60)) return n;
        out[n].index = i;
        out[n].obj = obj;
        n++;
        char name[128];
        if (object_name(obj, name, sizeof name) && name[0]) (*named)++;
        if (n == 8 && *named == 0) return n;  // not a table of objects
    }
    return n;
}

// The offset at which every sampled object carries its own position, or 0.
// No reads beyond the 0x60 bytes collect() has already proven readable.
static uint32_t index_offset(const Sample* s, int n)
{
    for (size_t k = 0; k < sizeof kIndexOffsets / sizeof *kIndexOffsets; k++) {
        uint32_t off = kIndexOffsets[k];
        int hit = 0;
        for (int i = 0; i < n; i++)
            if (*(const int32_t*)((const uint8_t*)s[i].obj + off) == s[i].index)
                hit++;
        if (hit == n) return off;
    }
    return 0;
}

int objects_init(HMODULE mod, char* why, size_t why_sz)
{
    uint8_t* dbase;
    size_t dsize;
    g_mod = mod;
    g_last_probe = GetTickCount();
    DWORD began = g_last_probe;

    if (!module_section(mod, ".data", &dbase, &dsize)) {
        _snprintf_s(why, why_sz, _TRUNCATE, "no .data section");
        return 0;
    }

    // The best near-miss, so a failure can say what it nearly found.
    int best_named = 0, best_live = 0;
    const FArray* best_arr = NULL;
    Sample sample[SAMPLE_MAX];

    for (size_t off = 0; off + sizeof(FArray) <= dsize; off += 4) {
        FArray* cand = (FArray*)(dbase + off);
        // Even in the shell the table held 121908 entries, so the floor can be
        // high enough to throw out most of the stray numbers in .data.
        if (cand->Num < 5000 || cand->Num > 8000000) continue;
        if (cand->Max < cand->Num || cand->Max > 16000000) continue;
        if (!cand->Data) continue;

        int named = 0;
        int n = collect(cand, sample, &named);
        if (n < 16 || named * 4 < n * 3) {
            if (named > best_named) { best_named = named; best_live = n; best_arr = cand; }
            continue;
        }

        uint32_t idx = index_offset(sample, n);
        if (idx) {
            g_objs = cand;
            g_index_off = idx;
            _snprintf_s(why, why_sz, _TRUNCATE,
                        "GObjObjects at %p (data %p, %d entries), "
                        "UObject::Index +0x%02X (%d of %d sampled named) in %u ms",
                        (void*)cand, cand->Data, cand->Num, idx, named, n,
                        (unsigned)(GetTickCount() - began));
            return 1;
        }
        if (named > best_named) { best_named = named; best_live = n; best_arr = cand; }
    }

    if (best_arr)
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "GObjObjects not found in %u ms; closest was %p (data %p, "
                    "%d entries) -- %d of %d sampled named, no offset carried "
                    "the index",
                    (unsigned)(GetTickCount() - began), (const void*)best_arr,
                    best_arr->Data, best_arr->Num, best_named, best_live);
    else
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "GObjObjects not found in %u ms; no array in .data even "
                    "looked like one", (unsigned)(GetTickCount() - began));
    return 0;
}

int objects_ready(void) { return g_objs != NULL; }

int objects_retry(char* why, size_t why_sz)
{
    if (why && why_sz) why[0] = 0;
    if (g_objs) return 1;
    if (!g_mod) return 0;
    DWORD now = GetTickCount();
    if (now - g_last_probe < RETRY_EVERY_MS) return 0;
    char scratch[256];
    return objects_init(g_mod, why && why_sz ? why : scratch,
                        why && why_sz ? why_sz : sizeof scratch);
}

int objects_count(void)
{
    if (!g_objs || !readable(g_objs, sizeof *g_objs)) return 0;
    return g_objs->Num;
}

void objects_last_walk(unsigned* ms, int* entries)
{
    if (ms) *ms = g_walk_ms;
    if (entries) *entries = g_walk_entries;
}

// ---- classes by pointer ----------------------------------------------------

#define CLASS_CACHE 8

typedef struct { char name[64]; const void* cls; } ClassSlot;
static ClassSlot g_classes[CLASS_CACHE];
static int       g_nclasses;

static int class_cached(const char* name, const void** out)
{
    for (int i = 0; i < g_nclasses; i++)
        if (strcmp(g_classes[i].name, name) == 0) { *out = g_classes[i].cls; return 1; }
    return 0;
}

static void class_remember(const char* name, const void* cls)
{
    if (g_nclasses >= CLASS_CACHE) return;
    strncpy_s(g_classes[g_nclasses].name, sizeof g_classes[g_nclasses].name,
              name, _TRUNCATE);
    g_classes[g_nclasses].cls = cls;
    g_nclasses++;
}

#define CLASS_BATCH 8

void objects_classes(const char* const* names, const void** out, int n)
{
    if (n > CLASS_BATCH) n = CLASS_BATCH;
    for (int i = 0; i < n; i++) out[i] = NULL;
    if (!g_objs || !names) return;

    // Anything already known is answered from the cache, and only what is left
    // is worth a pass.
    const char* want[CLASS_BATCH];
    int slot[CLASS_BATCH];
    int nwant = 0;
    for (int i = 0; i < n; i++) {
        if (!names[i]) continue;
        if (class_cached(names[i], &out[i])) continue;
        want[nwant] = names[i];
        slot[nwant] = i;
        nwant++;
    }
    if (!nwant) return;

    // The names, as table indices. An index of -1 means the game has never
    // heard of that name, so no object can carry it.
    int32_t idx[CLASS_BATCH];
    names_find(want, idx, nwant);

    if (!readable(g_objs, sizeof *g_objs)) return;
    void** data = (void**)g_objs->Data;
    int total = g_objs->Num;
    int left = nwant;
    region_forget();

    for (int i = 0; i < total && left; i++) {
        if (!region_ok(&data[i], sizeof(void*))) break;
        void* obj = data[i];
        if (!obj || !region_ok(obj, 0x60)) continue;

        // One integer compare per object. FName::Number is left out of it: a
        // class is never a numbered name.
        const FName* fn = (const FName*)((const uint8_t*)obj + UOBJECT_NAME);
        for (int k = 0; k < nwant; k++) {
            if (out[slot[k]] || idx[k] < 0 || fn->Index != idx[k]) continue;
            // The name is right; make sure this is the class and not, say, a
            // default object that shares it.
            if (!object_is_a(obj, "Class")) continue;
            out[slot[k]] = obj;
            left--;
            break;
        }
    }

    for (int k = 0; k < nwant; k++) class_remember(want[k], out[slot[k]]);
}

const void* objects_class(const char* name)
{
    const void* cls = NULL;
    objects_classes(&name, &cls, 1);
    return cls;
}

// Whether an object's class is one of `classes`, or below one, by pointer --
// and which. No names are read: this runs once per object in the table.
//
// The answer is kept per class, because a table of 175,000 objects holds only
// a few hundred distinct classes and the chain walk is the expensive part. A
// direct-mapped cache on the class pointer turns almost every object into one
// comparison.
#define DECIDED_SLOTS 256

typedef struct { const void* cls; int which; } Decided;

// `class_proved` says the caller has already proved the class pointer
// readable -- objects_each has, because it checks the object for 0x60 bytes
// and UObject::Class lies inside that. Without it this asked Windows, or at
// least the region cache, a second time about a range it had just approved.
// A NULL `cache` is for the one-off check, which has nothing to reuse.
static int decide(const void* obj, const void* const* classes, int n,
                  uint32_t class_off, uint32_t super_off, Decided* cache,
                  int class_proved)
{
    if (!class_proved &&
        !region_ok((const uint8_t*)obj + class_off, sizeof(void*))) return -1;
    const void* c0 = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!c0) return -1;

    size_t slot = ((uintptr_t)c0 >> 4) & (DECIDED_SLOTS - 1);
    if (cache) {
        if (cache[slot].cls == c0) return cache[slot].which;
    }

    int which = -1;
    const void* c = c0;
    for (int depth = 0; c && depth < 32 && which < 0; depth++) {
        for (int k = 0; k < n; k++)
            if (c == classes[k]) { which = k; break; }
        if (which >= 0) break;
        if (!region_ok((const uint8_t*)c + super_off, sizeof(void*))) break;
        c = *(const void* const*)((const uint8_t*)c + super_off);
    }
    if (cache) {
        cache[slot].cls = c0;
        cache[slot].which = which;
    }
    return which;
}

// A class default object is not a thing on the map. UE3 names every one of
// them "Default__<Class>", which is the cheapest way to tell from out here --
// and it is only asked about objects that already matched a class, so it costs
// a name decode per door and not per table entry.
static int is_default_object(void* obj)
{
    char name[128];
    if (!object_name(obj, name, sizeof name)) return 0;
    return strncmp(name, "Default__", 9) == 0;
}

int objects_each_from(const void* const* classes, int n, int from, int* next,
                      ObjectVisitFn fn, void* ctx)
{
    if (next) *next = from;
    if (!g_objs || !fn || !classes || n <= 0) return -1;
    uint32_t class_off = props_class_offset();
    uint32_t super_off = object_super_offset();
    if (!class_off || !super_off) return -1;

    // Re-read the array every walk: it grows, and it is reallocated when it
    // does, so a pointer kept from last time would walk freed memory. The
    // reallocation copies, so an index taken from an earlier walk still names
    // the same object; a table that has *shrunk* is a different table, and
    // starting over is the only safe reading of it.
    if (!readable(g_objs, sizeof *g_objs)) return -1;
    void** data = (void**)g_objs->Data;
    int total = g_objs->Num;
    if (!data || total <= 0) return -1;
    if (from < 0 || from > total) from = 0;

    static Decided cache[DECIDED_SLOTS];
    memset(cache, 0, sizeof cache);

    DWORD began = GetTickCount();
    region_forget();

    // The class pointer sits inside the 0x60 bytes proved below, so decide()
    // need not prove it again -- and the table is one allocation, so it is
    // proved once here instead of once per entry. Those two checks were two
    // thirds of the region-cache traffic of a full walk. A table too large
    // for a single committed region falls back to the per-entry check.
    int class_proved = class_off + sizeof(void*) <= 0x60;
    int array_proved = from < total &&
        region_ok(&data[from], (size_t)(total - from) * sizeof(void*));

    int seen = 0;
    int i;
    for (i = from; i < total; i++) {
        if (!array_proved && !region_ok(&data[i], sizeof(void*))) break;
        void* obj = data[i];
        if (!obj || !region_ok(obj, 0x60)) continue;
        int which = decide(obj, classes, n, class_off, super_off, cache,
                           class_proved);
        if (which < 0) continue;
        if (is_default_object(obj)) continue;
        seen++;
        // Stopped here, so this entry has not been handed over: `i` and not
        // `i + 1` is where a later walk must pick up, or a visitor that
        // stopped because its list was full would lose the object it stopped
        // on for the rest of the mission.
        if (!fn(obj, which, i, ctx)) break;
    }
    if (next) *next = i;
    g_walk_ms = (unsigned)(GetTickCount() - began);
    g_walk_entries = i - from;
    return seen;
}

int objects_each(const void* const* classes, int n, ObjectVisitFn fn, void* ctx)
{
    return objects_each_from(classes, n, 0, NULL, fn, ctx);
}

int objects_live(void* obj)
{
    if (!g_objs || !obj || !g_index_off) return 0;
    if (!readable(g_objs, sizeof *g_objs)) return 0;

    void** data = (void**)g_objs->Data;
    int total = g_objs->Num;
    if (!data || total <= 0) return 0;
    // Asked afresh, not of the region cache. The cache is emptied at the start
    // of each walk and trusted within it; between walks a load can free the
    // regions it vouches for. It did: the objectives panel of the mission being
    // left, asked about during the next one's load, faulted here three times
    // (2026-09-22, reading its index from released memory).
    if (!readable(obj, 0x60)) return 0;

    // The object is asked where it thinks it sits and the table is asked to
    // agree -- the same invariant the probe was built on. A freed object's
    // slot is nulled, so a pointer kept from a moment ago fails here.
    int32_t idx = *(const int32_t*)((const uint8_t*)obj + g_index_off);
    if (idx < 0 || idx >= total) return 0;
    if (!readable(&data[idx], sizeof(void*))) return 0;
    return data[idx] == obj;
}

int objects_still(void* obj, int idx, const void* const* classes, int n)
{
    if (!g_objs || !obj || idx < 0 || !classes || n <= 0) return 0;
    uint32_t class_off = props_class_offset();
    uint32_t super_off = object_super_offset();
    if (!class_off || !super_off) return 0;
    if (!readable(g_objs, sizeof *g_objs)) return 0;

    void** data = (void**)g_objs->Data;
    if (!data || idx >= g_objs->Num) return 0;
    if (!region_ok(&data[idx], sizeof(void*)) || data[idx] != obj) return 0;
    if (!region_ok(obj, 0x60)) return 0;

    return decide(obj, classes, n, class_off, super_off, NULL,
                  class_off + sizeof(void*) <= 0x60) >= 0;
}
