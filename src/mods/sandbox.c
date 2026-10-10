#include "mods/sandbox.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ModAlloc and MOD_MEM_CAP are declared in sandbox.h: mods.c constructs a ModAlloc itself (it
// owns that allocation, see mods.c's run_main_lua) before handing it to lua_newstate as `ud`,
// so both need to be visible there too. The measurement behind the 8 MiB figure: a fresh
// lua_newstate()+luaL_openlibs() costs about 19 KB on this dev machine, and a main.lua that
// builds a 1000-entry table of {x=,y=,name=} sub-tables costs about 250 KB total -- 8 MiB is
// >30x that already-elaborate example, generous for anything a reasonable main.lua would do,
// while still being far too small for an unbounded-allocation loop to get anywhere:
// `local t={} while true do t[#t+1]=string.rep('x',1e6) end` allocates a megabyte-sized string
// per iteration, so it trips after roughly 8 of them. MODS_MAX (32) mods can all be loaded at
// once, each with its own state capped at this: 32 * 8 MiB = 256 MiB of host memory a full mods
// folder can hold resident simultaneously -- nothing enforces a total across mods today, this is
// the number that total could reach, recorded here because nothing else currently records it.
//
// Lua's custom-allocator contract (see third_party/lua/lmem.c's comment on frealloc): ptr == NULL
// means "create a new block of size nsize, no matter what osize says" -- osize is not reliably a
// byte count in that case, it can be a type tag Lua uses only for its own GC bookkeeping -- and
// nsize == 0 means "free ptr (which osize does correctly describe here), return NULL". So `used`
// is adjusted by nsize alone on creation, and by the difference on resize/free, and osize is never
// consulted when ptr == NULL. Returning NULL for an over-cap allocation is exactly how an ordinary
// out-of-memory realloc looks to Lua, which already knows how to turn that into a clean LUA_ERRMEM
// error -- so the memory cap needs no Lua-side cooperation at all, unlike the instruction budget
// below, which has to raise that error itself.
void *mod_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  ModAlloc *a = (ModAlloc *)ud;
  if (ptr == NULL) {
    if (nsize == 0) return NULL;                    // frealloc(ud, NULL, x, 0) is defined as a no-op
    // Checked before the addition, not just via its result, so overflow safety does not rest on
    // malloc() happening to fail for an absurd nsize: on a 32-bit size_t target a request near
    // SIZE_MAX could otherwise wrap a->used + nsize back under a->cap and pass the check it was
    // supposed to fail.
    if (nsize > a->cap) return NULL;
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
  // push a mod over its own cap and so is never blocked by it. Reached by array-part growth
  // (`local t={} for i=1,1e9 do t[i]=i end`), not by string.rep('x', N): a fresh string.rep call
  // is always a brand-new block (the ptr == NULL path above), never a resize of an existing one.
  if (nsize > osize && (nsize - osize > a->cap || a->used + (nsize - osize) > a->cap)) return NULL;
                                                                           // old block, still valid,
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
// reasoning that lua_error's longjmp would unwind through whatever protected call was running
// the chunk, straight back out to this module's own C code. That is bypassable in one line,
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
// mod_open_libs_hardened) -- a mod with no way to call lua_newthread at all has no way
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
    // Fail closed: if the clock call itself fails (observed on no real platform this targets,
    // but POSIX allows it), `now` is left uninitialised stack garbage -- comparing against that
    // would make deadline_passed effectively random, silently turning "the deadline mechanism"
    // into "the deadline mechanism, maybe" exactly when it can least afford to be unreliable.
    // Treating a clock failure as "the deadline has already passed" costs a healthy mod nothing
    // (clock_gettime does not fail in practice) and costs a hostile one nothing it wasn't
    // already going to lose.
    bool deadline_passed = clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec > b->deadline.tv_sec ||
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
    //
    // With "coroutine" withheld (mod_open_libs_hardened below), a mod has no way to create a
    // nested thread at all, and `thread` itself (always `runner` from run_main_lua, never
    // exposed to Lua as something a mod could coroutine.resume() directly) is never resumed a
    // second time within one mod_resume_budgeted call either -- so this re-arm is currently
    // unreachable in practice, kept deliberately as defence-in-depth and for whenever a later
    // task reuses mod_resume_budgeted somewhere coroutines (or repeated resumes of the same
    // thread) are possible again.
    lua_sethook(L, mod_instruction_limit_hook, LUA_MASKCOUNT, 1);
    lua_yield(L, 0);
  }
}

