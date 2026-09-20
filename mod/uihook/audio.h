#pragma once
#include <stddef.h>
#include "sonar.h"

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
// The voices themselves are not ported: those mods play recorded assets, and
// these are resonant noise bands generated a buffer at a time, which is why
// this file carries a filter and a delay line rather than a WAV decoder.
//
// The voices are permanent. There is no pool and nothing is triggered: four
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

// How loud the field is, as a notch from 0 to audio_volume_notches() - 1, three
// decibels apart, starting in the middle. There is no one right loudness: the
// field has to be heard over whatever the game is playing, and a rainstorm and a
// quiet interior are a long way apart. The pulse on each band (audio.c) is what
// makes it audible through weather at all; this is what makes it comfortable.
// Nothing saves the setting -- the mod has no settings file -- so every run
// starts in the middle.
int audio_volume(void);
int audio_volume_notches(void);
int audio_volume_set(int notch);        // clamped; returns what it set

// Fades the whole field out and leaves it there, until the next audio_field.
// Used when there is nothing to describe: no cursor, the game in the
// background, the field switched off.
void audio_field_off(void);

// Closes the device. The mod does not call this: the DLL lives as long as the
// game does, and tearing down an audio thread on process exit is a good way
// to hang one. It exists for the offline test.
void audio_stop(void);
