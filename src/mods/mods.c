#include "mods/mods.h"
#include "mods/api.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// id doubles as the folder name and, from Task 9 on, a path component for a mod's store file.
// This is the gate against NAME-based path traversal: no byte this rejects can spell ".." or
// "/" or "\", so a validated id is unrepresentable as a traversal via its characters, on either
// a POSIX host or the MinGW/Windows target, where "\" is also a separator. It says nothing about
// a mod *folder* that is itself a symlink pointing outside the mods directory -- that is a
// separate concern, owned by Task 9's zip installer and the per-mod store writer, not this check.
bool mods_id_valid(const char *id) {
  if (!id || !*id) return false;
  for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
    bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
    if (!ok) return false;
  }
  return true;
}

#define MOD_FIELD_LEN 64

typedef struct {
  char id[MOD_FIELD_LEN];     // canonical: the validated folder name, never the mod.ini field directly
  char name[MOD_FIELD_LEN];
  char ini_id[MOD_FIELD_LEN]; // raw "id=" value from mod.ini (escaped; see mods_escape_into), kept
                              // only to cross-check against id
  bool ini_id_present;
  bool enabled;         // always true today; Task 6 adds mods.ini-based enable/disable and will be
                        // the first thing that ever clears this -- not a bug that nothing does yet.
  bool ages, seasons;
  bool api_present;     // true iff an "api=" line appeared at all
  bool api_parsed;      // true iff that value parsed as a well-defined int (meaningless unless
                        // api_present)
  int api;              // the parsed value iff api_parsed; otherwise unspecified. Whether it's an
                        // *acceptable* value (>= 1, <= MODS_API_VERSION) is mods_init/load_one's
                        // call, not read_ini's -- see the api= branch below for why that split
                        // matters.
  lua_State *L;         // NULL until run_main_lua succeeds; only mods actually stored in mods[]
                        // (post game/cap filtering) ever get one.
} Mod;

static Mod mods[MODS_MAX];
static int count;
// Set by mods_init, still read by nothing: a future oracles.game() binding is what eventually
// consumes it, so this is an intentional dead store for now, not an oversight.
static bool host_seasons;

static void trim(char *s) {
  char *start = s;
  while (*start == ' ' || *start == '\t') start++;
  size_t len = strlen(start);
  while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
  start[len] = 0;
  if (start != s) memmove(s, start, len + 1);
}

// See mods.h for why this exists and why it is shared with api.c. Takes an explicit src_len
// (rather than relying on strlen) so a hostile value containing an embedded NUL -- a mod.ini
// value can't, since contains_nul already refuses those, but a Lua string from oracles.log() or
// a failed main.lua's error text can -- is escaped in full rather than silently cut short at the
// first NUL byte.
void mods_escape_into(char *dst, size_t dst_size, const char *src, size_t src_len) {
  size_t o = 0;
  for (size_t i = 0; i < src_len && o + 1 < dst_size; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 0x20 && c < 0x7f) {
      dst[o++] = (char)c;
    } else {
      char esc[5];
      snprintf(esc, sizeof esc, "\\x%02x", c);
      for (int j = 0; esc[j] && o + 1 < dst_size; j++) dst[o++] = esc[j];
    }
  }
  dst[o] = 0;
}

// Reads one logical line into `buf` (size n), discarding anything past the buffer on an
// over-long line rather than leaving the tail for the next call to misparse as its own
// key=value pair. Returns false at EOF with nothing read. Relies on the caller having already
// refused any mod.ini containing an embedded NUL (see contains_nul below): strlen(buf) stops at
// the first NUL, which would otherwise silently defeat the "buffer is exactly full" check this
// function uses to detect an over-long line, letting a NUL-padded tail smuggle a fake key=value
// past the discard.
static bool read_logical_line(FILE *f, char *buf, size_t n) {
  if (!fgets(buf, (int)n, f)) return false;
  size_t len = strlen(buf);
  if (len == n - 1 && buf[len - 1] != '\n') {
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {}
  }
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
  return true;
}

// Scans the whole file for an embedded NUL byte, in bounded chunks so an attacker-sized mod.ini
// can't force an unbounded allocation. Leaves the file position wherever reading stopped --
// callers that go on to parse the file must rewind() first.
static bool contains_nul(FILE *f) {
  unsigned char buf[8192];
  size_t nread;
  while ((nread = fread(buf, 1, sizeof buf, f)) > 0)
    if (memchr(buf, 0, nread)) return true;
  return false;
}

