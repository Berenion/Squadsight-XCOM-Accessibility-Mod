// Finding the battle cursor and reading where it stands.  See cursor.h.

#include "cursor.h"
#include "props.h"
#include "names.h"
#include "ue3.h"
#include "objects.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

static void*    g_cursor;
static uint32_t g_location_off;
static uint32_t g_chained_off;
static int      g_resolved;
static int      g_tried;

// The grid's owner, XComWorldData; see "the world's grid" below.
static void*    g_world;
static int      g_world_resolved;
static int      g_world_tried;

void cursor_forget(void)
{
    g_cursor = NULL;
    g_location_off = 0;
    g_chained_off = 0;
    g_resolved = 0;
    g_tried = 0;
    g_world = NULL;
    g_world_resolved = 0;
    g_world_tried = 0;
}

void cursor_seen(void* self)
{
    // Runs on the game thread from a native that the cursor's own Tick calls
    // every frame, so it does as little as possible: after the first sighting
    // this is one comparison.
    if (self && self != g_cursor) {
        g_cursor = self;
        g_location_off = 0;
        g_chained_off = 0;
        g_resolved = 0;
        g_tried = 0;
    }
}

void* cursor_object(void) { return g_cursor; }

int cursor_resolved(void) { return g_cursor && g_resolved; }

// UStruct::SuperStruct, probed rather than assumed.
//
// A class only lists its *own* fields in Children, and Location belongs to
// Actor, several classes above XCom3DCursor -- so the chain has to be walked.
// The right offset is the one whose pointers form a chain of named objects
// that ends at "Object", which no wrong offset does: a stray pointer either
// fails to read, fails to resolve to a name, or wanders off without ever
// arriving.  Same judgement as the class-offset probe in props.c.
static uint32_t g_super_off;

static int chain_ends_at_object(const void* cls, uint32_t off)
{
    const void* at = cls;
    for (int depth = 0; depth < 24; depth++) {
        if (!readable((const uint8_t*)at + off, sizeof(void*))) return 0;
        const void* super = *(const void* const*)((const uint8_t*)at + off);
        if (!super) return 0;                 // ran out before reaching Object
        if (!readable(super, 0x60)) return 0;

        char name[128];
        if (!object_name((void*)super, name, sizeof name)) return 0;
        if (name[0] == '?' || !name[0]) return 0;
        if (strcmp(name, "Object") == 0) return 1;
        at = super;
    }
    return 0;
}

static int find_super_offset(const void* cls)
{
    if (g_super_off) return 1;
    // UStruct's own fields live in a small window; Children is already known
    // to be at 0x4C, and SuperStruct sits below it in every UE3 build seen.
    for (uint32_t off = 0x30; off <= 0x4C; off += 4) {
        if (chain_ends_at_object(cls, off)) { g_super_off = off; return 1; }
    }
    return 0;
}

// A named property, searched up the class chain. *owner, when asked for, is
// the class in the chain that declares it.
static const void* field_find_in(const void* cls, const char* want, const void** owner)
{
    for (int depth = 0; cls && depth < 24; depth++) {
        if (!readable((const uint8_t*)cls + USTRUCT_CHILDREN, sizeof(void*)))
            return NULL;
        const void* field = *(const void* const*)((const uint8_t*)cls + USTRUCT_CHILDREN);

        // XGUnit alone declares over 700 members -- functions, states and
        // constants are children too -- and a guard of 512 stopped short of
        // XGUnit.m_kPlayer, silently. The guard is against a cycle, not a
        // budget.
        for (int guard = 0; field && guard < 8192; guard++) {
            if (!readable(field, 0x68)) break;
            char name[128];
            if (object_name((void*)field, name, sizeof name) &&
                strcmp(name, want) == 0) {
                if (owner) *owner = cls;
                return field;
            }
            field = *(const void* const*)((const uint8_t*)field + UFIELD_NEXT);
        }

        if (!readable((const uint8_t*)cls + g_super_off, sizeof(void*))) return NULL;
        cls = *(const void* const*)((const uint8_t*)cls + g_super_off);
    }
    return NULL;
}

