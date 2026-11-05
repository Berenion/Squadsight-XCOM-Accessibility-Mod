// A cutscene's subtitles, said as the game shows them.
//
// The cinematics are Bink movies, and their subtitles never reach Scaleform:
// the engine draws them itself. What it draws comes from two files the game
// ships. Beside each movie, XComGame\Movies\<name>_LOC.txt times its lines --
// a header line ("1000"), then "start end key" per line in milliseconds
// ("3000 6900 TP00_Intro_01") -- and the keys are lines of [Subtitles] in
// XComGame\Localization\<LANG>\Subtitles.<lang>, in all ten of the game's
// languages (CHT only in EW). So this reads the same two files and says each
// line at its start, counted from the movie's start.
//
// Every movie with subtitles is started from script: UIPlayMovie
// (XComPresentationLayerBase: StopCurrentMovie, PlayMovie(bLoop,
// strMovieName), then WaitForMovie and StopCurrentMovie when it waits) --
// which the narrative's Bink moments (UINarrativeMgr.BeginConversation,
// eNarrMoment_Bink: UIPlayMovie(strBink)) and the EW intro (XGSetupPhaseManager:
// UIPlayMovie("XEW_CIN_Intro.bik")) go through -- and the loading movies
// (XGBattle_SP: PlaySpecificLoadingMovie("TP_UnloadScreen.bik")). WaitForMovie
// returns when the movie has ended or the player skipped it, so the lines stop
// there, and on StopCurrentMovie.
//
// Also here: the narrative's voice-only lines, which no screen shows -- the
// Skyranger on landing (see "voice-only conversations" below).
//
// Not here: the comm link (UINarrativeCommLink, main.c's NARRATIVE lines),
// which is Scaleform text, and the loading screen's tips (XEW_ShieldLogo.txt
// lists every tip at "0 0": the engine picks one at random, and which one is
// not said anywhere this can read).

#include "subtitles.h"
#include "game.h"
#include "log.h"
#include "strings.h"
#include "ue3.h"
#include "names.h"
#include "cursor.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SUB_MAX_CUES   96
#define SUB_FILE_MAX   (64 * 1024)
// A line whose end has passed is not said late: the next one is due.
#define SUB_LATE_MS    0

// ---- the files ---------------------------------------------------------------

