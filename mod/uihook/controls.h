#pragma once
#include <windows.h>

// The game's Controls screen (UIKeybindingsPCScreen), from the keyboard.
//
// The screen is the mouse's: OnUnrealCommand has a case for Enter (save) and
// Escape (cancel) and nothing else, and every other action -- choosing a slot
// to rebind, clearing one, switching tabs, resetting -- is a click handled in
// OnMouseEvent. So the arrows walk the screen's own m_arrBindings here, and a
// key that stands for a click calls OnMouseEvent with the arguments the click
// would have sent (through UObject::ProcessEvent). The binding itself is the
// game's: once a slot is waiting, RawInputHandler takes the next key.

// From rewrite_cmd, for every key on the screen (`screen` its object name):
// 1 when the key was the mod's and must not reach the screen.
int controls_key(LONG n, void* object, const char* screen, int cmd, int mask, int have_mask);

// From capture_body, for every ActionScript call the screen makes: what it
// draws is said here, not by the general path. Returns 1 when handled.
int controls_capture(LONG n, void* object, const char* obj_name, const char* fn_name);

// Whether the Controls screen was the last to take a key or draw, for the key
// list (Ctrl + /).
int controls_up(void);

// screen.OnMouseEvent(cmd, args) through UObject::ProcessEvent: what Flash
// sends for a mouse event on any screen. Runs the screen's script on the
// calling thread, so only from a hook the game's thread is in. 0 when the
// screen has no OnMouseEvent to call.
int screen_mouse_event(void* screen, int cmd, const char* const* args, int nargs);

// obj.<name>(arg) through UObject::ProcessEvent, for a script function whose
// first parameter is an int. The same rules: the game's thread only. 0 when
// `obj` has no such function.
int object_call_int(void* obj, const char* name, int arg);
