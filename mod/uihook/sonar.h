#pragma once
#include <stddef.h>

// The wall field: what the geometry around a tile sounds like, continuously.
//
// A sighted player takes in a room at a glance. Reading it tile by tile is
// the only way words give, and it is far too slow to convey shape. So the
// walls around the cursor are always sounding: four sustained voices, one per
// compass direction, each as loud as the wall that way is close. Move towards
// a wall and its voice swells; move away and it fades. Nothing is fired, and
// nothing has to be waited for -- the room is simply audible, the way it is
// simply visible.
//
// The model is the wall tones of the NonVisualCalculus accessibility mod for
// Disco Elysium (MIT, (c) 2026 Rashad Naqeeb), which took them in turn from
// the Pathfinder: Wrath of the Righteous accessibility mod. Two things are
// kept: sustained voices held open and glided between levels, and fixed
// compass panning, so a direction always sounds from the same place.
//
// An earlier version of this file fired a chord of struck sounds once per
// step instead -- the cursor steps rather than glides, and a drone on a cursor
// standing still says the same thing forever. Two things killed it. Holding a
// key to cross the map turned it into a machine gun, and a strike per step is
// a strike per *ray*: it answered "what does the nearest wall in each of four
// directions sound like", which is a radar ping, not a room. A long wall and a
// single pillar at the same distance were the same sound.
//
// ---- every face of every wall ---------------------------------------------
//
// So the field is not four rays. Every wall face within range is an emitter in
// its own right, and a direction's voice is what all the faces blocking that
// way add up to.
//
// Which way a face blocks is the face's own facing, not where it happens to
// lie. This matters more than it looks. A wall running north-south past the
// cursor's western side has faces stretching away both ahead and behind, and
// the far ones lie almost due north and south of the cursor -- but every one of
// them is a wall to the west, and nothing whatever to the north. Sharing them
// out by bearing instead, which is the obvious thing to do and was tried,
// makes standing next to any wall at all sound like standing in a box: a
// corridor came out as loud ahead as it was to the sides, which is the one
// thing a corridor is not. The game's cover bits already carry the facing --
// that is what a bit *is*, a side of a tile -- so nothing has to be inferred.
//
// A solid tile is the exception: a pillar, a truck, a tile simply filled in
// has no side, so it is shared out by where it lies, cos-squared, and a
// blocker to the northeast feeds north and east equally.
//
// What a side adds up to is worked out the way sound actually adds up. Each
// face is a source, its intensity falls off as the inverse square of its
// distance, the intensities of all the faces on that side are summed, and the
// level is the square root of the total -- because intensity is power and what
// a voice is set to is an amplitude. The sum is measured against what a solid
// wall half a tile away gives, so that wall sits at the top of the scale and
// everything else is honestly less than it, and only the very top is bent, so
// that a corner -- two walls and a share of each other's run -- cannot go over.
//
// Two things fall out of doing it properly, and both were got wrong first.
//
// Summing the whole surface, rather than taking the nearest face and calling it
// the distance, is what lets a level move smoothly: a wall slides past the
// cursor face by face instead of jumping from one ray hit to the next. But
// inverse square also means the nearest part of that surface dominates the
// total, which is right -- the wall a tile away is the news, not the twenty
// tiles of it stretching off into the distance.
//
// And the square root is not a nicety. As raw intensities the span from a wall
// against the tile to one six tiles off is nearly fifty decibels, almost all of
// it spent in the first tile or two; the first version used a gentler curve
// summed without the root and had the opposite fault, one and a half decibels
// between "touching a wall" and "a tile from it", which is nothing at all to
// hear. Rooted, the same span comes out at about five decibels a tile the whole
// way -- a gradient a listener can count along.
//
// ---- the sound language ---------------------------------------------------
//
// These are not tones. Sine drones were tried and they are unbearable inside a
// minute: a steady sine is a test signal, it beats against its neighbours, and
// the ear cannot stop hearing it. What a wall should sound like is a surface --
// air moving against solid matter -- and that is noise, not pitch.
//
// So each direction is a narrow band of noise: white noise through a resonant
// filter, which has a pitch to identify it by and no periodicity to grate.
// Four bands sit in the mix like four different materials breathing, they blend
// instead of beating, and at the level these run at they read as room rather
// than as signal.
//
//   north   a bright hiss   920 Hz   wide: unrelated noise in each ear
//   south   a low hum       210 Hz   a point, dead centre
//   east    mid             470 Hz   a point, hard right
//   west    mid             470 Hz   a point, hard left
//
// Four sounds at once need four *places*, and stereo has one axis to spend, so
// the two that cannot be moved along it are given different kinds of image
// instead. North is decorrelated -- the ears are handed unrelated noise of the
// same colour -- and has no direction in it at all: a wash rather than a
// source. South is a single point in the middle, and the only thing there. They
// are not two positions on a line, they are two different things, and the ear
// separates them at once.
//
// East and west share a pitch and differ only in side, because they are one
// thing mirrored and a listener hearing it on the left should not have to work
// out that it is what they heard on the right. Their noise is independent,
// though -- two walls are two walls -- and they are panned nearly the whole way
// over, which is what the first version, at 0.92 with both of the others
// centred, did not do far enough: it blended. audio.c holds the numbers and
// the rest of the reasoning.
//
// Low cover -- a crate, a railing, anything shootable over -- sounds exactly
// like a wall. It blocks the way just as surely, which is what the field is for,
// and an earlier version lifted its band a fifth to mark it out. That is gone:
// two facts on one voice means neither can be heard cleanly, and the level has
// to carry distance, which is the only thing it can carry without ambiguity.
// Whether cover is low is said in words instead (tile.c already does, on numpad
// 5), and will get a cue of its own that is not the level of a drone.
//
// None of this can be learned from a mission, where all four sound at once and
// quietly, so there is a practice mode that plays them one at a time and names
// them: see learn.h.

