#pragma once
// The mod host. Mods are folders under the app's mods directory, each with a mod.ini and a
// main.lua. The host is linked into the apps only, never into the engine libraries, so the headless
// runners and the TAS replays cannot load one.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODS_API_VERSION 1
#define MODS_MAX 32

// Scans `dir` for mods and runs the enabled ones' main.lua. Returns false only if `dir` is
// unusable; an empty or missing directory is success with no mods.
bool mods_init(const char *dir, bool seasons);
void mods_shutdown(void);

int mods_count(void);
const char *mods_id(int i);
const char *mods_name(int i);
bool mods_enabled(int i);

// True iff `id` is non-empty and every byte is a lowercase letter, digit, or hyphen. A mod's id
// doubles as a filesystem path component (its folder name, and later its store file's name), so
// this is the one gate standing between an attacker-controlled mod.ini/folder name and a path
// traversal: no byte this rejects can ever spell ".." or "/". Shared with the folder-name check
// in mods_init, and meant to be reused by Task 9's store path and any later zip installer, rather
// than reinventing a charset check at each new call site.
bool mods_id_valid(const char *id);
