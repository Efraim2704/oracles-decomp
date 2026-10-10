#include "mods/mods.h"
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

typedef struct {
  char id[MOD_FIELD_LEN];     // canonical: the validated folder name, never the mod.ini field directly
  char name[MOD_FIELD_LEN];
  char ini_id[MOD_FIELD_LEN]; // raw "id=" value from mod.ini (escaped; see escape_into), kept only
                              // to cross-check against id
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
} Mod;

static Mod mods[MODS_MAX];
static int count;
// Set by mods_init, read by nothing in this task; Task 5/8's oracles.game() binding is what
// eventually consumes it, so this is an intentional dead store for now, not an oversight.
static bool host_seasons;

static void trim(char *s) {
  char *start = s;
  while (*start == ' ' || *start == '\t') start++;
  size_t len = strlen(start);
  while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
  start[len] = 0;
  if (start != s) memmove(s, start, len + 1);
}

// Copies src into dst (bounded by dst_size, always NUL-terminated), replacing every byte outside
// printable ASCII (0x20..0x7e) with a \xHH escape. A mod.ini value reaches a terminal two ways --
// this process's own stderr diagnostics, and a future caller of mods_name()/mods_id() printing it
// directly (a mod list UI, say) -- so sanitizing once, at the point a value is stored, keeps both
// safe rather than re-escaping at every print site. Without this, a value like
// "victim\r\x1b[2K\rmods: 'victim' loaded OK (api 1)" overwrites the current terminal line with a
// forged success message the moment it's printed raw.
static void escape_into(char *dst, size_t dst_size, const char *src) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < dst_size; p++) {
    if (*p >= 0x20 && *p < 0x7f) {
      dst[o++] = (char)*p;
    } else {
      char esc[5];
      snprintf(esc, sizeof esc, "\\x%02x", *p);
      for (int i = 0; esc[i] && o + 1 < dst_size; i++) dst[o++] = esc[i];
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
      escape_into(m->ini_id, sizeof m->ini_id, value);
      m->ini_id_present = true;
    } else if (!strcmp(key, "name")) {
      escape_into(m->name, sizeof m->name, value);
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
          escape_into(esc, sizeof esc, tok);
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
// fopen/fgets), free of embedded NUL bytes, parses, declares a matching id, and has a usable
// api= and games=. Prints its own stderr diagnostic on every refusal path. Does not decide
// whether the mod applies to the game being played, or apply the MODS_MAX cap -- those depend on
// state (which game, how many mods are already loaded) that spans the whole scan, so they stay
// in mods_init's loop.
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

  snprintf(m->id, sizeof m->id, "%s", name);
  if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", name);
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
    mods[loaded++] = m;
  }
  count = loaded;
  if (dropped_for_cap > 0)
    fprintf(stderr, "mods: %d mod(s) dropped, only the first %d are loaded\n", dropped_for_cap, MODS_MAX);

  for (size_t i = 0; i < n; i++) free(names[i]);
  free(names);
  return true;
}

void mods_shutdown(void) { count = 0; }

int mods_count(void) { return count; }
const char *mods_id(int i) { return i >= 0 && i < count ? mods[i].id : ""; }
const char *mods_name(int i) { return i >= 0 && i < count ? mods[i].name : ""; }
bool mods_enabled(int i) { return i >= 0 && i < count && mods[i].enabled; }