// Drives `thread` (freshly loaded with a function to call via lua_resume, nargs of them already
// pushed) to completion or exhaustion, under a budget of `total` VM instructions *and* a
// `deadline_seconds` wall-clock cutoff, both shared across `thread` and any coroutine it creates
// while running -- see the long comments above for why that sharing, not just a plain per-thread
// lua_sethook, is what actually makes either bound unbypassable *by a mod spinning up or
// resuming threads*. Neither bound is reachable at all while the VM is stuck somewhere that
// never retires another instruction in the first place (a livelocked recursive pcall; an
// unpatched pattern-matcher backtracking catastrophically) -- see run_main_lua's "what is and is
// not guaranteed" comment for the current, honest list of what that excludes. Returns the
// lua_resume status
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
// (g_mod_budget) is saved, scoped to one call, and restored before returning, success or
// failure alike (see the save/restore below for why restored, not just cleared).
//
// Two things a per-frame caller must bring itself, because this function has no per-mod state
// to provide them from:
//
// 1. There is no poison state. Every call gets a *fresh* `total`/`deadline_seconds` budget --
//    that is the whole point, reusing the same mod across frames -- but it also means a mod
//    whose handler loops forever gets a fresh full budget every single frame, cut off each
//    time at `deadline_seconds`, forever. At 2s per frame that is not "a disabled mod", it is a
//    permanently wedged game. Something above this function must notice repeated exhaustion and
//    mark that mod dead (stop calling its handler at all) -- mod_resume_budgeted cannot do this
//    itself, since by design it remembers nothing between calls.
//
// 2. An exhausted thread cannot be reused as-is. Once yielded via the budget (not a normal,
//    cooperative yield the mod intended), `thread` stays suspended in that exact state; a later
//    plain lua_resume on it would continue the *old*, already-failed call where it left off,
//    not start a new one. A caller that wants to try this mod's handler again next frame must
//    either lua_resetthread(thread) first or hand mod_resume_budgeted a brand-new thread each
//    time.
//
// A smaller note for the same reason: the `runner` thread run_main_lua creates is never popped
// off `L`'s own stack, so it (and everything it still references) stays alive for the lifetime
// of that mod's state -- harmless for a single one-shot load, but worth knowing before building
// something that creates many such threads per mod per frame.
int mod_resume_budgeted(lua_State *thread, int nargs, long long total,
                                double deadline_seconds, bool *exhausted, bool *deadline_hit) {
  ModBudget budget = { .remaining = total, .exhausted = false, .deadline_exceeded = false };
  // Fail closed here too: if the clock call fails, leave budget.deadline at its zero-initialised
  // value rather than adding an offset to uninitialised garbage -- CLOCK_MONOTONIC's epoch is
  // unspecified but always non-negative and only ever increases from some point at or before
  // process start, so a deadline of "0" reads as already-elapsed the moment the hook first
  // checks it, same fail-closed choice as above.
  if (clock_gettime(CLOCK_MONOTONIC, &budget.deadline) == 0) {
    long whole_secs = (long)deadline_seconds;
    long extra_nsecs = (long)((deadline_seconds - (double)whole_secs) * 1e9);
    budget.deadline.tv_sec += whole_secs;
    budget.deadline.tv_nsec += extra_nsecs;
    if (budget.deadline.tv_nsec >= 1000000000L) {
      budget.deadline.tv_sec += 1;
      budget.deadline.tv_nsec -= 1000000000L;
    }
  }
  // Saved and restored, not just set-then-cleared, so a reentrant call -- a future C binding
  // that calls back into Lua (and so, potentially, into mod_resume_budgeted again) from inside
  // an already-running budgeted call -- cannot silently disarm the *outer* call's budget by
  // clobbering this global out from under it: the outer call's own hook would otherwise see
  // g_mod_budget as NULL for the rest of its execution once the inner call returns and clears
  // it, making mod_instruction_limit_hook's `if (!b) return;` a silent no-op for a run that is
  // still, as far as the outer caller believes, under budget enforcement.
  ModBudget *previous_budget = g_mod_budget;
  g_mod_budget = &budget;
  lua_sethook(thread, mod_instruction_limit_hook, LUA_MASKCOUNT, MOD_INSTRUCTION_CHECKIN);
  int nres;
  int status = lua_resume(thread, NULL, nargs, &nres);
  g_mod_budget = previous_budget;
  *exhausted = budget.exhausted;
  *deadline_hit = budget.deadline_exceeded;
  return status;
}

