#pragma once
// The security-critical containment a mod's Lua state runs under: a bounded custom allocator,
// an instruction-count + wall-clock budget enforced via a yielding hook, and a hardened library
// set with specific holes closed or wrapped (see sandbox.c for exactly which, and why each one
// breaks *this* module's own deliverable rather than merely overlapping with Task 7's eventual
// sandbox). Split out of mods.c into its own header/source pair because mod_resume_budgeted is
// also what a later per-frame callback runner (Task 8) needs to call from outside mods.c, and
// because this is one cohesive unit with a clean seam, not because mods.c's own discovery/
// mod.ini logic has anything to do with running Lua.
#include <stdbool.h>
#include <stddef.h>

struct lua_State;

// Per-mod Lua heap accounting, passed as the `ud` of lua_newstate's custom allocator. One of
// these is malloc'd by the caller (mods.c) for the lifetime of a mod's Lua state and freed right
// alongside it -- mod_lua_alloc only ever reads/writes an already-allocated one, it does not own
// the allocation itself.
typedef struct {
  size_t used;
  size_t cap;
} ModAlloc;

// How much Lua heap one mod's main.lua may use at once (and, from Task 8 on, its per-frame
// callbacks too, since they share the same state and so the same cap). See sandbox.c for the
// measurement this was picked from.
#define MOD_MEM_CAP ((size_t)8 * 1024 * 1024)

// Lua's custom allocator for one mod's state, enforcing MOD_MEM_CAP against `ud` (a ModAlloc*).
// Passed to lua_newstate as the allocator function; never called directly.
void *mod_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

// Opens the standard libraries for a mod's state, with specific holes closed or wrapped -- see
// sandbox.c for exactly which, and why. Not Task 7's sandbox (string/table/math only): io, os
// and package/require stay fully open.
void mod_open_libs_hardened(struct lua_State *L);

// The shared instruction total and wall-clock deadline mod_resume_budgeted enforces by default
// for a mod's one-shot main.lua load. See sandbox.c for how each number was chosen. A later
// per-frame caller is expected to pass its own, much smaller values instead of these two.
#define MOD_LOAD_INSTRUCTION_BUDGET 50000000
#define MOD_LOAD_WALLCLOCK_DEADLINE_SECONDS 2.0

// Drives `thread` (freshly loaded with a function to call via lua_resume, `nargs` of them
// already pushed) to completion or exhaustion, under a budget of `total` VM instructions *and*
// a `deadline_seconds` wall-clock cutoff. See sandbox.c for the full contract: what *exhausted
// and *deadline_hit mean, why a clean LUA_OK is not sufficient on its own, and two things a
// per-frame caller must bring itself that this function cannot provide (no poison state across
// calls, and an exhausted thread cannot simply be resumed again).
int mod_resume_budgeted(struct lua_State *thread, int nargs, long long total,
                         double deadline_seconds, bool *exhausted, bool *deadline_hit);
