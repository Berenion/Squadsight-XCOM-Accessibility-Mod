// Sound practice.  See learn.h.

#include "learn.h"
#include "sonar.h"
#include "audio.h"
#include "speech.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Fast enough that a key press is never missed -- the shortest tap is a good
// deal longer than this -- and slow enough to cost nothing. It also has to stay
// well under the mixer's lapse window, since this thread is what renews the
// field while practice is on.
#define POLL_MS 15

// How far out the demonstrated wall may be pushed. Past six the field has
// almost nothing left to say, which is worth hearing once and not worth being
// able to sit on.
#define MAX_TILES 6

static HANDLE        g_thread;
static HANDLE        g_wake;
static volatile LONG g_quit;
static volatile LONG g_on;

// What is being demonstrated: a set of directions, and how far off. Touched
// only by the poll thread.
static int g_dirs;              // bits, 1 << SONAR_W and friends
static int g_tiles;

// The game having the foreground is checked here rather than borrowed from
// main.c, because this file deliberately knows nothing about the game: it is
// the one piece of the mod that would work with no game attached at all.
static int has_focus(void)
{
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static const char* DIR_NAME[SONAR_DIRS] = { "West", "North", "South", "East" };

// "against you", "one tile", "four tiles" -- how the distance is spoken, and
// the same words in the log.
static void say_distance(char* out, size_t out_sz)
{
    if (g_tiles == 0)      _snprintf_s(out, out_sz, _TRUNCATE, "against you");
    else if (g_tiles == 1) _snprintf_s(out, out_sz, _TRUNCATE, "one tile");
    else                   _snprintf_s(out, out_sz, _TRUNCATE, "%d tiles", g_tiles);
}

// The level, named rather than numbered: a notch out of five means nothing said
// aloud, and "loudest" tells the player there is no point pressing again.
static void say_volume(int notch)
{
    static const char* NAME[] = {
        "Quietest", "Quiet", "Normal", "Loud", "Loudest"
    };
    int n = audio_volume_notches();
    if (notch >= 0 && notch < n && n == (int)(sizeof NAME / sizeof NAME[0]))
        speech_say_now(NAME[notch]);
    else {
        // The mixer grew a notch and this table did not. Say the number rather
        // than nothing, so the key still plainly works.
        char say[32];
        _snprintf_s(say, sizeof say, _TRUNCATE, "Level %d of %d", notch + 1, n);
        speech_say_now(say);
    }
}

// Builds the field the current state describes and hands it to the mixer.
// Called every tick, not only on a change, because an unrenewed field lapses.
static void push(void)
{
    SonarField f;
    sonar_field_clear(&f);
    for (int d = 0; d < SONAR_DIRS; d++)
        if (g_dirs & (1 << d)) sonar_demo_wall(&f, d, g_tiles);
    sonar_field_finish(&f);
    audio_field(&f);
}

// Names what is sounding now: the sides, then how far. Spoken at once, cutting
// off whatever was being read -- the player is pressing keys to hear things, and
// an announcement they have moved past is only in the way.
static void announce(void)
{
    char say[256];
    char who[128];
    char how_far[32];
    int n = 0;

    who[0] = 0;
    for (int d = 0; d < SONAR_DIRS; d++) {
        if (!(g_dirs & (1 << d))) continue;
        _snprintf_s(who + strlen(who), sizeof who - strlen(who), _TRUNCATE,
                    "%s%s", n ? " and " : "", DIR_NAME[d]);
        n++;
    }

    if (!n) {
        speech_say_now("Silent.");
        return;
    }
    say_distance(how_far, sizeof how_far);
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s, %s.",
                n == SONAR_DIRS ? "All four sides" : who, how_far);
    speech_say_now(say);
}

// Sets what is sounding and says so. `dirs` of 0 is silence.
static void choose(int dirs)
{
    g_dirs = dirs;
    push();
    announce();
}

#define BIT(d) (1 << (d))

// The digits, laid out as the numpad is: each key demonstrates the sides it
// points at, so the key that walks into a wall is the key that plays it.
static int dirs_for_digit(int digit)
{
    switch (digit) {
    case 8: return BIT(SONAR_N);
    case 2: return BIT(SONAR_S);
    case 4: return BIT(SONAR_W);
    case 6: return BIT(SONAR_E);
    case 7: return BIT(SONAR_N) | BIT(SONAR_W);
    case 9: return BIT(SONAR_N) | BIT(SONAR_E);
    case 1: return BIT(SONAR_S) | BIT(SONAR_W);
    case 3: return BIT(SONAR_S) | BIT(SONAR_E);
    case 5: return BIT(SONAR_N) | BIT(SONAR_S) | BIT(SONAR_W) | BIT(SONAR_E);
    case 0: return 0;
    default: return -1;
    }
}

