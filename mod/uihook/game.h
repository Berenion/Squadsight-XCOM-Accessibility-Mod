#pragma once
#include <windows.h>
#include <stdint.h>
#include "tile.h"
#include "ue3.h"

// Reading and asking the game from outside: the primitives every feature
// that touches a game object is built on.
//
//   readable / writable   VirtualQuery before any read or write of the
//                         game's memory, which a stray access would take down.
//   field_ptr             a property by name, its offset cached per class.
//   tile_vfn              a native, through the vtable slot tile_arm (main.c)
//                         resolves at startup from the exe's native table.
//   unit_is_live          the object table's word that a pointer is still an
//                         object, before anything is called on it.

// A pointer is only followed once the whole range is committed and readable.
int readable(const void* p, size_t n);

// The same, for the few places this DLL writes into the game.
int writable(const void* p, size_t n);

// An FString, as UTF-8; 0 unless it is plainly text. Num counts the NUL.
#define FSTRING_MAX 4096
int read_fstring(const FString* s, char* out, size_t out_sz);

// Whether an object is still one: objects_live, or yes while the object
// table has not been found (unit_is_live in game.c says why).
int unit_is_live(void* obj);

// ---- a field, by name ------------------------------------------------------
//
// Where a field lookup's answer is kept, for one call site.
//
// Both answers, deliberately. A miss is the expensive one: field_find walks
// every child of every class up the chain -- XGUnit alone declares over 700
// members -- decoding a name for each, and gives up only at Object. Keeping
// only the hit meant that a class *without* the field paid that walk on every
// single call, and a mission spent 311 of them on one lookup.
//
// Several classes rather than one, because the classes alternate. The unit
// flags are walked in a row, and the two whose class has no m_kUnit sit among
// fourteen whose class does; a single slot would have each of them evicting
// the other, which is how the miss got expensive in the first place. It was
// one hit and one miss until the heartbeats: they read every unit's pawn
// seven times a second, and a squad's pawns are several classes (soldiers,
// SHIVs, each kind of alien), so `Location` was walked for afresh on nearly
// every unit -- up to 105 ms of a frame, and 20-30 frames a second against 58
// with the hearts off (2026-09-23, 21:58 log). Oldest out when full.
// Children walked at most, per class, when a walk has no other end.
#define MAX_FIELDS   64

//
// And the class that declares the field, once a walk has found it (refactor
// step 5): `Location` is Actor's and `m_kPawn` XGUnitNativeBase's, so every
// pawn and unit class has them at one offset. A class not yet in `on` is
// settled by climbing its SuperStruct chain to a known declarer -- pointer
// comparisons -- and a mission with more than eight pawn classes no longer
// walks the property chain by name each time one is evicted. Several
// declarers, since one name can be declared on unrelated classes.
#define FIELD_HITS   8
#define FIELD_MISSES 4
#define FIELD_OWNERS 4
typedef struct {
    void*       on[FIELD_HITS];        // classes the offset was found on
    uint32_t    off[FIELD_HITS];       // and where, for each
    void*       absent[FIELD_MISSES];  // classes proved not to have the field
    const void* owner[FIELD_OWNERS];   // classes that declare it
    uint32_t    owner_off[FIELD_OWNERS];
    uint8_t     next_on, next_absent, next_owner;  // the next entry to replace
} FieldSlot;

// A pointer to `size` readable bytes of obj's field `name`, in *out; 0 when
// the object has no such field or it cannot be read. Logs "field: no X on
// Class" once per class that lacks it.
int field_ptr(void* obj, const char* name, FieldSlot* slot, size_t size, const void** out);

// The ScriptStruct behind obj's struct field `field`, and the field's offset
// in *field_off; NULL when there is no such field or its struct cannot be
// found. UStructProperty::Struct is not at a known offset, so the first call
// searches for it and keeps the answer (log "struct: UStructProperty::Struct
// at +0x..").
const void* field_struct(const void* obj, const char* field, uint32_t* field_off);

// Where `member` sits inside a ScriptStruct, and its UProperty; 0 when the
// struct has no such member.
int struct_member(const void* st, const char* member, uint32_t* off, const void** prop);

// How many lookups missed every slot and walked the class chain, and how
// many were settled instead by climbing to a known declaring class, for the
// perf line.
extern unsigned g_field_walks;
extern unsigned g_field_climbs;

// ---- the game's natives ----------------------------------------------------
//
// The game's own answers, asked of its C++ directly: see tile.h for why the
// vtable, and how the slots are found. These are pure queries, called on the
// game thread from inside one of its own natives, which is where the script
// would have called them from. Every slot is -1 until tile_arm finds it, and
// tile_vfn answers NULL for -1.
typedef int (__fastcall* TileCoverFn)(void* self, void* edx, float x, float y,
                                      float z, TileCoverPoint* out);
