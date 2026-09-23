#pragma once

// The mod's options menu, and sound practice inside it.
//
// Numpad / opens and closes the menu, anywhere: the shell, the base or a
// mission. It is a list of the settings in settings.h, then Sound practice:
//
//   8 2         up and down the list (it wraps; each entry says "n of 10")
//   4 6         change the entry: a switch flips, a scale steps and stops
//   5           open Sound practice; on a switch, flip it
//   /           close
//
// Each change is saved at once (settings.h). The wall level is also set from
// practice, below, and the two are the same setting.
//
// Sound practice: the wall field, one direction at a time, on demand.
//
// The field says four things at once and says them quietly, which is the right
// way for it to behave in a mission and the wrong way to learn it. Nobody can
// be asked to work out what "north" sounds like from a room that also has a
// wall to the east and a crate behind them, while a Sectoid is moving. So the
// four can be played on their own, by name, as loudly and as long as the player
// likes, with nothing else sounding.
//
// Opened with 5 on its menu entry. While it is on:
//
//   8 2 4 6     one side on its own: north, south, west, east
//   7 9 1 3     two at once, the corners: NW, NE, SW, SE
//   5           all four -- what being boxed in sounds like
//   0           silence, to hear the room without it
//   + -         the wall further off and nearer, nought to six tiles
//   * .         the whole field louder and quieter, and this one sticks:
//               it is the level the game will use afterwards
//   /           back to the menu
//
// Every one of them is spoken as it is chosen, so the sound and its name
// arrive together. That is the whole design: the player is not being asked to
// remember a key chart, they are being told what they are hearing.
//
// The level belongs here for a reason. It is the one setting that cannot be
// chosen in advance, because what it has to compete with is whatever the game is
// playing -- rain, wind, gunfire -- and practice does not silence any of that.
// So it is set where it can be heard against the real thing, as well as from
// the menu's list.
//
// ---- why this runs on its own thread ---------------------------------------
//
// Everything else the numpad does is polled from inside the battle cursor's own
// per-frame native, which is exactly right for it: those keys move a cursor, so
// they should be live precisely while there is a cursor to move. Practice is
// the opposite. It touches nothing in the game -- it only builds a field and
// speaks -- and it is most wanted in the shell, before a mission, where that
// native never runs at all. So it polls from a thread of its own and works
// everywhere, including at the main menu.
//
// That thread and the game thread both read the numpad, so only one of them may
// act on it at a time: while the menu or practice is open, the navigation poll
// stands aside (it checks learn_active and returns), and while both are closed
// this thread watches nothing but numpad /.
//
// Numpad / is safe to take. Like Multiply, DefaultInput.ini mentions Divide
// only in the alias lists of edit boxes and numeric edit boxes, so the game
// binds it to nothing -- except while a text field has focus, which is to say
// while naming a soldier, where the game will also type it.

#include <stddef.h>

// Starts the poll thread. Call after the mixer and speech are up, since the
// first thing practice does on being switched on is use both. Returns 1 on
// success; `why` receives a line for the log either way. Practice not starting
// costs the mod nothing else.
int learn_start(char* why, size_t why_sz);

// Whether the menu or practice has the numpad. The navigation poll must check this and do
// nothing while it is set, or a key press would both move the cursor and change
// the demonstration.
int learn_active(void);

// Stops the thread. The mod does not call this -- the DLL lives as long as the
// game does -- but the offline test does.
void learn_stop(void);
