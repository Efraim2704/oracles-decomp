#!/usr/bin/env python3
"""Generate C for the code a European ROM does not share with its USA release.

usage: tools/eu_gen.py ages|seasons EU.gbc [--why]

tools/eu_hooks.py puts in the EU hook table the routines the EU build left unchanged; everything
else the EU ROM runs (the routines it changed, the new ones such as the language screen, and the
unchanged ones whose C calls changed ones directly) is transliterated here from the EU ROM with
the EU symbols, cycle-exact (tools/transliterate.py), into src/game/eu/gen_<game>.c. Starting
from the USA hooks the EU table leaves out, every call, jump, fall-through, jump-table entry and
thread resume point of generated code that has no entry yet gets one, to a fixed point, so the
native build always finds C where the game goes. Entries the symbol file has no name for are
named here (src/hooks/labels[_seasons]_eu.sym: eu__<bank>_<addr>, <routine>__afterCall<addr>).
Writes the entries to src/hooks/generated[_seasons]_eu_gen.txt; then
  tools/gen_hooks.py ref/eu/<game>_eu.sym src/hooks/generated[_seasons]_eu.txt+src/hooks/generated[_seasons]_eu_gen.txt src/hooks/table[_seasons]_eu.h
"""
import contextlib, io, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    why = '--why' in sys.argv
    game, eu_rom = args
    seasons = game == 'seasons'
    sym = f'ref/eu/{game}_eu.sym'
    labels_path = f'src/hooks/labels{"_seasons" if seasons else ""}_eu.sym'
    open(labels_path, 'w').write('# entry points tools/eu_gen.py names in European code (regenerated)\n')
    sys.argv = ['transliterate.py', eu_rom, sym, 'src/hooks/ported.txt'] + (['--game=seasons'] if seasons else [])
    with contextlib.redirect_stdout(io.StringIO()):
        import transliterate as T
    prefix = 'eus_' if seasons else 'eua_'
    T.cname = lambda n: prefix + n.replace('@', '__').replace('.', '_')

    # the shared hooks the EU table already runs, at their EU addresses
    table = 'src/hooks/generated_seasons_eu.txt' if seasons else 'src/hooks/generated_eu.txt'
    T.entries.clear()
    shared = {}
    for l in open(table):
        p = l.split()
        if len(p) >= 2: shared[(int(p[0][:2], 16), int(p[0][3:], 16))] = p[1]
    T.entries.update(shared)

    # roots: the USA hooks the EU table leaves out, at their EU addresses
    from symfiles import eu_address_map
    emap = eu_address_map(f'ref/oracles-disasm/{game}.sym', sym)
    lists = ['src/hooks/generated_seasons.txt', 'src/hooks/generated_seasons_gen.txt'] if seasons else ['src/hooks/generated.txt']
    roots, unplaced = [], []
    resume_names = {}
    us_rom = open(f'roms/Legend of Zelda, The - Oracle of {"Seasons" if seasons else "Ages"} (USA, Australia).gbc', 'rb').read()
    eu_bytes = open(eu_rom, 'rb').read()
    def byte(rom, b, a):
        o = a if a < 0x4000 else b * 0x4000 + a - 0x4000
        return rom[o] if o < len(rom) else None
    mapped_by_bank = {}
    for (ub, ua) in emap: mapped_by_bank.setdefault(ub, []).append(ua)
    for v in mapped_by_bank.values(): v.sort()
    import bisect
    def resume_point(ub, ua):
        """A thread's resume point (after a call, or after `ld sp,hl`) that the symbol files do not
        carry over: the same distance from the start of its routine, if the EU code has the same
        call or `ld sp,hl` just before it."""
        starts = mapped_by_bank.get(ub, [])
        i = bisect.bisect_right(starts, ua) - 1
        if i < 0 or ua - starts[i] > 0x400: return None
        eb, ea0 = emap[(ub, starts[i])]
        ea = ea0 + (ua - starts[i])
        for back, ops in ((3, (0xcd, 0xc4, 0xcc, 0xd4, 0xdc)), (1, (0xf9,))):
            if byte(us_rom, ub, ua - back) in ops and byte(eu_bytes, eb, ea - back) == byte(us_rom, ub, ua - back):
                return (eb, ea)
        return None
    for path in lists:
        for l in open(path):
            p = l.split()
            if len(p) < 2 or l.startswith('#'): continue
            ub, ua = int(p[0][:2], 16), int(p[0][3:], 16)
            e = emap.get((ub, ua)) if ua < 0x8000 else None
            if ua >= 0x8000: continue
            if e is None and re.search(r'__after(Call|Sp)[0-9a-f]{4}', p[1]):
                e = resume_point(ub, ua)
                if e:       # the USA name, with the EU address after it
                    n = re.sub(r'^s_', '', p[1][:-len('_hook')] if p[1].endswith('_hook') else p[1])
                    resume_names[e] = re.sub(r'(__after(?:Call|Sp))[0-9a-f]{4}$', lambda m: f'{m.group(1)}{e[1]:04x}', n)
            if e is None: unplaced.append(p[1]); continue
            if e not in shared: roots.append(e)

    named = []          # (bank, addr, name) written to the labels file

    # roots: the jump-table entries of the shared routines. Their C reaches calls and tail jumps
    # directly, but a table it does not handle itself dispatches through the hook table, and the EU
    # table can name code the USA build never had.
    shared_targets = set()
    for (b, a) in shared:
        if a >= 0x8000: continue
        body = T.routine_body(b, a)
        addrs = {ia for ia, *_ in body}
        for ia, m, ln, cy, kind, info in body:
            if kind != 'jumptable': continue
            for t in info[0]:
                if t >= 0x8000 or t in addrs: continue
                tb = 0 if t < 0x4000 else b
                if tb and (tb, t) not in shared: shared_targets.add((tb, t))
                elif not tb and (0, t) not in shared: shared_targets.add((0, t))
    roots.extend(sorted(shared_targets))

    def ensure_label(b, a, name=None):
        if (b, a) in T.by_addr or (b, a) in T.local_by_addr: return
        n = name or f'eu__{b:02x}_{a:04x}'
        T.by_addr[(b, a)] = n
        T.labels.setdefault(n, (b, a))
        T.instances.setdefault(n, [(b, a)])
        T.extra_names.add(n)
        named.append((b, a, n))
        T._bodies.clear()
        T._switches.clear()

    def resume_points(b, a):
        """Where a thread comes back after a call that yields, or after `ld sp,hl`: an entry each."""
        body = T.routine_body(b, a)
        banks_at = T.infer_banks(b, body)
        name = (T.by_addr.get((b, a)) or T.local_by_addr[(b, a)]).replace('@', '__')
        out = []
        for ia, m, ln, cy, kind, info in body:
            if kind == 'spload': out.append((b, ia + ln, f'{name}__afterSp{ia + ln:04x}'))
            if kind in ('call', 'callcc'):
                tb = T.target_bank(b, info[0])
                if tb is None: tb = banks_at.get(ia)
                if tb is not None and T.switches_threads(tb, info[0]): out.append((b, ia + ln, f'{name}__afterCall{ia + ln:04x}'))
        return out


    generated, unknown_bank = {}, set()
    todo = list(dict.fromkeys(roots))
    rounds = 0
    while todo:
        rounds += 1
        for (b, a) in todo:
            if (b, a) in shared: continue
            ensure_label(b, a, resume_names.get((b, a)))
            generated[(b, a)] = None
        todo = []
        # walk every generated entry with the labels as they now stand (a new label splits a body)
        for (b, a) in list(generated):
            T.entries[(b, a)] = T.cname_at(b, a)
        for (b, a) in list(generated):
            for rb, ra, rn in resume_points(b, a):
                if (rb, ra) not in T.entries and (rb, ra) not in generated:
                    ensure_label(rb, ra, rn); todo.append((rb, ra))
            name = T.by_addr.get((b, a)) or T.local_by_addr[(b, a)]
            with contextlib.redirect_stderr(io.StringIO()):
                code = T.gen(name, b, a)
            if code is None: continue
            for tb, t in T.gen.targets:
                if t >= 0x8000: continue
                if t < 0x4000: tb = 0          # a jump-table entry into bank 0 code comes tagged with the table's bank
                if tb is None: unknown_bank.add((b, a, t)); continue
                if (tb, t) not in T.entries and (tb, t) not in generated: todo.append((tb, t))
        todo = list(dict.fromkeys(todo))

    # final emission, labels fixed; an entry whose body cannot be generated (an instruction the
    # transliterator does not support: data reached as code) is dropped, and whatever named it
    # dispatches there instead, so the whole emission runs again without it
    for (b, a) in generated: T.entries[(b, a)] = T.cname_at(b, a)
    skipped = []
    while True:
        bodies, rows, failed = [], [], []
        for (b, a) in sorted(generated):
            name = T.by_addr.get((b, a)) or T.local_by_addr[(b, a)]
            with contextlib.redirect_stderr(io.StringIO()):
                code = T.gen(name, b, a)
            if code is None: failed.append((b, a, name)); continue
            # a local label nothing jumps to (the transliterator marks every named one): drop it
            used = set(re.findall(r'goto (L_[0-9a-f]{4});', code))
            code = re.sub(r'^(L_[0-9a-f]{4}):\n', lambda m: m.group(0) if m.group(1) in used else '', code, flags=re.M)
            bodies.append(code)
            rows.append((b, a, T.cname_at(b, a), T.gen.flags))
        if not failed: break
        for b, a, name in failed:
            skipped.append(name); del generated[(b, a)]; T.entries.pop((b, a), None)
    os.makedirs('src/game/eu', exist_ok=True)
    hdr = f'src/game/eu/gen_{game}.h'
    with open(hdr, 'w') as h:
        h.write('// generated by tools/eu_gen.py; do not edit\n#pragma once\n#include "core/gb.h"\n')
        for b, a, cn, fl in rows: h.write(f'void {cn}(GB *gb);\n')
    with open(f'src/game/eu/gen_{game}.c', 'w') as f:
        f.write('// generated by tools/eu_gen.py from the European ROM; do not edit\n')
        f.write(f'// The code {"Oracle of Seasons" if seasons else "Oracle of Ages"} (Europe) does not share with the USA release, transliterated\n')
        f.write('// cycle-exact (tools/transliterate.py). Only the European hook table names these functions.\n')
        f.write('#include "game/asm.h"\n#include "game/gen.h"\n#include "game/seasons/gen.h"\n')
        f.write(f'#include "game/eu/gen_{game}.h"\n\n')
        f.write('\n\n'.join(bodies) + '\n')
    with open(f'src/hooks/generated{"_seasons" if seasons else ""}_eu_gen.txt', 'w') as g:
        for b, a, cn, fl in rows: g.write(f'{b:02x}:{a:04x} {cn} {fl}\n')
    with open(labels_path, 'a') as f:
        for b, a, n in sorted(named): f.write(f'{b:02x}:{a:04x} {n}\n')
    print(f'{game}: {len(rows)} routines generated from {len(set(roots))} roots in {rounds} rounds; '
          f'{len(named)} entries named; {len(unplaced)} USA hooks without an EU address; '
          f'{len(unknown_bank)} jumps to a banked address from bank 0 with no known bank; {len(skipped)} skipped')
    if why:
        for n in unplaced: print('  no EU address:', n)
        for b, a, t in sorted(unknown_bank): print(f'  bank unknown: {b:02x}:{a:04x} -> {t:04x}')
        for n in skipped: print('  skipped (unsupported instruction):', n)


if __name__ == '__main__':
    main()
