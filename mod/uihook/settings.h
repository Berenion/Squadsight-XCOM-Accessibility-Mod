#pragma once

// The mod's own options, chosen from the options menu (learn.h, numpad /) and
// kept in xcom_uihook_settings.ini beside the log, so a choice outlives the
// run. launcher.ini is the launcher's and is not touched.
//
// Every value is a small integer: a switch is 0 or 1, a scale a notch. They
// are written by the menu's thread and read by the game's, one int at a time,
// so no lock is needed -- a reader sees the old value or the new one.
//
// This file knows nothing of the game, the mixer or speech, so the offline
// checks can link it on its own. What a value does is decided where it is
// read: main.c for the announcements, the field, the hearts and the glide,
// learn.c for the levels it hands to the mixer.

#include <stddef.h>

enum {
    SET_FIELD,          // the wall field, on or off (numpad * in a mission)
    SET_WALL_LEVEL,     // its loudness, notch 0..4 (audio.h, AUDIO_WALLS)
    SET_HEARTS,         // ally heartbeats, on or off (heart.h)
    SET_HEART_SOLO,     // only the soldier last picked in the scanner beats
    SET_HEART_LEVEL,    // their loudness, notch 0..4 (AUDIO_HEARTS)
    SET_ALIENS,         // enemy heartbeats, on or off (heart.h)
    SET_ALIEN_LEVEL,    // their loudness, notch 0..4 (AUDIO_ALIENS)
    SET_DOORS,          // doors within 10 tiles, on or off
    SET_DOOR_LEVEL,     // their loudness, notch 0..4 (AUDIO_DOORS)
    SET_WINDOWS,        // windows within 10 tiles, on or off
    SET_WINDOW_LEVEL,   // their loudness, notch 0..4 (AUDIO_WINDOWS)
    SET_STEPS,          // a cue on a step up or down a floor, on or off
    SET_STEP_LEVEL,     // its loudness, notch 0..4 (AUDIO_STEPS)
    SET_DAYS,           // a tick as each day passes on the geoscape, on or off
    SET_DAY_LEVEL,      // its loudness, notch 0..4 (AUDIO_DAYS)
    SET_GLIDE,          // how fast a held numpad key moves: GLIDE_*
    SET_COMBAT,         // damage, misses and statuses
    SET_SIGHT,          // enemies coming into and going out of sight
    SET_TURN,           // "Alien activity." / "Your turn."
    SET_TICKER,         // the message ticker ("... takes a reaction shot!")
    SET_OBJECTIVES,     // objective changes, and the list shown by a script
    SET_NARRATIVE,      // comm-link lines (Central, Shen, Vahlen)
    SET_MOUSE,          // the physical mouse ignored while the game is in front (mouse.h)
    SET_DEBUG,          // the log's per-step lines (log.c, log_debug_only)
    SET_COUNT
};

enum { GLIDE_SLOW, GLIDE_NORMAL, GLIDE_FAST };

// Reads the file in `dir` (ending in a separator) and remembers it for saving.
// A missing file or key leaves the default. Without a call nothing is saved,
// which is what the offline practice run wants.
void settings_load(const char* dir, char* why, size_t why_sz);

int  settings_get(int id);

// Clamped to the setting's range and saved at once. Returns what was set.
int  settings_set(int id, int value);

// One step: a switch flips whichever way `delta` points; a scale moves by
// `delta` and stops at its ends. Returns the new value.
int  settings_step(int id, int delta);

// Whether the setting is a switch (on/off) rather than a scale.
int  settings_is_switch(int id);

// "Combat narration", as the menu says it.
const char* settings_name(int id);

// The value in words: "On", "Off", "Quiet", "Fast".
void settings_value_text(int id, char* out, size_t out_sz);

// Every setting back to its default, without saving. For the offline checks.
void settings_reset(void);
