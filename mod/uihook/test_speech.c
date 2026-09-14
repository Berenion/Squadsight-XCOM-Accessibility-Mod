// Offline check of the speech backend selection and output.
//
// The speech path is the one part of the hook that cannot be verified by
// reading the game's binary, so it gets exercised on its own rather than
// first appearing inside a live process.
//
//   test_speech.exe ["something to say"]
//
// Prints which backend was chosen and why, then speaks.

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "speech.h"

int main(int argc, char** argv)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0;
    printf("searching from: %s\n", dir);

    char why[256] = "";
    if (!speech_init(dir, why, sizeof why)) {
        printf("speech_init FAILED (%s)\n", why);
        return 1;
    }
    printf("backend: %s\n", why);

    const char* text = argc > 1
        ? argv[1]
        : "Classic. An extreme challenge for experienced XCOM players only.";
    printf("speaking: %s\n", text);
    speech_say(text);

    // The worker speaks asynchronously; give it time before tearing down.
    Sleep(4000);
    printf("dropped: %ld\n", speech_dropped());
    speech_shutdown();
    return 0;
}