static const void* field_find(const void* cls, const char* want)
{
    return field_find_in(cls, want, NULL);
}

static int field_offset(const void* cls, const char* want, uint32_t* out)
{
    const void* field = field_find(cls, want);
    if (!field) return 0;
    *out = *(const uint32_t*)((const uint8_t*)field + UPROPERTY_OFFSET);
    return 1;
}

int cursor_fields(char* why, size_t why_sz)
{
    if (g_resolved) {
        _snprintf_s(why, why_sz, _TRUNCATE, "Location +0x%X", g_location_off);
        return 1;
    }
    if (g_tried) { _snprintf_s(why, why_sz, _TRUNCATE, "already failed"); return 0; }
    g_tried = 1;

    if (!g_cursor) { _snprintf_s(why, why_sz, _TRUNCATE, "no cursor yet"); return 0; }
    uint32_t class_off = props_class_offset();
    if (!class_off) {
        _snprintf_s(why, why_sz, _TRUNCATE, "UObject::Class not probed yet");
        return 0;
    }
    if (!readable((const uint8_t*)g_cursor + class_off, sizeof(void*))) {
        _snprintf_s(why, why_sz, _TRUNCATE, "cursor class unreadable");
        return 0;
    }
    const void* cls = *(const void* const*)((const uint8_t*)g_cursor + class_off);
    char cls_name[128] = "?";
    object_name((void*)cls, cls_name, sizeof cls_name);

    if (!find_super_offset(cls)) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "class %s: no SuperStruct offset reaches Object", cls_name);
        return 0;
    }
    if (!field_offset(cls, "Location", &g_location_off)) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "class %s: no property named Location (super +0x%X)",
                    cls_name, g_super_off);
        return 0;
    }

    // Optional: which soldier the cursor is leashed to. Navigation lets go of
    // its target when this changes, since MoveToUnit has just put the cursor
    // on somebody else. Missing it costs only that, so it is not a failure.
    if (!field_offset(cls, "ChainedPawn", &g_chained_off)) g_chained_off = 0;

    g_resolved = 1;
    char chained[32] = "not found";
    if (g_chained_off)
        _snprintf_s(chained, sizeof chained, _TRUNCATE, "+0x%X", g_chained_off);
    _snprintf_s(why, why_sz, _TRUNCATE,
                "class %s, SuperStruct +0x%X, Location +0x%X, ChainedPawn %s",
                cls_name, g_super_off, g_location_off, chained);
    return 2;
}

int object_field_offset(const void* obj, const char* name, uint32_t* out)
{
    uint32_t class_off = props_class_offset();
    if (!obj || !class_off) return 0;
    if (!readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    const void* cls = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!cls || !readable(cls, 0x60)) return 0;
    if (!find_super_offset(cls)) return 0;
    return field_offset(cls, name, out);
}

int object_field_owner(const void* obj, const char* name, uint32_t* out, const void** owner)
{
    uint32_t class_off = props_class_offset();
    if (!obj || !class_off) return 0;
    if (!readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    const void* cls = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!cls || !readable(cls, 0x60)) return 0;
    if (!find_super_offset(cls)) return 0;
    const void* field = field_find_in(cls, name, owner);
    if (!field) return 0;
    *out = *(const uint32_t*)((const uint8_t*)field + UPROPERTY_OFFSET);
    return 1;
}

int class_derives(const void* cls, const void* base)
{
    if (!cls || !base || !g_super_off) return 0;
    for (int depth = 0; cls && depth < 32; depth++) {
        if (cls == base) return 1;
        if (!readable((const uint8_t*)cls + g_super_off, sizeof(void*))) return 0;
        cls = *(const void* const*)((const uint8_t*)cls + g_super_off);
    }
    return 0;
}

uint32_t object_super_offset(void) { return g_super_off; }

