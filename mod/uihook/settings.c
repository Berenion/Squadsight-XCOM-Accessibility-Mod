// The mod's options. See settings.h.

#include "settings.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char* key;            // in the file
    const char* name;           // in the menu
    int         lo, hi, dflt;
    const char* const* words;   // one per value, or NULL for On / Off
} Spec;

// A level's names, notch 0..4. Five notches three decibels apart around
// the built-in level, which is the middle one (audio.c, VOLUME_NOTCHES and
// VOLUME_MIDDLE); learn_apply_levels checks the mixer agrees.
static const char* const LEVEL_WORDS[] = { "Quietest", "Quiet", "Normal", "Loud", "Loudest" };
static const char* const GLIDE_WORDS[] = { "Slow", "Normal", "Fast" };

static const Spec SPEC[SET_COUNT] = {
    [SET_FIELD]       = { "WallField",    "Wall sound",          0, 1, 1, NULL },
    [SET_WALL_LEVEL]  = { "WallLevel",    "Wall sound level",    0, 4, 2, LEVEL_WORDS },
    [SET_HEARTS]      = { "Hearts",       "Ally heartbeats",     0, 1, 1, NULL },
    [SET_HEART_SOLO]  = { "HeartSolo",    "Follow one soldier",  0, 1, 0, NULL },
    [SET_HEART_LEVEL] = { "HeartLevel",   "Ally heartbeat level", 0, 4, 2, LEVEL_WORDS },
    [SET_ALIENS]      = { "Aliens",       "Alien heartbeats",    0, 1, 1, NULL },
    [SET_ALIEN_LEVEL] = { "AlienLevel",   "Alien heartbeat level", 0, 4, 2, LEVEL_WORDS },
    [SET_DOORS]       = { "Doors",        "Door sounds",         0, 1, 1, NULL },
    [SET_DOOR_LEVEL]  = { "DoorLevel",    "Door sound level",    0, 4, 2, LEVEL_WORDS },
    [SET_WINDOWS]     = { "Windows",      "Window sounds",       0, 1, 1, NULL },
    [SET_WINDOW_LEVEL] = { "WindowLevel", "Window sound level",  0, 4, 2, LEVEL_WORDS },
    [SET_STEPS]       = { "Steps",        "Height change sounds", 0, 1, 1, NULL },
    [SET_STEP_LEVEL]  = { "StepLevel",    "Height change level", 0, 4, 2, LEVEL_WORDS },
    [SET_DAYS]        = { "DayTick",      "Day passing sound",   0, 1, 1, NULL },
    [SET_DAY_LEVEL]   = { "DayTickLevel", "Day passing level",   0, 4, 2, LEVEL_WORDS },
    [SET_GLIDE]      = { "GlideSpeed",   "Glide speed",         0, 2, GLIDE_NORMAL, GLIDE_WORDS },
    [SET_COMBAT]     = { "Combat",       "Combat narration",    0, 1, 1, NULL },
    [SET_SIGHT]      = { "Sightings",    "Enemy sightings",     0, 1, 1, NULL },
    [SET_TURN]       = { "TurnHandover", "Turn changes",        0, 1, 1, NULL },
    [SET_TICKER]     = { "Ticker",       "Message ticker",      0, 1, 1, NULL },
    [SET_OBJECTIVES] = { "Objectives",   "Objective changes",   0, 1, 1, NULL },
    [SET_NARRATIVE]  = { "Narrative",    "Comm-link messages",  0, 1, 1, NULL },
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
    return id >= 0 && id < SET_COUNT ? SPEC[id].name : "";
}

void settings_value_text(int id, char* out, size_t out_sz)
{
    if (!out || !out_sz) return;
    out[0] = 0;
    if (id < 0 || id >= SET_COUNT) return;
    int v = settings_get(id);
    const char* w = SPEC[id].words ? SPEC[id].words[v - SPEC[id].lo] : (v ? "On" : "Off");
    strncpy_s(out, out_sz, w, _TRUNCATE);
}