// A finalizer is armed only if a metatable already carries a __gc key at the moment
// setmetatable() assigns it -- adding __gc to an already-set metatable afterwards does nothing
// (confirmed against this vendored build), so this is the one, complete choke point; the other
// route, debug.setmetatable(), is already unreachable since "debug" is withheld below. Rejecting
// outright, not silently stripping __gc, so a mod author relying on a finalizer is told so
// immediately rather than shipping something that silently never runs it.
//
// Why a __gc finalizer must never run at all, rather than merely being budgeted like everything
// else: third_party/lua/lgc.c's GCTM sets `L->allowhook = 0` before calling a finalizer, so no
// debug hook -- including mod_instruction_limit_hook -- can ever fire while one is running. No
// hook means neither the instruction budget nor the wall-clock deadline is reachable, so a
// finalizer that loops is a hang nothing in this module can stop. Confirmed in three shapes, all
// of which hang with __gc present and none of which are fixable by rearming the hook or keeping
// the budget "live" across the call that triggers GC: (1) `lua_close` at mod shutdown runs every
// live finalizer; (2) a mod that sets a looping-__gc object and then calls error() still hangs in
// run_main_lua's own lua_close *after* printing the refusal, so a rejected mod still takes the
// process down; (3) an ordinary mid-run garbage collection, reached from inside
// mod_resume_budgeted with no error or shutdown involved at all, can collect the object and hang
// there. This is the same hazard class as the hostile-__tostring case below -- Lua reached from
// *host* code (lua_close, the collector) rather than from inside mod_resume_budgeted's own
// lua_resume -- so anywhere else this module calls into Lua outside that one guarded call must be
// treated with the same suspicion.
static int l_setmetatable_no_gc(lua_State *L) {
  int t = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
  if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
    return luaL_error(L, "cannot change a protected metatable");
  if (t == LUA_TTABLE) {
    lua_getfield(L, 2, "__gc");
    bool has_gc = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (has_gc) return luaL_error(L, "cannot set a __gc metatable field in a mod");
  }
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;
}

// load()'s mode argument lets a caller request binary-chunk loading, and a corrupted binary dump
// crashes the host outright -- confirmed by fuzzing: a SIGSEGV on the first trial of
// `load(corrupted_dump, nil, 'b')`, strictly worse than any hang this module exists to prevent.
// There is no legitimate reason a mod ships pre-compiled bytecode, so this forces mode "t"
// regardless of what's requested, by calling through to the real `load` -- captured as this
// closure's one upvalue before it replaces the global -- with the mode argument always
// overridden. Preserves the "env argument was never passed" vs "env argument was explicitly nil"
// distinction the real `load` itself cares about (lua_isnone, not lua_isnil) by only forwarding
// an env argument when one was actually given.
static int l_load_text_only(lua_State *L) {
  int nargs = lua_gettop(L);
  bool has_env = nargs >= 4 && !lua_isnone(L, 4);
  lua_pushvalue(L, lua_upvalueindex(1));          // the real `load`
  lua_pushvalue(L, 1);                            // chunk
  if (nargs >= 2) lua_pushvalue(L, 2); else lua_pushnil(L);  // chunkname (or nil default)
  lua_pushliteral(L, "t");                        // mode, forced
  int call_nargs = 3;
  if (has_env) { lua_pushvalue(L, 4); call_nargs = 4; }
  lua_call(L, call_nargs, LUA_MULTRET);
  return lua_gettop(L) - nargs;
}