static void enter(void)
{
    InterlockedExchange(&g_on, 1);
    // Something audible from the first moment, so that "it is on" and "this is
    // what it sounds like" are not two separate discoveries. North on its own,
    // against the tile, is the loudest and plainest thing the field can do.
    g_dirs = BIT(SONAR_N);
    g_tiles = 0;
    push();
    speech_say_now(
        "Sound practice. 8, 2, 4 and 6 for one side. 7, 9, 1 and 3 for corners. "
        "5 for all four. 0 for silence. Plus and minus for distance. "
        "Star and dot for the level. Slash to leave. "
        "North, against you.");
}

static void leave(void)
{
    InterlockedExchange(&g_on, 0);
    g_dirs = 0;
    audio_field_off();
    speech_say_now("Sound practice off.");
}

// One pass over the keys. `down` is the previous state, updated in place.
static void poll(int* down)
{
    // A key held while the game was in the background must not read as a fresh
    // press when it comes back.
    if (!has_focus()) {
        memset(down, 0, sizeof(int) * 256);
        return;
    }

    // Numpad * and . are the level. Outside practice, * is the field's own off
    // switch and . is unused; in here neither of those jobs applies, so the pair
    // is free and they sit together under the hand.
    static const int WATCH[] = {
        VK_DIVIDE, VK_ADD, VK_SUBTRACT, VK_MULTIPLY, VK_DECIMAL,
        VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
        VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
    };
    int hit[sizeof WATCH / sizeof WATCH[0]];
    for (int i = 0; i < (int)(sizeof WATCH / sizeof WATCH[0]); i++) {
        int k = WATCH[i];
        int now = (GetAsyncKeyState(k) & 0x8000) != 0;
        hit[i] = now && !down[k];
        down[k] = now;
    }

    // Numpad / first, and on its own: it is the only key that means anything
    // while practice is off.
    if (hit[0]) {
        if (g_on) leave();
        else      enter();
        return;
    }
    if (!g_on) return;

    char how_far[32];
    if (hit[1] && g_tiles < MAX_TILES) {            // further
        g_tiles++;
        push();
        say_distance(how_far, sizeof how_far);
        speech_say_now(how_far);
    } else if (hit[2] && g_tiles > 0) {             // nearer
        g_tiles--;
        push();
        say_distance(how_far, sizeof how_far);
        speech_say_now(how_far);
    } else if (hit[3] || hit[4]) {                   // louder, quieter
        say_volume(audio_volume_set(audio_volume() + (hit[3] ? 1 : -1)));
    } else {
        for (int d = 0; d <= 9; d++) {
            if (!hit[5 + d]) continue;
            int dirs = dirs_for_digit(d);
            if (dirs >= 0) choose(dirs);
            break;
        }
    }
}

static DWORD WINAPI pump(LPVOID arg)
{
    (void)arg;
    // Indexed by virtual key so the loop above can use the codes directly;
    // 256 ints is nothing and it saves a lookup on every key of every pass.
    static int down[256];

    while (!g_quit) {
        poll(down);
        // The field has to be handed over again every pass, because the mixer
        // lets an unrenewed one lapse -- that is what stops practice playing on
        // if this thread ever stops.
        //
        // And it is exactly how practice goes quiet when the game is put in the
        // background: the field is simply not renewed, so it fades out on its
        // own, and comes back when the window does. Practice stays *on* through
        // all of that, because nothing was switched off -- which is right, since
        // with no focus there is no way to press the key that would switch it
        // back on.
        if (g_on && has_focus()) push();
        WaitForSingleObject(g_wake, POLL_MS);
    }
    return 0;
}

int learn_start(char* why, size_t why_sz)
{
    if (why && why_sz) why[0] = 0;
    if (g_thread) {
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "already running");
        return 1;
    }
    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_wake) {
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "no event (0x%08lx)",
                             GetLastError());
        return 0;
    }
    g_quit = 0;
    g_thread = CreateThread(NULL, 0, pump, NULL, 0, NULL);
    if (!g_thread) {
        DWORD err = GetLastError();
        CloseHandle(g_wake);
        g_wake = NULL;
        if (why) _snprintf_s(why, why_sz, _TRUNCATE, "no thread (0x%08lx)", err);
        return 0;
    }
    if (why) _snprintf_s(why, why_sz, _TRUNCATE,
                         "numpad / , polled every %d ms", POLL_MS);
    return 1;
}

int learn_active(void) { return g_on != 0; }

void learn_stop(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    CloseHandle(g_wake);
    g_wake = NULL;
    InterlockedExchange(&g_on, 0);
}