// "XEW_CIN_Intro.bik" -> "XEW_CIN_Intro": no folder, no extension.
static void movie_base(const char* movie, char* out, size_t out_sz)
{
    const char* s = movie;
    for (const char* p = movie; *p; p++)
        if (*p == '\\' || *p == '/') s = p + 1;
    strncpy_s(out, out_sz, s, _TRUNCATE);
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

// The timing file for `movie`, or 0. The game's own folder first; EW plays
// some of EU's movies, so EU's folder (above XEW) after it. The names tried:
// the movie's own, with _LOC (the subtitled cuts are all *_LOC), and with
// TUT_ before it -- the engine plays "TP_UnloadScreen.bik" as
// TUT_TP_UnloadScreen in the tutorial (XComPresentationLayerBase reads
// GetLastMovieName for "TUT_TP_UnloadScreen"), whose file is
// TUT_TP_UnloadScreen_LOC.txt.
static int timing_file(const char* root, const char* movie, char* path, size_t path_sz)
{
    char base[128];
    movie_base(movie, base, sizeof base);
    // The file on disk is "1080_SR_hover_02_LOC.bik" (EU also "1280x720_..."):
    // should the engine's player be given that name, the prefix goes.
    char* u = strchr(base, '_');
    if (u && u > base) {
        int prefix = 1;
        for (char* q = base; q < u; q++)
            if (!(*q >= '0' && *q <= '9') && *q != 'x') prefix = 0;
        if (prefix) memmove(base, u + 1, strlen(u + 1) + 1);
    }
    if (!base[0]) return 0;
    static const char* const DIRS[] = { "XComGame\\Movies\\", "..\\XComGame\\Movies\\" };
    static const char* const FORMS[] = { "%s%s%s.txt", "%s%s%s_LOC.txt",
                                         "%s%sTUT_%s_LOC.txt", "%s%sTUT_%s.txt" };
    for (int d = 0; d < 2; d++)
        for (int f = 0; f < 4; f++) {
            _snprintf_s(path, path_sz, _TRUNCATE, FORMS[f], root, DIRS[d], base);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    path[0] = 0;
    return 0;
}

// The timed lines of a timing file: "start end key" after the header line.
// A line at "0 0" is a loading tip, which has no time (see the top).
static int read_timing(const char* path, SubtitleCue* out, int max)
{
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") || !f) return 0;
    char* buf = (char*)malloc(SUB_FILE_MAX + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, SUB_FILE_MAX, f);
    fclose(f);
    buf[got] = 0;

    int n = 0, first = 1;
    char* ctx = NULL;
    for (char* line = strtok_s(buf, "\r\n", &ctx); line && n < max;
         line = strtok_s(NULL, "\r\n", &ctx)) {
        if (first) { first = 0; continue; }   // the header
        int a, b;
        char key[64];
        if (sscanf_s(line, "%d %d %63s", &a, &b, key, (unsigned)sizeof key) != 3) continue;
        if (a == 0 && b == 0) continue;
        out[n].start_ms = a;
        out[n].end_ms = b;
        strcpy_s(out[n].key, sizeof out[n].key, key);
        out[n].text[0] = 0;
        out[n].found = 0;
        out[n].always = 0;
        n++;
    }
    free(buf);
    return n;
}

typedef struct {
    SubtitleCue* cue;
    int          n;
} LocFill;

// One line of Subtitles.<lang>: the text of every cue with its key that has
// none yet. Blank lines (" ", the English intro's opening quote, which is in
// the picture) stay blank and are not said.
static int fill_line(const wchar_t* section, const wchar_t* key, const wchar_t* value, void* ctx)
{
    LocFill* l = (LocFill*)ctx;
    if (_wcsicmp(section, L"Subtitles") != 0) return 1;
    char k[64];
    if (!WideCharToMultiByte(CP_UTF8, 0, key, -1, k, sizeof k, NULL, NULL)) return 1;
    for (int i = 0; i < l->n; i++) {
        SubtitleCue* c = &l->cue[i];
        if (c->found || _stricmp(c->key, k) != 0) continue;
        c->found = 1;
        if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, c->text, sizeof c->text, NULL, NULL))
            c->text[0] = 0;
        // Trim, so " " is nothing.
        char* s = c->text;
        while (*s == ' ' || *s == '\t') s++;
        if (s != c->text) memmove(c->text, s, strlen(s) + 1);
        size_t len = strlen(c->text);
        while (len && (c->text[len - 1] == ' ' || c->text[len - 1] == '\t')) c->text[--len] = 0;
    }
    return 1;
}

int subtitles_load_from(const char* root, const char* lang, const char* movie,
                        SubtitleCue* out, int max, char* file, size_t file_sz)
{
    char path[MAX_PATH];
    if (file && file_sz) file[0] = 0;
    if (!timing_file(root, movie, path, sizeof path)) return 0;
    if (file && file_sz) strncpy_s(file, file_sz, path, _TRUNCATE);
    int n = read_timing(path, out, max);
    if (!n) return 0;
    LocFill l = { out, n };
    game_loc_each(root, lang, "Subtitles", fill_line, &l);
    // A line the language lacks, in English, as the engine falls back to INT.
    // Only a key missing from the file: one there as " " is blank on purpose
    // (the English intro's opening quote is in the picture; the Chinese
    // subtitles it), and fill_line passes over every cue already found.
    int missing = 0;
    for (int i = 0; i < n; i++) missing |= !out[i].found;
    if (missing && _stricmp(lang, "INT") != 0)
        game_loc_each(root, "INT", "Subtitles", fill_line, &l);

    // The intro's opening quote (TP00_Intro_01..03, both builds' intros) is
    // said whatever the option, as the picture shows it whatever the option.
    // The English file leaves it blank because it is written in the picture;
    // every other language subtitles it. Blank, it is the mod's own line
    // (strings.def, SUB_QUOTE_*, translated from those files).
    static const struct { const char* key; StrId id; } QUOTE[] = {
        { "TP00_Intro_01", SUB_QUOTE_1 },
        { "TP00_Intro_02", SUB_QUOTE_2 },
        { "TP00_Intro_03", SUB_QUOTE_AUTHOR },
    };
    for (int i = 0; i < n; i++)
        for (int q = 0; q < 3; q++) {
            if (_stricmp(out[i].key, QUOTE[q].key) != 0) continue;
            out[i].always = 1;
            if (!out[i].text[0])
                strncpy_s(out[i].text, sizeof out[i].text, T(QUOTE[q].id), _TRUNCATE);
        }
    return n;
}

