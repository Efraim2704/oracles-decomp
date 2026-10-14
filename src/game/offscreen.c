#include "game/game.h"
#include "game/offscreen.h"
#include "hw/render.h"
#include <string.h>

// Widescreen shows 48 pixels past each side of the screen. drawAllSprites drops every sprite that
// lands off the 160x144 screen (its positions are 8-bit), so this redoes its position sums for the
// queued objects at full width and keeps the sprites it dropped that the strips would show. It only
// reads memory: the game runs exactly as before.

enum { SIDE = 48, SCROLL_TRANSITION = 0x08 };

static uint8_t object_byte(GB *gb, uint16_t addr) { return gb->wram[1][addr & 0xfff]; }

static uint8_t rom_byte(GB *gb, int bank, uint16_t addr) {
  size_t at = (size_t)bank * 0x4000 + (addr & 0x3fff);
  return at < gb->rom_size ? gb->rom[at] : 0;
}

// The object's position as drawAllSprites adds its sprites to (OAM y and x, before each sprite's
// own offset), false where it is not visible. *boxed: during a screen transition, whether the
// object is inside the area the game draws objects in at all (elsewhere every object is).
static bool object_origin(GB *gb, uint16_t yh_addr, bool hidden, int *y, int *x, bool *boxed) {
  uint16_t base = (uint16_t)(yh_addr & ~0x3f);
  int yh = object_byte(gb, yh_addr), xh = object_byte(gb, yh_addr + OBJ_XH - OBJ_YH);
  int zh = (int8_t)object_byte(gb, yh_addr + OBJ_ZH - OBJ_YH);
  if (!hidden && !(object_byte(gb, base + OBJ_VISIBLE) & 0x80)) return false;
  *boxed = true;
  if (W8(wScrollMode) != SCROLL_TRANSITION) {
    *y = yh - H8(hCameraY) + 0x10 + zh;
    *x = xh - H8(hCameraX);
    return true;
  }
  if ((object_byte(gb, base + OBJ_ENABLED) & 0x03) == 0x03) {
    // a persistent object keeps the 8-bit sums while the camera spans two rooms: only the screen
    // position modulo 256 means anything, so take the one beside the screen
    int y8 = (yh - H8(hCameraY) + 0x10) & 0xff, x8 = (xh - H8(hCameraX)) & 0xff;
    *y = (y8 >= 0xf0 ? y8 - 0x100 : y8) + zh;
    *x = x8 >= FB_W + 8 + SIDE ? x8 - 0x100 : x8;
    return true;
  }
  int y16 = yh + 0x10, x16 = xh;
  if (H8(hFF8A) & object_byte(gb, base + OBJ_ENABLED)) {
    y16 += (int16_t)(H8(hFF90) | H8(hFF90 + 1) << 8);
    x16 += (int16_t)(H8(hFF90 + 2) | H8(hFF90 + 3) << 8);
  }
  int sy = (int16_t)(y16 - (H8(hCameraY) | H8(hCameraY + 1) << 8));
  int sx = (int16_t)(x16 - (H8(hCameraX) | H8(hCameraX + 1) << 8));
  *boxed = sy >= -0x20 && sy < 0xb0 && sx >= -0x18 && sx < 0xb8;
  *y = sy + zh;
  *x = sx;
  return true;
}

// One bit per object slot: pages $d0-$df, four objects a page
static uint64_t slot_bit(uint16_t base) { return 1ull << (((base >> 8) & 0x0f) << 2 | (base >> 6 & 3)); }