// mod.ini is the same shape as settings.ini: key=value, '#' comments, blanks ignored, whitespace
// tolerated around both key and value. Unlike settings_parse's `%31[a-z_] = %255s` approach, this
// keeps interior spaces in the value, since mod names and authors contain them. Every value is
// treated as hostile: over-long, empty, or full of coincidental '=' or '#' characters are all
// handled, not just well-formed input. `label` is the mod's folder name, used only to name the
// mod in this function's own error messages and in a per-token "games=" warning.
static bool read_ini(const char *path, const char *label, Mod *m) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "mods: '%s' has no mod.ini, skipping\n", label);
    return false;
  }
  if (contains_nul(f)) {
    fclose(f);
    fprintf(stderr, "mods: '%s/mod.ini' contains a NUL byte, refusing\n", label);
    return false;
  }
  rewind(f);

  char line[256];
  while (read_logical_line(f, line, sizeof line)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    // The whitespace-skip above means an indented '#' is already at `p` by the time this check
    // runs, so this is a readability nicety, not a safety boundary: `key` below always starts at
    // `p`, trim() never strips '#', so a comment's "key" (e.g. "# api" from "# api=99") can never
    // collide with a real keyword even if this exact check were deleted -- confirmed by mutation.
    // The actual safety net for both comments and blank lines is the `!eq` fallthrough just below.
    if (*p == '#' || *p == 0) continue;

    char *eq = strchr(p, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = p, *value = eq + 1;
    trim(key);
    trim(value);
    if (!*key) continue;

    if (!strcmp(key, "id")) {
      mods_escape_into(m->ini_id, sizeof m->ini_id, value, strlen(value));
      m->ini_id_present = true;
    } else if (!strcmp(key, "name")) {
      mods_escape_into(m->name, sizeof m->name, value, strlen(value));
    } else if (!strcmp(key, "api")) {
      // This function's job is only to establish whether the (int) cast is well-defined and to
      // record that the key was present at all -- not whether the resulting number is an
      // *acceptable* api version. Folding that policy decision in here is what previously made
      // "api=0" (a well-formed number) report as "missing or non-numeric" (a syntax complaint),
      // which is simply false, and it hid the >= 1 policy bound behind the same sentinel as every
      // syntax error, destroying mutation signal on the most security-relevant bound in the file.
      // mods_init/load_one owns the >= 1 / <= MODS_API_VERSION policy check instead.
      //
      // The errno ERANGE check matters beyond the obvious "huge decimal string" case: on an
      // LLP64 target (MinGW/Windows), `long` is only 32 bits, the same width as `int`, so strtol
      // silently clamps an overflowing value to LONG_MAX (== INT_MAX there) unless errno is
      // checked -- `v <= INT_MAX` alone can't tell "parsed exactly INT_MAX" apart from "overflowed
      // and got clamped to it", so api=2147483648 would be wrongly accepted on Windows while
      // correctly refused on a 64-bit-long host. Checking errno catches it on both.
      m->api_present = true;
      errno = 0;
      char *end;
      long v = strtol(value, &end, 10);
      bool consumed_all = end != value && *end == 0;
      m->api_parsed = consumed_all && errno != ERANGE && v >= INT_MIN && v <= INT_MAX;
      m->api = m->api_parsed ? (int)v : 0;
    } else if (!strcmp(key, "games")) {
      m->ages = m->seasons = false;
      char buf[256];
      snprintf(buf, sizeof buf, "%s", value);
      for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        trim(tok);
        if (!strcmp(tok, "ages")) m->ages = true;
        else if (!strcmp(tok, "seasons")) m->seasons = true;
        else if (*tok) {
          char esc[128];
          mods_escape_into(esc, sizeof esc, tok, strlen(tok));
          fprintf(stderr, "mods: '%s' names unrecognised game '%s' in its games=, ignoring that token\n",
                  label, esc);
        }
      }
    }
  }
  fclose(f);
  return true;
}

