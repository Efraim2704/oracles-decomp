#include "mods/api.h"
#include "mods/mods.h"
#include "mods/overlay.h"
#include "lua.h"
#include "lauxlib.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

// Large enough for any ordinary log line (and mods_escape_into's worst case, where a byte of
// input can expand to a 4-byte \xHH escape) without making the buffer itself a place a mod can
// grow memory usage unboundedly -- it can't, since mods_escape_into is already bounded by
// dst_size and simply truncates past it. A log line this long is already not something a status
// line could usefully show in full.
static char last_log[512];

const char *mods_last_log(void) { return last_log[0] ? last_log : NULL; }
void mods_api_clear_log(void) { last_log[0] = 0; }

static int l_log(lua_State *L) {
  // luaL_checklstring, not luaL_checkstring: a Lua string can contain an embedded NUL, and
  // checkstring's bare `const char *` would silently truncate at it with nobody having decided
  // that was fine. The decision made here is not to truncate at all: mods_escape_into renders
  // every byte outside printable ASCII -- NUL included -- as a literal \xHH escape, so the whole
  // message survives (readably) in last_log and in the stderr line below. That's the same
  // escaper mods.c already uses for mod.ini values, reused here rather than duplicated, and it
  // closes off the same terminal-injection hole an unescaped CR/ANSI-escape log string would
  // otherwise open (a mod could otherwise print "\r\x1b[2K" followed by a forged status line).
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  mods_escape_into(last_log, sizeof last_log, s, len);
  fprintf(stderr, "mod: %s\n", last_log);
  return 0;
}

// lua_Integer is 64-bit; a bare `(int)luaL_checkinteger(...)` therefore truncates rather than
// clamps -- draw_rect(0, 0, 4294967296 + 5, 1, c) would arrive at overlay_rect as w == 5, not an
// error and not INT_MAX, simply because 4294967301 mod 2^32 happens to be 5. The overlay's own
// guards (overlay_rect/overlay_text/overlay_icon) already cope with any int, so this is never a
// crash either way -- but silently wrapping is still baffling behaviour no mod author watching
// their rectangle draw five pixels wide instead of refusing outright could ever predict from
// their own source. Every binding below that takes a coordinate or size from Lua goes through
// this instead of a bare cast, so an out-of-int-range argument saturates to INT_MIN/INT_MAX,
// which is at least a value a mod author could reason about (and, for an absurd width like this,
// indistinguishable in effect from "the whole overlay", which overlay_rect already clips to).
static int check_int(lua_State *L, int arg) {
  lua_Integer v = luaL_checkinteger(L, arg);
  if (v < INT_MIN) return INT_MIN;
  if (v > INT_MAX) return INT_MAX;
  return (int)v;
}

static int l_on_frame(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  // luaL_ref pops the function off L's stack and stores it in the registry (shared across every
  // thread of this mod's Lua state, not just L), returning an int handle -- mods_set_on_frame_ref
  // is what actually remembers it, against whichever mod is currently running this call (mods.c's
  // active_mod), not a Lua global: see mods_set_on_frame_ref's own comment in mods.h for why a
  // global would let a mod bypass this binding entirely.
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  mods_set_on_frame_ref(L, ref);
  return 0;
}

static int l_on_room_change(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  mods_set_on_room_change_ref(L, ref);
  return 0;
}

static int l_game(lua_State *L) {
  lua_pushstring(L, mods_current_frame()->seasons ? "seasons" : "ages");
  return 1;
}

static int l_screen(lua_State *L) {
  lua_pushinteger(L, mods_current_frame()->width);
  return 1;
}

