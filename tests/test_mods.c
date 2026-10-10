#include "unit.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The vendored interpreter builds and runs: the floor everything else stands on.
static void lua_runs(void) {
  lua_State *L = luaL_newstate();
  ASSERT(L);
  luaL_openlibs(L);
  ASSERT_EQ(luaL_dostring(L, "return 1 + 1"), LUA_OK);
  ASSERT_EQ((int)lua_tointeger(L, -1), 2);
  lua_close(L);
}

int main(void) {
  RUN(lua_runs);
  return 0;
}