// ---- keeping time ------------------------------------------------------------
//
// Two tracks, each a thread of its own: the movie, and the voice-only
// conversation (below). They can overlap -- a voice line under a loading
// movie -- and a new one on a track replaces what that track was saying.

#define VOICE_MAX_CUES 32

typedef struct {
    const char*   what;           // "movie" / "voice", for the log
    SRWLOCK       lock;
    HANDLE        wake;
    volatile LONG gen;            // bumped on every start and end
    char          name[128];      // under lock
    ULONGLONG     started_at;     // under lock
    int           playing;        // under lock
    void*         tag;            // under lock: the voice's AudioComponent
    int           from_files;     // the movie: the thread reads the files
    SubtitleCue*  given;          // the voice: the lines, read on the game thread
    int           ngiven;         // under lock
    int           max;
} Track;

static SubtitleCue   g_voice_given[VOICE_MAX_CUES];
static Track         g_movie = { "movie", SRWLOCK_INIT, NULL, 0, "", 0, 0, NULL, 1, NULL, 0,
                                 SUB_MAX_CUES };
static Track         g_voice = { "voice", SRWLOCK_INIT, NULL, 0, "", 0, 0, NULL, 0,
                                 g_voice_given, 0, VOICE_MAX_CUES };
static SubtitleSayFn g_say, g_say_always;
static Track         g_line;          // defined with the tentpoles, below
static volatile LONG g_movie_last_ms;  // the playing movie's last line ends here

static void track_start(Track* t, const char* name, void* tag, const SubtitleCue* cues, int n)
{
    if (!t->wake) return;
    AcquireSRWLockExclusive(&t->lock);
    strncpy_s(t->name, sizeof t->name, name, _TRUNCATE);
    t->started_at = GetTickCount64();
    t->playing = 1;
    t->tag = tag;
    if (!t->from_files) {
        if (n > t->max) n = t->max;
        memcpy(t->given, cues, sizeof *cues * (size_t)n);
        t->ngiven = n;
    }
    InterlockedIncrement(&t->gen);
    ReleaseSRWLockExclusive(&t->lock);
    SetEvent(t->wake);
}

// `tag` NULL ends whatever is playing; otherwise only the one it started with.
static void track_end(Track* t, void* tag, const char* how)
{
    if (!t->wake) return;
    AcquireSRWLockExclusive(&t->lock);
    int was = t->playing && (!tag || tag == t->tag);
    ULONGLONG ran = GetTickCount64() - t->started_at;
    char name[128];
    strcpy_s(name, sizeof name, t->name);
    if (was) {
        t->playing = 0;
        t->tag = NULL;
        InterlockedIncrement(&t->gen);
    }
    ReleaseSRWLockExclusive(&t->lock);
    if (!was) return;
    logf_("subtitles: %s \"%s\" over after %llu ms (%s)\n", t->what, name, ran, how);
    SetEvent(t->wake);
}

