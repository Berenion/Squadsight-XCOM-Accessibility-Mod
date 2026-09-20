// The wall field, the part that does not touch the game.  See sonar.h.

#include "sonar.h"
#include <math.h>
#include <string.h>

// The steps, west, north, south, east.
static const int STEP_X[SONAR_DIRS] = { -1,  0,  0,  1 };
static const int STEP_Y[SONAR_DIRS] = {  0,  1, -1,  0 };

// The same as unit vectors, for a solid tile's bearing.
static const float DIR_X[SONAR_DIRS] = { -1.0f, 0.0f,  0.0f, 1.0f };
static const float DIR_Y[SONAR_DIRS] = {  0.0f, 1.0f, -1.0f, 0.0f };

// A face closer than this to the listening tile's own centre has no side to
// tell and no bearing to share it out by. Nothing the game reports can land
// there -- the nearest face is half a tile away -- so this only stops a divide
// by zero.
#define TOO_CLOSE 0.001f

// Where the level bends over. Below it a level is exactly the proportion of the
// reference; above it a corner, or a wall with a crate against it, folds into
// what is left instead of clipping flat.
#define KNEE 0.85f

void sonar_step(int dir, int* dx, int* dy)
{
    if (dir < 0 || dir >= SONAR_DIRS) {
        if (dx) *dx = 0;
        if (dy) *dy = 0;
        return;
    }
    if (dx) *dx = STEP_X[dir];
    if (dy) *dy = STEP_Y[dir];
}

float sonar_intensity(float distance, float range)
{
    if (range <= 0.0f || distance <= 0.0f || distance >= range) return 0.0f;
    // Inverse square, tapered to nothing at the range so that a face crossing
    // in does not pop. The taper is the quadratic curve this file used to use
    // on its own, which is now only the window on the law rather than the law.
    float t = 1.0f - distance / range;
    return t * t / (distance * distance);
}

float sonar_reference(void)
{
    // A straight wall whose face plane is half a tile away, running off in
    // both directions until the curve has nothing left to give: one face at
    // (0, 0.5) and a pair at (+/-a, 0.5) for every whole a. Summed once, on
    // the first call.
    static float ref;
    if (ref > 0.0f) return ref;
    float s = sonar_intensity(0.5f, SONAR_RANGE);
    for (int a = 1;; a++) {
        float d = sqrtf((float)a * a + 0.25f);
        float p = sonar_intensity(d, SONAR_RANGE);
        if (p <= 0.0f) break;
        s += 2.0f * p;
    }
    ref = s;
    return ref;
}

void sonar_field_clear(SonarField* f)
{
    if (f) memset(f, 0, sizeof *f);
}

void sonar_face(SonarField* f, int axis, float fx, float fy)
{
    if (!f) return;
    float d = sqrtf(fx * fx + fy * fy);
    if (d < TOO_CLOSE) return;
    float w = sonar_intensity(d, SONAR_RANGE);
    if (w <= 0.0f) return;

    // The whole of it goes one way: the way the face faces. Which of the pair
    // that is, is which side of the cursor it is on.
    int dir;
    if (axis == SONAR_AXIS_EW) dir = fx >= 0.0f ? SONAR_E : SONAR_W;
    else                       dir = fy >= 0.0f ? SONAR_N : SONAR_S;
    f->sum[dir] += w;
}

void sonar_block(SonarField* f, float fx, float fy)
{
    if (!f) return;
    float d = sqrtf(fx * fx + fy * fy);
    if (d < TOO_CLOSE) return;
    float w = sonar_intensity(d, SONAR_RANGE);
    if (w <= 0.0f) return;

    for (int i = 0; i < SONAR_DIRS; i++) {
        // Cos-squared of the angle between the blocker and the direction, and
        // nothing at all behind it. Over four directions at right angles these
        // add to exactly one, so a blocker is shared out and never multiplied:
        // cos^2 + sin^2, with the two negative lobes clamped off.
        float c = (fx * DIR_X[i] + fy * DIR_Y[i]) / d;
        if (c <= 0.0f) continue;
        f->sum[i] += w * c * c;
    }
}

void sonar_demo_wall(SonarField* f, int dir, int tiles)
{
    if (!f || dir < 0 || dir >= SONAR_DIRS || tiles < 0) return;
    int sx, sy;
    sonar_step(dir, &sx, &sy);
    int axis = sx != 0 ? SONAR_AXIS_EW : SONAR_AXIS_NS;
    // Out to the wall, then along it, far enough either way that the range
    // runs out before the wall does -- so it is a wall and not a panel.
    int half = (int)SONAR_RANGE + 2;
    for (int a = -half; a <= half; a++) {
        float fx = (float)sx * ((float)tiles + 0.5f) - (float)sy * (float)a;
        float fy = (float)sy * ((float)tiles + 0.5f) + (float)sx * (float)a;
        sonar_face(f, axis, fx, fy);
    }
}

void sonar_field_finish(SonarField* f)
{
    if (!f) return;
    float ref = sonar_reference();
    for (int i = 0; i < SONAR_DIRS; i++) {
        // Intensities add; what comes out is an amplitude, so it is the root
        // of the total. This is not a nicety -- it is what spaces the tiles
        // evenly for the ear. The intensities themselves span nearly fifty
        // decibels between a wall against the tile and one six tiles off, all
        // of it crowded into the first tile or two; rooted, the same span
        // comes out as a few decibels per tile the whole way, which is a
        // gradient a listener can count along.
        float x = ref > 0.0f ? sqrtf(f->sum[i] / ref) : 0.0f;
        // Proportional up to the knee, then bent -- the same soft fold the
        // mixer puts across its output, and for the same reason: rounding the
        // top off once is worth more than a hard ceiling that every near wall
        // would flatten against.
        f->level[i] = x <= KNEE
                      ? x
                      : KNEE + (1.0f - KNEE) * tanhf((x - KNEE) / (1.0f - KNEE));
    }
}
