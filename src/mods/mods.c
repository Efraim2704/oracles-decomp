#include "mods/mods.h"
#include "mods/api.h"
#include "mods/sandbox.h"
#include "lua.h"
#include "lauxlib.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

// How much of a failure reason mods_error() keeps. Deliberately short: this is for a one-line
// status display, not a full diagnostic -- the unabridged text (up to 512 bytes, see `reason` in
// run_main_lua) always also reaches stderr at the moment a mod is disabled, which is where a
// developer debugging a specific mod should look. Sized with headroom above the longest fixed
// message this file produces today (the "...via a nested coroutine..." budget/deadline wording,
// ~80 bytes) so none of those truncate, while still being far short of the 512-byte buffer a
// hostile main.lua's own error() text is escaped into before being cut down to this length --
// mods_escape_into's \xHH escaping already makes that truncation byte-safe (every escape sequence
// is plain ASCII, so cutting it short mid-sequence yields a shorter but still printable string,
// never a broken one).
#define MOD_ERROR_LEN 128

typedef struct {
  char id[MOD_FIELD_LEN];     // canonical: the validated folder name, never the mod.ini field directly
  char name[MOD_FIELD_LEN];
  char ini_id[MOD_FIELD_LEN]; // raw "id=" value from mod.ini (escaped; see mods_escape_into), kept
                              // only to cross-check against id
  bool ini_id_present;
  bool enabled;         // Set true only once run_main_lua (or the api-too-new check in mods_init)
                        // actually succeeds for this mod. A mod that is accepted as applying to
                        // this game but whose own payload fails is still stored in mods[] with
                        // this false -- see mods_init's own comment for exactly where that line
                        // sits -- never silently dropped the way a mod that isn't ours at all is.
  char error[MOD_ERROR_LEN]; // "" while enabled is true; the escaped, truncated reason otherwise.
                             // Populated at the same place enabled is set to false, never earlier.
  bool ages, seasons;
  bool api_present;     // true iff an "api=" line appeared at all
  bool api_parsed;      // true iff that value parsed as a well-defined int (meaningless unless
                        // api_present)
  int api;              // the parsed value iff api_parsed; otherwise unspecified. Whether it's an
                        // *acceptable* value is split across two different callers, not read_ini's
                        // call: load_one checks >= 1 (a malformed value, refused outright); the
                        // mods_init loop checks <= MODS_API_VERSION (a well-formed value this
                        // build is simply too old for, listed disabled rather than refused) --
                        // see the api= branch below for why that split exists.
  lua_State *L;         // Non-NULL exactly when `enabled` is true. A mod stored in mods[] with
                        // enabled false (api too new, or run_main_lua failed) always has L ==
                        // NULL: run_main_lua either never created one, or already closed and
                        // freed it itself before returning false -- see mods_shutdown's own
                        // comment for why that invariant is what keeps it from double-closing.
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
      // load_one owns the >= 1 policy check instead (api < 1 is a malformed declaration, refused
      // outright); mods_init owns the separate > MODS_API_VERSION case (a well-formed declaration
      // this build is simply too old for, which is listed disabled rather than refused -- see
      // load_one's own comment on its api= branch for why those two get different treatment).
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
// fopen/fgets), free of embedded NUL bytes, parses, declares a matching id, has a usable api=
// (present, parseable, and >= 1 -- but NOT necessarily <= MODS_API_VERSION: see the api= branch
// below for why that upper bound is deliberately not checked here), and a usable games=. Prints
// its own stderr diagnostic on every refusal path, where "refusal" means the whole mod is dropped
// outright -- it is not a mod for this game at all, so it never occupies a slot in mods[] and
// mods_count() never counts it. Does NOT check that main.lua exists, is runnable, or applies to
// the game being played, and does not apply the MODS_MAX cap: those either depend on state that
// spans the whole scan (which game, how many mods are already loaded), or -- for main.lua --
// belong to a different category of failure entirely. A mod this function accepts (returns true
// for) is a real, identified mod for some game; what happens to it next (listed enabled, or
// listed disabled because its own payload failed) is mods_init's call, together with
// run_main_lua below.
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
  // Deliberately NOT refused here if m->api > MODS_API_VERSION: unlike every check above, this
  // mod has a well-formed, validly-declared identity -- a real package the player installed, not
  // a malformed or absent one. mods_init treats "needs a newer build than this one" as a mod that
  // is accepted but disabled (mods_enabled() false, mods_error() names the required version),
  // the same way a failing main.lua is, rather than making it vanish the way the checks above do.
  if (!m->ages && !m->seasons) {
    fprintf(stderr, "mods: '%s' declares no supported game, refusing\n", name);
    return false;
  }

  // Whether main.lua exists, is a regular file, or runs successfully is no longer this function's
  // job: by this point mod.ini alone has fully qualified `name` as a real mod for some game, and
  // every failure past here (missing/non-regular main.lua, a Lua error, an exhausted budget, ...)
  // is "a real mod whose own payload didn't work", which mods_init lists as disabled rather than
  // dropping -- see run_main_lua, which now makes that same stat() check before ever touching the
  // file, for exactly the same FIFO/symlink-to-device reasons this used to guard against here.
  snprintf(m->id, sizeof m->id, "%s", name);
  if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", name);
  return true;
}