typedef int (__fastcall* TileTestFn)(void* self, void* edx, int x, int y, int z);
typedef int (__fastcall* UnitTestFn)(void* self, void* edx);
// GetAmmoCost(int iWeapon, int iAbility, bool bHasAmmoConservation,
// optional out TCharacter kCharacter, optional bool bReactionFire): the
// TCharacter goes by pointer and is read, so it must be a real one.
typedef int (__fastcall* AmmoCostFn)(void* self, void* edx, int weapon, int ability,
                                     int conserve, const void* character, int reaction);
typedef int (__fastcall* CursorFloorFn)(void* self, void* edx, float x, float y, float z);
// IsPositionOnFloor / IsPositionOnFloorAndValidDestination(const out Vector):
// an `out` Vector goes by pointer, not as three floats.
typedef int (__fastcall* PositionTestFn)(void* self, void* edx, const float* pos);

// XComWorldData.CanSeeActorToTile(Actor FromActor, int X, int Y, int Z,
//                                 optional bool bUseLineChecks)
// The actor is the enemy's PAWN, as XGPlayer.IsEnemyUnitVisibleFromTile
// passes it. An optional script parameter is still a real C++ one.
typedef int (__fastcall* SeeTileFn)(void* self, void* edx, void* from_actor,
                                    int x, int y, int z, int line_checks);

// XGUnitNativeBase.IsFlankingCoverPoint(XComCoverPoint kCover) -- `self` is
// the ENEMY, and the cover point goes by value as GetCoverPoint's Vector
// does. One argument, so nothing can be knocked out of place behind it.
typedef int (__fastcall* FlankCoverFn)(void* self, void* edx, TileCoverPoint cover);

// XGUnitNativeBase.IsPointWithinFiringRange(out float fHeightBonusModifier,
//     out float fDistSq, XGUnitNativeBase kTarget, Vector vTargetPoint,
//     Vector vShooterLocation, optional XGWeapon kWeapon,
//     optional float fOverrideRange)
// Two Vectors by value, six floats in declaration order.
typedef int (__fastcall* FiringRangeFn)(void* self, void* edx,
                                        float* height_bonus, float* dist_sq,
                                        void* target,
                                        float tx, float ty, float tz,
                                        float sx, float sy, float sz,
                                        void* weapon, float override_range);

// float GetFloorZForPosition(const out Vector Position, optional bool
// bUnlimitedSearch): the out Vector goes by pointer, as for IsPositionOnFloor,
// and a float comes back in st(0) whatever the convention. When it finds no
// floor it hands back the height it was given (XGUnit.IsAttemptingToHover
// relies on that).
typedef float (__fastcall* FloorZFn)(void* self, void* edx, const float* pos, int unlimited);

// Volume.EncompassesPoint(Vector Loc): its thunk calls AVolume::Encompasses
// outright, not through the vtable, with the point and a zero extent -- two
// Vectors by value, six floats (`ret 0x18`, EW 2026-09-27).
typedef int (__fastcall* EncompassFn)(void* self, void* edx, float px, float py, float pz,
                                      float ex, float ey, float ez);

// XGUnitNativeBase.IsFlankedBy_EnemyAtLocation(XGUnitNativeBase kEnemy,
//     const out Vector vEnemyLocation, optional bool bDebugLog) -- `self` is
// the unit that would BE flanked; a `const out` Vector goes by pointer.
typedef int (__fastcall* FlankedByFn)(void* self, void* edx, void* enemy,
                                      const float* enemy_loc, int debug_log);

// XComWorldData.IsTileBlockedByUnitFlag(const out int X, const out int Y,
// const out int Z, Actor IgnoreUnit): whether a unit's blocking flag is set on
// the tile (SetTileBlockedByUnitFlag, by any unit, seen or not). `const out`
// ints go by pointer.
typedef int (__fastcall* TileUnitBlockFn)(void* self, void* edx, const int* x, const int* y,
                                          const int* z, void* ignore);

// XComWorldData.GetKineticStrikeInfoFromTargetLocation(XComUnitPawnNativeBase
//     UnitPawn, const out Vector TargetLoc, out KineticStrikeAttackInfo
//     AttackInfo): what a MEC's kinetic strike aimed at TargetLoc would hit,
// as XGUnit's firing code asks it. Both `out`s go by pointer.
typedef struct KineticStrikeInfo {
    float   dir[3];             // AttackDirection
    uint8_t cover_type;         // DestroyCoverType: 0 none, 1 standing (high), 2 mid (low)
    uint8_t pad[3];
    float   cover_loc[3];       // DestroyCoverLocation
    void*   unit;               // AttackUnit, an XGUnitNativeBase
    int32_t unit_dz;            // AttackUnitDeltaZ
} KineticStrikeInfo;
typedef void (__fastcall* KineticStrikeFn)(void* self, void* edx, void* pawn,
                                           const float* target, KineticStrikeInfo* out);

