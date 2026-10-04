// Colours in words. See colors.h.

#include "colors.h"
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

static void put(char* out, size_t out_sz, const char* shade, const char* name)
{
    _snprintf_s(out, out_sz, _TRUNCATE, "%s%s", shade, name);
}

// Dark and light, for a hue that has no name of its own for either.
static const char* shade_of(float l)
{
    if (l < 0.25f) return "dark ";
    if (l > 0.72f) return "light ";
    return "";
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
        if (s < 0.15f)                                   put(out, out_sz, "", "jet black");
        else if (s >= 0.35f && h >= 190.0f && h < 260.0f) put(out, out_sz, "", "blue-black");
        else if (s < 0.35f)                              put(out, out_sz, "", "soft black");
        else                                             put(out, out_sz, "", "brown-black");
        return;
    }

    // Too little colour to name a hue: the greys, by lightness.
    if (s < 0.12f || (s < 0.2f && (l < 0.15f || l > 0.85f))) {
        if (hair) {
            put(out, out_sz, "", l < 0.12f ? "black" : l < 0.45f ? "dark grey" :
                                 l < 0.75f ? "grey" : "white");
        } else {
            put(out, out_sz, "", l < 0.1f ? "black" : l < 0.25f ? "very dark grey" :
                                 l < 0.45f ? "dark grey" : l < 0.65f ? "grey" :
                                 l < 0.85f ? "light grey" : "white");
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
                put(out, out_sz, "", h < 40.0f ? "copper" : "golden yellow");
            } else if (h < 28.0f && s > 0.45f && l >= 0.15f) {
                put(out, out_sz, "", l < 0.32f ? "dark auburn" : l < 0.36f ? "auburn" :
                                     l < 0.6f ? "ginger" : "strawberry blonde");
            // Neighbours the plain ladder gave one name each, in the same
            // log: #976631 and #7f6633 both "brown" (the second leans to
            // olive, hue 40 against 31), #9d8049 and #bf964a both "light
            // brown" (the second twice as saturated -- golden).
            } else if (l >= 0.27f && l < 0.4f && h >= 37.0f) {
                put(out, out_sz, "", "olive brown");
            // And two more pairs: #5f3f1b and #614c1a ("dark brown", hue 32
            // against 42), #3a2918 and #2d1f0d ("very dark brown", the second
            // darker still).
            } else if (l >= 0.18f && l < 0.27f && h >= 37.0f) {
                put(out, out_sz, "", "dark olive brown");
            } else if (l >= 0.1f && l < 0.14f) {
                put(out, out_sz, "", "darkest brown");
            } else if (l >= 0.4f && l < 0.54f && s > 0.45f) {
                put(out, out_sz, "", "golden brown");
            } else {
                put(out, out_sz, "", l < 0.1f ? "black" : l < 0.18f ? "very dark brown" :
                                     l < 0.27f ? "dark brown" : l < 0.4f ? "brown" :
                                     l < 0.52f ? "light brown" : l < 0.65f ? "dark blonde" :
                                     l < 0.85f ? "blonde" : "platinum blonde");
            }
            return;
        }
        if (l < 0.42f && (h < 45.0f || l < 0.3f)) {
            put(out, out_sz, "", l < 0.2f ? "dark brown" : l < 0.32f ? "brown" : "light brown");
        } else if (h < 40.0f) {
            if (s < 0.45f) put(out, out_sz, "", l > 0.7f ? "beige" : "tan");
            else           put(out, out_sz, shade_of(l), "orange");
        } else {
            if (s < 0.45f)      put(out, out_sz, "", "khaki");
            else if (l < 0.45f) put(out, out_sz, "", "gold");
            else                put(out, out_sz, shade_of(l), "yellow");
        }
        return;
    }

    if (h < 15.0f || h >= 345.0f) {
        // A strong red is red even when dark: #971e1a, the palette's dyed red.
        if (hair) {
            put(out, out_sz, "", l < 0.36f && s < 0.6f ? "auburn" : "red");
            return;
        }
        if (l > 0.7f) { put(out, out_sz, "", "pink"); return; }
        if (l < 0.22f) put(out, out_sz, "", "maroon");
        else           put(out, out_sz, l < 0.32f ? "dark " : "", "red");
        return;
    }
    if (h < 90.0f) {
        put(out, out_sz, "", l < 0.35f ? "olive" : l > 0.7f ? "light yellow green" :
                             "yellow green");
        return;
    }
    if (h < 160.0f) { put(out, out_sz, shade_of(l), "green"); return; }
    if (h < 195.0f) { put(out, out_sz, shade_of(l), "teal"); return; }
    if (h < 250.0f) {
        if (l < 0.2f) put(out, out_sz, "", "navy");
        else          put(out, out_sz, shade_of(l), "blue");
        return;
    }
    if (h < 290.0f) { put(out, out_sz, shade_of(l), "purple"); return; }
    put(out, out_sz, "", l > 0.7f ? "pink" : l < 0.25f ? "dark magenta" : "magenta");
}

const char* color_tone_word(int rank, int n)
{
    if (n <= 1) return "only tone";
    if (rank <= 0) return "lightest";
    if (rank >= n - 1) return "darkest";
    float f = (float)rank / (float)(n - 1);
    return f < 0.4f ? "light" : f <= 0.6f ? "medium" : "dark";
}

int color_srgb8(float c)
{
    return (int)(to_srgb(c) * 255.0f + 0.5f);
}
