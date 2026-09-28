#pragma once

// What the player hears of the map around them, from the tile they are
// listening from (the numpad's target, the aim, or the selected soldier):
// the wall field (sonar.h) and the heartbeats of allies and seen enemies,
// with door and window sounds (heart.h). The game is read here; the mixer is
// audio.c. Both polls run every frame from nav_poll and hand the mixer what
// they last read, since it lets anything unrenewed lapse.

void walls_poll(void);
void hearts_poll(void);

// Silences the field now (a menu or the background has the player).
void walls_quiet(void);

// Scans the walls afresh on the next poll, wherever the tile is.
void walls_rescan(void);

// The soldier "Follow one soldier" (SET_HEART_SOLO) hears: the scanner's
// label for them, as picked there.
void hearts_follow(const char* name);

// The perf line: frames, and the worst frame's walls and hearts, in
// QueryPerformanceCounter ticks. Logged every 5 s.
void perf_note(long long walls_ticks, long long hearts_ticks);
