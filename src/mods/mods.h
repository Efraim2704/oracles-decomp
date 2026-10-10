#pragma once
// The mod host. Mods are folders under the app's mods directory, each with a mod.ini and a
// main.lua. This file covers discovery (deciding which mod folders exist, are well-formed, and
// apply to the game being played) and now also runs each accepted mod's main.lua, under its own
// bounded Lua state: an instruction-count budget, a wall-clock deadline, and a memory cap per
// mod (see src/mods/sandbox.c), plus a handful of specific standard-library holes closed or
// wrapped there (mod_open_libs_hardened) because they would otherwise let a mod hang or crash
// the host regardless of those budgets -- so that a broken or hostile mod can be contained, not
// let to take the host down. A mod whose main.lua fails is disabled, not removed: see mods_init's
// own comment below for exactly where the line between "not a mod for this game" (skipped,
// occupies no slot) and "a real mod whose payload failed" (listed, mods_enabled() false) sits.
// The Lua API surface a mod sees (the `oracles` table) lives in api.h/api.c, not
// here. What main.lua is permitted to do beyond that containment is the spec's sandbox (the
// standard string, table and math libraries, plus oracles): io, os, require/package,
// load/loadfile/dofile, string.dump, print/warn and utf8 are all withheld (see
// mod_open_libs_hardened in sandbox.c for exactly which and why); nothing in the shipped app
// links this module yet either way. "Linked into the apps only, never into the engine libraries"
// is the eventual design constraint this module is built to hold, but today only test_mods links
// oraclesmods; no app target does yet. That is stated here as intent, not as present fact.
#include <stdbool.h>
#include <stddef.h>

#define MODS_API_VERSION 1
// The hard cap on how many mods occupy a slot in mods[] at once -- counting a disabled mod
// exactly the same as an enabled one, since a disabled mod still has an id, a name and a reason
// to show on a status line; only a mod never accepted for this game at all (wrong game, bad
// mod.ini, bad folder name) consumes no slot and so cannot be what pushes a later mod over this
// cap. See loading_stops_at_mods_max in tests/test_mods.c for the 33rd-mod boundary this guards.
#define MODS_MAX 32

// Scans `dir` for mod folders and decides which apply to `seasons` (true) or ages (false), then
// runs each applicable one's main.lua under sandbox.c's containment. Returns true once discovery
// (and, for every mod found applicable, its load attempt) has completed, including when it found
// zero mods: a `dir` that does not exist yet is zero mods (none installed), not a failure.
// Returns false if `dir` is NULL or empty, or if `dir` exists but could not be scanned for some
// other reason (e.g. permissions) -- silently reporting "no mods" in that case would hide a real
// problem from the caller instead of surfacing it.
//
// A mod whose own main.lua fails -- a Lua error, an exhausted instruction budget or wall-clock
// deadline, an out-of-memory allocation, a rejected __gc metatable or binary chunk, a missing or
// unreadable main.lua, or a declared `api=` this build is too old to support -- is *not* a
// mods_init failure and does not make the mod disappear: it is kept in the list (mods_count()
// counts it, mods_id()/mods_name() still describe it), with mods_enabled() false and mods_error()
// giving the reason, so a status line can tell the player their mod is there but broken rather
// than making it look like the install silently failed. The distinction mods_init itself draws is
// "is this folder a mod for this game at all" (a missing/unparseable mod.ini, a folder name
// outside the accepted charset, a stray non-directory entry, a mod.ini that names a different
// game) -- those are not mods for this game and are skipped outright, occupying no slot and
// reported only on stderr -- versus "this is a real, accepted mod for this game whose own payload
// did not run", which is what mods_enabled()/mods_error() exist to describe.
bool mods_init(const char *dir, bool seasons);
void mods_shutdown(void);

