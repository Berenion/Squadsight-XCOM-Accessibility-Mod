// Offline checks for the focus table, the dialogue box and the speech
// debounce.
//
// These encode the rules the live behaviour depends on:
//   - a list published once can be indexed later, per object
//   - a modal prompt is spoken whole, once, when its last call arrives
//   - a lone line is spoken, but a line followed by more is not
//
// None of it needs the game, so none of it should first be exercised inside
// it.

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "focus.h"
#include "dialog.h"
#include "help.h"
#include "shot.h"
#include "props.h"
#include "input.h"
#include "speech.h"
#include "cursor.h"
#include "nav.h"
#include "tile.h"

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

    // A settings widget names itself and states its value in two calls that
    // carry the same index. Filing both as labels let the value overwrite the
    // name, so the options screen said "Fullscreen" without saying of what.
    printf("\nlabel and value share a slot\n");
    void* opts = (void*)0xE000;
    focus_begin(opts);
    focus_set_part(opts, 0, FOCUS_PART_LABEL, "Mode:");
    focus_set_part(opts, 0, FOCUS_PART_VALUE, "Fullscreen");
    check(focus_label_at(opts, 0, buf, sizeof buf) &&
          strcmp(buf, "Mode: Fullscreen") == 0, "label and value joined");

    // Order must not matter: the value can arrive first on a refresh.
    focus_set_part(opts, 1, FOCUS_PART_VALUE, "1920 x 1080");
    focus_set_part(opts, 1, FOCUS_PART_LABEL, "Resolution:");
    check(focus_label_at(opts, 1, buf, sizeof buf) &&
          strcmp(buf, "Resolution: 1920 x 1080") == 0, "value may arrive first");

    // Changing the value leaves the name alone -- the whole point of parts.
    focus_set_part(opts, 0, FOCUS_PART_VALUE, "Windowed");
    check(focus_label_at(opts, 0, buf, sizeof buf) &&
          strcmp(buf, "Mode: Windowed") == 0, "new value keeps the label");

    // A slider names itself and states its position the same way, so it must
    // read as one control rather than as a label with a number after it:
    // SetSliderLabel(3, "Music volume:") then SetSliderValue(3, 49).
    focus_set_part(opts, 4, FOCUS_PART_LABEL, "Music volume:");
    check(focus_set_part(opts, 4, FOCUS_PART_VALUE, "39 percent") == 0,
          "a slider's first position is not a change");
    check(focus_label_at(opts, 4, buf, sizeof buf) &&
          strcmp(buf, "Music volume: 39 percent") == 0, "slider reads with its name");
    check(focus_set_part(opts, 4, FOCUS_PART_VALUE, "49 percent") == 1,
          "moving the slider is a change");
    check(focus_label_at(opts, 4, buf, sizeof buf) &&
          strcmp(buf, "Music volume: 49 percent") == 0, "the new position reads");

    // A control with only one half still reads (buttons, sliders).
    focus_set_part(opts, 2, FOCUS_PART_LABEL, "Gamma:");
    check(focus_label_at(opts, 2, buf, sizeof buf) &&
          strcmp(buf, "Gamma:") == 0, "label alone, no trailing space");
    focus_set_part(opts, 3, FOCUS_PART_VALUE, "On");
    check(focus_label_at(opts, 3, buf, sizeof buf) &&
          strcmp(buf, "On") == 0, "value alone");

    // A reclaimed slot must not leak the previous occupant's value under a
    // new object's label -- "Shadows: 1920 x 1080".
    focus_begin(opts);
    focus_set_part(opts, 0, FOCUS_PART_LABEL, "Shadows:");
    check(focus_label_at(opts, 0, buf, sizeof buf) &&
          strcmp(buf, "Shadows:") == 0, "republish drops the old value");

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

    // props.c decides a candidate BitMask offset is real only when the value
    // is a single bit, and decides a candidate UObject::Class offset is real
    // only when every field resolves to a name ending "Property". Both tests
    // run against live memory, but the predicates themselves are pure and are
    // what a wrong offset has to get past, so they are checked here.
    // A container widget's items are its own, not the screen's. Flattening
    // them into the slot is what made EU's difficulty read as one run-on
    // string, so they are kept per (object, slot).
    printf("\ncontainer widget items\n");
    void* diff = (void*)0xF000;
    focus_options_begin(diff, 0);
    focus_options_add(diff, 0, "Easy");
    focus_options_add(diff, 0, "Normal");
    focus_options_add(diff, 0, "Classic");
    focus_options_add(diff, 0, "Impossible");
    check(focus_option_at(diff, 0, 0, buf, sizeof buf) &&
          strcmp(buf, "Easy") == 0, "item 0 resolves");
    check(focus_option_at(diff, 0, 3, buf, sizeof buf) &&
          strcmp(buf, "Impossible") == 0, "item 3 resolves");
    check(!focus_option_at(diff, 0, 4, buf, sizeof buf), "item past end rejected");
    check(!focus_option_at(diff, 1, 0, buf, sizeof buf), "other slot has no items");

    // The slot table must be untouched by all of that: the list is one widget,
    // and its label belongs to the widget, not to its contents.
    focus_set_part(diff, 1, FOCUS_PART_LABEL, "Enable Ironman?");
    check(focus_label_at(diff, 1, buf, sizeof buf) &&
          strcmp(buf, "Enable Ironman?") == 0, "slots unaffected by items");

    // The screen announcing the choice is not always the object holding the
    // list, so the most recent list resolves a bare index.
    check(focus_recent_option_at(1, buf, sizeof buf) &&
          strcmp(buf, "Normal") == 0, "recent list resolves a marker");

    // Republishing a list replaces it rather than appending.
    focus_options_begin(diff, 0);
    focus_options_add(diff, 0, "Rookie");
    check(focus_option_at(diff, 0, 0, buf, sizeof buf) &&
          strcmp(buf, "Rookie") == 0, "republished list replaced");
    check(!focus_option_at(diff, 0, 1, buf, sizeof buf), "old items gone");

    // Telling a real change from a screen redrawing itself: only the former
    // should ever be spoken, or every refresh would talk.
    printf("\nvalue change detection\n");
    void* cb = (void*)0xF100;
    focus_set_part(cb, 0, FOCUS_PART_LABEL, "Enable Ironman?");
    check(focus_set_part(cb, 0, FOCUS_PART_VALUE, "unchecked") == 0,
          "first value is not a change");
    check(focus_set_part(cb, 0, FOCUS_PART_VALUE, "unchecked") == 0,
          "same value again is not a change");
    check(focus_set_part(cb, 0, FOCUS_PART_VALUE, "checked") == 1,
          "different value is a change");
    // The join adds a space and nothing else -- the game's own labels carry
    // their punctuation ("Show action cam:" does, "Enable Ironman?" does not),
    // so inventing a separator would double it on half the screens.
    check(focus_label_at(cb, 0, buf, sizeof buf) &&
          strcmp(buf, "Enable Ironman? checked") == 0,
          "changed value reads with its name");

    // Giving the keyboard the gamepad-only actions. The table is pure policy,
    // so what it does and -- more importantly -- what it leaves alone can be
    // pinned down without the game.
    printf("\nkey remap\n");
    check(input_remap("UIShellDifficulty_0", 612) == 302,
          "1 opens advanced options");
    check(input_remap("UIShellDifficulty_0", 613) == 303,
          "2 opens Second Wave");
    // EW confirms the difficulty on Start alone; Enter and Space belong to
    // the checkbox under the cursor, so the screen had no keyboard way out.
    check(input_remap("UIShellDifficulty_0", 614) == 321,
          "3 starts the game");
    check(input_remap("UIOptionsPCScreen_0", 614) == 0,
          "3 is free elsewhere");
    check(input_remap("UIOptionsPCScreen_0", 571) == 331, "Tab is next tab");
    check(input_remap("UIOptionsPCScreen_0", 612) == 330, "1 is previous tab");
    // Escape only ever discards: saving lives on X and nowhere else.
    check(input_remap("UIOptionsPCScreen_0", 613) == 302, "2 saves and exits");
    check(input_remap("UIShellDifficulty_0", 613) == 303,
          "2 still opens Second Wave where it already did");

    // The screen name arrives with the game's instance suffix, and matching
    // has to survive it.
    check(input_remap("UIShellDifficulty_12", 612) == 302, "suffix ignored");

    // Letter keys are bound in [XComGame.XComTacticalInput], which is only
    // live during a mission, so in a menu they never become UI commands at
    // all. Mapping one is dead code -- twice already: Q/E, then X/Y. Only
    // keys bound in [Engine.PlayerInput] can appear on the left below.
    check(input_remap("UIShellDifficulty_0", 531) == 0, "Q cannot arrive in a menu");
    check(input_remap("UIShellDifficulty_0", 519) == 0, "E cannot arrive in a menu");
    check(input_remap("UIShellDifficulty_0", 538) == 0, "X cannot arrive in a menu");
    check(input_remap("UIShellDifficulty_0", 539) == 0, "Y cannot arrive in a menu");

    // Nothing outside the listed screens may be touched.
    check(input_remap("UITacticalHUD_0", 612) == 0, "tactical 1 untouched");
    check(input_remap("UIFinalShell_0", 571) == 0, "main menu Tab untouched");
    check(input_remap("UILoadGame_0", 571) == 0, "load game Tab untouched");

    // Keys the listed screens already use must keep their meaning.
    check(input_remap("UIShellDifficulty_0", 511) == 0, "Enter untouched");
    check(input_remap("UIShellDifficulty_0", 510) == 0, "Escape untouched");
    check(input_remap("UIShellDifficulty_0", 500) == 0, "arrow up untouched");
    check(input_remap("UIShellDifficulty_0", 502) == 0, "arrow down untouched");
    check(input_remap("UIShellDifficulty_0", 537) == 0, "W untouched, it is up");
    check(input_remap("UIOptionsPCScreen_0", 513) == 0, "space untouched");

    // A name that merely starts the same way must not match a longer one.
    check(input_remap("UIShell", 612) == 0, "partial name rejected");
    check(input_remap(NULL, 612) == 0, "null screen rejected");

    printf("\nproperty probe predicates\n");
    {
        // A mask selects one bit. Zero means "no mask found", and several bits
        // means the offset is pointing at something that is not a mask.
        check(is_single_bit(0x00000001), "mask 1 accepted");
        check(is_single_bit(0x00000080), "mask 0x80 accepted");
        check(is_single_bit(0x80000000), "mask 0x80000000 accepted");
        check(!is_single_bit(0), "zero rejected");
        check(!is_single_bit(3), "two bits rejected");
        check(!is_single_bit(0xFFFFFFFF), "all bits rejected");

        check(ends_with_property("IntProperty"), "IntProperty accepted");
        check(ends_with_property("BoolProperty"), "BoolProperty accepted");
        check(!ends_with_property("Property"), "bare Property rejected");
        check(!ends_with_property("UIWidgetHelper"), "class name rejected");
        check(!ends_with_property(""), "empty rejected");
        check(!ends_with_property("Propert"), "near miss rejected");
    }

    // The options screen's exit prompt: five calls, none of which says
    // anything on its own, and which cancelled one another on the general
    // text path. This is the sequence UIDialogueBox.Realize sends, with the
    // strings the log recorded live.
    printf("\ndialogue box\n");
    {
        void* box = (void*)0xD000;
        char say[DIALOG_MAX_TEXT];

        dialog_reset();
        check(dialog_is_box("UIDialogueBox_0"), "instance name recognised");
        check(!dialog_is_box("UIProgressDialogue_0"), "progress dialogue is not it");
        check(!dialog_is_box("UIOptionsPCScreen_0"), "options screen is not it");
        check(!dialog_is_box(NULL), "null name rejected");

        check(dialog_note(box, "AS_SetStyleNormal", -1, "", say, sizeof say)
              == DIALOG_SILENT, "style starts the box");
        // The title arrives empty: IgnoreChangesAndExit sets no strTitle, and
        // the struct default "<DEFAULT TITLE>" is stripped as markup long
        // before it gets here.
        check(dialog_note(box, "AS_SetTitle", -1, "", say, sizeof say)
              == DIALOG_SILENT, "an empty title says nothing yet");
        check(dialog_note(box, "AS_SetText", -1,
                          "Do you want to exit and discard your changes?",
                          say, sizeof say) == DIALOG_SILENT,
              "the question alone is not announced");
        check(dialog_note(box, "AS_SetHelp", 0, "EXIT WITHOUT CHANGES",
                          say, sizeof say) == DIALOG_SILENT,
              "the first answer waits for the second");
        check(dialog_note(box, "AS_SetHelp", 1, "BACK TO OPTIONS",
                          say, sizeof say) == DIALOG_SPEAK,
              "the last call announces the box");
        check(strcmp(say, "Do you want to exit and discard your changes? "
                          "Enter: EXIT WITHOUT CHANGES. "
                          "Escape: BACK TO OPTIONS") == 0,
              "question and both answers, with the keys");

        // RefreshNavigationHelp is also a watch-variable callback on
        // m_bMouseIsActive, so the help pair re-arrives whenever the mouse
        // wakes up. Nothing has changed, so nothing may be said.
        dialog_note(box, "AS_SetHelp", 0, "EXIT WITHOUT CHANGES", say, sizeof say);
        check(dialog_note(box, "AS_SetHelp", 1, "BACK TO OPTIONS",
                          say, sizeof say) == DIALOG_SILENT,
              "a help refresh with no change is silent");

        // Raising the same prompt again must announce it again -- the style
        // call is what says a box is being drawn.
        dialog_note(box, "AS_SetStyleNormal", -1, "", say, sizeof say);
        dialog_note(box, "AS_SetTitle", -1, "", say, sizeof say);
        dialog_note(box, "AS_SetText", -1,
                    "Do you want to exit and discard your changes?",
                    say, sizeof say);
        dialog_note(box, "AS_SetHelp", 0, "EXIT WITHOUT CHANGES", say, sizeof say);
        check(dialog_note(box, "AS_SetHelp", 1, "BACK TO OPTIONS",
                          say, sizeof say) == DIALOG_SPEAK,
              "the same prompt raised again is announced again");

        // The video prompt: a warning style, a title, and a body that
        // UIOptionsPCScreen.KeepResolutionCountdown replaces once a second
        // through UpdateDialogText -- which sends the body and nothing else.
        dialog_reset();
        dialog_note(box, "AS_SetStyleWarning", -1, "", say, sizeof say);
        dialog_note(box, "AS_SetTitle", -1, "KEEP SETTINGS?", say, sizeof say);
        dialog_note(box, "AS_SetText", -1,
                    "Do you wish to keep the current settings? Settings will "
                    "automatically revert in 15 seconds.", say, sizeof say);
        dialog_note(box, "AS_SetHelp", 0, "KEEP SETTINGS", say, sizeof say);
        check(dialog_note(box, "AS_SetHelp", 1, "CANCEL", say, sizeof say)
              == DIALOG_SPEAK, "the warning box is announced");
        check(strcmp(say, "Warning. KEEP SETTINGS? Do you wish to keep the "
                          "current settings? Settings will automatically "
                          "revert in 15 seconds. Enter: KEEP SETTINGS. "
                          "Escape: CANCEL") == 0,
              "severity, title, question and answers in one sentence");
        check(dialog_note(box, "AS_SetText", -1,
                          "Do you wish to keep the current settings? Settings "
                          "will automatically revert in 14 seconds.",
                          say, sizeof say) == DIALOG_UPDATE,
              "a later body change reports itself");
        check(strstr(say, "14 seconds") && !strstr(say, "Enter:"),
              "the countdown repeats the line that moved, not the whole box");

        // A box with only one answer still has to be announced: waiting for a
        // cancel label that never comes would lose it entirely.
        dialog_reset();
        dialog_note(box, "AS_SetStyleNormal", -1, "", say, sizeof say);
        dialog_note(box, "AS_SetText", -1, "Saving your options failed.",
                    say, sizeof say);
        dialog_note(box, "AS_SetHelp", 0, "ACCEPT", say, sizeof say);
        check(dialog_note(box, "AS_SetHelp", 1, "", say, sizeof say)
              == DIALOG_SPEAK, "an empty cancel still ends the box");
        check(strcmp(say, "Saving your options failed. Enter: ACCEPT") == 0,
              "an answer that does not exist is not offered");

        // Everything else the box does keeps to the general path.
        check(dialog_note(box, "Show", -1, "", say, sizeof say) == DIALOG_IGNORED,
              "Show is not one of the setters");
        check(dialog_note(box, "AS_SetImage", -1, "img:///UILibrary.Alien",
                          say, sizeof say) == DIALOG_IGNORED,
              "the image is not spoken");

        // The join must not run past its buffer, whatever the game hands it.
        // speech_say drops an utterance too long for its queue rather than
        // shortening it, so an over-long box has to be cut here -- at a word,
        // and keeping the front of the question, which is the part that says
        // what is being asked.
        dialog_reset();
        {
            char long_body[DIALOG_MAX_TEXT * 2];
            for (size_t k = 0; k < sizeof long_body - 1; k++)
                long_body[k] = (k % 5 == 4) ? ' ' : 'x';
            long_body[sizeof long_body - 1] = 0;
            dialog_note(box, "AS_SetStyleAlert", -1, "", say, sizeof say);
            dialog_note(box, "AS_SetText", -1, long_body, say, sizeof say);
            dialog_note(box, "AS_SetHelp", 0, "ACCEPT", say, sizeof say);
            dialog_note(box, "AS_SetHelp", 1, "CANCEL", say, sizeof say);
            check(strlen(say) < DIALOG_MAX_TEXT, "the announcement stays in bounds");
            check(strncmp(say, "Alert. xxxx xxxx", 16) == 0,
                  "severity and the start of the question survive");
            check(say[strlen(say) - 1] != ' ', "no trailing space at the cut");

            // A body with no word boundary at all is dropped rather than cut
            // mid-word: half a word tells a listener nothing.
            char unbroken[DIALOG_MAX_TEXT * 2];
            memset(unbroken, 'x', sizeof unbroken - 1);
            unbroken[sizeof unbroken - 1] = 0;
            dialog_reset();
            dialog_note(box, "AS_SetStyleNormal", -1, "", say, sizeof say);
            dialog_note(box, "AS_SetText", -1, unbroken, say, sizeof say);
            dialog_note(box, "AS_SetHelp", 0, "ACCEPT", say, sizeof say);
            check(dialog_note(box, "AS_SetHelp", 1, "CANCEL", say, sizeof say)
                  == DIALOG_SPEAK, "the answers are still announced");
            check(strcmp(say, "Enter: ACCEPT. Escape: CANCEL") == 0,
                  "an uncuttable body is dropped, not spliced");
        }
    }

    // The help bar, read back on request. The difficulty screen is the case
    // that prompted it: two bars, and the one thing the keyboard could not
    // reach sitting in the second entry of the first.
    printf("\nhelp bar\n");
    {
        void* bar1 = (void*)0xB000;   // UINavigationHelp_0
        void* bar2 = (void*)0xB100;   // UINavigationHelp_1
        char say[512];

        help_reset();
        check(help_icon_cmd("Icon_START") == 321, "Start glyph resolves");
        check(help_icon_cmd("Icon_Y_TRIANGLE") == 303, "Y glyph resolves");
        check(help_icon_cmd("Icon_B_CIRCLE") == 301, "B glyph resolves");
        check(help_icon_cmd("Icon_DPAD") == 0, "movement glyphs are not commands");
        check(help_icon_cmd("img:///UILibrary.Thing") == 0, "an image is not a glyph");
        check(help_icon_cmd("") == 0, "empty glyph rejected");

        // A screen that has published nothing yet still has whatever keys the
        // mod adds to it -- those do not depend on the bar.
        check(help_announce("UIShellDifficulty_0", say, sizeof say) == 3,
              "with no bar, the added keys still stand");
        check(help_announce("UINothingHere_0", say, sizeof say) == 0,
              "a screen with neither claims nothing");
        check(strstr(say, "lists no commands") != NULL, "and says so out loud");

        // Exactly what UpdateButtonHelp and UpdateButtonHelp2 send.
        help_set(bar1, 0, "SECOND WAVE", "Icon_Y_TRIANGLE", 0);
        help_set(bar1, 1, "START GAME", "Icon_START", 0);
        help_set(bar2, 0, "BACK", "Icon_B_CIRCLE", 0);

        // Three advertised, plus the one the mod adds that the screen says
        // nothing about: UpdateButtonHelp never mentions the advanced options
        // behind X, so without that last entry the list is confidently
        // incomplete.
        check(help_announce("UIShellDifficulty_0", say, sizeof say) == 4,
              "three advertised commands and one added");
        check(strcmp(say, "SECOND WAVE: 2. START GAME: 3. BACK: Escape. "
                          "Advanced options: 1") == 0,
              "each command with the key that reaches it");
        check(strstr(say, "Second Wave: 2") == NULL,
              "an added key the bar already named is not repeated");

        // The same screen with no remap in the table: the gap has to be
        // audible, because that is the whole purpose of the list.
        check(help_announce("UISomethingNew_0", say, sizeof say) == 3,
              "an unmapped screen still lists its commands");
        check(strstr(say, "START GAME: no key") != NULL,
              "an unreachable command says so");
        check(strstr(say, "BACK: Escape") != NULL,
              "B is Escape on any screen");

        // A bar rebuilding itself replaces its entries rather than adding to
        // them -- UpdateButtonHelp opens with ClearButtonHelp.
        help_clear(bar1);
        help_set(bar1, 0, "BACK", "Icon_B_CIRCLE", 0);
        check(help_announce("UIShellDifficulty_0", say, sizeof say) == 5,
              "a cleared bar drops its old entries");
        check(strstr(say, "START GAME") == NULL, "the old entry is gone");
        // With Start no longer advertised, the key that reaches it has to be
        // offered on its own account.
        check(strstr(say, "Start the game: 3") != NULL,
              "an added key appears once the bar stops naming it");

        // An empty label is how a screen empties one slot without clearing
        // the bar: UIOptionsPCScreen sends AS_SetHelp(1, "", "") when the
        // credits link does not apply.
        help_reset();
        void* opts = (void*)0xB200;
        help_set(opts, 0, "BACK", "Icon_B_CIRCLE", 0);
        help_set(opts, 1, "CREDITS", "Icon_RT_R2", 0);
        help_set(opts, 2, "RESET ALL SETTINGS", "Icon_Y_TRIANGLE", 0);
        help_set(opts, 3, "SAVE CHANGES AND EXIT", "Icon_X_SQUARE", 0);
        // Four advertised, plus the two tabs -- which the screen publishes
        // through AS_SetTabHelp, carrying no glyph, so they never reach the
        // bar at all.
        check(help_announce("UIOptionsPCScreen_0", say, sizeof say) == 6,
              "the options screen lists four and both tabs");
        check(strstr(say, "SAVE CHANGES AND EXIT: 2") != NULL,
              "the key we added is reported");
        check(strstr(say, "RESET ALL SETTINGS: no key") != NULL,
              "the gap we have not filled is reported");
        check(strstr(say, "Next tab: Tab") && strstr(say, "Previous tab: 1"),
              "the tabs are listed although the bar never mentions them");

        help_set(opts, 1, "", "", 0);
        check(help_announce("UIOptionsPCScreen_0", say, sizeof say) == 5,
              "an emptied slot drops out");
        check(strstr(say, "CREDITS") == NULL, "and takes its label with it");

        // A disabled action is still listed -- knowing it is there and greyed
        // out is different from not knowing it exists.
        help_set(opts, 1, "CREDITS", "Icon_RT_R2", 1);
        help_announce("UIOptionsPCScreen_0", say, sizeof say);
        check(strstr(say, "CREDITS: no key, unavailable") != NULL,
              "a disabled action says so");

        // Bounds: a screen cannot talk the buffer off its end.
        help_reset();
        void* many = (void*)0xB300;
        for (int k = 0; k < HELP_MAX_ENTRIES; k++)
            help_set(many, k, "A COMMAND WITH A FAIRLY LONG LABEL ON IT",
                     "Icon_START", 0);
        // NB: not `small` -- the Windows RPC headers define that as char.
        char narrow[64];
        help_announce("UIShellDifficulty_0", narrow, sizeof narrow);
        check(strlen(narrow) < sizeof narrow, "the announcement stays in bounds");
    }

    // The menu over that list. Its whole trick is that firing is the same
    // rewrite the table does, with the target chosen at runtime -- so what
    // these check is that the right command comes back out, and that keys
    // meant for the menu never reach the screen.
    printf("\nhelp menu\n");
    {
        void* bar = (void*)0xC000;
        char say[512];
        int fire;

        help_reset();
        help_menu_close();
        help_set(bar, 0, "SECOND WAVE", "Icon_Y_TRIANGLE", 0);
        help_set(bar, 1, "START GAME", "Icon_START", 0);
        help_set(bar, 2, "BACK", "Icon_B_CIRCLE", 0);

        check(!help_menu_is_open(), "starts closed");
        check(help_menu_key("UIShellDifficulty_0", 511, &fire, say, sizeof say)
              == HELP_MENU_PASS, "a closed menu takes nothing");

        check(help_menu_open("UIShellDifficulty_0", say, sizeof say) == 4,
              "opening reads the whole list");
        check(help_menu_is_open(), "and stays open");

        // Down moves on and says where it is.
        check(help_menu_key("UIShellDifficulty_0", 502, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "down is the menu's, not the screen's");
        check(strcmp(say, "START GAME: 3. 2 of 4") == 0, "it says which and where");

        // Any axis moves: the screen never sees these, so which one it would
        // have used does not matter.
        check(help_menu_key("UIShellDifficulty_0", 501, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "right moves too");
        check(strstr(say, "3 of 4") != NULL, "onwards");
        check(help_menu_key("UIShellDifficulty_0", 500, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "up goes back");
        check(strstr(say, "2 of 4") != NULL, "backwards");

        // Firing hands the screen the command the entry names.
        fire = 0;
        check(help_menu_key("UIShellDifficulty_0", 511, &fire, say, sizeof say)
              == HELP_MENU_FIRE, "Enter fires");
        check(fire == 321, "and fires Start, which no key reaches");
        check(!help_menu_is_open(), "firing closes the menu");

        // Wrapping, both ways.
        help_menu_open("UIShellDifficulty_0", say, sizeof say);
        help_menu_key("UIShellDifficulty_0", 500, &fire, say, sizeof say);
        check(strstr(say, "4 of 4") != NULL, "up from the first wraps to the last");
        help_menu_key("UIShellDifficulty_0", 502, &fire, say, sizeof say);
        check(strstr(say, "1 of 4") != NULL, "and down wraps back");

        // An entry the mod added, with no glyph of its own, fires the command
        // the game put behind a gamepad button.
        help_menu_key("UIShellDifficulty_0", 500, &fire, say, sizeof say);
        check(strstr(say, "Advanced options: 1") != NULL, "the added key is in the list");
        fire = 0;
        check(help_menu_key("UIShellDifficulty_0", 300, &fire, say, sizeof say)
              == HELP_MENU_FIRE && fire == 302, "and fires X");

        // Escape closes, and says so: silence would leave the player unsure
        // whether they were still in a mode.
        help_menu_open("UIShellDifficulty_0", say, sizeof say);
        check(help_menu_key("UIShellDifficulty_0", 510, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "Escape closes");
        check(strcmp(say, "Menu closed") == 0, "out loud");
        check(!help_menu_is_open(), "and it really is closed");

        // The key that opened it closes it again.
        help_menu_open("UIShellDifficulty_0", say, sizeof say);
        help_menu_key("UIShellDifficulty_0", 621, &fire, say, sizeof say);
        check(!help_menu_is_open(), "the same key toggles it shut");

        // Everything else is swallowed while it is open. A key reaching the
        // screen underneath would act unseen.
        help_menu_open("UIShellDifficulty_0", say, sizeof say);
        check(help_menu_key("UIShellDifficulty_0", 571, &fire, say, sizeof say)
              == HELP_MENU_QUIET, "Tab does not reach the screen");
        check(help_menu_is_open(), "and the menu is still up");

        // A menu left open on a screen the player has left is stale: it must
        // not swallow the new screen's keys.
        check(help_menu_key("UIFinalShell_0", 502, &fire, say, sizeof say)
              == HELP_MENU_PASS, "another screen's key passes through");
        check(!help_menu_is_open(), "and the stale menu closes itself");

        // A disabled entry is named, not fired.
        help_reset();
        void* bar2 = (void*)0xC100;
        help_set(bar2, 0, "CREDITS", "Icon_RT_R2", 1);
        help_menu_open("UINowhere_0", say, sizeof say);
        fire = 99;
        check(help_menu_key("UINowhere_0", 511, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "a disabled entry does not fire");
        check(fire == 99 && strstr(say, "unavailable") != NULL, "it says why");

        // Nor does one whose glyph names no command at all.
        help_reset();
        help_set(bar2, 0, "MOVE", "Icon_DPAD", 0);
        help_menu_open("UINowhere_0", say, sizeof say);
        fire = 99;
        check(help_menu_key("UINowhere_0", 511, &fire, say, sizeof say)
              == HELP_MENU_SPEAK, "an entry with no command does not fire");
        check(fire == 99 && strstr(say, "cannot be pressed") != NULL, "it says so");

        // A screen offering nothing must not leave a mode behind.
        help_reset();
        check(help_menu_open("UINothingAtAll_0", say, sizeof say) == 0,
              "nothing to show");
        check(!help_menu_is_open(), "so nothing is opened");
    }

    // The tactical shot readout. UITacticalHUD_InfoPanel.Update sends this
    // burst on every change of ability or target and always ends it with
    // UpdateLayout, so that is where it is spoken.
    printf("\nshot readout\n");
    {
        char say[SHOT_MAX_TEXT];

        shot_reset();
        check(shot_is_panel("UITacticalHUD_InfoPanel_0"), "the info panel is known");
        check(!shot_is_panel("UITacticalHUD_AbilityContainer_0"), "its neighbour is not");
        check(!shot_is_panel(NULL), "null rejected");

        check(shot_note("SetIsAvailable", "", "", 1, say, sizeof say) == 0,
              "availability alone says nothing");
        check(shot_note("SetShotName", "Standard Shot", "", -1, say, sizeof say) == 0,
              "the name alone says nothing");
        check(shot_note("SetShotChance", "to hit", "73%", -1, say, sizeof say) == 0,
              "the chance alone says nothing");
        check(shot_note("SetCriticalChance", "critical", "20%", -1, say, sizeof say) == 0,
              "nor the critical chance");
        check(shot_note("SetWeaponStats", "Assault Rifle", "", -1, say, sizeof say) == 0,
              "nor the weapon");
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1,
              "the end of the burst speaks");
        // The number first: the panel puts "to hit" beside a big box because
        // of how it is drawn, but spoken that way it reads backwards.
        check(strcmp(say, "Standard Shot. 73% to hit. 20% critical. "
                          "Assault Rifle") == 0,
              "name, chance, critical, and the weapon the first time");

        // Update runs whenever the targeting state is touched. The same shot
        // again must not repeat itself.
        shot_note("SetIsAvailable", "", "", 1, say, sizeof say);
        shot_note("SetShotName", "Standard Shot", "", -1, say, sizeof say);
        shot_note("SetShotChance", "to hit", "73%", -1, say, sizeof say);
        shot_note("SetCriticalChance", "critical", "20%", -1, say, sizeof say);
        shot_note("SetWeaponStats", "Assault Rifle", "", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 0,
              "the same shot is not announced twice");

        // Cycling to another target changes the odds and nothing else.
        shot_note("SetShotChance", "to hit", "45%", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1,
              "a new chance is news");
        check(strcmp(say, "Standard Shot. 45% to hit. 20% critical") == 0,
              "and the weapon, already said, is left out");

        // A zero crit chance is noise; any chance at all is not.
        shot_note("SetCriticalChance", "critical", "0%", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1 &&
              strcmp(say, "Standard Shot. 45% to hit") == 0,
              "0% critical is left out");
        shot_note("SetCriticalChance", "critical", "0.0 %", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 0,
              "any spelling of zero is still zero");
        shot_note("SetCriticalChance", "critical", "10%", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1 &&
              strcmp(say, "Standard Shot. 45% to hit. 10% critical") == 0,
              "a chance ending in zero is not zero");
        shot_note("SetShotChance", "to hit", "0%", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1 &&
              strstr(say, "0% to hit") != NULL,
              "0% to hit is still said -- it is the one warning that matters");

        // Switching to an ability with a different weapon says so.
        shot_note("SetShotName", "Rocket", "", -1, say, sizeof say);
        shot_note("SetWeaponStats", "Rocket Launcher", "", -1, say, sizeof say);
        shot_note("UpdateLayout", "", "", -1, say, sizeof say);
        check(strstr(say, "Rocket Launcher") != NULL, "a new weapon is news again");

        // A shot with no percentage sends SetShotChance("", ""), which
        // reaches the hook as a call with no text at all.
        shot_reset();
        shot_note("SetIsAvailable", "", "", 1, say, sizeof say);
        shot_note("SetShotName", "Hunker Down", "", -1, say, sizeof say);
        shot_note("SetShotChance", "", "", -1, say, sizeof say);
        shot_note("SetCriticalChance", "", "", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1,
              "a shot with no odds still announces");
        check(strcmp(say, "Hunker Down") == 0, "and says only its name");

        // Unavailable is the one thing a player must not discover by
        // pressing fire.
        shot_reset();
        shot_note("SetIsAvailable", "", "", 0, say, sizeof say);
        shot_note("SetShotName", "", "", -1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 1,
              "an unavailable shot announces");
        check(strcmp(say, "Unavailable") == 0, "plainly");

        // Nothing at all to say: the panel is being cleared.
        shot_reset();
        shot_note("SetIsAvailable", "", "", 1, say, sizeof say);
        check(shot_note("UpdateLayout", "", "", -1, say, sizeof say) == 0,
              "an empty panel stays quiet");

        // Bounds, with a name far longer than any the game has.
        shot_reset();
        {
            char long_name[SHOT_MAX_TEXT * 2];
            memset(long_name, 'x', sizeof long_name - 1);
            long_name[sizeof long_name - 1] = 0;
            shot_note("SetIsAvailable", "", "", 1, say, sizeof say);
            shot_note("SetShotName", long_name, "", -1, say, sizeof say);
            shot_note("SetShotChance", "to hit", "73%", -1, say, sizeof say);
            shot_note("UpdateLayout", "", "", -1, say, sizeof say);
            check(strlen(say) < SHOT_MAX_TEXT, "the announcement stays in bounds");
        }
    }

    printf("\ncursor tile arithmetic\n");
    {
        // A C cast truncates toward zero; the native floors.  They part on
        // every negative coordinate, which is what the first mission hit.
        check(cursor_tile_axis(-913.0f, 0.0f, CURSOR_TILE) == -10,
              "a negative position floors rather than truncates");
        check(cursor_tile_axis(-1.0f, 0.0f, CURSOR_TILE) == -1,
              "just below the origin is tile -1, not 0");
        check(cursor_tile_axis(0.0f, 0.0f, CURSOR_TILE) == 0,
              "the origin itself is tile 0");

        // The origin moves the lattice.  Positions from the mission run, with
        // a Min that puts them mid-tile: adjacent readings step by one.
        float min = -4801.0f;
        check(cursor_tile_axis(-1009.0f, min, CURSOR_TILE) == 39 &&
              cursor_tile_axis(-913.0f, min, CURSOR_TILE) == 40 &&
              cursor_tile_axis(-817.0f, min, CURSOR_TILE) == 41,
              "adjacent positions are adjacent tiles");
        check(cursor_tile_axis(min + 95.9f, min, CURSOR_TILE) == 0 &&
              cursor_tile_axis(min + 96.0f, min, CURSOR_TILE) == 1,
              "a tile boundary is measured from Min");
    }

    printf("\nnumpad navigation\n");
    {
        int dx, dy;
        check(nav_step_for_digit(8, &dx, &dy) && dx == 0 && dy == 1, "8 is north, +Y");
        check(nav_step_for_digit(2, &dx, &dy) && dx == 0 && dy == -1, "2 is south");
        check(nav_step_for_digit(6, &dx, &dy) && dx == 1 && dy == 0, "6 is east, +X");
        check(nav_step_for_digit(4, &dx, &dy) && dx == -1 && dy == 0, "4 is west");
        check(nav_step_for_digit(9, &dx, &dy) && dx == 1 && dy == 1, "9 is north-east");
        check(nav_step_for_digit(1, &dx, &dy) && dx == -1 && dy == -1, "1 is south-west");
        check(!nav_step_for_digit(5, &dx, &dy) && !nav_step_for_digit(0, &dx, &dy),
              "5 and 0 are not directions");
        check(!nav_step_for_digit(10, &dx, &dy) && !nav_step_for_digit(-1, &dx, &dy),
              "out-of-range digits are refused");

        NavGrid g = { 74, 61 };
        char say[NAV_MAX_TEXT];
        int tx, ty;

        nav_end();
        check(!nav_active() && !nav_move(&g, 1, 0, say, sizeof say),
              "nothing moves before navigation begins");

        nav_begin(45, 16);
        check(nav_move(&g, 0, 1, say, sizeof say) == 1 && strcmp(say, "45, 17") == 0,
              "a step north says the new tile");
        check(nav_target(&tx, &ty) && tx == 45 && ty == 17, "and holds it as the target");

        nav_begin(73, 30);
        check(nav_move(&g, 1, 0, say, sizeof say) == 0 && strcmp(say, "Edge") == 0,
              "the east edge stops a step and says so");
        check(nav_target(&tx, &ty) && tx == 73 && ty == 30, "without moving the target");
        check(nav_move(&g, 1, 1, say, sizeof say) == 1 && strcmp(say, "73, 31") == 0,
              "a diagonal into the edge slides along it");

        nav_begin(0, 0);
        check(nav_move(&g, -1, -1, say, sizeof say) == 0, "the corner stops a diagonal");

        nav_end();
        check(!nav_target(&tx, &ty), "no target once navigation ends");
    }

    printf("\nfinding the floor under a tile\n");
    {
        // Level ground: the first search start finds the floor, and a path
        // built there decides the tile at once.
        navh_set_ground(-37.9f);
        navh_begin_tile();
        float z = navh_query_z();
        check(z > -37.9f, "the first search starts above the ground");
        navh_floor_result(z, -37.9f);
        check(navh_phase() == NAVH_SETTLED && navh_query_z() == -37.9f,
              "a found floor settles the height");
        check(navh_path_result(-37.9f + NAVH_LIFT, 1, 1000) == NAVH_REACHABLE,
              "a path built on it is reachable");
        check(navh_path_result(-37.9f + NAVH_LIFT, 0, 1010) == NAVH_WAIT,
              "and one verdict per tile");

        // The ground carries over to the next tile.
        navh_begin_tile();
        check(navh_query_z() > -37.9f && navh_ground() == -37.9f,
              "the next tile searches from the ground just found");

        // The bottom layer: no start ever finds a floor, so the probe takes
        // over and the pathfinder decides the height.
        navh_set_ground(-146.8f);
        navh_begin_tile();
        int quiet = 1;
        for (int i = 0; i < 64 && navh_phase() == NAVH_SEARCH; i++) {
            z = navh_query_z();
            if (navh_path_result(z + NAVH_LIFT, 0, 2000 + i) != NAVH_WAIT) quiet = 0;
            navh_floor_result(z, z);        // nothing found: our own height back
        }
        check(quiet, "failed paths during the search are not verdicts");
        check(navh_phase() == NAVH_PROBE, "a search that never finds a floor turns to probing");
        z = navh_query_z();
        check(z == -146.8f, "the first probe is the estimate itself");
        check(navh_path_result(z + NAVH_LIFT, 0, 3000) == NAVH_WAIT,
              "a failed probe moves on");
        float z2 = navh_query_z();
        check(z2 != z, "to another height");
        check(navh_path_result(z + NAVH_LIFT, 0, 3010) == NAVH_WAIT && navh_query_z() == z2,
              "a late answer for the previous height does not skip one");
        check(navh_path_result(z2 + NAVH_LIFT, 1, 3020) == NAVH_REACHABLE,
              "a probe the game builds a path to is reachable");
        check(navh_phase() == NAVH_SETTLED && navh_ground() == z2,
              "and becomes the ground");

        // Every probe fails: say so, once.
        navh_set_ground(500.0f);
        navh_begin_tile();
        for (int i = 0; i < 64 && navh_phase() == NAVH_SEARCH; i++) {
            z = navh_query_z();
            navh_floor_result(z, z);
        }
        NavVerdict v = NAVH_WAIT;
        for (int i = 0; i < 64 && v == NAVH_WAIT; i++)
            v = navh_path_result(navh_query_z() + NAVH_LIFT, 0, 4000 + i);
        check(v == NAVH_NO_PATH && navh_phase() == NAVH_NONE,
              "every probe failing is No path");

        // A floor that is found but cannot be reached: No path after the
        // settle time, and not before; a success in between cancels it.
        navh_set_ground(0.0f);
        navh_begin_tile();
        z = navh_query_z();
        navh_floor_result(z, 0.0f);
        check(navh_path_result(0.0f + NAVH_LIFT, 0, 5000) == NAVH_WAIT,
              "a failure on a found floor waits");
        check(navh_poll(5000 + NAVH_SETTLE_MS - 1) == NAVH_WAIT, "until the settle time");
        check(navh_poll(5000 + NAVH_SETTLE_MS + 1) == NAVH_NO_PATH, "then is No path");
        check(navh_poll(6000) == NAVH_WAIT, "once");

        navh_begin_tile();
        z = navh_query_z();
        navh_floor_result(z, 0.0f);
        navh_path_result(0.0f + NAVH_LIFT, 0, 7000);
        check(navh_path_result(0.0f + NAVH_LIFT, 1, 7100) == NAVH_REACHABLE &&
              navh_poll(8000) == NAVH_WAIT,
              "a success before the settle time cancels the failure");

        // While still searching, a failure at an unsettled height is not a verdict.
        navh_set_ground(0.0f);
        navh_begin_tile();
        z = navh_query_z();
        check(navh_path_result(z + NAVH_LIFT, 0, 9000) == NAVH_WAIT &&
              navh_poll(9000 + 10 * NAVH_SETTLE_MS) == NAVH_WAIT,
              "failures during the search say nothing");
    }

    printf("\nwhat is on a tile\n");
    {
        // A thunk's shape, written by hand: the out-parameter notify calls
        // through a register too, but without loading `this` first.
        static const unsigned char thunk[] = {
            0x8B, 0x80, 0xD0, 0x00, 0x00, 0x00,     // mov eax, [eax+0xD0]
            0x52,                                   // push edx
            0xFF, 0xD0,                             // call eax     (notify)
            0x8B, 0x92, 0xF4, 0x01, 0x00, 0x00,     // mov edx, [edx+0x1F4]
            0x55,                                   // push ebp
            0x8B, 0xCB,                             // mov ecx, ebx
            0xFF, 0xD2,                             // call edx     (the method)
            0xC2, 0x08, 0x00,                       // ret 8
            0x8B, 0x91, 0x00, 0x02, 0x00, 0x00,     // past the end: ignored
            0x8B, 0xCB, 0xFF, 0xD1,
        };
        check(tile_vtable_slot(thunk, sizeof thunk) == 0x1F4,
              "the slot is the call that has `this` loaded");
        check(tile_vtable_slot(thunk, 9) == -1, "a notify alone is not a slot");
        static const unsigned char other_reg[] = {
            0x8B, 0x92, 0xEC, 0x01, 0x00, 0x00,     // mov edx, [edx+0x1EC]
            0x8B, 0xCB, 0xFF, 0xD0,                 // call eax: not the loaded register
            0xC2, 0x08, 0x00,
        };
        check(tile_vtable_slot(other_reg, sizeof other_reg) == -1,
              "a call through another register does not count");

        TileReport r;
        char say[TILE_MAX_TEXT];

        memset(&r, 0, sizeof r);
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "No cover.") == 0, "nothing: no cover");

        // The game's North is +Y, the numpad's too; its East is -X, which the
        // numpad calls west.
        r.cover_flags = TILE_COVER_N;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "High cover north.") == 0, "a direction bit alone is high cover");
        r.cover_flags = TILE_COVER_E | TILE_COVER_ELOW;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "Low cover west.") == 0, "the game's East is -X, spoken west");
        r.cover_flags = TILE_COVER_W;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "High cover east.") == 0, "and its West is east");

        r.cover_flags = TILE_COVER_N | TILE_COVER_S | TILE_COVER_W |
                        TILE_COVER_E | TILE_COVER_ELOW;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "High cover north, east and south. Low cover west.") == 0,
              "high then low, each clockwise from north");

        r.cover_flags = TILE_COVER_DIAGONAL | TILE_COVER_N | TILE_COVER_W | TILE_COVER_WLOW;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "High cover northwest. Low cover northeast.") == 0,
              "diagonal cover is turned 45 degrees");

        r.cover_flags = 0;
        r.dash = 1;
        r.smoke = 1;
        r.poison = 1;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "Dash. No cover. Smoke. Poison.") == 0,
              "dash first, hazards last");

        char tiny[8];
        tile_describe(&r, tiny, sizeof tiny);
        check(strlen(tiny) < sizeof tiny, "a short buffer truncates");

        r.turns = 3;
        r.smoke = r.poison = 0;
        tile_describe(&r, say, sizeof say);
        check(strcmp(say, "3 turns. No cover.") == 0, "a move past this turn says its turns");

        // Costs from the second mission run: standard move 12, dash 24.
        check(tile_turns(10, 24, 12) == 1, "within this turn's reach is one turn");
        check(tile_turns(24, 24, 12) == 1, "the dash limit itself is still one turn");
        check(tile_turns(25, 24, 12) == 2, "one past it is two");
        check(tile_turns(54, 24, 12) == 3, "54 is 24 + 24 + 6: three turns");
        check(tile_turns(30, 12, 12) == 2,
              "after moving once, this turn has only the standard move left");
        check(tile_turns(-1, 24, 12) == 0 && tile_turns(5, 0, 12) == 0,
              "nonsense numbers give no answer");

        // A thunk that takes no arguments keeps `this` in esi.
        static const unsigned char no_args[] = {
            0x8B, 0x06,                             // mov eax, [esi]
            0x8B, 0x90, 0xC8, 0x04, 0x00, 0x00,     // mov edx, [eax+0x4C8]
            0x8B, 0xCE,                             // mov ecx, esi
            0xFF, 0xD2,                             // call edx
            0xC2, 0x08, 0x00,
        };
        check(tile_vtable_slot(no_args, sizeof no_args) == 0x4C8,
              "`this` from esi counts too");

        char where[64];
        tile_offset_text(3, -2, where, sizeof where);
        check(strcmp(where, "2 south, 3 east") == 0, "an offset says north-south first");
        tile_offset_text(0, 5, where, sizeof where);
        check(strcmp(where, "5 north") == 0, "a straight offset has one part");
        tile_offset_text(0, 0, where, sizeof where);
        check(strcmp(where, "here") == 0, "no offset is here");

        TileContact c[3] = {
            { "Zombie", 0, -8 }, { "Chryssalid", 5, 2 }, { "Sectoid", -1, 1 },
        };
        char list[256];
        tile_contacts(c, 3, "none", list, sizeof list);
        check(strcmp(list, "Sectoid, 1 north, 1 west. Chryssalid, 2 north, 5 east. "
                           "Zombie, 8 south.") == 0,
              "contacts nearest first");
        tile_contacts(c, 0, "No enemies in sight.", list, sizeof list);
        check(strcmp(list, "No enemies in sight.") == 0, "an empty list says so");
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
