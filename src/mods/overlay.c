#include "mods/overlay.h"
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
