#include "mods/api.h"
#include "mods/mods.h"
#include "lua.h"
#include "lauxlib.h"
#include <stdio.h>
#include <string.h>

// Large enough for any ordinary log line (and mods_escape_into's worst case, where a byte of
// input can expand to a 4-byte \xHH escape) without making the buffer itself a place a mod can
// grow memory usage unboundedly -- it can't, since mods_escape_into is already bounded by
// dst_size and simply truncates past it. A log line this long is already not something a status
// line could usefully show in full.
static char last_log[512];

const char *mods_last_log(void) { return last_log[0] ? last_log : NULL; }
void mods_api_clear_log(void) { last_log[0] = 0; }

static int l_log(lua_State *L) {
  // luaL_checklstring, not luaL_checkstring: a Lua string can contain an embedded NUL, and
  // checkstring's bare `const char *` would silently truncate at it with nobody having decided
  // that was fine. The decision made here is not to truncate at all: mods_escape_into renders
  // every byte outside printable ASCII -- NUL included -- as a literal \xHH escape, so the whole
  // message survives (readably) in last_log and in the stderr line below. That's the same
  // escaper mods.c already uses for mod.ini values, reused here rather than duplicated, and it
  // closes off the same terminal-injection hole an unescaped CR/ANSI-escape log string would
  // otherwise open (a mod could otherwise print "\r\x1b[2K" followed by a forged status line).
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  mods_escape_into(last_log, sizeof last_log, s, len);
  fprintf(stderr, "mod: %s\n", last_log);
  return 0;
}

// The one list of what the API is. MODDING.md must document exactly these.
static const luaL_Reg api[] = {
  {"log", l_log},
  {NULL, NULL},
};

void mods_api_open(lua_State *L) {
  luaL_newlib(L, api);
  lua_setglobal(L, "oracles");
}

int mods_api_count(void) { return (int)(sizeof api / sizeof api[0]) - 1; }
const char *mods_api_name(int i) { return i >= 0 && i < mods_api_count() ? api[i].name : ""; }
