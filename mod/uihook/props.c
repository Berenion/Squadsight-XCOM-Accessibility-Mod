// Reading a UProperty's type, so plain scalars in a frame are not invisible.
//
// A UProperty is a UObject, so its type is simply the class it points at:
// "IntProperty", "BoolProperty", and so on.  That means one unknown -- where
// UObject keeps its UClass* -- and once that is known every scalar in a frame
// can be read.
//
// The offset is not guessed.  A UFunction's children are all properties by
// definition, so the correct offset is the one at which every child resolves
// to a class whose name ends in "Property"; a wrong offset yields garbage
// pointers or unrelated names and is rejected.  This is the same
// self-validating shape as the GNames scan in names.c, which accepts an
// address only when index 0 decodes to "None".

#include "props.h"
#include "ue3.h"
#include "names.h"
#include <string.h>
#include <stdio.h>

static uint32_t g_class_off;     // UObject::Class
static uint32_t g_mask_off;      // UBoolProperty::BitMask, 0 if undiscovered
static LONG     g_ready;

// UObject::Name sits at 0x2C and Class follows the name in every UE3 layout,
// so the search starts just past it.  The window is deliberately short: a
// wrong hit that happens to look valid is worse than no hit at all.
#define CLASS_OFF_MIN  0x30
#define CLASS_OFF_MAX  0x48

// UProperty::Offset is at 0x60, so a subclass field lands after it.
#define MASK_OFF_MIN   0x64
#define MASK_OFF_MAX   0x90

// Reads the class name of `prop` assuming UObject::Class lives at `off`.
static int class_name_at(const void* prop, uint32_t off, char* out, size_t n)
{
    if (!readable((const uint8_t*)prop + off, sizeof(void*))) return 0;
    const void* cls = *(const void* const*)((const uint8_t*)prop + off);
    if (!cls || !readable(cls, UOBJECT_NAME + sizeof(FName))) return 0;
    return object_name(cls, out, n) && out[0];
}

// Scores a candidate offset over a field chain: every child must resolve to a
// *Property class, and at least two must do so, since a single match can be
// coincidence.
static int score_offset(const void* node, uint32_t off)
{
    const void* field = NULL;
    if (readable((const uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        field = *(const void* const*)((const uint8_t*)node + USTRUCT_CHILDREN);

    int hits = 0;
    for (int guard = 0; field && guard < 64; guard++) {
        if (!readable(field, 0x68)) return 0;
        char name[96];
        if (!class_name_at(field, off, name, sizeof name)) return 0;
        if (!ends_with_property(name)) return 0;
        hits++;
        field = *(const void* const*)((const uint8_t*)field + UFIELD_NEXT);
    }
    return hits >= 2 ? hits : 0;
}

// A bool's mask must be a single bit.  Anything else -- zero, or several bits
// -- is not a mask, so a candidate offset is only accepted when every
// BoolProperty in reach agrees.
static int mask_plausible(const void* prop, uint32_t off)
{
    if (!readable((const uint8_t*)prop + off, sizeof(uint32_t))) return 0;
    return is_single_bit(*(const uint32_t*)((const uint8_t*)prop + off));
}

static void find_mask_offset(const void* node)
{
    const void* field = NULL;
    if (readable((const uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*)))
        field = *(const void* const*)((const uint8_t*)node + USTRUCT_CHILDREN);

    for (int guard = 0; field && guard < 64; guard++) {
        char name[96];
        if (class_name_at(field, g_class_off, name, sizeof name) &&
            strcmp(name, "BoolProperty") == 0) {
            for (uint32_t off = MASK_OFF_MIN; off <= MASK_OFF_MAX; off += 4) {
                if (mask_plausible(field, off)) { g_mask_off = off; return; }
            }
        }
        if (!readable((const uint8_t*)field + UFIELD_NEXT, sizeof(void*))) return;
        field = *(const void* const*)((const uint8_t*)field + UFIELD_NEXT);
    }
}

int props_init(const void* node, char* why, size_t why_sz)
{
    if (g_ready) return 1;
    if (!node || !names_ready()) return 0;

    uint32_t best_off = 0;
    int best_hits = 0;
    for (uint32_t off = CLASS_OFF_MIN; off <= CLASS_OFF_MAX; off += 4) {
        int hits = score_offset(node, off);
        if (hits > best_hits) { best_hits = hits; best_off = off; }
    }
    if (!best_hits) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "no UObject::Class offset validated; scalars stay unread");
        return 0;
    }

    g_class_off = best_off;
    find_mask_offset(node);
    InterlockedExchange(&g_ready, 1);

    if (g_mask_off)
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "UObject::Class +0x%X (%d fields), BitMask +0x%X",
                    g_class_off, best_hits, g_mask_off);
    else
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "UObject::Class +0x%X (%d fields), no BitMask -- "
                    "bools read as a whole dword",
                    g_class_off, best_hits);
    return 1;
}

