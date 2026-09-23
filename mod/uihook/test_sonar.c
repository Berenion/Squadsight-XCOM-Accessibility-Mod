// Offline checks for the wall field, and a listen to it.
//
// The rules the live behaviour depends on:
//   - a face's intensity falls as the inverse square, and is exactly nothing
//     at the range
//   - open ground makes no sound at all
//   - a wall face is heard in the direction it faces and nowhere else, so a
//     wall beside the cursor does not bleed into the way ahead
//   - a solid tile, which has no facing, is shared out by bearing, and its
//     four shares add up to exactly it
//   - a wall against the tile is full scale, and nothing else reaches it
//   - nearer is louder, always, and the whole of a surface contributes, so a
//     level moves smoothly rather than jumping between ray hits
//   - the steps between one tile of distance and the next are even enough to
//     count along -- roughly equal in decibels, which is the whole reason the
//     level is the root of a sum of intensities
//   - no level ever runs past full scale
//
// Run with an argument to hear it instead of checking it:
//
//     test_sonar.exe play        a fixed tour: the four sides, a walk, rooms
//     test_sonar.exe practice    the in-game practice menu, at the desktop
//
// `play` holds the four directions one at a time, then walks towards a wall,
// down a corridor and out into the open -- the quickest way to tell whether a
// tuning change made the map easier or harder to picture. Judge distinctness on
// the first four: a field can sound busy and still be four bands nobody can
// name apart. Judge the walk on whether the wall arriving is heard as arriving,
// rather than as a level being changed.
//
// `practice` is the same code the mod runs on numpad / during a game, so the
// sounds can be learned without launching XCOM at all -- which is why it lives
// here as well as in the DLL.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "sonar.h"
#include "audio.h"
#include "learn.h"
#include "speech.h"

static int failures;

static void check(int ok, const char* what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static int close_to(float a, float b) { return fabsf(a - b) < 0.0005f; }

static void room(SonarField* f, int west, int north, int south, int east)
{
    const int d[SONAR_DIRS] = { west, north, south, east };
    sonar_field_clear(f);
    for (int i = 0; i < SONAR_DIRS; i++)
        if (d[i] >= 0) sonar_demo_wall(f, i, d[i]);
    sonar_field_finish(f);
}

static void hold(const char* what, const SonarField* f, int ms)
{
    printf("  %-26s W %.2f N %.2f S %.2f E %.2f\n", what,
           f->level[SONAR_W], f->level[SONAR_N],
           f->level[SONAR_S], f->level[SONAR_E]);
    // Renewed as the mod renews it, because an unrenewed field lapses.
    for (int t = 0; t < ms; t += 20) {
        audio_field(f);
        Sleep(20);
    }
}

// The options menu and practice, driven by their own thread exactly as in the
// game. settings_load is never called, so nothing chosen here is saved.
// Nothing here talks to it but the wait: the keys are its own.
static int practice(void)
{
    char why[256];
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    speech_init(dir, why, sizeof why);
    printf("speech: %s\n", why);
    if (!audio_start(why, sizeof why)) {
        printf("audio: %s\n", why);
        return 1;
    }
    printf("audio: %s\n", why);
    static const char* const BEAT_FILE[HEART_KINDS] = { "ekgbeep.wav", "alienbeat.wav",
                                                         "doorsound.wav", "windowsound.wav" };
    for (int kind = 0; kind < HEART_KINDS; kind++) {
        char beat[MAX_PATH];
        _snprintf_s(beat, sizeof beat, _TRUNCATE, "%s\\%s", dir, BEAT_FILE[kind]);
        audio_heart_load(kind, beat, why, sizeof why);
        printf("audio: %s\n", why);
    }
    if (!learn_start(why, sizeof why)) {
        printf("practice: %s\n", why);
        return 1;
    }
    printf("practice: %s\n\n", why);

    printf("  Numpad /      the options menu open and closed\n"
           "  In the menu: 8 2 move, 4 6 change, 5 opens Sound practice or Hear the heartbeats\n\n"
           "  In practice, / goes back to the menu:\n"
           "  Numpad 8 2 4 6   north, south, west, east, one at a time\n"
           "  Numpad 7 9 1 3   the corners, two at a time\n"
           "  Numpad 5      all four\n"
           "  Numpad 0      silence\n"
           "  Numpad + -    the wall further off and nearer\n"
           "  Numpad * .    the whole field louder and quieter\n\n"
           "  Escape        quit\n\n"
           "Num Lock must be on, and this window must have the focus.\n");

    while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) Sleep(30);
    learn_stop();
    audio_field_off();
    Sleep(300);
    audio_stop();
    speech_shutdown();
    return 0;
}

