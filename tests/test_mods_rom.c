// The two tests/test_mods.c tests that need the real ROM, split into their own executable so that
// `test_mods` itself can stay unlabelled and run under `ctest -LE rom` (and so, on a machine with
// no ROM -- which is what CI is -- the ~150 ROM-free mod tests actually run under that gate instead
// of ctest skipping the whole `mods` test before it even starts). See CMakeLists.txt: this target
// carries LABELS "rom" and SKIP_RETURN_CODE 77, same as every other ROM-dependent *_test.
//
// Deliberately a separate root temp directory ("test_mods_rom_root", not test_mods.c's
// "test_mods_root") even though the two executables are never expected to touch the same mod ids:
// ctest may run tests in parallel, and clear_mods()'s `rm -rf` racing against the other
// executable's make_mod()'s `mkdir -p`/`fopen` on a shared directory would be a flaky, spurious
// failure that has nothing to do with either test's own assertions.
#include "unit.h"
#include "mods/mods.h"
#include "mods/overlay.h"
#include "wide/item.h"
#include <stdbool.h>

// Mirrors test_mods.c's own SKIP_TEST/RUN_SKIPPABLE: unit.h's SKIP is exit(77), which would abort
// this process before the second test ever ran. See test_mods.c's comment on the same pair for
// the full rationale; kept in sync here rather than shared, since the two files do not otherwise
// share a header of their own.
static int skipped;
#define SKIP_TEST(msg) do { printf("skip: %s\n", msg); return false; } while (0)
#define RUN_SKIPPABLE(fn) do { if (fn()) printf("ok %s\n", #fn); else skipped = 1; } while (0)

// Writes <dir>/<id>/{mod.ini,main.lua} and returns <dir>. Copied from test_mods.c's make_mod,
// with its own root so this executable never shares a directory with test_mods's.
static const char *make_mod(const char *id, const char *ini, const char *lua) {
  static char root[512];
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  snprintf(root, sizeof root, "%s/test_mods_rom_root", tmp);
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
  char cmd[700];
  snprintf(cmd, sizeof cmd, "chmod -R u+rwX '%s/test_mods_rom_root' 2>/dev/null; rm -rf '%s/test_mods_rom_root'", tmp, tmp);
  (void)system(cmd);
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

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  RUN_SKIPPABLE(overlay_draws_an_item_icon);
  RUN_SKIPPABLE(draw_icon_with_no_wram_does_not_crash);
  return skipped ? 77 : 0;
}
