// Ally heartbeats: where and how. See heart.h.

#include "heart.h"
#include "soldier.h"
#include <math.h>

// Where a heart is heard is a map: east-west is the stereo field and
// north-south is pitch, each by *tiles*, not by bearing. The first version
// used the bearing alone, so "5 west" and "10 west, 5 north" came out almost
// the same -- the pan 0.9 against 0.8, the pitch under two semitones apart --
// and only loudness told the distance. Now 10 west is twice as far left as 5
// west, and 5 north is 3 semitones up.
//
// The pan reaches its side at this many tiles, and stops short of the hard
// side there, as the wall field's does, so a far ally is still heard a little
// in the other ear.
#define HEART_PAN_TILES  12.0f
#define HEART_PAN        0.9f
// A twentieth of an octave a tile north (up) or south (down), to at most this
// many octaves at 12 tiles. With the beep at 1600 Hz (make_ekg.py) that keeps
// it within 1060-2420 Hz, clear of the wall field's bands at 210, 470 and
// 920 Hz.
#define HEART_OCT_TILE   0.05f
#define HEART_PITCH_MAX  0.6f
// Half as loud at this many tiles, a third at twice it. Raised from 8 after
// the first run: far hearts were lost.
#define HEART_HALF_TILES 12.0f
// The floor: an ally across the map is quiet, not gone -- about 9 dB under
// a near one, which still reads as further off. 0.12 was inaudible at Loud.
#define HEART_MIN_GAIN   0.35f

// The pace: nearly dead is about twice as fast as calm. Doubled for a while
// when a squad's beeps ran together; allies now take turns (audio.c,
// hearts_turns), so they cannot collide, and the pace is back where it began.
#define HEART_CALM_S     1.2f
#define HEART_WEAK_S     0.55f
#define HEART_BLEED_S    0.45f
#define HEART_STABLE_S   1.5f
#define HEART_FAINT      0.6f
// Panic: a quick triple, then a pause, once a period -- "ba-ba-ba, ba-ba-ba".
// The player's figure. Before it: gaps alternating 0.6 and 1.4 (a limp),
// then an uneven eight-step pattern (erratic, but no shape to recognise).
// The triple's notes are this far apart whatever the pace: as sixteenths of
// the period they were quick at 1.2 s, and at the slower paces they would
// have stopped being a triple at all. The rest of the period is the pause,
// so the figure's pace still says the health.
#define PANIC_NOTE_S     0.15f
#define PANIC_STEPS      3
#define HEART_IRREGULAR  1.0f

void heart_sound(int dx, int dy, int hp, int hp_max, int panicked, int wounded,
                 HeartSound* out)
{
    float dist = sqrtf((float)(dx * dx + dy * dy));
    out->kind = HEART_ALLY;
    float across = (float)dx / HEART_PAN_TILES;
    if (across > 1.0f) across = 1.0f;
    if (across < -1.0f) across = -1.0f;
    out->pan = HEART_PAN * across;
    float oct = HEART_OCT_TILE * (float)dy;
    if (oct > HEART_PITCH_MAX) oct = HEART_PITCH_MAX;
    if (oct < -HEART_PITCH_MAX) oct = -HEART_PITCH_MAX;
    out->pitch = powf(2.0f, oct);
    out->gain = 1.0f / (1.0f + dist / HEART_HALF_TILES);
    if (out->gain < HEART_MIN_GAIN) out->gain = HEART_MIN_GAIN;

    float health = 1.0f;
    if (hp >= 0 && hp_max > 0) {
        health = (float)hp / (float)hp_max;
        if (health > 1.0f) health = 1.0f;
        if (health < 0.0f) health = 0.0f;
    }
    out->period = HEART_WEAK_S + (HEART_CALM_S - HEART_WEAK_S) * health;
    out->irregular = panicked == 1 ? HEART_IRREGULAR : 0.0f;
    if (wounded == SOLDIER_BLEEDING) {
        out->period = HEART_BLEED_S;
        out->gain *= HEART_FAINT;
    } else if (wounded == SOLDIER_STABILISED) {
        out->period = HEART_STABLE_S;
        out->gain *= HEART_FAINT;
    }
}

float heart_gap(const HeartSound* s, int beat)
{
    if (!s) return 1.0f;
    if (s->irregular <= 0.0f) return s->period;
    if (beat < 0) beat = -beat;
    if (beat % PANIC_STEPS < PANIC_STEPS - 1) return PANIC_NOTE_S;
    float rest = s->period - (PANIC_STEPS - 1) * PANIC_NOTE_S;
    return rest > PANIC_NOTE_S ? rest : PANIC_NOTE_S;
}

// heart_turn_gap's share of the full gap, and its floor, so the quickest
// hearts do not run their two beats into one.
#define TURN_SCALE     0.35f
#define TURN_MIN_S     0.15f

float heart_turn_gap(const HeartSound* s, int beat)
{
    if (!s) return 1.0f;
    float g;
    if (s->irregular > 0.0f && (beat < 0 ? -beat : beat) % PANIC_STEPS < PANIC_STEPS - 1)
        g = PANIC_NOTE_S;
    else
        g = s->period * TURN_SCALE;
    return g > TURN_MIN_S ? g : TURN_MIN_S;
}

int heart_figure_len(const HeartSound* s)
{
    return s && s->irregular > 0.0f ? PANIC_STEPS : 1;
}

int heart_figure_start(const HeartSound* s, int beat)
{
    if (!s || s->irregular <= 0.0f) return 1;
    if (beat < 0) beat = -beat;
    return beat % PANIC_STEPS == 0;
}
