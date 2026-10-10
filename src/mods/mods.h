#pragma once
// The mod host. Mods are folders under the app's mods directory, each with a mod.ini. This file
// covers discovery only: deciding which mod folders exist, are well-formed, and apply to the game
// being played. Running a mod's main.lua is a later task's job -- mods_init does not look at
// main.lua at all yet. Likewise, "linked into the apps only, never into the engine libraries" is
// the eventual design constraint this module is built to hold, but today only test_mods links
// oraclesmods; no app target does yet. Both of those are stated here as intent, not as present fact.
#include <stdbool.h>

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
// array. That range check matters beyond ordinary robustness: Task 5 will index these from Lua
// with an integer the mod itself chooses, so `i` must be treated as hostile input here, not just
// a programmer mistake.
const char *mods_id(int i);
const char *mods_name(int i);
// Returns whether the mod at `i` is enabled, or false for `i` outside [0, mods_count()) -- same
// out-of-range contract, and the same reason it matters, as mods_id/mods_name above.
bool mods_enabled(int i);

// True iff `id` is non-empty and every byte is a lowercase letter, digit, or hyphen. A mod's id
// doubles as a filesystem path component (its folder name, and later its store file's name), so
// this is the gate against NAME-based path traversal: no byte this rejects can ever spell ".."
// or "/" or "\". It is not a complete defence against every path hazard a mod folder could pose:
// a mod folder that is itself a symlink pointing outside the mods directory is a separate
// concern, left to Task 9's zip installer and the per-mod store writer to close, not something
// this check already covers. Shared with the folder-name check in mods_init, and meant to be
// reused by those later call sites rather than reinventing a charset check at each one.
bool mods_id_valid(const char *id);
