#!/usr/bin/env python3
"""Rewrite the pointers tools/eu_operands.py found wrong for the European ROMs.

usage: tools/eu_fix_pointers.py AGES.tsv SEASONS.tsv [--apply]

Each listed `SET_xx(<expr>)` becomes `SET_xx(RV(<expr>, <EU value>))`: the USA build keeps its
expression (RV picks it when game_eu is 0), the EU build loads the operand its ROM has. The EU
value is a literal, as GV() already holds Seasons literals: the ROM it describes never changes.
A line that also runs under the other game takes GV() of the two EU values; one listed for only
one game keeps that game's expression for the other (RV(e, GV(0x..., e))), so it stays correct
there whatever the other game's EU ROM has. For `GV(a, s)` the arm of the listed game is wrapped.
Without --apply it prints the edits.
"""
import os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symfiles import seasons_code, ages_code


def split_gv(expr):
    """GV(a, s) -> (a, s), or None."""
    if not expr.startswith('GV(') or not expr.endswith(')'): return None
    depth, inner = 0, expr[3:-1]
    for i, ch in enumerate(inner):
        if ch == '(': depth += 1
        elif ch == ')': depth -= 1
        elif ch == ',' and depth == 0: return inner[:i].strip(), inner[i + 1:].strip()
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    apply = '--apply' in sys.argv
    flagged = defaultdict(dict)          # (path, line, expr) -> {game: eu value}
    for game, tsv in zip(('ages', 'seasons'), args):
        for l in open(tsv):
            path, no, fn, expr, us, c, rom = l.rstrip('\n').split('\t')
            flagged[(path, int(no), expr)][game] = int(rom, 16)
    from eu_operands import load_syms
    cols = {g: load_syms(g) for g in ('ages', 'seasons')}
    def base_at(src, no):
        for i in range(no - 1, -1, -1):
            m = re.search(r'\bBASE\((\w+)\)', src[i])
            if m: return m.group(1)
            if re.match(r'^(?:static )?(?:void|uint16_t|uint8_t|bool|int|unsigned) \*?\w+\(', src[i]): return None
        return None
    views = {}
    edits = defaultdict(list)
    for (path, no, expr), eu in sorted(flagged.items()):
        if path not in views:
            src = open(path).read().split('\n')
            views[path] = (src, ages_code(src), seasons_code(src))
        src, av, sv = views[path]
        base = base_at(src, no)
        # the games the line runs in: its own view keeps it, and its routine exists in that game
        runs = {g for g, v in (('ages', av), ('seasons', sv)) if v[no - 1].strip() and f'/{"seasons" if g == "ages" else "ages"}/' not in path
                and not (base and base in cols[g] and cols[g][base][0] == 0xffffffff)}
        gv = split_gv(expr)
        lit = lambda v: f'0x{v:04x}'
        if gv:
            a, s = gv
            if 'ages' in eu: a = f'RV({a}, {lit(eu["ages"])})'
            if 'seasons' in eu: s = f'RV({s}, {lit(eu["seasons"])})'
            new = f'GV({a}, {s})'
        elif runs <= set(eu):                       # every game it runs in is listed
            vals = {g: eu[g] for g in runs} or eu
            if len(vals) == 2 and vals['ages'] != vals['seasons']: new = f'RV({expr}, GV({lit(vals["ages"])}, {lit(vals["seasons"])}))'
            else: new = f'RV({expr}, {lit(next(iter(vals.values())))})'
        else:                                       # also runs where it was not listed: keep it there
            new = f'RV({expr}, GV({lit(eu["ages"]) if "ages" in eu else expr}, {lit(eu["seasons"]) if "seasons" in eu else expr}))'
        edits[path].append((no, expr, new))
    total = 0
    for path, es in sorted(edits.items()):
        src = views[path][0]
        for no, expr, new in es:
            line = src[no - 1]
            m = re.search(r'SET_(HL|BC|DE)\(' + re.escape(expr) + r'\);', line)
            if not m: print(f'{path}:{no}: cannot find SET_xx({expr})', file=sys.stderr); continue
            src[no - 1] = line[:m.start()] + f'SET_{m.group(1)}({new});' + line[m.end():]
            total += 1
            if not apply: print(f'{path}:{no}: {expr} -> {new}')
        if apply: open(path, 'w').write('\n'.join(src))
    print(f'{total} pointers {"rewritten" if apply else "to rewrite"} in {len(edits)} files')


if __name__ == '__main__':
    main()
