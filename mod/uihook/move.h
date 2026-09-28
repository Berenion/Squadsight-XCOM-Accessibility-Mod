#pragma once
#include <windows.h>

// A move the numpad confirmed, watched until the soldier stands still
// (move.c): the path the game was given, logged point by point, and where
// the soldier ended up against where the path ended. A soldier who stops
// short is said aloud, since nothing in the game says it.

// Numpad 0 is about to send its click: the soldier being moved and the path
// their pathing pawn holds now, which is the one the click performs.
// (tx, ty) and `floor` are the numpad's target (tx -1 when there is none).
// 0 when the click is to be held back: the path stops short of the target,
// which has been said; the same confirm again within a few seconds goes.
int move_confirmed(int tx, int ty, float floor);

// Every frame: follows the soldier until the move is over.
void move_poll(void);

// The last point of the path a pathing pawn holds -- where a move along it
// would end, which is not always where it was asked to go. 0 when unreadable.
int path_end(void* ppawn, float* end);

// How far a path's end may stand from the height asked for and still be that
// floor: half a storey (192) either way. A path's points stand a few units
// under floor + NAVH_LIFT (58 over a floor at 2.0 in the 18:58 log).
#define PATH_END_Z_SLACK 96.0f