// The number of mods accepted for the current game, counting both the ones running normally and
// the ones listed disabled by mods_enabled()/mods_error() below -- a mod is dropped from this
// count only when it was never accepted as a mod for this game at all (wrong game, malformed
// mod.ini, bad folder name, MODS_MAX overflow), never because its own main.lua failed.
int mods_count(void);
// Returns the mod at `i`'s canonical id (its folder name) or display name. For `i` outside
// [0, mods_count()), returns "" -- never NULL, and never a read past the end of the loaded-mods
// array. That range check matters beyond ordinary robustness: a later task will index these from
// Lua with an integer the mod itself chooses, so `i` must be treated as hostile input here, not
// just a programmer mistake. Both are populated identically whether the mod ends up enabled or
// disabled -- id/name describe the package, not whether it runs.
const char *mods_id(int i);
const char *mods_name(int i);
// Returns whether the mod at `i` loaded and ran successfully, or false for `i` outside
// [0, mods_count()) -- same out-of-range contract, and the same reason it matters, as
// mods_id/mods_name above. False for an in-range `i` means the mod is accepted and listed but
// disabled for this session (see mods_error() for why): it has no live Lua state, consumes none
// of its 8 MiB allocator budget, and a per-frame caller (Task 8) must skip it.
bool mods_enabled(int i);
// Returns why the mod at `i` is disabled, or "" if it is enabled, or `i` is outside
// [0, mods_count()) -- never NULL, matching mods_id/mods_name's convention of never returning
// NULL for a valid-looking call. Attacker-controlled (a Lua error message is the mod author's own
// text) and so already passed through mods_escape_into before being stored: no raw control byte
// or invalid-UTF-8 sequence can reach whatever prints this, directly or on a future status line.
// Deliberately short (see MOD_ERROR_LEN in mods.c) -- this is for a one-line display, not a full
// diagnostic; the unabridged detail always also goes to stderr at the moment the mod is disabled,
// which is where a developer debugging a specific mod should look.
const char *mods_error(int i);

// The most recent oracles.log() message, or NULL if no mod has logged anything since the last
// mods_init/mods_shutdown. Implemented in api.c (that's where the Lua binding and its storage
// live); declared here because this is the mod host's public surface -- tests and a future status
// line both read it through mods.h, not api.h, the same way they read mods_name()/mods_id().
// Cleared by mods_shutdown (and so, transitively, by the mods_init that always calls it first) so
// a silent mod loaded after a chatty one never inherits its predecessor's stale log line.
//
// oracles.log('') is indistinguishable from no call at all: both leave last_log empty and this
// returns NULL either way. That's an accepted limitation, not a bug -- "logged nothing" and
// "logged the empty string" are the same thing to a one-line status display, which is the only
// consumer this exists for today -- but it does mean this function cannot be used to detect
// *that a mod called oracles.log at all* if the string it passed happened to be empty.
const char *mods_last_log(void);

// Copies src (exactly src_len bytes, which may contain embedded NULs -- it is not assumed to be a
// NUL-terminated C string) into dst (bounded by dst_size, always NUL-terminated), replacing every
// byte outside printable ASCII (0x20..0x7e) with a \xHH escape. A value under this module's
// control reaches a terminal three ways -- this process's own stderr diagnostics, whatever
// mods_name()/mods_id() hand back to a future caller (a mod list UI, say), and now oracles.log()
// and a failed main.lua's own error text, both straight from untrusted Lua -- so sanitizing once,
// at the point a value is stored or about to be printed, keeps all of them safe rather than
// re-escaping at every print site. Shared between mods.c (mod.ini values) and api.c (oracles.log
// and the "mod failed to load" message) rather than each keeping its own copy. Without this, a
// value like "victim\r\x1b[2K\rmods: 'victim' loaded OK (api 1)" overwrites the current terminal
// line with a forged success message the moment it's printed raw.
void mods_escape_into(char *dst, size_t dst_size, const char *src, size_t src_len);

// True iff `id` is non-empty and every byte is a lowercase letter, digit, or hyphen. A mod's id
// doubles as a filesystem path component (its folder name, and later its store file's name), so
// this is the gate against NAME-based path traversal: no byte this rejects can ever spell ".."
// or "/" or "\". It is not a complete defence against every path hazard a mod folder could pose:
// a mod folder that is itself a symlink pointing outside the mods directory is a separate
// concern, left to Task 9's zip installer and the per-mod store writer to close, not something
// this check already covers. Shared with the folder-name check in mods_init, and meant to be
// reused by those later call sites rather than reinventing a charset check at each one.
bool mods_id_valid(const char *id);