// Validates and fully populates *m for the mod folder `name` directly under `dir`: charset and
// length of the id, that it's actually a directory, that its mod.ini is a regular file (not a
// FIFO, a symlink to a character device, or anything else that could block or loop forever in
// fopen/fgets), free of embedded NUL bytes, parses, declares a matching id, has a usable api= and
// games=, and that main.lua exists and is also a regular file (the exact same FIFO/device-symlink
// hazard that mod.ini faces applies equally to main.lua -- lauxlib's own file loader is just an
// fopen/fread loop underneath, with no protection of its own). Prints its own stderr diagnostic on
// every refusal path. Does not decide whether the mod applies to the game being played, apply the
// MODS_MAX cap, or actually run main.lua -- those depend on state (which game, how many mods are
// already loaded) that spans the whole scan, or are too expensive/risky to do for a mod that
// might not even end up kept, so they stay in mods_init's loop (see run_main_lua below).
static bool load_one(const char *dir, const char *name, Mod *m) {
  if (!mods_id_valid(name)) {
    fprintf(stderr, "mods: '%s' is not a valid mod id (lowercase letters, digits, hyphen only), skipping\n", name);
    return false;
  }
  if (strlen(name) >= MOD_FIELD_LEN) {
    fprintf(stderr, "mods: '%s' is longer than the %d-byte id limit, skipping\n", name, MOD_FIELD_LEN - 1);
    return false;
  }

  char entry_path[1024];
  int n1 = snprintf(entry_path, sizeof entry_path, "%s/%s", dir, name);
  if (n1 < 0 || (size_t)n1 >= sizeof entry_path) {
    fprintf(stderr, "mods: '%s' path too long, skipping\n", name);
    return false;
  }
  struct stat st;
  if (stat(entry_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    fprintf(stderr, "mods: '%s' in the mods folder is not a directory, skipping\n", name);
    return false;
  }

  char ini_path[1040];
  int n2 = snprintf(ini_path, sizeof ini_path, "%s/mod.ini", entry_path);
  if (n2 < 0 || (size_t)n2 >= sizeof ini_path) {
    fprintf(stderr, "mods: '%s' path too long, skipping\n", name);
    return false;
  }

  // Require a regular file before ever calling fopen on it. A FIFO blocks fopen/fread
  // indefinitely waiting for a writer that will never come, and a symlink to a character device
  // (e.g. mod.ini -> /dev/zero) opens fine and then streams forever, since fgets never sees a
  // newline. stat() follows symlinks, so this also catches the symlink case, and it unifies the
  // "mod.ini is actually a directory" message across platforms: on macOS, fopen() on a directory
  // succeeds (reading as if empty), which previously produced a confusing "has no id=" instead of
  // "has no mod.ini", where Linux's fopen just fails outright for the same input.
  struct stat ini_st;
  // Distinguish absent from present-but-unusable. A FIFO, or a symlink to a device, is a
  // mispackaged or hostile mod; telling its author "has no mod.ini" about a file that plainly
  // exists sends them looking for the wrong problem. Test the stat result rather than errno,
  // which is untouched when stat succeeds and S_ISREG is what fails.
  if (stat(ini_path, &ini_st) != 0) {
    fprintf(stderr, "mods: '%s' has no mod.ini (%s), skipping\n", name, strerror(errno));
    return false;
  }
  if (!S_ISREG(ini_st.st_mode)) {
    fprintf(stderr, "mods: '%s' has a mod.ini that is not a regular file, skipping\n", name);
    return false;
  }

  if (!read_ini(ini_path, name, m)) return false;    // read_ini already printed why

  // The folder name is the canonical id, and mod.ini must declare the same id itself -- a mod
  // package should say who it is, not rely solely on whatever folder it happens to be unpacked
  // into. mod.ini's "id=" is never used to build a path, so a traversing value here (e.g.
  // "../evil") simply fails this string comparison rather than ever being dereferenced as a
  // filename.
  if (!m->ini_id_present) {
    fprintf(stderr, "mods: '%s/mod.ini' has no id=; it must declare id=%s to agree with its folder name\n",
            name, name);
    return false;
  }
  if (strcmp(m->ini_id, name) != 0) {
    fprintf(stderr, "mods: '%s/mod.ini' declares id '%s', which does not match its folder name '%s'; refusing\n",
            name, m->ini_id, name);
    return false;
  }

  if (!m->api_present) {
    fprintf(stderr, "mods: '%s' has no api=, refusing\n", name);
    return false;
  }
  if (!m->api_parsed) {
    fprintf(stderr, "mods: '%s' has an unparseable or out-of-range api=, refusing\n", name);
    return false;
  }
  if (m->api < 1) {
    fprintf(stderr, "mods: '%s' has api=%d, which is below the minimum of 1, refusing\n", name, m->api);
    return false;
  }
  if (m->api > MODS_API_VERSION) {
    fprintf(stderr, "mods: '%s' wants api %d, this build only supports %d, refusing\n",
            name, m->api, MODS_API_VERSION);
    return false;
  }
  if (!m->ages && !m->seasons) {
    fprintf(stderr, "mods: '%s' declares no supported game, refusing\n", name);
    return false;
  }

  // Checked last, only once mod.ini itself is fully valid: a mod with a broken mod.ini should be
  // told about *that* first (e.g. the embedded-NUL or missing-id= checks above), rather than
  // "has no main.lua" masking the real problem for a mod that, as it happens, also forgot
  // main.lua. Task 4 deliberately left main.lua unchecked (running it wasn't this module's job
  // yet); now that it is, a mod that forgot to ship one deserves this specific message, not
  // whatever generic complaint lauxlib's own file loader happens to produce about a missing file
  // -- and, just as importantly, the same FIFO-blocks-forever / symlink-to-device-streams-forever
  // hazard that mod.ini's S_ISREG check above guards against applies identically here, since
  // lauxlib's loader is just another fopen/fread loop with no defence of its own. stat() before
  // ever touching the file closes that off the same way.
  char lua_path[1056];
  int n3 = snprintf(lua_path, sizeof lua_path, "%s/main.lua", entry_path);
  if (n3 < 0 || (size_t)n3 >= sizeof lua_path) {
    fprintf(stderr, "mods: '%s' path too long, skipping\n", name);
    return false;
  }
  struct stat lua_st;
  if (stat(lua_path, &lua_st) != 0) {
    fprintf(stderr, "mods: '%s' has no main.lua, skipping\n", name);
    return false;
  }
  if (!S_ISREG(lua_st.st_mode)) {
    fprintf(stderr, "mods: '%s' has a main.lua that is not a regular file, skipping\n", name);
    return false;
  }

  snprintf(m->id, sizeof m->id, "%s", name);
  if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", name);
  return true;
}

// Per-mod Lua heap accounting, passed as the `ud` of lua_newstate's custom allocator. One of
// these is malloc'd for the lifetime of a mod's Lua state and freed right alongside it.
typedef struct {
  size_t used;
  size_t cap;
} ModAlloc;

// How much Lua heap one mod's main.lua may use at once (and, from Task 8 on, its per-frame
// callbacks too, since they share the same state and so the same cap). Measured empirically on
// this dev machine with this exact allocator wired up: a fresh lua_newstate()+luaL_openlibs()
// costs about 19 KB, and a main.lua that builds a 1000-entry table of {x=,y=,name=} sub-tables
// costs about 250 KB total. 8 MiB is >30x that already-elaborate example -- generous for anything
// a reasonable main.lua would do -- while still being far too small for an unbounded-allocation
// loop to get anywhere: `local t={} while true do t[#t+1]=string.rep('x',1e6) end` allocates a
// megabyte-sized string per iteration, so it trips after roughly 8 of them.
//
// MODS_MAX (32) mods can all be loaded at once, each with its own state capped at this: 32 * 8
// MiB = 256 MiB of host memory a full mods folder can hold resident simultaneously. Nothing
// enforces a total across mods today -- this is the number that total could reach, recorded here
// because nothing else currently records it.
#define MOD_MEM_CAP ((size_t)8 * 1024 * 1024)

// Lua's custom-allocator contract (see third_party/lua/lmem.c's comment on frealloc): ptr == NULL
// means "create a new block of size nsize, no matter what osize says" -- osize is not reliably a
// byte count in that case, it can be a type tag Lua uses only for its own GC bookkeeping -- and
// nsize == 0 means "free ptr (which osize does correctly describe here), return NULL". So `used`
// is adjusted by nsize alone on creation, and by the difference on resize/free, and osize is never
// consulted when ptr == NULL. Returning NULL for an over-cap allocation is exactly how an ordinary
// out-of-memory realloc looks to Lua, which already knows how to turn that into a clean LUA_ERRMEM
// error -- so the memory cap needs no Lua-side cooperation at all, unlike the instruction budget
// below, which has to raise that error itself.
static void *mod_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  ModAlloc *a = (ModAlloc *)ud;
  if (ptr == NULL) {
    if (nsize == 0) return NULL;                    // frealloc(ud, NULL, x, 0) is defined as a no-op
    if (a->used + nsize > a->cap) return NULL;       // over budget before even trying to allocate
    void *p = malloc(nsize);
    if (!p) return NULL;
    a->used += nsize;
    return p;
  }
  if (nsize == 0) {
    free(ptr);
    a->used -= osize;
    return NULL;
  }
  // Guard the comparison with nsize > osize so it's only ever evaluated as a widening resize
  // (nsize - osize would underflow as size_t arithmetic otherwise); a shrinking resize can never
  // push a mod over its own cap and so is never blocked by it.
  if (nsize > osize && a->used + (nsize - osize) > a->cap) return NULL;  // old block, still valid,
                                                                           // untouched -- matches
                                                                           // what a real failing
                                                                           // realloc() would leave
  void *p = realloc(ptr, nsize);
  if (!p) return NULL;
  // nsize - osize computed as size_t can itself "underflow" when nsize < osize (a shrink), but
  // adding that wrapped value back to `used` is exactly correct modulo 2^64 -- unsigned
  // subtraction-then-addition cancels out to the true signed difference as long as neither byte
  // count is anywhere near the 64-bit wraparound, which no real allocation ever is.
  a->used += nsize - osize;
  return p;
}

