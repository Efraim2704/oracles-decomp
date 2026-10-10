#include "unit.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "mods/mods.h"
#include "mods/overlay.h"
#include "ui/ui.h"
#include "wide/item.h"
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
  ASSERT(dup2(fd, fileno(stderr)) >= 0);
  body();
  fflush(stderr);
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
  mods_shutdown();
}

// A mod written against a newer API is refused rather than half-running.
static void a_newer_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("future", "id=future\nname=Future\napi=99\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
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
  // Print directly with write(2), not fprintf: inside a signal handler, stdio is not safe to
  // call (fprintf may be mid-buffer-flush when the signal lands), and _exit (not exit) skips
  // atexit/stdio teardown for the same reason.
  const char msg[] = "FAIL: mods_init hung (did not return within the timeout)\n";
  write(2, msg, sizeof msg - 1);
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

int main(void) {
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
  return skipped ? 77 : 0;
}