// $c000..$cfff and $ff80..$fffe only -- the same two windows wide.c reads, and the only ones
// ModsFrame carries. The switchable bank at $d000..$dfff is not exposed at all: a plausible-
// looking address in that range silently reads back 0, the same as any other address outside
// both windows, rather than erroring -- acceptable for this stage (nothing in the planned API
// needs it yet), but worth a mod author being told in MODDING.md (Task 11) rather than
// discovering it by reading zeroes and assuming their own math was wrong.
static int l_rd(lua_State *L) {
  int addr = check_int(L, 1);
  const ModsFrame *f = mods_current_frame();
  int v = 0;
  if (addr >= 0xc000 && addr <= 0xcfff && f->wram) v = f->wram[addr - 0xc000];
  else if (addr >= 0xff80 && addr <= 0xfffe && f->hram) v = f->hram[addr - 0xff80];
  lua_pushinteger(L, v);
  return 1;
}

static int l_draw_rect(lua_State *L) {
  overlay_rect(mods_overlay_mut(), check_int(L, 1), check_int(L, 2), check_int(L, 3),
               check_int(L, 4), check_int(L, 5));
  return 0;
}

static int l_draw_text(lua_State *L) {
  const ModsFrame *f = mods_current_frame();
  if (f->font) {
    // luaL_checklstring, not luaL_checkstring: a Lua string may legally contain an embedded
    // NUL, and that decision is made explicit here on purpose -- overlay_text (Task 3, not
    // touched by this task) takes a plain NUL-terminated C string with no length parameter of
    // its own, so "a\0b" still only draws "a"'s glyph no matter which checker is used to fetch
    // the argument. Using checklstring instead of checkstring is this binding saying that
    // truncation out loud -- choosing it deliberately, as the task requires, rather than
    // silently inheriting checkstring's behaviour and leaving it undecided. `len` is kept (not
    // discarded) so the decision is visible at the call site, and so a future change here, or in
    // overlay_text, has it on hand rather than needing to recompute it. See
    // draw_text_truncates_at_an_embedded_nul in test_mods.c for the pinned behaviour, and
    // MODDING.md (Task 11) for where a mod author is actually told.
    size_t len;
    const char *s = luaL_checklstring(L, 3, &len);
    (void)len;
    overlay_text(mods_overlay_mut(), f->font, check_int(L, 1), check_int(L, 2), s, check_int(L, 4));
  }
  return 0;
}

static int l_draw_icon(lua_State *L) {
  const ModsFrame *f = mods_current_frame();
  bool ok = false;
  // Both guarded, not just f->rom: overlay_icon's c600 argument is f->wram + 0x600, which is
  // NULL + 0x600 (undefined behaviour, not a null pointer) if f->wram is NULL -- true in the
  // launcher before any game is running, exactly the state f->rom is also NULL in, but a
  // distinct field a mod-author-visible guard on f->rom alone would not catch. wide_item_icon
  // null-checks rom but not c600, so this is the only guard standing between a NULL wram and a
  // read through it.
  if (f->rom && f->wram)
    ok = overlay_icon(mods_overlay_mut(), f->rom, f->rom_size, f->seasons,
                      (uint8_t)luaL_checkinteger(L, 1), f->wram + 0x600,
                      check_int(L, 2), check_int(L, 3));
  lua_pushboolean(L, ok);
  return 1;
}

// The one list of what the API is. MODDING.md must document exactly these. Kept alphabetical so
// that cross-check can simply walk both lists in order.
static const luaL_Reg api[] = {
  {"draw_icon", l_draw_icon},
  {"draw_rect", l_draw_rect},
  {"draw_text", l_draw_text},
  {"game", l_game},
  {"log", l_log},
  {"on_frame", l_on_frame},
  {"on_room_change", l_on_room_change},
  {"rd", l_rd},
  {"screen", l_screen},
  {NULL, NULL},
};

void mods_api_open(lua_State *L) {
  luaL_newlib(L, api);
  lua_setglobal(L, "oracles");
}

int mods_api_count(void) { return (int)(sizeof api / sizeof api[0]) - 1; }
const char *mods_api_name(int i) { return i >= 0 && i < mods_api_count() ? api[i].name : ""; }