static bool add_sprites(GB *gb, OffscreenSprites *out, uint16_t yh_addr, bool hidden, bool room_objects, bool *boxed_out) {
  uint16_t base = (uint16_t)(yh_addr & ~0x3f);
  int oy, ox;
  bool boxed;
  if (!object_origin(gb, yh_addr, hidden, &oy, &ox, &boxed)) return true;
  if (boxed_out) *boxed_out = boxed;
  uint8_t flags = object_byte(gb, base + OBJ_OAM_FLAGS), tile_base = object_byte(gb, base + OBJ_OAM_TILE_INDEX_BASE);
  uint16_t data = (uint16_t)(object_byte(gb, base + OBJ_OAM_DATA_ADDRESS) | object_byte(gb, base + OBJ_OAM_DATA_ADDRESS + 1) << 8);
  int bank = GV(0x13, 0x12) + (data >> 14);
  uint16_t p = (uint16_t)((data & 0x3fff) | 0x4000);
  int count = rom_byte(gb, bank, p++);
  for (int i = 0; i < count; i++, p += 4) {
    int dy = rom_byte(gb, bank, p), dx = rom_byte(gb, bank, p + 1);
    int sy = oy + (int8_t)dy - 16, sx = ox + (int8_t)dx - 8;
    if (!room_objects) {
      if (boxed && !hidden && ((oy + dy) & 0xff) < 0xa0 && ((ox + dx) & 0xff) < 0xa8) continue;   // the game drew it
      if (sy <= -16 || sy >= FB_H || sx <= -SIDE - 8 || sx >= FB_W + SIDE) continue;
    }
    if (out->n == OFFSCREEN_MAX) return false;
    out->s[out->n++] = (OffscreenSprite){(int16_t)sy, (int16_t)sx, (uint8_t)(tile_base + rom_byte(gb, bank, p + 2)),
                                         (uint8_t)(flags ^ rom_byte(gb, bank, p + 3))};
  }
  return true;
}

void offscreen_random_placed(GB *gb, uint8_t page) {
  render_line_regs(gb)->offscreen_random |= slot_bit((uint16_t)(page << 8 | 0x80));
}

// Before drawAllSprites' draw loop: wObjectsToDraw holds the queue, the camera and the screen
// transition offsets are set.
void offscreen_sprites_capture(GB *gb) {
  RenderLineRegs *lines = render_line_regs(gb);
  OffscreenSprites *out = &lines->offscreen_pending;
  bool room_objects = lines->offscreen_room_objects, transition = W8(wScrollMode) == SCROLL_TRANSITION;
  uint16_t follower = (uint16_t)(W8(wFollowingLinkObject) << 8 | W8(wFollowingLinkObjectType));
  const uint8_t *queue = &W8(wObjectsToDraw);
  uint64_t queued = 0;
  if (!transition) lines->offscreen_hidden = 0;
  out->n = 0;
  for (int e = 0; e < 0x80; e += 2) {
    uint8_t lo = queue[e], hi = queue[e + 1];
    if (!lo) continue;
    uint16_t yh_addr = (uint16_t)(hi << 8 | lo), base = (uint16_t)(yh_addr & ~0x3f);
    // Link, his companion and items take the first slot of each page
    if (room_objects && (!(base & 0xc0) || base == follower || (lines->offscreen_random & slot_bit(base)))) continue;
    bool boxed = true;
    queued |= slot_bit(base);
    if (!add_sprites(gb, out, yh_addr, false, room_objects, &boxed)) return;
    // the game hides an object of the room being left (enabled bit 1) once it leaves the box
    if (transition && !boxed && (object_byte(gb, base + OBJ_ENABLED) & 0x02)) lines->offscreen_hidden |= slot_bit(base);
  }
  for (int slot = 0; slot < 64 && lines->offscreen_hidden; slot++) {
    uint64_t bit = 1ull << slot;
    if (!(lines->offscreen_hidden & bit) || (queued & bit)) continue;
    uint16_t base = (uint16_t)(w1Link | (slot >> 2) << 8 | (slot & 3) << 6);
    if (!object_byte(gb, base + OBJ_ENABLED)) { lines->offscreen_hidden &= ~bit; continue; }
    if (!add_sprites(gb, out, (uint16_t)(base + OBJ_YH), true, room_objects, NULL)) return;
  }
}

// At the end of drawAllSprites: the OAM buffer these sprites go with.
void offscreen_sprites_commit(GB *gb) {
  memcpy(render_line_regs(gb)->pending_oam, &W8(wOam), 160);
}
