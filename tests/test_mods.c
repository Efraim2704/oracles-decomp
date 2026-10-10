#include "unit.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "mods/mods.h"
#include "mods/overlay.h"
#include "ui/ui.h"
#include "wide/item.h"
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

static void clear_mods(void) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s/test_mods_root'", tmp);
  (void)system(cmd);
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

// A folder name outside [a-z0-9-] is skipped before its mod.ini is even opened.
static void a_badly_named_folder_is_skipped(void) {
  clear_mods();
  const char *root = make_mod("Bad_Name", "id=Bad_Name\nname=X\napi=1\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// api is the compatibility gate: absence must refuse, not silently default to 0 and pass.
static void a_missing_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("noapi", "id=noapi\nname=NoApi\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// atoi("1x") is 1 and would incorrectly pass; strtol with endptr checking must not accept it.
static void a_non_numeric_api_is_refused(void) {
  clear_mods();
  const char *root = make_mod("badapi", "id=badapi\nname=BadApi\napi=1x\ngames=ages,seasons\n", "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 0);
  mods_shutdown();
}

// Captures everything written to stderr while running `body`. Needed specifically for the test
// below: a mod with no "games=" key is refused either way, because the later "not for this game"
// check (ages/seasons both false) refuses it too -- so a bare mods_count() == 0 assertion cannot
// tell "the dedicated no-supported-game check ran" apart from "it was deleted and the other check
// happened to catch this case anyway". Confirmed by mutation: deleting that check left every
// existing assertion passing.
static void capture_stderr(void (*body)(void), char *out, size_t out_size) {
  fflush(stderr);
  char path[] = "/tmp/test_mods_stderr.XXXXXX";
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
static void run_cap_mods_init(void) { ASSERT(mods_init(cap_root, cap_seasons)); }

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

// Matches this project's ini conventions (settings_parse): leading whitespace, spaces around
// '=', and an indented comment are all tolerated; unlike settings_parse's %255s, interior
// spaces in a value (a mod's display name) survive.
static void whitespace_and_comments_are_tolerated(void) {
  clear_mods();
  const char *root = make_mod("spacey",
      "  id = spacey  \n"
      "  name = Hello World  \n"
      "   # indented comment, with leading whitespace before the '#'\n"
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
// mods.c reads lines into a 256-byte buffer, so fgets fills exactly 255 usable bytes before
// returning without having seen a newline. The padding length below (250) is chosen so that
// "note=" (5 bytes) plus the padding lands at exactly that 255-byte boundary, with "api=99\n"
// starting clean on the far side of it -- the one split point where a leaked tail would be
// re-parsed as the *exact* key "api" rather than some longer garbled key that no known field
// matches. Confirmed by mutation: removing the discard-the-rest-of-the-line step and rerunning
// with an unaligned (280-byte) padding did not fail this test, because the leaked fragment's key
// was "<partial padding>api", not "api"; only this precise length makes the test discriminate.
static void an_overlong_line_does_not_leak_into_the_next_key(void) {
  clear_mods();
  char padding[251];
  memset(padding, 'x', sizeof padding - 1);
  padding[sizeof padding - 1] = 0;
  char ini[700];
  snprintf(ini, sizeof ini,
      "id=longline\nname=LongLine\napi=1\ngames=ages,seasons\nnote=%sapi=99\n", padding);
  const char *root = make_mod("longline", ini, "\n");
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), 1);
  mods_shutdown();
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
// but it is the 33rd one specifically that is dropped, not just "scanning stopped early" --
// which is why this creates MODS_MAX + 1 mods and checks the count lands exactly on MODS_MAX.
static void loading_stops_at_mods_max(void) {
  clear_mods();
  const char *root = NULL;
  for (int i = 0; i < MODS_MAX + 1; i++) {
    char id[16];
    snprintf(id, sizeof id, "m%02d", i);
    char ini[128];
    snprintf(ini, sizeof ini, "id=%s\napi=1\ngames=ages,seasons\n", id);
    root = make_mod(id, ini, "\n");
  }
  ASSERT(mods_init(root, true));
  ASSERT_EQ(mods_count(), MODS_MAX);
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
  RUN(a_missing_mods_directory_is_not_a_failure);
  RUN(an_empty_mods_directory_has_no_mods);
  RUN(a_mod_is_found_and_described);
  RUN(a_newer_api_is_refused);
  RUN(the_other_games_mod_is_skipped);
  RUN(a_traversing_ini_id_is_refused);
  RUN(a_mismatched_ini_id_is_refused);
  RUN(a_badly_named_folder_is_skipped);
  RUN(a_missing_api_is_refused);
  RUN(a_non_numeric_api_is_refused);
  RUN(a_missing_games_is_refused);
  RUN(a_substring_game_token_is_refused);
  RUN(whitespace_and_comments_are_tolerated);
  RUN(an_overlong_line_does_not_leak_into_the_next_key);
  RUN(a_stray_file_is_skipped);
  RUN(mods_load_in_alphabetical_order);
  RUN(loading_stops_at_mods_max);
  return skipped ? 77 : 0;
}
