// The European ROMs on the native build. (Skipped without them.)
//
// There is no European movie, so the game state comes from a USA one: its file data at a frame of
// the movie goes in the European battery RAM's first slot (the file format is the same), the
// European ROM boots, picks a language on its language screen and loads that file. From there, as
// test_rooms does for the USA ROMs, every group and room is warped to and run a little: a room
// whose code the native build has no C for stops the engine and fails the test. Each room that
// settles is also decoded from the ROM alone (wide/room.c, which reads the European tables) and
// compared with what the game loaded, as test_room does along the USA movies.
// EU_LANGUAGE=n picks the language (0 English .. 4 Spanish); EU_DIAG=1 lists the rooms that differ;
// EU_ROOM=g:rr runs one room.
#include "unit.h"
#include "core/gb.h"
#include "platform/tas.h"
#include "platform/setup.h"
#include "hooks/hooks.h"
#include "game/game.h"
#include "assets/assets.h"
#include "rt/fibers.h"
#include "wide/room.h"
#include "wide/item.h"
#include <stdlib.h>

#define GROUPS 8
#define ROOMS 256
#define ROOM_FRAMES 80
#define FILE_START 0x5b0         // wFileStart - $c000: the file data the game saves
#define FILE_SIZE 0x550
#define LANGUAGE 0x62a           // wActiveLanguage
#define WHICH_GAME 0x611         // wWhichGame

static uint8_t tas_cb(void *ctx, uint64_t frame) { return tas_input_at((const Tas *)ctx, frame); }

// The file data of a USA movie at a frame, as a save of language `lang` for the given game.
static bool usa_file_at(const char *rom_path, const char *inputs, const char *boot_state, uint64_t frames,
                        bool seasons, int lang, uint8_t block[FILE_SIZE]) {
  size_t n;
  uint8_t *rom = oracles_read_file(rom_path, &n);
  Tas t;
  if (!rom || !tas_load(&t, inputs)) { free(rom); return false; }
  GB *gb = calloc(1, sizeof *gb);
  gb_init(gb);
  bool ok = gb_load_rom(gb, rom, n);
  gb->cyctab = cyctab_alloc(rom, n);
  assets_zero_code(rom, n);
  gb_reset(gb);
  ok = ok && oracles_load_boot_state(gb, boot_state);
  gb->input_at = tas_cb; gb->input_ctx = &t;
  for (uint64_t i = 0; ok && i < frames && !gb->hung; i++) gb_run_frame(gb);
  ok = ok && !gb->hung;
  memcpy(block, &gb->wram[0][FILE_START], FILE_SIZE);
  block[LANGUAGE - FILE_START] = (uint8_t)lang;
  block[WHICH_GAME - FILE_START] = seasons ? 0 : 1;
  memcpy(block + 2, seasons ? "Z11216-0" : "Z21216-0", 8);
  unsigned sum = 0;
  for (int o = 2; o < FILE_SIZE; o += 2) sum += block[o] | block[o + 1] << 8;
  block[0] = (uint8_t)sum;
  block[1] = (uint8_t)(sum >> 8);
  free(gb); free(rom); tas_free(&t);
  return ok;
}

// Start, Start (past the intro and the title), the language screen (down `lang` times, A), then A
// on file 1 and its confirmation.
typedef struct { int lang; } Script;
static uint8_t script_cb(void *ctx, uint64_t f) {
  const Script *s = ctx;
  enum { PAD_A = 1, PAD_START = 8, PAD_DOWN = 128 };
  if ((f >= 300 && f < 304) || (f >= 480 && f < 484)) return PAD_START;
  for (int i = 0; i < s->lang; i++)
    if (f >= 540 + 10u * i && f < 544 + 10u * i) return PAD_DOWN;
  if (f >= 590 && f < 594) return PAD_A;
  if (f >= 680 && f < 1100 && (f - 680) % 60 < 4) return PAD_A;
  return 0;
}

typedef struct { int rooms, tiles_exact, gfx_exact, pal_exact; } Score;

