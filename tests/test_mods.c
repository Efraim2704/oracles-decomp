#include "unit.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "mods/mods.h"
#include "mods/api.h"
#include "mods/overlay.h"
#include "mods/sandbox.h"
#include "ui/ui.h"
#include "wide/item.h"
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Wall-clock, not clock(): clock() measures *CPU* time consumed by this process, which is
// exactly the wrong thing to assert against here. Under the external host contention that
// motivated the wall-clock deadline in mods.c in the first place, a starved process can show
// very little CPU time consumed while the *user* -- and ctest, and anyone watching the app start
// -- experienced several real seconds of wait. A clock()-based assertion would silently pass
// through exactly the degradation this is supposed to catch. CLOCK_MONOTONIC mirrors what
// mods.c's own deadline check uses, for the same reason.
static double wall_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// mods.c now backs its instruction budget with a MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS (2.0s)
// wall-clock cutoff precisely because instruction count alone could not bound real time: a mod
// that repeatedly raises and catches a genuine Lua error costs 50-150x more wall-clock time per
// VM instruction than a bare loop, which an earlier version of this suite pinned with a
// deliberately wider, one-off timing bound on the single test that showed it. With the deadline
// in place, every budget-triggered refusal -- cheap loop or expensive one alike -- is now
// bounded at the same ~2 second ceiling (plus a small, bounded margin for how long it takes to
// get scheduled again after the deadline nominally passes), so there is no longer a reason for
// one test to need a different bound than the rest: 5.0s gives real margin above that ~2s
// ceiling for ordinary scheduling jitter, while still catching a mutation that weakens or removes
// the deadline outright (which would put a cheap-per-instruction loop back at risk of the
// multi-second-to-unbounded blowup this was built to close). The alarm()-based hang detection
// below is NOT gated by this and must always run -- it is what catches an actual hang, as
// opposed to a merely slow-but-bounded pass, and it stays generous (15-20s) precisely so it never
// fires on the latter.
//
// There used to be a MODS_TEST_TIMING escape hatch here, unset by CMake for sanitizer builds,
// calibrated against the old 2.0s bound that ASan+UBSan together could not reliably meet. With
// this bound now at 5.0s (and ASan+UBSan needing more like 2.5s), both configurations pass
// comfortably either way -- confirmed by building both ways -- so the gate was removed rather
// than kept: it no longer bought any reliability, only a hole in coverage under sanitizers, and
// its detection of "am I under a sanitizer" was unsound in both directions besides (a
// no-sanitizer build via -fno-sanitize-recover=all in CMAKE_C_FLAGS would have silently disabled
// it; a sanitizer configured through CMAKE_C_FLAGS_DEBUG instead of CMAKE_C_FLAGS would have been
// missed entirely).
#define ASSERT_PROMPT(t0, t1) ASSERT(((t1) - (t0)) < 5.0)

// Which of the two mechanisms in mods.c actually catches a given runaway mod now depends on how
// expensive its instructions are and how busy the host is, not just on the mod's shape: a cheap
// loop on an idle host trips the instruction budget well before the wall-clock deadline; the
// same loop on a sufficiently busy host, or an expensive-per-instruction loop on any host, trips
// the deadline instead. Both are the budget mechanism doing its job, so tests that don't care
// which one fired assert this instead of a single fixed message.
#define ASSERT_BUDGET_OR_DEADLINE(out) \
  ASSERT(strstr((out), "instruction budget") != NULL || strstr((out), "cut off") != NULL)

// read_file comes from unit.h (used the same way by test_acid2.c / test_blargg.c): test_mods
// links oraclesmods + oraclesitem only, not the engine, so it cannot call oracles_read_file --
// linking the engine into the mod library is exactly what the design forbids.

// unit.h's SKIP is exit(77), which ends the whole process. Every other *_tas/*_wide-style test
// file has exactly one ROM-dependent test, so that's harmless there. This file mixes ROM-free
// tests with ROM-dependent ones, and will keep growing more of each as later tasks add tests, so
// exiting mid-run would silently stop running every test registered after the one that skips --
// and ctest would still report "Skipped", not a failure. SKIP_TEST prints the same line SKIP does
// and returns false instead of exiting, so the rest of main() keeps running; RUN_SKIPPABLE is what
// records that something was skipped, from that false return. Do not "simplify" this back to SKIP.
//
// A skippable test returns false rather than void, because unit.h's RUN prints "ok" unconditionally
// once the body returns -- a skipped test would be reported as both skipped and passing, which
// reads as a pass in a CI log. RUN_SKIPPABLE prints one line or the other, never both.
static int skipped;
#define SKIP_TEST(msg) do { printf("skip: %s\n", msg); return false; } while (0)
#define RUN_SKIPPABLE(fn) do { if (fn()) printf("ok %s\n", #fn); else skipped = 1; } while (0)

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

// Text uses the built-in font so a mod can draw before any ROM is installed. Checked exactly,
// pixel by pixel, against the font data itself: this single full-overlay comparison subsumes the
// inversion (clear bit is ink), the x advance between characters, the y offset per row, "only ink
// is covered" and "not a filled block" all at once, and fails on a stray write anywhere outside
// the two glyph cells too.
static void overlay_draws_text(void) {
  static ModOverlay o;
  static UiFont font;
  ui_font_builtin(&font);
  overlay_begin(&o, 160);
  const char *s = "AB";
  overlay_text(&o, &font, 4, 4, s, 0x7fff);

  int inkx = -1, inky = -1;
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) {
      int rel_x = x - 4, rel_y = y - 4;
      int expected = 0;
      if (rel_x >= 0 && rel_x < (int)strlen(s) * UI_GLYPH_W && rel_y >= 0 && rel_y < UI_GLYPH_H) {
        int i = rel_x / UI_GLYPH_W, b = rel_x % UI_GLYPH_W;
        expected = !(font.glyph[(uint8_t)s[i]][rel_y] & (0x80 >> b)) ? 255 : 0;
      }
      ASSERT_EQ(overlay_cover_at(&o, x, y), expected);
      if (expected && inkx < 0) { inkx = x; inky = y; }
    }
  ASSERT(inkx >= 0);                                   // sanity: "AB" has some ink to check below

  // The ink pixels carry the colour that was passed in, not just coverage.
  const uint8_t *p = overlay_rgb_at(&o, inkx, inky);
  ASSERT_EQ(p[0], 255); ASSERT_EQ(p[1], 255); ASSERT_EQ(p[2], 255);  // 0x7fff -> white
}

// A mod-supplied string can contain bytes >= 0x80, and ui_font_builtin's glyph table is only
// [128][UI_GLYPH_H]: without the `ch >= 128` skip, overlay_text would read font.glyph[ch] past the
// end of that array. Verified once under -fsanitize=address with the skip removed -- see the
// report for what ASan said -- rather than relying on this assertion to notice stray memory
// contents happening to look like ink, which it cannot do reliably.
static void overlay_text_skips_high_bytes(void) {
  static ModOverlay o;
  static UiFont font;
  ui_font_builtin(&font);
  overlay_begin(&o, 160);
  char s[] = { (char)0x80, (char)0xff, 0 };
  overlay_text(&o, &font, 4, 4, s, 0x7fff);
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) ASSERT_EQ(overlay_cover_at(&o, x, y), 0);
}

// Before a ROM is installed and ui_font_load has never run (or ui_font_builtin hasn't either),
// a zero-initialized UiFont has loaded == false and must draw nothing rather than reading
// never-filled-in glyph data.
static void overlay_text_with_unloaded_font_draws_nothing(void) {
  static ModOverlay o;
  static UiFont font;                  // zero-initialized static storage: loaded == false
  overlay_begin(&o, 160);
  overlay_text(&o, &font, 4, 4, "AB", 0x7fff);
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) ASSERT_EQ(overlay_cover_at(&o, x, y), 0);
}

// A mod is untrusted code: a huge string must clip in O(length), not run the 8x16 inner loop for
// every character regardless of position. Unlike overlay_rect's clamp, these guards only change
// how much work is done, not what ends up on the overlay: put() already clips, so the pixels this
// test checks come out identical whether the guards are there or not (confirmed by removing them
// and rerunning this test unchanged: it still passed). The only thing that actually distinguishes
// "clipped in O(length)" from "ran the full 128-put inner loop on every character anyway" is time,
// so this also times the two calls directly, excluding the malloc/memset above: on this machine
// that's ~0.005s with the guards and ~0.9s without them for an 8 MiB string, so 0.3s is a loose
// bound that is nowhere near either side.
static void overlay_survives_a_huge_string(void) {
  static ModOverlay o;
  static UiFont font;
  ui_font_builtin(&font);
  size_t n = 8 * 1024 * 1024;
  char *s = malloc(n + 1);
  ASSERT(s);
  memset(s, 'A', n);
  s[n] = 0;

  // Runs off the right edge after ~20 glyphs: the `break` guard must stop the rest from being
  // walked pixel-by-pixel.
  overlay_begin(&o, 160);
  clock_t t0 = clock();
  overlay_text(&o, &font, 0, 4, s, 0x7fff);
  clock_t t1 = clock();
  int onscreen = 0;
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) onscreen += overlay_cover_at(&o, x, y) ? 1 : 0;
  ASSERT(onscreen > 0);                                  // the glyphs that fit on screen drew
  ASSERT(onscreen < 160 * MOD_OVERLAY_H);                // but the rest of the string did not

  // Entirely off the left edge: the `continue` guard must skip every glyph's pixels while still
  // walking the (long) string.
  overlay_begin(&o, 160);
  clock_t t2 = clock();
  overlay_text(&o, &font, -100000000, 4, s, 0x7fff);
  clock_t t3 = clock();
  int offscreen = 0;
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) offscreen += overlay_cover_at(&o, x, y) ? 1 : 0;
  ASSERT_EQ(offscreen, 0);                               // not one glyph could land

  // clock() is process CPU time on the platforms CI actually runs this on (ubuntu/macos); on
  // MSVC/MinGW it is wall-clock instead, which would make this bound load-sensitive. Nothing to
  // change today -- the MinGW job only builds, it doesn't run ctest -- but worth knowing before
  // this is ever run there.
  double secs = (double)((t1 - t0) + (t3 - t2)) / CLOCKS_PER_SEC;
  ASSERT(secs < 0.3);   // only the loop-skipping guards keep this fast; see the comment above

  free(s);
}

// Partially off the left edge: unlike the huge-string test's x = -100000000 above (skipped
// wholesale by the `continue` guard, which only fires once a whole glyph cell is off-screen),
// x = -4 straddles the left edge of a single glyph, so put() must clip its columns 0-3
// individually rather than ever being allowed to wrap onto another row. This is the failure
// overlay_clips' comment already claims to cover for overlay_rect; nothing exercised it for
// overlay_text until now. Drawn in a 256-wide (widescreen) overlay, not 160: a wraparound bug --
// the off-screen columns landing on the previous row's right edge instead of being clipped --
// writes into physical columns 252-255 regardless of the logical width, and overlay_cover_at
// refuses to report anything at or past that logical width. At width 160 those columns are
// unreachable through the public API and the bug would be invisible to this test; at 256 they
// are in range, so a real clipping failure actually shows up.
static void overlay_draws_a_partially_clipped_glyph(void) {
  static ModOverlay o;
  static UiFont font;
  ui_font_builtin(&font);
  overlay_begin(&o, 256);
  overlay_text(&o, &font, -4, 4, "A", 0x7fff);

  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 256; x++) {
      int rel_x = x + 4, rel_y = y - 4;          // generalizes the same formula to negative x
      int expected = 0;
      if (rel_x >= 0 && rel_x < UI_GLYPH_W && rel_y >= 0 && rel_y < UI_GLYPH_H)
        expected = !(font.glyph[(uint8_t)'A'][rel_y] & (0x80 >> rel_x)) ? 255 : 0;
      ASSERT_EQ(overlay_cover_at(&o, x, y), expected);
    }
}

// Shared by overlay_draws_an_item_icon for both items it checks: asserts the whole overlay
// matches a decoded icon drawn at (ox, oy), pixel by pixel, including that nothing is covered
// outside its rectangle. (overlay_icon's `i < ic->halves * 8` loop bound is defensive only --
// wide_item_icon zero-fills ci beyond the real width and the index-0 check already suppresses an
// unused second half, so i < 16 is an equivalent mutant and no pixel-level test can pin that
// bound specifically.)
static void assert_icon_drawn_exactly(const ModOverlay *o, const WideItemIcon *ic, int ox, int oy) {
  static const int shade[4] = {0, 0x4210, 0x6318, 0x7fff};   // mirrors overlay.c's static shade[]
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) {
      int rel_x = x - ox, rel_y = y - oy;
      bool in_icon = rel_x >= 0 && rel_x < ic->halves * 8 && rel_y >= 0 && rel_y < 16;
      int ci = in_icon ? ic->ci[rel_y][rel_x] : 0;
      ASSERT_EQ(overlay_cover_at(o, x, y), ci ? 255 : 0);
      if (ci) {
        int colour = shade[ci];
        int r = colour & 31, g = (colour >> 5) & 31, b = (colour >> 10) & 31;
        const uint8_t *p = overlay_rgb_at(o, x, y);
        ASSERT_EQ(p[0], (uint8_t)((r << 3) | (r >> 2)));
        ASSERT_EQ(p[1], (uint8_t)((g << 3) | (g >> 2)));
        ASSERT_EQ(p[2], (uint8_t)((b << 3) | (b >> 2)));
      }
    }
}

// draw_icon reuses the ROM item-icon decoder, so mods get real item sprites. Decoding directly
// here with wide_item_icon (not just through overlay_icon) is legitimate rather than circular:
// overlay_icon's job is compositing an already-decoded WideItemIcon onto the overlay, and the
// decode itself is independently verified against the game's own WRAM by the wide tests. Checked
// exactly, pixel by pixel, against that decode: this subsumes index-0-stays-clear, the (x, y)
// placement and the grey ramp all at once, and fails on a stray write anywhere outside the icon's
// rectangle too.
static bool overlay_draws_an_item_icon(void) {
  size_t n;
  uint8_t *rom = read_file(GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Seasons (USA, Australia).gbc", &n);
  if (!rom) SKIP_TEST("ROM not present");
  static uint8_t c600[256];
  // The entries name a byte in $c600 -- a level, or which seeds are selected -- that picks the
  // variant to show; every byte being 1 makes each item decode its level-1 (or first-seed)
  // variant, whatever that item has.
  memset(c600, 1, sizeof c600);
  static ModOverlay o;

  // Bombs (0x03): halves == 1, the 8-wide path.
  WideItemIcon bombs;
  ASSERT(wide_item_icon(rom, n, true, 0x03, c600, &bombs));
  int ink = 0;
  for (int j = 0; j < 16; j++)
    for (int i = 0; i < bombs.halves * 8; i++) ink += bombs.ci[j][i] ? 1 : 0;
  ASSERT(ink > 0);          // a blank decode would make the pixel sweep below vacuously pass
  overlay_begin(&o, 160);
  ASSERT(overlay_icon(&o, rom, n, true, 0x03, c600, 20, 20));
  assert_icon_drawn_exactly(&o, &bombs, 20, 20);

  // Item 0x0e: halves == 2, the 16-wide path nothing else here exercises.
  WideItemIcon wide_item;
  ASSERT(wide_item_icon(rom, n, true, 0x0e, c600, &wide_item));
  ink = 0;
  for (int j = 0; j < 16; j++)
    for (int i = 0; i < wide_item.halves * 8; i++) ink += wide_item.ci[j][i] ? 1 : 0;
  ASSERT(ink > 0);
  overlay_begin(&o, 160);
  ASSERT(overlay_icon(&o, rom, n, true, 0x0e, c600, 20, 20));
  assert_icon_drawn_exactly(&o, &wide_item, 20, 20);

  // No item: false, and nothing drawn -- checked for real with a fresh, otherwise-untouched
  // overlay, not just the return value.
  overlay_begin(&o, 160);
  ASSERT(!overlay_icon(&o, rom, n, true, 0, c600, 0, 0));
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) ASSERT_EQ(overlay_cover_at(&o, x, y), 0);

  free(rom);
  return true;
}

// Writes <dir>/<id>/{mod.ini,main.lua} and returns <dir>.
static const char *make_mod(const char *id, const char *ini, const char *lua) {
  static char root[512];
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  char dir[640], path[768];
  snprintf(dir, sizeof dir, "%s/%s", root, id);
  char cmd[800];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
  ASSERT_EQ(system(cmd), 0);
  snprintf(path, sizeof path, "%s/mod.ini", dir);
  FILE *f = fopen(path, "wb"); ASSERT(f); fputs(ini, f); fclose(f);
  snprintf(path, sizeof path, "%s/main.lua", dir);
  f = fopen(path, "wb"); ASSERT(f); fputs(lua, f); fclose(f);
  return root;
}

// Like make_mod, but writes mod.ini from an explicit byte buffer rather than a NUL-terminated
// C string -- needed only by the embedded-NUL smuggling test below, since a plain `const char *`
// can never represent a NUL byte in the middle of its own content.
static const char *make_mod_bytes(const char *id, const unsigned char *ini_bytes, size_t ini_len) {
  static char root[512];
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  char dir[640], path[768];
  snprintf(dir, sizeof dir, "%s/%s", root, id);
  char cmd[800];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
  ASSERT_EQ(system(cmd), 0);
  snprintf(path, sizeof path, "%s/mod.ini", dir);
  FILE *f = fopen(path, "wb");
  ASSERT(f);
  ASSERT_EQ(fwrite(ini_bytes, 1, ini_len, f), ini_len);
  fclose(f);
  return root;
}

// chmod -R u+rwX before the rm -rf, not just rm -rf, so this self-heals if a previous run died
// between an_unreadable_mods_directory_is_a_failure's chmod(dir, 0) and its restoring chmod back
// -- exactly where a crash or an ASan abort would land while debugging mods.c itself. Without
// this, a mode-000 leftover directory makes system()'s rm -rf fail silently (its status is
// discarded below on purpose, since "nothing to remove" is the overwhelmingly common case and
// not worth an assertion), and every later test run then dies in make_mod's own
// ASSERT_EQ(system(cmd), 0) until a human manually fixes the permissions.
static void clear_mods(void) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char cmd[700];
  snprintf(cmd, sizeof cmd, "chmod -R u+rwX '%s/test_mods_root' 2>/dev/null; rm -rf '%s/test_mods_root'", tmp, tmp);
  (void)system(cmd);
}