// The containment a mod's Lua state runs under -- the custom allocator, the instruction/
// wall-clock budget, and the hardened library set (mod_open_libs_hardened, mod_resume_budgeted,
// ModAlloc/mod_lua_alloc) -- lives in sandbox.{c,h}, not here: it is one cohesive
// security-critical unit with a clean seam, and mod_resume_budgeted is also what a later
// per-frame callback runner (Task 8) needs to call from outside this file.

// Actually runs a mod's main.lua -- the one part of loading a mod that executes untrusted code,
// which is why it happens here, in mods_init's loop, rather than inside load_one: by the time this
// is called, the mod has already cleared every load_one check *and* been confirmed to apply to the
// game being played *and* still fit under MODS_MAX, so Lua only ever runs for a mod that is
// actually going to be kept (at least one slot in mods[], enabled or disabled). Also checks that
// main.lua exists and is a regular file -- load_one used to do this, but a mod whose zip forgot
// main.lua is exactly the same category of failure as one whose main.lua errors out, so this
// function owns both now: whatever it refuses, the mod stays listed, disabled, with the reason in
// *m's `error` field (see MOD_ERROR_LEN) -- it is mods_init's caller, not this function, that
// decides a mod is dropped outright, and only for reasons load_one already covers.
//
// What "a broken mod must never take the app down" actually covers today, stated plainly rather
// than claimed in the abstract -- this comment has twice before asserted a guarantee that didn't
// hold, so each line here is something a test actually pins, not something assumed true because
// the design says it should be:
//
// Caught cleanly, mod disabled, host unaffected: a Lua-level infinite loop, however it's shaped
// (bare, pcall-wrapped, or -- before "coroutine" was withheld -- via a freshly-minted thread); a
// looping __gc finalizer (never allowed to arm at all, see l_setmetatable_no_gc); an unbounded
// allocation, in either of Lua's own allocator call shapes; a corrupted or malicious binary
// chunk, from load() or from main.lua itself; and a hostile __tostring metamethod reached while
// reporting an unrelated error. Each of those has a dedicated test under alarm().
//
// Not guaranteed, and now closed by Task 7's sandbox rather than merely scoped out of this one:
// before Task 7, io and os were still fully open, so a mod could call os.exit() to take the
// whole process down outright, or os.execute("sleep 30") (or any other blocking os/io call) to
// stall past the wall-clock deadline with no VM instructions ever executing for the hook to
// catch. mod_open_libs_hardened (sandbox.c) now withholds both libraries entirely -- along with
// require/package, load/loadfile/dofile, string.dump, print/warn and utf8 -- so neither route is
// reachable any more; this comment is kept to record what the gap was and that it is the reason
// those specific names, not some other plausible-looking set, are the ones withheld.
//
// One known, accepted gap that is neither of the above: a mod that recursively calls pcall on
// itself (`local function guard() while true do pcall(guard) end end guard()`) livelocks inside
// lua_resume's own C-level error recovery, never returning to the VM to retire instructions, so
// neither the budget nor the deadline is ever reachable. This is upstream Lua behaviour, requires
// deliberate construction rather than being a plausible accident, and is left unfixed by
// deliberate decision -- not a TODO, a decision -- rather than taking on out-of-band machinery
// (a signal-based watchdog, abandoning Lua states on timeout) disproportionate to the risk at
// this stage. Task 11's user-facing docs should carry this forward once they exist.
static bool run_main_lua(const char *dir, const char *name, Mod *m) {
  char lua_path[1056];
  int n = snprintf(lua_path, sizeof lua_path, "%s/%s/main.lua", dir, name);
  if (n < 0 || (size_t)n >= sizeof lua_path) {
    fprintf(stderr, "mods: '%s' path too long; disabling it for this session\n", name);
    snprintf(m->error, sizeof m->error, "path too long");
    return false;
  }

  // Require a regular file before ever calling luaL_loadfilex on it, for exactly the same reason
  // load_one's own mod.ini check does: a FIFO blocks fopen/fread indefinitely waiting for a writer
  // that will never come, and a symlink to a character device (main.lua -> /dev/zero) streams
  // forever with no newline for lauxlib's loader to stop on. stat() follows symlinks, so this
  // catches that case too.
  struct stat lua_st;
  if (stat(lua_path, &lua_st) != 0) {
    fprintf(stderr, "mods: '%s' has no main.lua; disabling it for this session\n", name);
    snprintf(m->error, sizeof m->error, "has no main.lua");
    return false;
  }
  if (!S_ISREG(lua_st.st_mode)) {
    fprintf(stderr, "mods: '%s' has a main.lua that is not a regular file; disabling it for this session\n", name);
    snprintf(m->error, sizeof m->error, "main.lua is not a regular file");
    return false;
  }

  ModAlloc *a = malloc(sizeof *a);
  if (!a) {
    fprintf(stderr, "mods: '%s' out of memory allocating its Lua accounting; disabling it for this session\n", name);
    snprintf(m->error, sizeof m->error, "out of memory");
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
    fprintf(stderr, "mods: '%s' could not create a Lua state; disabling it for this session\n", name);
    snprintf(m->error, sizeof m->error, "could not create a Lua state");
    free(a);
    return false;
  }

  // mod_open_libs_hardened (sandbox.c) cuts the standard libraries down to the spec's sandbox --
  // string, table and math -- plus whatever CPU/memory containment holes the earlier tasks found
  // and closed regardless of where that line falls; io, os, require/package,
  // load/loadfile/dofile, string.dump, print/warn and utf8 are all withheld there.
  mod_open_libs_hardened(L);
  mods_api_open(L);

  // Run on its own coroutine thread, driven by mod_resume_budgeted (lua_resume) rather than a
  // plain lua_pcall -- see the long comment above mod_instruction_limit_hook for why that is what
  // actually makes the instruction budget unbypassable by a mod's own pcall or coroutine.resume.
  lua_State *runner = lua_newthread(L);
  char reason[512] = "";
  bool failed = false;

  // "t" forced, not luaL_loadfile's NULL-mode default of "bt": main.lua itself is read straight
  // off disk, and a file that happens to look like a Lua binary chunk (crafted, or merely
  // corrupted) can crash the host outright -- fuzzed to a SIGSEGV on the first trial. This is the
  // host-side half of that fix; `load` itself (the other half) is now withheld from mods
  // entirely rather than kept wrapped to force the same mode -- see mod_open_libs_hardened's own
  // comment in sandbox.c.
  if (luaL_loadfilex(runner, lua_path, "t") != LUA_OK) {
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
      //
      // Currently unreachable with "coroutine" withheld -- a mod cannot create the nested
      // thread this branch exists to catch -- kept deliberately as a tripwire against that
      // protection ever being loosened without this branch (and the two "via a nested
      // coroutine" messages below) being revisited at the same time.
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
    // could forge a fake success line on a terminal via an embedded CR and ANSI escape. Already
    // escaped by the time it's copied into m->error below, so simply truncating it to
    // MOD_ERROR_LEN there (via snprintf, not another escape pass) stays byte-safe -- it can only
    // ever shorten a run of plain, printable \xHH/ASCII text, never reintroduce a raw control
    // byte. stderr keeps the untruncated version (`reason`, up to 512 bytes); the stored,
    // short copy is for mods_error()'s status-line use, a different audience with a different
    // length budget.
    fprintf(stderr, "mods: '%s' failed to run main.lua: %s\n", name, reason);
    snprintf(m->error, sizeof m->error, "%s", reason);
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
    Mod m = {0};                                          // enabled starts false; set true below
                                                            // only once this mod actually succeeds
    if (!load_one(dir, name, &m)) continue;                // not a mod for us at all: no slot used
    if (!(seasons ? m.seasons : m.ages)) continue;         // valid mod, just not for this game

    if (loaded >= MODS_MAX) {
      dropped_for_cap++;
      continue;
    }

    // From here on this is a real, identified mod for this game, and it occupies slot `loaded`
    // regardless of what happens next: a failure past this point disables the mod rather than
    // dropping it, so a player can see it listed with a reason instead of concluding their
    // install silently failed. See mods.h's mods_init comment for the full rationale.
    if (m.api > MODS_API_VERSION) {
      // A well-formed, validly-declared api= this build is simply too old for -- unlike every
      // refusal inside load_one, this is not a malformed package, so it is listed disabled
      // rather than dropped (see load_one's comment on its api= branch).
      fprintf(stderr, "mods: '%s' wants api %d, this build only supports %d; disabling it for this session\n",
              name, m.api, MODS_API_VERSION);
      snprintf(m.error, sizeof m.error, "needs mod api %d (this build supports up to %d)",
               m.api, MODS_API_VERSION);
      m.enabled = false;
    } else {
      // Only now -- known to apply to this game, known to still have a slot, and declaring an api
      // this build actually supports -- is it worth the cost and risk of actually running this
      // mod's Lua. run_main_lua already printed why, and filled m.error, on failure.
      m.enabled = run_main_lua(dir, name, &m);
    }
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
  // before resetting count -- not after, since mods[] is about to become meaningless. The
  // `!mods[i].L` skip is no longer a defensive no-op kept "just in case": since Task 6, a
  // disabled mod (mods_enabled(i) false) is routinely stored in mods[] with L == NULL -- run_main_lua
  // either never created one (main.lua missing, an allocation failure before lua_newstate) or
  // already closed and freed it itself on the failure path before ever returning -- so this is
  // the thing that keeps mods_shutdown from calling lua_close(NULL) on every disabled mod in the
  // list.
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
const char *mods_error(int i) { return i >= 0 && i < count ? mods[i].error : ""; }
