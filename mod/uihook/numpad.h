#pragma once
#include <windows.h>
#include "ue3.h"

// Numpad navigation's game side (numpad.c): the target the numpad moves and
// the hooks that make the game path to it, the step readout, gliding, F / C
// and Home's nav_focus, aiming from the numpad, the blast list, and the
// per-frame poll that reads the mod's keys in a mission. The rules it
// follows are nav.c's, where the offline checks reach them.

// Aiming a rocket or grenade rather than choosing a move (see numpad.c).
extern int g_nav_aim;

// Whether a targeting action's shot is used on the soldier taking it
// (eTarget_Self, and not aimed at the ground): nothing to aim, Enter uses it.
int  shot_used_on_self(void* action);

// Whether the selected soldier is flying (XGUnit.m_bIsFlying, Toggle Flight).
int  soldier_flying(void);

// "Hovering, 3 storeys up." when the flying soldier's pawn at `loc` is in
// the air; 0 (and nothing written) otherwise.
int  fly_hover_words(const float* loc, char* out, size_t out_sz);

// Lets go of the numpad's target; `why` is for the log.
void nav_stop(const char* why);

// Whether the game's window is in front.
int  game_has_focus(void);

// The natives navigation hooks, swapped in by pointer (main.c, "the hooks,
// by pointer"), and the originals they call on to.
void __fastcall hook_worlddata(void* self, void* edx, void* stack, void* result);
void __fastcall hook_cursormode(void* self, void* edx, void* stack, void* result);
void __fastcall hook_floorz(void* self, void* edx, void* stack, void* result);
void __fastcall hook_computepath(void* self, void* edx, void* stack, void* result);
void __fastcall hook_jetpackpath(void* self, void* edx, void* stack, void* result);
void __fastcall hook_flashhit(void* self, void* edx, void* stack, void* result);
void __fastcall hook_moviecheck(void* self, void* edx, void* stack, void* result);
void __fastcall hook_getengine(void* self, void* edx, void* stack, void* result);
void __fastcall hook_worldinfo(void* self, void* edx, void* stack, void* result);
void __fastcall hook_chained(void* self, void* edx, void* stack, void* result);
void __fastcall hook_validpos(void* self, void* edx, void* stack, void* result);
extern ExecFn g_orig_worlddata, g_orig_cursormode, g_orig_floorz, g_orig_computepath,
              g_orig_flashhit, g_orig_moviecheck, g_orig_getengine, g_orig_worldinfo,
              g_orig_chained, g_orig_validpos, g_orig_jetpackpath;

// ---- provided by main.c --------------------------------------------------------
// The per-frame work of the other readouts, which nav_poll runs in a mission.
void combat_poll(void);
int  info_poll(void);
void info_settle(void);
void mission_poll(void);
int  mission_visible(void);
int  msum_screen_up(void);
void sight_poll(void);
void soldier_poll(void);
void soldier_readout(void);
void weapon_x_pressed(void);
extern int g_weapon_x_down;

extern volatile LONG g_calls;           // the capture's call counter, for log lines
extern int  g_speak;                    // speech on at all
extern char g_reticle_said[128];        // the aiming reticle's last message
