// Offline checks for the focus table and the speech debounce.
//
// These encode the two rules the live behaviour depends on:
//   - a list published once can be indexed later, per object
//   - a lone line is spoken, but a line followed by more is not
//
// Neither needs the game, so neither should first be exercised inside it.

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "focus.h"
#include "speech.h"

static int failures;

static void check(int ok, const char* what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    char buf[FOCUS_MAX_LABEL];
    void* screenA = (void*)0x1000;
    void* screenB = (void*)0x2000;

    printf("focus table\n");

    focus_begin(screenA);
    focus_add(screenA, "SINGLE PLAYER");
    focus_add(screenA, "MULTIPLAYER");
    focus_add(screenA, "LOAD GAME");
    focus_add(screenA, "OPTIONS");
    focus_add(screenA, "EXIT TO DESKTOP");

    check(focus_count(screenA) == 5, "five labels recorded");
    check(focus_label_at(screenA, 0, buf, sizeof buf) &&
          strcmp(buf, "SINGLE PLAYER") == 0, "index 0 resolves");
    check(focus_label_at(screenA, 4, buf, sizeof buf) &&
          strcmp(buf, "EXIT TO DESKTOP") == 0, "index 4 resolves");
    check(!focus_label_at(screenA, 5, buf, sizeof buf), "index past end rejected");
    check(!focus_label_at(screenA, -1, buf, sizeof buf), "negative index rejected");

    // A second screen must not see the first screen's labels.
    focus_begin(screenB);
    focus_add(screenB, "Load Game");
    check(focus_count(screenB) == 1, "second object tracked separately");
    check(focus_label_at(screenA, 1, buf, sizeof buf) &&
          strcmp(buf, "MULTIPLAYER") == 0, "first object still intact");
    check(!focus_label_at(screenB, 1, buf, sizeof buf), "no bleed between objects");

    // Republishing replaces rather than appends.
    focus_begin(screenA);
    focus_add(screenA, "RESUME");
    check(focus_count(screenA) == 1, "republish resets the list");

    check(!focus_label_at((void*)0x9999, 0, buf, sizeof buf), "unknown object empty");

    printf("\nspeech debounce\n");
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0;

    char why[256] = "";
    if (!speech_init(dir, why, sizeof why)) {
        printf("  speech_init failed: %s\n", why);
        return 1;
    }
    printf("  backend: %s\n", why);

    // Cancelled before it settles: a list row, must stay silent.
    speech_say_after("row one of a long list", 250);
    Sleep(60);
    speech_cancel_pending();
    Sleep(500);
    printf("  (silence expected above)\n");

    // Left alone: a real announcement, must be spoken.
    speech_say_after("Classic", 250);
    Sleep(1500);
    printf("  (should have heard: Classic)\n");

    speech_shutdown();

    printf("\n%s\n", failures ? "FAILURES" : "PASS");
    return failures ? 1 : 0;
}