static DWORD WINAPI track_thread(void* arg)
{
    Track* t = (Track*)arg;
    SubtitleCue* cue = (SubtitleCue*)malloc(sizeof *cue * (size_t)t->max);
    if (!cue) return 0;
    for (;;) {
        WaitForSingleObject(t->wake, INFINITE);
    again:;
        AcquireSRWLockShared(&t->lock);
        LONG gen = t->gen;
        int playing = t->playing;
        ULONGLONG started = t->started_at;
        char name[128];
        strcpy_s(name, sizeof name, t->name);
        int n = 0;
        if (playing && !t->from_files) {
            n = t->ngiven;
            memcpy(cue, t->given, sizeof *cue * (size_t)n);
        }
        ReleaseSRWLockShared(&t->lock);
        if (!playing) continue;

        if (t->from_files) {
            char root[MAX_PATH], file[MAX_PATH];
            game_root(root, sizeof root);
            const char* lang = strings_lang();
            n = subtitles_load_from(root, lang, name, cue, t->max, file, sizeof file);
            int texts = 0;
            for (int i = 0; i < n; i++) texts += cue[i].text[0] != 0;
            if (!file[0])
                logf_("subtitles: no timing file for \"%s\" -- nothing to say\n", name);
            else
                logf_("subtitles: %d timed lines (%d with text, %s) from %s\n", n, texts, lang,
                      file);
        }

        int said = 0;
        for (int i = 0; i < n; ) {
            if (t->gen != gen) goto again;
            ULONGLONG elapsed = GetTickCount64() - started;
            const SubtitleCue* c = &cue[i];
            if ((ULONGLONG)c->start_ms <= elapsed) {
                if (!c->text[0]) {
                    // A blank line: the text is in the picture.
                } else if ((ULONGLONG)c->end_ms + SUB_LATE_MS < elapsed) {
                    logf_("subtitles: %s missed (due at %d ms, now %llu)\n",
                          c->key, c->start_ms, elapsed);
                } else {
                    logf_("subtitles: %s at %d ms (+%llu)%s: \"%s\"\n", c->key, c->start_ms,
                          elapsed, c->always ? ", said whatever the option" : "", c->text);
                    SubtitleSayFn say = c->always ? g_say_always : g_say;
                    if (say) say(c->text);
                    said++;
                }
                i++;
                continue;
            }
            DWORD wait = (DWORD)((ULONGLONG)c->start_ms - elapsed);
            if (WaitForSingleObject(t->wake, wait) == WAIT_OBJECT_0) goto again;
        }
        if (n && t->gen == gen)
            logf_("subtitles: %s \"%s\" -- all %d lines due, %d said\n", t->what, name, n, said);
    }
}

// The same movie told twice -- by the script's call and by the engine's
// player underneath it (main.c, hook_gtplay) -- within this is one start.
#define MOVIE_SAME_START_MS 2000

void subtitles_movie_started(const char* movie, const char* how)
{
    if (!movie || !*movie) return;
    char base[128], was[128];
    movie_base(movie, base, sizeof base);
    AcquireSRWLockShared(&g_movie.lock);
    movie_base(g_movie.name, was, sizeof was);
    int again = g_movie.playing && _stricmp(base, was) == 0 &&
                GetTickCount64() - g_movie.started_at < MOVIE_SAME_START_MS;
    ReleaseSRWLockShared(&g_movie.lock);
    if (again) {
        logf_("subtitles: movie \"%s\" (%s) is the one already started\n", movie, how);
        return;
    }
    // A movie with no timed lines (a loading screen: the black one, the
    // shield logo with its untimed tips) does not cut short one that has them,
    // nor count as one playing (subtitles_movie_busy).
    char root[MAX_PATH], path[MAX_PATH];
    game_root(root, sizeof root);
    static SubtitleCue timing[SUB_MAX_CUES];
    int n = timing_file(root, movie, path, sizeof path)
          ? read_timing(path, timing, SUB_MAX_CUES) : 0;
    if (!n) {
        logf_("subtitles: movie \"%s\" started (%s) -- no timed lines, nothing to say\n", movie,
              how);
        return;
    }
    int last = 0;
    for (int i = 0; i < n; i++)
        if (timing[i].end_ms > last) last = timing[i].end_ms;
    logf_("subtitles: movie \"%s\" started (%s)\n", movie, how);
    InterlockedExchange(&g_movie_last_ms, last);
    track_start(&g_movie, movie, NULL, NULL, 0);
}

// ---- held until the movies are over -------------------------------------------
//
// EU's first mission: the briefing (UIBriefing.AS_SetLoadingMessage) is drawn
// before the intro and TUT_MissionPrep play over it, so it was said before the
// opening quote (reported 2026-11-05, 12:19 EU log: BRIEFING at [184], then
// CIN_Intro_Movie started); EW draws it after them. A held line waits
// HOLD_FIRST_MS for a movie to start, then until no movie with lines has
// played for HOLD_GAP_MS (TUT_MissionPrep starts the moment the intro ends),
// then is said. Later held lines ("READY TO ENGAGE") queue behind it.

