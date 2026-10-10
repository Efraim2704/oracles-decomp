# Lua

Lua 5.4.9 (https://www.lua.org/ftp/lua-5.4.9.tar.gz), used to run mods (`src/mods/`).

MIT licensed. Upstream ships no top-level `LICENSE` file; the licence text lives in
`doc/readme.html` of the distribution (https://www.lua.org/license.html). Not vendored here
since this directory only carries the library sources.

Local changes:

- `lua.c` and `luac.c` are not vendored: they are the standalone interpreter and compiler, and the
  app embeds the library only.
- The build (`CMakeLists.txt`) defines `LUA_USE_POSIX` directly instead of `LUA_USE_LINUX` or
  `LUA_USE_MACOSX`. In `luaconf.h`, both of those also pull in `LUA_USE_DLOPEN`; defining
  `LUA_USE_POSIX` alone leaves it undefined. This is deliberate, not an oversight: mods are
  untrusted code, and without `LUA_USE_DLOPEN`, `package.loadlib` cannot load native `.so`/
  `.dylib` modules, so a mod is limited to the Lua language itself and whatever host API we choose
  to expose. Switching the define to `LUA_USE_LINUX`/`LUA_USE_MACOSX` would silently hand mods the
  ability to load arbitrary native code — don't.

- `lstrlib.c`'s `MatchState` carries one extra field, `steps`, and `match()` decrements and
  checks it the same way it already does `matchdepth` (same `"pattern too complex"` error, so the
  message stays idiomatic). This is a deliberate divergence from upstream Lua semantics, not a
  bug fix upstream would accept: `matchdepth` bounds recursion *depth*, but a pattern like
  `('.-'):rep(10) .. 'b'` matched against 40 bytes of `'a'` stays shallow while backtracking
  catastrophically -- `string.find(('a'):rep(40), ('.-'):rep(10)..'b')` hangs past 20 seconds on
  this project's own measurement, consuming no VM instructions the whole time, since the C
  recursion inside `match()` never returns to the bytecode dispatcher. mods.c's instruction
  budget and wall-clock deadline (`src/mods/sandbox.c`) are exactly what this host relies on to
  bound a mod's CPU use, and neither one is reachable while stuck inside the matcher -- no hook
  fires there -- so nothing *outside* `lstrlib.c` can close this; the host genuinely cannot bound
  pattern-matching work any other way. `steps` is initialised once per Lua-level call to
  `string.find`/`match`/`gmatch`/`gsub` (in `prepstate`, not `reprepstate`, which runs once per
  *starting position* within one call -- resetting there would let a long subject multiply the
  budget by its number of positions, defeating it) and is never reset mid-call; its budget
  (50,000,000) was sized by measuring real workloads, not guessed: `gsub` over a 1 MB subject
  with an ordinary pattern costs 450,000-620,000 calls to `match()` (3-30 ms), and even a 10 MB
  subject -- already past what a mod's 8 MiB memory cap could hold -- costs about 6.2 million,
  linear with size; 50,000,000 leaves roughly 8-100x headroom over those measurements while the
  pathological case above still raises `"pattern too complex"` in well under a second (measured:
  ~200 million calls accumulate every 2 seconds, so the budget is spent in a small fraction of
  that). Once the error is raised, control returns to the VM, which starts retiring bytecode
  instructions again, so the existing instruction budget and wall-clock deadline become reachable
  as a second line of defence from that point on -- this patch does not need to be a perfect or
  permanent bound by itself, only to get control back out of the matcher.

Builds clean under this project's `-Wall -Wextra -Wno-unused-parameter` (clang on macOS); no
warning suppression was needed for these sources.
