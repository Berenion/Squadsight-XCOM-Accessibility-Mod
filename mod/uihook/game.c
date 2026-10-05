// Reading and asking the game from outside: see game.h.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "log.h"
#include "cursor.h"
#include "props.h"
#include "objects.h"
#include "names.h"
#include "ue3.h"

// The natives' slots, -1 until tile_arm (main.c) finds them.
int   g_tile_slot_cover = -1, g_tile_slot_smoke = -1, g_tile_slot_poison = -1;
int   g_tile_slot_occupied = -1;
int   g_tile_slot_unitblock = -1;
int   g_tile_slot_onfloor = -1;
int   g_tile_slot_standable = -1;
int   g_tile_slot_floorz = -1;
int   g_unit_slot_visible = -1;
int   g_unit_slot_alive = -1;
int   g_unit_slot_overwatch = -1;
int   g_panel_slot_visible = -1;
int   g_cursor_slot_floor = -1;
int   g_world_slot_seetile = -1;
int   g_world_slot_vismap = -1;
int   g_world_slot_kinetic = -1;
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

// UStructProperty::Struct: the first guess, the bool's BitMask offset, found
// nothing live. So a short window past UProperty's own fields is searched for
// the one pointer whose class is ScriptStruct, and the answer is kept. Moved
// here from main.c's struct_bool for the countries (countries.c).
static uint32_t g_struct_ptr_off;

const void* field_struct(const void* obj, const char* field, uint32_t* field_off)
{
    const uint8_t* sp = (const uint8_t*)object_field_prop(obj, field);
    if (!sp || !readable(sp + UPROPERTY_OFFSET, sizeof(uint32_t))) return NULL;
    if (!g_struct_ptr_off) {
        for (uint32_t off = UPROPERTY_OFFSET + 4; off <= 0x90 && !g_struct_ptr_off; off += 4) {
            if (!readable(sp + off, sizeof(void*))) break;
            void* cand = *(void* const*)(sp + off);
            char cls[64];
            if (cand && readable(cand, 0x40) &&
                object_class_name(cand, cls, sizeof cls) && strcmp(cls, "ScriptStruct") == 0)
                g_struct_ptr_off = off;
        }
        if (!g_struct_ptr_off) return NULL;
        logf_("struct: UStructProperty::Struct at +0x%X\n", g_struct_ptr_off);
    }
    if (!readable(sp + g_struct_ptr_off, sizeof(void*))) return NULL;
    if (field_off) *field_off = *(const uint32_t*)(sp + UPROPERTY_OFFSET);
    return *(void* const*)(sp + g_struct_ptr_off);
}

int struct_member(const void* st, const char* member, uint32_t* off, const void** prop)
{
    void* m = st && readable((const uint8_t*)st + USTRUCT_CHILDREN, sizeof(void*))
                  ? *(void* const*)((const uint8_t*)st + USTRUCT_CHILDREN) : NULL;
    for (int guard = 0; m && guard < MAX_FIELDS; guard++) {
        char name[64];
        if (!readable(m, 0x68)) return 0;
        if (object_name(m, name, sizeof name) && strcmp(name, member) == 0) {
            if (off) *off = *(const uint32_t*)((const uint8_t*)m + UPROPERTY_OFFSET);
            if (prop) *prop = m;
            return 1;
        }
        m = *(void**)((uint8_t*)m + UFIELD_NEXT);
    }
    return 0;
}

// How many lookups missed every slot and walked the class chain; the perf
// line reports it, since a walk is the expensive part of a field read.
unsigned g_field_walks;
unsigned g_field_climbs;

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
        uint32_t off = 0;
        int known = 0;
        // Below a class already known to declare it: the same offset, found
        // by climbing SuperStruct rather than reading every member's name.
        for (int i = 0; i < FIELD_OWNERS && !known; i++)
            if (slot->owner[i] && class_derives(cls, slot->owner[i])) {
                off = slot->owner_off[i];
                known = 1;
            }
        if (known) {
            g_field_climbs++;
        } else {
            const void* owner = NULL;
            g_field_walks++;
            if (!object_field_owner(obj, name, &off, &owner)) {
                slot->absent[slot->next_absent] = cls;
                slot->next_absent = (uint8_t)((slot->next_absent + 1) % FIELD_MISSES);
                // A class that cannot be read is not a missing field, it is a
                // dead object, and the answer is to stop holding the pointer --
                // which is whoever is holding it to say, not this. Saying it
                // here filled a log with "on an unreadable class" and named
                // neither the object nor anything that could be done about it.
                char cls_name[128];
                if (object_class_name(obj, cls_name, sizeof cls_name))
                    logf_("field: no %s on %s\n", name, cls_name);
                return 0;
            }
            if (owner) {
                int o = slot->next_owner;
                slot->owner[o] = owner;
                slot->owner_off[o] = off;
                slot->next_owner = (uint8_t)((o + 1) % FIELD_OWNERS);
            }
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

// Reads an FString.  Num counts the terminating NUL.
int read_fstring(const FString* s, char* out, size_t out_sz)
{
    if (!readable(s, sizeof *s)) return 0;
    if (s->Num < 2 || s->Num > FSTRING_MAX) return 0;
    if (s->Max < s->Num) return 0;
    if (!readable(s->Data, (size_t)s->Num * sizeof(wchar_t))) return 0;
    if (s->Data[s->Num - 1] != 0) return 0;

    for (int i = 0; i < s->Num - 1; i++) {
        wchar_t c = s->Data[i];
        if (c == 0) return 0;
        if (c < 32 && c != '\n' && c != '\t' && c != '\r') return 0;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, s->Data, s->Num - 1,
                                out, (int)out_sz - 1, NULL, NULL);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

int fault_note(EXCEPTION_POINTERS* ep, Fault* f)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    f->code = r->ExceptionCode;
    f->at = r->ExceptionAddress;
    f->has_addr = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                  r->NumberParameters >= 2;
    f->access = f->has_addr ? r->ExceptionInformation[0] : 0;
    f->addr = f->has_addr ? r->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

void fault_log(const char* prefix, const Fault* f, const char* where)
{
    char mod[MAX_PATH] = "?";
    uintptr_t rva = (uintptr_t)f->at;
    HMODULE m;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)f->at, &m) &&
        GetModuleFileNameA(m, mod, sizeof mod)) {
        rva -= (uintptr_t)m;
        char* slash = strrchr(mod, '\\');
        if (slash) memmove(mod, slash + 1, strlen(slash + 1) + 1);
    }
    char access[48] = "";
    if (f->has_addr)
        _snprintf_s(access, sizeof access, _TRUNCATE, ", %s %p",
                    f->access == 1 ? "writing" : f->access == 8 ? "executing" : "reading",
                    (void*)f->addr);
    logf_("%s faulted (0x%08lx) at %s+0x%X%s%s%s\n", prefix, f->code, mod,
          (unsigned)rva, access, where && *where ? ", in " : "", where ? where : "");
}
