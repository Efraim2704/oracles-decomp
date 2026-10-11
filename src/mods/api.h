#pragma once
// The Lua bindings. Every binding is named in one table so MODDING.md can be checked against it:
// an undocumented function is a test failure, the way leaked ROM bytes already are.
#include <stdbool.h>

struct lua_State;

// Puts the `oracles` table into the given state.
void mods_api_open(struct lua_State *lua_vm);

// The registry, for the documentation check.
int mods_api_count(void);
const char *mods_api_name(int i);

// Clears the most recent oracles.log() message (see mods_last_log in mods.h). Called by
// mods_shutdown so a silent mod loaded after a chatty one doesn't inherit its predecessor's stale
// log line -- not itself a Lua-visible binding, so it is not one of the entries mods_api_count/
// mods_api_name enumerate.
void mods_api_clear_log(void);
