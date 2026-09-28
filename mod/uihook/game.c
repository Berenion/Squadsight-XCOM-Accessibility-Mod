// Reading and asking the game from outside: see game.h.

#include <windows.h>
#include <stdint.h>
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "props.h"
#include "objects.h"

// The natives' slots, -1 until tile_arm (main.c) finds them.
int   g_tile_slot_cover = -1, g_tile_slot_smoke = -1, g_tile_slot_poison = -1;
int   g_tile_slot_occupied = -1;
int   g_tile_slot_onfloor = -1;
int   g_tile_slot_standable = -1;
int   g_tile_slot_floorz = -1;
int   g_unit_slot_visible = -1;
int   g_unit_slot_alive = -1;
int   g_unit_slot_overwatch = -1;
int   g_panel_slot_visible = -1;
int   g_cursor_slot_floor = -1;
int   g_world_slot_seetile = -1;
int   g_unit_slot_flanking = -1;
void* g_unit_fn_flanking;
int   g_volume_slot_encompass = -1;
void* g_volume_fn_encompass;
int   g_unit_slot_range = -1;
int   g_unit_slot_flankedby = -1;
uint8_t* g_image_lo;
uint8_t* g_image_hi;

// A pointer is only dereferenced after VirtualQuery says the whole range is
// committed and readable -- this runs on the game's own UI thread and a stray
// read would take the process down with it.  names.c and the rest use it too.
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
int writable(const void* p, size_t n)
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

// A virtual function of `obj`, or NULL when the slot is unknown or the entry
// does not point into the game's image.
void* tile_vfn(void* obj, int slot)
{
    if (slot < 0 || !obj || !readable(obj, sizeof(void*))) return NULL;
    uint8_t* vt = *(uint8_t**)obj;
    if (!readable(vt + slot, sizeof(void*))) return NULL;
    uint8_t* fn = *(uint8_t**)(vt + slot);
    if (fn < g_image_lo || fn >= g_image_hi) return NULL;
    return fn;
}

// How many lookups missed every slot and walked the class chain; the perf
// line reports it, since a walk is the expensive part of a field read.
unsigned g_field_walks;

// An object's field, by name: the offset is looked up again whenever the
// object's class is not one this slot has already decided. An offset belongs
// to the class, so that is once per class, not once per unit per key press.
int field_ptr(void* obj, const char* name, FieldSlot* slot,
              size_t size, const void** out)
{
    if (!obj) return 0;
    uint32_t class_off = props_class_offset();
    if (!class_off || !readable((const uint8_t*)obj + class_off, sizeof(void*))) return 0;
    void* cls = *(void* const*)((const uint8_t*)obj + class_off);
    if (!cls) return 0;
    for (int i = 0; i < FIELD_MISSES; i++)
        if (slot->absent[i] == cls) return 0;
    int hit = -1;
    for (int i = 0; i < FIELD_HITS && hit < 0; i++)
        if (slot->on[i] == cls) hit = i;
    if (hit < 0) {
        uint32_t off;
        g_field_walks++;
        if (!object_field_offset(obj, name, &off)) {
            slot->absent[slot->next_absent] = cls;
            slot->next_absent = (uint8_t)((slot->next_absent + 1) % FIELD_MISSES);
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
        hit = slot->next_on;
        slot->on[hit] = cls;
        slot->off[hit] = off;
        slot->next_on = (uint8_t)((hit + 1) % FIELD_HITS);
    }
    const uint8_t* v = (const uint8_t*)obj + slot->off[hit];
    if (!readable(v, size)) return 0;
    *out = v;
    return 1;
}

// Liveness, where not knowing is not a reason to go quiet.
//
// objects_live can only answer once GObjObjects has been found. Without it
// the mod has no way to ask, and refusing every unit would cost the whole
// units readout -- who is on a tile, the radar, the squad list -- to guard
// against a fault a build with no table cannot be protected from anyway. So
// an unknown table means carry on, exactly as the mod did before this guard
// existed. The table has been found on every run so far.
int unit_is_live(void* obj)
{
    return !objects_ready() || objects_live(obj);
}