// The room the game has settled in against the ROM decoder's version of it
static void compare_room(GB *gb, const uint8_t *rom, size_t size, bool seasons, int lang, int group, int room, Score *sc) {
  const uint8_t *w = gb->wram[0];
  int split = gb->hram[(seasons ? 0xff99 : 0xff9b) - 0xff80];
  bool settled = (w[0xd00] & 0x0f) == 0x01 && (split == 2 || split == 3) && w[0x4ab] == 0 && w[0xbcb] == 0;
  int g = w[seasons ? 0xc49 : 0xc2d], r = w[seasons ? 0xc4c : 0xc30], modifier = w[seasons ? 0xc4e : 0xc32];
  if (!settled || g != group || r != room) return;
  WideRoomKey key = {group, room, modifier & 3, 0, 0, false, false, 0, lang};
  static WideRoom d;
  if (!wide_decode_room(rom, size, seasons, &key, &d)) return;
  int width = w[0xd0a], height = w[0xd0b], bad_tiles = 0, bad_gfx = 0, bad_pal = 0;
  if (width > 32 || height > 24) return;
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      int k = y * 32 + x;
      if (d.tiles[k] != gb->wram[3][0x800 + k] || d.attrs[k] != gb->wram[3][0xc00 + k]) bad_tiles++;
    }
  for (int bank = 0; bank < 2; bank++)
    for (int t = 0; t < 0x180; t++)
      if (d.vram_set[bank][t] && !d.animated[bank][t] && memcmp(&d.vram[bank][t * 16], &gb->vram[bank][t * 16], 16)) bad_gfx++;
  for (int p = 0; p < 8; p++)
    if (d.pal_set[p] && memcmp(&d.bg_pal[p * 8], &gb->wram[2][0xe80 + p * 8], 8)) bad_pal++;
  sc->rooms++;
  sc->tiles_exact += !bad_tiles;
  sc->gfx_exact += !bad_gfx;
  sc->pal_exact += !bad_pal;
  if ((bad_tiles || bad_gfx || bad_pal) && getenv("EU_DIAG"))
    fprintf(stderr, "    %s group %d room %02x mod %d: %d tiles, %d gfx tiles, %d palettes differ\n",
            seasons ? "S" : "A", group, room, modifier, bad_tiles, bad_gfx, bad_pal);
}

// wWarpDestGroup..wWarpTransition2, five consecutive bytes the game warps on
static void warp_to(GB *gb, uint16_t dest, int group, int room) {
  uint8_t *w = &gb->wram[0][dest - 0xc000];
  w[0] = (uint8_t)(0x80 | group);
  w[1] = (uint8_t)room;
  w[2] = 0x00;
  w[3] = 0x44;
  w[4] = 0x01;
}

