#include "mods/mods.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// id doubles as the folder name and, from Task 9 on, a path component for a mod's store file.
// This is the one gate between an attacker-controlled mod.ini/folder name and a path traversal:
// no byte this rejects can spell ".." or "/" or "\", so a validated id is unrepresentable as a
// traversal on either a POSIX host or the MinGW/Windows target, where "\" is also a separator.
bool mods_id_valid(const char *id) {
  if (!id || !*id) return false;
  for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
    bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-';
    if (!ok) return false;
  }
  return true;
}

typedef struct {
  char id[64];          // canonical: the validated folder name, never the mod.ini field directly
  char name[64];
  char ini_id[64];      // raw "id=" value from mod.ini, kept only to cross-check against id
  bool ini_id_present;
  bool enabled;
  bool ages, seasons;
  int api;               // parsed "api=" value, or -1 if absent/non-numeric/zero/negative
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

// Reads one logical line into `buf` (size n), discarding anything past the buffer on an
// over-long line rather than leaving the tail for the next call to misparse as its own
// key=value pair. Returns false at EOF with nothing read.
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

// mod.ini is the same shape as settings.ini: key=value, '#' comments (even indented), blanks
// ignored, whitespace tolerated around both key and value. Unlike settings_parse's
// `%31[a-z_] = %255s` approach, this keeps interior spaces in the value, since mod names and
// authors contain them. Every value is treated as hostile: over-long, empty, or full of
// coincidental '=' or '#' characters are all handled, not just well-formed input. `label` is the
// mod's folder name, used only to name the mod in a per-token "games=" warning.
static bool read_ini(const char *path, const char *label, Mod *m) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  char line[256];
  while (read_logical_line(f, line, sizeof line)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == 0) continue;

    char *eq = strchr(p, '=');
    if (!eq) continue;
    *eq = 0;
    char *key = p, *value = eq + 1;
    trim(key);
    trim(value);
    if (!*key) continue;

    if (!strcmp(key, "id")) {
      snprintf(m->ini_id, sizeof m->ini_id, "%s", value);
      m->ini_id_present = true;
    } else if (!strcmp(key, "name")) {
      snprintf(m->name, sizeof m->name, "%s", value);
    } else if (!strcmp(key, "api")) {
      // `end` is initialized even on the empty-value path (where strtol is not called) so this
      // never reads an uninitialized pointer, even though *value's short-circuit already makes
      // that path safe on its own -- relying on evaluation order here would be too subtle.
      char *end = value;
      long v = *value ? strtol(value, &end, 10) : 0;
      m->api = (*value && !*end && v >= 1 && v <= INT_MAX) ? (int)v : -1;
    } else if (!strcmp(key, "games")) {
      m->ages = m->seasons = false;
      char buf[256];
      snprintf(buf, sizeof buf, "%s", value);
      for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        trim(tok);
        if (!strcmp(tok, "ages")) m->ages = true;
        else if (!strcmp(tok, "seasons")) m->seasons = true;
        else if (*tok)
          fprintf(stderr, "mods: '%s' names unrecognised game '%s' in its games=, ignoring that token\n",
                  label, tok);
      }
    }
  }
  fclose(f);
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
    if (e->d_name[0] == '.') continue;                   // skip ".", "..", and hidden entries
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

    if (!mods_id_valid(name)) {
      fprintf(stderr, "mods: '%s' is not a valid mod id (lowercase letters, digits, hyphen only), skipping\n", name);
      continue;
    }

    char entry_path[1024];
    snprintf(entry_path, sizeof entry_path, "%s/%s", dir, name);
    struct stat st;
    if (stat(entry_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      fprintf(stderr, "mods: '%s' in the mods folder is not a directory, skipping\n", name);
      continue;
    }

    char ini_path[1040];
    snprintf(ini_path, sizeof ini_path, "%s/mod.ini", entry_path);
    Mod m = {.enabled = true, .api = -1};
    if (!read_ini(ini_path, name, &m)) {
      fprintf(stderr, "mods: '%s' has no mod.ini, skipping\n", name);
      continue;
    }

    // The folder name is the canonical id, and mod.ini must declare the same id itself -- a mod
    // package should say who it is, not rely solely on whatever folder it happens to be unpacked
    // into. mod.ini's "id=" is never used to build a path, so a traversing value here (e.g.
    // "../evil") simply fails this string comparison rather than ever being dereferenced as a
    // filename.
    if (!m.ini_id_present) {
      fprintf(stderr, "mods: '%s/mod.ini' has no id=; it must declare id=%s to agree with its folder name\n",
              name, name);
      continue;
    }
    if (strcmp(m.ini_id, name) != 0) {
      fprintf(stderr, "mods: '%s/mod.ini' declares id '%s', which does not match its folder name '%s'; refusing\n",
              name, m.ini_id, name);
      continue;
    }

    if (m.api < 1) {
      fprintf(stderr, "mods: '%s' has a missing or non-numeric api, refusing\n", name);
      continue;
    }
    if (m.api > MODS_API_VERSION) {
      fprintf(stderr, "mods: '%s' wants api %d, this build only supports %d, refusing\n",
              name, m.api, MODS_API_VERSION);
      continue;
    }
    if (!m.ages && !m.seasons) {
      fprintf(stderr, "mods: '%s' declares no supported game, refusing\n", name);
      continue;
    }
    if (!(seasons ? m.seasons : m.ages)) continue;        // valid mod, just not for this game

    if (loaded >= MODS_MAX) {
      dropped_for_cap++;
      continue;
    }
    snprintf(m.id, sizeof m.id, "%s", name);
    if (!m.name[0]) snprintf(m.name, sizeof m.name, "%s", name);
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
