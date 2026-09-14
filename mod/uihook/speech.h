#pragma once
#include <windows.h>

// NB: sapi.h is deliberately NOT included here.  In C its file-scope `const`
// declarations have external linkage, so including it from more than one
// translation unit produces duplicate symbols at link time.  It belongs to
// speech.c alone; this interface stays plain.

// Brings up Tolk if available, SAPI otherwise, plus the worker thread that
// does the actual speaking.  `why` receives which backend was chosen.
int  speech_init(const char* dll_dir, char* why, size_t why_sz);

// Queues an utterance.  Safe to call from the game's UI thread: it only
// copies into a ring buffer and signals the worker.
void speech_say(const char* utf8);

long speech_dropped(void);
void speech_shutdown(void);