// A first implementation of this budget raised a Lua error from the count hook, on the
// reasoning that lua_error's longjmp would unwind through whatever VM code was running straight
// into the lua_pcall luaL_dofile already wraps the chunk in. That is bypassable in one line,
// because a Lua *error* is exactly the kind of thing Lua code can catch and swallow:
//
//   while true do pcall(function() while true do end end) end
//
// pcall catches the hook's error, the mod carries on, and lua_sethook's countdown simply starts
// over -- unlimited CPU, one budget at a time. The fix is to make the hook unwind in a way no
// Lua-level pcall/xpcall/coroutine.resume can intercept: lua_yield. A yield yields the *thread*,
// not an error value; it unwinds all the way out to the lua_resume call that is driving that
// thread, which is this module, in C, not any Lua code a mod could wrap around itself. See
// mod_resume_budgeted below for how that lua_resume call is actually made.
//
// A second, subtler bypass survives even the yield fix on its own: a mod can run
// coroutine.create(spinner) and coroutine.resume it in a loop. coroutine.resume is itself
// implemented by calling lua_resume on the *new* coroutine's thread; when that thread's hook
// fires and yields, coroutine.resume treats it exactly like an ordinary, intentional
// coroutine.yield() -- it returns `true` to the *caller*, on the outer thread, and the outer
// thread's own lua_resume (the one this module is driving) never sees anything happen. Worse,
// lua_newthread's lstate.c implementation copies the creating thread's hookmask/basehookcount to
// the new thread *and resets its countdown fresh* -- so every single coroutine.resume() call on
// that same coroutine buys it another full, fresh MOD_LOAD_INSTRUCTION_BUDGET-sized slice before
// its hook fires again. Measured against the naive per-thread-only version of this fix: a bare
// `while true do coroutine.resume(co) end` around a spinner coroutine took over three minutes to
// return control to the host on this dev machine, worse as the budget grows, and unbounded in
// principle since the outer loop's own countdown only measures *its own* instructions, a handful
// per iteration, never the (arbitrarily slow) work hidden inside each resume.
//
// The fix: a budget shared across every thread belonging to one mod run -- the top-level one
// mod_resume_budgeted drives directly, and any coroutine.create()'d thread spun up underneath it
// -- rather than relying on Lua's own independent per-thread accounting. mod_instruction_limit_hook
// decrements the *same* ModBudget on every firing, from whichever thread happens to be running
// when LUA_MASKCOUNT elapses, regardless of which thread that is. The moment the shared total is
// used up, that thread is marked "exhausted" and immediately re-armed to fire on its very next
// instruction (count = 1) before yielding -- so it can never again buy a fresh slice merely by
// being resumed, from any caller. That makes every further coroutine.resume() on a poisoned
// coroutine cost close to nothing, so the *outer* loop's own (independent, Lua-native) countdown
// -- which was ticking down on its own schedule the whole time, just slowly, since each iteration
// looked cheap to it -- catches up and fires for real within a fraction of a second once
// exhaustion starts, instead of needing one full fresh slice per iteration. Verified empirically
// with the exact scenario above: 0.21s to regain control, not three-plus minutes.
//
// One more wrinkle this leaves: if the *coroutine* is the thing that actually exhausts the shared
// budget, and the caller never calls coroutine.resume on it again, the outer chunk can simply run
// to completion normally (LUA_OK) with the exhaustion fully absorbed and invisible to the status
// code alone -- mod_resume_budgeted's `exhausted` out-parameter is what callers must check
// instead of trusting a clean LUA_OK to mean "nothing went wrong".
typedef struct {
  long long remaining;        // instruction-count budget left; see MOD_LOAD_INSTRUCTION_BUDGET
  struct timespec deadline;   // CLOCK_MONOTONIC point past which this run is cut off regardless
                               // of remaining -- see MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS
  bool exhausted;             // true once EITHER bound above has been hit
  bool deadline_exceeded;     // true iff it was specifically the deadline, not the instruction
                               // count, that set `exhausted` -- decides which message
                               // run_main_lua reports, so a mod author whose script was just
                               // legitimately slow is told that, not that it looped forever
} ModBudget;

