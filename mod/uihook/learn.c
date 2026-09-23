// The options menu and sound practice.  See learn.h.

#include "learn.h"
#include "settings.h"
#include "heart.h"
#include "soldier.h"
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
static volatile LONG g_mode;    // MODE_*: who has the numpad

enum { MODE_OFF, MODE_MENU, MODE_PRACTICE, MODE_HEARTS };

// The menu's entries: the settings, with the two things to listen to beside
// the sounds they teach. Negative entries are actions, the rest settings.h ids.
#define ITEM_PRACTICE (-1)
#define ITEM_HEAR     (-2)
static const int MENU[] = {
    SET_FIELD, SET_WALL_LEVEL, ITEM_PRACTICE,
    SET_HEARTS, SET_HEART_SOLO, SET_HEART_LEVEL, SET_ALIENS, SET_ALIEN_LEVEL,
    SET_DOORS, SET_DOOR_LEVEL, SET_WINDOWS, SET_WINDOW_LEVEL, ITEM_HEAR,
    SET_GLIDE,
    SET_COMBAT, SET_SIGHT, SET_TURN, SET_TICKER, SET_OBJECTIVES, SET_NARRATIVE,
};

// Which level setting belongs to which of the mixer's sources. The one place
// they are paired: startup and the menu both go through it.
static const struct { int setting, source; } LEVELS[] = {
    { SET_WALL_LEVEL,  AUDIO_WALLS  },
    { SET_HEART_LEVEL, AUDIO_HEARTS },
    { SET_ALIEN_LEVEL, AUDIO_ALIENS },
    { SET_DOOR_LEVEL,  AUDIO_DOORS  },
    { SET_WINDOW_LEVEL, AUDIO_WINDOWS },
};
#define NLEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))

static int level_source(int setting)
{
    for (int i = 0; i < NLEVELS; i++)
        if (LEVELS[i].setting == setting) return LEVELS[i].source;
    return -1;
}

// A level just changed from the menu is heard at once, on its own: the wall
// field against the tile to the north for this long, or one heartbeat. Only
// in the menu -- in a mission the game's own poll stands aside while it is
// open, so nothing else is sounding.
#define PREVIEW_MS 700
static ULONGLONG g_preview_until;   // the wall preview, renewed by pump
#define ITEMS ((int)(sizeof MENU / sizeof MENU[0]))
static int g_item;              // where the menu's cursor is; kept between visits
static int g_hinted;            // the keys have been named once this run

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
    InterlockedExchange(&g_mode, MODE_PRACTICE);
    // Something audible from the first moment, so that "it is on" and "this is
    // what it sounds like" are not two separate discoveries. North on its own,
    // against the tile, is the loudest and plainest thing the field can do.
    g_dirs = BIT(SONAR_N);
    g_tiles = 0;
    push();
    speech_say_now(
        "Sound practice. 8, 2, 4 and 6 for one side. 7, 9, 1 and 3 for corners. "
        "5 for all four. 0 for silence. Plus and minus for distance. "
        "Star and dot for the level. Slash to go back. "
        "North, against you.");
}

// Practice ends back in the menu, on its own entry, rather than closing
// everything: it was opened from there.
static void leave(void)
{
    InterlockedExchange(&g_mode, MODE_MENU);
    g_dirs = 0;
    audio_field_off();
}

// ---- hear the heartbeats ----------------------------------------------------
//
// A list of the ways an ally can sound. 8 and 2 move and name the entry, 5
// plays it -- a few beats, as often as wanted -- and / goes back to the menu.
// A move plays nothing: the player chooses when to listen. The beats are
// timed from the poll, so the keys stay live while they play.
typedef struct {
    const char* say;
    int kind, dx, dy, hp, hp_max, panicked, wounded;
} HeartDemo;

