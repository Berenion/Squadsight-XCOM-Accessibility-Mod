#pragma once

// The log, xcom_uihook.log beside the game exe: the only feedback a run
// gives. A line is copied into memory by logf_ and written by a thread of
// its own every 200 ms (log.c has the reasons), so it is cheap on the game
// thread and survives a crash.

// Opens the log, shared for reading so it can be tailed, and starts its
// writer. Until it is called, and if it fails, logf_ does nothing.
void log_open(const char* path);

// At process exit: writes what is waiting, taking no lock.
void log_on_exit(void);

// A line, printf-style. The same line again is counted, not repeated.
void logf_(const char* fmt, ...);