#define HOLD_FIRST_MS 1000
#define HOLD_GAP_MS   1500
#define HOLD_MAX_MS   (10 * 60 * 1000)
#define HOLD_LINES    4

static CRITICAL_SECTION g_hold_lock;
static HANDLE           g_hold_wake;
static char             g_held[HOLD_LINES][2560];
static int              g_nheld;
static SubtitleSayFn    g_hold_say;

int subtitles_movie_busy(void)
{
    AcquireSRWLockShared(&g_movie.lock);
    int busy = g_movie.playing &&
               GetTickCount64() - g_movie.started_at < (ULONGLONG)g_movie_last_ms + 500;
    ReleaseSRWLockShared(&g_movie.lock);
    return busy;
}

void subtitles_say_after_movies(const char* text, SubtitleSayFn say)
{
    if (!text || !*text) return;
    if (!g_hold_wake) { if (say) say(text); return; }
    EnterCriticalSection(&g_hold_lock);
    g_hold_say = say;
    if (g_nheld < HOLD_LINES) strncpy_s(g_held[g_nheld++], sizeof g_held[0], text, _TRUNCATE);
    LeaveCriticalSection(&g_hold_lock);
    logf_("subtitles: held until the movies are over: \"%.80s\"\n", text);
    SetEvent(g_hold_wake);
}

static DWORD WINAPI hold_thread(void* arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_hold_wake, INFINITE);
        Sleep(HOLD_FIRST_MS);
        ULONGLONG since = GetTickCount64(), quiet_from = 0;
        int waited = 0;
        while (GetTickCount64() - since < HOLD_MAX_MS) {
            if (subtitles_movie_busy()) { quiet_from = 0; waited = 1; }
            else if (!quiet_from) quiet_from = GetTickCount64();
            if (quiet_from && (!waited || GetTickCount64() - quiet_from >= HOLD_GAP_MS)) break;
            Sleep(100);
        }
        for (;;) {
            char line[2560];
            SubtitleSayFn say;
            EnterCriticalSection(&g_hold_lock);
            if (!g_nheld) { LeaveCriticalSection(&g_hold_lock); break; }
            strcpy_s(line, sizeof line, g_held[0]);
            memmove(g_held[0], g_held[1], sizeof g_held[0] * (size_t)(g_nheld - 1));
            g_nheld--;
            say = g_hold_say;
            LeaveCriticalSection(&g_hold_lock);
            logf_("subtitles: said after %s: \"%.80s\"\n", waited ? "the movies" : "a second",
                  line);
            if (say) say(line);
        }
    }
}

void subtitles_movie_ended(const char* how)
{
    track_end(&g_movie, NULL, how);
}

int subtitles_start(SubtitleSayFn say, SubtitleSayFn say_always, char* why, size_t why_sz)
{
    g_say = say;
    g_say_always = say_always;
    InitializeCriticalSection(&g_hold_lock);
    g_hold_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_hold_wake && !CreateThread(NULL, 0, hold_thread, NULL, 0, NULL)) {
        CloseHandle(g_hold_wake);
        g_hold_wake = NULL;          // held lines are then said at once
    }
    Track* tracks[3] = { &g_movie, &g_voice, &g_line };
    for (int i = 0; i < 3; i++) {
        tracks[i]->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!tracks[i]->wake || !CreateThread(NULL, 0, track_thread, tracks[i], 0, NULL)) {
            _snprintf_s(why, why_sz, _TRUNCATE, "no %s thread (0x%08lx) -- its lines will be silent",
                        tracks[i]->what, GetLastError());
            if (tracks[i]->wake) { CloseHandle(tracks[i]->wake); tracks[i]->wake = NULL; }
            return 0;
        }
    }
    _snprintf_s(why, why_sz, _TRUNCATE,
                "cutscene lines timed from XComGame\\Movies, voice-only lines from their waves");
    return 1;
}

