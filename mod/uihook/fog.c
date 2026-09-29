// The fog of war, per tile. See fog.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "fog.h"
#include "game.h"
#include "log.h"

// ---- where the fog is kept -------------------------------------------------
//
// XCOM paints the fog as a volume texture over the level: XComFOWEffect's
// FogColor (black) on a tile never seen, HaveSeenTintColor (grey) on one seen
// before, nothing on one in sight. No script function answers for a tile --
// XCOM 2's GetTileFOWValue does not exist here (HANDOFF, "Fog gating") -- but
// the bytes the texture is filled from are a script property:
// XComWorldData.FOWUpdateTextureBuffer, an array<byte>, which field_ptr reads
// like any other array. One byte a tile is the expectation, NumX * NumY * NumZ
// of them, and a buffer of any other size is not read at all.
//
// Which tile a byte belongs to is the game's own answer where it can be had:
// GetVisibilityMapTileIndex(X, Y, Z), exported and native, through its vtable
// slot. Without the slot the index is counted X fastest, then Y, then Z; the
// first answer from the native is logged beside that count, so a log says
// whether they agree.
//
// What a byte means is not documented anywhere, so it is settled from the
// squad rather than assumed: the tile a soldier stands on is in sight, and
// never-seen is the other end of the range from it. A soldier on a 0 means 0
// is sight and 255 the black; on anything else, 0 is the black. Soldiers who
// disagree mean the buffer is not what it is taken for, and nothing is gated
// (FOG_UNKNOWN), which leaves the scanner as it was before the fog.
static FieldSlot g_fow_buffer;

static void* g_fog_world;       // the world data the verdict is for
static int   g_fog_never = -1;  // the byte a never-seen tile holds; -1 unknown
static int   g_fog_said = -2;   // the verdict last logged
static int   g_fog_size_said;   // the buffer size last logged as wrong
static int   g_fog_index_said;  // the native's index logged beside the count

// The buffer, when it is one byte a tile of grid `g`.
static const uint8_t* fog_buffer(void* world, const CursorGrid* g, int* num)
{
    const void* v;
    if (!field_ptr(world, "FOWUpdateTextureBuffer", &g_fow_buffer, sizeof(FArray), &v))
        return NULL;
    const FArray* a = (const FArray*)v;
    int want = g->num_x * g->num_y * g->num_z;
    if (!a->Data || a->Num <= 0) return NULL;
    // The first run (2026-09-29, 12:31 log) had 63612 bytes for 92 x 57 x 12
    // = 62928 tiles: 93 x 57 x 12 exactly, a byte more on every row. So the
    // size alone does not decide, when the game's own index can say where a
    // tile's byte is; only the counted fallback needs the sizes to agree.
    if (a->Num != want) {
        int by_game = g_world_slot_vismap >= 0;
        if (g_fog_size_said != a->Num) {
            g_fog_size_said = a->Num;
            logf_("fog: the buffer is %d bytes for %d x %d x %d = %d tiles -- %s\n",
                  a->Num, g->num_x, g->num_y, g->num_z, want,
                  by_game ? "read by the game's index" : "not read");
        }
        if (!by_game) return NULL;
    }
    *num = a->Num;
    return (const uint8_t*)a->Data;
}

static int fog_index(void* world, const CursorGrid* g, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= g->num_x || y >= g->num_y || z >= g->num_z)
        return -1;
    int counted = x + y * g->num_x + z * g->num_x * g->num_y;
    TileTestFn index = (TileTestFn)tile_vfn(world, g_world_slot_vismap);
    if (!index) return counted;
    int i = -1;
    GUARDED("fog: GetVisibilityMapTileIndex", i = index(world, NULL, x, y, z), i = -1);
    if (!g_fog_index_said) {
        g_fog_index_said = 1;
        logf_("fog: tile %d, %d, %d is index %d to the game, %d counted -- %s\n",
              x, y, z, i, counted, i == counted ? "the same" : "the game's is used");
    }
    return i;
}

