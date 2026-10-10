// The mod's options. See settings.h.

#include "settings.h"
#include "strings.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* key;            // in the file
    StrId       name;           // in the menu
    int         lo, hi, dflt;
    const StrId* words;         // one per value, or NULL for On / Off
} Spec;

// A level's names, notch 0..4. Five notches three decibels apart around
// the built-in level, which is the middle one (audio.c, VOLUME_NOTCHES and
// VOLUME_MIDDLE); learn_apply_levels checks the mixer agrees.
static const StrId LEVEL_WORDS[] = { SET_LEVEL_QUIETEST, SET_LEVEL_QUIET, SET_LEVEL_NORMAL,
                                     SET_LEVEL_LOUD, SET_LEVEL_LOUDEST };
static const StrId GLIDE_WORDS[] = { SET_GLIDE_SLOW, SET_GLIDE_NORMAL, SET_GLIDE_FAST };

static const Spec SPEC[SET_COUNT] = {
    [SET_FIELD]       = { "WallField",    SET_NAME_FIELD,          0, 1, 1, NULL },
    [SET_WALL_LEVEL]  = { "WallLevel",    SET_NAME_WALL_LEVEL,    0, 4, 2, LEVEL_WORDS },
    [SET_HEARTS]      = { "Hearts",       SET_NAME_HEARTS,     0, 1, 1, NULL },
    [SET_HEART_SOLO]  = { "HeartSolo",    SET_NAME_HEART_SOLO,  0, 1, 0, NULL },
    [SET_HEART_LEVEL] = { "HeartLevel",   SET_NAME_HEART_LEVEL, 0, 4, 2, LEVEL_WORDS },
    [SET_ALIENS]      = { "Aliens",       SET_NAME_ALIENS,    0, 1, 1, NULL },
    [SET_ALIEN_LEVEL] = { "AlienLevel",   SET_NAME_ALIEN_LEVEL, 0, 4, 2, LEVEL_WORDS },
    [SET_DOORS]       = { "Doors",        SET_NAME_DOORS,         0, 1, 1, NULL },
    [SET_DOOR_LEVEL]  = { "DoorLevel",    SET_NAME_DOOR_LEVEL,    0, 4, 2, LEVEL_WORDS },
    [SET_WINDOWS]     = { "Windows",      SET_NAME_WINDOWS,       0, 1, 1, NULL },
    [SET_WINDOW_LEVEL] = { "WindowLevel", SET_NAME_WINDOW_LEVEL,  0, 4, 2, LEVEL_WORDS },
    [SET_STEPS]       = { "Steps",        SET_NAME_STEPS, 0, 1, 1, NULL },
    [SET_STEP_LEVEL]  = { "StepLevel",    SET_NAME_STEP_LEVEL, 0, 4, 2, LEVEL_WORDS },
    [SET_DAYS]        = { "DayTick",      SET_NAME_DAYS,   0, 1, 1, NULL },
    [SET_DAY_LEVEL]   = { "DayTickLevel", SET_NAME_DAY_LEVEL,   0, 4, 2, LEVEL_WORDS },
    [SET_GLIDE]      = { "GlideSpeed",   SET_NAME_GLIDE,         0, 2, GLIDE_NORMAL, GLIDE_WORDS },
    [SET_COMBAT]     = { "Combat",       SET_NAME_COMBAT,    0, 1, 1, NULL },
    [SET_SIGHT]      = { "Sightings",    SET_NAME_SIGHT,     0, 1, 1, NULL },
    [SET_TURN]       = { "TurnHandover", SET_NAME_TURN,        0, 1, 1, NULL },
    [SET_TICKER]     = { "Ticker",       SET_NAME_TICKER,      0, 1, 1, NULL },
    [SET_OBJECTIVES] = { "Objectives",   SET_NAME_OBJECTIVES,   0, 1, 1, NULL },
    [SET_NARRATIVE]  = { "Narrative",    SET_NAME_NARRATIVE,  0, 1, 1, NULL },
    [SET_MOUSE]      = { "BlockMouse",   SET_NAME_MOUSE,     0, 1, 1, NULL },
    [SET_DEBUG]      = { "DebugLog",     SET_NAME_DEBUG,           0, 1, 1, NULL },
};

#define SECTION "settings"

static volatile LONG g_value[SET_COUNT];
static char          g_path[MAX_PATH];
static int           g_ready;

static void defaults(void)
{
    for (int i = 0; i < SET_COUNT; i++) InterlockedExchange(&g_value[i], SPEC[i].dflt);
    g_ready = 1;
}

static int clamp(int id, int v)
{
    if (v < SPEC[id].lo) return SPEC[id].lo;
    if (v > SPEC[id].hi) return SPEC[id].hi;
    return v;
}

void settings_reset(void)
{
    defaults();
}

void settings_load(const char* dir, char* why, size_t why_sz)
{
    defaults();
    g_path[0] = 0;
    if (why && why_sz) why[0] = 0;
    if (!dir) return;
    _snprintf_s(g_path, sizeof g_path, _TRUNCATE, "%sxcom_uihook_settings.ini", dir);
    int found = 0;
    for (int i = 0; i < SET_COUNT; i++) {
        // -1 is outside every range, so it marks a key the file lacks.
        int v = (int)GetPrivateProfileIntA(SECTION, SPEC[i].key, -1, g_path);
        if (v == -1) continue;
        InterlockedExchange(&g_value[i], clamp(i, v));
        found++;
    }
    if (why) _snprintf_s(why, why_sz, _TRUNCATE, "%d of %d read from %s",
                         found, SET_COUNT, g_path);
}

static void save(int id)
{
    if (!g_path[0]) return;
    char v[16];
    _snprintf_s(v, sizeof v, _TRUNCATE, "%d", (int)g_value[id]);
    WritePrivateProfileStringA(SECTION, SPEC[id].key, v, g_path);
}

int settings_get(int id)
{
    if (id < 0 || id >= SET_COUNT) return 0;
    if (!g_ready) defaults();
    return (int)g_value[id];
}

int settings_set(int id, int value)
{
    if (id < 0 || id >= SET_COUNT) return 0;
    if (!g_ready) defaults();
    value = clamp(id, value);
    InterlockedExchange(&g_value[id], value);
    save(id);
    return value;
}

int settings_is_switch(int id)
{
    return id >= 0 && id < SET_COUNT && !SPEC[id].words;
}

int settings_step(int id, int delta)
{
    if (id < 0 || id >= SET_COUNT || !delta) return settings_get(id);
    if (settings_is_switch(id)) return settings_set(id, !settings_get(id));
    return settings_set(id, settings_get(id) + (delta > 0 ? 1 : -1));
}

const char* settings_name(int id)
{
    return id >= 0 && id < SET_COUNT ? T(SPEC[id].name) : "";
}

void settings_value_text(int id, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    if (id < 0 || id >= SET_COUNT) return;
    int v = settings_get(id);
    const char* w = T(SPEC[id].words ? SPEC[id].words[v - SPEC[id].lo] : v ? SET_ON : SET_OFF);
    strncpy_s(out, out_sz, w, _TRUNCATE);
}