// ---- voice-only conversations -------------------------------------------------
//
// The lines a narrative moment of type VoiceOnly or VoiceOnlyMissionStream
// (XComNarrativeMoment.IsVoiceOnly: eType 6, 7) says -- the Skyranger's
// "Central, this is Big Sky..." on landing. Every other narrative goes to a
// screen (UINarrativeMgr.CheckForNextConversation: the comm link or the
// popup, whose AS_SetText main.c says); these never do. OnConversationLoaded
// hands them to BeginVOOnlyConversation, which only plays the cue:
// CreateAudioComponent(ResolvedCue, true, true), with
// bToggleDontStartNewWaveAfterStart false, so the conversation node (an
// XComConversationNode, a SoundNodeConcatenator) plays its lines back to back.
// So each line is one child of that node, its text the wave's SpokenText (a
// localized property: the player's language, from the *WaveData files) and its
// length the wave's Duration; a line starts where the ones before it end.

static int node_class(const void* node, char* cls, size_t cls_sz)
{
    return node && unit_is_live((void*)node) && object_class_name(node, cls, cls_sz);
}

// The child nodes of a sound node, at most `max`.
static int node_children(void* node, void** out, int max)
{
    static FieldSlot s_children;
    const void* p = NULL;
    if (!field_ptr(node, "ChildNodes", &s_children, sizeof(FString), &p)) return 0;
    const FString* a = (const FString*)p;     // a TArray has the FString's shape
    int n = a->Num;
    if (n <= 0 || !a->Data) return 0;
    if (n > max) n = max;
    if (!readable(a->Data, sizeof(void*) * (size_t)n)) return 0;
    memcpy(out, a->Data, sizeof(void*) * (size_t)n);
    return n;
}

// The waves under `node`, depth first, at most `max`.
static int node_waves(void* node, void** out, int max, int depth)
{
    char cls[64];
    if (depth > 8 || max <= 0 || !node_class(node, cls, sizeof cls)) return 0;
    if (strcmp(cls, "SoundNodeWave") == 0) { out[0] = node; return 1; }
    void* kids[16];
    int nk = node_children(node, kids, 16), got = 0;
    for (int i = 0; i < nk && got < max; i++)
        if (kids[i]) got += node_waves(kids[i], out + got, max - got, depth + 1);
    return got;
}

static void* find_conversation(void* node, int depth)
{
    char cls[64];
    if (depth > 8 || !node_class(node, cls, sizeof cls)) return NULL;
    if (strcmp(cls, "XComConversationNode") == 0) return node;
    void* kids[16];
    int nk = node_children(node, kids, 16);
    for (int i = 0; i < nk; i++) {
        void* c = kids[i] ? find_conversation(kids[i], depth + 1) : NULL;
        if (c) return c;
    }
    return NULL;
}

// The lines of the conversation `audio_component` plays, one per child of its
// XComConversationNode, timed back to back from `offset_ms`. The count; the
// cue's name in `cue_name`, the node in *conv_out.
static int conversation_lines(void* audio_component, int offset_ms, SubtitleCue* cues, int max,
                              char* cue_name, size_t cue_name_sz, void** conv_out)
{
    static FieldSlot s_cue, s_first, s_text, s_dur;
    const void* p = NULL;
    void* cue = NULL;
    cue_name[0] = 0;
    if (conv_out) *conv_out = NULL;
    if (audio_component && field_ptr(audio_component, "SoundCue", &s_cue, sizeof(void*), &p))
        cue = *(void* const*)p;
    if (!cue || !object_name(cue, cue_name, cue_name_sz)) {
        logf_("subtitles: a conversation with no cue read\n");
        return 0;
    }
    void* first = NULL;
    if (field_ptr(cue, "FirstNode", &s_first, sizeof(void*), &p)) first = *(void* const*)p;
    void* conv = find_conversation(first, 0);
    if (!conv) {
        logf_("subtitles: cue %s has no conversation node\n", cue_name);
        return 0;
    }
    if (conv_out) *conv_out = conv;

    void* lines[VOICE_MAX_CUES];
    if (max > VOICE_MAX_CUES) max = VOICE_MAX_CUES;
    int nl = node_children(conv, lines, max), n = 0;
    float at = 0.0f;
    for (int i = 0; i < nl; i++) {
        void* waves[8];
        int nw = lines[i] ? node_waves(lines[i], waves, 8, 0) : 0;
        if (!nw) continue;
        float dur = 0.0f;
        if (field_ptr(waves[0], "Duration", &s_dur, sizeof(float), &p)) dur = *(const float*)p;
        SubtitleCue* c = &cues[n];
        memset(c, 0, sizeof *c);
        c->start_ms = offset_ms + (int)(at * 1000.0f);
        c->end_ms = offset_ms + (int)((at + dur) * 1000.0f) + 1000;   // up to a second late
        object_name(waves[0], c->key, sizeof c->key);
        if (nw > 1) {
            // A choice of waves (a SoundNodeRandom): which one plays is not
            // told anywhere this can read, so none is said.
            logf_("subtitles: line %d of %s is one of %d waves -- not said\n", i, cue_name, nw);
        } else if (field_ptr(waves[0], "SpokenText", &s_text, sizeof(FString), &p)) {
            read_fstring((const FString*)p, c->text, sizeof c->text);
        }
        logf_("subtitles:   %s line %d: %s, %.1f s, \"%s\"\n", cue_name, n, c->key, dur, c->text);
        at += dur;
        n++;
    }
    return n;
}

