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
void* natives_find(const NativeEntry* tbl, int n, const char* name);
