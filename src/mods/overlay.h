#pragma once
// What a mod draws, before the picture goes through the filters. Sized to the picture rather than
// reusing UiCanvas, which is fixed at 160 wide: in widescreen a mod must be able to draw in the
// side strips too. Coverage is per pixel so the game shows through wherever the mod drew nothing.
#include <stdbool.h>
#include <stdint.h>

#define MOD_OVERLAY_H 144
#define MOD_OVERLAY_MAX_W 256

typedef struct {
  int width;                                           // 160, or 256 in widescreen
  uint8_t rgb[MOD_OVERLAY_H][MOD_OVERLAY_MAX_W][3];
  uint8_t cover[MOD_OVERLAY_H][MOD_OVERLAY_MAX_W];     // 0 clear, 255 drawn
} ModOverlay;

// Clears the overlay and sets the picture width for this frame.
void overlay_begin(ModOverlay *o, int width);
// Colours are RGB555, as the renderer uses.
void overlay_rect(ModOverlay *o, int x, int y, int w, int h, int colour);
// Reads, for tests and for compositing. Out-of-range coordinates are reported as uncovered/black
// rather than read, since x and y may come from mod-controlled Lua in later tasks.
uint8_t overlay_cover_at(const ModOverlay *o, int x, int y);
const uint8_t *overlay_rgb_at(const ModOverlay *o, int x, int y);
