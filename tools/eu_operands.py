#!/usr/bin/env python3
"""Check every 16-bit pointer the C builds from SYM() against the operand the European ROM has.

usage: tools/eu_operands.py ages|seasons EU.gbc [--tsv OUT]

tools/symbolize.py writes a ROM pointer as `SYM(label) + offset` under the nearest label, which
for the USA ROM equals the literal by construction. In the EU ROM the label and the pointed-to
bytes can move by different amounts (a pointer into another bank's data, written relative to a
label of the routine's own bank), and the C then loads the wrong address although the routine's
instructions are identical. For each `CYC(b_+X, b_+Y); SET_HL|SET_BC|SET_DE(<expr>);` in a
function whose BASE routine is instruction-identical in the EU ROM (tools/eu_routines.py), this
evaluates <expr> with the EU symbols and compares it with the operand of the `ld rr,nn` at the EU
address of b_+X; the USA operand at b_+X must equal the USA value (else the statement is skipped). Prints the
mismatches (file:line, routine, expression, C value, ROM value); --tsv also writes them with the
USA value of the expression, for tools/eu_fix_pointers.py.
"""
import os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symfiles import rom_labels, seasons_code, ages_code

FUNC = re.compile(r'^(?:static )?(?:void|uint16_t|uint8_t|bool|int|unsigned) \*?(\w+)\([^)]*\)\s*\{')
STMT = re.compile(r'CYCT?(?:15)?\(b_\+(O\()?(\d+)\)?,\s*[^;]*\);\s*SET_(HL|BC|DE)\((.*?)\);')
LD16 = {'HL': 0x21, 'BC': 0x01, 'DE': 0x11}