// Withholds and wraps the standard-library surface that plain luaL_openlibs would otherwise
// grant, closing holes found while building the containment above -- none of this is a start on
// Task 7's full sandbox (string/table/math only); each entry is a specific, load-bearing defect
// in *this* task's own deliverable (a budget that actually bounds a mod's CPU use and a host that
// does not crash or hang loading one), closed here because leaving it open breaks that mechanism
// today, not because it happens to overlap with where the eventual sandbox line falls:
//
// - "debug": debug.sethook() lets ordinary Lua code replace or remove *any* hook set from C,
//   including mod_instruction_limit_hook. A single `debug.sethook()` call at the top of main.lua
//   would silently disarm the anti-hang protection before the mod's own infinite loop ever ran --
//   confirmed empirically: unpatched, `debug.sethook() while true do end` inside a
//   1,000,000-instruction checkin ran a 2,000,000-iteration loop to completion instead of
//   aborting. (debug.setmetatable, the other route to a __gc finalizer below, is withheld by the
//   same entry.)
//
// - "coroutine": the shared-budget design above closes every bypass that *resumes the same
//   thread repeatedly*, but not one that mints a *fresh* thread every time -- lua_newthread
//   resets a new thread's own countdown to a full basehookcount regardless of how exhausted the
//   shared ModBudget already is, confirmed to hang past an 8-second hard alarm via
//   `while true do coroutine.resume(coroutine.create(spinner)) end` (or the same shape through
//   coroutine.wrap, or nested inside pcall). No amount of per-thread accounting closes this; only
//   not letting a mod create new threads at all does, for now. See the long comment above
//   ModBudget for what to do if a later task needs this library back.
//
// - "dofile": the same binary-chunk crash load() has (see l_load_text_only above) reaches the
//   host identically through dofile(), which calls luaL_loadfile with the same unrestricted
//   default mode internally and there is no argument to override it through. Found while fixing
//   load() specifically, not separately reported; withheld outright rather than wrapped, since a
//   mod has no legitimate need to execute an arbitrary file straight off disk (distinct from the
//   already-open io library's ordinary read/write primitives).
//
// setmetatable and load are replaced with l_setmetatable_no_gc / l_load_text_only above rather
// than withheld outright, since both are otherwise genuinely useful and safe -- see each
// function's own comment for what it closes and why wrapping, not removal, is correct there.
//
// Everything else luaL_openlibs would open is opened exactly as before -- io, os and
// package/require stay fully available, unlike the eventual sandbox (see this function's call
// site). package.loadlib exists but is inert on every platform this project builds for, only
// because LUA_USE_DLOPEN is off in loadlib.c's build configuration (CMakeLists.txt does not
// define it) -- enabling dynamic C library loading there would hand a mod a second route to
// debug.sethook (and everything else "debug" closes above) via any system library that exports
// it, so that flag must stay off for as long as mods run unsandboxed C code is a possibility.
//
// "debug", "coroutine" and "dofile" are each removed from both the globals table and
// package.loaded (require() checks package.loaded first, so nil-ing only the global would leave
// e.g. `require('coroutine')` able to resurrect it) -- nothing else the standard library opens
// re-registers any of these three names under either key, so this makes all of them unreachable
// by any ordinary means.
void mod_open_libs_hardened(lua_State *L) {
  luaL_openlibs(L);

  lua_getglobal(L, "load");
  lua_pushcclosure(L, l_load_text_only, 1);
  lua_setglobal(L, "load");

  lua_pushcfunction(L, l_setmetatable_no_gc);
  lua_setglobal(L, "setmetatable");

  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  static const char *const withheld[] = {"debug", "coroutine", "dofile"};
  for (size_t i = 0; i < sizeof withheld / sizeof withheld[0]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, withheld[i]);
    lua_pushnil(L);
    lua_setfield(L, -2, withheld[i]);      // package.loaded[withheld[i]] = nil (harmless no-op
                                            // for "dofile", which was never a loadable module --
                                            // kept uniform rather than special-cased)
  }
  lua_pop(L, 2);           // package.loaded, package
}
