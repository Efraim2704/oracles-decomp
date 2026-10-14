#include "wide/npcs.h"
#include "game/game.h"
#include "platform/setup.h"
#include "rt/fibers.h"
#include "hw/render.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { GIVE_UP_AFTER = 120, SETTLE = 3, CACHED = 8, KEEP_VISITS = 3, REFRESH_HERE = 60,
       SCROLL_NORMAL = 0x01, CUTSCENE_INGAME = 0x01, HERE = 0, DIR_RIGHT = 1, DIR_LEFT = 3 };

// The copy's hand-over: the main thread prepares it (IDLE to READY), the worker runs it (RUNNING)
// and leaves its picture in `staged` (DONE), the main thread files that away (back to IDLE).
enum { IDLE, READY, RUNNING, DONE };

typedef struct {
  bool used, ok;
  uint64_t visit;               // which of Link's room visits it was made for
  uint64_t made;                // the update it was started on
  WideRoomSprites sprites;
} Cached;

static struct {
  GB *copy;
  atomic_int state;
  int group, target;
  bool ok;
  uint64_t generation;
  WideRoomSprites staged;
} job;

static Cached cache[CACHED];
static int last_group = -1, last_room = -1;
static uint64_t visits, generation, updates;

void wide_npcs_reset(void) {
  memset(cache, 0, sizeof cache);
  last_group = last_room = -1;
  generation++;
}

void wide_npcs_stop(void) {
  int ready = READY;
  atomic_compare_exchange_strong(&job.state, &ready, IDLE);
  while (atomic_load(&job.state) == RUNNING) {}
  atomic_store(&job.state, IDLE);
  wide_npcs_reset();
}

const WideRoomSprites *wide_npcs_room(int group, int room) {
  for (int i = 0; i < CACHED; i++)
    if (cache[i].used && cache[i].ok && cache[i].sprites.group == group && cache[i].sprites.room == room) return &cache[i].sprites;
  return NULL;
}

static Cached *entry(int group, int room) {
  for (int i = 0; i < CACHED; i++)
    if (cache[i].used && cache[i].sprites.group == group && cache[i].sprites.room == room) return &cache[i];
  return NULL;
}

static Cached *oldest(void) {
  Cached *best = &cache[0];
  for (int i = 0; i < CACHED; i++) {
    if (!cache[i].used) return &cache[i];
    if (cache[i].visit < best->visit) best = &cache[i];
  }
  return best;
}

// The room beside this one on the overworld's grid (Ages 14 wide, Seasons 16), or -1
static int beside(int room, int dir) {
  int col = (room & 0x0f) + (dir == DIR_RIGHT ? 1 : -1), last = game_seasons ? 15 : 13;
  return col < 0 || col > last ? -1 : (room & 0xf0) | col;
}

// The copy's own objects, from its sprite list (render.h: offscreen_room_objects), in room
// coordinates.
static void snapshot(GB *gb, WideRoomSprites *out) {
  const GBSample *s = gb->sample;
  for (int y = 0; y < 176; y++)
    for (int x = 0; x < 256; x++) out->c[y][x] = -1;
  static int16_t c[WIDE_W];
  static bool behind[WIDE_W];
  for (int y = 0; y < FB_H; y++) {
    int rx, ry;
    if (!wide_room_origin(s, game_seasons, y, &rx, &ry) || ry < 0 || ry >= 176) continue;
    wide_sprite_row(s, y, c, behind);
    for (int x = 0; x < WIDE_W; x++) {
      int lx = rx + x - WIDE_SIDE;
      if (c[x] < 0 || lx < 0 || lx >= 256) continue;
      out->c[ry][lx] = c[x];
      out->behind[ry][lx] = behind[x];
    }
  }
}

// wCutsceneIndex: only normal play (not a scripted scene) runs a requested screen transition
static uint8_t cutscene(GB *gb) { return gb->wram[0][(wThreadStateBuffer + 0xf) & 0xfff]; }

