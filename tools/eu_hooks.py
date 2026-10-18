#!/usr/bin/env python3
"""Write src/hooks/generated_eu.txt / generated_seasons_eu.txt: the hooks that are safe to run on the
European ROM of a game, at their EU addresses.

usage: tools/eu_hooks.py ages|seasons US.gbc EU.gbc [--why]

The same test tools/seasons_hooks.py makes for Seasons against Ages, made here for a game's EU ROM
against its USA ROM (tools/eu_routines.py compare()): a hook is eligible when its routine is
instruction-identical in the EU ROM, every label its C burns is identical too, every SYM() it
names has an EU address, the bytes it burns past its routine's body sit at the same distance in
the EU ROM, the targets of its CALL_L are identical, and every C function it calls directly is
eligible (CALL_C and TAIL targets are checked against the hook table at run time). A routine
left out runs interpreted in the development app, and is a hole the native app stops at.
Then run tools/gen_hooks.py ref/eu/<game>_eu.sym src/hooks/generated[_seasons]_eu.txt
src/hooks/table[_seasons]_eu.h.
"""
import glob, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symfiles import rom_labels, seasons_code, ages_code, eu_address_map, sym_macros
from routine_equiv import Game
from eu_routines import compare
from eu_sym import Aligner, bank_pairs, rd