// The byte for a tile; -1 when there is none.
static int fog_byte(void* world, const CursorGrid* g, int x, int y, int z)
{
    int num;
    const uint8_t* buf = fog_buffer(world, g, &num);
    if (!buf) return -1;
    int i = fog_index(world, g, x, y, z);
    if (i < 0 || i >= num || !readable(buf + i, 1)) return -1;
    return buf[i];
}

// What the buffer holds, for the log whenever the verdict changes: each value
// and how many tiles have it, most first.
static void fog_log_values(void* world, const CursorGrid* g)
{
    int num;
    const uint8_t* buf = fog_buffer(world, g, &num);
    if (!buf || !readable(buf, (size_t)num)) return;
    static int count[256];
    memset(count, 0, sizeof count);
    for (int i = 0; i < num; i++) count[buf[i]]++;
    char line[512];
    size_t used = 0;
    for (int shown = 0; shown < 10; shown++) {
        int best = -1;
        for (int b = 0; b < 256; b++)
            if (count[b] && (best < 0 || count[b] > count[best])) best = b;
        if (best < 0) break;
        int w = _snprintf_s(line + used, sizeof line - used, _TRUNCATE, "%s%d x%d",
                            used ? ", " : "", best, count[best]);
        if (w > 0) used += (size_t)w;
        count[best] = 0;
    }
    logf_("fog: %d bytes hold %s\n", num, used ? line : "nothing");
}

void fog_calibrate(const CursorGrid* g, const int (*tiles)[3], const char* const* names, int n)
{
    void* world = cursor_world();
    if (world != g_fog_world) {
        g_fog_world = world;
        g_fog_never = -1;
        g_fog_said = -2;
        g_fog_size_said = 0;
        g_fog_index_said = 0;
    }
    if (!world || n <= 0) return;

    // Once a map: where the game puts a step along each axis, which is the
    // buffer's layout (the row padding the 12:31 log implied).
    static void* layout_said;
    if (layout_said != world && g_world_slot_vismap >= 0) {
        layout_said = world;
        logf_("fog: the game indexes 0,0,0 at %d; 1,0,0 at %d; 0,1,0 at %d; 0,0,1 at %d\n",
              fog_index(world, g, 0, 0, 0), fog_index(world, g, 1, 0, 0),
              fog_index(world, g, 0, 1, 0), fog_index(world, g, 0, 0, 1));
    }

    int zero = 0, other = 0;
    char seen[256];
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        int b = fog_byte(world, g, tiles[i][0], tiles[i][1], tiles[i][2]);
        if (b < 0) continue;
        if (b == 0) zero++; else other++;
        int w = _snprintf_s(seen + used, sizeof seen - used, _TRUNCATE, "%s%s %d",
                            used ? ", " : "", names[i], b);
        if (w > 0) used += (size_t)w;
    }
    int never = zero && other ? -1 : zero ? 255 : other ? 0 : -1;
    // Nothing read leaves the verdict there was: a soldier between flags, or
    // a moment with no pawn, says nothing about the buffer.
    if (!zero && !other) return;
    g_fog_never = never;
    if (never == g_fog_said) return;
    g_fog_said = never;
    logf_("fog: under the squad %s -- %s\n", used ? seen : "nothing",
          never < 0 ? "they disagree, so nothing is hidden by the fog"
                    : never == 0 ? "never seen is 0" : "never seen is 255");
    fog_log_values(world, g);
}

int fog_tile(const CursorGrid* g, int x, int y, int z)
{
    void* world = cursor_world();
    if (!world || world != g_fog_world || g_fog_never < 0) return FOG_UNKNOWN;
    int b = fog_byte(world, g, x, y, z);
    if (b < 0) return FOG_UNKNOWN;
    return b == g_fog_never ? FOG_NEVER : FOG_SEEN;
}
