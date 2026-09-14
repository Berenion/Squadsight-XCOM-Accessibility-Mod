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

// Implemented in natives.c; shared so names.c can scan .data too.
int module_section(HMODULE mod, const char* want, uint8_t** base, size_t* size);