void subtitles_voice_started(void* audio_component, const char* how)
{
    static SubtitleCue cues[VOICE_MAX_CUES];
    char cue_name[128];
    int n = conversation_lines(audio_component, 0, cues, VOICE_MAX_CUES, cue_name,
                               sizeof cue_name, NULL);
    if (!n) return;
    logf_("subtitles: voice-only %s (%s): %d lines\n", cue_name, how, n);
    track_start(&g_voice, cue_name, audio_component, cues, n);
}

// ---- tentpoles, and any line no screen draws -----------------------------------
//
// The landing (reported again 2026-11-05, 11:51 log): the tutorial's
// "Central, this is Big Sky. Looks like we found the crash site." and the
// three lines after it (TUT_XCOMDeployed_08..11) are a tentpole, eNarrMoment
// 5 -- the log has `CreateAudioComponent from StartTentpoleRemoteEvent` and
// then no comm-link line until _12 ("Central, you getting all this?"), the
// next moment. A tentpole fires a remote event for its matinee
// (UINarrativeMgr.StartTentpoleRemoteEvent) and makes the cue's component
// unplayed; Kismet's PlayNextDialogueLine then steps it line by line
// (NextDialogueLine -> UpdateConversationDialogueBox, which asks the node's
// native GetCurrentDialogue and shows the comm link), unless the matinee
// carries the voice itself (XComConversationNode.bMatineeContainsVoice).
//
// So two things. Every line GetCurrentDialogue gives is said unless a comm
// link or narrative popup draws it within LINE_SHOWN_MS (main.c tells
// subtitles_dialogue_shown) -- the box draws it in the same call, so a line
// it shows is never said twice. And a tentpole's lines are read when it
// starts and, if nothing steps them within TENTPOLE_GRACE_MS, said by their
// durations from then on: the matinee's own timing is not read, so this is
// the fallback, and the first stepped line cancels it.

#define LINE_SHOWN_MS      500
#define TENTPOLE_GRACE_MS 1500

static SubtitleCue g_line_given[1];
static Track       g_line = { "line", SRWLOCK_INIT, NULL, 0, "", 0, 0, NULL, 0, g_line_given, 0, 1 };
static void*       g_tentpole;          // the tentpole's component, game thread only

void subtitles_tentpole_started(void* audio_component)
{
    static SubtitleCue cues[VOICE_MAX_CUES];
    char cue_name[128];
    void* conv = NULL;
    int n = conversation_lines(audio_component, TENTPOLE_GRACE_MS, cues, VOICE_MAX_CUES,
                               cue_name, sizeof cue_name, &conv);
    if (!n) return;
    static FieldSlot s_mat;
    const void* p = NULL;
    uint32_t matinee_voice = 0xFFFFFFFF;   // the class's packed bools, as they are
    if (conv && field_ptr(conv, "bMatineeContainsVoice", &s_mat, sizeof(uint32_t), &p))
        matinee_voice = *(const uint32_t*)p;
    logf_("subtitles: tentpole %s: %d lines (bools 0x%08x, bMatineeContainsVoice first) -- said by their "
          "durations unless the game steps them\n", cue_name, n, matinee_voice);
    g_tentpole = audio_component;
    track_start(&g_voice, cue_name, audio_component, cues, n);
}