def load_syms(game):
    """label -> value for the USA and EU columns of src/game/syms.c, by symbol id."""
    ids = [l.split()[1] for l in open('src/hooks/syms_used.txt')]
    sids = [l.split()[0] for l in open('src/hooks/syms_used.txt')]
    src = open('src/game/syms.c').read()
    arr = {m.group(1): [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', m.group(2))]
           for m in re.finditer(r'static const uint32_t syms_(\w+)\[SYM_COUNT\] = \{(.*?)\};', src, re.S)}
    us, eu = arr[game], arr[game + '_eu']
    return {sid: (us[i], eu[i]) for i, sid in enumerate(sids)}


def load_ofs():
    """Seasons offsets of the routines whose shared C maps Ages offsets (src/game/ofs.c)."""
    src = open('src/game/ofs.c').read()
    tabs = {m.group(1): [int(x, 0) for x in m.group(2).split(',')] for m in re.finditer(r'static const uint16_t ofs_(\w+)\[\d+\] = \{([^}]*)\}', src)}
    by_sid = {m.group(1): m.group(2) for m in re.finditer(r'case S_(\w+): return ofs_(\w+);', src)}
    return {sid: tabs[t] for sid, t in by_sid.items() if t in tabs}


def main():
    args = sys.argv[1:]
    tsv = None
    if '--tsv' in args: i = args.index('--tsv'); tsv = args[i + 1]; del args[i:i + 2]
    game, eu_path = args
    eu = open(eu_path, 'rb').read()
    syms = load_syms(game)
    ofs = load_ofs() if game == 'seasons' else {}
    us_rom = open(f'roms/Legend of Zelda, The - Oracle of {"Ages" if game == "ages" else "Seasons"} (USA, Australia).gbc', 'rb').read()
    from routine_equiv import Game
    from eu_routines import compare
    from eu_sym import Aligner, bank_pairs
    from symfiles import eu_address_map
    US = Game(f'roms/Legend of Zelda, The - Oracle of {"Ages" if game == "ages" else "Seasons"} (USA, Australia).gbc', f'ref/oracles-disasm/{game}.sym')
    EU = Game(eu_path, f'ref/eu/{game}_eu.sym')
    al = Aligner(us_rom, eu, bank_pairs(us_rom, eu))
    verdicts = {}
    def identical(base):
        v = syms[base]
        if v[0] == 0xffffffff or v[1] == 0xffffffff: return False
        ua, ea = (v[0] >> 16, v[0] & 0xffff), (v[1] >> 16, v[1] & 0xffff)
        if ua not in verdicts:
            try: verdicts[ua] = compare(US, EU, ua, ea, al)[0] in ('IDENTICAL', 'CONST')
            except Exception: verdicts[ua] = False
        return verdicts[ua]
    def rd_us(bank, addr):
        o = addr if addr < 0x4000 else bank * 0x4000 + addr - 0x4000
        return us_rom[o] if o < len(us_rom) else 0

    def rd(bank, addr):
        o = addr if addr < 0x4000 else bank * 0x4000 + addr - 0x4000
        return eu[o] if o < len(eu) else 0

    from symfiles import sym_macros
    macros = sym_macros()
    def value(expr, col):
        e = expr
        for _ in range(3):        # header names for SYM() expressions
            e = re.sub(r'\b(' + '|'.join(sorted(macros, key=len, reverse=True)) + r')\b', lambda m: '(' + macros[m.group(1)] + ')', e) if macros else e
        # RV(us, eu): the region's arm (tools/eu_fix_pointers.py writes these)
        while 'RV(' in e:
            i = e.index('RV(') + 3
            depth, j, comma = 1, i, None
            while depth:
                if e[j] == '(': depth += 1
                elif e[j] == ')': depth -= 1
                elif e[j] == ',' and depth == 1 and comma is None: comma = j
                j += 1
            e = e[:i - 3] + '(' + (e[i:comma] if col == 0 else e[comma + 1:j - 1]) + ')' + e[j:]
        e = re.sub(r'\bGV\(((?:[^(),]|\([^()]*(?:\([^()]*\))?[^()]*\))*),\s*((?:[^(),]|\([^()]*(?:\([^()]*\))?[^()]*\))*)\)', lambda m: m.group(2) if game == 'seasons' else m.group(1), e)
        def sym(m):
            v = syms.get(m.group(1))
            if v is None: raise KeyError(m.group(1))
            v = v[col]
            if v == 0xffffffff: raise KeyError(m.group(1) + ' (poisoned)')
            return str(v & 0xffff)
        e = re.sub(r'\bSYM\((\w+)\)', sym, e)
        if not re.fullmatch(r'[\s()+\-0-9a-fx]*', e): raise ValueError(expr)
        return eval(e) & 0xffff

    view = seasons_code if game == 'seasons' else ages_code
    macro_names = list(sym_macros())
    bad, checked, skipped = [], 0, defaultdict(int)
    import glob
    for path in sorted(glob.glob('src/game/**/*.c', recursive=True)):
        if os.path.basename(path) in ('syms.c', 'ofs.c'): continue
        if f'/{"ages" if game == "seasons" else "seasons"}/' in path: continue     # the other game's own code
        src = open(path, errors='replace').read().split('\n')
        fn, base = None, None
        for no, (line, code) in enumerate(zip(src, view(src)), 1):
            m = FUNC.match(line)
            if m: fn, base = m.group(1), None
            elif line.startswith('}'): fn = None; continue
            if fn is None: continue
            bm = re.search(r'\bBASE\((\w+)\)', code)
            if bm: base = bm.group(1)
            for sm in STMT.finditer(code):
                has_o, off, reg, expr = sm.group(1), int(sm.group(2)), sm.group(3), sm.group(4)
                if 'SYM(' not in expr and not any(re.search(r'\b' + k + r'\b', expr) for k in macro_names): continue
                if base is None or base not in syms: skipped['no BASE'] += 1; continue
                eb_full, ub_full = syms[base][1], syms[base][0]
                if eb_full == 0xffffffff or ub_full == 0xffffffff: skipped['BASE not placed'] += 1; continue
                if not identical(base): skipped['BASE routine differs in EU'] += 1; continue
                if has_o and game == 'seasons':
                    tab = ofs.get(base)
                    if not tab or off >= tab[0] or tab[off + 1] == 0xffff: skipped['O() unmapped'] += 1; continue
                    off = tab[off + 1]
                eb, ea = eb_full >> 16, (eb_full & 0xffff) + off
                ub, ua = ub_full >> 16, (ub_full & 0xffff) + off
                if rd(eb, ea) != LD16[reg] or rd_us(ub, ua) != LD16[reg]: skipped['not an ld rr,nn there'] += 1; continue
                rom = rd(eb, ea + 1) | rd(eb, ea + 2) << 8
                try: c, cu = value(expr, 1), value(expr, 0)
                except (KeyError, ValueError) as ex: skipped[f'unevaluated'] += 1; continue
                if cu != (rd_us(ub, ua + 1) | rd_us(ub, ua + 2) << 8): skipped['USA value is not the USA operand'] += 1; continue
                checked += 1
                if c != rom: bad.append((path, no, fn, expr, c, rom, value(expr, 0)))
    for path, no, fn, expr, c, rom, us in bad:
        print(f'{path}:{no}: {fn}: {expr} = ${c:04x}, EU ROM has ${rom:04x}')
    if tsv:
        with open(tsv, 'w') as f:
            for path, no, fn, expr, c, rom, us in bad: f.write(f'{path}\t{no}\t{fn}\t{expr}\t{us:04x}\t{c:04x}\t{rom:04x}\n')
    print(f'{game}: {checked} pointers checked, {len(bad)} wrong in EU; not checked: {dict(skipped)}')
    if bad: print('fix them with tools/eu_fix_pointers.py (--tsv OUT, then the TSV)', file=sys.stderr)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