// Captures everything written to stderr while running `body`. Needed because several of
// mods_init's refusal paths are also reachable (for the exact same input) by a different,
// later check -- so a bare mods_count() assertion alone cannot tell "the dedicated check I'm
// testing actually ran" apart from "it was deleted and some other check masked that". Confirmed
// by mutation more than once below.
//
// `body` must never itself ASSERT: an assertion failing while stderr is dup2'd into this
// function's temp file sends the FAIL line into that file instead of the terminal/CI log, so the
// developer sees nothing but a bare nonzero exit, and the temp file (never reached by the
// cleanup below, since ASSERT calls exit()) leaks. run_cap_mods_init below exists to avoid this:
// it records mods_init's result in a static instead of asserting on it, so every caller asserts
// on cap_result only after capture_stderr has returned and stderr is restored.
//
// g_real_stderr_fd exists for exactly the same reason, for a hazard found while adding the
// instruction-budget tests below: mods_test_alarm_handler's own "mods_init hung" diagnostic is
// written with a raw write(2, ...) to make it signal-safe, but a hardcoded fd 2 is *this*
// function's own redirect target while `body` is running -- a real hang inside a
// capture_stderr-wrapped mods_init would fire the alarm while fd 2 still points at the temp
// file, trapping the one diagnostic this whole mechanism exists to produce: the process still
// exits (via the handler's _exit), but silently, leaking an unlinked temp file with "FAIL:
// mods_init hung" inside it and showing nothing at all on the terminal or in a CI log --
// confirmed by deliberately triggering it: the hang-detection alarm then looks indistinguishable
// from the test binary just vanishing with no explanation. g_real_stderr_fd is set to `saved`
// (a dup of the *real* stderr, obtained before the redirect) for the duration of `body()`, and
// the handler writes there instead of the literal fd 2 whenever it is set, so the diagnostic
// always reaches the real terminal regardless of what fd 2 happens to be redirected to at the
// moment the alarm fires.
static volatile int g_real_stderr_fd = -1;

static void capture_stderr(void (*body)(void), char *out, size_t out_size) {
  fflush(stderr);
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char path[768];
  snprintf(path, sizeof path, "%s/test_mods_stderr.XXXXXX", tmp);
  int fd = mkstemp(path);
  ASSERT(fd >= 0);
  int saved = dup(fileno(stderr));
  ASSERT(saved >= 0);
  g_real_stderr_fd = saved;
  ASSERT(dup2(fd, fileno(stderr)) >= 0);
  body();
  fflush(stderr);
  g_real_stderr_fd = -1;
  ASSERT(dup2(saved, fileno(stderr)) >= 0);
  close(saved);
  ASSERT(lseek(fd, 0, SEEK_SET) == 0);
  ssize_t n = read(fd, out, out_size - 1);
  ASSERT(n >= 0);
  out[n] = 0;
  close(fd);
  unlink(path);
}

static const char *cap_root;
static bool cap_seasons;
static bool cap_result;
static void run_cap_mods_init(void) { cap_result = mods_init(cap_root, cap_seasons); }

// Forward declaration: defined further down (next to g_real_stderr_fd, which it uses), but a few
// Task 6 tests just above that point already need to arm a hang-detection alarm.
static void mods_test_alarm_handler(int sig);