void subtitles_dialogue(const char* text)
{
    if (!text || !*text) return;
    // The game steps its lines: the tentpole fallback is not wanted.
    if (g_tentpole) {
        track_end(&g_voice, g_tentpole, "the game steps the lines");
        g_tentpole = NULL;
    }
    SubtitleCue c;
    memset(&c, 0, sizeof c);
    c.start_ms = LINE_SHOWN_MS;
    c.end_ms = 60000;
    strcpy_s(c.key, sizeof c.key, "GetCurrentDialogue");
    strncpy_s(c.text, sizeof c.text, text, _TRUNCATE);
    track_start(&g_line, text, NULL, &c, 1);
}

// Two lines alike in their letters and digits: the capture's copy of a line
// may come with markup or spacing GetCurrentDialogue's has not, and a miss
// here would say every comm-link line twice.
static int same_words(const char* a, const char* b)
{
    for (;;) {
        while (*a && (unsigned char)*a < 0x80 && !isalnum((unsigned char)*a)) a++;
        while (*b && (unsigned char)*b < 0x80 && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++, b++;
    }
}

void subtitles_dialogue_shown(const char* text)
{
    if (!text || !*text || !g_line.wake) return;
    AcquireSRWLockShared(&g_line.lock);
    int same = g_line.playing && same_words(g_line.given[0].text, text);
    ReleaseSRWLockShared(&g_line.lock);
    if (same) track_end(&g_line, NULL, "drawn on screen, said from there");
}

void subtitles_voice_stopped(void* audio_component, const char* how)
{
    track_end(&g_voice, audio_component, how);
    if (audio_component && audio_component == g_tentpole) g_tentpole = NULL;
}


// ---- the movie's name ------------------------------------------------------

static int is_movie_name(const char* s)
{
    size_t n = strlen(s);
    return n > 4 && (_stricmp(s + n - 4, ".bik") == 0 || _stricmp(s + n - 4, ".bk2") == 0);
}

int subtitles_movie_name(void* stack, int any_string, char* out, size_t out_sz)
{
    out[0] = 0;
    if (!readable(stack, 0x24)) return 0;
    uint8_t* fr = (uint8_t*)stack;
    void*    node   = *(void**)(fr + FFRAME_NODE);
    uint8_t* locals = *(uint8_t**)(fr + FFRAME_LOCALS);
    uint8_t* code   = *(uint8_t**)(fr + FFRAME_CODE);

    // The caller's own string: UIPlayMovie's strMovieName, the HQ's
    // strMovieFile.
    if (node && locals && readable((uint8_t*)node + USTRUCT_CHILDREN, sizeof(void*))) {
        void* prop = *(void**)((uint8_t*)node + USTRUCT_CHILDREN);
        for (int guard = 0; prop && guard < 64; guard++) {
            if (!readable(prop, UPROPERTY_OFFSET + 4)) break;
            uint32_t off = *(uint32_t*)((uint8_t*)prop + UPROPERTY_OFFSET);
            char s[160];
            if (off < 0x1000 && readable(locals + off, sizeof(FString)) &&
                read_fstring((const FString*)(locals + off), s, sizeof s) && s[0] && (any_string || is_movie_name(s))) {
                strncpy_s(out, out_sz, s, _TRUNCATE);
                return 1;
            }
            prop = *(void**)((uint8_t*)prop + UFIELD_NEXT);
        }
    }

    // A literal in the call: PlaySpecificLoadingMovie("TP_UnloadScreen.bik")
    // is EX_StringConst (0x1F) and the text; PlayMovie(false, "x.bik") has
    // the bool (EX_True 0x27 / EX_False 0x28) before it. Read, not run.
    if (code && readable(code, 2)) {
        uint8_t* p = code;
        if (p[0] == 0x27 || p[0] == 0x28) p++;
        if (p[0] == 0x1F && readable(p + 1, 128)) {
            char s[128];
            size_t i = 0;
            for (; i + 1 < sizeof s && p[1 + i] >= 0x20 && p[1 + i] < 0x7F; i++) s[i] = (char)p[1 + i];
            s[i] = 0;
            if (p[1 + i] == 0 && is_movie_name(s)) {
                strncpy_s(out, out_sz, s, _TRUNCATE);
                return 1;
            }
        }
        logf_("subtitles: no movie name in the call (code %02x %02x)\n", code[0], code[1]);
    }
    return 0;
}
