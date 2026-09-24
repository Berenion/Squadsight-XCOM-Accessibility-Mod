#pragma once

// Heartbeats: each living squad member beats from where they stand, and so
// does each enemy the squad can see, measured from the tile the wall field
// listens from (the cursor, or the tile being navigated to). The two are
// different recordings, switched and levelled apart in the options menu
// (settings.h, SET_HEARTS and SET_ALIENS). An enemy is heard only while a
// squad member sees it -- the radar's rule, so a heart never gives a hidden
// alien away.
//
// Where an ally is, as a map:
//   - east and west by stereo pan, by tiles: further east is further right,
//     all the way at 12 tiles;
//   - north and south by pitch, by tiles: higher to the north and lower to
//     the south, as the field's north band is its high one and its south
//     band its low one (audio.c, BAND);
//   - distance by loudness, never down to silence, so a squad member across
//     the map is still there.
// How they are: the pace is health -- steady at full, quickening as hit
// points fall; bleeding out fast and faint, stabilised slow and faint.
// Panic is the rhythm: a quick triple and a short pause, over and over. It
// was a pace at first, and panicked (0.6 s) could not be told from badly hurt
// (0.66 s); a rhythm is heard apart from any pace, and a wounded, panicked
// soldier's triples then come faster.
//
// Only the mapping lives here, so the offline checks can reach it. Which
// allies, and the mixing, are main.c's and audio.c's.

// Which recording a heart plays. Doors and windows are not hearts, but they
// are placed, levelled and taken in turns the same way, so they are kinds here
// too.
// The two height cues and the day's tick are generated, not recorded, and
// played as a cue (audio_cue) rather than placed.
enum { HEART_ALLY, HEART_ALIEN, HEART_DOOR, HEART_WINDOW,
       HEART_STEP_UP, HEART_STEP_DOWN, HEART_TICK, HEART_KINDS };

typedef struct {
    int   kind;     // HEART_*
    float pan;      // -1 west .. 1 east
    float pitch;    // playback rate: 1 is the recording
    float gain;     // 0..1, before the field's level
    float period;   // seconds from one beat to the next, on average
    float irregular; // 0 steady; 1 the whole of the panic pattern (heart.c)
} HeartSound;

// `dx`, `dy`: tiles from the listening point, east and north positive.
// `hp`, `hp_max`: as the flag shows them, -1 unknown. `wounded`:
// SOLDIER_WOUND_* (soldier.h). An enemy's pace follows its hit points the
// same way; it has no panic or wound state here. `kind` is left HEART_ALLY
// for the caller to change.
void heart_sound(int dx, int dy, int hp, int hp_max, int panicked, int wounded,
                 HeartSound* out);

// Seconds from beat `beat` (0, 1, 2 ...) to the next: the period, or for an
// irregular heart that beat's step of the panic figure -- two short gaps
// between the triple's notes, then the rest of the period. The mixer and the
// menu's demonstration both time beats by it.
float heart_gap(const HeartSound* s, int beat);

// Whether beat `beat` begins a figure: every beat of a steady heart, the
// first of each triple of a panicked one. The mixer holds back only these
// to keep hearts apart, so a triple is never broken up.
int heart_figure_start(const HeartSound* s, int beat);

// How many beats one figure is: 1 for a steady heart, 3 for a panic triple.
int heart_figure_len(const HeartSound* s);

// The gap after beat `beat` when allies take turns (audio.c, hearts_turns):
// heart_gap drawn in to a third, so a turn's beats sit together and the pause
// between two soldiers is the longest thing heard. At full pace a soldier's
// second beat came 1.2 s after the first and the next soldier 0.6 s after
// that, and the ear paired each soldier's second beat with the next one's
// first. The proportions still say the health: 0.42 s calm, 0.19 s nearly
// dead. A panic triple's notes keep their 0.15 s.
float heart_turn_gap(const HeartSound* s, int beat);
