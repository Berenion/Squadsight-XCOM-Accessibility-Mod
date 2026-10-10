// Colours in words. See colors.h.

#include "colors.h"
#include "strings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// The palettes are linear-space (XComLinearColorPaletteEntry.Primary is a
// LinearColor the materials take as it is), and what the eye sees is the sRGB
// the screen shows, so names are judged on that. Out-of-range values are
// clamped: a palette may push a channel past 1 for a brighter material.
static float to_srgb(float c)
{
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void to_hsl(float r, float g, float b, float* h, float* s, float* l)
{
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *l = (mx + mn) / 2.0f;
    *s = d <= 0.0f ? 0.0f : d / (1.0f - fabsf(2.0f * *l - 1.0f));
    if (d <= 0.0f) { *h = 0.0f; return; }
    float hh;
    if (mx == r)      hh = fmodf((g - b) / d, 6.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else              hh = (r - g) / d + 4.0f;
    hh *= 60.0f;
    if (hh < 0.0f) hh += 360.0f;
    *h = hh;
}

float color_lightness(float r, float g, float b)
{
    r = to_srgb(r); g = to_srgb(g); b = to_srgb(b);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// A colour's name, with "dark %s" / "light %s" around it when `shade` is
// one: a line of its own, since where the word goes, and how it agrees, is
// the language's business.
static void put(char* out, size_t out_sz, StrId shade, StrId name)
{
    if (shade == TXT_EMPTY) strncpy_s(out, out_sz, T(name), _TRUNCATE);
    else tfmt(out, out_sz, shade, T(name));
}

// Dark and light, for a hue that has no name of its own for either.
static StrId shade_of(float l)
{
    if (l < 0.25f) return COLOR_DARK_OF;
    if (l > 0.72f) return COLOR_LIGHT_OF;
    return TXT_EMPTY;
}

void color_name(float r, float g, float b, int hair, char* out, size_t out_sz)
{
    float h, s, l;
    to_hsl(to_srgb(r), to_srgb(g), to_srgb(b), &h, &s, &l);

    // The near-blacks of hair, told apart by the tint they carry. The game's
    // HairColor palette has three of them side by side -- #11110e, #1b1911,
    // #1a120a, entries 12 to 14 in the 2026-10-04 log -- and the hair colour
    // spinner changes nothing but this one colour (XComHumanPawn.SetHairColor
    // -> UpdateHairMaterial sets 'ColorMod' from the entry's Primary), so the
    // tint is the only difference there is to name.
    if (hair && l < 0.1f) {
        if (s < 0.15f)                                   put(out, out_sz, TXT_EMPTY, COLOR_JET_BLACK);
        else if (s >= 0.35f && h >= 190.0f && h < 260.0f) put(out, out_sz, TXT_EMPTY, COLOR_BLUE_BLACK);
        else if (s < 0.35f)                              put(out, out_sz, TXT_EMPTY, COLOR_SOFT_BLACK);
        else                                             put(out, out_sz, TXT_EMPTY, COLOR_BROWN_BLACK);
        return;
    }

    // Too little colour to name a hue: the greys, by lightness.
    if (s < 0.12f || (s < 0.2f && (l < 0.15f || l > 0.85f))) {
        if (hair) {
            put(out, out_sz, TXT_EMPTY, l < 0.12f ? COLOR_BLACK : l < 0.45f ? COLOR_DARK_GREY :
                                 l < 0.75f ? COLOR_GREY : COLOR_WHITE);
        } else {
            put(out, out_sz, TXT_EMPTY, l < 0.1f ? COLOR_BLACK : l < 0.25f ? COLOR_VERY_DARK_GREY :
                                 l < 0.45f ? COLOR_DARK_GREY : l < 0.65f ? COLOR_GREY :
                                 l < 0.85f ? COLOR_LIGHT_GREY : COLOR_WHITE);
        }
        return;
    }

    // The browns, oranges and yellows: where hair and paint part ways.
    if (h >= 15.0f && h < 55.0f) {
        if (hair) {
            // The dyes. The game's HairColor palette ends in vivid colours
            // past the natural ones -- #a75b00 and #a7960d in the 2026-10-04
            // log -- which the natural names called plain "brown".
            if (s > 0.82f && l >= 0.2f && l < 0.6f) {
                put(out, out_sz, TXT_EMPTY, h < 40.0f ? COLOR_COPPER : COLOR_GOLDEN_YELLOW);
            } else if (h < 28.0f && s > 0.45f && l >= 0.15f) {
                put(out, out_sz, TXT_EMPTY, l < 0.32f ? COLOR_DARK_AUBURN : l < 0.36f ? COLOR_AUBURN :
                                     l < 0.6f ? COLOR_GINGER : COLOR_STRAWBERRY_BLONDE);
            // Neighbours the plain ladder gave one name each, in the same
            // log: #976631 and #7f6633 both "brown" (the second leans to
            // olive, hue 40 against 31), #9d8049 and #bf964a both "light
            // brown" (the second twice as saturated -- golden).
            } else if (l >= 0.27f && l < 0.4f && h >= 37.0f) {
                put(out, out_sz, TXT_EMPTY, COLOR_OLIVE_BROWN);
            // And two more pairs: #5f3f1b and #614c1a ("dark brown", hue 32
            // against 42), #3a2918 and #2d1f0d ("very dark brown", the second
            // darker still).
            } else if (l >= 0.18f && l < 0.27f && h >= 37.0f) {
                put(out, out_sz, TXT_EMPTY, COLOR_DARK_OLIVE_BROWN);
            } else if (l >= 0.1f && l < 0.14f) {
                put(out, out_sz, TXT_EMPTY, COLOR_DARKEST_BROWN);
            } else if (l >= 0.4f && l < 0.54f && s > 0.45f) {
                put(out, out_sz, TXT_EMPTY, COLOR_GOLDEN_BROWN);
            } else {
                put(out, out_sz, TXT_EMPTY, l < 0.1f ? COLOR_BLACK : l < 0.18f ? COLOR_VERY_DARK_BROWN :
                                     l < 0.27f ? COLOR_DARK_BROWN : l < 0.4f ? COLOR_BROWN :
                                     l < 0.52f ? COLOR_LIGHT_BROWN : l < 0.65f ? COLOR_DARK_BLONDE :
                                     l < 0.85f ? COLOR_BLONDE : COLOR_PLATINUM_BLONDE);
            }
            return;
        }
        if (l < 0.42f && (h < 45.0f || l < 0.3f)) {
            put(out, out_sz, TXT_EMPTY, l < 0.2f ? COLOR_DARK_BROWN : l < 0.32f ? COLOR_BROWN : COLOR_LIGHT_BROWN);
        } else if (h < 40.0f) {
            if (s < 0.45f) put(out, out_sz, TXT_EMPTY, l > 0.7f ? COLOR_BEIGE : COLOR_TAN);
            else           put(out, out_sz, shade_of(l), COLOR_ORANGE);
        } else {
            if (s < 0.45f)      put(out, out_sz, TXT_EMPTY, COLOR_KHAKI);
            else if (l < 0.45f) put(out, out_sz, TXT_EMPTY, COLOR_GOLD);
            else                put(out, out_sz, shade_of(l), COLOR_YELLOW);
        }
        return;
    }

    if (h < 15.0f || h >= 345.0f) {
        // A strong red is red even when dark: #971e1a, the palette's dyed red.
        if (hair) {
            put(out, out_sz, TXT_EMPTY, l < 0.36f && s < 0.6f ? COLOR_AUBURN : COLOR_RED);
            return;
        }
        if (l > 0.7f) { put(out, out_sz, TXT_EMPTY, COLOR_PINK); return; }
        if (l < 0.22f) put(out, out_sz, TXT_EMPTY, COLOR_MAROON);
        else           put(out, out_sz, l < 0.32f ? COLOR_DARK_OF : TXT_EMPTY, COLOR_RED);
        return;
    }
    if (h < 90.0f) {
        put(out, out_sz, TXT_EMPTY, l < 0.35f ? COLOR_OLIVE : l > 0.7f ? COLOR_LIGHT_YELLOW_GREEN :
                             COLOR_YELLOW_GREEN);
        return;
    }
    if (h < 160.0f) { put(out, out_sz, shade_of(l), COLOR_GREEN); return; }
    if (h < 195.0f) { put(out, out_sz, shade_of(l), COLOR_TEAL); return; }
    if (h < 250.0f) {
        if (l < 0.2f) put(out, out_sz, TXT_EMPTY, COLOR_NAVY);
        else          put(out, out_sz, shade_of(l), COLOR_BLUE);
        return;
    }
    if (h < 290.0f) { put(out, out_sz, shade_of(l), COLOR_PURPLE); return; }
    put(out, out_sz, TXT_EMPTY, l > 0.7f ? COLOR_PINK : l < 0.25f ? COLOR_DARK_MAGENTA : COLOR_MAGENTA);
}

const char* color_tone_word(int rank, int n)
{
    if (n <= 1) return T(COLOR_TONE_ONLY);
    if (rank <= 0) return T(COLOR_TONE_LIGHTEST);
    if (rank >= n - 1) return T(COLOR_TONE_DARKEST);
    float f = (float)rank / (float)(n - 1);
    return T(f < 0.4f ? COLOR_TONE_LIGHT : f <= 0.6f ? COLOR_TONE_MEDIUM : COLOR_TONE_DARK);
}

int color_srgb8(float c)
{
    return (int)(to_srgb(c) * 255.0f + 0.5f);
}
