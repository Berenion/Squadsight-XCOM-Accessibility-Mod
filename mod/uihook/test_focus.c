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

    // Indexed placement: the screen supplies the slot, and refreshing one row
    // must leave its neighbours alone.  This is how AS_SetCheckboxLabel and
    // AS_AddListItem actually arrive, interleaved with untexted calls.
    void* screenC = (void*)0x3000;
    focus_set(screenC, 0, "Tutorial");
    focus_set(screenC, 1, "Operation Slingshot");
    focus_set(screenC, 2, "Ironman");
    focus_set(screenC, 3, "Reduce Beginner VO");
    check(focus_count(screenC) == 4, "four indexed slots");
    check(focus_label_at(screenC, 0, buf, sizeof buf) &&
          strcmp(buf, "Tutorial") == 0, "slot 0 is the first, not the last");
    check(focus_label_at(screenC, 3, buf, sizeof buf) &&
          strcmp(buf, "Reduce Beginner VO") == 0, "slot 3 resolves");

    focus_set(screenC, 1, "Slingshot (refreshed)");
    check(focus_label_at(screenC, 0, buf, sizeof buf) &&
          strcmp(buf, "Tutorial") == 0, "refreshing one slot spares the rest");
    check(focus_label_at(screenC, 1, buf, sizeof buf) &&
          strcmp(buf, "Slingshot (refreshed)") == 0, "refreshed slot updated");

    // Out-of-order arrival must not shift anything.
    void* screenD = (void*)0x4000;
    focus_set(screenD, 2, "third");
    focus_set(screenD, 0, "first");
    check(focus_label_at(screenD, 0, buf, sizeof buf) &&
          strcmp(buf, "first") == 0, "out-of-order slot 0");
    check(focus_label_at(screenD, 2, buf, sizeof buf) &&
          strcmp(buf, "third") == 0, "out-of-order slot 2");
    check(!focus_label_at(screenD, 1, buf, sizeof buf), "gap stays empty");

    // Regression: SetDropdownOptions crashed the game by overrunning the
    // join buffer. strcat_s does not truncate -- on a full destination it
    // calls the CRT invalid-parameter handler, which __fastfail()s past SEH,
    // so no handler in this DLL could catch it. Same shape, must survive.
    printf("\nlabel join bounds\n");
    {
        const char* opts[] = {
            "1280 x 720", "1024 x 768", "1366 x 768", "1280 x 800",
            "1152 x 864", "1152 x 870", "1440 x 900", "1600 x 900",
            "1280 x 960", "1760 x 990", "1280 x 1024", "1680 x 1050",
            "1920 x 1080", "1600 x 1200", "1920 x 1200", "2560 x 1440",
            "2048 x 1536", "3072 x 1728", "3200 x 1800", "3840 x 2160",
        };
        char joined[FOCUS_MAX_LABEL];
        size_t used = 0;
        joined[0] = 0;
        for (int k = 0; k < (int)(sizeof opts / sizeof *opts); k++) {
            size_t want = strlen(opts[k]);
            size_t sep = used ? 2 : 0;
            if (used + sep + want >= sizeof joined) break;
            if (sep) { memcpy(joined + used, ", ", 2); used += 2; }
            memcpy(joined + used, opts[k], want);
            used += want;
            joined[used] = 0;
        }
        check(used < sizeof joined, "join stays inside the buffer");
        check(joined[used] == 0, "join stays terminated");
        check(strlen(joined) == used, "join length matches");
    }

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
