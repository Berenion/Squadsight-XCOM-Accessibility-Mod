#pragma once
#include <stddef.h>

// A cutscene's subtitles, said as the game shows them. See subtitles.c.

// How a line is said: `say` is main.c's announce_as(SET_SUBTITLES, ...), so
// the option, the quiet spell and the Insert history all apply; `say_always`
// is its announce, for the intro's opening quote, which is said with the
// option off too.
typedef void (*SubtitleSayFn)(const char* utf8);

// The thread that keeps time with the movie. FALSE when it could not start.
int  subtitles_start(SubtitleSayFn say, SubtitleSayFn say_always, char* why, size_t why_sz);

// The movie's name as the script passed it to XComEngine.PlayMovie or
// PlaySpecificLoadingMovie ("XEW_CIN_Intro.bik"), read from the calling
// frame -- the caller's string local, or the literal in its bytecode -- and
// never by stepping the code. 0 when there is none. `any_string` takes the
// frame's first string as it is, extension or not: for
// GamePlayerController.ClientPlayMovie, whose frame is its own parameters
// (MovieName first) when the engine raises it -- Kismet's Control Game Movie
// passes "SR_hover_02_LOC" with no ".bik".
int  subtitles_movie_name(void* stack, int any_string, char* out, size_t out_sz);

// A movie has started / is over (finished, skipped, or stopped by the game).
// Both cheap: the files are read on the timing thread.
void subtitles_movie_started(const char* movie, const char* how);
void subtitles_movie_ended(const char* how);

// A voice-only narrative conversation has started playing on
// `audio_component` (what BeginVOOnlyConversation's CreateAudioComponent
// returned): its lines are read from the cue's waves now, on the game thread,
// and said as they come. Stopped when that component is stopped or faded out.
void subtitles_voice_started(void* audio_component, const char* how);
void subtitles_voice_stopped(void* audio_component, const char* how);

// A tentpole's conversation component (StartTentpoleRemoteEvent's
// CreateAudioComponent): its lines read and logged, and said by their
// durations if the game does not step them (subtitles.c says why).
void subtitles_tentpole_started(void* audio_component);

// A narrative line, as XComConversationNode.GetCurrentDialogue gave it: said
// unless subtitles_dialogue_shown reports the same text drawn on a comm link
// or narrative popup within half a second.
void subtitles_dialogue(const char* text);

// Whether a movie with subtitles is playing (until its last line's end).
int  subtitles_movie_busy(void);

// `text` said through `say` once the movies are over: after a second, if no
// movie with lines has started, at once; otherwise when they have ended. In
// order, after any held before it. For EU's first briefing, drawn before the
// intro plays over it.
void subtitles_say_after_movies(const char* text, SubtitleSayFn say);
void subtitles_dialogue_shown(const char* text);

// For the offline check: the timed lines of `movie` under `root` (the folder
// with XComGame in it) in `lang`, with the English for a line the language
// lacks. The count; at most `max`.
typedef struct {
    int  start_ms, end_ms;
    char key[64];
    char text[1024];
    int  found;             // the key was in the file, even as a blank line
    int  always;            // said whatever the option: the intro's quote
} SubtitleCue;
int  subtitles_load_from(const char* root, const char* lang, const char* movie,
                         SubtitleCue* out, int max, char* file, size_t file_sz);