// A constraint that outlives this task, found by a later spec review and worth recording here
// so the next person touching this doesn't rediscover it with a hang: everything above closes
// every bypass that *resumes an existing thread*, repeatedly or otherwise, but it cannot close
// one that mints a *fresh* thread, because lua_newthread resets a brand-new thread's own
// instruction countdown to a full basehookcount regardless of how exhausted the shared budget
// already is -- confirmed by probing the vendored interpreter directly (hook function and mask
// are inherited from the creating thread; the count is not, it comes back full). Our hook only
// gets a chance to check the shared ModBudget once Lua's own per-thread countdown decides to
// invoke it, and a fresh thread's countdown always starts from the full MOD_INSTRUCTION_CHECKIN,
// no matter what. mods.c handles this today by withholding the "coroutine" library entirely (see
// mod_open_libs_sandboxed_partially) -- a mod with no way to call lua_newthread at all has no way
// to exploit this, so per-thread accounting never needs to be bulletproof against it. That is a
// restriction on the *library*, not a fix to *this* mechanism: if coroutines are ever granted
// back to mods (the rest of Task 7's sandbox deciding they belong, a later per-frame API,
// anything else in this host that spins up a new thread to run mod code), the exact same hang
// reopens unless something *also* re-arms new threads against the shared budget at creation time
// -- which Lua's public API has no hook for; the vendored lua_newthread (third_party/lua/lstate.c)
// would need to carry the parent's *remaining* count into the child instead of a fresh
// basehookcount, a deliberate fork of Lua's own semantics that was discussed and declined for now
// rather than done as a side effect of closing this specific hole.

// How often (in Lua VM instructions) the hook checks in against the shared budget, on whichever
// thread happens to be running when the count elapses. Deliberately much smaller than the total
// budget itself: since lua_newthread resets a new coroutine's own countdown fresh (see the long
// comment above), a mod spinning up many short-lived coroutines could otherwise run each one for
// up to a full `count` instructions before its very first check-in, each slice invisible to every
// other thread's own accounting until it checks in. 1,000,000 bounds that slack to roughly a
// couple of milliseconds per coroutine, however many a mod creates, while keeping the hook's own
// call overhead negligible against legitimate work (confirmed: the "healthy" case below still
// completes instantly).
#define MOD_INSTRUCTION_CHECKIN 1000000

// The shared instruction total. Measured empirically on this dev machine: a tight
// `while true do i=i+1 end` loop runs at roughly 450 million VM instructions per second, so 50
// million is about a tenth of a second of real wall-clock time for a *cheap* runaway script --
// generous margin under "a couple of seconds" even on CI hardware several times slower than this
// machine, while still being orders of magnitude more than any realistic main.lua setup script
// (defining a handful of functions and a modest table) will ever use.
//
// That "tenth of a second" assumption is exactly what a later spec review found does not hold
// for every script: instruction cost varies by orders of magnitude depending on what the
// instructions actually do. A script that repeatedly raises and catches a real Lua error (string
// formatting for the message, a longjmp, stack unwinding, a fresh closure per iteration) can take
// 50-150x longer to burn the same 50 million instructions than a bare arithmetic loop does --
// measured at 5-16 seconds for exactly that shape, worse under host CPU contention, since a
// starved process still has to actually execute those expensive instructions once it gets
// scheduled, however long that takes to happen. An instruction count alone cannot bound
// wall-clock time against that; see MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS below for the fix.
#define MOD_LOAD_INSTRUCTION_BUDGET 50000000

// The wall-clock backstop, in seconds, checked alongside the instruction count on every hook
// firing (so it adds no new overhead of its own: MOD_INSTRUCTION_CHECKIN already decides how
// often that happens). Picked, not measured, the way the instruction budget was: any plausible
// real main.lua finishes in milliseconds, so 2 seconds is a thousand-fold margin over that --
// while being short enough that a hostile mod is cut off promptly regardless of how expensive its
// particular instructions are, which the instruction count alone cannot promise. MODS_MAX (32)
// mods, each of this worst-case shape, now stalls startup by at most roughly 32 * 2 = 64 seconds
// instead of the multiple *minutes* the same scan could previously take under load -- not fast,
// but bounded, which an instruction count alone was not. This is the primary reason a mod gets
// cut off in practice today; MOD_LOAD_INSTRUCTION_BUDGET above still matters as the *first* line
// of defence for the common case (a cheap infinite loop trips it in ~0.1s, well before 2 seconds
// of wall clock ever passes), and remains the only bound at all for a host so idle that cheap
// instructions never come anywhere near this deadline.
#define MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS 2.0

// Valid only while mod_resume_budgeted (immediately below) is driving some thread's lua_resume --
// mods run strictly one at a time, never concurrently, so a single pointer rather than a
// per-state registry entry is correct here and avoids registry bookkeeping inside the hook's hot
// path. Set and cleared by mod_resume_budgeted around each call; NULL the rest of the time.
static ModBudget *g_mod_budget;

