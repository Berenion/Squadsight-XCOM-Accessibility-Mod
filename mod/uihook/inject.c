// Minimal LoadLibrary injector for the UI hook.
//
//   inject.exe <XComEW.exe|XComGame.exe> <full\path\to\xcom_uihook.dll>
//
// launcher.exe does this and the launching too; this stays for injecting into
// a game that is already running, and for scripting.
//
// Both the injector and the DLL must be 32-bit to match the game.

#include "injector.h"

#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: inject <process.exe> <full path to dll>\n");
        return 2;
    }

    DWORD pid = injector_find_pid(argv[1]);
    if (!pid) { fprintf(stderr, "process not running: %s\n", argv[1]); return 1; }

    char detail[512];
    if (!injector_inject(pid, argv[2], detail, sizeof detail)) {
        fprintf(stderr, "%s\n", detail);
        return 1;
    }

    printf("injected into pid %lu (module %s)\n", pid, detail);
    printf("log: xcom_uihook.log next to the game exe\n");
    return 0;
}
