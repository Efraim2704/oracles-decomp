#pragma once
// Assets v1: the ROM image minus its code bytes. tools/codemap.py lists, per game, the byte
// ranges that are instructions of hooked routines; the native runtime builds its cycle table
// from the original bytes and then zeroes those ranges, so nothing it does afterwards can depend
// on the ROM's code. The map can also be installed on a GB as a bitmap: bus_read then aborts on
// a data read of a mapped byte, which is how the ranges to keep (src/assets/keep_code.txt) are
// found.
#include "core/gb.h"
#include <stddef.h>

typedef struct { uint32_t off, len; } CodeRange;
extern const CodeRange codemap_ages[], codemap_seasons[], codemap_ages_eu[], codemap_seasons_eu[];
extern const size_t codemap_ages_n, codemap_seasons_n, codemap_ages_eu_n, codemap_seasons_eu_n;

bool assets_is_seasons(const uint8_t *rom, size_t size);
// The European release (2 MB, five languages): the header's game code ends in 'P' (AZ8P, AZ7P)
// where the USA one has 'E'.
bool assets_is_eu(const uint8_t *rom, size_t size);
// 1 bit per ROM byte, set for mapped code; malloc'd.
uint8_t *assets_code_bits(const uint8_t *rom, size_t size);
// Zeroes the mapped bytes in place; returns how many.
size_t assets_zero_code(uint8_t *rom, size_t size);