int main(int argc, char** argv)
{
    SonarField f;

    if (argc > 1 && strcmp(argv[1], "practice") == 0) return practice();

    if (argc > 1) {
        char why[256];
        if (!audio_start(why, sizeof why)) {
            printf("audio: %s\n", why);
            return 1;
        }
        printf("audio: %s\n\n", why);

        // Each on its own first: the only way to judge whether four directions
        // are really four sounds is to hear them one at a time before hearing
        // them together.
        room(&f, 0, -1, -1, -1); hold("west alone", &f, 1600);
        room(&f, -1, 0, -1, -1); hold("north alone", &f, 1600);
        room(&f, -1, -1, 0, -1); hold("south alone", &f, 1600);
        room(&f, -1, -1, -1, 0); hold("east alone", &f, 1600);

        // The whole point of the thing: a wall that arrives. Six tiles out to
        // nothing between, at about the rate a held key glides.
        printf("  %-26s\n", "walking north into a wall");
        for (int d = 6; d >= 0; d--) {
            room(&f, -1, d, -1, -1);
            hold("", &f, 280);
        }
        hold("  standing at it", &f, 1200);
        room(&f, -1, -1, -1, -1);
        hold("  and backing off", &f, 1400);

        room(&f, 0, -1, -1, 0);  hold("corridor north-south", &f, 2200);
        room(&f, -1, 0, 0, -1);  hold("corridor east-west", &f, 2200);
        room(&f, 1, 0, -1, -1);  hold("northwest corner", &f, 2200);
        room(&f, 3, 3, 3, 3);    hold("a room, in the middle", &f, 2200);
        sonar_field_clear(&f);
        sonar_block(&f, 0.0f, 1.0f);
        sonar_field_finish(&f);
        hold("one pillar north", &f, 2200);
        room(&f, -1, -1, -1, -1); hold("open ground", &f, 1500);

        // Lapsing. Nothing renews the field from here, and it should fade out
        // on its own rather than play until the device closes.
        printf("  %-26s\n", "left to lapse");
        room(&f, 0, 0, 0, 0);
        audio_field(&f);
        Sleep(1500);

        audio_stop();
        return 0;
    }

    printf("one face's intensity\n");
    check(sonar_intensity(5.0f, 5.0f) == 0.0f, "a face at the range is nothing");
    check(sonar_intensity(6.0f, 5.0f) == 0.0f, "a face past the range is nothing");
    check(sonar_intensity(1.0f, 5.0f) > sonar_intensity(2.0f, 5.0f),
          "nearer is louder");
    // Inverse square, measured deep inside the window where the taper is
    // almost 1 and so barely tilts the ratio.
    check(fabsf(sonar_intensity(1.0f, 1000.0f) /
                sonar_intensity(2.0f, 1000.0f) - 4.0f) < 0.02f,
          "twice the distance is a quarter the intensity");
    check(sonar_intensity(0.0f, 5.0f) == 0.0f, "a distance of nothing is nothing");
    check(sonar_intensity(1.0f, 0.0f) == 0.0f, "a range of nothing is nothing");
    check(sonar_intensity(-1.0f, 5.0f) == 0.0f, "a negative distance is nothing");

    printf("one face\n");
    sonar_field_clear(&f);
    check(f.level[SONAR_N] == 0.0f && f.level[SONAR_S] == 0.0f &&
          f.level[SONAR_E] == 0.0f && f.level[SONAR_W] == 0.0f,
          "a cleared field is silent in every direction");
    sonar_field_finish(&f);
    check(f.level[SONAR_N] == 0.0f, "open ground makes no sound");

    // A face is heard by its facing. This one is on the cursor's own northern
    // edge, and the whole of it is north.
    sonar_field_clear(&f);
    sonar_face(&f, SONAR_AXIS_NS, 0.0f, 0.5f);
    sonar_field_finish(&f);
    check(close_to(f.level[SONAR_N],
                   sqrtf(sonar_intensity(0.5f, SONAR_RANGE) / sonar_reference())),
          "a face due north is the root of its share of the reference");
    check(f.level[SONAR_S] == 0.0f && f.level[SONAR_E] == 0.0f &&
          f.level[SONAR_W] == 0.0f, "and is heard nowhere else");

    // The rule the design turns on, and the one bearing got wrong: a
    // north-facing face far off to the side is still a wall to the north, and
    // is nothing at all to the east.
    sonar_field_clear(&f);
    sonar_face(&f, SONAR_AXIS_NS, 5.0f, 0.5f);
    sonar_field_finish(&f);
    check(f.level[SONAR_N] > 0.0f, "a face away along a northern wall is north");
    check(f.level[SONAR_E] == 0.0f, "and not partly east, wherever it lies");

    // Which side of the pair it is, is which side of the cursor it is on.
    sonar_field_clear(&f);
    sonar_face(&f, SONAR_AXIS_NS, 0.0f, -0.5f);
    sonar_face(&f, SONAR_AXIS_EW, -0.5f, 0.0f);
    sonar_field_finish(&f);
    check(f.level[SONAR_S] > 0.0f && f.level[SONAR_N] == 0.0f,
          "a north-south face behind the cursor blocks south");
    check(f.level[SONAR_W] > 0.0f && f.level[SONAR_E] == 0.0f,
          "an east-west face to its left blocks west");

    printf("a solid tile\n");
    // No facing, so it is shared by bearing -- evenly, northeast, and the two
    // halves add up to the whole.
    sonar_field_clear(&f);
    sonar_block(&f, 2.0f, 2.0f);
    sonar_field_finish(&f);
    check(close_to(f.level[SONAR_N], f.level[SONAR_E]),
          "a blocker northeast is shared evenly between north and east");
    check(close_to((f.sum[SONAR_N] + f.sum[SONAR_E]),
                   sonar_intensity(sqrtf(8.0f), SONAR_RANGE)),
          "its shares add up to exactly the blocker");
    check(f.level[SONAR_S] == 0.0f && f.level[SONAR_W] == 0.0f,
          "and nothing leaks behind it");

    printf("what a room adds up to\n");
    room(&f, -1, 0, -1, -1);
    // The reference itself, so it lands at the top -- not exactly at 1, because
    // the knee is already bending by then and has to leave room above for a
    // corner. Within a few per cent is what "full scale" means here.
    check(f.level[SONAR_N] > 0.95f, "a wall against the tile is full scale");
    check(f.level[SONAR_S] == 0.0f && f.level[SONAR_E] == 0.0f &&
          f.level[SONAR_W] == 0.0f, "and silent in the other three");

    // No amount of wall further off may reach what is against the tile, or
    // loudness stops meaning distance.
    float against = f.level[SONAR_N];
    room(&f, -1, 2, -1, -1);
    check(f.level[SONAR_N] < against, "the same wall two tiles off is quieter");
    float two = f.level[SONAR_N];
    room(&f, -1, 4, -1, -1);
    check(f.level[SONAR_N] < two, "and four tiles off quieter again");
    check(f.level[SONAR_N] > 0.05f, "but still loud enough to hear coming");

    // The whole surface counts, so more of a wall at one distance is louder
    // than less of it -- but only somewhat: inverse square means the nearest
    // face carries most of it, which is why loudness still reads as distance.
    sonar_field_clear(&f);
    sonar_face(&f, SONAR_AXIS_NS, 0.0f, 1.5f);
    sonar_field_finish(&f);
    float one = f.level[SONAR_N];
    room(&f, -1, 1, -1, -1);
    check(f.level[SONAR_N] > one, "a long wall is louder than a single face");
    check(f.level[SONAR_N] < one * 2.0f,
          "but not so much louder that its length drowns its distance");

    // The gradient the ear actually reads: every tile of distance worth
    // something like the same number of decibels, all the way out. A ratio
    // between neighbouring tiles is the same statement.
    float lvl[8];
    for (int d = 0; d <= 7; d++) {
        room(&f, -1, d, -1, -1);
        lvl[d] = f.level[SONAR_N];
    }
    int falls = 1, even = 1;
    for (int d = 1; d <= 6; d++) if (lvl[d] >= lvl[d - 1]) falls = 0;
    // Every tile between touching a wall and five away is worth at least three
    // decibels and at most nine: audible, and never a cliff.
    for (int d = 1; d <= 5; d++) {
        float r = lvl[d - 1] / lvl[d];
        if (r < 1.41f || r > 2.82f) even = 0;
    }
    check(falls, "every tile further out is quieter than the last");
    check(even, "and each is worth between three and nine decibels");
    check(lvl[7] == 0.0f, "at the range there is nothing left");

    room(&f, 0, 0, 0, 0);
    int in_range = 1;
    for (int i = 0; i < SONAR_DIRS; i++)
        if (f.level[i] > 1.0f || f.level[i] < 0.0f) in_range = 0;
    check(in_range, "boxed in, no direction runs past full scale");

    // A corner is two walls and a bit of each other's run: it must bend rather
    // than clip, and must not come out below either wall on its own.
    sonar_field_clear(&f);
    sonar_demo_wall(&f, SONAR_N, 0);
    sonar_face(&f, SONAR_AXIS_NS, 1.0f, 0.5f);   // one more, on top
    sonar_field_finish(&f);
    check(f.level[SONAR_N] <= 1.0f && f.level[SONAR_N] >= against,
          "extra wall on a full side bends up, never over");

    room(&f, 0, -1, -1, 0);
    check(f.level[SONAR_W] > 0.9f && f.level[SONAR_E] > 0.9f,
          "a corridor is full scale on its two sides");
    check(f.level[SONAR_N] == 0.0f && f.level[SONAR_S] == 0.0f,
          "and dead silent along it");
    check(close_to(f.level[SONAR_W], f.level[SONAR_E]),
          "and the same on both sides");

    room(&f, 3, 3, 3, 3);
    check(f.level[SONAR_N] > 0.1f && f.level[SONAR_N] < 0.6f,
          "in the middle of a room every side is present but none is near");

    // The player's own level. Checked here because it needs no device: the notch
    // is a number the mixer reads, so it can be set and read back with nothing
    // plugged in.
    printf("the level\n");
    check(audio_volume_notches() == 5, "there are five notches");
    check(audio_volume(AUDIO_WALLS) == 2 && audio_volume(AUDIO_HEARTS) == 2,
          "and every source starts in the middle of them");
    check(audio_volume_set(AUDIO_WALLS, 4) == 4 && audio_volume(AUDIO_WALLS) == 4,
          "a notch sticks");
    check(audio_volume(AUDIO_HEARTS) == 2, "and moves only its own source");
    check(audio_volume_set(AUDIO_WALLS, 9) == 4, "past the top it stops at the top");
    check(audio_volume_set(AUDIO_WALLS, -3) == 0, "and below the bottom at the bottom");
    audio_volume_set(AUDIO_WALLS, 2);
    check(audio_volume(AUDIO_WALLS) == 2, "and it can be put back");

    printf("steps\n");
    int dx, dy;
    sonar_step(SONAR_N, &dx, &dy);
    check(dx == 0 && dy == 1, "north is +Y");
    sonar_step(SONAR_E, &dx, &dy);
    check(dx == 1 && dy == 0, "east is +X");
    sonar_step(SONAR_W, &dx, &dy);
    check(dx == -1 && dy == 0, "west is -X");
    sonar_step(SONAR_S, &dx, &dy);
    check(dx == 0 && dy == -1, "south is -Y");
    sonar_step(99, &dx, &dy);
    check(dx == 0 && dy == 0, "a direction that is not one stands still");

    printf("\n%s\n", failures ? "FAILURES" : "all ok");
    return failures != 0;
}
