#include "unit.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "mods/overlay.h"

// The vendored interpreter builds and runs: the floor everything else stands on.
static void lua_runs(void) {
  lua_State *L = luaL_newstate();
  ASSERT(L);
  luaL_openlibs(L);
  ASSERT_EQ(luaL_dostring(L, "return 1 + 1"), LUA_OK);
  ASSERT_EQ((int)lua_tointeger(L, -1), 2);
  lua_close(L);
}

// The overlay is the picture's size, and only what a mod drew is covered.
static void overlay_draws_a_rect(void) {
  static ModOverlay o;
  overlay_begin(&o, 256);
  ASSERT_EQ(o.width, 256);
  ASSERT_EQ(overlay_cover_at(&o, 10, 10), 0);          // nothing drawn yet
  overlay_rect(&o, 8, 4, 3, 2, 0x7fff);                // white, RGB555
  ASSERT_EQ(overlay_cover_at(&o, 8, 4), 255);
  ASSERT_EQ(overlay_cover_at(&o, 10, 5), 255);
  ASSERT_EQ(overlay_cover_at(&o, 11, 4), 0);           // one past the right edge
  ASSERT_EQ(overlay_cover_at(&o, 8, 6), 0);            // one past the bottom
  const uint8_t *p = overlay_rgb_at(&o, 8, 4);
  ASSERT_EQ(p[0], 255); ASSERT_EQ(p[1], 255); ASSERT_EQ(p[2], 255);
}

// Drawing off the edge is clipped, not a crash or a wrap onto the next row.
static void overlay_clips(void) {
  static ModOverlay o;
  overlay_begin(&o, 160);
  overlay_rect(&o, 158, 142, 10, 10, 0x7fff);
  ASSERT_EQ(overlay_cover_at(&o, 159, 143), 255);
  overlay_rect(&o, -4, -4, 6, 6, 0x7fff);
  ASSERT_EQ(overlay_cover_at(&o, 1, 1), 255);
  ASSERT_EQ(overlay_cover_at(&o, 2, 2), 0);
}

// A mod is untrusted code: a huge rectangle must clip, not iterate w*h times.
static void overlay_survives_a_huge_rect(void) {
  static ModOverlay o;
  overlay_begin(&o, 160);
  overlay_rect(&o, -1000000, -1000000, 2000000000, 2000000000, 0x7fff);
  ASSERT_EQ(overlay_cover_at(&o, 0, 0), 255);
  ASSERT_EQ(overlay_cover_at(&o, 159, 143), 255);
  overlay_begin(&o, 160);
  overlay_rect(&o, 0, 0, 0, 0, 0x7fff);                // empty rect draws nothing
  ASSERT_EQ(overlay_cover_at(&o, 0, 0), 0);
}

// Each frame starts clear, including when widescreen is turned off and the picture narrows.
static void overlay_begin_clears_the_last_frame(void) {
  static ModOverlay o;
  overlay_begin(&o, 256);
  overlay_rect(&o, 200, 10, 4, 4, 0x7fff);             // in the side strip
  ASSERT_EQ(overlay_cover_at(&o, 200, 10), 255);
  overlay_begin(&o, 160);
  ASSERT_EQ(overlay_cover_at(&o, 100, 10), 0);
  overlay_begin(&o, 256);
  ASSERT_EQ(overlay_cover_at(&o, 200, 10), 0);         // not left over from two frames ago
}

int main(void) {
  RUN(lua_runs);
  RUN(overlay_draws_a_rect);
  RUN(overlay_clips);
  RUN(overlay_survives_a_huge_rect);
  RUN(overlay_begin_clears_the_last_frame);
  return 0;
}
