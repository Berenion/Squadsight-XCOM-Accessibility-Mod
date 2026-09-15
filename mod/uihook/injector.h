// Shared injection back end for inject.exe and launcher.exe.
//
// Both callers are 32-bit, as the game is, so the local address of
// LoadLibraryA is valid in the target and no cross-architecture work is
// needed.  Every failure is reported as a sentence rather than a code: the
// launcher shows these to a player who cannot see the console.
#ifndef INJECTOR_H
#define INJECTOR_H

#include <windows.h>

// PID of the first running process with this exe name, or 0.
DWORD injector_find_pid(const char* exe);

// Waits up to timeout_ms for such a process to appear.  Returns its PID, or 0.
DWORD injector_wait_for_pid(const char* exe, DWORD timeout_ms);

// TRUE once the process owns a visible top-level window with a title, which is
// the earliest outside signal that the engine is up rather than still loading.
BOOL injector_has_window(DWORD pid);

// Loads dll into pid.  On failure fills err with the reason and returns FALSE.
BOOL injector_inject(DWORD pid, const char* dll, char* err, size_t err_len);

#endif