extern int   g_tile_slot_cover, g_tile_slot_smoke, g_tile_slot_poison;
extern int   g_tile_slot_occupied;      // XComWorldData.IsTileOccupied
extern int   g_tile_slot_unitblock;     // XComWorldData.IsTileBlockedByUnitFlag
extern int   g_tile_slot_onfloor;       // XComWorldData.IsPositionOnFloor
extern int   g_tile_slot_standable;     // ...OnFloorAndValidDestination
extern int   g_tile_slot_floorz;        // XComWorldData.GetFloorZForPosition
extern int   g_unit_slot_visible;       // XGUnitNativeBase.IsAliveAndVisible
extern int   g_unit_slot_alive;         // XGUnitNativeBase.IsAlive
extern int   g_unit_slot_overwatch;     // XGUnitNativeBase.IsInOverwatch
extern int   g_core_slot_ammocost;      // XGTacticalGameCoreNativeBase.GetAmmoCost
extern int   g_panel_slot_visible;      // UI_FxsPanel.IsVisible
extern int   g_cursor_slot_floor;       // XCom3DCursor.WorldZToCursorFloor
extern int   g_world_slot_seetile;      // XComWorldData.CanSeeActorToTile
extern int   g_world_slot_vismap;       // XComWorldData.GetVisibilityMapTileIndex
extern int   g_world_slot_kinetic;      // XComWorldData.GetKineticStrikeInfoFromTargetLocation
extern int   g_unit_slot_flanking;      // XGUnitNativeBase.IsFlankingCoverPoint
extern void* g_unit_fn_flanking;        // ...which is final, so not virtual
extern int   g_volume_slot_encompass;   // Volume.EncompassesPoint
extern void* g_volume_fn_encompass;     // ...called outright by its thunk
extern int   g_unit_slot_range;         // XGUnitNativeBase.IsPointWithinFiringRange
extern int   g_unit_slot_flankedby;     // XGUnitNativeBase.IsFlankedBy_EnemyAtLocation

// The game's image, to check a vtable entry points into it before calling it.
// Set by tile_arm.
extern uint8_t* g_image_lo;
extern uint8_t* g_image_hi;

// A virtual function of `obj`, or NULL when the slot is unknown or the entry
// does not point into the game's image.
void* tile_vfn(void* obj, int slot);

// A value no measurement will be, so an out-parameter the native never wrote
// is not mistaken for an answer.
#define SENTINEL_FLOAT 1.0e9f

// ---- when a call into the game faults --------------------------------------
//
// Every call into the game sits in __try: GUARDED (below) for one statement,
// or for a longer body
//     __except (fault_note(GetExceptionInformation(), &f)) { fault_log("what", &f, NULL); }
// What a fault was doing, taken in the exception filter while the record is
// still available: the code address, as module+offset so it can be found in a
// disassembly of this DLL, and the address it tried to read or write.
typedef struct {
    DWORD     code;
    void*     at;
    ULONG_PTR access;     // 0 read, 1 write, 8 execute
    ULONG_PTR addr;
    int       has_addr;
} Fault;

int  fault_note(EXCEPTION_POINTERS* ep, Fault* f);    // the filter: always handles
void fault_log(const char* prefix, const Fault* f, const char* where);

// One statement run under that guard (refactor step 7): a fault in it is
// logged as "<label> faulted ..." and then the rest of the arguments run --
// the fallback a caller reads afterwards, or state to put back. Nothing after
// the label when there is nothing to recover. The statement cannot hold a
// comma outside brackets; a body that long is its own __try.
#define GUARDED(label, stmt, ...)                                       \
    do {                                                                \
        Fault guard_fault_;                                             \
        __try { stmt; }                                                 \
        __except (fault_note(GetExceptionInformation(), &guard_fault_)) { \
            fault_log(label, &guard_fault_, NULL);                      \
            __VA_ARGS__;                                                \
        }                                                               \
    } while (0)

// State set before a call that can fault is cleared here, not before each
// return: a fault leaves through none of them. Traps in HANDOFF.md: a
// re-entry flag cleared by hand stayed set after one fault while aiming, and
// every later capture returned at once -- the HUD, the pause menu and the
// dialogue box silent for the rest of the session.
#define CLEARED(stmt, cleanup)                                          \
    do {                                                                \
        __try { stmt; }                                                 \
        __finally { cleanup; }                                          \
    } while (0)
