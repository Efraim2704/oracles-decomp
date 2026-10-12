#!/usr/bin/env python3
"""Pins app.c's rd_room through the real frame path, which nothing else does: app.c is not linked
into test_mods, and the TAS replays that exercise the same WRAM address (test_wide.c, test_room.c)
never call through app.c at all. Installs a mod whose on_room_change logs the room number, drives
the real app (SDL dummy drivers, ORACLES_TEST_KEYS) through the start of the known-good Seasons
movie far enough to walk out of the starting room, and asserts the handler actually fired with a
changed, non-starting room -- matching the rooms test_room.c's record_cb already validates for the
same address (0xc4c, i.e. $cc4c) from the engine side.

The movie's inputs are frame-exact, but ORACLES_TEST_KEYS ticks are not frames: in normal play each
tick is paced to real wall-clock time against the (dummy) audio clock, so tick:frame drifts all over
the place early on. Holding the FAST action for the whole run sidesteps that pacing (fast mode runs
a fixed 4 real GB frames per tick, unconditionally -- see run_game's `fast` branch in app.c) and
makes tick:frame a known, constant 4:1, so every 4 consecutive movie frames are OR'd into one tick's
held buttons. That is coarser than the original frame-exact movie, but every button in the opening
sequence is held for several frames at a time, so the OR'd version still drives the same menus,
naming screen and short walk, just not frame-perfect.

usage: test_mods_room.py ORACLES_NATIVE ROM TAS_INPUTS   (exit 77 when either is missing)"""
import os, subprocess, struct, sys, tempfile

app, rom, tas_path = sys.argv[1], sys.argv[2], sys.argv[3]
if not os.path.exists(rom) or not os.path.exists(tas_path):
    print('skip: ROM or TAS inputs not present')
    sys.exit(77)

with open(tas_path, 'rb') as f:
    data = f.read()
if data[:4] != b'ORIN':
    print('skip: unrecognised TAS inputs format')
    sys.exit(77)
count = struct.unpack('<I', data[4:8])[0]
inputs = data[8:8 + count]

# Enough of the movie to leave the starting room at least once (confirmed empirically: the first
# room change away from the pre-game "room 0" lands around movie frame 1881 of 265064).
NFRAMES = min(2200, count)
GROUP = 4  # matches fast mode's 4-real-frames-per-tick

# GB joypad bit -> SDL key name, via this app's default bindings (src/ui/controls.c): A=X, B=Z,
# Right/Left/Up/Down = arrow keys, Start=Return. Select is never used by the opening sequence.
NAMES = ['X', 'Z', None, 'Return', 'Right', 'Left', 'Up', 'Down']

entries = []
nticks = (NFRAMES + GROUP - 1) // GROUP
for t in range(1, nticks + 2):
    entries.append(f'{t}:Tab')               # hold FAST the whole time
for tk in range(nticks):
    agg = 0
    for f in range(tk * GROUP, min((tk + 1) * GROUP, NFRAMES)):
        agg |= inputs[f]
    tick = tk + 1
    for k in range(8):
        if (agg >> k) & 1 and NAMES[k]:
            entries.append(f'{tick}:{NAMES[k]}')
keys = ','.join(entries)

env = dict(os.environ, SDL_VIDEO_DRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
with tempfile.TemporaryDirectory() as cache:
    r = subprocess.run([app, rom, '--cache', cache, '--frames', '1'], env=env, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr

    moddir = os.path.join(cache, 'mods', 'roomwatch')
    os.makedirs(moddir, exist_ok=True)
    with open(os.path.join(moddir, 'mod.ini'), 'w') as f:
        f.write('id=roomwatch\nname=Roomwatch\napi=1\ngames=ages,seasons\n')
    with open(os.path.join(moddir, 'main.lua'), 'w') as f:
        f.write("oracles.on_room_change(function(r) oracles.log('roomwatch room=' .. r) end)\n")

    max_frames = NFRAMES + 40
    r = subprocess.run([app, '--game', 'seasons', '--cache', cache, '--frames', str(max_frames)],
                        env=dict(env, ORACLES_TEST_KEYS=keys), capture_output=True, text=True, timeout=90)
    assert r.returncode == 0, r.stderr
    rooms = [int(l.rsplit('=', 1)[1]) for l in r.stderr.splitlines() if 'roomwatch room=' in l]
    assert len(rooms) >= 2, f'on_room_change fired {len(rooms)} time(s), expected at least 2; stderr:\n{r.stderr}'
    assert rooms[0] == 0, f'first room should be the pre-game default (0), got {rooms[0]}'
    assert any(x != 0 for x in rooms[1:]), f'never left room 0: {rooms}'
print('ok mods_room')
