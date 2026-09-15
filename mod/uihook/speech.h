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

// Says it at once, dropping whatever is queued and cutting off whatever is
// being read.  For an answer the player asked for by pressing a key: moving
// through a menu has to keep up with the keypresses, and everything already
// queued described the state they have just left.
void speech_say_now(const char* utf8);

// Queues an utterance only if nothing cancels it within delay_ms.  Used for
// text that might turn out to be the first row of a list rather than an
// announcement in its own right.
void speech_say_after(const char* utf8, unsigned delay_ms);
void speech_cancel_pending(void);

long speech_dropped(void);
void speech_shutdown(void);
