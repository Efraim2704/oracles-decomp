#pragma once
#include "core/gb.h"

void offscreen_sprites_capture(GB *gb);
void offscreen_sprites_commit(GB *gb);
// assignRandomPositionToEnemy placed the enemy on this page
void offscreen_random_placed(GB *gb, uint8_t page);