static void mod_instruction_limit_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  ModBudget *b = g_mod_budget;
  if (!b) return;                            // defensive: should never fire with no run in flight
  if (!b->exhausted) {
    b->remaining -= MOD_INSTRUCTION_CHECKIN;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    bool deadline_passed = now.tv_sec > b->deadline.tv_sec ||
        (now.tv_sec == b->deadline.tv_sec && now.tv_nsec >= b->deadline.tv_nsec);
    if (b->remaining <= 0 || deadline_passed) {
      b->exhausted = true;
      b->deadline_exceeded = deadline_passed;    // wins the tie if both happened at once: the
                                                  // wall-clock framing is the more useful one
                                                  // for a mod author to be told
    }
  }
  if (b->exhausted) {
    // Re-armed at count = 1: once poisoned, this thread must never again be able to buy a full
    // fresh checkin's worth of CPU just by being resumed again -- directly, or via
    // coroutine.resume from another thread of the same mod -- so from here on it has to check in
    // on literally its very next instruction, every single time, for the rest of its life.
    lua_sethook(L, mod_instruction_limit_hook, LUA_MASKCOUNT, 1);
    lua_yield(L, 0);
  }
}

// Drives `thread` (freshly loaded with a function to call via lua_resume, nargs of them already
// pushed) to completion or exhaustion, under a budget of `total` VM instructions *and* a
// `deadline_seconds` wall-clock cutoff, both shared across `thread` and any coroutine it creates
// while running -- see the long comments above for why that sharing, not just a plain per-thread
// lua_sethook, is what actually makes either bound unbypassable. Returns the lua_resume status
// (LUA_OK / LUA_YIELD / an error code); on return, `thread`'s stack holds its results (LUA_OK) or
// error object (an error status) exactly as lua_resume leaves them. *exhausted is set whenever
// either bound was used up at any point during this call, even if `thread` itself happened to
// return LUA_OK -- because the exhaustion can be fully absorbed by a nested coroutine.resume
// rather than ever surfacing as this thread's own yield (see the comment on ModBudget above).
// *deadline_hit distinguishes which bound did it, for the caller's error message. Callers must
// treat *exhausted as a failure in its own right, never assume a clean LUA_OK on its own means
// nothing went wrong.
//
// Built as a function taking `thread`/`nargs`/`total`/`deadline_seconds` as parameters, not a
// single hardcoded lua_resume call inside run_main_lua, specifically so a later per-frame
// callback runner can call this again -- on a different thread, with a much smaller `total` and
// `deadline_seconds` of its own -- without this function changing at all: the only state it owns
// (g_mod_budget) is scoped to one call and always cleared before returning, success or failure
// alike.
static int mod_resume_budgeted(lua_State *thread, int nargs, long long total,
                                double deadline_seconds, bool *exhausted, bool *deadline_hit) {
  ModBudget budget = { .remaining = total, .exhausted = false, .deadline_exceeded = false };
  clock_gettime(CLOCK_MONOTONIC, &budget.deadline);
  long whole_secs = (long)deadline_seconds;
  long extra_nsecs = (long)((deadline_seconds - (double)whole_secs) * 1e9);
  budget.deadline.tv_sec += whole_secs;
  budget.deadline.tv_nsec += extra_nsecs;
  if (budget.deadline.tv_nsec >= 1000000000L) {
    budget.deadline.tv_sec += 1;
    budget.deadline.tv_nsec -= 1000000000L;
  }
  g_mod_budget = &budget;
  lua_sethook(thread, mod_instruction_limit_hook, LUA_MASKCOUNT, MOD_INSTRUCTION_CHECKIN);
  int nres;
  int status = lua_resume(thread, NULL, nargs, &nres);
  g_mod_budget = NULL;
  *exhausted = budget.exhausted;
  *deadline_hit = budget.deadline_exceeded;
  return status;
}

// Withholds two standard libraries that plain luaL_openlibs would otherwise grant, each for its
// own reason, neither of them a start on Task 7's full sandbox (string/table/math only) -- both
// are specific, load-bearing holes in *this* task's own deliverable (a budget that actually
// bounds a mod's CPU use), discovered while building mod_resume_budgeted/
// mod_instruction_limit_hook above, and closed here because leaving them open would make that
// mechanism not do its job today, not because they happen to overlap with where the eventual
// sandbox line falls:
//
// - "debug": debug.sethook() lets ordinary Lua code replace or remove *any* hook set from C,
//   including mod_instruction_limit_hook. A single `debug.sethook()` call at the top of main.lua
//   would silently disarm the anti-hang protection before the mod's own infinite loop ever ran --
//   confirmed empirically: unpatched, `debug.sethook() while true do end` inside a
//   1,000,000-instruction checkin ran a 2,000,000-iteration loop to completion instead of
//   aborting.
//
// - "coroutine": a later spec review found that the shared-budget design above closes every
//   bypass that *resumes the same thread repeatedly*, but not one that mints a *fresh* thread
//   every time. lua_newthread inherits the creating thread's hook function and mask, but resets
//   the new thread's own countdown to a full basehookcount (confirmed by probing the vendored
//   interpreter directly: hook and mask matched between parent and child, but count was back at
//   the full value) -- regardless of how exhausted the shared ModBudget already is, since that
//   exhaustion is only ever checked *inside* the hook, and Lua's own per-thread countdown is what
//   decides when the hook gets invoked at all. A mod minting a fresh coroutine every loop
//   iteration (`while true do coroutine.resume(coroutine.create(spinner)) end`, or the same
//   shape through coroutine.wrap, or nested inside pcall) buys each one a full, free
//   MOD_INSTRUCTION_CHECKIN-sized ride before its hook is ever invoked even once, independent of
//   the shared total -- confirmed to hang past an 8-second hard alarm. No amount of per-thread
//   accounting closes this; only not letting a mod create new threads at all does, for now. See
//   the long comment above ModBudget for what to do if a later task needs this library back.
//
// Everything else luaL_openlibs would open is opened exactly as before -- io, os and
// package/require stay fully available, unlike the eventual sandbox (see this function's call
// site) -- only "debug" and "coroutine" are withheld. Each is removed from both the globals
// table and package.loaded (require() checks package.loaded first, so nil-ing only the global
// would leave e.g. `require('coroutine')` able to resurrect it) -- nothing else the standard
// library opens re-registers either name under either key, so this makes both unreachable by any
// ordinary means.
static void mod_open_libs_sandboxed_partially(lua_State *L) {
  luaL_openlibs(L);
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  static const char *const withheld[] = {"debug", "coroutine"};
  for (size_t i = 0; i < sizeof withheld / sizeof withheld[0]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, withheld[i]);
    lua_pushnil(L);
    lua_setfield(L, -2, withheld[i]);      // package.loaded[withheld[i]] = nil
  }
  lua_pop(L, 2);           // package.loaded, package
}

