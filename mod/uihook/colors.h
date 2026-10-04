#pragma once
#include <stddef.h>

// Colours in words, for the soldier customisation screen (customize.c). The
// game shows hair colour, skin colour and armour tint as bare numbers; what
// stands behind each number is an entry of an XComLinearColorPalette, a
// linear-space RGB the game hands its materials. Nothing here touches the
// game, so test_colors can check it offline.

// A colour name for a linear-space RGB, e.g. "dark brown", "light blue",
// "grey". `hair` names the browns and yellows the way hair is spoken of
// ("blonde", "auburn") rather than as paint ("tan", "beige").
void color_name(float r, float g, float b, int hair, char* out, size_t out_sz);

// How light a linear-space RGB looks, 0 (black) to 1 (white): the sRGB
// lightness, which is what ranks one skin tone against another.
float color_lightness(float r, float g, float b);

// A skin tone's place among `n` tones, by lightness, `rank` 0 being the
// lightest: "lightest", "light", "medium", "dark", "darkest". One tone alone
// is "only tone".
const char* color_tone_word(int rank, int n);

// A linear-space channel as the 0-255 sRGB a screen shows, for logging a
// palette the way a colour picker would write it.
int color_srgb8(float c);
