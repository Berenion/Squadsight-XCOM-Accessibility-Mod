#pragma once

// The mouse, kept still (SET_MOUSE).
//
// Nothing this mod does wants the physical mouse, and a mouse that drifts
// does harm: in a mission the cursor follows it every frame
// (Mouse_CheckForPathing), so a nudge releases numpad navigation, moves the
// tile the wall field listens from, and over the HUD stops the game pathing at
// all; at a screen edge Mouse_CheckForWindowScroll pans the camera; in a menu
// a hover moves the selection.
//
// A low-level mouse hook (WH_MOUSE_LL), on a thread of its own with the
// message loop such a hook needs, swallows every movement, button and wheel
// event that comes from a device while the game's window is in front. Input
// sent by a program -- the mod's own right click for numpad 0, NVDA's mouse
// keys -- is flagged injected and passes, and SetCursorPos never reaches the
// hook at all. With the game in the background the mouse is left alone.
//
// Swallowing is decided on every event from the setting, so the options menu
// switches it without touching this thread.

#include <stddef.h>

// Starts the hook's thread. Says in `why` what happened; never fatal.
int mouse_start(char* why, size_t why_sz);

// Whether events are being swallowed right now: the setting is on, the hook
// is in, and the game's window is in front.
int mouse_blocking(void);

// How many device events have been swallowed so far, for the log.
unsigned mouse_swallowed(void);
