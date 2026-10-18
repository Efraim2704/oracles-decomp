#!/usr/bin/env python3
"""Which hooked routines the European ROM changed: the USA body against the EU body, instruction by
instruction, through the two symbol files.

usage: tools/eu_routines.py ages|seasons US.gbc EU.gbc [--tsv OUT] [--show NAME]

Reads ref/oracles-disasm/<game>.sym and ref/eu/<game>_eu.sym (tools/eu_sym.py). Each routine of
src/hooks/generated*.txt is normalized the way tools/routine_equiv.py does it (ROM targets as
label+offset, RAM as names), so a routine the EU build only moved compares IDENTICAL; see compare()
for the other verdicts. UNPLACED: eu_sym.py could not place the entry.
"""
import collections, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from routine_equiv import Game

ORDER = ('IDENTICAL', 'TARGET_UNPLACED', 'CONST', 'POINTER', 'RAM', 'DIFFERENT')


def operand(g, bank, start, addrs, k, v, hint=None):
    if k == 'imm': return ('imm', v)
    if k == 'ram' and v < 0x8000: return ('mbc', v)        # ld ($2222),a and friends: a register
    if v >= 0x8000: return ('ram', g.ram_sym(v))
    if v in addrs: return ('rel', v - start)
    return ('rom', g.rom_sym(bank, v, hint))


def compare(US, EU, ua, ea, al=None):
    """(verdict, note) for one routine: the worst operand difference over the instruction stream.
    CONST: an immediate differs (mostly the bank of data the EU build moved). POINTER: a ROM
    operand names another label. TARGET_UNPLACED: only operands whose USA target eu_sym.py could
    not place. RAM: a RAM operand differs. DIFFERENT: the instructions themselves differ."""
    ub, uaddr = ua
    eb, eaddr = ea
    ui, ei = US.body(ub, uaddr), EU.body(eb, eaddr)
    if [(x[1], x[3]) for x in ui] != [(x[1], x[3]) for x in ei]:
        return 'DIFFERENT', f'{len(ui)} vs {len(ei)} insns'
    uaddrs, eaddrs = {x[0] for x in ui}, {x[0] for x in ei}
    worst, notes = 0, []
    for x, y in zip(ui, ei):
        for (k, v), (k2, v2) in zip(x[4], y[4]):
            a, b = operand(US, ub, uaddr, uaddrs, k, v), operand(EU, eb, eaddr, eaddrs, k2, v2)
            if a == b: continue
            if k == 'any' and v == v2 and a[0] == 'rom' and (a[1].startswith('$') or '+' in a[1]): continue   # a constant
            if al and a[0] == 'rom' and b[0] == 'rom':
                # the two symbol files cut the bytes differently, but the alignment puts the USA
                # target where the EU operand points: the same data or code, moved
                tbanks = [0] if v < 0x4000 else [ub] + [x for x in range(len(US.rom) // 0x4000) if x != ub and (x, v) in US.by_addr]
                if any((lambda p: p and p[1] == v2)(al.place(tb, v)) for tb in tbanks): continue
            if a[0] == 'imm': w = 2
            elif a[0] == 'ram' or b[0] == 'ram': w = 4
            elif a[0] == 'rom' and b[0] == 'rom' and b[1].startswith('$') and not a[1].startswith('$'): w = 1
            else: w = 3
            worst = max(worst, w)
            if len(notes) < 3: notes.append(f'{x[0] - uaddr:+d} {x[3] % tuple("_" * x[3].count("%s"))}: {a[1]} -> {b[1]}')
    return ORDER[worst], '; '.join(notes)


def main():
    args = sys.argv[1:]
    tsv = show = None
    if '--tsv' in args: i = args.index('--tsv'); tsv = args[i + 1]; del args[i:i + 2]
    if '--show' in args: i = args.index('--show'); show = args[i + 1]; del args[i:i + 2]
    game, us_rom, eu_rom = args
    from eu_sym import Aligner, bank_pairs, rd
    us, eu = rd(us_rom), rd(eu_rom)
    al = Aligner(us, eu, bank_pairs(us, eu))
    US = Game(us_rom, f'ref/oracles-disasm/{game}.sym')
    EU = Game(eu_rom, f'ref/eu/{game}_eu.sym')
    lists = ['src/hooks/generated.txt'] if game == 'ages' else ['src/hooks/generated_seasons.txt', 'src/hooks/generated_seasons_gen.txt']
    # USA address -> EU address, through any label the two files share at that address
    eu_at = {}
    for n, insts in EU.instances.items():
        us_insts = sorted(US.instances.get(n, []))
        if len(insts) == 1:
            for x in us_insts: eu_at.setdefault(x, insts[0])
        elif len(insts) == len(us_insts):                  # copies in several code banks keep their order
            for x, y in zip(us_insts, sorted(insts)): eu_at.setdefault(x, y)
    for n, (eb, ea) in EU.labels.items():
        if n in US.labels: eu_at.setdefault(US.labels[n], (eb, ea))
    res, rows = collections.Counter(), []
    seen = set()
    for path in lists:
        for line in open(path):
            if line.startswith('#') or not line.strip(): continue
            ba, hook = line.split()[:2]
            b, a = int(ba[:2], 16), int(ba[3:], 16)
            name = hook[:-5] if hook.endswith('_hook') else hook
            if (b, a) in seen: continue
            seen.add((b, a))
            e = eu_at.get((b, a))
            if e is None:
                res['UNPLACED'] += 1; rows.append((name, ba, '-', 'UNPLACED', '')); continue
            try:
                v, note = compare(US, EU, (b, a), e, al)
            except Exception as ex:
                v, note = 'ERROR', str(ex)
            res[v] += 1
            rows.append((name, ba, f'{e[0]:02x}:{e[1]:04x}', v, note))
            if show and name == show:
                un, en = US.normalized(b, a)[0], EU.normalized(*e)[0]
                for x, y in zip(un + [None] * (len(en) - len(un)), en + [None] * (len(un) - len(en))):
                    print(f'{"=" if x == y else "!"} {x[1] if x else "":44s} {y[1] if y else ""}')
    total = sum(res.values())
    print(f'{game}: {total} hooked routines')
    for k in ORDER + ('UNPLACED', 'ERROR'):
        if res[k]: print(f'  {k:11s} {res[k]:5d}  ({100.0 * res[k] / total:5.1f}%)')
    if tsv:
        with open(tsv, 'w') as f:
            for r in sorted(rows, key=lambda r: (r[3], r[1])): f.write('\t'.join(r) + '\n')


if __name__ == '__main__':
    main()
