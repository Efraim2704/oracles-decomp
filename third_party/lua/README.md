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

Builds clean under this project's `-Wall -Wextra -Wno-unused-parameter` (clang on macOS); no
warning suppression was needed for these sources.