FUNC = re.compile(r'^(?:static )?(?:void|uint16_t|uint8_t|bool|int|unsigned) \*?(\w+)\([^)]*\)\s*\{')
CALL = re.compile(r'\b(\w+)\(gb[,)]')
EXEC = re.compile(r'\b(?:CYCT?[0-9a-f]*|burn_rom|RET|RET_TAKEN|RETI|I|push_effect|BASE)\(([^;]*)')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    why_out = '--why' in sys.argv
    game, us_rom, eu_rom = args
    us_sym, eu_sym = f'ref/oracles-disasm/{game}.sym', f'ref/eu/{game}_eu.sym'
    US, EU = Game(us_rom, us_sym), Game(eu_rom, eu_sym)
    us, eu = rd(us_rom), rd(eu_rom)
    al = Aligner(us, eu, bank_pairs(us, eu))
    emap = eu_address_map(us_sym, eu_sym)
    labels = rom_labels(us_sym)
    lists = ['src/hooks/generated.txt'] if game == 'ages' else ['src/hooks/generated_seasons.txt', 'src/hooks/generated_seasons_gen.txt']
    hook_lines = []
    for path in lists:
        for l in open(path):
            p = l.split()
            if len(p) >= 2 and not l.startswith('#'): hook_lines.append((int(p[0][:2], 16), int(p[0][3:], 16), p[1], p[2] if len(p) > 2 else '-'))
    hook_at = defaultdict(list)                 # hook function -> USA addresses it is entered at
    for b, a, fn, _ in hook_lines: hook_at[fn].append((b, a))

    memo = {}
    def verdict_at(b, a):
        if (b, a) in memo: return memo[(b, a)]
        e = emap.get((b, a))
        if e is None: v = 'UNPLACED'
        else:
            try: v = compare(US, EU, (b, a), e, al)[0]
            except Exception: v = 'ERROR'
        memo[(b, a)] = v
        return v

    def label_insts(lab):
        name = re.sub(r'_b[0-9a-f]{2}$', '', lab)
        for n in (lab, name, lab.replace('__', '@'), name.replace('__', '@')):
            if n in labels:
                insts = labels[n]
                m = re.search(r'_b([0-9a-f]{2})$', lab)
                if m and n != lab: insts = [x for x in insts if x[0] == int(m.group(1), 16)] or insts
                return n, insts
        return None, []

    def label_verdict(lab):
        n, insts = label_insts(lab)
        if not insts: return None
        vs = [verdict_at(b, a) for b, a in insts if a < 0x8000]
        return 'IDENTICAL' if all(v == 'IDENTICAL' for v in vs) else next(v for v in vs if v != 'IDENTICAL')

    def placed(lab):
        n, insts = label_insts(lab)
        return not insts or all(x in emap or x[1] >= 0x8000 for x in insts)

    # C call graph in this game's view of the sources
    view = seasons_code if game == 'seasons' else ages_code
    macros = sym_macros()
    macro_re = re.compile(r'\b(' + '|'.join(sorted(macros, key=len, reverse=True)) + r')\b') if macros else None
    callees, burns, syms, local_calls, raw_offs = {}, {}, {}, {}, {}
    for path in sorted(glob.glob('src/game/**/*.c', recursive=True)):
        if os.path.basename(path) in ('syms.c', 'ofs.c'): continue
        cur = None
        src = open(path, errors='replace').read().split('\n')
        for line, vcode in zip(src, view(src)):
            m = FUNC.match(line)
            if m:
                cur = (path, m.group(1))
                callees[cur], burns[cur], syms[cur], local_calls[cur], raw_offs[cur] = set(), set(), set(), set(), set()
            elif line.startswith('}'): cur = None; continue
            if cur is None: continue
            code = vcode[m.end():] if m else vcode
            if 'AGES_ONLY()' in code and game == 'seasons': callees[cur] = None; cur = None; continue
            oneliner = bool(m) and code.count('}') > code.count('{')
            for c in CALL.findall(code): callees[cur].add(c)
            syms[cur].update(re.findall(r'\bSYM(?:BANK)?\((\w+)\)', code))
            if macro_re:          # header names for SYM() (ROM_b07_fileManagementFunction)
                for mm in macro_re.findall(code): syms[cur].update(re.findall(r'\bSYM\((\w+)\)', macros[mm]))
            syms[cur].update(re.findall(r'\bBASE\((\w+)\)', code))
            for off, lab, loff in re.findall(r'\bCALL_L(?:_CC)?\((?:b_\+(?:O\()?(\d+)\)?|\(SYM\((\w+)\) \+ (\d+)\)), \w+,', code):
                local_calls[cur].add((lab or None, int(off or loff)))
            for a_ in EXEC.findall(code):
                for lab in re.findall(r'\bSYM\((\w+)\)', a_): burns[cur].add(lab)
                nm = re.match(r'\s*b_\+(?:O\()?(\d+)\b', a_)
                if nm: raw_offs[cur].add(int(nm.group(1)))
                bm = re.match(r'\s*(\w+)\)', a_)
                if bm and 'BASE(' in code: burns[cur].add(bm.group(1))
            if oneliner: cur = None
    callees = {k: v for k, v in callees.items() if v is not None}
    by_name = defaultdict(list)
    for (path, fn) in callees: by_name[fn].append(path)

    def call_target_ok(base, off):
        n, insts = label_insts(base)
        for b, a in insts:
            e = emap.get((b, a))
            if e is None: return False
            if US.rd(b, a + off) not in (0xcd, 0xc4, 0xcc, 0xd4, 0xdc): return False
            t = US.rd(b, a + off + 1) | US.rd(b, a + off + 2) << 8
            if verdict_at(0 if t < 0x4000 else b, t) != 'IDENTICAL': return False
        return bool(insts)

    eligible, why = {}, {}
    for key in callees:
        fn = key[1]
        ok = True
        if fn in hook_at:
            for b, a in hook_at[fn]:
                v = verdict_at(b, a)
                if v != 'IDENTICAL': ok = False; why[key] = f'routine {v}'; break
        for lab in syms[key]:
            if not placed(lab): ok = False; why.setdefault(key, f'SYM({lab}) has no EU address'); break
        for lab in burns[key]:
            v = label_verdict(lab)
            if v is not None and v != 'IDENTICAL': ok = False; why.setdefault(key, f'burns {lab} ({v})'); break
        base_labels = [l for l in burns[key] if label_insts(l)[1]]
        if ok and len(base_labels) == 1 and raw_offs[key]:
            n_, insts = label_insts(base_labels[0])
            for b, a in insts:
                if a >= 0x8000: continue
                try: body = set(x[0] for x in US.body(b, a))
                except Exception: body = set()
                for off in sorted(raw_offs[key]):
                    if a + off in body or a + off >= 0x8000: continue
                    t = a + off
                    tb = b if t >= 0x4000 else 0
                    while t > a and (tb, t) not in US.by_addr: t -= 1
                    cover = US.by_addr.get((tb, t))
                    if not cover or (tb, t) == (b, a): continue
                    eb, ec = emap.get((b, a)), emap.get((tb, t))
                    if eb is None or ec is None or ec[1] - eb[1] != t - a or verdict_at(tb, t) != 'IDENTICAL':
                        ok = False; why.setdefault(key, f'burns {base_labels[0]}+{off} in {cover}, not the same in EU'); break
                if not ok: break
        for lab, off in local_calls[key]:
            base = lab or (base_labels[0] if len(base_labels) == 1 else None)
            if base is None or not call_target_ok(base, off): ok = False; why.setdefault(key, f'CALL_L target at {base}+{off} differs')
        eligible[key] = ok
    changed = True
    while changed:
        changed = False
        for key, cs in callees.items():
            if not eligible[key]: continue
            for c in cs:
                if (key[0], c) in callees: ok = eligible[(key[0], c)]
                elif c.endswith('_hook') or c.startswith('s_'):
                    ok = any(eligible.get((p, c), False) for p in by_name.get(c, [])) if by_name.get(c) else True
                else: ok = True        # engine helpers (mem_rd, alu_*, push_effect...) are region-neutral
                if not ok: eligible[key] = False; changed = True; why[key] = f'calls {c}'; break

    rows, skipped = [], defaultdict(int)
    for b, a, fn, flags in hook_lines:
        if not any(eligible.get((p, fn), False) for p in by_name.get(fn, [])):
            skipped['not eligible'] += 1
            if why_out:
                r = [why[(p, fn)] for p in by_name.get(fn, []) if (p, fn) in why]
                print(f'{b:02x}:{a:04x} {fn}: {r[0] if r else "no C function"}')
            continue
        e = emap.get((b, a))
        if e is None: skipped['no EU address'] += 1; continue
        rows.append((e[0], e[1], fn, flags))
    rows.sort()
    out = 'src/hooks/generated_eu.txt' if game == 'ages' else 'src/hooks/generated_seasons_eu.txt'
    with open(out, 'w') as f:
        for b, a, fn, fl in rows: f.write(f'{b:02x}:{a:04x} {fn} {fl}\n')
    print(f'{out}: {len(rows)} of {len(hook_lines)} hooks eligible for the EU ROM; skipped: {dict(skipped)}')


if __name__ == '__main__':
    main()