#define SONAR_DIRS 4

// The directions. These are the mod's own, the ones the numpad moves in:
// north is +Y, east is +X.
enum { SONAR_W, SONAR_N, SONAR_S, SONAR_E };

// The step in tiles for a direction: dx east, dy north.
void sonar_step(int dir, int* dx, int* dy);

// How far the field hears, in tiles. Beyond it a wall is not worth hearing: the
// taper below has brought it to nothing by then anyway. Seven rather than the
// five the struck chord used, because a level is read against the reference
// below rather than off one face, and five left a wall four tiles out too quiet
// to hear coming.
#define SONAR_RANGE 7.0f

// Which pair of directions a wall face can block. A face's own position then
// says which of the pair it is: a north-south face north of the cursor blocks
// north, one south of it blocks south.
enum { SONAR_AXIS_NS, SONAR_AXIS_EW };

// How loud each direction's voice is.
typedef struct {
    float level[SONAR_DIRS];    // 0..1, what the mixer holds that voice at
    // The running total of intensity, only meaningful until
    // sonar_field_finish turns it into the level above.
    float sum[SONAR_DIRS];
} SonarField;

// The intensity one face contributes from `distance` tiles off: inverse square,
// windowed by (1 - d/range)^2 so that it is exactly nothing at the range and
// does not pop as a face crosses in. An intensity, not a level -- see
// sonar_field_finish, which sums these and takes the root.
float sonar_intensity(float distance, float range);

// The intensity a wall standing right against the tile adds up to, and so what
// a level of 1 means. Worked out from the law rather than written down, so
// changing SONAR_RANGE needs no second edit.
float sonar_reference(void);

// Starts a fresh field. Every direction silent, nothing accumulated.
void sonar_field_clear(SonarField* f);

// Adds one wall face. `fx`, `fy` are where it is relative to the tile being
// listened from, in tiles, east and north -- so a wall on that tile's own
// northern edge is at (0, 0.5). `axis` is the pair of directions it can block;
// which one it does block follows from its position. Low cover is a face like
// any other: see the note on it above.
void sonar_face(SonarField* f, int axis, float fx, float fy);

// Adds one solid tile, at (fx, fy) in the same terms: something with no side
// to it, shared out by where it lies.
void sonar_block(SonarField* f, float fx, float fy);

// Lays down a straight wall `tiles` away on one side, as the game's cover bits
// would give it: a run of faces reaching out until the range ends. Not used by
// the mod against a real map, where the faces come from the game -- this is for
// the practice mode and the offline test, which both need a wall that is
// exactly and only what it says it is.
void sonar_demo_wall(SonarField* f, int dir, int tiles);

// Settles `level` from what was added. Must be called before the field is
// handed to the mixer.
void sonar_field_finish(SonarField* f);
