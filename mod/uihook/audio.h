#pragma once
#include <stddef.h>
#include "sonar.h"
#include "heart.h"

// A small stereo mixer of generated sound, played through waveOut.
//
// Nothing here goes near the game's own audio. XCOM mixes through Unreal, and
// reaching into that from outside would mean matching its sound-cue model,
// its volume settings and its occlusion, all so the mod's cues could be
// coloured by a mixer meant for gunfire. It is far less work, and far more
// predictable, to open a second device and put noise through it: the field
// then sounds the same on every map, and the player's own game-volume setting
// leaves it alone.
//
// The shape is the one the NonVisualCalculus mod uses (MIT, (c) 2026 Rashad
// Naqeeb), with its NAudio parts written out by hand here:
//
//   - one mixer feeding one device, every voice an input on it;
//   - a soft limiter across the output, and no clamp on the voices: four
//     bands at once may sum past full scale, and rounding that off once at
//     the end distorts far less than clipping each voice on the way in;
//   - the device opened on first use and shut off for good if it fails, so a
//     machine with no sound card loses the field and nothing else.
//
// The field's voices are not ported: those mods play recorded assets, and
// these are resonant noise bands generated a buffer at a time, which is why
// this file carries a filter and a delay line. The one recording is the ally
// heartbeat (below), a single short sample.
//
// The bands are permanent. There is no pool and nothing is triggered: four
// bands are open from the moment the device is, and all the mod ever does is
// tell them how loud to be. Each glides to the level it is given over about a
// tenth of a second, which is what turns a cursor stepping between tiles into
// a wall that approaches. A level is the only thing about a band that ever
// changes: pitch, filter, loudness correction and stereo place are settled once
// at startup and then left alone.

// Opens the device. Returns 1 on success, 0 if there is no usable output --
// which is not a failure of the mod, only of its field. `why` receives a line
// for the log either way.
int audio_start(char* why, size_t why_sz);

// Whether the field will be heard.
int audio_available(void);

// Where the walls are now: every direction's level, as sonar_field_finish left
// it. Safe to call when the device never opened -- it then does nothing.
//
// It must be called every frame, not only when the field changes. A field
// left unrenewed lapses to silence in a quarter of a second, which is what
// stops the walls sounding on after the mission they described has ended: the
// cursor's own native is what drives all of this, and when the cursor is
// destroyed nothing runs to switch anything off.
void audio_field(const SonarField* f);

// The mod's sounds, each with a level of its own. A new sound adds an entry
// here, a level in settings.h, and a line in learn.c's LEVELS table.
enum {
    AUDIO_WALLS,        // the wall field
    AUDIO_HEARTS,       // ally heartbeats
    AUDIO_ALIENS,       // enemy heartbeats
    AUDIO_DOORS,        // doors nearby
    AUDIO_WINDOWS,      // windows nearby
    AUDIO_SOURCES
};

// How loud a source is, as a notch from 0 to audio_volume_notches() - 1, three
// decibels apart, starting in the middle. There is no one right loudness: the
// field has to be heard over whatever the game is playing, and a rainstorm and a
// quiet interior are a long way apart. The pulse on each band (audio.c) is what
// makes it audible through weather at all; this is what makes it comfortable.
// And no one balance between the sources: which matters more is the player's
// to say. The options menu saves each (settings.h) and learn_apply_levels
// hands them over at startup.
int audio_volume(int source);
int audio_volume_notches(void);
int audio_volume_set(int source, int notch);    // clamped; returns what it set

// Fades the whole field out and leaves it there, until the next audio_field.
// Used when there is nothing to describe: no cursor, the game in the
// background, the field switched off.
void audio_field_off(void);

// ---- ally heartbeats (heart.h) ----------------------------------------------
//
// Loads a kind's beat (HEART_*), 16-bit mono PCM at 44.1 kHz. Without it
// that kind is silent and nothing else changes. `why` receives a line for
// the log.
int audio_heart_load(int kind, const char* path, char* why, size_t why_sz);

// Whether a kind will be heard: the device is open and its beat loaded.
int audio_hearts_available(int kind);

// Who is beating now, and how: `ids` names each ally (any pointer that stays
// the same for them), so a heart keeps its own time from one call to the
// next. Like audio_field it must be renewed every frame, and lapses when it
// is not; one left out stops after the beat already sounding. Allies and
// enemies go in the one set, each HeartSound naming its kind. At most 32.
void audio_hearts(const void* const* ids, const HeartSound* s, int n);

// No new beats until the next audio_hearts.
void audio_hearts_off(void);

// One beat on a voice of its own, outside the set: the options menu's
// demonstration.
void audio_heart_once(const HeartSound* s);

// Closes the device. The mod does not call this: the DLL lives as long as the
// game does, and tearing down an audio thread on process exit is a good way
// to hang one. It exists for the offline test.
void audio_stop(void);