static void sweep(const char *game, const char *eu_path, const char *us_path, const char *inputs, const char *boot_state,
                  uint64_t file_frame, uint16_t warp_dest, bool seasons) {
  int lang = getenv("EU_LANGUAGE") ? atoi(getenv("EU_LANGUAGE")) % 5 : 4;
  size_t n;
  uint8_t *rom = oracles_read_file(eu_path, &n);
  if (!rom) SKIP("European ROM not present");
  static uint8_t block[FILE_SIZE];
  if (!usa_file_at(us_path, inputs, boot_state, file_frame, seasons, lang, block)) SKIP("USA ROM or movie missing");
  GB *gb = calloc(1, sizeof *gb), *base = calloc(1, sizeof *base);
  gb_init(gb);
  ASSERT(gb_load_rom(gb, rom, n));
  gb->cyctab = cyctab_alloc(rom, n);
  assets_zero_code(rom, n);                 // as the app does; the decoder reads this copy too
  gb_reset(gb);
  memcpy(&gb->eram[0x0010], block, FILE_SIZE);           // file 1 and its backup
  memcpy(&gb->eram[0x1000], block, FILE_SIZE);
  Script script = {lang};
  gb->input_at = script_cb; gb->input_ctx = &script;
  for (uint64_t i = 0; i < 1800 && !gb->hung; i++) gb_run_frame(gb);
  ASSERT(!gb->hung);
  ASSERT_EQ(gb->wram[0][LANGUAGE], lang);
  ASSERT((gb->wram[0][0xd00] & 0x0f) == 0x01);           // in normal play
  gb->input_at = NULL; gb->input_ctx = NULL; gb->joy = 0;
  static DispatchRing ring;
  if (getenv("EU_DIAG")) gb->ring = &ring;               // the last dispatches before a stop
  *base = *gb;

  int failures = 0, only_group = -1, only_room = -1;
  if (getenv("EU_ROOM")) sscanf(getenv("EU_ROOM"), "%x:%x", &only_group, &only_room);   // EU_ROOM=g:rr, one room
  Score sc = {0, 0, 0, 0};
  for (int group = 0; group < GROUPS; group++)
    for (int room = 0; room < ROOMS; room++) {
      if (only_group >= 0 && (group != only_group || room != only_room)) continue;
      oracles_copy_state(gb, base);
      fibers_reset(gb);
      gb_stop_reason[0] = '\0';
      warp_to(gb, warp_dest, group, room);
      for (int f = 0; f < ROOM_FRAMES && !gb->hung; f++) gb_run_frame(gb);
      if (gb->hung) {
        fprintf(stderr, "  %s (Europe) group %x room %02x: %s\n", game, group, room, gb_stop_reason[0] ? gb_stop_reason : "engine stopped");
        for (unsigned i = ring.n > 24 ? ring.n - 24 : 0; gb->ring && i < ring.n; i++)
          fprintf(stderr, "    %02x:%04x %s\n", ring.r[i % 64].bank, ring.r[i % 64].addr, ring.r[i % 64].name);
        failures++;
        continue;
      }
      compare_room(gb, rom, n, seasons, lang, group, room, &sc);
    }
  fprintf(stderr, "  %s (Europe), language %d: %d of %d rooms stopped the engine; %d rooms settled: tiles exact %d, gfx exact %d, palettes exact %d\n",
          game, lang, failures, GROUPS * ROOMS, sc.rooms, sc.tiles_exact, sc.gfx_exact, sc.pal_exact);
  ASSERT_EQ(failures, 0);
  if (only_group >= 0) { free(gb); free(base); free(rom); return; }
  // Spanish, at 60000 frames into the movies: Ages 1,831 rooms settle, 94.7% with exact tiles, 98.2%
  // exact gfx, 99.5% exact palettes; Seasons 2,047 rooms, 96.6%, 99.95%, 100%. The rest is what the
  // save changed (chests, doors), which the ROM alone cannot show.
  ASSERT(sc.rooms > 1500);
  ASSERT(sc.tiles_exact * 100 >= sc.rooms * 90);
  ASSERT(sc.gfx_exact * 100 >= sc.rooms * 95);
  ASSERT(sc.pal_exact * 100 >= sc.rooms * 98);
  free(gb); free(base); free(rom);
}

#define AGES_EU GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Ages (Europe) (En,Fr,De,Es,It).gbc"
#define SEASONS_EU GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Seasons (Europe) (En,Fr,De,Es,It).gbc"

static void ages_eu_every_room(void) {
  sweep("ages", AGES_EU, GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Ages (USA, Australia).gbc",
        TAS_DIR "/ages-consoleverified.inputs", TAS_DIR "/ages-boot.state", 60000, 0xcc47, false);
}

static void seasons_eu_every_room(void) {
  sweep("seasons", SEASONS_EU, GAME_ROM_DIR "/Legend of Zelda, The - Oracle of Seasons (USA, Australia).gbc",
        TAS_DIR "/seasons-consoleverified.inputs", TAS_DIR "/seasons-consoleverified-boot.state", 60000, 0xcc63, true);
}

// The status bar's item icons from the European tables (wide/item.c)
static void eu_item_icons_decode_from_the_rom(void) {
  const char *paths[2] = {AGES_EU, SEASONS_EU};
  for (int g = 0; g < 2; g++) {
    size_t n;
    uint8_t *rom = oracles_read_file(paths[g], &n);
    if (!rom) SKIP("European ROM not present");
    static uint8_t c600[256];
    memset(c600, 1, sizeof c600);
    WideItemIcon ic;
    bool seasons = g == 1;
    ASSERT(!wide_item_icon(rom, n, seasons, 0, c600, &ic));
    ASSERT(wide_item_icon(rom, n, seasons, 0x03, c600, &ic));           // bombs
    ASSERT_EQ(ic.halves, 1);
    ASSERT_EQ(ic.pal[0], 4);
    ASSERT(wide_item_icon(rom, n, seasons, 0x19, c600, &ic));           // seed satchel: the bag and a seed
    ASSERT_EQ(ic.halves, 2);
    free(rom);
  }
}

int main(void) {
  RUN(eu_item_icons_decode_from_the_rom);
  RUN(ages_eu_every_room);
  RUN(seasons_eu_every_room);
  return 0;
}
