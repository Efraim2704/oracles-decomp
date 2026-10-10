#include "mods/overlay.h"
#include "ui/ui.h"
#include "wide/item.h"
#include <string.h>

void overlay_begin(ModOverlay *o, int width) {
  o->width = width < 0 ? 0 : width > MOD_OVERLAY_MAX_W ? MOD_OVERLAY_MAX_W : width;
  memset(o->cover, 0, sizeof o->cover);
}

static void put(ModOverlay *o, int x, int y, int colour) {
  if (x < 0 || y < 0 || x >= o->width || y >= MOD_OVERLAY_H) return;
  int r = colour & 31, g = (colour >> 5) & 31, b = (colour >> 10) & 31;
  o->rgb[y][x][0] = (uint8_t)((r << 3) | (r >> 2));
  o->rgb[y][x][1] = (uint8_t)((g << 3) | (g >> 2));
  o->rgb[y][x][2] = (uint8_t)((b << 3) | (b >> 2));
  o->cover[y][x] = 255;
}

void overlay_rect(ModOverlay *o, int x, int y, int w, int h, int colour) {
  if (w <= 0 || h <= 0) return;
  // Clamp before looping, not per pixel: these bounds come from mod code (Task 8 binds this to
  // oracles.draw_rect straight from luaL_checkinteger), and iterating w*h would let
  // draw_rect(0, 0, 1e9, 1e9) spin forever. The wider type keeps x + w / y + h from overflowing.
  // long long rather than long: on LLP64 targets (MSVC) long is only 32 bits, which would not
  // protect this addition from overflowing.
  long long x0 = x > 0 ? x : 0, y0 = y > 0 ? y : 0;
  long long x1 = (long long)x + w, y1 = (long long)y + h;
  if (x1 > o->width) x1 = o->width;
  if (y1 > MOD_OVERLAY_H) y1 = MOD_OVERLAY_H;
  for (long long j = y0; j < y1; j++)
    for (long long i = x0; i < x1; i++) put(o, (int)i, (int)j, colour);
}

void overlay_text(ModOverlay *o, const UiFont *font, int x, int y, const char *s, int colour) {
  const UiFont *f = font;
  if (!f->loaded) return;
  for (; *s; s++, x += UI_GLYPH_W) {
    // Guards against mod-supplied strings, not micro-optimisation: Task 8 hands this string
    // straight from Lua, and Lua strings can be megabytes. Without these, every character would
    // run a 16x8 inner loop even when the glyph is nowhere near the visible overlay, so a huge
    // string would spin for a very long time instead of clipping like overlay_rect does.
    if (x >= o->width) break;          // x only grows, so nothing further right can draw either
    if (x + UI_GLYPH_W <= 0) continue; // glyph is entirely off the left edge: skip its pixels
    uint8_t ch = (uint8_t)*s;
    if (ch >= 128) continue;
    for (int r = 0; r < UI_GLYPH_H; r++)
      for (int b = 0; b < UI_GLYPH_W; b++)
        if (!(f->glyph[ch][r] & (0x80 >> b))) put(o, x + b, y + r, colour);
  }
}

bool overlay_icon(ModOverlay *o, const uint8_t *rom, size_t rom_size, bool seasons, uint8_t item,
                  const uint8_t *c600, int x, int y) {
  WideItemIcon ic;
  if (!wide_item_icon(rom, rom_size, seasons, item, c600, &ic)) return false;
  // The icon is palette-indexed; without the game's live palettes a mod gets a readable grey ramp.
  static const int shade[4] = {0, 0x4210, 0x6318, 0x7fff};
  for (int j = 0; j < 16; j++)
    for (int i = 0; i < ic.halves * 8; i++)
      if (ic.ci[j][i]) put(o, x + i, y + j, shade[ic.ci[j][i]]);
  return true;
}

uint8_t overlay_cover_at(const ModOverlay *o, int x, int y) {
  if (x < 0 || y < 0 || x >= o->width || y >= MOD_OVERLAY_H) return 0;
  return o->cover[y][x];
}

const uint8_t *overlay_rgb_at(const ModOverlay *o, int x, int y) {
  // Bounds-checked, unlike the naive version: callers that are (or will be, once Lua bindings
  // land) handed mod-supplied coordinates must not be able to read outside the backing array.
  static const uint8_t uncovered[3] = {0, 0, 0};
  if (x < 0 || y < 0 || x >= o->width || y >= MOD_OVERLAY_H) return uncovered;
  return o->rgb[y][x];
}
