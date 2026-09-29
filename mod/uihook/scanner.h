#pragma once

// The scanner's game side (scanner.c): its keys -- Page Up / Page Down (Ctrl
// for the category, Alt for the storey), Home to put the target on the
// selection, End for how far it is -- and where its lists come from: the
// units, the target strip, the level actors, the tutorial's point, the evac
// zone and the ways up. The list itself and its words are scan.c's.

// Reads the keys; every frame from nav_poll, while the game has the
// foreground and practice does not have the keys.
void scan_poll(void);

// Forgets which keys were down, so one released elsewhere is not a press.
void scan_keys_forget(void);

// A terror mission's civilian counter (UITerrorInfo) has drawn: the panel,
// whose m_civilians gives the count listed under Objectives.
void scanner_terror_panel(void* panel);

// Provided by main.c, with the target strip's capture (strip_note): the
// enemies the strip draws, in its order; their count, 0 when unreadable.
int strip_enemies(void* const** out);

// Provided by main.c (navigation): puts the numpad target on (tx, ty) with
// its floor at `ground`, as a step would, so the tile describes itself.
// `what` is for the log.
void nav_focus(int tx, int ty, float ground, const char* what);