int object_class_name(const void* obj, char* out, size_t out_sz)
{
    if (out && out_sz) out[0] = 0;
    uint32_t class_off = props_class_offset();
    if (!obj || !class_off) return 0;
    if (!readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    const void* cls = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!cls || !readable(cls, 0x60)) return 0;
    return object_name((void*)cls, out, out_sz);
}

int object_is_a(const void* obj, const char* name)
{
    uint32_t class_off = props_class_offset();
    if (!obj || !name || !class_off) return 0;
    if (!readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    const void* cls = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!cls || !readable(cls, 0x60)) return 0;
    if (!find_super_offset(cls)) return 0;

    for (int depth = 0; cls && depth < 32; depth++) {
        char got[128];
        if (!object_name((void*)cls, got, sizeof got)) return 0;
        if (strcmp(got, name) == 0) return 1;
        if (!readable((const uint8_t*)cls + g_super_off, sizeof(void*))) return 0;
        cls = *(const void* const*)((const uint8_t*)cls + g_super_off);
        if (cls && !readable(cls, 0x60)) return 0;
    }
    return 0;
}

const void* object_field_prop(const void* obj, const char* name)
{
    uint32_t class_off = props_class_offset();
    if (!obj || !class_off) return NULL;
    if (!readable((const uint8_t*)obj + class_off, sizeof(void*))) return NULL;
    const void* cls = *(const void* const*)((const uint8_t*)obj + class_off);
    if (!cls || !readable(cls, 0x60)) return NULL;
    if (!find_super_offset(cls)) return NULL;
    return field_find(cls, name);
}

int cursor_chained_pawn(void** out)
{
    if (!g_resolved || !g_cursor || !g_chained_off) return 0;
    const void* const* slot =
        (const void* const*)((const uint8_t*)g_cursor + g_chained_off);
    if (!readable(slot, sizeof *slot)) return 0;
    *out = (void*)*slot;
    return 1;
}

// ---- the world's grid ------------------------------------------------------

static uint32_t g_bounds_off, g_numx_off, g_numy_off, g_numz_off;

void cursor_world_seen(void* world)
{
    if (world && world != g_world) {
        g_world = world;
        g_world_resolved = 0;
        g_world_tried = 0;
    }
}

int cursor_world_fields(char* why, size_t why_sz)
{
    if (g_world_resolved) {
        _snprintf_s(why, why_sz, _TRUNCATE, "WorldBounds +0x%X", g_bounds_off);
        return 1;
    }
    if (g_world_tried) { _snprintf_s(why, why_sz, _TRUNCATE, "already failed"); return 0; }
    if (!g_world) { _snprintf_s(why, why_sz, _TRUNCATE, "no world data yet"); return 0; }
    g_world_tried = 1;

    uint32_t class_off = props_class_offset();
    if (!class_off) {
        // Not a verdict on this object, so let the next call try again.
        g_world_tried = 0;
        _snprintf_s(why, why_sz, _TRUNCATE, "UObject::Class not probed yet");
        return 0;
    }
    if (!readable((const uint8_t*)g_world + class_off, sizeof(void*))) {
        _snprintf_s(why, why_sz, _TRUNCATE, "world data %p: class unreadable", g_world);
        return 0;
    }
    const void* cls = *(const void* const*)((const uint8_t*)g_world + class_off);
    char cls_name[128] = "?";
    object_name((void*)cls, cls_name, sizeof cls_name);

    // The object came out of a return slot, so this is the check that the slot
    // was read correctly: anything else in it would not be this class.
    if (strcmp(cls_name, "XComWorldData") != 0) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "GetWorldData returned %p of class %s, not XComWorldData",
                    g_world, cls_name);
        return 0;
    }
    if (!find_super_offset(cls)) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "class %s: no SuperStruct offset reaches Object", cls_name);
        return 0;
    }
    const char* missing =
        !field_offset(cls, "WorldBounds", &g_bounds_off) ? "WorldBounds" :
        !field_offset(cls, "NumX", &g_numx_off)          ? "NumX" :
        !field_offset(cls, "NumY", &g_numy_off)          ? "NumY" :
        !field_offset(cls, "NumZ", &g_numz_off)          ? "NumZ" : NULL;
    if (missing) {
        _snprintf_s(why, why_sz, _TRUNCATE,
                    "class %s: no property named %s", cls_name, missing);
        return 0;
    }

    g_world_resolved = 1;
    _snprintf_s(why, why_sz, _TRUNCATE,
                "%p class %s, WorldBounds +0x%X, NumX +0x%X, NumY +0x%X, NumZ +0x%X",
                g_world, cls_name, g_bounds_off, g_numx_off, g_numy_off, g_numz_off);
    return 2;
}