static const HeartDemo DEMO[] = {
    // Placed in tiles, as the map puts them: pan by east-west tiles, pitch
    // by north-south tiles. The states are heard here, on the listening tile.
    { "Ally, here",                 HEART_ALLY,    0,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, 5 west",               HEART_ALLY,   -5,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, 10 west, 5 north",     HEART_ALLY,  -10,   5, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, 10 east",              HEART_ALLY,   10,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, 10 north",             HEART_ALLY,    0,  10, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, 10 south",             HEART_ALLY,    0, -10, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, far off, 25 north",    HEART_ALLY,    0,  25, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, badly hurt",           HEART_ALLY,    0,   0, 1, 6, 0, SOLDIER_WOUND_NONE },
    { "Ally, panicked",             HEART_ALLY,    0,   0, 6, 6, 1, SOLDIER_WOUND_NONE },
    { "Ally, bleeding out",         HEART_ALLY,    0,   0, 0, 6, 0, SOLDIER_BLEEDING   },
    { "Ally, stabilised",           HEART_ALLY,    0,   0, 0, 6, 0, SOLDIER_STABILISED },
    { "Alien, here",                HEART_ALIEN,   0,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, 5 west",              HEART_ALIEN,  -5,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, 10 west, 5 north",    HEART_ALIEN, -10,   5, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, 10 east",             HEART_ALIEN,  10,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, 10 north",            HEART_ALIEN,   0,  10, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, 10 south",            HEART_ALIEN,   0, -10, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Alien, badly hurt",          HEART_ALIEN,   0,   0, 1, 6, 0, SOLDIER_WOUND_NONE },
    { "Door, here",                 HEART_DOOR,    0,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Door, 5 west",               HEART_DOOR,   -5,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Door, 8 east, 4 north",      HEART_DOOR,    8,   4, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Door, 10 south",             HEART_DOOR,    0, -10, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Window, here",               HEART_WINDOW,  0,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Window, 5 east",             HEART_WINDOW,  5,   0, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Window, 8 west, 4 south",    HEART_WINDOW, -8,  -4, 6, 6, 0, SOLDIER_WOUND_NONE },
    { "Window, 10 north",           HEART_WINDOW,  0,  10, 6, 6, 0, SOLDIER_WOUND_NONE },
};
#define DEMO_ITEMS ((int)(sizeof DEMO / sizeof DEMO[0]))
// Three of a steady heart -- at the calm pace that is already five seconds
// -- and six of a panicked one: two triples, so the figure is heard repeating.
#define DEMO_BEATS       3
#define DEMO_PANIC_BEATS 6
static int        g_demo_total;     // beats this play, for heart_gap's count

static int        g_demo_item;      // kept between visits, like the menu's
static int        g_demo_left;      // beats still to play
static ULONGLONG  g_demo_due;
static HeartSound g_demo_sound;
static int        g_demo_hinted;

static void hear_say(const char* before)
{
    char say[128];
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s. %d of %d",
                before ? before : "", before ? " " : "", DEMO[g_demo_item].say,
                g_demo_item + 1, DEMO_ITEMS);
    speech_say_now(say);
}

static void hear_open(void)
{
    InterlockedExchange(&g_mode, MODE_HEARTS);
    g_demo_left = 0;
    hear_say("Heartbeats.");
    if (!g_demo_hinted) {
        g_demo_hinted = 1;
        speech_say("8 and 2 to choose, 5 to play, slash to go back.");
    }
}

static void hear_play(void)
{
    const HeartDemo* d = &DEMO[g_demo_item];
    heart_sound(d->dx, d->dy, d->hp, d->hp_max, d->panicked, d->wounded, &g_demo_sound);
    g_demo_sound.kind = d->kind;
    if (!audio_hearts_available(d->kind)) {
        speech_say_now("No sound for it yet.");
        return;
    }
    g_demo_total = g_demo_sound.irregular > 0.0f ? DEMO_PANIC_BEATS : DEMO_BEATS;
    g_demo_left = g_demo_total;
    g_demo_due = GetTickCount64();
}

static void hear_poll(const int* hit)
{
    // hit[] follows WATCH in poll(): 5 + d is numpad digit d.
    if (hit[5 + 8] || hit[5 + 2]) {
        g_demo_left = 0;
        g_demo_item = (g_demo_item + (hit[5 + 8] ? -1 : 1) + DEMO_ITEMS) % DEMO_ITEMS;
        hear_say(NULL);
    } else if (hit[5 + 5]) {
        hear_play();
    }
    if (g_demo_left > 0 && GetTickCount64() >= g_demo_due) {
        int beat = g_demo_total - g_demo_left;
        audio_heart_once(&g_demo_sound);
        g_demo_left--;
        g_demo_due = GetTickCount64() +
                     (ULONGLONG)(heart_gap(&g_demo_sound, beat) * 1000.0f);
    }
}

// ---- the menu ---------------------------------------------------------------

// "Glide speed, Normal. 3 of 12". The position is said every time, because
// the list wraps and nothing else tells the player they went round.
static void menu_say_item(const char* before)
{
    char say[256], value[32];
    int id = MENU[g_item];
    const char* name = id == ITEM_PRACTICE ? "Sound practice"
                     : id == ITEM_HEAR     ? "Hear the sounds"
                     : settings_name(id);
    value[0] = 0;
    if (id >= 0) settings_value_text(id, value, sizeof value);
    _snprintf_s(say, sizeof say, _TRUNCATE, "%s%s%s%s%s. %d of %d",
                before ? before : "", before ? " " : "", name,
                value[0] ? ", " : "", value, g_item + 1, ITEMS);
    speech_say_now(say);
}

static void menu_open(void)
{
    InterlockedExchange(&g_mode, MODE_MENU);
    menu_say_item("Mod options.");
    if (!g_hinted) {
        g_hinted = 1;
        speech_say("8 and 2 to move, 4 and 6 to change, 5 to open, slash to close.");
    }
}

