#pragma once
// The characters of the rooms beside the screen, for widescreen's strips. Objects only exist in the
// room Link is in, so a copy of the game goes into each neighbouring overworld room (the game's own
// screen transition, so the room spawns what it would for Link) and its sprites are kept, frozen as
// they stood when the copy got there.
#include "core/gb.h"
#include "wide/wide.h"

// Main thread, once per frame after the game's: takes a finished room and, when the game is at a
// point a copy can start from, prepares the next one. True when a copy now waits for
// wide_npcs_work.
bool wide_npcs_update(GB *real);
// Runs the waiting copy into its room, on a thread of its own (always the same one) so the game
// never waits for it. Does nothing when no copy waits.
void wide_npcs_work(void);
// Forget every room (a state was loaded, the game changed).
void wide_npcs_reset(void);
// Before the game's ROM goes away: drops a copy that has not started and waits for a running one.
void wide_npcs_stop(void);
// For WideOptions.room_sprites (main thread).
const WideRoomSprites *wide_npcs_room(int group, int room);