// Whether the world data is still one. A load frees the old XComWorldData,
// and until GetWorldData hands over the new one (cursor_world_seen) g_world
// points at freed memory -- which a native then runs on. The 2026-09-28
// (11:26) log has "walls: scan faulted ... executing 00000003" three times
// just after a load: GetCoverPoint, called on the dead object, jumped through
// what was left of it. objects_live asks the object table, as unit_is_live
// does for units; before the table is found it cannot, and the pointer is
// trusted as it always was.
static void* g_world_dead;          // the one already logged as gone

static int world_live(void)
{
    if (!objects_ready() || objects_live(g_world)) return 1;
    if (g_world_dead != g_world) {
        g_world_dead = g_world;
        logf_("cursor: the world data %p is no longer an object -- not asked until "
              "the next one is seen\n", g_world);
    }
    return 0;
}

int cursor_grid(CursorGrid* g)
{
    if (!g_world_resolved || !g_world || !world_live()) return 0;
    const uint8_t* w = (const uint8_t*)g_world;
    uint32_t lo = g_bounds_off, hi = g_bounds_off + 3 * sizeof(float);
    if (g_numx_off < lo) lo = g_numx_off;
    if (g_numy_off < lo) lo = g_numy_off;
    if (g_numz_off < lo) lo = g_numz_off;
    if (g_numx_off + 4 > hi) hi = g_numx_off + 4;
    if (g_numy_off + 4 > hi) hi = g_numy_off + 4;
    if (g_numz_off + 4 > hi) hi = g_numz_off + 4;
    if (!readable(w + lo, hi - lo)) return 0;   // one query covers all four

    const float* min = (const float*)(w + g_bounds_off);   // Box.Min leads
    g->min_x = min[0];
    g->min_y = min[1];
    g->min_z = min[2];
    g->num_x = *(const int32_t*)(w + g_numx_off);
    g->num_y = *(const int32_t*)(w + g_numy_off);
    g->num_z = *(const int32_t*)(w + g_numz_off);
    // A world data not yet filled in: the 12:17 log of 2026-09-28 had "grid:
    // Min 0.0, 0.0, 0.0  size 1061158912 x 1065353216 x 1083182765 tiles"
    // during a load, which put the cursor 41 tiles off the map. Maps are tens
    // of tiles each way.
    if (g->num_x <= 0 || g->num_y <= 0 || g->num_z <= 0 ||
        g->num_x > 1000 || g->num_y > 1000 || g->num_z > 1000)
        return 0;
    return 1;
}

// The tile the cursor stands on, by the native's own arithmetic.
int cursor_tile(const CursorGrid* g, int* tx, int* ty, float* z)
{
    float x, y, cz;
    if (!cursor_position(&x, &y, &cz)) return 0;
    *tx = grid_x(g, x);
    *ty = grid_y(g, y);
    if (z) *z = cz;
    return 1;
}

void* cursor_world(void) { return g_world_resolved && g_world && world_live() ? g_world : NULL; }

int cursor_position(float* x, float* y, float* z)
{
    if (!g_resolved || !g_cursor) return 0;
    const float* v = (const float*)((const uint8_t*)g_cursor + g_location_off);
    if (!readable(v, 3 * sizeof(float))) return 0;
    if (x) *x = v[0];
    if (y) *y = v[1];
    if (z) *z = v[2];
    return 1;
}