static void menu_close(void)
{
    InterlockedExchange(&g_mode, MODE_OFF);
    speech_say_now("Mod options closed.");
}

// The level goes to the mixer as well as the file: it is the one setting the
// mixer holds itself.
static int level_apply(int setting, int notch)
{
    return audio_volume_set(level_source(setting), settings_set(setting, notch));
}

void learn_apply_levels(char* why, size_t why_sz)
{
    size_t used = 0;
    if (why && why_sz) why[0] = 0;
    for (int i = 0; i < NLEVELS; i++) {
        int want = settings_get(LEVELS[i].setting);
        int got = audio_volume_set(LEVELS[i].source, want);
        if (why)
            used += _snprintf_s(why + used, why_sz - used, _TRUNCATE, "%s%s notch %d of %d%s",
                                i ? ", " : "", settings_name(LEVELS[i].setting), got + 1,
                                audio_volume_notches(),
                                got != want ? " (clamped by the mixer)" : "");
        if (used >= why_sz) used = why_sz - 1;
    }
}

static void preview(int setting)
{
    if (level_source(setting) == AUDIO_WALLS) {
        g_dirs = BIT(SONAR_N);
        g_tiles = 0;
        g_preview_until = GetTickCount64() + PREVIEW_MS;
    } else if (level_source(setting) >= 0) {
        // Every other source is a kind of heart, in the same order.
        static const int KIND[AUDIO_SOURCES] = {
            -1, HEART_ALLY, HEART_ALIEN, HEART_DOOR, HEART_WINDOW
        };
        HeartSound s;
        heart_sound(0, 0, -1, -1, 0, SOLDIER_WOUND_NONE, &s);
        s.kind = KIND[level_source(setting)];
        if (audio_hearts_available(s.kind)) audio_heart_once(&s);
        else speech_say("No sound for it yet.");
    }
}

// 4 and 6 on the entry under the cursor. A switch flips either way; a scale
// moves and says its end again when pressed past it.
static void menu_change(int delta)
{
    int id = MENU[g_item];
    if (id < 0) {
        speech_say_now("5 to open.");
        return;
    }
    int level = level_source(id) >= 0;
    if (level) level_apply(id, settings_get(id) + delta);
    else settings_step(id, delta);
    char value[32];
    settings_value_text(id, value, sizeof value);
    speech_say_now(value);
    if (level) preview(id);
    // Otherwise it goes quiet with nothing to say why: nobody is followed
    // until the scanner picks someone.
    if (id == SET_HEART_SOLO && settings_get(id))
        speech_say("The soldier you pick with Page Up and Page Down is the one heard.");
}

static void menu_poll(const int* hit)
{
    // hit[] follows WATCH in poll(): 5 + d is numpad digit d.
    if (hit[5 + 8] || hit[5 + 2]) {
        int step = hit[5 + 8] ? -1 : 1;
        g_item = (g_item + step + ITEMS) % ITEMS;
        menu_say_item(NULL);
    } else if (hit[5 + 4] || hit[5 + 6]) {
        menu_change(hit[5 + 6] ? 1 : -1);
    } else if (hit[5 + 5]) {
        int id = MENU[g_item];
        if (id == ITEM_PRACTICE) enter();
        else if (id == ITEM_HEAR) {
            int any = 0;
            for (int kind = 0; kind < HEART_KINDS; kind++)
                if (audio_hearts_available(kind)) any = 1;
            if (any) hear_open();
            else speech_say_now("No heartbeat sound. It did not load.");
        }
        else if (settings_is_switch(id)) menu_change(1);
        else menu_say_item(NULL);
    }
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
    // while the menu is closed. From practice or the heartbeats it goes back
    // to the menu, on the entry it came from.
    if (hit[0]) {
        if (g_mode == MODE_OFF)         menu_open();
        else if (g_mode == MODE_MENU)   menu_close();
        else if (g_mode == MODE_HEARTS) {
            g_demo_left = 0;
            InterlockedExchange(&g_mode, MODE_MENU);
            menu_say_item(NULL);
        }
        else { leave(); menu_say_item("Sound practice off."); }
        return;
    }
    if (g_mode == MODE_OFF) return;
    if (g_mode == MODE_MENU)   { menu_poll(hit); return; }
    if (g_mode == MODE_HEARTS) { hear_poll(hit); return; }

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
        say_volume(level_apply(SET_WALL_LEVEL,
                               audio_volume(AUDIO_WALLS) + (hit[3] ? 1 : -1)));
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
        if ((g_mode == MODE_PRACTICE ||
             (g_mode == MODE_MENU && GetTickCount64() < g_preview_until)) && has_focus())
            push();
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

int learn_active(void) { return g_mode != MODE_OFF; }

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
    InterlockedExchange(&g_mode, MODE_OFF);
}
