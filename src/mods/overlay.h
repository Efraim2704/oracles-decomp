#pragma once
// What a mod draws, before the picture goes through the filters. Sized to the picture rather than
// reusing UiCanvas, which is fixed at 160 wide: in widescreen a mod must be able to draw in the
// side strips too. Coverage is per pixel so the game shows through wherever the mod drew nothing.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// UiFont is `typedef struct { ... } UiFont`, an anonymous struct with no tag, so a `struct UiFont;`
// forward declaration would name an unrelated incomplete type and make every real UiFont* argument
// an incompatible-pointer-types warning at the call site. oraclesmods already links oraclesui (for
// the font and item-icon decoder), so there is no dependency to hide by forward-declaring here.
#include "ui/ui.h"

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
// Draws with the UI font's glyphs: ink is a clear bit, so only the ink is covered.
void overlay_text(ModOverlay *o, const UiFont *font, int x, int y, const char *s, int colour);
// An item's own icon from the ROM, via wide_item_icon. Colour index 0 is left clear, as the status
// bar draws these as sprites. Returns false when the item has no icon.
bool overlay_icon(ModOverlay *o, const uint8_t *rom, size_t rom_size, bool seasons, uint8_t item,
                  const uint8_t *c600, int x, int y);
// Reads, for tests and for compositing. Out-of-range coordinates are reported as uncovered/black
// rather than read, since x and y may come from mod-controlled Lua in later tasks.
uint8_t overlay_cover_at(const ModOverlay *o, int x, int y);
const uint8_t *overlay_rgb_at(const ModOverlay *o, int x, int y);
