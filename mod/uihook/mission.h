#pragma once
#include <stddef.h>

// The mission's objectives, as the HUD lists them top left.
//
// UITacticalHUD_ObjectivesList draws them, and every change reaches Flash
// through one of its AS_ calls:
//
//   AS_AddObjective(Id, Title, Description, showCheckmark, queuePos)
//       a new objective, or new text for one already there (a counter, the
//       turns left); showCheckmark false is a hint, drawn with no box
//   AS_CompleteObjective(Id) / AS_FailObjective(Id) / AS_RemoveObjective(Id)
//   RemoveAllObjectives()           -- Invoke("clear")
//   AS_SetSortedList(array<string>) -- the order drawn, which is followed
//                                      (open above complete, in the logs)
//
// Calls come in bursts: at mission start the list is cleared and rebuilt,
// and objectives already met are completed straight after. So changes are
// collected and compared, once the burst is over, with the list as it stood
// before it. A list replaced outright is read whole ("Objectives: ...");
// otherwise each difference is an event ("Objective complete: ..."). Removals
// are silent: the game removes to re-add.

#define MISSION_MAX  24
#define MISSION_TEXT 1024

void mission_reset(void);

void mission_clear(void);
void mission_add(const char* id, const char* title, const char* desc, int checkmark);
void mission_complete(const char* id);
void mission_fail(const char* id);
void mission_remove(const char* id);
void mission_order(const char* const* ids, int n);

// Whether anything has changed since the last mission_changes.
int mission_dirty(void);

// What changed since the last call, as one announcement, and the current
// list becomes the one the next call compares with. Empty when nothing a
// player would notice changed.
void mission_changes(char* out, size_t out_sz);

// The whole list, in the order drawn, each with its state after it:
// "Objectives: Find the source of the infestation. Investigate the response
// team's disappearance, complete."
void mission_list(char* out, size_t out_sz);