static void request_transition(GB *gb, int dir) { W8(wScreenTransitionDirection) = (uint8_t)(0x80 | dir); }

static void prepare(GB *gb, int dir, int target) {
  if (!job.copy) {
    job.copy = calloc(1, sizeof *job.copy);
    gb_init(job.copy);
    gb_load_rom(job.copy, gb->rom, gb->rom_size);
    job.copy->cyctab = gb->cyctab;
    gb_reset(job.copy);
    render_line_regs(job.copy)->offscreen_room_objects = true;
  }
  oracles_copy_state(job.copy, gb);
  render_line_regs(job.copy)->offscreen_random = 0;
  job.copy->rom = gb->rom;
  job.copy->rom_size = gb->rom_size;
  job.copy->cyctab = gb->cyctab;
  fibers_reset(job.copy);
  // the same game: keep the copy from rebuilding the hook tables the game is using
  job.copy->hooks_checked = job.copy->hooks_ok = true;
  job.copy->joy = 0;
  job.copy->input_at = NULL;
  if (dir != HERE) request_transition(job.copy, dir);
  job.group = W8(wActiveGroup);
  job.target = target;
  job.generation = generation;
  // a room seen before keeps its old picture until the new one is in
  Cached *c = entry(job.group, target);
  if (!c) {
    c = oldest();
    memset(c, 0, sizeof *c);
    c->used = true;
    c->sprites.group = job.group;
    c->sprites.room = target;
  }
  c->visit = visits;
  c->made = updates;
  atomic_store(&job.state, READY);
}

void wide_npcs_work(void) {
  int ready = READY;
  if (!atomic_compare_exchange_strong(&job.state, &ready, RUNNING)) return;
  GB *gb = job.copy;
  job.ok = false;
  for (int frames = 0, settled = 0; frames < GIVE_UP_AFTER && !gb->hung; frames++) {
    gb_run_frame(gb);
    bool there = W8(wActiveGroup) == job.group && W8(wActiveRoom) == job.target && (W8(wScrollMode) & 0x0f) == SCROLL_NORMAL;
    if (there && ++settled > SETTLE) {
      snapshot(gb, &job.staged);
      job.staged.group = job.group;
      job.staged.room = job.target;
      job.ok = true;
      break;
    }
  }
  atomic_store(&job.state, DONE);
}

static void file_away(void) {
  Cached *c = job.generation == generation ? entry(job.group, job.target) : NULL;
  if (c && job.ok) {
    c->sprites = job.staged;
    c->ok = true;
  }
  atomic_store(&job.state, IDLE);
}

bool wide_npcs_update(GB *gb) {
  updates++;
  int state = atomic_load(&job.state);
  if (state == DONE) file_away();
  else if (state != IDLE) return false;
  int group = W8(wActiveGroup), room = W8(wActiveRoom);
  if (group > 1) { last_group = group; return false; }
  if (group != last_group || room != last_room) { visits++; last_group = group; last_room = room; }
  if ((W8(wScrollMode) & 0x0f) != SCROLL_NORMAL || cutscene(gb) != CUTSCENE_INGAME || W8(wOpenedMenuType) || W8(wTextIsActive) ||
      !threads_parked(gb))
    return false;
  static const int dirs[2] = {DIR_RIGHT, DIR_LEFT};
  for (int i = 0; i < 2; i++) {
    int target = beside(room, dirs[i]);
    if (target < 0) continue;
    Cached *c = entry(group, target);
    if (c && c->visit + KEEP_VISITS > visits) continue;
    prepare(gb, dirs[i], target);
    return true;
  }
  // The room Link is in, kept recent: once he leaves it, it is beside the screen at once, with the
  // characters as they stood a moment ago rather than none until a copy has gone back in.
  Cached *here = entry(group, room);
  if (!here || updates - here->made >= REFRESH_HERE) {
    prepare(gb, HERE, room);
    return true;
  }
  return false;
}
