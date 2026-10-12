#pragma once
#include <stddef.h>

// What the base has to spend in Research and Engineering, read from the
// game's own actors: "SCIENTISTS: 30. ENGINEERS: 10. ELERIUM: 25. ALLOYS: 40.
// Weapon Fragments: 230." 0 when nothing could be read. See stores.c.
int stores_line(char* out, size_t out_sz);
