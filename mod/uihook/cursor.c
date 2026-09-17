// Finding the battle cursor and reading where it stands.  See cursor.h.

#include "cursor.h"
#include "props.h"
#include "names.h"
#include "ue3.h"
#include <string.h>
#include <stdio.h>

static void*    g_cursor;
static uint32_t g_location_off;
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
        g_resolved = 0;
        g_tried = 0;
    }
}

void* cursor_object(void) { return g_cursor; }

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

// The offset of a named property, searched up the class chain.
static int field_offset(const void* cls, const char* want, uint32_t* out)
{
    for (int depth = 0; cls && depth < 24; depth++) {
        if (!readable((const uint8_t*)cls + USTRUCT_CHILDREN, sizeof(void*)))
            return 0;
        const void* field = *(const void* const*)((const uint8_t*)cls + USTRUCT_CHILDREN);

        for (int guard = 0; field && guard < 512; guard++) {
            if (!readable(field, 0x68)) break;
            char name[128];
            if (object_name((void*)field, name, sizeof name) &&
                strcmp(name, want) == 0) {
                *out = *(const uint32_t*)((const uint8_t*)field + UPROPERTY_OFFSET);
                return 1;
            }
            field = *(const void* const*)((const uint8_t*)field + UFIELD_NEXT);
        }

        if (!readable((const uint8_t*)cls + g_super_off, sizeof(void*))) return 0;
        cls = *(const void* const*)((const uint8_t*)cls + g_super_off);
    }
    return 0;
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

    g_resolved = 1;
    _snprintf_s(why, why_sz, _TRUNCATE,
                "class %s, SuperStruct +0x%X, Location +0x%X",
                cls_name, g_super_off, g_location_off);
    return 2;
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

int cursor_grid(CursorGrid* g)
{
    if (!g_world_resolved || !g_world) return 0;
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
    return 1;
}

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