uint32_t props_class_offset(void) { return g_class_off; }

int props_ready(void) { return g_ready != 0; }

// Resolving a name per property per UI call would mean several dereferences
// and a GNames lookup on the game's UI thread.  There are only a handful of
// property classes in the whole engine, so each UClass* is classified once
// and thereafter recognised by pointer.
#define KIND_CACHE 32
static struct { const void* cls; PropKind kind; } g_cache[KIND_CACHE];
static int g_ncache;

static PropKind classify(const char* name)
{
    if (strcmp(name, "IntProperty")   == 0) return PROP_INT;
    if (strcmp(name, "BoolProperty")  == 0) return PROP_BOOL;
    if (strcmp(name, "FloatProperty") == 0) return PROP_FLOAT;
    if (strcmp(name, "ByteProperty")  == 0) return PROP_BYTE;
    return PROP_UNKNOWN;
}

PropKind props_kind(const void* prop)
{
    if (!g_ready || !prop) return PROP_UNKNOWN;
    if (!readable((const uint8_t*)prop + g_class_off, sizeof(void*)))
        return PROP_UNKNOWN;

    const void* cls = *(const void* const*)((const uint8_t*)prop + g_class_off);
    if (!cls) return PROP_UNKNOWN;

    for (int i = 0; i < g_ncache; i++)
        if (g_cache[i].cls == cls) return g_cache[i].kind;

    char name[96];
    PropKind k = PROP_UNKNOWN;
    if (readable(cls, UOBJECT_NAME + sizeof(FName)) &&
        object_name(cls, name, sizeof name))
        k = classify(name);

    if (g_ncache < KIND_CACHE) {
        g_cache[g_ncache].cls = cls;
        g_cache[g_ncache].kind = k;
        g_ncache++;
    }
    return k;
}

int props_read_bool(const void* prop, const uint8_t* base, int* out)
{
    if (!g_ready || !prop || !base) return 0;
    if (!readable((const uint8_t*)prop + UPROPERTY_OFFSET, sizeof(uint32_t))) return 0;

    uint32_t off = *(const uint32_t*)((const uint8_t*)prop + UPROPERTY_OFFSET);
    if (off >= 0x1000) return 0;
    if (!readable(base + off, sizeof(uint32_t))) return 0;

    uint32_t word = *(const uint32_t*)(base + off);
    uint32_t mask = 0xFFFFFFFFu;
    if (g_mask_off && readable((const uint8_t*)prop + g_mask_off, sizeof(uint32_t)))
        mask = *(const uint32_t*)((const uint8_t*)prop + g_mask_off);

    *out = (word & mask) != 0;
    return 1;
}

int props_read_object_bool(const void* prop, const uint8_t* obj, int* out)
{
    if (!g_ready || !prop || !obj) return 0;
    if (!readable((const uint8_t*)prop + UPROPERTY_OFFSET, sizeof(uint32_t))) return 0;
    uint32_t off = *(const uint32_t*)((const uint8_t*)prop + UPROPERTY_OFFSET);
    if (off >= 0x10000 || !readable(obj + off, sizeof(uint32_t))) return 0;
    uint32_t word = *(const uint32_t*)(obj + off);
    uint32_t mask = 0xFFFFFFFFu;
    if (g_mask_off && readable((const uint8_t*)prop + g_mask_off, sizeof(uint32_t)))
        mask = *(const uint32_t*)((const uint8_t*)prop + g_mask_off);
    *out = (word & mask) != 0;
    return 1;
}

uint32_t props_mask_offset(void) { return g_mask_off; }

int props_learn_mask(const void* node)
{
    if (g_mask_off) return 1;
    if (!g_ready || !node) return 0;
    find_mask_offset(node);
    return g_mask_off != 0;
}
