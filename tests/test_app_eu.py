#!/usr/bin/env python3
"""The app with a game's USA and European ROMs (headless, SDL dummy drivers): each ROM gets its own
folder in the cache, the one added last is the one the game's tab starts (active.txt), adding the
other again switches back, and each keeps its own saves. Booting the European ROM into a file stops
at its language screen for the player, then goes on to the file once a language is picked.
usage: test_app_eu.py ORACLES_NATIVE USA_ROM EU_ROM [SHOT_DIR]   (exit 77 when a ROM is missing)"""
import os, subprocess, sys, tempfile

app, us_rom, eu_rom = sys.argv[1:4]
shots = sys.argv[4] if len(sys.argv) > 4 else None
if not os.path.exists(us_rom) or not os.path.exists(eu_rom): print('skip: ROM not present'); sys.exit(77)
env = dict(os.environ, SDL_VIDEO_DRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
game = 'ages' if 'Ages' in os.path.basename(us_rom) else 'seasons'


def run(*args, **extra):
    r = subprocess.run([app, *args], env=dict(env, **extra), capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr
    return r


def active(cache):
    p = os.path.join(cache, 'active.txt')
    return dict(l.split() for l in open(p) if l.strip()) if os.path.exists(p) else {}


with tempfile.TemporaryDirectory() as cache:
    run(us_rom, '--cache', cache, '--frames', '1')
    assert os.path.exists(os.path.join(cache, game, 'rom.bin'))
    assert active(cache).get(game) == game, active(cache)
    # a save of the USA release, to see that it stays where it is
    open(os.path.join(cache, game, 'sram.sav'), 'wb').write(b'\x55' * 0x2000)

    run(eu_rom, '--cache', cache, '--frames', '1')
    eu = game + '-eu'
    assert os.path.exists(os.path.join(cache, eu, 'rom.bin'))
    manifest = open(os.path.join(cache, eu, 'manifest.txt')).read()
    assert f'game {game}\n' in manifest and 'region eu\n' in manifest, manifest
    assert active(cache).get(game) == eu, active(cache)

    # --game starts the active ROM, the European one, and --file 1 boots into file 1: the driver
    # fast-forwards to the language screen the game opens the first time and leaves it to the
    # player (A takes the highlighted one), then goes on to the file. The key is held for 60 loop
    # ticks, counted from the language screen, so the game sees it whatever the frame pacing.
    keys = [f'language+{100 + t}:X' for t in range(60)]
    shot_args = {}
    if shots:
        os.makedirs(shots, exist_ok=True)
        shot_args['ORACLES_SHOT'] = f'2000:{shots}/{game}_eu_language.png,6000:{shots}/{game}_eu_files.png'
    r = run('--game', game, '--file', '1', '--cache', cache, '--frames', '1500', ORACLES_TEST_KEYS=','.join(keys), **shot_args)
    assert 'no code at' not in r.stderr, r.stderr
    assert 'boot into file 1 done' in r.stderr, f'the boot driver did not get past the language screen; stderr: {r.stderr}'
    assert open(os.path.join(cache, game, 'sram.sav'), 'rb').read() == b'\x55' * 0x2000, 'the USA save changed'

    run(us_rom, '--cache', cache, '--frames', '1')            # adding the USA ROM again switches back
    assert active(cache).get(game) == game, active(cache)
    assert os.path.exists(os.path.join(cache, eu, 'rom.bin'))  # the European one stays installed
print(f'ok app_eu {game}')