// Actually runs a mod's main.lua -- the one part of loading a mod that executes untrusted code,
// which is why it happens here, in mods_init's loop, rather than inside load_one: by the time this
// is called, the mod has already cleared every load_one check *and* been confirmed to apply to the
// game being played *and* still fit under MODS_MAX, so Lua only ever runs for a mod that is
// actually going to be kept. load_one has already proven main.lua exists and is a regular file, so
// the only new failure modes here are the mod's own code, an over-budget allocation, and an
// over-budget instruction count -- all three land in the same "mod failed to load" refusal, and
// all three leave the rest of the scan completely unaffected: a broken mod is skipped, never
// fatal, exactly per the "a broken mod must never take the app down" design.
static bool run_main_lua(const char *dir, const char *name, Mod *m) {
  char lua_path[1056];
  int n = snprintf(lua_path, sizeof lua_path, "%s/%s/main.lua", dir, name);
  if (n < 0 || (size_t)n >= sizeof lua_path) {
    fprintf(stderr, "mods: '%s' path too long, skipping\n", name);
    return false;
  }

  ModAlloc *a = malloc(sizeof *a);
  if (!a) {
    fprintf(stderr, "mods: '%s' out of memory allocating its Lua accounting, skipping\n", name);
    return false;
  }
  a->used = 0;
  a->cap = MOD_MEM_CAP;

  // lua_newstate, not luaL_newstate: every byte this mod's Lua allocates -- its own strings and
  // tables as well as the interpreter's internal objects -- is counted against that mod's own
  // cap, not the host process's whole heap. Without this, an unbounded allocation loop would grow
  // until the OS killed the process rather than failing cleanly inside Lua.
  lua_State *L = lua_newstate(mod_lua_alloc, a);
  if (!L) {
    // Not silent: a NULL here means even the initial handful of KB the interpreter needs to boot
    // itself failed to allocate, which the caller deserves to know about rather than just losing
    // the mod with no explanation.
    fprintf(stderr, "mods: '%s' could not create a Lua state, skipping\n", name);
    free(a);
    return false;
  }

  // Deliberately unrestricted for now, except for the two specific holes
  // mod_open_libs_sandboxed_partially closes below: io, os and package/require stay wide open,
  // none of which the approved sandbox design grants a mod (string, table and math only).
  // Closing that gap generally is a later task's job specifically, not an oversight here -- and
  // nothing in the shipped app links this module yet either way, so no build anyone runs today
  // is affected by a mod being able to read/write files or spawn processes through main.lua.
  mod_open_libs_sandboxed_partially(L);
  mods_api_open(L);

  // Run on its own coroutine thread, driven by mod_resume_budgeted rather than luaL_dofile's
  // plain lua_pcall -- see the long comment above mod_instruction_limit_hook for why that is what
  // actually makes the instruction budget unbypassable by a mod's own pcall or coroutine.resume.
  lua_State *runner = lua_newthread(L);
  char reason[512] = "";
  bool failed = false;

  if (luaL_loadfile(runner, lua_path) != LUA_OK) {
    failed = true;
    size_t elen = 0;
    const char *emsg = lua_tolstring(runner, -1, &elen);
    // A non-string error object (a mod calling error({}), or bare error() raising nil) makes
    // plain lua_tolstring return NULL -- the spec requires detail on stderr either way, so fall
    // back to a literal rather than printing "...: " with nothing after the colon. Deliberately
    // not luaL_tolstring here: it would invoke a __tostring metamethod the error object's
    // metatable might define, which is itself a Lua call -- and this is not a context (just
    // after a load/resume failure, outside any protection of our own) where it's safe to reason
    // about a second failure landing cleanly. A hostile mod could define __tostring to loop
    // forever or to error again itself; handing it that extra entry point is not worth the
    // richer message it would sometimes produce, so this always takes the plain, inert literal
    // instead.
    if (!emsg) { emsg = "(non-string error object)"; elen = strlen(emsg); }
    mods_escape_into(reason, sizeof reason, emsg, elen);
  } else {
    bool exhausted = false, deadline_hit = false;
    int status = mod_resume_budgeted(runner, 0, MOD_LOAD_INSTRUCTION_BUDGET,
                                      MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS, &exhausted, &deadline_hit);
    if (status == LUA_YIELD) {
      failed = true;
      // Two different messages for two different mod authors: one whose script loops forever
      // (the instruction budget caught it quickly, deadline_hit false) and one whose script is
      // just legitimately slow -- expensive per instruction, or merely unlucky on a busy host --
      // and was cut off by the wall-clock deadline instead. Telling them apart means a mod
      // author debugging a timeout is pointed at the right half of the problem.
      if (deadline_hit) {
        snprintf(reason, sizeof reason,
            "ran for longer than %.0f seconds and was cut off", MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS);
      } else {
        snprintf(reason, sizeof reason, "exceeded its instruction budget (possible infinite loop)");
      }
    } else if (status != LUA_OK) {
      failed = true;
      size_t elen = 0;
      const char *emsg = lua_tolstring(runner, -1, &elen);
      if (!emsg) { emsg = "(non-string error object)"; elen = strlen(emsg); }
      mods_escape_into(reason, sizeof reason, emsg, elen);
    } else if (exhausted) {
      // The chunk itself returned cleanly (LUA_OK), but a coroutine it created along the way
      // burned through the whole shared budget and had that absorbed by its own
      // coroutine.resume call rather than ever yielding *this* thread -- see
      // mod_resume_budgeted's doc comment. A clean-looking LUA_OK is not the end of the story.
      failed = true;
      if (deadline_hit) {
        snprintf(reason, sizeof reason,
            "ran for longer than %.0f seconds via a nested coroutine and was cut off",
            MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS);
      } else {
        snprintf(reason, sizeof reason,
            "exceeded its instruction budget via a nested coroutine (possible infinite loop)");
      }
    }
  }

  if (failed) {
    // reason is attacker-controlled either way: main.lua's own error() argument, the literal
    // budget-exceeded text above, or Lua's fixed "not enough memory" string when the allocator
    // cap was hit. mods_escape_into is the same escaper mod.ini values go through, reused here
    // rather than duplicated, for the identical reason: printed raw, a crafted error string
    // could forge a fake success line on a terminal via an embedded CR and ANSI escape.
    fprintf(stderr, "mods: '%s' failed to run main.lua: %s\n", name, reason);
    lua_close(L);
    free(a);
    return false;
  }

  m->L = L;
  return true;
}

