// A small window that says what a download is doing, for the launcher's update
// and for the setup.  The work runs on a thread of its own while this one keeps
// the window alive, so that a screen reader hears it come up and Windows does
// not call the program hung.
//
// Needs the IDD_PROGRESS dialog (progress.rc) in the program's resources.
#ifndef PROGRESS_H
#define PROGRESS_H

#include <windows.h>

// Opens the window saying `text`, runs work(ctx) on a thread and returns when
// it ends.  FALSE if the thread could not be started.
BOOL progress_run(HWND owner, const char* text, LPTHREAD_START_ROUTINE work, void* ctx);

// Changes what the window says.  Callable from the work's thread; nothing
// happens when no window is open.  The ctx is unused, so that it fits
// update_fetch_files' progress callback.
void progress_say(void* ctx, const char* text);

#endif
