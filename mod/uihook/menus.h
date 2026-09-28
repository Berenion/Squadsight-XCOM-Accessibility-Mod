#pragma once
#include <stdint.h>

// The mod's own menus in a mission (menus.c): the ability menu on numpad .,
// and the announcement list on Insert (history.h). While either is open --
// and for a moment after it closes -- the arrows, Enter and Escape are kept
// from the game (hook_moviecheck, rewrite_cmd); abar.c holds the menu's list
// and words.

// Every frame in a mission: the ability menu's keys. 1 while it is open.
int  abar_menu_poll(void);

// Whether InputEvent(cmd, mask) is the menus' and must not reach the game.
int  abar_menu_swallow(int cmd, int mask);

// Whether Enter is owed a rewrite into an ability's number key, and the
// rewrite itself, on InputEvent's frame (the GetEngine hook).
int  abar_pick_pending(void);
void abar_menu_pick(void* stack);

// The Insert list's keys, from review_pump on any screen. 1 while it is open.
int  review_poll(void);
void review_end(const char* why);

// Whether a menu has just closed and its keys are still owed to it.
int  menu_grace(void);
// A menu opened from elsewhere (Delete's pages): its keys are being read now.
void menu_polled(void);

// ---- provided by main.c --------------------------------------------------------
void*    abar_container(void);          // UITacticalHUD_AbilityContainer, as captured
void     strip_markup(char* s);         // Flash markup out of a string, in place
int      input_event_cmd(void* stack, int* mask_out);   // InputEvent's Cmd, or -1
int32_t* input_event_cmd_slot(void* stack);             // where to rewrite it
