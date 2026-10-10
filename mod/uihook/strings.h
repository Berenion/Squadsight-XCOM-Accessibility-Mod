#pragma once
#include <stddef.h>

// Every line the mod writes itself, as opposed to text the game drew, comes
// from here, so that the mod can be translated without touching the code.
//
// The English is compiled in (strings.def, one entry per line). A translation
// is lang\<CODE>.txt beside the DLL, where CODE is the game's own language --
// INT, DEU, FRA, ITA, ESN, POL, RUS, JPN, KOR, CHT, the folder names under
// XComGame\Localization -- so the mod follows whatever language the player
// set the game to. A line missing from the file, or one whose placeholders
// do not match the English, stays English (and the log says which).
//
// The file, UTF-8:
//
//     # comment
//     LEARN_SILENT = "Stumm."
//     SPIN_POS     = "%2$d von %3$d: %1$s"
//
// A format line may reorder its placeholders with %n$ (the CRT's positional
// printf), and must keep the English line's types in order; it may leave out
// trailing ones, never one in the middle.
//
// This file knows nothing of the game, so the offline checks can link it.

typedef enum {
#define S(key, text) key,
#define F(key, text) key,
#define P(key, one, other) key,
#include "strings.def"
#undef S
#undef F
#undef P
    STR_COUNT
} StrId;

// The line in the current language. Never NULL. Not for a P line (tpfmt).
const char* T(StrId id);

// A line that changes with a count (P in strings.def): the form the language
// uses for `n`, filled in with the arguments after `n` (which usually repeat
// it). English has two forms, one and other; a translation gives as many as
// its language has, separated by |:
//     DEU, FRA, ITA, ESN   one|other          (FRA: 0 and 1 are "one")
//     RUS, POL             one|few|many       (21 is "one" in RUS, not POL)
//     JPN, KOR, CHT        other
// A form may leave out trailing placeholders ("one turn" with no %d).
int tpfmt(char* out, size_t out_sz, StrId id, int n, ...);
int tpfmt_cat(char* out, size_t out_sz, size_t* len, StrId id, int n, ...);

// A format line, filled in: printf into `out`, cut to fit (at a whole UTF-8
// character) rather than failing. Returns the length written.
int tfmt(char* out, size_t out_sz, StrId id, ...);

// The same, appended at `*len` (which it advances); for lines built in parts.
int tfmt_cat(char* out, size_t out_sz, size_t* len, StrId id, ...);

// Loads lang\<code>.txt from `dll_dir` (ending in a separator) over the
// English. "INT" or an empty code is English and reads nothing. Safe to call
// again with another code: the table is swapped whole. `why` says what came
// of it, for the log.
void strings_load(const char* dll_dir, const char* code, char* why, size_t why_sz);

// The code last loaded ("INT" before any).
const char* strings_lang(void);

// Whether the lines are English: the language is INT or has no file. For the
// few places that shape a game word the English way (a plural made with -s).
int strings_is_english(void);

// Names the mod makes up rather than keeps as lines -- an object named from
// its mesh ("Wooden crate stack") -- have no key; a translation file can
// still give them, one per line, as the English with an @ in front:
//     @Wooden crate stack = "Holzkistenstapel"
// The translation of `english` (any case), or `english` itself.
const char* strings_phrase(const char* english);

// For translators: writes every line, key = "English", in the file format
// above. Returns 0 if the file could not be written.
int strings_write_template(const char* path);

// Whether a translated format line may stand in for the English: the same
// placeholders, of the same types, none in the middle skipped (printf reads
// only the arguments a format names, so trailing ones may go unused). 0 with
// the reason otherwise. Exposed for the offline check.
int strings_formats_match(const char* english, const char* other, char* why, size_t why_sz);

// ---- text the game drew ----------------------------------------------------
//
// The game's own words arrive in the player's language, so anything that
// reshapes them must not assume English or ASCII: case is changed through
// Windows (CharUpperBuffW / CharLowerBuffW, which know Cyrillic, Polish and
// German letters), and a sentence's end may be a CJK full stop.

// Case-insensitive search in UTF-8; the match in `hay`, or NULL.
const char* text_find_ci(const char* hay, const char* needle);

// Whole-string, case-insensitive equality in UTF-8.
int text_equal_ci(const char* a, const char* b);

// "ALIEN ACTIVITY" -> "Alien activity": a screen reader spells out capitals.
void text_sentence_case(const char* in, char* out, size_t out_sz);

// "HUNKER DOWN" -> "Hunker Down".
void text_title_case(const char* in, char* out, size_t out_sz);

// The whole string in lower case.
void text_lower(const char* in, char* out, size_t out_sz);

// Whether `s` has a letter in lower case (so is not all capitals).
int text_has_lower(const char* s);

// Whether `s` already ends a sentence (. ! ? or their CJK forms).
int text_ends_sentence(const char* s);

// Appends the language's full stop (TXT_STOP) unless `s` already ends one.
void text_end_sentence(char* s, size_t s_sz);

// ---- the game's language ---------------------------------------------------
//
// UObject.GetLanguage (native static final function string GetLanguage()):
// its exec thunk calls a getter that returns the address of a static TCHAR
// buffer the engine fills at startup (EW 0x5426E0 -> 0xBB8270, EU 0x50FCA0 ->
// 0xA5EB90, both `mov eax, imm32; ret`). The buffer's address is read out of
// that code, not written down, and the buffer is read directly, so nothing
// is called on the game and it can be asked from any thread.
//
// The address of the buffer, from the exec thunk's code; NULL when the code
// is not the expected shape.
const unsigned short* strings_language_buffer(const unsigned char* exec);