// mods_id_valid is the one gate standing between an attacker-controlled id and a path traversal
// (correction A), so it gets its own direct, table-driven test rather than relying only on
// mods_init's end-to-end behaviour to exercise it. Covers the accepted charset, the two
// traversal spellings (POSIX "/" and the MinGW/Windows "\\"), a bare "..", a bare ".", a
// trailing dot, shell metacharacters, an embedded newline (as if a hostile field were spliced
// into a format string elsewhere), and the near-misses a narrower check might still accept:
// uppercase, underscore, embedded space, and empty.
//
// "a\\b" and "mods\\evil" are made of nothing but [a-z0-9-] plus one backslash -- unlike
// "..\\evil" above, which also contains '.' and so would die for that reason alone even if the
// backslash check were deleted. Both mods.h and mods.c name "\\" and the Windows/MinGW target
// explicitly as why this check exists, so a mutant that quietly reopens that hole (adding
// `|| *p == '\\'` to the accepted charset) needs a case that can only be caught by the backslash
// check specifically.
static void mods_id_valid_rejects_hostile_input(void) {
  static const struct { const char *id; bool want; } cases[] = {
    {"ok-mod-1", true},
    {"a", true},
    {"0", true},
    {"a-b-c", true},
    {"", false},
    {"..", false},
    {".", false},
    {"../evil", false},
    {"..\\evil", false},
    {"a\\b", false},
    {"mods\\evil", false},
    {"/etc/passwd", false},
    {"a/b", false},
    {"a.b", false},
    {"Bad_Name", false},
    {"BADNAME", false},
    {"has_underscore", false},
    {"sp ace", false},
    {"tilde~", false},
    {"a$(whoami)", false},
    {"mod\nid=other", false},
    {"a.", false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    bool got = mods_id_valid(cases[i].id);
    if (got != cases[i].want) {
      fprintf(stderr, "FAIL %s:%d: mods_id_valid(\"%s\") = %d, want %d\n",
              __FILE__, __LINE__, cases[i].id, got, cases[i].want);
      exit(1);
    }
  }
}

// mods.h promises "an empty or missing directory is success with no mods" -- exercised directly
// here because it's the n == 0 path through the candidate-name array (never grown past NULL),
// which is easy to get wrong (e.g. passing a NULL base pointer to qsort).
static void a_missing_mods_directory_is_not_a_failure(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char dir[600];
  snprintf(dir, sizeof dir, "%s/test_mods_root_does_not_exist", tmp);
  ASSERT(mods_init(dir, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

static void an_empty_mods_directory_has_no_mods(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char dir[600];
  snprintf(dir, sizeof dir, "%s/test_mods_root", tmp);
  char cmd[700];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
  ASSERT_EQ(system(cmd), 0);
  ASSERT(mods_init(dir, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// A NULL directory can never be the "no mods installed yet" case (that's what a nonexistent
// path means) -- it's a programming error in the caller, so it must return false rather than
// silently reporting zero mods.
static void mods_init_rejects_a_null_directory(void) {
  clear_mods();
  ASSERT(!mods_init(NULL, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

static void mods_init_rejects_an_empty_directory_string(void) {
  clear_mods();
  ASSERT(!mods_init("", true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// A mods directory that exists but can't be read (e.g. permissions) is a real problem the
// caller should hear about, distinct from "doesn't exist yet" -- opendir fails with EACCES
// here, not ENOENT, and mods_init must tell those apart rather than reporting "no mods" either
// way. Skipped when running as root, since permission bits don't block root's own access and
// the test would otherwise spuriously fail (or, worse, pass for the wrong reason).
static bool an_unreadable_mods_directory_is_a_failure(void) {
  if (geteuid() == 0) SKIP_TEST("running as root; directory permissions don't block access");
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char dir[600];
  snprintf(dir, sizeof dir, "%s/test_mods_root", tmp);
  char cmd[700];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
  ASSERT_EQ(system(cmd), 0);
  ASSERT_EQ(chmod(dir, 0), 0);
  bool ok = mods_init(dir, true);
  ASSERT_EQ(chmod(dir, 0700), 0);   // restore first, so clear_mods()'s rm -rf can still remove it
  ASSERT(!ok);
  mods_shutdown();
  return true;
}

static void a_mod_is_found_and_described(void) {
  clear_mods();
  const char *root = make_mod("hello",
      "id=hello\nname=Hello\nversion=1.0\nauthor=nobody\napi=1\ngames=ages,seasons\n",
      "-- nothing yet\n");
  ASSERT(mods_init(root, true));                       // true: Seasons
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "Hello"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_error(0), ""), 0);             // nothing to report for a healthy mod
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Task 6: a Lua error disables that mod for the session -- it does not remove it.
//
// The plan's own draft of this test ("a_broken_mod_is_skipped", asserting mods_count() == 1 with
// only the healthy mod counted) encoded the behaviour Task 5 happened to have, not what the
// approved spec actually asks for: "a Lua error disables that mod for the session, shows the
// failure in the status line, and writes detail to stderr -- the same treatment the archive
// errors already get." A player whose mod has a syntax error should see it listed as broken, not
// watch it vanish and wonder whether the install failed. So the real test here is the opposite of
// the plan's literal text: both mods must be counted, the broken one disabled with a reason, the
// healthy one unaffected.
// ---------------------------------------------------------------------------

// The direct case: one mod throws, one does not. Both stay in the list; only the broken one's
// mods_enabled() is false, and mods_error() names why. Critically, this also pins the *other*
// direction that a "disable everything on any error" implementation would still pass if left
// unchecked: the healthy mod's mods_enabled() must stay true.
static void a_broken_mod_is_disabled_not_removed(void) {
  clear_mods();
  const char *root = make_mod("ok", "id=ok\nname=Fine\napi=1\ngames=ages,seasons\n", "oracles.log('fine')\n");
  make_mod("bad", "id=bad\nname=Broken\napi=1\ngames=ages,seasons\n", "error('boom')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 2);                          // both: nothing vanished
  ASSERT_EQ(strcmp(mods_id(0), "bad"), 0);              // alphabetical: "bad" sorts before "ok"
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(mods_error(0), "boom") != NULL);
  ASSERT_EQ(strcmp(mods_id(1), "ok"), 0);
  ASSERT_EQ(strcmp(mods_name(1), "Fine"), 0);
  ASSERT(mods_enabled(1));                              // the other direction: not disabled too
  ASSERT_EQ(strcmp(mods_error(1), ""), 0);
  ASSERT(mods_last_log() && strstr(mods_last_log(), "fine") != NULL);  // the healthy mod really ran
  mods_shutdown();
}

// Breadth: the "listed, disabled" treatment must be uniform across different *kinds* of failure,
// not special-cased for a plain Lua error. Three mods: healthy, one that hangs (the instruction/
// deadline budget), and one that allocates without bound (the memory cap) -- both failure mods
// disabled with a plausible reason, the healthy one unaffected by either.
static void disabled_treatment_is_uniform_across_failure_kinds(void) {
  clear_mods();
  make_mod("aaa-ok", "id=aaa-ok\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-spin", "id=mid-spin\napi=1\ngames=ages,seasons\n", "while true do end\n");
  const char *root = make_mod("zzz-oom", "id=zzz-oom\napi=1\ngames=ages,seasons\n",
      "local t = {} while true do t[#t+1] = string.rep('x', 1000000) end\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-ok"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_error(0), ""), 0);
  ASSERT_EQ(strcmp(mods_id(1), "mid-spin"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT(mods_error(1)[0] != 0);
  ASSERT_BUDGET_OR_DEADLINE(mods_error(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-oom"), 0);
  ASSERT(!mods_enabled(2));
  ASSERT(strstr(mods_error(2), "memory") != NULL);
  mods_shutdown();
}

// Ordering: a disabled mod that sorts *before* a healthy one must not shift or hide the healthy
// mod's index -- the off-by-one an implementation storing only enabled mods (or compacting the
// array after a failure) would introduce. "aaa-broken" is first alphabetically; if it were ever
// skipped the way a non-mod folder is, "zzz-healthy" would land at index 0 instead of 1, and this
// is exactly the case that would catch it.
static void a_disabled_mod_before_a_healthy_one_does_not_shift_indices(void) {
  clear_mods();
  make_mod("aaa-broken", "id=aaa-broken\napi=1\ngames=ages,seasons\n", "error('broken first')\n");
  const char *root = make_mod("zzz-healthy9", "id=zzz-healthy9\napi=1\ngames=ages,seasons\n",
      "oracles.log('zzz ok')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 2);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-broken"), 0);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(mods_error(0), "broken first") != NULL);
  ASSERT_EQ(strcmp(mods_id(1), "zzz-healthy9"), 0);
  ASSERT(mods_enabled(1));
  ASSERT_EQ(strcmp(mods_error(1), ""), 0);
  ASSERT(mods_last_log() && strstr(mods_last_log(), "zzz ok") != NULL);
  mods_shutdown();
}

// mods_error's out-of-range contract mirrors mods_id/mods_name/mods_enabled: never NULL, "" for
// an index outside [0, mods_count()) regardless of what's actually loaded.
static void mods_error_rejects_out_of_range_indices(void) {
  clear_mods();
  const char *root = make_mod("erridx", "id=erridx\napi=1\ngames=ages,seasons\n", "error('x')\n");
  ASSERT(mods_init(root, true));
  int n = mods_count();
  ASSERT_EQ(n, 1);
  ASSERT_EQ(strcmp(mods_error(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_error(n), ""), 0);
  mods_shutdown();
}

// A mod written against a newer API than this build supports is a real, validly-packaged mod --
// the player actually installed something, and will reasonably expect to see it listed, not
// wonder whether the install failed. Task 6: unlike a malformed mod.ini, this is listed disabled
// rather than dropped, with a reason naming the api it wants.
static void a_newer_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("future", "id=future\nname=Future\napi=99\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "Future"), 0);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(mods_error(0), "99") != NULL);
  ASSERT(strstr(out, "future") != NULL);
  mods_shutdown();
}

// A mod that names only the other game is not loaded.
static void the_other_games_mod_is_skipped(void) {
  clear_mods();
  const char *root = make_mod("agesonly", "id=agesonly\nname=Ages Only\napi=1\ngames=ages\n", "\n");
  ASSERT(mods_init(root, true));                       // Seasons
  ASSERT_EQ(mods_count(), 0);
  ASSERT(mods_init(root, false));                      // Ages
  ASSERT_EQ(mods_count(), 1);
  mods_shutdown();
}

// The folder name is the canonical id. A mod.ini claiming a traversing id is refused outright --
// it never gets the chance to be used as a path component -- and nothing is ever written at the
// location the traversal points to, which is the actual hazard this guards against (Task 9's
// per-mod store write uses id as a path component).
static void a_traversing_ini_id_is_refused(void) {
  clear_mods();
  const char *root = make_mod("evilwrap", "id=../evil\nname=Evil\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);

  // "<root>/evilwrap/../evil" normalizes to "<root>/../evil", i.e. a sibling of the mods root
  // itself. Confirm no such directory got created by this scan.
  char traversal_target[700];
  snprintf(traversal_target, sizeof traversal_target, "%s/../evil", root);
  struct stat st;
  ASSERT(stat(traversal_target, &st) != 0);
  mods_shutdown();
}

// mod.ini's "id=" must match the folder it lives in. Letting them disagree would mean two
// different things both claim to be "the id" and a caller (e.g. a future store path) could pick
// either one.
static void a_mismatched_ini_id_is_refused(void) {
  clear_mods();
  const char *root = make_mod("realfolder", "id=notrealfolder\nname=X\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// The id/folder cross-check must be a full-string comparison, not a prefix match: a weakened
// check (e.g. strncmp with a short fixed length) would accept either of these, since both ids
// share the first two bytes with their folder name.
static void an_ini_id_that_is_a_prefix_of_the_folder_name_is_refused(void) {
  clear_mods();
  const char *root = make_mod("realfolderxyz", "id=realfolder\nname=X\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

static void a_folder_name_that_is_a_prefix_of_the_ini_id_is_refused(void) {
  clear_mods();
  const char *root = make_mod("realfolder", "id=realfolderxyz\nname=X\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// id is required, symmetrically with api: a mod package should declare its own identity rather
// than silently inheriting whatever folder a user happened to unpack or rename it into. The
// message must be actionable, so it's checked for naming both sides of the disagreement.
//
// mods_count() == 0 alone cannot prove this dedicated check is what ran: with no "id=" line,
// m.ini_id is simply the empty string, and the *separate* id-vs-folder mismatch check ("" !=
// "noid") would refuse the mod anyway even with the dedicated required-id check deleted
// (confirmed by mutation). So this checks for the specific "has no id=" wording, which only the
// dedicated check prints -- the generic mismatch message never says that, even though it also
// names "noid".
static void a_missing_id_is_refused(void) {
  clear_mods();
  const char *root = make_mod("noid", "name=NoId\napi=1\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "noid") != NULL);
  ASSERT(strstr(out, "has no id=") != NULL);
  mods_shutdown();
}

// A folder name outside [a-z0-9-] is skipped before its mod.ini is even opened.
static void a_badly_named_folder_is_skipped(void) {
  clear_mods();
  const char *root = make_mod("Bad_Name", "id=Bad_Name\nname=X\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// api is the compatibility gate: absence must refuse, not silently default to 0 and pass. This
// is now one of three independently-worded refusals (absent / unparseable-or-out-of-range /
// below-the-policy-minimum) rather than one shared "missing or non-numeric" sentinel, so each
// gets its own stderr-message check: a mods_count() == 0 assertion alone can't tell which of the
// three actually fired, since read_ini's api_present/api_parsed split and mods_init/load_one's
// separate >= 1 policy check all funnel to the same refusal outcome for most malformed inputs.
static void a_missing_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("noapi", "id=noapi\nname=NoApi\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "has no api=") != NULL);
  mods_shutdown();
}

// atoi("1x") is 1 and would incorrectly pass; strtol with endptr checking must not accept it.
// Must report "unparseable", specifically -- not the "absent" or "below minimum" wording, which
// read_ini's api_present (true here) and the separate >= 1 policy check (never reached, since
// api_parsed is false) would otherwise produce instead.
static void a_non_numeric_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("badapi", "id=badapi\nname=BadApi\napi=1x\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "unparseable or out-of-range") != NULL);
  mods_shutdown();
}

// api= with nothing after the '=' is a present-but-empty value, not a present-and-valid one --
// also reported as "unparseable", since no digits were consumed at all.
static void an_empty_api_value_is_refused(void) {
  clear_mods();
  const char *root = make_mod("emptyapi", "id=emptyapi\nname=EmptyApi\napi=\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "unparseable or out-of-range") != NULL);
  mods_shutdown();
}

// api=0 is a well-formed, in-range number -- read_ini's api_parsed is genuinely true here, so
// this is the one case that exercises the *policy* ">= 1" bound in mods_init/load_one, as
// opposed to read_ini's syntax/range checks above. It must report "below the minimum", not
// "unparseable" -- those used to be the same sentinel (-1) before the api_present/api_parsed
// split, which is exactly the bug the reviewer flagged: api=0 was reported as "missing or
// non-numeric", which is simply false for a well-formed "0".
static void an_api_of_zero_is_refused(void) {
  clear_mods();
  const char *root = make_mod("zeroapi", "id=zeroapi\nname=ZeroApi\napi=0\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "below the minimum of 1") != NULL);
  mods_shutdown();
}

// 2147483648 is exactly INT_MAX + 1. On a 64-bit-long host, strtol parses it as the real value
// (no overflow at the `long` level), so only the "v <= INT_MAX" check in read_ini rejects it. On
// an LLP64 target (MinGW/Windows, 32-bit long == int), strtol itself cannot represent this value
// and must clamp to LONG_MAX while setting errno = ERANGE -- which is exactly why read_ini
// checks errno rather than relying on the v <= INT_MAX comparison alone, which a clamped result
// would silently satisfy on that platform.
static void an_api_over_int_max_is_refused(void) {
  clear_mods();
  const char *root = make_mod("bigapi", "id=bigapi\nname=BigApi\napi=2147483648\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "unparseable or out-of-range") != NULL);
  mods_shutdown();
}

// Astronomically large, so even a 64-bit `long` overflows and strtol sets errno = ERANGE
// regardless of platform -- unlike the INT_MAX+1 case above, which depends on the host's long
// width to decide which specific guard catches it.
static void an_astronomically_large_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("hugeapi", "id=hugeapi\nname=HugeApi\napi=999999999999999999999999\ngames=ages,seasons\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "unparseable or out-of-range") != NULL);
  mods_shutdown();
}

// A mod.ini with no "name=" falls back to the (already-validated) folder name rather than
// shipping with an empty display name.
static void a_mod_without_a_name_falls_back_to_its_id(void) {
  clear_mods();
  const char *root = make_mod("noname", "id=noname\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "noname"), 0);
  mods_shutdown();
}

// Mods ship in downloaded zips, where CRLF line endings are the likely case (zip tooling on
// Windows, or an author who edited mod.ini there). Every key must still parse.
static void crlf_line_endings_are_tolerated(void) {
  clear_mods();
  const char *root = make_mod("crlf", "id=crlf\r\nname=Crlf\r\napi=1\r\ngames=ages,seasons\r\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "Crlf"), 0);
  mods_shutdown();
}

// A mod folder with no mod.ini at all (as opposed to one whose mod.ini fails to parse) must be
// skipped, not treated as present-and-empty.
//
// mods_count() == 0 alone cannot prove read_ini's fopen-failure path is what refused it: a mod
// with no mod.ini also has no id= and no api=, and both of *those* checks independently refuse
// it too (confirmed by mutation: making read_ini report success on a missing file still left
// mods_count() at 0, because the resulting all-defaults Mod has no id and an invalid api). So
// this checks for the specific "has no mod.ini" message.
static void a_folder_without_mod_ini_is_skipped(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[800];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/bare", root);
  snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
  ASSERT_EQ(system(cmd), 0);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "bare") != NULL);
  ASSERT(strstr(out, "has no mod.ini") != NULL);
  mods_shutdown();
}

// A mod that declares no game at all (or an unrecognised one) is a broken package, and must say
// so explicitly on stderr rather than being refused as an accidental side effect of some other
// check. (It is in fact also refused by the "not for this game" check below, since ages and
// seasons are both false either way -- which is exactly why this test must check the stderr
// message and not just mods_count().)
static void a_missing_games_is_refused(void) {
  clear_mods();
  const char *root = make_mod("nogames", "id=nogames\nname=NoGames\napi=1\n", "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "nogames") != NULL);
  ASSERT(strstr(out, "no supported game") != NULL);
  mods_shutdown();
}

// strstr-based matching would let "games=sages" match "ages" as a substring. Token matching
// must not.
static void a_substring_game_token_is_refused(void) {
  clear_mods();
  const char *root = make_mod("sagesmod", "id=sagesmod\nname=Sages\napi=1\ngames=sages\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  ASSERT(mods_init(root, false));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// Unlike a wholly-unrecognised games= value (refused outright, above), one bad token among
// otherwise-valid ones must not sink the whole mod: "games=ages,bogus" is Ages-only, not
// nothing, but the typo must still be visible on stderr -- a silent "ages,seaons" installs a
// mod that mysteriously never shows up in Seasons.
static void an_unrecognised_game_token_is_reported_but_the_valid_one_still_loads(void) {
  clear_mods();
  const char *root = make_mod("partlybogus", "id=partlybogus\nname=PartlyBogus\napi=1\ngames=ages,bogus\n", "\n");
  cap_root = root;
  cap_seasons = false;                                   // Ages: the valid token
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(strstr(out, "bogus") != NULL);

  cap_root = root;
  cap_seasons = true;                                     // Seasons: not named, so not loaded
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// Matches this project's ini conventions (settings_parse): leading whitespace, spaces around
// '=', and an indented comment are all tolerated; unlike settings_parse's %255s, interior
// spaces in a value (a mod's display name) survive.
//
// The comment contains an '=' on purpose, but -- despite an earlier version of this comment
// claiming otherwise -- that does NOT make this a safety-relevant test of the leading-
// whitespace-before-'#' check: that check is readability only, not a safety boundary. `key`
// always starts at the first non-whitespace byte, trim() never strips '#', so a comment's "key"
// (here, "# api" from "# api=99") can never collide with a real keyword -- deleting that check
// entirely still passes this test, confirmed by mutation. This test's actual job is just
// confirming the parser tolerates an indented comment with an '=' in it without side effects.
static void whitespace_and_comments_are_tolerated(void) {
  clear_mods();
  const char *root = make_mod("spacey",
      "  id = spacey  \n"
      "  name = Hello World  \n"
      "   # api=99 commented out, with leading whitespace before the '#'\n"
      "api=1\n"
      "games = ages , seasons \n",
      "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "Hello World"), 0);
  mods_shutdown();
}

// fgets with a bounded buffer must not let an over-long line's tail be reparsed as its own
// key=value pair. The injected "api=99" sits in the tail of a single very long "note=" line; if
// the tail leaked into a new line, it would override the real "api=1" declared earlier and the
// mod would be refused as wanting a newer API than this build supports.
//
// mods.c reads lines into a buffer of some fixed size (256 bytes today), so fgets fills it to
// one byte short of capacity before returning without having seen a newline. Rather than hard-
// coding a padding length tied to that exact number -- which, confirmed by mutation, survives
// simply changing the buffer size (e.g. to 512) alongside deleting the discard step, since the
// hard-coded split point no longer lands on "api=99" either way -- this sweeps the padding length
// over a wide range so that some iteration's split lands exactly on the "api=99" boundary for any
// plausible buffer size, catching the bug regardless of the implementation's exact buffer size.
static void an_overlong_line_does_not_leak_into_the_next_key(void) {
  clear_mods();
  for (int pad = 1; pad <= 600; pad++) {
    char padding[601];
    memset(padding, 'x', (size_t)pad);
    padding[pad] = 0;
    char ini[900];
    snprintf(ini, sizeof ini,
        "id=longline\nname=LongLine\napi=1\ngames=ages,seasons\nnote=%sapi=99\n", padding);
    const char *root = make_mod("longline", ini, "\n");
    ASSERT(mods_init(root, true));
    if (mods_count() != 1) {
      fprintf(stderr, "FAIL %s:%d: an overlong line leaked into the next key at padding length %d\n",
              __FILE__, __LINE__, pad);
      exit(1);
    }
    mods_shutdown();
  }
}

// A stray regular file sitting in the mods directory (not a mod folder at all) must be skipped
// without disturbing discovery of the real mods around it, and must say so explicitly on stderr.
// The stderr check matters here for the same reason as a_missing_games_is_refused above: a
// stray file named "strayfile" also fails the later "open its mod.ini" step (ENOTDIR), which
// alone already drives mods_count() to the same answer -- so a mods_count()-only assertion
// cannot tell "the dedicated not-a-directory check ran" apart from "it was deleted and the
// mod.ini open failure masked that". Confirmed by mutation: deleting the stat()/S_ISDIR check
// left mods_count() and mods_id(0) exactly as this test expects.
static void a_stray_file_is_skipped(void) {
  clear_mods();
  const char *root = make_mod("real", "id=real\nname=Real\napi=1\ngames=ages,seasons\n", "\n");
  char path[700];
  snprintf(path, sizeof path, "%s/strayfile", root);
  FILE *f = fopen(path, "wb");
  ASSERT(f);
  fputs("not a mod\n", f);
  fclose(f);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_id(0), "real"), 0);
  ASSERT(strstr(out, "strayfile") != NULL);
  ASSERT(strstr(out, "not a directory") != NULL);
  mods_shutdown();
}

// Task 5 will run each enabled mod's main.lua in this order, so it must be deterministic rather
// than following readdir's platform-dependent order. Folders are created out of lexical order
// here specifically to catch a sort that was never applied.
static void mods_load_in_alphabetical_order(void) {
  clear_mods();
  make_mod("zeta", "id=zeta\nname=Zeta\napi=1\ngames=ages,seasons\n", "\n");
  make_mod("alpha", "id=alpha\nname=Alpha\napi=1\ngames=ages,seasons\n", "\n");
  const char *root = make_mod("mid", "id=mid\nname=Mid\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "alpha"), 0);
  ASSERT_EQ(strcmp(mods_id(1), "mid"), 0);
  ASSERT_EQ(strcmp(mods_id(2), "zeta"), 0);
  mods_shutdown();
}

// The 33rd valid mod must not vanish silently: the cap still applies (so loading stays bounded),
// but it is the 33rd one specifically that is dropped, not just "scanning stopped early" -- a
// distinction the previous version of this test (33 valid mods and nothing else) could not make,
// since the two read the same either way. Two invalid entries -- a bad folder name and a stray
// file -- are seeded ahead of the valid ones in sort order (uppercase 'B' and digit '0' both sort
// before lowercase 'm' in the alphabetical scan order), so a cap that's really "the 32nd directory
// entry, valid or not" would consume a slot on one of them and either miscount or load "m00" into
// slot 1 instead of slot 0. The "N dropped" stderr line is also checked, since nothing did before.
static void loading_stops_at_mods_max(void) {
  clear_mods();
  const char *root = make_mod("Bad_Name", "id=Bad_Name\napi=1\ngames=ages,seasons\n", "\n");
  char stray_path[700];
  snprintf(stray_path, sizeof stray_path, "%s/0alsostray", root);
  FILE *sf = fopen(stray_path, "wb");
  ASSERT(sf);
  fputs("not a mod\n", sf);
  fclose(sf);
  for (int i = 0; i < MODS_MAX + 1; i++) {
    char id[16];
    snprintf(id, sizeof id, "m%02d", i);
    char ini[128];
    snprintf(ini, sizeof ini, "id=%s\napi=1\ngames=ages,seasons\n", id);
    root = make_mod(id, ini, "\n");
  }
  cap_root = root;
  cap_seasons = true;
  char out[8192];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), MODS_MAX);
  ASSERT_EQ(strcmp(mods_id(0), "m00"), 0);               // the invalid entries consumed no slot
  ASSERT(strstr(out, "1 mod(s) dropped") != NULL);
  mods_shutdown();
}

// Task 5 will index these accessors from Lua with an integer the mod itself chooses, so `i` is
// hostile input, not a programmer mistake -- these pin the out-of-range contract with no mods
// loaded at all. mods_id/mods_name must never read mods[i] for i outside [0, count), and
// mods_enabled must never report a disabled-by-default index as enabled.
static void accessors_reject_out_of_range_indices_with_no_mods_loaded(void) {
  clear_mods();
  ASSERT(mods_init("/this/path/does/not/exist/for/test_mods", true));
  ASSERT_EQ(mods_count(), 0);
  ASSERT_EQ(strcmp(mods_id(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_id(0), ""), 0);
  ASSERT_EQ(strcmp(mods_name(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_name(0), ""), 0);
  ASSERT(!mods_enabled(-1));
  ASSERT(!mods_enabled(0));
  mods_shutdown();
}

// Same contract, but with count > 0: this is the case the reviewer actually broke under ASan
// (33 valid mods, i <= count mutated in) -- mods_id(mods_count()), one past the last valid
// index, must come back "" rather than reading mods[count], which is one element past the end
// of a MODS_MAX-sized array and therefore a real global-buffer-overflow once count == MODS_MAX.
static void accessors_reject_out_of_range_indices_after_a_successful_load(void) {
  clear_mods();
  const char *root = make_mod("onlyone", "id=onlyone\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  int n = mods_count();
  ASSERT(n > 0);
  ASSERT_EQ(strcmp(mods_id(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_id(n), ""), 0);
  ASSERT_EQ(strcmp(mods_name(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_name(n), ""), 0);
  ASSERT(!mods_enabled(-1));
  ASSERT(!mods_enabled(n));
  mods_shutdown();
}

// Both existing two-call tests (the_other_games_mod_is_skipped, a_substring_game_token_is_refused)
// happen to end on a call that assigns mods_count() itself, so neither pins mods_init's
// reset-on-entry: "drop the mods_shutdown() call" and "make mods_shutdown a no-op" both survive
// against them. This instead loads a real mod, then calls mods_init with a directory that makes
// it fail early (NULL), and checks that the *previous* successful load's state didn't survive.
static void mods_init_resets_state_even_when_it_then_fails(void) {
  clear_mods();
  const char *root = make_mod("resetme", "id=resetme\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_init(NULL, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// strchr finds the *first* '=' on the line; strrchr (the classic ini off-by-one) would find the
// last one instead, giving key "name=a=b" here instead of "name", which would fall through to
// "no known key" and silently leave the name defaulted to the folder id. This mod.ini's name
// value is kept whole, '=' signs included.
static void a_value_containing_equals_signs_is_kept_whole(void) {
  clear_mods();
  const char *root = make_mod("eqmod", "id=eqmod\napi=1\ngames=ages,seasons\nname=a=b=c\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_name(0), "a=b=c"), 0);
  mods_shutdown();
}

// "agesx" is a superset of "ages", not a token that equals it. A weakened per-token match (e.g.
// strncmp(tok, "ages", 2)) would accept this; exact strcmp must not. Mirrors the ini id/folder
// cross-check's own prefix tests above -- game tokens deserve the same rigour.
static void a_games_token_that_is_a_superset_of_ages_is_refused(void) {
  clear_mods();
  const char *root = make_mod("agesxmod", "id=agesxmod\napi=1\ngames=agesx\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  ASSERT(mods_init(root, false));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// Each "games=" line resets ages/seasons before reparsing its own tokens, so the *last* games=
// line in the file wins outright rather than the two lines accumulating together.
static void duplicate_games_lines_let_the_last_one_win(void) {
  clear_mods();
  const char *root = make_mod("dupgames", "id=dupgames\napi=1\ngames=ages\ngames=seasons\n", "\n");
  ASSERT(mods_init(root, false));                        // Ages: overridden by the later line
  ASSERT_EQ(mods_count(), 0);
  ASSERT(mods_init(root, true));                         // Seasons: the line that actually wins
  ASSERT_EQ(mods_count(), 1);
  mods_shutdown();
}

// ".hidden" and ".DS_Store" (the latter a near-certainty in any mods folder unzipped on macOS)
// are skipped before mods_id_valid even runs on them, purely to keep the scan quiet: both would
// fail the charset check anyway (mods_id_valid rejects '.'), so this buys silence, not
// correctness. Checked by asserting stderr never names either one, not just that the real mod
// still loads.
static void dotfiles_in_the_mods_folder_are_silently_ignored(void) {
  clear_mods();
  const char *root = make_mod("real2", "id=real2\napi=1\ngames=ages,seasons\n", "\n");
  char cmd[800];
  snprintf(cmd, sizeof cmd, "mkdir -p '%s/.hidden' && touch '%s/.DS_Store'", root, root);
  ASSERT_EQ(system(cmd), 0);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(strstr(out, ".hidden") == NULL);
  ASSERT(strstr(out, ".DS_Store") == NULL);
  mods_shutdown();
}

// A folder name of exactly MOD_FIELD_LEN (64) bytes must be refused explicitly, with its own
// message -- not merely as an accidental side effect of the id truncating to 63 bytes and then
// mismatching mod.ini's own (also truncated, but to a different degree depending on the field)
// id= value. Today that incidental mismatch would also refuse it, but a later relaxation of
// "mod.ini must declare id=" (ini_id becoming optional again) would silently remove that
// incidental protection while leaving two 64+ byte folders sharing a 63-byte prefix free to
// collide on a single truncated id -- and, from Task 9 on, a single store path.
static void a_64_char_folder_name_is_refused(void) {
  clear_mods();
  char id[65];
  memset(id, 'a', 64);
  id[64] = 0;
  char ini[200];
  snprintf(ini, sizeof ini, "id=%s\napi=1\ngames=ages,seasons\n", id);
  const char *root = make_mod(id, ini, "\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "byte id limit") != NULL);
  mods_shutdown();
}

// A mod.ini value containing a CR and an ANSI "clear line" escape could otherwise forge a
// terminal message once printed raw -- e.g. "id=victim\r\x1b[2K\rmods: 'victim' loaded OK (api
// 1)" overwrites the current line with a fake success report. escape_into() must turn every
// non-printable byte into a literal \xHH sequence, both in this process's own stderr output and
// in whatever mods_name()/mods_id() hand back to a future caller (a mod list UI, say).
static void hostile_bytes_in_a_value_are_escaped(void) {
  clear_mods();
  const char *ini = "id=escapetest\napi=1\ngames=ages,seasons\n"
                     "name=victim\r\x1b[2K\rFAKE SUCCESS\n";
  const char *root = make_mod("escapetest", ini, "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  const char *got = mods_name(0);
  for (const unsigned char *p = (const unsigned char *)got; *p; p++)
    ASSERT(*p >= 0x20 && *p < 0x7f);                     // no raw control byte survived
  ASSERT(strstr(got, "\\x0d") != NULL);                  // the CR became visible text instead
  ASSERT(strstr(got, "\\x1b") != NULL);                  // and so did the ESC
  mods_shutdown();
}

// A mod.ini containing an embedded NUL byte must be refused outright, not filtered.
//
// This payload is constructed to actually *work* as smuggling if the fix were removed --
// checked by mutation below, not just asserted -- rather than merely containing a NUL
// somewhere incidental. The attack line is exactly 255 bytes of "games=ages" + NUL + padding
// (255 is mods.c's fgets fill size: 256-byte buffer, minus 1 for the NUL terminator fgets
// itself appends), with no real newline anywhere in those 255 bytes. Unpatched, this defeats
// read_logical_line's overlong-line discard: strlen(buf) stops at the embedded NUL and reports
// a short apparent length, so the "buffer is exactly full with no newline seen" check reads
// false even though fgets genuinely filled the buffer -- so the discard-the-rest-of-the-line
// step never runs, and the file position is left sitting at byte 255 of this line rather than
// drained to the next real line boundary. The very next read then resumes exactly at a second,
// otherwise invisible "games=seasons\n" line, which overrides the visible "games=ages" the file
// appears to declare -- so the mod would load for Seasons despite its visible text saying
// Ages-only. Confirmed with contains_nul() deleted: the payload below does exactly this
// (mods_count() == 1 for Seasons). With the fix, none of that mechanism gets a chance to run:
// any NUL anywhere in mod.ini refuses the whole file outright, for every game.
static void a_nul_byte_in_mod_ini_is_refused(void) {
  clear_mods();
  unsigned char ini[512];
  size_t len = 0;
  memcpy(ini + len, "id=nulmod\napi=1\n", 16); len += 16;  // each its own short, ordinary line
  size_t line_start = len;
  memcpy(ini + len, "games=ages", 10); len += 10;
  ini[len++] = 0;                                          // the smuggling NUL
  size_t written_on_this_line = len - line_start;          // "games=ages" + NUL == 11 bytes
  size_t padding = 255 - written_on_this_line;
  memset(ini + len, 'x', padding); len += padding;         // pads this physical line to exactly
                                                             // 255 bytes with no real newline in it
  memcpy(ini + len, "games=seasons\n", 14); len += 14;      // the hidden, overriding line
  const char *root = make_mod_bytes("nulmod", ini, len);

  cap_root = root;
  cap_seasons = true;                                      // Seasons: must NOT load
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  ASSERT(strstr(out, "NUL") != NULL);

  cap_root = root;
  cap_seasons = false;                                     // Ages: must NOT load either
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// Control: the exact same visible text, with no NUL at all, must load normally -- proving the
// refusal above is specifically about the embedded NUL, not some other mistake in that payload.
static void the_same_ini_without_a_nul_byte_loads_fine(void) {
  clear_mods();
  const char *root = make_mod("nonulmod", "id=nonulmod\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  mods_shutdown();
}

static volatile int mods_test_alarm_fired;
static void mods_test_alarm_handler(int sig) {
  (void)sig;
  mods_test_alarm_fired = 1;
  // Print directly with write(), not fprintf: inside a signal handler, stdio is not safe to
  // call (fprintf may be mid-buffer-flush when the signal lands), and _exit (not exit) skips
  // atexit/stdio teardown for the same reason. Write to g_real_stderr_fd, not the literal fd 2,
  // whenever capture_stderr has one saved -- see its comment for why: fd 2 may currently be
  // redirected into capture_stderr's temp file, and writing this diagnostic there instead of
  // the real terminal would make a genuine hang fail completely silently (confirmed before this
  // fix: the message ended up trapped in an unlinked temp file, and the process just vanished).
  const char msg[] = "FAIL: mods_init hung (did not return within the timeout)\n";
  int fd = g_real_stderr_fd >= 0 ? g_real_stderr_fd : 2;
  write(fd, msg, sizeof msg - 1);
  _exit(1);
}

// A FIFO as mod.ini must not block fopen() forever waiting for a writer that will never come.
// Wrapped in a real alarm(), not just an assumption that the fix works, so a regression actually
// fails this test (and kills the whole binary promptly) instead of hanging CI indefinitely.
static bool a_fifo_mod_ini_does_not_hang(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[800];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/fifomod", root);
  snprintf(cmd, sizeof cmd, "mkdir -p '%s' && mkfifo '%s/mod.ini'", dir, dir);
  if (system(cmd) != 0) SKIP_TEST("mkfifo not available on this filesystem");
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(5);
  bool ok = mods_init(root, true);
  alarm(0);
  ASSERT(ok);
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
  return true;
}

// A symlink to a character device (the canonical "mod.ini -> /dev/zero" payload: an infinite
// NUL stream) must not loop forever in fgets, which would never see a newline. stat() follows
// the symlink to the device's real mode (S_ISCHR, not S_ISREG), so the same regular-file check
// that stops the FIFO case above also stops this one before fopen is ever called.
static bool a_symlink_to_a_character_device_does_not_hang(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[800];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/zeromod", root);
  snprintf(cmd, sizeof cmd, "mkdir -p '%s' && ln -sf /dev/zero '%s/mod.ini'", dir, dir);
  if (system(cmd) != 0) SKIP_TEST("/dev/zero not available on this machine");
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(5);
  bool ok = mods_init(root, true);
  alarm(0);
  ASSERT(ok);
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
  return true;
}

// A directory literally named "mod.ini" must be refused with the same wording as a missing
// mod.ini, not read as if it were an empty file -- which is what fopen() on a directory does on
// macOS (producing a confusing "has no id=" there, while Linux's fopen just fails outright for
// the same input). The stat()-based S_ISREG check added for the FIFO/symlink hazards above
// unifies this across platforms as a side effect.
static void a_directory_named_mod_ini_is_refused(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[800];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/dirmod", root);
  snprintf(cmd, sizeof cmd, "mkdir -p '%s/mod.ini'", dir);
  ASSERT_EQ(system(cmd), 0);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 0);
  // Specifically not "has no mod.ini": the file exists, it is just not a regular file, and an
  // author told the former would go looking for a missing file that is right there.
  ASSERT(strstr(out, "not a regular file") != NULL);
  ASSERT(strstr(out, "has no mod.ini") == NULL);
  mods_shutdown();
}

// Task 5: main.lua runs at load, and the oracles table is there.
static void main_lua_runs(void) {
  clear_mods();
  const char *root = make_mod("logger", "id=logger\nname=Logger\napi=1\ngames=ages,seasons\n",
      "oracles.log('loaded ok')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "loaded ok"));
  mods_shutdown();
}

// Every binding the host registers is discoverable, which is what MODDING.md is checked against.
static void the_api_registry_lists_itself(void) {
  int n = mods_api_count();
  ASSERT(n > 0);
  bool saw_log = false;
  for (int i = 0; i < n; i++)
    if (!strcmp(mods_api_name(i), "log")) saw_log = true;
  ASSERT(saw_log);
}

// Correction B: mods_shutdown must not let a stale oracles.log() message outlive the mod that
// logged it. A mods_count()-only check can't see this bug at all; only reading mods_last_log()
// itself, after an unrelated, silent mod has since loaded, can.
static void mods_last_log_is_cleared_by_shutdown(void) {
  clear_mods();
  const char *root = make_mod("chatty", "id=chatty\napi=1\ngames=ages,seasons\n", "oracles.log('loaded ok')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "loaded ok") != NULL);
  mods_shutdown();
  ASSERT(mods_last_log() == NULL);

  clear_mods();
  const char *root2 = make_mod("silent", "id=silent\napi=1\ngames=ages,seasons\n", "-- says nothing\n");
  ASSERT(mods_init(root2, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log() == NULL);           // must not inherit "loaded ok" from the previous mod
  mods_shutdown();
}

// Correction A, instruction half: a `while true do end` main.lua never returns on its own --
// there is nothing for an ordinary pcall to catch, unlike a Lua-level `error("boom")` -- so
// without mod_resume_budgeted's LUA_MASKCOUNT hook (mod_instruction_limit_hook) this would hang
// mods_init forever.
// Two healthy mods, sorted alphabetically on either side of the hang, make sure a mutant that
// aborted the *whole scan* on any single failure (rather than just disabling the one bad mod)
// still fails this test. Timed, not just asserted to terminate: "aborts eventually" is not the
// bar -- the budget must be small enough to abort within about a tenth of a second on this
// machine, so even a CI box several times slower stays well under the 2s bound here.
//
// Task 6: the hung mod is now listed disabled (mid-hang occupies index 1, mods_enabled(1) false)
// rather than vanishing from the count -- it is a real mod for this game, just one whose own
// payload never returned. The two healthy mods either side keep their own slots and stay enabled,
// pinning that a disabled mod in the middle doesn't shift or hide its neighbours' indices.
static void a_hanging_mod_is_disabled_and_other_mods_still_load(void) {
  clear_mods();
  make_mod("aaa-healthy", "id=aaa-healthy\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-hang", "id=mid-hang\napi=1\ngames=ages,seasons\n", "while true do end\n");
  const char *root = make_mod("zzz-healthy", "id=zzz-healthy\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  // A real alarm, not just an assumption the budget works: a regression here must fail promptly
  // and loudly (killing the whole binary with a clear message), never wedge CI for the ctest
  // default 1500s timeout -- unconditional regardless of MODS_TEST_TIMING, since this is what
  // actually catches a hang rather than merely a slow pass.
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-hang"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-hang") != NULL);
  // Not a fixed "instruction budget" check any more: on a sufficiently busy host even this cheap
  // loop can take long enough in wall-clock terms to be cut off by the deadline instead -- see
  // ASSERT_BUDGET_OR_DEADLINE's own comment.
  ASSERT_BUDGET_OR_DEADLINE(out);
  ASSERT_BUDGET_OR_DEADLINE(mods_error(1));                // the stored reason, not just stderr
  mods_shutdown();
}

// Correction A, memory half: an allocation loop with no infinite-loop shape at all (each iteration
// terminates fine; it's the total that's unbounded) must be caught by the memory cap specifically,
// not the instruction budget -- this loop runs only a handful of VM instructions per iteration, so
// it hits MOD_MEM_CAP (8 MiB, 1 MiB per iteration) in about 8 iterations, nowhere near
// MOD_LOAD_INSTRUCTION_BUDGET's 50 million. Same "two healthy mods either side, and it's fast"
// shape as the instruction-budget test above, for the same reasons.
//
// Reviewer correction: despite an earlier version of this comment claiming otherwise, this does
// NOT exercise mod_lua_alloc's realloc-widening branch -- each string.rep('x', 1000000) call
// allocates a brand-new string object, always the ptr == NULL path, identical in kind to the
// single-huge-allocation test below; only `t`'s own array part grows via realloc here, and it
// never grows past a handful of elements (8 iterations), nowhere near the cap through that path.
// array_growth_is_refused_via_the_realloc_path, further down, is what actually pins the realloc
// branch specifically.
//
// Task 6: as with the hanging mod above, mid-oom is now listed disabled (index 1) rather than
// dropped -- a resource-exhaustion failure gets the same list-and-disable treatment as an
// instruction-budget one, which is the point of pairing this test with the hanging-mod test above
// rather than relying on just one failure shape to prove the new contract is uniform.
static void an_unbounded_allocation_mod_is_disabled_and_other_mods_still_load(void) {
  clear_mods();
  make_mod("aaa-healthy2", "id=aaa-healthy2\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-oom", "id=mid-oom\napi=1\ngames=ages,seasons\n",
      "local t = {} while true do t[#t+1] = string.rep('x', 1000000) end\n");
  const char *root = make_mod("zzz-healthy2", "id=zzz-healthy2\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy2"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-oom"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy2"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-oom") != NULL);
  ASSERT(strstr(out, "not enough memory") != NULL);
  ASSERT(strstr(mods_error(1), "not enough memory") != NULL);
  mods_shutdown();
}

// Correction F: a mod whose zip forgot main.lua gets a specific, actionable message -- not
// whatever generic complaint lauxlib's own file-open failure happens to produce. Task 6: this is
// now a mod that is listed, disabled, rather than dropped -- it declared a real, valid id/api/
// games and the player installed something real; "forgot to ship main.lua" is a defect in that
// mod's own payload, the same category as a Lua error, not a reason to make it vanish.
static void a_mod_with_no_main_lua_is_refused_clearly(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[800];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/nomain", root);
  snprintf(cmd, sizeof cmd,
      "mkdir -p '%s' && printf 'id=nomain\\napi=1\\ngames=ages,seasons\\n' > '%s/mod.ini'", dir, dir);
  ASSERT_EQ(system(cmd), 0);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT_EQ(strcmp(mods_id(0), "nomain"), 0);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "nomain") != NULL);
  ASSERT(strstr(out, "has no main.lua") != NULL);
  ASSERT(strstr(mods_error(0), "main.lua") != NULL);
  mods_shutdown();
}

// Hazard found while implementing Task 5, not in the original plan: the exact same FIFO-blocks-
// fopen-forever hazard Task 4 fixed for mod.ini applies identically to main.lua, since lauxlib's
// own file loader is just another fopen/fread loop underneath with no protection of its own.
// Wrapped in a real alarm(), like the mod.ini version, so a regression actually fails this test
// (and kills the whole binary promptly) instead of hanging CI indefinitely.
static bool a_fifo_main_lua_does_not_hang(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[900];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/fifolua", root);
  snprintf(cmd, sizeof cmd,
      "mkdir -p '%s' && printf 'id=fifolua\\napi=1\\ngames=ages,seasons\\n' > '%s/mod.ini' && mkfifo '%s/main.lua'",
      dir, dir, dir);
  if (system(cmd) != 0) SKIP_TEST("mkfifo not available on this filesystem");
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(5);
  bool ok = mods_init(root, true);
  alarm(0);
  ASSERT(ok);
  // Task 6: a real, validly-declared mod whose main.lua just happens to be a FIFO is listed
  // disabled, not dropped -- the hang-prevention this test exists for is unaffected either way,
  // since the stat() check runs before fopen is ever attempted regardless of the outcome.
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  mods_shutdown();
  return true;
}

// Same hazard, the symlink-to-character-device shape: main.lua -> /dev/zero would stream forever
// with no newline for lauxlib's loader to stop on, exactly like mod.ini's own version of this.
static bool a_symlink_main_lua_to_a_character_device_does_not_hang(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[900];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/zerolua", root);
  snprintf(cmd, sizeof cmd,
      "mkdir -p '%s' && printf 'id=zerolua\\napi=1\\ngames=ages,seasons\\n' > '%s/mod.ini' && ln -sf /dev/zero '%s/main.lua'",
      dir, dir, dir);
  if (system(cmd) != 0) SKIP_TEST("/dev/zero not available on this machine");
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(5);
  bool ok = mods_init(root, true);
  alarm(0);
  ASSERT(ok);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  mods_shutdown();
  return true;
}

// Correction D: the error text a failed main.lua produces is attacker-controlled (main.lua's own
// error() argument), exactly like a hostile mod.ini value, and must go through the same escaper
// before it ever reaches a terminal -- otherwise an embedded CR + ANSI "clear line" escape could
// forge a fake success line the moment this process's own diagnostic is printed raw.
static void a_hostile_lua_error_is_escaped_before_printing(void) {
  clear_mods();
  const char *root = make_mod("badlua", "id=badlua\napi=1\ngames=ages,seasons\n",
      "error('boom\\r\\27[2KFAKE SUCCESS')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  // Task 6: 'badlua' is listed, disabled, rather than dropped -- and mods_error(0) must be
  // escaped exactly the same way the stderr line already is, since it is the same attacker-
  // controlled text, now also handed to a status line rather than only ever printed once.
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "badlua") != NULL);
  for (const char *p = out; *p; p++)
    ASSERT(!((unsigned char)*p < 0x20 && *p != '\n'));    // no raw control byte survived
  ASSERT(strstr(out, "\\x0d") != NULL);                    // the CR became visible text instead
  ASSERT(strstr(out, "\\x1b") != NULL);                    // and so did the ESC
  const char *err = mods_error(0);
  for (const unsigned char *p = (const unsigned char *)err; *p; p++)
    ASSERT(*p >= 0x20 && *p < 0x7f);                        // no raw control byte in the stored reason
  ASSERT(strstr(err, "\\x0d") != NULL);
  ASSERT(strstr(err, "\\x1b") != NULL);
  mods_shutdown();
}

// Correction E: oracles.log() must not silently truncate at an embedded NUL the way
// luaL_checkstring's bare `const char *` would. The choice made in api.c is to escape rather than
// truncate -- mods_escape_into renders a NUL as a literal "\x00", so the byte after it ('b' here)
// is not silently lost.
static void oracles_log_with_an_embedded_nul_is_escaped_not_truncated(void) {
  clear_mods();
  const char *root = make_mod("nullog", "id=nullog\napi=1\ngames=ages,seasons\n", "oracles.log('a\\0b')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "a\\x00b") != NULL);
  mods_shutdown();
}

// Hazard found while implementing Task 5, not in the original plan: debug.sethook() lets Lua code
// remove any hook set from C, including mod_instruction_limit_hook. A mod that calls it before
// looping forever would otherwise disarm the very protection this task exists to add -- the
// instruction budget would never fire, so this would hang exactly like a_hanging_mod above but
// with one extra line of Lua defeating the defence outright. Confirmed empirically before the fix
// (see mods.c's mod_open_libs_hardened): unpatched, this exact script ran to completion
// instead of aborting. Paired with healthy mods either side for the same reason as the other
// budget tests above, and timed the same way.
static void a_mod_cannot_disarm_its_own_instruction_budget_via_debug_sethook(void) {
  clear_mods();
  make_mod("aaa-healthy3", "id=aaa-healthy3\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-disarm", "id=mid-disarm\napi=1\ngames=ages,seasons\n",
      "debug.sethook()\nwhile true do end\n");
  const char *root = make_mod("zzz-healthy3", "id=zzz-healthy3\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy3"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-disarm"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy3"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-disarm") != NULL);
  mods_shutdown();
}

// Hazard, continued: "debug" must be unreachable outright, not merely harmless to call once --
// otherwise a mod could still read debug.gethook()/debug.getinfo() or any other piece of it.
// require() checks package.loaded before searching the filesystem, so this also pins that
// mod_open_libs_hardened clears package.loaded.debug, not just the global: nil-ing only the
// global would leave require('debug') able to resurrect it from package.loaded.
static void the_debug_library_is_unreachable_by_global_or_require(void) {
  clear_mods();
  const char *root = make_mod("nodebug",
      "id=nodebug\napi=1\ngames=ages,seasons\n",
      "if debug ~= nil then error('debug global is reachable') end\n"
      "local ok = pcall(require, 'debug')\n"
      "if ok then error('require(\"debug\") succeeded') end\n"
      "oracles.log('debug is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "debug is unreachable") != NULL);
  mods_shutdown();
}

// CRITICAL fix (spec review): a plain luaL_error-raising count hook is catchable by the mod's own
// pcall, which is core Lua and squarely inside Task 7's planned sandbox (string/table/math), so
// it can never be withheld the way "debug" was above. lua_sethook's countdown simply restarts
// every time the error is swallowed -- unlimited CPU, one budget at a time -- unless the hook
// unwinds via lua_yield instead, which no pcall/xpcall can intercept (it yields the *thread*, not
// an error value, straight out to the lua_resume boundary in mod_resume_budgeted, in the host).
// Same "two healthy mods either side, and it's fast" shape as the other budget tests, for the
// same reasons; same alarm() safety net, since getting this wrong hangs the whole process.
static void a_pcall_wrapped_spinner_is_refused_promptly(void) {
  clear_mods();
  make_mod("aaa-healthy4", "id=aaa-healthy4\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-pcall-spin", "id=mid-pcall-spin\napi=1\ngames=ages,seasons\n",
      "while true do pcall(function() while true do end end) end\n");
  const char *root = make_mod("zzz-healthy4", "id=zzz-healthy4\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy4"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-pcall-spin"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy4"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-pcall-spin") != NULL);
  ASSERT_BUDGET_OR_DEADLINE(out);
  mods_shutdown();
}

// CRITICAL fix, second route (as first designed): coroutine.resume is itself implemented by
// calling lua_resume on the nested coroutine's own thread; a yield there is indistinguishable, to
// coroutine.resume's caller, from an ordinary, intentional coroutine.yield() -- it just returns
// `true`, and the outer thread's own lua_resume (the one mod_resume_budgeted drives) never sees
// anything happen. Resuming the *same* coroutine again would, without the shared-budget fix in
// mods.c, buy it a full fresh instruction budget every time -- unbounded in principle, since the
// outer loop's own countdown only measures its own handful of instructions per iteration, never
// the arbitrarily slow work hidden inside each resume. Measured against the naive
// per-thread-only version of this fix while designing it: over three minutes to regain control.
//
// A later spec review found a second, worse bypass of this exact shape -- minting a *fresh*
// coroutine every iteration, rather than resuming the same one, defeats the shared budget too
// (see mod_open_libs_sandboxed_partially's comment on "coroutine" for why) -- which is why
// `coroutine` is now withheld from mods entirely rather than left reachable with the
// shared-budget fix alone. That makes this exact script fail on its very first line instead of
// ever reaching the loop; this test is kept anyway as a tripwire, same alarm() and all, so that
// if `coroutine` is ever reopened to mods without also closing the fresh-thread hole, this goes
// back to actually exercising it rather than silently testing something else.
static void a_coroutine_resume_wrapped_spinner_is_refused_promptly(void) {
  clear_mods();
  make_mod("aaa-healthy5", "id=aaa-healthy5\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-coro-spin", "id=mid-coro-spin\napi=1\ngames=ages,seasons\n",
      "local co = coroutine.create(function() while true do end end)\n"
      "while true do coroutine.resume(co) end\n");
  const char *root = make_mod("zzz-healthy5", "id=zzz-healthy5\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy5"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-coro-spin"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy5"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-coro-spin") != NULL);
  ASSERT(strstr(out, "coroutine") != NULL);     // today: "nil value (global 'coroutine')"
  mods_shutdown();
}

// A third shape of the same route: the top-level chunk would finish normally (LUA_OK) once its
// single coroutine.resume call returns, with the spinner's own exhaustion fully absorbed and
// invisible to the status code alone, if `coroutine` were reachable at all -- mod_resume_budgeted's
// `exhausted` out-parameter, not the status, would be what catches that. With `coroutine`
// withheld (see above), this fails on its first line instead; kept as the same kind of tripwire.
static void a_coroutine_resumed_only_once_still_counts_as_exhausted(void) {
  clear_mods();
  const char *root = make_mod("resumeonce", "id=resumeonce\napi=1\ngames=ages,seasons\n",
      "local co = coroutine.create(function() while true do end end)\n"
      "coroutine.resume(co)\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "resumeonce") != NULL);
  ASSERT(strstr(out, "coroutine") != NULL);     // today: "nil value (global 'coroutine')"
  mods_shutdown();
}

// Second spec review, CRITICAL: the shared ModBudget closes every bypass that *resumes an
// existing thread*, but not one that mints a *fresh* thread every iteration -- lua_newthread
// resets the new thread's own countdown to a full MOD_INSTRUCTION_CHECKIN regardless of how
// exhausted the shared total already is, and that exhaustion is only ever checked *inside* the
// hook, which Lua's own per-thread countdown (not us) decides when to invoke at all. Confirmed to
// hang past an 8-second hard alarm, isolated, before the fix: minting a fresh coroutine every
// outer-loop iteration lets each one buy its own free ride. The fix is mod_open_libs_sandboxed_
// partially withholding "coroutine" entirely (see its own comment for the full mechanism) --
// with no way to call coroutine.create at all, a mod has no way to mint a fresh thread, so this
// now fails on its very first iteration rather than ever looping. Wrapped in alarm() regardless,
// exactly like the other budget tests, so a regression here fails fast with a diagnostic.
static void a_fresh_coroutine_per_iteration_is_refused_promptly(void) {
  clear_mods();
  make_mod("aaa-healthy6", "id=aaa-healthy6\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-coro-mint", "id=mid-coro-mint\napi=1\ngames=ages,seasons\n",
      "while true do\n"
      "  local co = coroutine.create(function() while true do end end)\n"
      "  coroutine.resume(co)\n"
      "end\n");
  const char *root = make_mod("zzz-healthy6", "id=zzz-healthy6\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy6"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-coro-mint"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy6"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-coro-mint") != NULL);
  ASSERT(strstr(out, "coroutine") != NULL);     // today: "nil value (global 'coroutine')"
  mods_shutdown();
}

// Same hazard, the "many small threads" shape rather than "one big loop of threads": 50 fresh
// coroutines minted and immediately resumed per outer iteration, each one a separate free ride
// under the unfixed mechanism (more of them, not deeper nesting, despite the name) -- confirmed
// to hang past an 8-second hard alarm before the fix, same as above.
static void many_fresh_coroutines_per_iteration_are_refused_promptly(void) {
  clear_mods();
  make_mod("aaa-healthy7", "id=aaa-healthy7\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-coro-deep", "id=mid-coro-deep\napi=1\ngames=ages,seasons\n",
      "local function spin() while true do end end\n"
      "while true do\n"
      "  for i = 1, 50 do\n"
      "    local c = coroutine.create(spin)\n"
      "    coroutine.resume(c)\n"
      "  end\n"
      "end\n");
  const char *root = make_mod("zzz-healthy7", "id=zzz-healthy7\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy7"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-coro-deep"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy7"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-coro-deep") != NULL);
  ASSERT(strstr(out, "coroutine") != NULL);     // today: "nil value (global 'coroutine')"
  mods_shutdown();
}

// The third hazard shape: fresh threads minted from inside *nested* pcalls, via coroutine.wrap
// (coroutine.create plus an auto-resuming call wrapper) rather than coroutine.create/resume
// spelled out -- confirmed to hang past an 8-second hard alarm before the coroutine-withholding
// fix (minting a fresh thread every iteration, same underlying mechanism as the other two
// fixtures). With `coroutine` withheld, coroutine.wrap itself never runs -- but because this
// specific script wraps the access in pcall, the resulting "nil value" error is swallowed rather
// than ever escaping, so the outer infinite loop keeps spinning on pure Lua overhead regardless.
// This is exactly the shape that originally needed its own, wider timing bound: repeatedly
// raising and catching that error is 50-150x more wall-clock-expensive per VM instruction than a
// bare loop (measured 5-16s, worse under host load), which is precisely what
// MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS now exists to bound regardless of how expensive a mod's
// instructions turn out to be -- so this test needs no special treatment any more; it is
// expected to land at the same ~2-second deadline as every other budget test, via whichever of
// the two messages the deadline or instruction count happens to produce.
static void a_pcall_wrapped_coroutine_wrap_spinner_is_refused_promptly(void) {
  clear_mods();
  make_mod("aaa-healthy8", "id=aaa-healthy8\napi=1\ngames=ages,seasons\n", "oracles.log('aaa ok')\n");
  make_mod("mid-nested-pcall", "id=mid-nested-pcall\napi=1\ngames=ages,seasons\n",
      "while true do\n"
      "  pcall(function()\n"
      "    pcall(function()\n"
      "      coroutine.wrap(function() while true do end end)()\n"
      "    end)\n"
      "  end)\n"
      "end\n");
  const char *root = make_mod("zzz-healthy8", "id=zzz-healthy8\napi=1\ngames=ages,seasons\n", "oracles.log('zzz ok')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 3);
  ASSERT_EQ(strcmp(mods_id(0), "aaa-healthy8"), 0);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(strcmp(mods_id(1), "mid-nested-pcall"), 0);
  ASSERT(!mods_enabled(1));
  ASSERT_EQ(strcmp(mods_id(2), "zzz-healthy8"), 0);
  ASSERT(mods_enabled(2));
  ASSERT(strstr(out, "mid-nested-pcall") != NULL);
  // Unlike the other two fresh-thread fixtures above, this one's own coroutine.create access is
  // wrapped in pcall, so the "nil value (global 'coroutine')" error never escapes the inner
  // pcall at all -- withholding `coroutine` is what makes every iteration cheap-*ish* (a
  // nil-index error and two unwinds, not a real fresh thread), but still far more
  // wall-clock-expensive per VM instruction than a bare loop. That is exactly this fixture's
  // reason to exist: unlike every other test in this file, it must be caught by the *deadline*
  // specifically, not merely "the budget mechanism, whichever one" -- so this asserts the
  // message text directly rather than leaning on ASSERT_PROMPT's elapsed-time bound, which a
  // mutation disabling the deadline check does not reliably fail: on this machine, disabling it
  // still finishes (via the instruction count alone) in ~2.7s -- nowhere near the 5-16s this
  // shape measured under heavier host load, but still comfortably under any elapsed-time bound
  // loose enough to tolerate ordinary jitter. The message, not the clock, is what actually
  // changes the moment the deadline stops being what catches this.
  ASSERT(strstr(out, "cut off") != NULL);
  mods_shutdown();
}

// The explicit assertion the review asked for: if a later task re-introduces `coroutine` to a
// mod's environment without separately re-closing the fresh-thread hole above, this is the one
// test whose entire job is noticing that reintroduction immediately, rather than relying on the
// three hang-shaped tests above (which would, at that point, actually start hanging -- useful as
// a worst-case tripwire, but a much slower and less specific signal than this).
static void coroutine_is_nil_in_a_loaded_mod_environment(void) {
  clear_mods();
  const char *root = make_mod("nocoroutine", "id=nocoroutine\napi=1\ngames=ages,seasons\n",
      "if coroutine ~= nil then error('coroutine global is reachable') end\n"
      "oracles.log('coroutine is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "coroutine is unreachable") != NULL);
  mods_shutdown();
}

// Mirrors the_debug_library_is_unreachable_by_global_or_require: require() checks
// package.loaded before searching the filesystem, so nil-ing only the global would leave
// require('coroutine') able to resurrect it -- mod_open_libs_sandboxed_partially clears
// package.loaded.coroutine too, for exactly the reason it does for "debug".
static void the_coroutine_library_is_unreachable_by_global_or_require(void) {
  clear_mods();
  const char *root = make_mod("nocoroutine2",
      "id=nocoroutine2\napi=1\ngames=ages,seasons\n",
      "if coroutine ~= nil then error('coroutine global is reachable') end\n"
      "local ok = pcall(require, 'coroutine')\n"
      "if ok then error('require(\"coroutine\") succeeded') end\n"
      "oracles.log('coroutine is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "coroutine is unreachable") != NULL);
  mods_shutdown();
}

// Reviewer: the registry (mods_api_count/mods_api_name) is what a later docs-drift check
// compares MODDING.md against -- it never looks at the live `oracles` table. A binding installed
// via lua_pushcfunction/lua_setfield right next to some other code (the likeliest real path once
// a later task adds a binding next to, say, the overlay code, rather than editing api.c's array)
// would be invisible to both the registry and that doc check, while still being fully callable
// from main.lua -- exactly the failure mode the registry exists to prevent. Confirmed by the
// reviewer: a real oracles.undocumented() added this way passed the whole suite. This walks the
// *live* table with lua_next and checks its key set is exactly {mods_api_name(0..count-1)}: same
// cardinality, every key present, every name found -- closing both directions at once.
static void the_oracles_table_matches_the_registry_exactly(void) {
  lua_State *L = luaL_newstate();
  ASSERT(L);
  mods_api_open(L);
  lua_getglobal(L, "oracles");
  ASSERT(lua_istable(L, -1));

  int n = mods_api_count();
  ASSERT(n > 0);
  ASSERT(n <= 64);               // generous fixed bound for the `found` array below
  bool found[64] = {0};
  int live_count = 0;

  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    ASSERT_EQ(lua_type(L, -2), LUA_TSTRING);     // every key in `oracles` names a function slot
    // Task 8 correction: the registry must match what a mod actually sees, including that every
    // entry really is callable -- not merely that a key with the right name exists. Without
    // this, a future `oracles.draw_icon = {}` (a table, not a function) would satisfy every
    // check above while silently breaking any mod that calls it.
    ASSERT(lua_iscfunction(L, -1));
    const char *key = lua_tostring(L, -2);
    bool matched = false;
    for (int i = 0; i < n; i++) {
      if (!strcmp(mods_api_name(i), key)) { found[i] = true; matched = true; break; }
    }
    ASSERT(matched);           // every live key must be a name the registry actually lists
    live_count++;
    lua_pop(L, 1);             // pop value; leave key on top for lua_next's next call
  }
  ASSERT_EQ(live_count, n);    // same cardinality: nothing live is missing from the registry
  for (int i = 0; i < n; i++) ASSERT(found[i]);   // ...and nothing registered is missing live
  lua_close(L);
}

// Reviewer: deleting the fresh-allocation branch of mod_lua_alloc (`if (a->used + nsize > a->cap)
// return NULL` for ptr == NULL) survives the rest of the suite, since the existing memory-cap
// test only exercises the realloc path (table growth via many small string.rep calls). A single
// huge allocation request is always ptr == NULL, never a realloc of an existing block, so this
// pins that branch specifically: refused instantly, at negligible real memory cost (the cap check
// runs before malloc is ever called), rather than asking libc for ~300 MB first and hoping it
// fails -- which, on a machine with that much spare RAM, it might not, defeating the cap outright.
static void a_single_huge_allocation_is_refused_without_growing_first(void) {
  clear_mods();
  const char *root = make_mod("hugealloc", "id=hugealloc\napi=1\ngames=ages,seasons\n",
      "local ok, err = pcall(string.rep, 'x', 300000000)\n"
      "if ok then error('a single 300 MB allocation should have been refused') end\n"
      "oracles.log('huge allocation refused: ' .. tostring(err))\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "huge allocation refused") != NULL);
  mods_shutdown();
}

// Reviewer: deleting `a->used -= osize` on free in mod_lua_alloc survives the rest of the suite
// -- harmless today only because a mod's main.lua runs once and is then discarded, so a leaked
// `used` count never gets the chance to matter within a single run; once a later task reuses the
// same state every frame it would ratchet up monotonically under ordinary GC churn and produce
// spurious out-of-memory failures. This pins it directly: churn far more total bytes (20 MB, in
// 100 KB pieces) through alloc-then-drop-then-collect than the 8 MiB cap could ever hold at once,
// and require the mod to still load -- only possible if freed memory is actually credited back
// rather than `used` only ever growing.
static void repeated_allocate_and_free_does_not_ratchet_toward_the_cap(void) {
  clear_mods();
  const char *root = make_mod("churn", "id=churn\napi=1\ngames=ages,seasons\n",
      "for i = 1, 200 do\n"
      "  local junk = string.rep('x', 100000)\n"
      "  junk = nil\n"
      "  if i % 20 == 0 then collectgarbage() end\n"
      "end\n"
      "oracles.log('churned 20MB through an 8MB cap without hitting it')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "churned 20MB") != NULL);
  mods_shutdown();
}

// Reviewer: error({}) and bare error() must still write detail to stderr -- plain lua_tolstring
// returns NULL for a non-string error object, which would otherwise print "...: " with nothing
// after the colon. Checked for both a table (the common "structured error" pattern) and a bare
// error() call (which raises nil as the error object, per lbaselib.c's luaB_error).
static void a_table_error_object_still_produces_detail_on_stderr(void) {
  clear_mods();
  const char *root = make_mod("tableerr", "id=tableerr\napi=1\ngames=ages,seasons\n", "error({code = 42})\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                 // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  const char *tag = "failed to run main.lua: ";
  const char *p = strstr(out, tag);
  ASSERT(p != NULL);
  ASSERT(p[strlen(tag)] != '\n' && p[strlen(tag)] != 0);    // something follows the colon
  ASSERT(mods_error(0)[0] != 0);                            // the stored reason is non-empty too
  mods_shutdown();
}

static void a_bare_error_call_still_produces_detail_on_stderr(void) {
  clear_mods();
  const char *root = make_mod("bareerr", "id=bareerr\napi=1\ngames=ages,seasons\n", "error()\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  const char *tag = "failed to run main.lua: ";
  const char *p = strstr(out, tag);
  ASSERT(p != NULL);
  ASSERT(p[strlen(tag)] != '\n' && p[strlen(tag)] != 0);
  ASSERT(mods_error(0)[0] != 0);
  mods_shutdown();
}

// Reviewer: mods_api_name's bounds check can be deleted unnoticed by the rest of the suite.
static void mods_api_name_rejects_out_of_range_indices(void) {
  ASSERT_EQ(strcmp(mods_api_name(-1), ""), 0);
  ASSERT_EQ(strcmp(mods_api_name(mods_api_count()), ""), 0);
}

// Reviewer: l_log's fprintf to stderr can be deleted unnoticed -- mods_last_log() alone can't
// tell "the mod's message reached the status line" apart from "it also reached the terminal
// where a player might be watching the app's own stderr".
static void oracles_log_reaches_stderr_not_just_mods_last_log(void) {
  clear_mods();
  const char *root = make_mod("stderrlog", "id=stderrlog\napi=1\ngames=ages,seasons\n",
      "oracles.log('visible on stderr')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(strstr(out, "visible on stderr") != NULL);
  mods_shutdown();
}

// Reviewer: luaL_checklstring -> lua_tolstring survives and is genuinely unsafe (len is read
// uninitialised when the argument is nil, and mods_escape_into would then walk a NULL `s` for
// `len` bytes). oracles.log(nil) and oracles.log({}) must each produce a clean Lua argument-type
// error -- caught the same way any other main.lua error is -- never a crash.
static void oracles_log_with_a_non_string_argument_is_a_clean_lua_error(void) {
  clear_mods();
  const char *root = make_mod("lognil", "id=lognil\napi=1\ngames=ages,seasons\n", "oracles.log(nil)\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "lognil") != NULL);
  mods_shutdown();

  clear_mods();
  const char *root2 = make_mod("logtable", "id=logtable\napi=1\ngames=ages,seasons\n", "oracles.log({})\n");
  cap_root = root2;
  cap_seasons = true;
  capture_stderr(run_cap_mods_init, out, sizeof out);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "logtable") != NULL);
  mods_shutdown();
}

// Reviewer: moving run_main_lua above the game filter survives -- untrusted Lua must provably
// never run at all for a mod that doesn't apply to the current game, not merely "not end up
// counted as loaded". mods_last_log() staying NULL is the only observable proof main.lua's own
// oracles.log() call was never reached, since mods_count() alone can't distinguish "ran and was
// then discarded" from "never ran".
static void untrusted_lua_never_runs_for_a_mod_that_does_not_apply(void) {
  clear_mods();
  const char *root = make_mod("agesonlylog", "id=agesonlylog\napi=1\ngames=ages\n",
      "oracles.log('should not run under seasons')\n");
  ASSERT(mods_init(root, true));              // Seasons: agesonlylog does not apply
  ASSERT_EQ(mods_count(), 0);
  ASSERT(mods_last_log() == NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Fourth spec review: a looping __gc finalizer, and load()'s binary-chunk mode.
// ---------------------------------------------------------------------------

// CRITICAL, load-path shape: a finalizer is armed only if __gc is present in the metatable at
// the moment setmetatable() assigns it -- debug.setmetatable (the only other route) is already
// unreachable. mod_open_libs_hardened's l_setmetatable_no_gc wrapper is the one, complete choke
// point, so this must be refused right there, promptly, not reach GC or lua_close at all.
static void a_gc_finalizer_cannot_be_armed_at_load_time(void) {
  clear_mods();
  const char *root = make_mod("gcarm", "id=gcarm\napi=1\ngames=ages,seasons\n",
      "setmetatable({}, {__gc = function() while true do end end})\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(10);
  capture_stderr(run_cap_mods_init, out, sizeof out);
  alarm(0);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "gcarm") != NULL);
  ASSERT(strstr(out, "__gc") != NULL);
  mods_shutdown();
}

// CRITICAL, refusal-path shape: the coordinator's exact scenario -- a mod that arms a looping
// finalizer and then deliberately errors. Confirms the __gc rejection fires *before* the
// deliberate error ever runs (the message names "__gc", not "refuse me"), which is also exactly
// why this can never reach run_main_lua's own lua_close with a live finalizer attached: nothing
// ever got attached. Before this fix, this hung in lua_close *after* printing the refusal -- a
// mod that was rejected still took the process down.
static void a_gc_finalizer_cannot_be_armed_before_a_refusal(void) {
  clear_mods();
  const char *root = make_mod("gcrefuse", "id=gcrefuse\napi=1\ngames=ages,seasons\n",
      "_G.k = setmetatable({}, {__gc = function() while true do end end})\n"
      "error('refuse me')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(10);
  capture_stderr(run_cap_mods_init, out, sizeof out);
  alarm(0);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "gcrefuse") != NULL);
  ASSERT(strstr(out, "__gc") != NULL);
  ASSERT(strstr(out, "refuse me") == NULL);    // never reached: the setmetatable rejection wins
  mods_shutdown();
}

// CRITICAL, mid-run-GC shape: unlike the two above, this *catches* the rejection with pcall and
// keeps running, then forces a collection cycle explicitly -- proving the attempt genuinely never
// attached a finalizer (a GC pass over the object does not hang), not just that the one call
// that tried to raised an error. Before this fix, an ordinary mid-run GC step reaching a
// successfully-armed finalizer could hang here with no error and no shutdown involved at all.
static void a_gc_finalizer_cannot_be_armed_mid_run(void) {
  clear_mods();
  const char *root = make_mod("gcmidrun", "id=gcmidrun\napi=1\ngames=ages,seasons\n",
      "local ok, err = pcall(setmetatable, {}, {__gc = function() while true do end end})\n"
      "if ok then error('setmetatable should have refused the __gc metatable') end\n"
      "collectgarbage()\n"
      "oracles.log('survived: ' .. tostring(err))\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(10);
  capture_stderr(run_cap_mods_init, out, sizeof out);
  alarm(0);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "survived") != NULL);
  mods_shutdown();
}

// Task 7, superseding the above: load()'s mode argument segfaulted the host on the reviewer's
// very first fuzz trial of a corrupted binary dump -- worse than any hang this module exists to
// prevent. Task 5's fix (a since-deleted wrapper, l_load_text_only) forced mode "t" regardless of
// what was requested; Task 7's sandbox review went further and withholds `load` outright, since
// nothing in the planned API needs a mod to compile Lua source at runtime at all. This replaces
// the old binary-chunk reproduction (asserting the wrapper's text-mode behaviour) with the
// simpler, stronger claim that actually holds now: `load` itself is gone, so the crash it used to
// risk is unreachable regardless of what argument anyone could still pass it.
static void load_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("loadbin", "id=loadbin\napi=1\ngames=ages,seasons\n",
      "if load ~= nil then error('load is reachable') end\n"
      "oracles.log('load is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "load is unreachable") != NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Task 7: the sandbox. The spec grants a mod the standard string, table and math libraries plus
// oracles, and nothing else -- io, os, require/package, load/loadfile/dofile (dofile already
// covered above), string.dump, print/warn and utf8 are all withheld by mod_open_libs_hardened.
// Each gets its own small test below, one assertion per name, so that deleting any single entry
// from sandbox.c's `withheld` array makes exactly one test fail and names which entry is gone --
// not a shared fixture whose one failure could be any of several missing nils at once.
// ---------------------------------------------------------------------------

static void io_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("noio", "id=noio\napi=1\ngames=ages,seasons\n",
      "if io ~= nil then error('io is reachable') end\n"
      "oracles.log('io is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "io is unreachable") != NULL);
  mods_shutdown();
}

// os is withheld entirely, not merely the dangerous members (os.exit, os.execute) -- see
// mod_open_libs_hardened's own comment for why a partial allow-list was rejected. One
// consequence that outlives this task: no os.time()/os.clock() either, so a per-frame timer
// (the follow-up plan's speedrun-timer mod) must count frames instead.
static void os_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("noos", "id=noos\napi=1\ngames=ages,seasons\n",
      "if os ~= nil then error('os is reachable') end\n"
      "oracles.log('os is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "os is unreachable") != NULL);
  mods_shutdown();
}

static void require_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("norequire", "id=norequire\napi=1\ngames=ages,seasons\n",
      "if require ~= nil then error('require is reachable') end\n"
      "oracles.log('require is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "require is unreachable") != NULL);
  mods_shutdown();
}

static void package_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("nopackage", "id=nopackage\napi=1\ngames=ages,seasons\n",
      "if package ~= nil then error('package is reachable') end\n"
      "oracles.log('package is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "package is unreachable") != NULL);
  mods_shutdown();
}

// loadfile reaches the exact same binary-chunk crash load() used to, via luaL_loadfile
// internally, with no mode argument a caller could even force to "t" -- withheld outright for
// the same reason load() now is, rather than wrapped.
static void loadfile_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("noloadfile", "id=noloadfile\napi=1\ngames=ages,seasons\n",
      "if loadfile ~= nil then error('loadfile is reachable') end\n"
      "oracles.log('loadfile is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "loadfile is unreachable") != NULL);
  mods_shutdown();
}

// print writes straight to this process's own stdout, the exact channel ctest's own output (and
// this suite's line-buffered "ok <test>" lines) depends on -- oracles.log is the facility a mod
// actually has for this.
static void print_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("noprint", "id=noprint\napi=1\ngames=ages,seasons\n",
      "if print ~= nil then error('print is reachable') end\n"
      "oracles.log('print is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "print is unreachable") != NULL);
  mods_shutdown();
}

// warn is Lua 5.4's addition to lbaselib.c (not present in earlier Lua tasks' own enumeration of
// what luaL_openlibs grants): same host-stderr-writing hazard class as print, same reasoning.
static void warn_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("nowarn", "id=nowarn\napi=1\ngames=ages,seasons\n",
      "if warn ~= nil then error('warn is reachable') end\n"
      "oracles.log('warn is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "warn is unreachable") != NULL);
  mods_shutdown();
}

// utf8 is not one of the three libraries the spec grants -- withheld because it was never
// granted, not because it was found unsafe on its own.
static void utf8_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("noutf8", "id=noutf8\napi=1\ngames=ages,seasons\n",
      "if utf8 ~= nil then error('utf8 is reachable') end\n"
      "oracles.log('utf8 is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "utf8 is unreachable") != NULL);
  mods_shutdown();
}

// string.dump is a field inside the one library actually granted, not a global of its own, so it
// needs its own check rather than riding along with load_is_withheld above: a mutation that
// re-adds load to the withheld array but forgets string.dump specifically would otherwise pass
// every other test in this file.
static void string_dump_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("nodump", "id=nodump\napi=1\ngames=ages,seasons\n",
      "if string.dump ~= nil then error('string.dump is reachable') end\n"
      "oracles.log('string.dump is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "string.dump is unreachable") != NULL);
  mods_shutdown();
}

// Defense-in-depth, forward-looking: with `require` and `package` both nil, nothing in a mod's
// own Lua can reach package.loaded today regardless of whether it's scrubbed -- so this cannot be
// tested through a mod's main.lua the way the debug/coroutine require-resurrection tests above
// are. It is tested here instead by reading the registry's real LUA_LOADED_TABLE directly via the
// C API (not through the "package" global, which is nil and so proves nothing either way about
// the table underneath it), confirming every withheld name mod_open_libs_hardened's own list
// clears is actually absent from the table itself, not merely absent from the global that happens
// to expose it today. This is what keeps the defence alive if a later change ever restores
// `require` without separately revisiting mod_open_libs_hardened.
static void package_loaded_is_scrubbed_for_every_withheld_name(void) {
  lua_State *L = luaL_newstate();
  ASSERT(L);
  mod_open_libs_hardened(L);
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  static const char *const withheld[] = {
    "debug", "coroutine", "dofile", "loadfile", "io", "os", "require", "package",
    "load", "print", "warn", "utf8",
  };
  for (size_t i = 0; i < sizeof withheld / sizeof withheld[0]; i++) {
    lua_getfield(L, -1, withheld[i]);
    ASSERT(lua_isnil(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  lua_close(L);
}

// The spec's sandbox, demonstrated end to end by a mod that actively tries to escape it: checks
// every withheld name is absent AND that every granted library is both present and actually
// works. Correction from the original plan sketch: asserting mods_count() == 1 alone is no
// longer the signal its own comment once claimed ("it loaded, so none of those existed") --
// since Task 6, a mod whose own code *errors* is also counted, still listed with
// mods_enabled() false, so mods_count() == 1 is true either way. mods_enabled(0) plus the log
// line are the real pair: proof this mod's main.lua actually ran every check and reached its
// final oracles.log call, not that it merely got a slot. And checking only that the blocked
// names are absent would equally pass a sandbox that nils everything, including string/table/
// math/oracles themselves -- so this also confirms the granted libraries are present and a
// representative function from each actually works, not just that they're non-nil.
static void the_sandbox_holds(void) {
  clear_mods();
  const char *root = make_mod("nosy",
      "id=nosy\nname=Nosy\napi=1\ngames=ages,seasons\n",
      "if io ~= nil then error('io reachable') end\n"
      "if os ~= nil then error('os reachable') end\n"
      "if require ~= nil then error('require reachable') end\n"
      "if package ~= nil then error('package reachable') end\n"
      "if load ~= nil then error('load reachable') end\n"
      "if loadfile ~= nil then error('loadfile reachable') end\n"
      "if dofile ~= nil then error('dofile reachable') end\n"
      "if debug ~= nil then error('debug reachable') end\n"
      "if coroutine ~= nil then error('coroutine reachable') end\n"
      "if print ~= nil then error('print reachable') end\n"
      "if warn ~= nil then error('warn reachable') end\n"
      "if utf8 ~= nil then error('utf8 reachable') end\n"
      "if string.dump ~= nil then error('string.dump reachable') end\n"
      "if string == nil or table == nil or math == nil or oracles == nil then\n"
      "  error('a granted library is missing')\n"
      "end\n"
      "if string.upper('a') ~= 'A' then error('string does not work') end\n"
      "if table.concat({'a', 'b'}) ~= 'ab' then error('table does not work') end\n"
      "if math.floor(1.5) ~= 1 then error('math does not work') end\n"
      "oracles.log('sandboxed')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "sandboxed") != NULL);
  mods_shutdown();
}

// The full inventory, read rather than reasoned about: dumps every key in _G from inside a
// loaded mod's own live environment, sorted, so the exact surface a mod can see is pinned
// directly against the expected set -- not reconstructed from a blocklist that was assumed
// complete because it looked complete. This is literally the fixture the task asked for: run it,
// read the list, decide name by name. It is also how "warn" (Lua 5.4's addition to lbaselib.c,
// not named anywhere in this file before Task 7) was actually found, rather than reasoned about
// in the abstract -- the first run of this exact test, before mod_open_libs_hardened withheld it,
// printed it in the list.
static void the_global_environment_is_exactly_the_granted_set(void) {
  clear_mods();
  const char *root = make_mod("inventory", "id=inventory\napi=1\ngames=ages,seasons\n",
      "local names = {}\n"
      "for k in pairs(_G) do names[#names + 1] = tostring(k) end\n"
      "table.sort(names)\n"
      "oracles.log(table.concat(names, ','))\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT_EQ(strcmp(mods_last_log(),
      "_G,_VERSION,assert,collectgarbage,error,getmetatable,ipairs,math,next,oracles,"
      "pairs,pcall,rawequal,rawget,rawlen,rawset,select,setmetatable,string,table,"
      "tonumber,tostring,type,xpcall"), 0);
  mods_shutdown();
}

// The same crash is reachable through a main.lua file that is *itself* a binary chunk (crafted,
// or merely corrupted in transit) -- our own luaL_loadfilex call for main.lua needed the same
// "t"-forced fix as load() does, not just load() itself.
static void a_binary_main_lua_is_refused_safely(void) {
  clear_mods();
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char root[600], dir[700], cmd[900];
  snprintf(root, sizeof root, "%s/test_mods_root", tmp);
  snprintf(dir, sizeof dir, "%s/binarymain", root);
  snprintf(cmd, sizeof cmd,
      "mkdir -p '%s' && printf 'id=binarymain\\napi=1\\ngames=ages,seasons\\n' > '%s/mod.ini' && "
      "printf '\\x1bLua\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff' > '%s/main.lua'",
      dir, dir, dir);
  ASSERT_EQ(system(cmd), 0);
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(10);
  capture_stderr(run_cap_mods_init, out, sizeof out);
  alarm(0);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "binarymain") != NULL);
  mods_shutdown();
}

// Hazard found while fixing load(), not separately reported: dofile() reaches the exact same
// binary-chunk crash through luaL_loadfile with no mode argument to override, so it is withheld
// outright rather than wrapped (a mod has no legitimate need to execute an arbitrary file
// straight off disk, distinct from the already-open io library's ordinary read/write calls).
static void dofile_is_withheld(void) {
  clear_mods();
  const char *root = make_mod("nodofile", "id=nodofile\napi=1\ngames=ages,seasons\n",
      "if dofile ~= nil then error('dofile is reachable') end\n"
      "oracles.log('dofile is unreachable')\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "dofile is unreachable") != NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Fourth spec review: pin the instruction budget itself, and the two-message split.
// ---------------------------------------------------------------------------

// Reviewer: multiplying MOD_LOAD_INSTRUCTION_BUDGET by 10,000 -- effectively disabling it --
// passes every one of the 86 tests that existed before this one; the wall-clock deadline
// absorbs every refusal either way, since ASSERT_BUDGET_OR_DEADLINE accepts both messages by
// design. This also pins the opposite bug: hardcoding deadline_exceeded to always true would
// make every refusal say "cut off" regardless of which bound actually fired. A bare, cheap loop
// is assumed fast enough on any host this suite runs on to reach the instruction budget well
// inside the 2-second deadline (measured 0.1-0.5s across every load this session, including
// under host load averages past 100 on a 10-core machine) -- the same host-speed assumption the
// instruction budget already rests on, not a new one.
static void a_cheap_infinite_loop_is_caught_by_the_instruction_budget_specifically(void) {
  clear_mods();
  const char *root = make_mod("cheaploop", "id=cheaploop\napi=1\ngames=ages,seasons\n",
      "while true do end\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "cheaploop") != NULL);
  ASSERT(strstr(out, "instruction budget") != NULL);
  ASSERT(strstr(out, "cut off") == NULL);
  ASSERT(strstr(mods_error(0), "instruction budget") != NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Fourth spec review: the registry must match what a loaded mod actually sees.
// ---------------------------------------------------------------------------

static int cmp_cstr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Reviewer: the original registry test built its own Lua state and called mods_api_open
// directly, so a binding installed anywhere else along the real load path -- the reviewer added
// five lines right after mods_api_open(L) inside run_main_lua itself, exactly where a future
// task is most likely to add one -- was completely invisible to it; a real, callable
// oracles.undocumented() passed the whole suite, including that test. This instead walks the
// live `oracles` table from *inside* a fully loaded mod's own environment, built however
// run_main_lua actually builds it. Also pins "every entry is actually callable", not just "every
// entry's key exists": a future `oracles.overlay = {rect = ...}` would satisfy a names-only check
// with just "overlay" listed while actually being a table a mod cannot call directly --
// type(v) == 'function' is true for both Lua and C functions, which is the distinction that
// actually matters (can a mod call oracles.xxx(...) at all), not which language implemented it.
static void the_live_oracles_table_in_a_loaded_mod_matches_the_registry_exactly(void) {
  clear_mods();
  const char *root = make_mod("registrycheck", "id=registrycheck\napi=1\ngames=ages,seasons\n",
      "local k = {}\n"
      "for n, v in pairs(oracles) do\n"
      "  if type(v) ~= 'function' then error('oracles.' .. tostring(n) .. ' is not callable') end\n"
      "  k[#k+1] = n\n"
      "end\n"
      "table.sort(k)\n"
      "oracles.log(table.concat(k, ','))\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());

  int n = mods_api_count();
  ASSERT(n > 0);
  ASSERT(n <= 64);
  const char *names[64];
  for (int i = 0; i < n; i++) names[i] = mods_api_name(i);
  qsort(names, (size_t)n, sizeof names[0], cmp_cstr);
  char expected[1024] = "";
  for (int i = 0; i < n; i++) {
    if (i > 0) strcat(expected, ",");
    strcat(expected, names[i]);
  }
  ASSERT_EQ(strcmp(mods_last_log(), expected), 0);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Fourth spec review: pin the hostile-__tostring fix, and the realloc-widening cap branch.
// ---------------------------------------------------------------------------

// Reviewer: lua_tolstring (not luaL_tolstring) in run_main_lua's error reporting was measured as
// load-bearing, not stylistic -- 0.000s refused as shipped, a hang past 12s with luaL_tolstring
// substituted on this exact script. Currently unpinned, so a future "let's get a richer error
// message" revert to luaL_tolstring would pass every other test and reopen it silently. Same
// hazard class as the __gc finalizer above: Lua reached from host code outside
// mod_resume_budgeted's one guarded lua_resume call, where neither the instruction budget nor
// the deadline is ever consulted -- generalise from this, rather than re-deriving it, the next
// time this module calls into Lua from anywhere else.
static void a_hostile_tostring_metamethod_does_not_hang_error_reporting(void) {
  clear_mods();
  const char *root = make_mod("hostiletostring", "id=hostiletostring\napi=1\ngames=ages,seasons\n",
      "error(setmetatable({}, {__tostring = function() while true do end end}))\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(15);
  capture_stderr(run_cap_mods_init, out, sizeof out);
  alarm(0);
  ASSERT(cap_result);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "hostiletostring") != NULL);
  mods_shutdown();
}

// Reviewer: deleting mod_lua_alloc's realloc-widening cap check (the one guarding
// `a->used + (nsize - osize) > a->cap`) passes all 86 tests that existed before this one -- the
// existing memory-cap test never actually reaches it (see its corrected comment above). Array-
// part growth is what does: each assignment into a growing integer-keyed table can force Lua to
// realloc the table's own backing array to a larger size, repeatedly, which is the realloc path
// specifically, never a fresh ptr == NULL allocation.
static void array_growth_is_refused_via_the_realloc_path(void) {
  clear_mods();
  const char *root = make_mod("arraygrowth", "id=arraygrowth\napi=1\ngames=ages,seasons\n",
      "local t = {}\n"
      "for i = 1, 1000000000 do t[i] = i end\n"
      "oracles.log('should have been refused before reaching here')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);                  // Task 6: listed, disabled, not dropped
  ASSERT(!mods_enabled(0));
  ASSERT(strstr(out, "arraygrowth") != NULL);
  ASSERT(strstr(out, "not enough memory") != NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Fifth spec review: catastrophic pattern-matching backtracking, patched into
// third_party/lua/lstrlib.c itself (see its README.md entry) rather than closed from mods.c --
// no VM instruction ever retires inside the matcher, so neither the instruction budget nor the
// wall-clock deadline in sandbox.c is reachable from here; this is the one hazard in this module
// that only a change to the vendored interpreter itself can close.
// ---------------------------------------------------------------------------

// find: the exact shape the reviewer measured hanging past 20 seconds unpatched. Pins the
// lstrlib.c MOD_PATTERN_STEP_BUDGET mechanism specifically -- the message ("pattern too
// complex") is distinct from both the instruction-budget and deadline wording, so this is not
// just "something stopped it", it is specifically the matcher's own bound doing so.
static void a_catastrophic_find_pattern_is_refused_by_the_matcher_itself(void) {
  clear_mods();
  const char *root = make_mod("patfind", "id=patfind\napi=1\ngames=ages,seasons\n",
      "local ok, err = pcall(string.find, ('a'):rep(40), ('.-'):rep(10) .. 'b')\n"
      "if ok then error('should have raised pattern too complex') end\n"
      "oracles.log('refused: ' .. tostring(err))\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "pattern too complex") != NULL);
  mods_shutdown();
}

// Same shape through gsub, the other entry point str_gsub drives its own per-position loop
// through (reprepstate, not prepstate, confirming the budget is shared across the whole call
// rather than reset per starting position the way matchdepth already was).
static void a_catastrophic_gsub_pattern_is_refused_by_the_matcher_itself(void) {
  clear_mods();
  const char *root = make_mod("patgsub", "id=patgsub\napi=1\ngames=ages,seasons\n",
      "local ok, err = pcall(string.gsub, ('a'):rep(40), ('.-'):rep(10) .. 'b', 'x')\n"
      "if ok then error('should have raised pattern too complex') end\n"
      "oracles.log('refused: ' .. tostring(err))\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "pattern too complex") != NULL);
  mods_shutdown();
}

// The regression guard that is easy to forget: a legitimate, moderately heavy gsub (the exact
// shape measured when choosing MOD_PATTERN_STEP_BUDGET -- a ~100 KB subject, an ordinary
// pattern) must still load successfully. Without this, someone could tighten the budget until
// real mods break and nothing here would notice.
static void a_legitimately_heavy_pattern_still_loads(void) {
  clear_mods();
  const char *root = make_mod("patgood", "id=patgood\napi=1\ngames=ages,seasons\n",
      "local s = string.rep('hello world, this is a test sentence. ', 2700)\n"
      "local n = select(2, s:gsub('%a+', function(w) return w:upper() end))\n"
      "oracles.log('processed ' .. n .. ' words')\n");
  cap_root = root;
  cap_seasons = true;
  char out[4096];
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  capture_stderr(run_cap_mods_init, out, sizeof out);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(cap_result);
  ASSERT_PROMPT(t0, t1);
  ASSERT_EQ(mods_count(), 1);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "processed 18900 words") != NULL);
  mods_shutdown();
}

// ---------------------------------------------------------------------------
// Task 8: events, game state and drawing from Lua.
// ---------------------------------------------------------------------------

// on_frame fires each frame; rd() reads game RAM; draw_rect lands on the overlay. Designated
// initializers for ModsFrame, not positional -- Task 4's review required this for an identically
// shaped struct, for the identical reason: a positional { wram, hram, true, 256, 0, NULL, 0,
// NULL } keeps silently compiling (and silently means something different) the moment a field
// is reordered or a new one inserted.
static void a_mod_sees_frames_and_draws(void) {
  clear_mods();
  const char *root = make_mod("hud",
      "id=hud\nname=HUD\napi=1\ngames=ages,seasons\n",
      "local n = 0\n"
      "oracles.on_frame(function()\n"
      "  n = n + 1\n"
      "  oracles.log('frames=' .. n .. ' ram=' .. oracles.rd(0xc6bb) .. ' w=' .. oracles.screen())\n"
      "  oracles.draw_rect(2, 3, 4, 5, 0x7fff)\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  memset(wram, 0, sizeof wram);
  memset(hram, 0, sizeof hram);
  wram[0x6bb] = 0x05;                                  // wEssencesObtained
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 256, .room = 0 };
  mods_frame(&f);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "frames=1"));
  ASSERT(strstr(mods_last_log(), "ram=5"));
  ASSERT(strstr(mods_last_log(), "w=256"));
  ASSERT_EQ(overlay_cover_at(mods_overlay(), 2, 3), 255);
  ASSERT_EQ(overlay_cover_at(mods_overlay(), 5, 7), 255);
  ASSERT_EQ(overlay_cover_at(mods_overlay(), 6, 3), 0);
  mods_frame(&f);
  ASSERT(strstr(mods_last_log(), "frames=2"));
  ASSERT(mods_enabled(0));
  mods_shutdown();
}

// on_room_change fires only when the room actually changes.
static void room_changes_fire_once(void) {
  clear_mods();
  const char *root = make_mod("roomschg",
      "id=roomschg\nname=Rooms\napi=1\ngames=ages,seasons\n",
      "local n = 0\n"
      "oracles.on_room_change(function(r) n = n + 1 oracles.log('changes=' .. n .. ' room=' .. r) end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 7 };
  mods_frame(&f);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "changes=1"));
  mods_frame(&f);                                      // same room
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "changes=1"));
  f.room = 8;
  mods_frame(&f);
  ASSERT(strstr(mods_last_log(), "changes=2"));
  ASSERT(strstr(mods_last_log(), "room=8"));
  mods_shutdown();
}

// Correction: "an exhausted thread cannot be reused" is about the failure path, but even a
// *healthy* repeated call must not reuse a thread lua_resume has already driven to completion --
// a finished coroutine is "dead" to Lua, and a second lua_resume on a dead thread errors outright
// ("cannot resume dead coroutine"), which would poison this mod on its second frame if
// call_handler reused one thread across calls instead of minting a fresh one each time. Five
// frames mirrors the depth Task 5's own review simulated for the identical hazard at load time.
static void a_healthy_mod_runs_many_frames_without_reusing_a_dead_thread(void) {
  clear_mods();
  const char *root = make_mod("multiframe", "id=multiframe\napi=1\ngames=ages,seasons\n",
      "local n = 0\n"
      "oracles.on_frame(function() n = n + 1 oracles.log('n=' .. n) end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  for (int i = 1; i <= 5; i++) {
    mods_frame(&f);
    ASSERT(mods_enabled(0));
    char expect[16];
    snprintf(expect, sizeof expect, "n=%d", i);
    ASSERT(mods_last_log());
    ASSERT(strstr(mods_last_log(), expect) != NULL);
  }
  mods_shutdown();
}

// Correction: oracles.screen() must not return 0, and oracles.game() must not report the wrong
// game, when called from main.lua's own top-level body -- before mods_frame has ever run once.
// mods_init is the one place that actually knows this session's real `seasons`; width defaults
// to 160 (never widescreen) until the first real frame says otherwise.
static void screen_and_game_are_sane_before_the_first_frame(void) {
  clear_mods();
  const char *root = make_mod("early",
      "id=early\napi=1\ngames=ages,seasons\n",
      "oracles.log('game=' .. oracles.game() .. ' screen=' .. oracles.screen())\n");
  ASSERT(mods_init(root, true));                       // true: Seasons
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "game=seasons") != NULL);
  ASSERT(strstr(mods_last_log(), "screen=160") != NULL);
  mods_shutdown();

  clear_mods();
  const char *root2 = make_mod("early2",
      "id=early2\napi=1\ngames=ages,seasons\n",
      "oracles.log('game=' .. oracles.game())\n");
  ASSERT(mods_init(root2, false));                     // false: Ages
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "game=ages") != NULL);
  mods_shutdown();
}

// Correction: mods_frame dereferences `f` with no guard in the plan's sketch. A NULL `f` is the
// launcher's own state (no game running yet) and must be a no-op, not a crash.
static void mods_frame_with_a_null_frame_is_a_safe_no_op(void) {
  clear_mods();
  const char *root = make_mod("nullframe", "id=nullframe\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() oracles.log('should not run') end)\n");
  ASSERT(mods_init(root, true));
  mods_frame(NULL);
  ASSERT(mods_last_log() == NULL);                     // the handler never ran
  ASSERT(mods_enabled(0));                              // and the mod itself is unaffected
  mods_shutdown();
}

// Correction: overlay_icon's c600 argument is f->wram + 0x600 -- NULL + 0x600 is undefined
// behaviour, not a null pointer, if f->wram is NULL, which is exactly the launcher's state
// before any game is running. f->rom being non-NULL alone must not be enough to proceed.
//
// A degenerate fake ROM (a handful of zero bytes) does NOT exercise this: wide_item_icon's own
// table walk only ever reads c600 through two gated paths -- the seasons/slingshot special case,
// and a nonzero `level_byte` -- and with every ROM byte reading back 0 (wide_item_icon's own `rb`
// helper returns 0 past the end of a short buffer), the item is simply never "found" and both
// gates stay closed, so a first attempt at this test (a 4-byte fake ROM, item 0x03) passed with
// the f->wram guard removed by hand -- the mutation never crashed because c600 was never
// actually read, not because the guard caught anything. Confirmed by deliberately re-removing
// the guard and rerunning: still green, for the wrong reason.
//
// A real ROM and item 0x13 (the slingshot) with seasons=true is what item.c's own comment
// documents as reading c600[SLINGSHOT_LEVEL] (0xb3) -- the seasons-specific "listed twice, take
// the hyper slingshot entry at level 2" check -- so this exercises the actual dereference rather
// than merely calling the binding and hoping. Skips cleanly if the ROM isn't present, the same
// convention every other ROM-dependent test in this file follows.
static bool draw_icon_with_no_wram_does_not_crash(void) {
  size_t n;
  uint8_t *rom = read_file(GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Seasons (USA, Australia).gbc", &n);
  if (!rom) SKIP_TEST("ROM not present");

  clear_mods();
  const char *root = make_mod("iconnowram", "id=iconnowram\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function()\n"
      "  local ok = oracles.draw_icon(0x13, 10, 10)\n"            // 0x13: the slingshot
      "  oracles.log('ok=' .. tostring(ok))\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  ModsFrame f = { .wram = NULL, .hram = NULL, .seasons = true, .width = 160, .room = 1,
                  .rom = rom, .rom_size = n };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "ok=false") != NULL);
  mods_shutdown();
  free(rom);
  return true;
}

// draw_text with no font installed (before a ROM provides one) must silently draw nothing, never
// crash by dereferencing a NULL font.
static void draw_text_with_no_font_draws_nothing(void) {
  clear_mods();
  const char *root = make_mod("textnofont", "id=textnofont\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function()\n"
      "  oracles.draw_text(4, 4, 'hello', 0x7fff)\n"
      "  oracles.log('drew')\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "drew") != NULL);
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) ASSERT_EQ(overlay_cover_at(mods_overlay(), x, y), 0);
  mods_shutdown();
}

// Correction: (int)luaL_checkinteger truncates a 64-bit lua_Integer, so a Lua number like
// 4294967296 + 5 (2^32 + 5) would arrive at a bare cast as w == 5 -- not an error, not INT_MAX,
// simply wrong in a way no mod author watching their own source could predict. check_int clamps
// instead: the whole row, not five columns, must end up covered.
static void draw_rect_clamps_rather_than_wrapping_a_huge_width(void) {
  clear_mods();
  const char *root = make_mod("hugerect", "id=hugerect\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() oracles.draw_rect(0, 0, 4294967296 + 5, 1, 0x7fff) end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT_EQ(overlay_cover_at(mods_overlay(), 0, 0), 255);
  ASSERT_EQ(overlay_cover_at(mods_overlay(), 159, 0), 255);   // the whole row, not just 5 columns
  mods_shutdown();
}

// Correction: a Lua string may legally contain an embedded NUL. Decided deliberately (see
// l_draw_text's own comment in api.c): overlay_text (Task 3, unchanged here) takes a plain
// NUL-terminated C string, so "A\0B" draws only "A"'s glyph. Pinned so a future accidental
// change either way -- silently "fixing" the truncation, or regressing to read past the NUL --
// is caught.
static void draw_text_truncates_at_an_embedded_nul(void) {
  clear_mods();
  const char *root = make_mod("textnul", "id=textnul\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() oracles.draw_text(0, 0, 'A\\0B', 0x7fff) end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  static UiFont font;
  ui_font_builtin(&font);
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1, .font = &font };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  // "A" alone puts ink only in the first glyph cell's columns; a second glyph ("B") would put
  // ink starting at x == UI_GLYPH_W, which must not happen if the embedded NUL actually stopped
  // the draw there.
  bool ink_in_first_glyph = false, ink_past_first_glyph = false;
  for (int y = 0; y < MOD_OVERLAY_H; y++)
    for (int x = 0; x < 160; x++) {
      if (!overlay_cover_at(mods_overlay(), x, y)) continue;
      if (x < UI_GLYPH_W) ink_in_first_glyph = true;
      else ink_past_first_glyph = true;
    }
  ASSERT(ink_in_first_glyph);
  ASSERT(!ink_past_first_glyph);
  mods_shutdown();
}

// Correction (noted for MODDING.md, Task 11): oracles.rd only reaches $c000..$cfff and
// $ff80..$fffe. $d000 (the switchable WRAM bank) and $0000 are both outside those windows and
// must read back 0 -- not whatever bytes happen to sit at that offset in ModsFrame's buffers,
// and not a crash.
static void rd_outside_the_known_windows_reads_as_zero(void) {
  clear_mods();
  const char *root = make_mod("rdrange", "id=rdrange\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function()\n"
      "  oracles.log('d000=' .. oracles.rd(0xd000) .. ' zero=' .. oracles.rd(0))\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  memset(wram, 0xff, sizeof wram);     // if the range guard were missing, a nonzero byte would leak
  memset(hram, 0xff, sizeof hram);
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "d000=0 zero=0") != NULL);
  mods_shutdown();
}

// Correction: a handler must not be reachable by a mod setting a plain Lua global itself (the
// plan's own sketch used lua_setglobal(L, "__on_frame")) -- that is also a global a mod could
// set directly, bypassing oracles.on_frame entirely. Pinning the negative: a mod that assigns
// what would be that exact global, without ever calling oracles.on_frame, must never have it
// invoked by the host.
static void a_bare_global_assignment_never_runs_as_a_handler(void) {
  clear_mods();
  const char *root = make_mod("bypass", "id=bypass\napi=1\ngames=ages,seasons\n",
      "__on_frame = function() oracles.log('bypass ran') end\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log() == NULL);              // the host never reads any such global
  mods_shutdown();
}

// Registering on_frame twice replaces, rather than stacking, the handler -- consistent with the
// registry-ref design holding exactly one slot per mod per event (see mods_set_on_frame_ref's own
// comment in mods.h), and confirms the luaL_unref of the previous ref does not somehow also
// un-register the new one.
static void re_registering_on_frame_replaces_the_previous_handler(void) {
  clear_mods();
  const char *root = make_mod("reregister", "id=reregister\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() oracles.log('first') end)\n"
      "oracles.on_frame(function() oracles.log('second') end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "second") != NULL);
  ASSERT(strstr(mods_last_log(), "first") == NULL);
  mods_shutdown();
}

// on_room_change must be a safe no-op for a mod that never registered one, even though the room
// did change.
static void on_room_change_is_skipped_when_never_registered(void) {
  clear_mods();
  const char *root = make_mod("noroomhandler", "id=noroomhandler\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() oracles.log('frame ran') end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "frame ran") != NULL);
  f.room = 2;                      // room changes, but this mod never called on_room_change
  mods_frame(&f);
  ASSERT(mods_enabled(0));         // must not error just because there is nothing to call
  mods_shutdown();
}

// Correction: last_room is static and, without an explicit reset, would outlive mods_shutdown/
// mods_init -- the same stale-state class as last_log (Task 5). A fresh session whose first real
// room happens to equal the *previous* session's last room must still fire on_room_change once,
// since this genuinely is the first frame of a new session.
static void last_room_resets_across_a_reinit(void) {
  clear_mods();
  const char *root = make_mod("roomreset", "id=roomreset\napi=1\ngames=ages,seasons\n",
      "local n = 0\n"
      "oracles.on_room_change(function(r) n = n + 1 oracles.log('changes=' .. n) end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 5 };
  mods_frame(&f);
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "changes=1") != NULL);
  mods_shutdown();

  ASSERT(mods_init(root, true));                 // a brand new session
  mods_frame(&f);                                // same room (5) as the previous session's last
  // Checked separately from the strstr below, not combined into one ASSERT(strstr(...)): if
  // last_room were ever left stale, the handler above would simply never fire on this "new"
  // session's first frame, mods_last_log() would return NULL (nothing logged, same as before
  // mods_init), and strstr(NULL, ...) is undefined behaviour -- a crash that obscures which half
  // of the check actually failed. Asserting non-NULL first turns that into a clean, named
  // failure instead.
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "changes=1") != NULL);   // must fire again: frame 1 of a new
                                                            // session, not "no change"
  mods_shutdown();
}

// CRITICAL correction: the plan's own sketch calls lua_pcall directly in call_handler, with no
// budget, no deadline, and no dedicated thread -- oracles.on_frame(function() while true do end
// end) hangs the game on its first frame and every one after, the precise failure Task 5's whole
// budget mechanism exists to prevent at load time. pcall is core Lua and cannot be withheld the
// way "debug" is, so this must be tested the same way the load-time budget is: a pcall-wrapped
// spinner inside on_frame must be caught promptly and the mod poisoned (mods_enabled false, not
// merely yielded-and-retried). The healthy mods on either side -- and a second mods_frame call
// afterward -- are the half a load-time-only test cannot show, since a load happens once per
// session: "disable everything" must not pass this.
static void a_pcall_wrapped_spinner_inside_on_frame_is_poisoned_promptly(void) {
  clear_mods();
  make_mod("aaa-frame-healthy", "id=aaa-frame-healthy\napi=1\ngames=ages,seasons\n",
      "local n = 0\noracles.on_frame(function() n = n + 1 oracles.log('aaa n=' .. n) end)\n");
  make_mod("mid-frame-pcall-spin", "id=mid-frame-pcall-spin\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function() while true do pcall(function() while true do end end) end end)\n");
  const char *root = make_mod("zzz-frame-healthy", "id=zzz-frame-healthy\napi=1\ngames=ages,seasons\n",
      "local n = 0\noracles.on_frame(function() n = n + 1 oracles.log('zzz n=' .. n) end)\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 3);
  ASSERT(mods_enabled(0));
  ASSERT(mods_enabled(1));
  ASSERT(mods_enabled(2));

  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };

  signal(SIGALRM, mods_test_alarm_handler);
  alarm(20);
  double t0 = wall_seconds();
  mods_frame(&f);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT_PROMPT(t0, t1);
  ASSERT(mods_enabled(0));
  ASSERT(!mods_enabled(1));                             // poisoned, not merely yielded
  ASSERT(mods_enabled(2));
  ASSERT_BUDGET_OR_DEADLINE(mods_error(1));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "zzz n=1") != NULL);    // zzz still ran on this very frame

  // A second frame: the poisoned mod has no L left to even try resuming, and the healthy ones
  // either side must keep going.
  mods_frame(&f);
  ASSERT(mods_enabled(0));
  ASSERT(!mods_enabled(1));
  ASSERT(mods_enabled(2));
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "zzz n=2") != NULL);
  mods_shutdown();
}

// CRITICAL correction, second route: coroutine.resume is itself implemented via lua_resume on
// the nested thread, and a budget yield there is indistinguishable, to coroutine.resume's own
// caller, from an ordinary coroutine.yield() -- the same bypass the load-time tests above pin.
// "coroutine" is withheld from mods entirely (mod_open_libs_hardened), so inside on_frame this
// fails on its very first line instead -- a nil-global error, not a budget message. The task
// asks specifically which mechanism stopped it, not just that something did, so that is what
// this asserts.
static void a_coroutine_wrapped_spinner_inside_on_frame_fails_on_the_nil_global(void) {
  clear_mods();
  const char *root = make_mod("framecoro", "id=framecoro\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function()\n"
      "  local ok, err = pcall(function()\n"
      "    local co = coroutine.create(function() while true do end end)\n"
      "    coroutine.resume(co)\n"
      "  end)\n"
      "  oracles.log('ok=' .. tostring(ok) .. ' err=' .. tostring(err))\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(10);
  mods_frame(&f);
  alarm(0);
  ASSERT(mods_enabled(0));                        // the handler's own pcall caught the error --
                                                    // it never even reached the frame budget
  ASSERT(mods_last_log());
  ASSERT(strstr(mods_last_log(), "ok=false") != NULL);
  ASSERT(strstr(mods_last_log(), "coroutine") != NULL);  // "...nil value (global 'coroutine')"
  mods_shutdown();
}

// Measured, not guessed (see MOD_FRAME_INSTRUCTION_BUDGET's own comment in mods.c, and
// MOD_INSTRUCTION_CHECKIN's in sandbox.c): a handler built specifically to maximise
// per-VM-instruction cost (repeated pcall/error, 50-150x more expensive per instruction than
// plain arithmetic) stresses the one thing the per-frame deadline cannot fully promise on its
// own -- the wall-clock deadline is only ever consulted when the shared instruction-count hook
// fires, so the deadline is only as enforceable as that hook's firing interval is fine-grained.
// It used to not be: with MOD_INSTRUCTION_CHECKIN at its original 1,000,000, this exact shape
// measured ~60-67ms to be caught (15-17x the nominal 4ms deadline), both in this repo's own
// benchmark and in an independent reviewer re-measurement. Lowering MOD_INSTRUCTION_CHECKIN to
// 10,000 (sandbox.c) closed that gap: the same shape now measures ~4.2ms (9-trial median),
// within ~5% of the deadline instead of 15-17x over it. The bound below is tightened from the
// original (nowhere-near-a-hang) 2.0s to 0.05s specifically so it actually pins that
// improvement -- 0.05s is still ~12x the measured 4.2ms for ordinary scheduling jitter and slower
// CI hardware, but it is well under the old ~60-67ms figure, so a regression back to that
// behaviour (MOD_INSTRUCTION_CHECKIN raised again, or any other change that coarsens how often
// the hook fires) fails this test rather than silently passing under a bound generous enough to
// hide it.
static void a_pathologically_expensive_on_frame_handler_is_poisoned_after_one_bounded_stall(void) {
  clear_mods();
  const char *root = make_mod("expensive", "id=expensive\napi=1\ngames=ages,seasons\n",
      "oracles.on_frame(function()\n"
      "  while true do pcall(function() error('x') end) end\n"
      "end)\n");
  ASSERT(mods_init(root, true));
  static uint8_t wram[0x1000], hram[127];
  ModsFrame f = { .wram = wram, .hram = hram, .seasons = true, .width = 160, .room = 1 };
  signal(SIGALRM, mods_test_alarm_handler);
  alarm(15);
  double t0 = wall_seconds();
  mods_frame(&f);
  double t1 = wall_seconds();
  alarm(0);
  ASSERT(!mods_enabled(0));
  ASSERT((t1 - t0) < 0.05);          // bounded and tight (measured ~0.0042s on this machine) --
                                      // not merely "not a hang" (see this function's own comment)
  double t2 = wall_seconds();
  mods_frame(&f);                    // a second call must not stall again: there is no L left
  double t3 = wall_seconds();
  ASSERT((t3 - t2) < 0.1);
  mods_shutdown();
}

int main(void) {
  // The alarm handler's _exit(1) skips normal stdio teardown (deliberately: calling atexit
  // handlers or flushing from inside a signal handler is not safe), which means any buffered
  // "ok <test>" line not yet flushed to the terminal is simply lost on a real hang -- the
  // operator sees the alarm's own "FAIL: mods_init hung" message but not which test printed
  // last, right when that information matters most. Line-buffering stdout (RUN already prints a
  // newline after every test) means each "ok" line reaches the terminal as soon as it's
  // written, not whenever the C library's own block-buffering heuristic decides to flush.
  setvbuf(stdout, NULL, _IOLBF, 0);
  RUN(lua_runs);
  RUN(overlay_draws_a_rect);
  RUN(overlay_clips);
  RUN(overlay_survives_a_huge_rect);
  RUN(overlay_begin_clears_the_last_frame);
  RUN(overlay_draws_text);
  RUN(overlay_text_skips_high_bytes);
  RUN(overlay_text_with_unloaded_font_draws_nothing);
  RUN(overlay_survives_a_huge_string);
  RUN(overlay_draws_a_partially_clipped_glyph);
  RUN_SKIPPABLE(overlay_draws_an_item_icon);
  RUN(mods_id_valid_rejects_hostile_input);
  RUN(a_missing_mods_directory_is_not_a_failure);
  RUN(an_empty_mods_directory_has_no_mods);
  RUN(mods_init_rejects_a_null_directory);
  RUN(mods_init_rejects_an_empty_directory_string);
  RUN_SKIPPABLE(an_unreadable_mods_directory_is_a_failure);
  RUN(a_mod_is_found_and_described);
  RUN(a_broken_mod_is_disabled_not_removed);
  RUN(disabled_treatment_is_uniform_across_failure_kinds);
  RUN(a_disabled_mod_before_a_healthy_one_does_not_shift_indices);
  RUN(mods_error_rejects_out_of_range_indices);
  RUN(a_newer_api_is_refused);
  RUN(the_other_games_mod_is_skipped);
  RUN(a_traversing_ini_id_is_refused);
  RUN(a_mismatched_ini_id_is_refused);
  RUN(an_ini_id_that_is_a_prefix_of_the_folder_name_is_refused);
  RUN(a_folder_name_that_is_a_prefix_of_the_ini_id_is_refused);
  RUN(a_missing_id_is_refused);
  RUN(a_badly_named_folder_is_skipped);
  RUN(a_missing_api_is_refused);
  RUN(a_non_numeric_api_is_refused);
  RUN(an_empty_api_value_is_refused);
  RUN(an_api_of_zero_is_refused);
  RUN(an_api_over_int_max_is_refused);
  RUN(an_astronomically_large_api_is_refused);
  RUN(a_mod_without_a_name_falls_back_to_its_id);
  RUN(crlf_line_endings_are_tolerated);
  RUN(a_folder_without_mod_ini_is_skipped);
  RUN(a_missing_games_is_refused);
  RUN(a_substring_game_token_is_refused);
  RUN(an_unrecognised_game_token_is_reported_but_the_valid_one_still_loads);
  RUN(whitespace_and_comments_are_tolerated);
  RUN(an_overlong_line_does_not_leak_into_the_next_key);
  RUN(a_stray_file_is_skipped);
  RUN(mods_load_in_alphabetical_order);
  RUN(loading_stops_at_mods_max);
  RUN(accessors_reject_out_of_range_indices_with_no_mods_loaded);
  RUN(accessors_reject_out_of_range_indices_after_a_successful_load);
  RUN(mods_init_resets_state_even_when_it_then_fails);
  RUN(a_value_containing_equals_signs_is_kept_whole);
  RUN(a_games_token_that_is_a_superset_of_ages_is_refused);
  RUN(duplicate_games_lines_let_the_last_one_win);
  RUN(dotfiles_in_the_mods_folder_are_silently_ignored);
  RUN(a_64_char_folder_name_is_refused);
  RUN(hostile_bytes_in_a_value_are_escaped);
  RUN(a_nul_byte_in_mod_ini_is_refused);
  RUN(the_same_ini_without_a_nul_byte_loads_fine);
  RUN_SKIPPABLE(a_fifo_mod_ini_does_not_hang);
  RUN_SKIPPABLE(a_symlink_to_a_character_device_does_not_hang);
  RUN(a_directory_named_mod_ini_is_refused);
  RUN(main_lua_runs);
  RUN(the_api_registry_lists_itself);
  RUN(mods_last_log_is_cleared_by_shutdown);
  RUN(a_hanging_mod_is_disabled_and_other_mods_still_load);
  RUN(an_unbounded_allocation_mod_is_disabled_and_other_mods_still_load);
  RUN(a_mod_with_no_main_lua_is_refused_clearly);
  RUN_SKIPPABLE(a_fifo_main_lua_does_not_hang);
  RUN_SKIPPABLE(a_symlink_main_lua_to_a_character_device_does_not_hang);
  RUN(a_hostile_lua_error_is_escaped_before_printing);
  RUN(oracles_log_with_an_embedded_nul_is_escaped_not_truncated);
  RUN(a_mod_cannot_disarm_its_own_instruction_budget_via_debug_sethook);
  RUN(the_debug_library_is_unreachable_by_global_or_require);
  RUN(a_pcall_wrapped_spinner_is_refused_promptly);
  RUN(a_coroutine_resume_wrapped_spinner_is_refused_promptly);
  RUN(a_coroutine_resumed_only_once_still_counts_as_exhausted);
  RUN(a_fresh_coroutine_per_iteration_is_refused_promptly);
  RUN(many_fresh_coroutines_per_iteration_are_refused_promptly);
  RUN(a_pcall_wrapped_coroutine_wrap_spinner_is_refused_promptly);
  RUN(coroutine_is_nil_in_a_loaded_mod_environment);
  RUN(the_coroutine_library_is_unreachable_by_global_or_require);
  RUN(the_oracles_table_matches_the_registry_exactly);
  RUN(a_single_huge_allocation_is_refused_without_growing_first);
  RUN(repeated_allocate_and_free_does_not_ratchet_toward_the_cap);
  RUN(a_table_error_object_still_produces_detail_on_stderr);
  RUN(a_bare_error_call_still_produces_detail_on_stderr);
  RUN(mods_api_name_rejects_out_of_range_indices);
  RUN(oracles_log_reaches_stderr_not_just_mods_last_log);
  RUN(oracles_log_with_a_non_string_argument_is_a_clean_lua_error);
  RUN(untrusted_lua_never_runs_for_a_mod_that_does_not_apply);
  RUN(a_gc_finalizer_cannot_be_armed_at_load_time);
  RUN(a_gc_finalizer_cannot_be_armed_before_a_refusal);
  RUN(a_gc_finalizer_cannot_be_armed_mid_run);
  RUN(load_is_withheld);
  RUN(a_binary_main_lua_is_refused_safely);
  RUN(dofile_is_withheld);
  RUN(io_is_withheld);
  RUN(os_is_withheld);
  RUN(require_is_withheld);
  RUN(package_is_withheld);
  RUN(loadfile_is_withheld);
  RUN(print_is_withheld);
  RUN(warn_is_withheld);
  RUN(utf8_is_withheld);
  RUN(string_dump_is_withheld);
  RUN(package_loaded_is_scrubbed_for_every_withheld_name);
  RUN(the_sandbox_holds);
  RUN(the_global_environment_is_exactly_the_granted_set);
  RUN(a_cheap_infinite_loop_is_caught_by_the_instruction_budget_specifically);
  RUN(the_live_oracles_table_in_a_loaded_mod_matches_the_registry_exactly);
  RUN(a_hostile_tostring_metamethod_does_not_hang_error_reporting);
  RUN(array_growth_is_refused_via_the_realloc_path);
  RUN(a_catastrophic_find_pattern_is_refused_by_the_matcher_itself);
  RUN(a_catastrophic_gsub_pattern_is_refused_by_the_matcher_itself);
  RUN(a_legitimately_heavy_pattern_still_loads);
  RUN(a_mod_sees_frames_and_draws);
  RUN(room_changes_fire_once);
  RUN(a_healthy_mod_runs_many_frames_without_reusing_a_dead_thread);
  RUN(screen_and_game_are_sane_before_the_first_frame);
  RUN(mods_frame_with_a_null_frame_is_a_safe_no_op);
  RUN_SKIPPABLE(draw_icon_with_no_wram_does_not_crash);
  RUN(draw_text_with_no_font_draws_nothing);
  RUN(draw_rect_clamps_rather_than_wrapping_a_huge_width);
  RUN(draw_text_truncates_at_an_embedded_nul);
  RUN(rd_outside_the_known_windows_reads_as_zero);
  RUN(a_bare_global_assignment_never_runs_as_a_handler);
  RUN(re_registering_on_frame_replaces_the_previous_handler);
  RUN(on_room_change_is_skipped_when_never_registered);
  RUN(last_room_resets_across_a_reinit);
  RUN(a_pcall_wrapped_spinner_inside_on_frame_is_poisoned_promptly);
  RUN(a_coroutine_wrapped_spinner_inside_on_frame_fails_on_the_nil_global);
  RUN(a_pathologically_expensive_on_frame_handler_is_poisoned_after_one_bounded_stall);
  return skipped ? 77 : 0;
}
