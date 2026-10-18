#!/usr/bin/env python3
"""Code map: which ROM bytes are instructions the C has replaced, per game, as C range tables.

usage: tools/codemap.py AGES.gbc ages.sym SEASONS.gbc seasons.sym [AGES_EU.gbc SEASONS_EU.gbc] [--out src/assets/codemap.c]

With the European ROMs too, their maps come from ref/eu/<game>_eu.sym and the EU hook lists
(tools/eu_hooks.py, tools/eu_gen.py); without them the EU tables are written empty (nothing is zeroed). The
keep_code.txt offset ranges of a game are kept in its EU ROM both at the same offset (a read
through a pointer the data holds) and where the symbol files carry the code around them.

A byte is code when it belongs to an instruction of a hooked routine (every entry of
src/hooks/generated.txt or generated_seasons.txt, followed with transliterate.py's routine_body:
jumps inside the routine, jump-table targets, never calls). Jump-table entries (.dw after
rst_jumpTable) are data and stay. Bytes the game reads as data although they are instructions
(code copied to RAM: the `RAM = ROM` relocations in src/hooks/extra.sym, sized by the RAM
label's _sizeof_) and the ranges listed in src/assets/keep_code.txt are left out. The native
runner zeroes the mapped bytes after building the cycle table; with the map installed on the
GB, bus_read aborts on a data read of a mapped byte, which is how new keep_code entries are found.
"""
import contextlib, io, itertools, os, re, sys

