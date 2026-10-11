# Mods

A mod is a small Lua script that runs alongside the game and draws on top of it. It can read the
game's memory, put text, boxes and item icons on the screen, react to room changes, and remember
things between sessions. It cannot change the game: the game runs exactly as it always does, and your
mod watches and draws.

This page is for writing one. For playing, see the [README](README.md).

Mods are new. The API below is small on purpose, and it is versioned (`api=1`), so a mod written
today keeps working as it grows. See [Stable and unstable](#stable-and-unstable) for what is safe to
build on.

## Installing a mod

A mod is a folder. Drop it in the `mods` folder inside the app's own folder — the same folder your
saves live in:

| System | Folder |
|---|---|
| macOS | `~/Library/Application Support/oracles-decomp/oracles/mods/` |
| Windows | `%APPDATA%\oracles-decomp\oracles\mods\` |
| Linux | `~/.local/share/oracles-decomp/oracles/mods/` |
| Android | the app's own storage |

Create `mods` yourself if it isn't there. Mods are picked up when a game starts, so restart the game
(leaving to the launcher and choosing a file again is enough) after adding or editing one. They load
in alphabetical order by folder name, and at most 32 load at once.

There is no in-app mod manager yet — no list, no on/off switches, no installer. A mod is installed by
being in that folder and removed by deleting it.

**Only install mods you trust.** A mod is a program, and the app keeps it boxed in deliberately: it
cannot open your files (only its own small save file, through `oracles.store`), start other
programs, or reach the network (see [What a mod can and cannot
see](#what-a-mod-can-and-cannot-see)), and one that loops forever or eats memory is cut off rather
than taking the game with it. But a box is not a promise that a hostile script can do nothing
at all — see [Limitations, honestly](#limitations-honestly).

## What's in a mod

Two files, in a folder named after the mod:

```
mods/
  essence-tracker/
    mod.ini
    main.lua
```

The folder name is the mod's **id**. It may contain only lowercase letters, digits and hyphens
(`a`-`z`, `0`-`9`, `-`), and must be under 64 characters. No spaces, no capitals, no underscores.

### mod.ini

One `key=value` per line. Blank lines are fine, `#` starts a comment, and spaces around the key and
the value are ignored (spaces inside a value are kept). Both Unix and Windows line endings work.
Lines longer than 255 characters are cut.

```ini
id=essence-tracker
name=Essence Tracker
version=1.0
author=you
api=1
games=ages,seasons
```

| Key | Required | What it does |
|---|---|---|
| `id` | yes | Must be exactly the folder name. A mod that disagrees with its own folder is refused — this is how a package says who it is rather than relying on where it was unzipped. |
| `api` | yes | Which version of the API the mod was written against. Must be 1 or more. This build supports up to **1**. |
| `games` | yes | `ages`, `seasons`, or `ages,seasons`. A mod is only loaded for a game it names. Anything else in the list is reported and ignored, so a typo costs you that one word, not the mod. |
| `name` | no | The display name. Defaults to the id. |

Any other key is ignored, so `version=` and `author=` above are documentation for whoever reads your
package — the app does not read them today, and keeping them is harmless.

Get `id`, `api` or `games` wrong and the folder is not treated as a mod at all: it is skipped, with a
line on the app's log saying why. Declare an `api` higher than the build supports and the mod is kept
but switched off, so it shows up as broken rather than silently missing.

### main.lua

Plain Lua 5.4 source text — compiled bytecode is refused, not run. It is read and run when your mod
loads, and a typical one registers its handlers and then gets out of the way:

```lua
oracles.log("hello from my first mod")

oracles.on_frame(function()
  -- a white 8x8 box near the top-left, redrawn every frame
  oracles.draw_rect(8, 8, 8, 8, 0x7fff)
end)
```

Everything a mod does afterwards happens inside the handlers it registered here. If `main.lua` is
missing, throws an error, loops forever or runs out of its budget, the mod is switched off for the
session and the game carries on without it.

## How a mod runs

- `main.lua` runs **once per scan** of the mods folder, in a brand new Lua world with nothing carried
  over from the last one. The app scans when the launcher opens and again when a game starts, so
  expect your script to run more than once per launch. Set up there; don't compute anything heavy.
- The launcher's scan hasn't got a game yet and assumes Seasons, so a mod that names only `ages`
  doesn't load at all until Ages is actually played. Nothing is drawn from the launcher either way.
- Your `oracles.on_frame` handler runs **once per drawn frame while you are playing** — about 60
  times a second. Nothing runs in the launcher or while the pause menu is open.
- Your `oracles.on_room_change` handler runs after it, on frames where the room changed (including
  the very first frame).
- The drawing surface is **wiped at the start of every frame**. Whatever you want visible, draw it
  again this frame.
- What you draw goes on the picture *before* the screen filter, so it scales and filters along with
  the game instead of sitting on top looking different.
- Each mod gets its own private Lua world. Two mods cannot see each other's variables, and one mod
  failing does not affect another.

The surface is the size of the picture: **160x144**, or **256x144** with widescreen on, with `x` to
the right and `y` down from the top-left. Drawing outside it is clipped, not an error. Ask
`oracles.screen()` for the width rather than assuming.

Colours are **RGB555** integers, the format the renderer itself uses: five bits each of red, green
and blue, red lowest.

```lua
local function rgb(r, g, b) return r + g * 32 + b * 1024 end   -- each 0-31
local WHITE, RED, GREEN, BLUE = 0x7fff, rgb(31, 0, 0), rgb(0, 31, 0), rgb(0, 0, 31)
```

Every pixel you draw is opaque; there is no blending. Where you draw nothing, the game shows through.

## API reference

Everything a mod can call lives in the `oracles` table. This is the whole of it — twelve functions.
Anything not listed here does not exist (and a test fails the build if this list and the real one
ever disagree).

Arguments called *integer* must be whole numbers: `5` and `5.0` are both fine, `5.5` is **an error**
rather than a rounded 5, so round it yourself with `math.floor`. A number far too big for a 32-bit
integer is clamped to the end of that range rather than wrapping around. Anything that isn't a
number at all raises an error too, which switches your mod off unless you catch it with `pcall`.

### `oracles.draw_icon(item, x, y)`

Draws an item's own icon from your ROM, 16 pixels tall and 8 or 16 wide, with its top-left corner at
`x`, `y`. `item` is the game's item id (integer, only the low 8 bits are used). Returns `true` if
that item has an icon — including when you placed it off-screen — and `false` if it has none, or if
the ROM isn't available this frame. An item that comes in levels (the slingshot, for instance) is drawn at
the level you currently have.

```lua
oracles.draw_icon(0x13, 4, 120)     -- 0x13 is the slingshot
```

The icon is drawn in a grey ramp, not the game's own colours: the status bar draws these as sprites
with live palettes, which a mod has no access to.

### `oracles.draw_rect(x, y, w, h, colour)`

Fills a rectangle `w` by `h` pixels with `colour`. All five are integers. A width or height of zero
or less draws nothing. Returns nothing.

```lua
oracles.draw_rect(0, 0, oracles.screen(), 1, 0x7fff)   -- a white line across the top
```

### `oracles.draw_text(x, y, text, colour)`

Draws `text` in `colour`, with the top-left of the first character at `x`, `y`. Each character is 8
pixels wide and 16 tall, so text advances 8 pixels per character. Returns nothing.

```lua
oracles.draw_text(4, 4, "HELLO", 0x7fff)
```

The glyphs are the game's own font, read from your ROM, so stick to the characters the game itself
shows: capitals, digits and simple punctuation are the safe set. Bytes above 127 are skipped. A
string containing a zero byte stops there — only the part before it is drawn.

### `oracles.game()`

Returns `"ages"` or `"seasons"`, the game being played. Safe to call while `main.lua` is still
loading.

```lua
local in_seasons = oracles.game() == "seasons"
```

Worth calling even in a mod that only names one game in its `games=` line, because almost every
memory address differs between the two.

### `oracles.load(key)`

Reads back a value your mod saved with `oracles.store`, or `nil` if there is none. `key` is a string.
Numbers come back as the same kind of number they went in as — an integer stays an integer.

```lua
local best = oracles.load("best_time") or 0
```

A key that breaks the rules listed under `oracles.store`, or a `store.ini` the app cannot make sense
of, simply reads back `nil` — reading never raises an error, because a mod asking for a value back
isn't asking to be switched off over whatever is wrong with a file on disk.

### `oracles.log(text)`

Writes a line to the app's log. `text` is a string. Returns nothing.

```lua
oracles.log("essences: " .. count)
```

This is your `print` — `print` itself is not available (see
[What a mod can and cannot see](#what-a-mod-can-and-cannot-see)). The log is the app's standard
error: run the app from a terminal to watch it, or `adb logcat -s oracles` on Android. Bytes that
aren't printable text are shown as `\xNN` escapes rather than being sent to your terminal raw. Lines
over about 500 characters are cut, and logging on every single frame will flood the log — log when
something changes.

### `oracles.on_frame(f)`

Registers `f` to be called once per drawn frame while you are playing. `f` takes no arguments.
Returns nothing. Calling this again replaces the handler, so there is exactly one per mod.

```lua
oracles.on_frame(function() oracles.draw_rect(8, 8, 8, 8, 0x7fff) end)
```

### `oracles.on_room_change(f)`

Registers `f` to be called when the room changes, with the new room number as its one argument. It
also fires on the first frame after a game starts. Returns nothing, and replaces any previous
handler.

```lua
oracles.on_room_change(function(room) oracles.log("room " .. room) end)
```

### `oracles.rd(addr)`

Reads one byte of the game's memory and returns it as an integer 0-255. `addr` is an integer address.

```lua
local essences = oracles.rd(oracles.game() == "seasons" and 0xc6bb or 0xc6bf)
```

**Only two windows are readable:** `$c000`-`$cfff` (work RAM bank 0) and `$ff80`-`$fffe` (high RAM).
Anything else — including the switchable work RAM bank at `$d000`-`$dfff`, which this build does not
expose at all — **reads back 0** instead of raising an error. A plausible-looking `$d1xx` address
gives you a silent zero, not a complaint, so if a value is stubbornly 0 check the address is in range
before suspecting your arithmetic.

Addresses are the game's own, and nearly all of them differ between Ages and Seasons — so read them
through `oracles.game()`, as above. The community Oracle of Ages/Seasons disassembly is where the
names and addresses on this page come from.

### `oracles.screen()`

Returns the picture's width in pixels: 160, or 256 when the player has widescreen on. The height is
always 144. Safe to call while `main.lua` is still loading.

```lua
oracles.draw_rect(oracles.screen() - 8, 0, 8, 8, 0x7fff)   -- top-right corner, either width
```

### `oracles.setting(name, default)`

Registers one settings row for your mod — the menu that will show them is still to come, see the
caveats below — and returns a **function** that reads the row's current value. `name` is a short
string (up to 31 characters). There are two shapes:

```lua
local show = oracles.setting("Show pips", true)        -- an ON/OFF row; show() returns a boolean
local size = oracles.setting("Pip size", {1, 8, 5})    -- a number row {min, max, default}
                                                       -- size() returns an integer
```

Read it by calling the function, every time you need it — don't store the value in a local, or you
will miss the player changing it. Registering the same name twice from the same mod gives you back
the same row. Two different mods can use the same name without colliding. There are 32 rows in total
across all mods; asking for more raises an error.

Two honest caveats: **there is no settings menu for these yet** (it comes with the mod manager), so
today the function always returns your declared default; and a row's value is **not saved** — every
session starts at the default. Use `oracles.store` for anything that must survive a restart.

### `oracles.store(key, value)`

Saves `value` under `key` in your mod's own `store.ini`, next to its `main.lua`, and returns nothing.
It persists across sessions, and each mod has its own file that no other mod can read.

```lua
oracles.store("best_time", frames)
```

`value` may be a string, a number or a boolean — nothing else. The rules, which are enforced by
refusing rather than by quietly mangling your data:

- `key`: 1 to 63 characters, printable ASCII, no `=`, not starting with `#` or a space, not ending
  with a space.
- a string `value`: up to 503 characters, printable ASCII (`=` is fine). No newlines, tabs or zero
  bytes.

Break one of those and this raises a Lua error — which switches your mod off unless you wrap the call
in `pcall`. If you are storing something a player typed or a value from the game, check it first.

## Stable and unstable

The twelve functions above are the **stable core**, and the reason they can promise that is worth
understanding, because it explains why some obvious-looking additions are not there.

`oracles.rd` takes a **raw address**. Addresses are a property of the game — they are fixed by the
ROM you own, the same bytes in the same places as in 2001. Nothing this project does to its own code
can move `wEssencesObtained`. So a mod built on `oracles.rd` keeps working through any amount of
refactoring on our side: there is nothing of ours in the middle to break. The same goes for the
drawing functions (pixels on a 144-line picture), `oracles.game()`, `oracles.screen()`, the two
event handlers, and the store. They describe the game and the screen, not our implementation.

What is **not** here, and will arrive clearly marked unstable when it does:

- **Symbol hooks** — running your code when a named routine of ours runs. Those names are *our*
  function names, not the game's data. They move, split and get renamed as the port is worked on, so
  a mod built on them can break on any build. That is exactly why they are a later stage and will be
  marked unstable rather than folded in beside `oracles.rd`.
- **Writing** memory, or changing what the game does. Mods draw and read today.

`api=1` is the contract. If a future build ever has to change the meaning of something above, it will
do it under `api=2` and keep honouring `api=1`, rather than quietly changing what your mod does.

## What a mod can and cannot see

A mod gets the Lua language, three standard libraries, and the `oracles` table. The complete list of
globals inside a mod is:

```
_G  _VERSION  assert  collectgarbage  error  getmetatable  ipairs  math  next  oracles
pairs  pcall  rawequal  rawget  rawlen  rawset  select  setmetatable  string  table
tonumber  tostring  type  xpcall
```

So `string`, `table` and `math` are all there, and so is ordinary Lua: closures, metatables,
`pcall`, string patterns.

**Withheld**, and the first one will catch you out:

| Gone | Instead |
|---|---|
| `print`, `warn` | `oracles.log` |
| `io`, `os` | nothing — a mod cannot read or write files, start programs, or see the clock. Use `oracles.store` to save, and count frames rather than seconds. |
| `require`, `package` | nothing — a mod is one `main.lua`, with no second file to load |
| `load`, `loadfile`, `dofile`, `string.dump` | nothing — a mod cannot build and run new code at runtime |
| `coroutine` | nothing |
| `debug` | nothing |
| `utf8` | `string`, on plain bytes |

`setmetatable` is there but refuses a `__gc` field: a mod cannot install a finalizer.

Those names are gone for a reason in each case rather than as a blanket policy. `debug.sethook`
would let a mod remove the very hook that keeps it to its budget; `coroutine` offered a way to buy a
fresh budget forever; `load` and friends could be handed a crafted binary chunk; `io` and `os` reach
your filesystem and could block the game indefinitely or exit the app outright. `utf8` is simply not
one of the three libraries a mod is granted.

## Limits, and why

Your mod is given a bounded amount of work to do, so a buggy one slows nothing down for long:

| Limit | Value | When |
|---|---|---|
| Memory | **8 MiB** of Lua data | the whole session, main script and handlers together |
| Instructions | 50,000,000 | loading `main.lua` |
| Time | 2 seconds | loading `main.lua` |
| Instructions | 1,000,000 | each single handler call |
| Time | **4 milliseconds** | each single handler call |

A frame is 16.7 ms, which is where the 4 ms comes from: a handler that needs longer than a quarter of
a frame is not something a game can run 60 times a second. These are generous for what mods actually
do — a fresh Lua world costs about 19 KB, and a script building a thousand-entry table of tables
costs about 250 KB, against the 8 MiB cap; a typical handler finishes so far inside 4 ms that the
check never fires once.

Go past any of them and **your mod is switched off for the rest of the session**, with a reason, and
the game keeps running. It is not retried next frame: a handler given a fresh budget every frame
would be a permanently stuttering game rather than a disabled mod. The same happens if your code
raises an error it doesn't catch.

If you hit the memory cap, you are probably accumulating something per frame. If you hit the
per-frame limits, do the work once in `main.lua` or on `oracles.on_room_change`, and keep
`oracles.on_frame` to drawing.

## Limitations, honestly

Things worth knowing before you spend an evening on them.

**What is guaranteed.** A mod that loops forever, allocates without limit, writes a pathological
string pattern, installs a looping finalizer, ships a binary chunk instead of text, or throws an
error is refused or switched off, by itself, without affecting the game, the other mods, or the rest
of the scan. That is the large majority of what goes wrong, and all of it is tested.

**The one hang that is still possible.** A script that calls `pcall` on itself recursively — the
shape `local function guard() while true do pcall(guard) end end guard()` — can wedge the game. It
livelocks inside Lua's own error-recovery machinery, where no instruction ever finishes executing, so
neither the instruction budget nor the time limit is ever reached to cut it off. This is Lua's own
behaviour, not something this app does, and it takes deliberate construction rather than happening by
accident. It is left unfixed on purpose for now, because the alternative is machinery
(watchdog threads, abandoning Lua worlds mid-run) out of proportion to the risk. It is also the
concrete reason for the advice above: **install mods only from sources you trust.**

**`oracles.rd` sees two windows, not all of memory.** `$c000`-`$cfff` and `$ff80`-`$fffe`. The
switchable bank at `$d000`-`$dfff` is not exposed, and an address outside both windows reads 0
rather than raising an error. See `oracles.rd` above.

**Numbers are clamped, not wrapped.** Lua integers are 64-bit, the drawing functions take 32-bit
ones. Pass `4294967301` as a width and you get a very wide rectangle (clipped to the screen), not the
five-pixel one that wrapping the number around would have given you.

**Zero bytes in strings are handled, differently in two places.** `oracles.log` escapes a zero byte
as `\x00` and logs the whole string; `oracles.draw_text` stops at it, drawing only the part before.
`oracles.store` refuses a key or value containing one.

**Settings do not persist, and have no menu yet.** See `oracles.setting`.

**No mod manager.** No list, no enable/disable, no zip install. The only sign a player gets that
something went wrong is a **MOD DISABLED** message at the bottom of the screen the moment a mod is
switched off; the reason goes to the log.

## A worked example: an essence tracker

Eight pips across the top of the screen, one per essence, lit as you collect them — most of this
page, in about thirty lines.

The essences live in one byte, `wEssencesObtained`, with one bit per essence — and that byte is at a
**different address in each game**, which is what `oracles.game()` is for.

`mods/essence-tracker/mod.ini`:

```ini
id=essence-tracker
name=Essence Tracker
version=1.0
api=1
games=ages,seasons
```

`mods/essence-tracker/main.lua`:

```lua
-- wEssencesObtained: one byte, one bit per essence. Both addresses are inside
-- oracles.rd's $c000-$cfff window.
local ESSENCES = oracles.game() == "seasons" and 0xc6bb or 0xc6bf

-- RGB555: five bits each, red lowest.
local function rgb(r, g, b) return r + g * 32 + b * 1024 end
local LIT    = rgb(31, 27, 8)       -- warm yellow
local UNLIT  = rgb(5, 5, 7)         -- nearly black
local BORDER = rgb(0, 0, 0)

-- An ON/OFF row. There is no settings menu yet, so show() is your default for now -- written this
-- way so it starts working the day there is one.
local show = oracles.setting("Show essences", true)

local PIP, GAP = 5, 2               -- a pip is 5x5 with 2 pixels between

oracles.on_frame(function()
  if not show() then return end

  local bits = oracles.rd(ESSENCES)
  local total = 8 * PIP + 7 * GAP
  local x0 = (oracles.screen() - total) // 2        -- centred at either picture width
  local y0 = 2

  -- One dark plate behind the row, so pips stay readable over a bright room.
  oracles.draw_rect(x0 - 2, y0 - 2, total + 4, PIP + 4, BORDER)

  for i = 0, 7 do
    local have = ((bits >> i) & 1) == 1
    oracles.draw_rect(x0 + i * (PIP + GAP), y0, PIP, PIP, have and LIT or UNLIT)
  end
end)

-- The surface is wiped every frame, so the handler above draws the whole row from scratch each
-- time. That is the intended shape: cheap, no saved state, nothing accumulating.
oracles.log("essence-tracker ready")
```

Points worth taking from it:

- **Pick addresses by game, once, at load.** `oracles.game()` works while `main.lua` is still
  running, so resolve the address there rather than branching on every frame.
- **Ask for the width.** `oracles.screen()` is 160 or 256; centring with it means the mod is right in
  widescreen without a second code path.
- **Draw every frame.** The surface is wiped each frame; there is nothing to erase and nothing that
  stays.
- **Read settings through the function.** `show()`, never a cached `show` value.
- **Keep `oracles.on_frame` cheap.** One memory read and nine rectangles is nothing against the 4 ms
  limit.

To make it remember something — the most essences you have ever had, say — add this to the same
file:

```lua
local best = oracles.load("best") or 0

oracles.on_room_change(function()
  local n, bits = 0, oracles.rd(ESSENCES)
  for i = 0, 7 do n = n + ((bits >> i) & 1) end
  if n > best then
    best = n
    oracles.store("best", best)      -- written to this mod's own store.ini
  end
end)
```

On a room change rather than every frame, deliberately: `oracles.store` rewrites its whole file on
every call, which is not something to do sixty times a second.

## When something goes wrong

- **MOD DISABLED** at the bottom of the screen means a mod was switched off this session. The reason
  goes to the log.
- **Nothing happens at all.** The usual causes, in order: the folder name and `id=` don't match;
  `games=` doesn't name the game you're playing; the folder is not in the `mods` folder next to your
  saves; you didn't restart the game after adding it.
- **Reading the log.** Run the app from a terminal and watch its standard error; on Android,
  `adb logcat -s oracles`. Every refusal says which mod and why.
- **A value is always 0.** Check the address is inside `$c000`-`$cfff` or `$ff80`-`$fffe`, and that
  you used the right game's address.
