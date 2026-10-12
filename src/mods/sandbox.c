#include "mods/sandbox.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

// The monotonic clock the budget below runs on. clock_gettime is not usable on Windows: mingw-w64
// keeps it in winpthreads, which only a posix-thread-model toolchain links implicitly -- Homebrew's
// mingw does, Ubuntu's (win32 model, what the release workflow installs) does not, so the release
// build's Windows link failed on `undefined reference to clock_gettime` while the same cross-build
// succeeded locally. QueryPerformanceCounter needs nothing but kernel32, is monotonic, and is what
// winpthreads would have called anyway. Returns 0 on success, like clock_gettime, so both callers
// keep their fail-closed handling of a clock that did not answer.
static int mono_now(struct timespec *ts) {
#ifdef _WIN32
  LARGE_INTEGER freq, now;
  if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&now) || freq.QuadPart <= 0) return -1;
  ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
  ts->tv_nsec = (long)(((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
  return 0;
#else
  return clock_gettime(CLOCK_MONOTONIC, ts);
#endif
}

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
  struct timespec deadline;   // mono_now() point past which this run is cut off regardless
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
// other thread's own accounting until it checks in.
//
// Originally 1,000,000, picked only against that coroutine-slack concern. Task 8 (mods.c's
// call_handler) found a second, sharper reason this matters: the wall-clock deadline passed to
// mod_resume_budgeted is only ever consulted when this hook fires, so this constant is also the
// floor on how promptly *any* caller's deadline can actually be enforced -- a caller with a tiny
// deadline (Task 8's own per-frame budget is a few milliseconds, against a 16.7ms frame) gets no
// benefit from that deadline being small if the hook that checks it only fires once every
// 1,000,000 instructions, because an adversarial handler built to make every instruction
// maximally expensive (the shape right above this comment: repeated pcall/error, 50-150x the
// per-instruction cost of plain arithmetic) can take tens of milliseconds of real time to retire
// even one checkin's worth of instructions -- confirmed by measurement, twice independently: a
// handler of exactly that shape, run under mod_resume_budgeted with this constant at 1,000,000
// and a nominal 4ms deadline, actually took ~67ms (both this repo's own benchmark against
// liboraclesmods.a and an independent re-measurement by a reviewer agreed to within noise) to be
// caught -- 17x the deadline it was nominally bounded by, because the deadline simply never got
// *checked* until that first, very expensive, checkin arrived.
//
// Lowering this constant is what actually closes that gap, and it is cheap to lower: the hook
// decrements a counter and compares two integers regardless of the threshold, so the per-firing
// cost (one mono_now -- a vDSO call on Linux, a counter read on Windows, tens of nanoseconds --
// plus the arithmetic above) is
// the same at any threshold; only how *often* that cost is paid changes, and legitimate code pays
// it rarely to never, since an ordinary handler finishes before the hook ever fires once. Measured
// directly (9-trial medians, this dev machine, against the same adversarial pcall/error shape,
// total=1,000,000 and deadline=4ms -- Task 8's own per-frame shape):
//
//   checkin    adversarial-handler median      vs. nominal 4ms deadline
//   1,000,000  ~67ms                           17x over
//     100,000  ~11ms                            3x over
//      50,000  ~5.9ms                         1.5x over
//      30,000  ~5.4ms                         1.4x over
//      20,000  ~4.8ms                         1.2x over
//      10,000  ~4.2ms                         1.05x over  <- chosen
//       5,000  ~4.2ms                         1.05x over
//       2,000  ~4.1ms                         1.02x over
//       1,000  ~4.0ms                         1.01x over
//
// 10,000 is where the curve has already flattened: tightening further to 1,000 (10x more
// check-ins everywhere, including the load path's 50,000,000-instruction budget) buys only
// another ~0.2ms, while 10,000 already brings the worst case to within ~5% of the nominal
// deadline instead of 17x over it. The same benchmark run against the load path's own shape
// (total=50,000,000, deadline=2.0s) with two representative *legitimate* finite scripts --
// a_legitimately_heavy_pattern_still_loads' own ~19,000-word gsub, and the thousand-entry table
// build MOD_MEM_CAP's own comment measures -- showed no measurable trend across the entire
// 1,000..1,000,000 range tried (every value landed within the same sub-millisecond run-to-run
// noise band), confirming the "noise, not signal" prediction this change was made on: checking in
// 100x more often costs nothing a legitimate mod would ever notice, because a legitimate mod's
// own handler or main.lua finishes long before the hook needs to fire even once.
//
// MOD_PATTERN_STEP_BUDGET (third_party/lua/lstrlib.c) is a separate mechanism -- a step counter
// inside the pattern-matching engine itself, not tied to this hook or to Lua's own VM-instruction
// count at all -- and is unaffected by this constant either way; re-confirmed by rerunning
// a_catastrophic_find_pattern_is_refused_by_the_matcher_itself and its gsub counterpart after
// this change, unchanged.
#define MOD_INSTRUCTION_CHECKIN 10000

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
    // (the monotonic clock does not fail in practice) and costs a hostile one nothing it wasn't
    // already going to lose.
    bool deadline_passed = mono_now(&now) != 0 ||
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
  // value rather than adding an offset to uninitialised garbage -- the epoch is unspecified on
  // both clocks behind mono_now (POSIX leaves it open, QueryPerformanceCounter counts from boot)
  // but always non-negative and only ever increases from some point at or before process start,
  // so a deadline of "0" reads as already-elapsed the moment the hook first checks it, same
  // fail-closed choice as above.
  if (mono_now(&budget.deadline) == 0) {
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

// Task 7's sandbox review decided `load` should be withheld outright rather than kept wrapped to
// force text mode (the fix this function used to apply, l_load_text_only, now deleted): nothing
// in the planned API needs a mod to compile Lua source at runtime at all, so removing `load`
// removes the whole bytecode-chunk surface -- the thing that actually crashed the host (a SIGSEGV
// on the first fuzz trial of `load(corrupted_dump, nil, 'b')`) -- rather than leaving it reachable
// and continuing to police every call for the one argument that made it dangerous. `string.dump`
// is withheld alongside it for the same reason the review raised it: once nothing can call `load`
// on a mod's output, `string.dump` is inert on its own, but leaving a dump/load pair that would
// become reassemblable again the moment either one is restored is a pointless risk for a function
// nothing in the planned API has a use for either.
//
// Withholds the standard-library surface that plain luaL_openlibs would otherwise grant beyond
// what the spec's sandbox actually names -- "the standard string, table and math libraries" plus
// the oracles table mods_api_open installs separately -- closing both holes found while building
// the earlier CPU/memory containment above (which predate this list and remain load-bearing
// regardless of where the sandbox line falls) and the filesystem/process/host-channel surface the
// spec never grants a mod in the first place:
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
// - "dofile" and "loadfile": both reach the exact same binary-chunk crash `load` did, via
//   luaL_loadfile(x) internally, with no mode argument a caller could even force to "t" the way
//   l_load_text_only used to for `load` -- found while fixing `load` specifically, not separately
//   reported. Withheld outright rather than wrapped for the same reason `load` now is: a mod has
//   no legitimate need to execute an arbitrary file straight off disk or compile one, distinct
//   from the ordinary read/write primitives "io" would otherwise offer (and "io" itself is
//   withheld below besides).
//
// - "io", "os": the spec's sandbox grants string, table and math -- nothing in either of these.
//   Beyond that, both are a direct route around the CPU containment this module otherwise
//   enforces: os.exit() takes the whole host process down outright with no error for anything
//   here to catch, and a blocking call (os.execute("sleep 30"), any io read/write against a slow
//   or adversarial filesystem path) stalls past the wall-clock deadline with no VM instruction
//   ever executing for mod_instruction_limit_hook to fire on, since that hook only runs between
//   Lua opcodes, never while a C library call it's inside of is blocked. This is also why no
//   number of per-name wrappers under "os" or "io" was considered as an alternative to withholding
//   each outright: os.clock()/os.time() are individually harmless, but the module as a whole is
//   not something a partial allow-list can make safe without re-deriving io's and os's entire
//   surface by hand. One consequence worth recording here because nothing else will: with "os"
//   gone, a mod has no os.time()/os.clock() either, so anything a mod needs to measure must count
//   frames instead -- frame-accurate by construction, not a workaround.
//
// - "require", "package": require() reads files off disk via package.path/package.searchers, the
//   same filesystem reach "io" and "dofile" are withheld for; package itself exposes
//   package.loadlib (inert today only because LUA_USE_DLOPEN is off in loadlib.c's build
//   configuration -- CMakeLists.txt does not define it -- which must stay off for as long as mods
//   run unsandboxed C code is a possibility, since enabling it would hand a mod a second route to
//   debug.sethook, and everything else "debug" closes above, through any system library that
//   happens to export it) plus package.path/package.cpath/package.preload/package.searchers, none
//   of which the spec's sandbox grants either.
//
// - "print", "warn": both write directly to this process's own stdout/stderr, a channel neither
//   the spec's sandbox nor oracles.log (the facility a mod actually has for this) ever grants a
//   mod -- and, concretely, the very channel this test binary's own capture_stderr mechanism and
//   ctest's own output rely on, so a mod writing there corrupts the thing watching it rather than
//   merely being unauthorised. oracles.log already covers every legitimate use "print" or "warn"
//   would have had.
//
// - "utf8": luaL_openlibs installs it, but the spec's sandbox names string, table and math only --
//   "utf8" is simply not one of the three, with no CPU/memory/host-reach defect of its own driving
//   this one; it is withheld because it was never granted, not because it was found unsafe.
//
// setmetatable is replaced with l_setmetatable_no_gc above rather than withheld outright, since it
// is otherwise genuinely useful and safe for the one thing it closes -- see that function's own
// comment for what and why wrapping, not removal, is correct there (`load`, by contrast, had no
// comparably narrow fix once withholding it outright was on the table at all -- see above).
//
// Every withheld name here is removed from both the globals table and package.loaded (require()
// checks package.loaded first, so nil-ing only the global would leave e.g.
// `require('coroutine')` able to resurrect it) -- a defence that is currently moot in its own
// right, since "require" is itself withheld and so nothing in a mod's Lua can call it at all, but
// kept anyway (two lines per name) so it still holds if a later change ever restores "require"
// without separately revisiting this function. Several names in the list below were never
// loadable modules in the first place (load, loadfile, dofile, require, print, warn, since none
// of them is a luaL_requiref'd library name) -- clearing package.loaded for those is a harmless
// no-op, kept uniform rather than special-cased, the same way "dofile" already was before this
// list grew.
void mod_open_libs_hardened(lua_State *L) {
  luaL_openlibs(L);

  lua_pushcfunction(L, l_setmetatable_no_gc);
  lua_setglobal(L, "setmetatable");

  // Inert once "load" (below) is gone -- nothing can reassemble what it produces -- but kept
  // withheld anyway rather than left reachable for no purpose; see this function's own comment.
  lua_getglobal(L, "string");
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);

  lua_getglobal(L, "package");
  lua_getfield(L, -1, "loaded");
  static const char *const withheld[] = {
    "debug", "coroutine", "dofile", "loadfile", "io", "os", "require", "package",
    "load", "print", "warn", "utf8",
  };
  for (size_t i = 0; i < sizeof withheld / sizeof withheld[0]; i++) {
    lua_pushnil(L);
    lua_setglobal(L, withheld[i]);
    lua_pushnil(L);
    lua_setfield(L, -2, withheld[i]);      // package.loaded[withheld[i]] = nil -- a no-op for the
                                            // several names above that were never a loadable
                                            // module to begin with; see this function's own
                                            // comment for which and why that's fine.
  }
  lua_pop(L, 2);           // package.loaded, package
}
