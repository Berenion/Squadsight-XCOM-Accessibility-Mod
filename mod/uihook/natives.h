#pragma once
#include <windows.h>
#include <stdint.h>

typedef struct {
    const char* name;   // e.g. "UGFxMoviePlayerexecActionScriptVoid"
    void*       func;
} NativeEntry;

// Scans the module's .data for the native registration table.
// Returns the number of entries written to `out`.
int   natives_scan(HMODULE mod, NativeEntry* out, int max);

// Locates a PE section in the mapped module.
int   module_section(HMODULE mod, const char* want, uint8_t** base, size_t* size);
void* natives_find(const NativeEntry* tbl, int n, const char* name);

// The same, given the name WITHOUT its C++ class prefix -- "XGPlayerexec..."
// rather than "AXGPlayerexec..." -- trying both an Actor's and an Object's.
void* natives_find_class(const NativeEntry* tbl, int n, const char* name);
