#pragma once
// The mod host. Mods are folders under the app's mods directory, each with a mod.ini and a
// main.lua. This file covers discovery (deciding which mod folders exist, are well-formed, and
// apply to the game being played) and now also runs each accepted mod's main.lua, under its own
// bounded Lua state: an instruction-count budget, a wall-clock deadline, and a memory cap per
// mod (see src/mods/sandbox.c), plus a handful of specific standard-library holes closed or
// wrapped there (mod_open_libs_hardened) because they would otherwise let a mod hang or crash
// the host regardless of those budgets -- so that a broken or hostile mod can be skipped, not
// let to take the host down. The Lua API surface a mod sees (the `oracles` table) lives in api.h/api.c, not
// here. What main.lua is permitted to do beyond that containment -- today it still gets io, os
// and require, which the approved sandbox design (string/table/math only) does not grant -- is
// still to be narrowed by a later task; nothing in the shipped app links this module yet either
// way. "Linked into the apps only, never into the engine libraries" is the eventual design
// constraint this module is built to hold, but today only test_mods links oraclesmods; no app
// target does yet. Both of those are stated here as intent, not as present fact.
#include <stdbool.h>
#include <stddef.h>

#define MODS_API_VERSION 1
#define MODS_MAX 32

// Scans `dir` for mod folders and decides which are enabled for `seasons` (true) or ages (false).
// Runs no mod code. Returns true once discovery has completed, including when it found zero mods:
// a `dir` that does not exist yet is zero mods (none installed), not a failure. Returns false if
// `dir` is NULL or empty, or if `dir` exists but could not be scanned for some other reason (e.g.
// permissions) -- silently reporting "no mods" in that case would hide a real problem from the
// caller instead of surfacing it.
bool mods_init(const char *dir, bool seasons);
void mods_shutdown(void);

int mods_count(void);
// Returns the mod at `i`'s canonical id (its folder name) or display name. For `i` outside
// [0, mods_count()), returns "" -- never NULL, and never a read past the end of the loaded-mods
// array. That range check matters beyond ordinary robustness: a later task will index these from
// Lua with an integer the mod itself chooses, so `i` must be treated as hostile input here, not
// just a programmer mistake.
const char *mods_id(int i);
const char *mods_name(int i);
// Returns whether the mod at `i` is enabled, or false for `i` outside [0, mods_count()) -- same
// out-of-range contract, and the same reason it matters, as mods_id/mods_name above.
bool mods_enabled(int i);

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