static int cmp_name(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool mods_init(const char *dir, bool seasons) {
  mods_shutdown();
  host_seasons = seasons;

  if (!dir || !*dir) {
    fprintf(stderr, "mods: no mods directory given\n");
    return false;
  }

  errno = 0;
  DIR *d = opendir(dir);
  if (!d) {
    if (errno == ENOENT) return true;                    // no mods installed yet is not a failure
    fprintf(stderr, "mods: could not open mods directory '%s': %s\n", dir, strerror(errno));
    return false;
  }

  // Collect folder names first and sort them alphabetically before parsing anything, so load
  // order (and, from Task 5 on, main.lua run order) is deterministic rather than following
  // readdir's platform- and history-dependent order. This is alphabetical for determinism only;
  // it is not yet the install-order the spec eventually wants.
  char **names = NULL;
  size_t n = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    // Skips ".", "..", every hidden entry, and (on macOS) ".DS_Store" -- none of these can ever
    // pass mods_id_valid anyway, so this buys silence rather than correctness: without it, a
    // freshly-unzipped mods folder would print a "not a valid mod id" line for Finder's own
    // litter on every single launch.
    if (e->d_name[0] == '.') continue;
    if (n == cap) {
      size_t new_cap = cap ? cap * 2 : 16;
      char **grown = realloc(names, new_cap * sizeof *names);
      if (!grown) { fprintf(stderr, "mods: out of memory scanning %s\n", dir); break; }
      names = grown;
      cap = new_cap;
    }
    names[n] = strdup(e->d_name);
    if (!names[n]) { fprintf(stderr, "mods: out of memory scanning %s\n", dir); break; }
    n++;
  }
  closedir(d);
  if (n > 0) qsort(names, n, sizeof *names, cmp_name);   // n == 0 leaves names NULL; don't touch it

  int loaded = 0, dropped_for_cap = 0;
  for (size_t i = 0; i < n; i++) {
    const char *name = names[i];
    Mod m = {.enabled = true};
    if (!load_one(dir, name, &m)) continue;
    if (!(seasons ? m.seasons : m.ages)) continue;        // valid mod, just not for this game

    if (loaded >= MODS_MAX) {
      dropped_for_cap++;
      continue;
    }
    // Only now -- known to apply to this game, and known to still have a slot -- is it worth the
    // cost and risk of actually running this mod's Lua. run_main_lua already printed why on
    // failure.
    if (!run_main_lua(dir, name, &m)) continue;
    mods[loaded++] = m;
  }
  count = loaded;
  if (dropped_for_cap > 0)
    fprintf(stderr, "mods: %d mod(s) dropped, only the first %d are loaded\n", dropped_for_cap, MODS_MAX);

  for (size_t i = 0; i < n; i++) free(names[i]);
  free(names);
  return true;
}

void mods_shutdown(void) {
  // Close every mod's Lua state (and free the per-mod allocator accounting that went with it)
  // before resetting count -- not after, since mods[] is about to become meaningless, and not
  // skipped for mods whose L is NULL (none in mods[] today ever are, since only a mod that
  // run_main_lua fully succeeded on is ever stored there, but the check costs nothing and keeps
  // this safe against that invariant ever loosening).
  for (int i = 0; i < count; i++) {
    if (!mods[i].L) continue;
    void *ud = NULL;
    lua_getallocf(mods[i].L, &ud);
    lua_close(mods[i].L);
    free(ud);
  }
  count = 0;
  // Correction: a stale oracles.log() message must not outlive the mod that logged it, or a
  // silent mod loaded afterwards would be misreported as having said something. mods_init always
  // calls mods_shutdown first, so this also covers "the top of mods_init", not just an explicit
  // shutdown call.
  mods_api_clear_log();
}

int mods_count(void) { return count; }
const char *mods_id(int i) { return i >= 0 && i < count ? mods[i].id : ""; }
const char *mods_name(int i) { return i >= 0 && i < count ? mods[i].name : ""; }
bool mods_enabled(int i) { return i >= 0 && i < count && mods[i].enabled; }