def load_game(rom_path, sym_path, hooks_path):
    # transliterate in the game's mode (extra labels, RAM relocations) so routine bodies match
    sys.argv = ['transliterate.py', rom_path, sym_path, 'src/hooks/ported.txt'] + (['--game=seasons'] if 'seasons' in sym_path else [])
    for m in list(sys.modules):
        if m == 'transliterate': del sys.modules[m]
    with contextlib.redirect_stdout(io.StringIO()):
        import transliterate as T
    sizes = {}
    for line in open(sym_path):
        m = re.match(r'([0-9a-f]{8}) _sizeof_(\S+)$', line.strip())
        if m: sizes[m.group(2)] = int(m.group(1), 16)
    code = set()
    entries = 0
    for line in itertools.chain.from_iterable(open(h) for h in hooks_path.split('+') if os.path.exists(h)):
        m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) ', line)
        if not m: continue
        b, a = int(m.group(1), 16), int(m.group(2), 16)
        if a >= 0x8000: continue
        entries += 1
        for ia, mn, ln, cy, kind, info in T.routine_body(b, a):
            if ia >= 0x8000: continue
            off = ia if ia < 0x4000 else b * 0x4000 + (ia - 0x4000)
            for i in range(ln): code.add(off + i)
    keep = set()
    eu = os.path.basename(sym_path).endswith('_eu.sym')
    for line in open(('src/hooks/extra_seasons' if 'seasons' in sym_path else 'src/hooks/extra') + ('_eu' if eu else '') + '.sym'):
        m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) (\S+) = ([0-9a-f]{2}):([0-9a-f]{4})', line.strip())
        if not m: continue
        b, a = int(m.group(4), 16), int(m.group(5), 16)
        n = sizes.get(m.group(3)) or sizes.get(T.by_addr.get((b, a), ''), 0)
        off = a if a < 0x4000 else b * 0x4000 + (a - 0x4000)
        for i in range(n): keep.add(off + i)
    game = os.path.basename(sym_path).split('.')[0].replace('_eu', '')
    def off_of(b, a): return a if a < 0x4000 else b * 0x4000 + (a - 0x4000)
    us_sym = os.path.join('ref', 'oracles-disasm', game + '.sym')
    to_eu = None
    if eu:          # USA ROM offsets (keep_code.txt) -> EU offsets, through the label that covers them
        from symfiles import eu_address_map, rom_labels
        emap = eu_address_map(us_sym, sym_path)
        starts = sorted(emap)
        import bisect
        def to_eu(o):
            b, a = (0, o) if o < 0x4000 else (o // 0x4000, 0x4000 + o % 0x4000)
            i = bisect.bisect_right(starts, (b, a)) - 1
            if i < 0 or starts[i][0] != b: return None
            eb, ea = emap[starts[i]]
            return off_of(eb, ea + (a - starts[i][1]))
    if os.path.exists('src/assets/keep_code.txt'):
        for line in open('src/assets/keep_code.txt'):
            line = line.split('#')[0].strip()
            if not line: continue
            kind, arg = line.split()
            if kind == 'label':
                if arg not in T.labels: print(f'warning: keep_code label {arg} unknown in {game}', file=sys.stderr); continue
                for (b, a) in T.instances.get(arg, [T.labels[arg]]): keep.update(range(off_of(b, a), off_of(b, a) + sizes.get(arg, 0)))
            elif kind in ('both', game):
                lo, hi = (int(x, 16) for x in arg.split('-'))
                if to_eu:
                    # a range is read either through a pointer the data holds (the same offset in
                    # the EU ROM, whatever code sits there now) or relative to the code around it
                    # (a table overrun: where that code moved); keep both
                    keep.update(range(lo, hi))
                    lo2 = to_eu(lo)
                    if lo2 is None: print(f'warning: keep_code {arg} has no EU address', file=sys.stderr); continue
                    lo, hi = lo2, lo2 + (hi - lo)
                keep.update(range(lo, hi))
    # scriptCmd_loadScript copies 256 bytes from every loadscript target into wBigBuffer, so the
    # instruction bytes that follow a short script land in RAM (and in the state hash): keep them.
    scripts = os.path.join(os.path.dirname(os.path.realpath(us_sym)), 'scripts', game, 'scripts.s')
    nscripts = 0
    for line in open(scripts, errors='replace'):
        m = re.match(r'\s*loadscript\s+(\S+)', line)
        if not m: continue
        arg = m.group(1).split(',')[0]
        name = arg.split('.')[-1]
        insts = T.instances.get(name, [T.labels[name]] if name in T.labels else [])
        if arg.startswith('scriptHelp.'): insts = [i for i in insts if i[0] == 0x15] or insts
        if not insts: print(f'warning: loadscript target {arg} unknown in {game}', file=sys.stderr); continue
        b, a = insts[0]
        keep.update(range(off_of(b, a), off_of(b, a) + 256))
        nscripts += 1
    print(f'{game}: {nscripts} loadscript targets', file=sys.stderr)
    code -= keep
    ranges = []
    for off in sorted(code):
        if ranges and ranges[-1][0] + ranges[-1][1] == off: ranges[-1][1] += 1
        else: ranges.append([off, 1])
    return ranges, entries, len(code), len(keep & set(range(len(T.rom))))

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = 'src/assets/codemap.c'
    if '--out' in sys.argv: out = sys.argv[sys.argv.index('--out') + 1]; args = [a for a in args if a != out]
    ages, seasons = args[0:2], args[2:4]
    games = [('ages', ages, 'src/hooks/generated.txt'), ('seasons', seasons, 'src/hooks/generated_seasons.txt+src/hooks/generated_seasons_gen.txt')]
    if len(args) >= 6:
        games += [('ages_eu', (args[4], 'ref/eu/ages_eu.sym'), 'src/hooks/generated_eu.txt+src/hooks/generated_eu_gen.txt'),
                  ('seasons_eu', (args[5], 'ref/eu/seasons_eu.sym'), 'src/hooks/generated_seasons_eu.txt+src/hooks/generated_seasons_eu_gen.txt')]
    parts = ['// generated by tools/codemap.py; do not edit\n#include "assets/assets.h"\n']
    for name, (rom, sym), hooks in games:
        ranges, entries, nbytes, nkeep = load_game(rom, sym, hooks)
        print(f'{name}: {entries} entries, {nbytes} code bytes in {len(ranges)} ranges, {nkeep} instruction bytes kept as data', file=sys.stderr)
        parts.append(f'const CodeRange codemap_{name}[] = {{\n' + ''.join(f'  {{0x{o:06x}, {n}}},\n' for o, n in ranges) + '};\n' + f'const size_t codemap_{name}_n = {len(ranges)};\n')
    for name in ('ages_eu', 'seasons_eu'):
        if name not in [g[0] for g in games]:
            parts.append(f'const CodeRange codemap_{name}[] = {{{{0, 0}}}};\nconst size_t codemap_{name}_n = 0;\n')
    open(out, 'w').write('\n'.join(parts))

main()
