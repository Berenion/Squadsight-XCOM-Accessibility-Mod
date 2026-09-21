#pragma once
#include <windows.h>
#include <stdint.h>
#include "ue3.h"

// Locates the global name table by scanning .data for a TArray whose index 0
// resolves to "None".  `why` receives a human-readable result either way.
int names_init(HMODULE mod, char* why, size_t why_sz);
int names_ready(void);

int name_to_string(const FName* fn, char* out, size_t out_sz);
int object_name(const void* obj, char* out, size_t out_sz);

// The table index of each of `n` names, or -1 for one that is not in the
// table, in a single pass.
//
// For objects.c, which has to find a handful of UClasses among a hundred
// thousand objects. Decoding every object's name to compare it as a string
// costs a read and a decode per object, three times over; comparing FName
// indices costs one integer compare, and this is what turns the names into
// indices. One pass for however many names are wanted, because the pass
// itself is the expensive part.
void names_find(const char* const* texts, int32_t* out, int n);

// Implemented in natives.c; shared so names.c can scan .data too.
int module_section(HMODULE mod, const char* want, uint8_t** base, size_t* size);
