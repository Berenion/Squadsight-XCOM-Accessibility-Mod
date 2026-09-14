// Offline check of natives_scan against the shipped executables.
//
// Maps the game exe as a module (so sections are laid out and relocated
// exactly as they are at runtime) and runs the same scanner the DLL uses.
// Expected counts come from tools/native_table.py: EW 3336, EU 3377.
//
//   test_natives.exe <path to XComEW.exe|XComGame.exe>

#include <windows.h>
#include <stdio.h>
#include "natives.h"

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_natives <game exe>\n"); return 2; }

    HMODULE mod = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) { fprintf(stderr, "LoadLibraryEx failed (%lu)\n", GetLastError()); return 1; }
    printf("mapped %s at %p\n", argv[1], (void*)mod);

    static NativeEntry tbl[8192];
    int n = natives_scan(mod, tbl, 8192);
    printf("natives discovered: %d\n", n);

    const char* wanted[] = {
        "UGFxMoviePlayerexecActionScriptVoid",
        "AUI_FxsPanelexecInvoke",
        "AUI_FxsPanelexecSetVariable",
        "UGFxObjectexecActionScriptVoid",
    };
    int missing = 0;
    for (int i = 0; i < 4; i++) {
        void* f = natives_find(tbl, n, wanted[i]);
        printf("  %-38s %s", wanted[i], f ? "" : "** MISSING **");
        if (f) printf("rva %p", (void*)((char*)f - (char*)mod));
        printf("\n");
        if (!f) missing++;
    }
    printf(n > 3000 && !missing ? "\nPASS\n" : "\nFAIL\n");
    return (n > 3000 && !missing) ? 0 : 1;
}
