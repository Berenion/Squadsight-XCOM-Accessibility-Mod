// Offline checks for the colour names the customisation screen speaks.
//
//   build\test_colors.exe
//
// The inputs are written as the sRGB a person would pick (#3b241a) and turned
// into the linear values the game's palettes hold, so each case reads as the
// colour it is meant to be.

#include "colors.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static float to_linear(int c8)
{
    float c = c8 / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void name_is(int rgb, int hair, const char* want)
{
    char got[64];
    color_name(to_linear((rgb >> 16) & 0xFF), to_linear((rgb >> 8) & 0xFF),
               to_linear(rgb & 0xFF), hair, got, sizeof got);
    int ok = strcmp(got, want) == 0;
    printf("#%06x %-5s %-22s %s%s%s\n", rgb, hair ? "hair" : "paint", want,
           ok ? "ok" : "FAIL (got \"", ok ? "" : got, ok ? "" : "\")");
    if (!ok) ++g_failures;
}

static void tone_is(int rank, int n, const char* want)
{
    const char* got = color_tone_word(rank, n);
    int ok = strcmp(got, want) == 0;
    printf("tone %d of %d  %-22s %s\n", rank, n, want, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

int main(void)
{
    name_is(0x000000, 0, "black");
    name_is(0xffffff, 0, "white");
    name_is(0x808080, 0, "grey");
    name_is(0x101010, 1, "jet black");
    name_is(0xc0c0c0, 1, "white");
    name_is(0x3b241a, 1, "very dark brown");
    name_is(0x6a4a30, 1, "brown");
    name_is(0xe6c88c, 1, "blonde");
    name_is(0xa53a1a, 1, "red");
    name_is(0x8a3b12, 1, "dark auburn");
    // The game's own HairColor entries, from the 2026-10-04 log.
    name_is(0x5f3f1b, 1, "dark brown");
    name_is(0x2d1f0d, 1, "darkest brown");
    name_is(0x3a2918, 1, "very dark brown");
    name_is(0x76401e, 1, "dark auburn");
    name_is(0x904f23, 1, "auburn");
    name_is(0x976631, 1, "brown");
    name_is(0x7f6633, 1, "olive brown");
    name_is(0x9d8049, 1, "light brown");
    name_is(0xbf964a, 1, "golden brown");
    name_is(0x614c1a, 1, "dark olive brown");
    name_is(0xac6338, 1, "ginger");
    name_is(0xa75b00, 1, "copper");
    name_is(0xa7960d, 1, "golden yellow");
    name_is(0x971e1a, 1, "red");
    name_is(0x1b1911, 1, "soft black");
    name_is(0x11110e, 1, "jet black");
    name_is(0x1a120a, 1, "brown-black");
    name_is(0x0a1020, 1, "blue-black");
    name_is(0xa5a09f, 1, "grey");
    name_is(0xb99a63, 1, "dark blonde");
    name_is(0x1e3c78, 0, "blue");
    name_is(0x101840, 0, "navy");
    name_is(0x556b2f, 0, "olive");
    name_is(0x2e5e2e, 0, "green");
    name_is(0xd2b48c, 0, "tan");
    name_is(0x5a0f0f, 0, "maroon");
    name_is(0xb02020, 0, "red");
    name_is(0x6a2c8a, 0, "purple");

    tone_is(0, 1, "only tone");
    tone_is(0, 5, "lightest");
    tone_is(1, 5, "light");
    tone_is(2, 5, "medium");
    tone_is(3, 5, "dark");
    tone_is(4, 5, "darkest");

    // Lightness ranks the way the eye does.
    int order = color_lightness(to_linear(0xf0), to_linear(0xd0), to_linear(0xb0)) >
                color_lightness(to_linear(0x60), to_linear(0x40), to_linear(0x30));
    printf("lightness ranks pale above dark  %s\n", order ? "ok" : "FAIL");
    if (!order) ++g_failures;

    // Every entry of the game's HairColor palette (the 2026-10-04 log) has a
    // name of its own: the spinner should never say the same words for two
    // different colours.
    static const int hair[24] = {
        0x5f3f1b, 0x3a2918, 0x2d1f0d, 0x976631, 0x614c1a, 0x7f6633, 0x9d8049, 0xb99a63,
        0x76401e, 0x904f23, 0xac6338, 0x11110e, 0x1b1911, 0x1a120a, 0x971e1a, 0xa75b00,
        0xa7960d, 0x40a740, 0x2ca5a7, 0x0e5cb2, 0x5d18a2, 0xba0085, 0xa5a09f, 0xbf964a,
    };
    char names[24][64];
    for (int i = 0; i < 24; i++)
        color_name(to_linear((hair[i] >> 16) & 0xFF), to_linear((hair[i] >> 8) & 0xFF),
                   to_linear(hair[i] & 0xFF), 1, names[i], sizeof names[i]);
    for (int i = 0; i < 24; i++)
        for (int j = i + 1; j < 24; j++)
            if (strcmp(names[i], names[j]) == 0) {
                printf("hair %d and %d both \"%s\"  FAIL\n", i + 1, j + 1, names[i]);
                ++g_failures;
            }
    printf("the game's 24 hair colours have 24 names\n");

    printf("\n%s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
