#!/usr/bin/env python3
"""Symbol file for a European ROM, carried over from the USA one by aligning the two ROMs.

usage: tools/eu_sym.py US.gbc us.sym EU.gbc OUT.sym [--report OUT.tsv] [--extra SRC DST]...

The disassembly only builds the USA ROMs, so there is no .sym for the European cartridges
(2 MB, five languages). Most of their code is the USA code, at the same address or shifted by
what the EU build inserted before it, and the data moved to other banks. This tool:

  1. pairs every USA bank with the EU bank(s) holding its content (24-byte chunks that occur once
     in the EU ROM vote for the bank they land in);
  2. aligns each USA bank with its EU bank(s) (difflib matching blocks), and runs the paired banks
     do not hold with wherever the rest of the EU ROM has them (Ages' link-cable code moved
     from bank $16 to $17);
  3. places each ROM label: inside a matching block it moves with the block ("block"); otherwise
     the label's first bytes are searched near where the surrounding blocks put it ("search");
  4. reads every aligned instruction's ROM operands on both sides: a label the references agree on
     is placed or corrected there ("ref") -- the entry of a routine the EU build changed, a table
     that moved; what is still left is written as a comment and listed in the report as unplaced;
  5. places what is left between two placed labels the EU build kept the same distance apart
     (tables whose pointers moved: other bytes, same size);
  6. copies RAM labels and [definitions] unchanged (the WRAM/HRAM layout is the same in every
     region: wActiveLanguage at $c62a is already reserved in the USA map).

The output is a WLA-style .sym the decomp's tools read like the disassembly's own. Labels in code
the EU build changed come out unplaced or misplaced; tools/eu_routines.py checks every hooked
routine instruction by instruction and lists the ones that need EU code.
"""
import bisect, collections, difflib, re, sys

BANK = 0x4000
MIN_BLOCK = 6        # a shorter matching block counts only next to a long one with the same shift


def rd(path):
    return open(path, 'rb').read()


def off_of(bank, addr):
    return addr if addr < BANK else bank * BANK + addr - BANK


def bank_pairs(us, eu, k=24, step=8):
    """USA bank -> [EU banks] that hold most of its unique 24-byte chunks (in EU bank order)."""
    seen = {}
    for o in range(len(eu) - k):
        c = eu[o:o + k]
        seen[c] = -1 if c in seen else o
    pairs = {}
    for b in range(len(us) // BANK):
        votes, total = collections.Counter(), 0
        for o in range(b * BANK, (b + 1) * BANK - k, step):
            c = us[o:o + k]
            if len(set(c)) < 3: continue            # fill and runs say nothing about placement
            total += 1
            e = seen.get(c)
            if e is not None and e >= 0: votes[e // BANK] += 1
        same = sum(x == y for x, y in zip(us[b * BANK:(b + 1) * BANK], eu[b * BANK:(b + 1) * BANK]))
        if same > BANK * 0.95:                       # identical bank (audio): keep it in place
            pairs[b] = [b]; continue
        top = [eb for eb, n in votes.most_common(3) if total and n >= 0.15 * total]
        if not top and votes: top = [votes.most_common(1)[0][0]]
        if b in (0,) or (b < 0x16 and b in [eb for eb, _ in votes.most_common(2)]):
            top = [b]                                # code banks keep their number
        pairs[b] = sorted(top) if top else [b]
    return pairs


class Aligner:
    """US bank -> list of (us_off_in_bank, eu_bank, eu_addr, length) matching blocks."""

    def __init__(self, us, eu, pairs):
        self.us, self.eu, self.pairs = us, eu, pairs
        self.blocks = {}

    def bank(self, b):
        if b in self.blocks: return self.blocks[b]
        a = self.us[b * BANK:(b + 1) * BANK]
        ebanks = self.pairs[b]
        e = b''.join(self.eu[x * BANK:(x + 1) * BANK] for x in ebanks)
        sm = difflib.SequenceMatcher(None, a, e, autojunk=False)
        out = []
        for i, j, n in sm.get_matching_blocks():
            if not n: continue
            # split blocks that straddle two EU banks
            while n:
                eb = ebanks[j // BANK]
                room = BANK - j % BANK
                m = min(n, room)
                out.append((i, eb, j % BANK, m))
                i, j, n = i + m, j + m, n - m
        # a short block is trusted only when it moves by the same delta as a long block next to it:
        # code keeps its bytes between operands that point at moved things, so a routine that only
        # shifted aligns as many short blocks with one delta, while a coincidence has its own
        def delta(x): return (x[1] * BANK + x[2]) - x[0]
        longs = [k for k, x in enumerate(out) if x[3] >= MIN_BLOCK]
        keep = []
        for k, x in enumerate(out):
            if x[3] >= MIN_BLOCK: keep.append(x); continue
            p = bisect.bisect_left(longs, k)
            near = [out[longs[q]] for q in (p - 1, p) if 0 <= q < len(longs)]
            if any(delta(y) == delta(x) and abs(y[0] - x[0]) < 0x100 for y in near): keep.append(x)
        keep = self.relocated_runs(b, a, keep)
        self.blocks[b] = keep
        return keep

    def relocated_runs(self, b, a, blocks, gap_min=64, k=16):
        """Runs of the USA bank the paired EU banks do not hold (the EU build moved them to another
        bank: Ages' link-cable code went from $16 to $17) are looked up in the whole EU ROM by their
        unique 16-byte chunks and aligned where most chunks land."""
        if not hasattr(self, 'index'):
            self.index = {}
            for o in range(len(self.eu) - k):
                c = self.eu[o:o + k]
                self.index[c] = -1 if c in self.index else o
        covered = bytearray(BANK)
        for i, eb, ej, n in blocks: covered[i:i + n] = b'\1' * n
        gaps, i = [], 0
        while i < BANK:
            if covered[i]: i += 1; continue
            j = i
            while j < BANK and not covered[j]: j += 1
            if j - i >= gap_min: gaps.append((i, j))
            i = j
        extra = []
        for gs, ge in gaps:
            deltas = collections.Counter()
            for o in range(gs, ge - k, 4):
                c = a[o:o + k]
                if len(set(c)) < 3: continue
                e = self.index.get(c)
                if e is not None and e >= 0: deltas[e - (b * BANK + o)] += 1
            if not deltas: continue
            d, n = deltas.most_common(1)[0]
            if n < 3: continue
            e0 = b * BANK + gs + d
            eb = e0 // BANK
            lo, hi = max(eb * BANK, e0 - 0x100), min((eb + 1) * BANK, e0 + (ge - gs) + 0x100)
            sm = difflib.SequenceMatcher(None, a[gs:ge], self.eu[lo:hi], autojunk=False)
            for i, j, n in sm.get_matching_blocks():
                if n >= MIN_BLOCK: extra.append((gs + i, eb, (lo + j) % BANK, n))
        return sorted(blocks + extra)

    def place(self, b, addr):
        """(eu_bank, eu_addr, how) for a USA ROM address, or None."""
        bank = 0 if addr < BANK else b
        blocks = self.bank(bank)
        off = addr - (0 if addr < BANK else BANK)
        starts = [x[0] for x in blocks]
        i = bisect.bisect_right(starts, off) - 1
        if i >= 0:
            s, eb, ej, n = blocks[i]
            if off < s + n:
                ea = ej + (off - s)
                return eb, ea + (0 if bank == 0 else BANK), 'block'
        # not inside a block: search for the label's bytes around the neighbours' placement
        guesses = []
        for k in (i, i + 1):
            if 0 <= k < len(blocks):
                s, eb, ej, n = blocks[k]
                guesses.append((eb, ej + (off - s)))
        us_off = off_of(bank, addr)
        pat = self.us[us_off:us_off + 8]
        if len(set(pat)) < 3: return None
        hits = set()
        for eb, g in guesses:
            lo, hi = max(0, g - 0x400), min(BANK - len(pat), g + 0x400)
            base = eb * BANK
            seg = self.eu[base + lo:base + hi + len(pat)]
            p = seg.find(pat)
            while p >= 0:
                hits.add((eb, lo + p))
                p = seg.find(pat, p + 1)
        if len(hits) == 1:
            eb, ea = hits.pop()
            return (0 if bank == 0 else eb), ea + (0 if bank == 0 else BANK), 'search'
        return None


def map_extra(al, src, dst):
    """src/hooks/extra*.sym carried over: ROM labels placed like the .sym's, a `= bank:addr` source
    placed too, RAM addresses kept. Returns the number of lines that could not be placed."""
    bad = 0
    out = []
    for line in open(src):
        body = line.rstrip('\n')
        m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) (\S+)(?: = ([0-9a-f]{2}):([0-9a-f]{4})| (extern))?(.*)$', body.split('#')[0].strip()) if body.strip() and not body.startswith('#') else None
        if not m: out.append(body); continue
        b, a, n = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if a < 0x8000:
            p = al.place(b, a)
            if p is None: out.append(f'# unplaced {body}'); bad += 1; continue
            b, a = p[0], p[1]
        tail = ''
        if m.group(4):
            p = al.place(int(m.group(4), 16), int(m.group(5), 16))
            if p is None: out.append(f'# unplaced {body}'); bad += 1; continue
            tail = f' = {p[0]:02x}:{p[1]:04x}'
        elif m.group(6): tail = ' extern'
        out.append(f'{b:02x}:{a:04x} {n}{tail}')
    open(dst, 'w').write('\n'.join(out) + '\n')
    return bad


CODE_BANKS = set(range(0x00, 0x17)) | {0x3f}


def place_by_reference(us_path, sym_path, us, eu, al, placed):
    """Second pass: code the alignment carried over names its targets. Where an aligned instruction
    points at a label in the USA ROM, the same instruction in the EU ROM points at that label's EU
    address. Unplaced labels (the start of a routine the EU build changed, a table that moved)
    take the address their references agree on; placed labels the references contradict are
    returned for the report."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from routine_equiv import Game
    US = Game(us_path, sym_path)
    E = Game.__new__(Game); E.rom = eu; E.relocs = []
    at = collections.defaultdict(list)             # (bank, addr) -> [(name, bank, addr)]
    by_addr = collections.defaultdict(list)        # addr -> the same, every bank
    for key in placed: at[(key[1], key[2])].append(key); by_addr[key[2]].append(key)
    votes = collections.defaultdict(collections.Counter)
    strong = collections.defaultdict(collections.Counter)      # from instructions the alignment placed itself
    def vote(ub, v, eb, v2, sure):
        if v >= 0x8000 or v2 >= 0x8000: return
        if (v < BANK) != (v2 < BANK): return        # bank 0 stays bank 0, a switchable address stays switchable
        if v < BANK: keys, tb = at.get((0, v), []), 0
        else:
            keys = at.get((ub, v), [])
            tb = eb if keys else None
            if not keys:                            # another bank's table: only when unambiguous
                hits = [k for k in by_addr.get(v, []) if k[1] != ub]
                banks = {k[1] for k in hits}
                if len(banks) != 1: return
                keys = hits
                cands = al.pairs[hits[0][1]]
                ok = [c for c in cands if eu[c * BANK + v2 - BANK:c * BANK + v2 - BANK + 4] == us[hits[0][1] * BANK + v - BANK:hits[0][1] * BANK + v - BANK + 4]]
                if len(ok) != 1: return
                tb = ok[0]
        for k in keys:
            votes[k][(tb, v2)] += 1
            if sure: strong[k][(tb, v2)] += 1
    # walk only code the decomp hooks: a data label decoded as instructions votes for whatever
    # its bytes happen to say
    code = set()
    for path in ('src/hooks/generated.txt', 'src/hooks/generated_seasons.txt', 'src/hooks/generated_seasons_gen.txt'):
        if ('seasons' in path) != ('seasons' in os.path.basename(sym_path)) or not os.path.exists(path): continue
        for l in open(path):
            m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) ', l)
            if m: code.add((int(m.group(1), 16), int(m.group(2), 16)))
    for name, insts in US.instances.items():
        for (b, a) in insts:
            if b not in CODE_BANKS or a >= 0x8000 or (b, a) not in code: continue
            try: body = US.body(b, a)
            except Exception: continue
            start = placed.get((name, b, a))
            for (ia, ln, kind, tmpl, ops) in body:
                p = al.place(b, ia)
                sure = bool(p and p[2] == 'block')
                if sure: eb, ea = (p[0] if ia >= BANK else b), p[1]
                elif start and start[2] == 'block':
                    # an instruction the alignment broke up (operands that point at moved code
                    # leave one-byte blocks): where it sits from the routine's own entry
                    eb, ea = (start[0] if ia >= BANK else b), start[1] + (ia - a)
                else: continue
                ln2, kind2, tmpl2, ops2 = E.decode(eb, ea)
                if kind == 'jumptable':
                    if tmpl2 != 'rst $00': continue
                    for i, (k, v) in enumerate(ops):
                        o = (ea + 1 + 2 * i) if ea < BANK else eb * BANK + ea + 1 + 2 * i - BANK
                        vote(b, v, eb, eu[o] | eu[o + 1] << 8, sure)
                    continue
                if (ln2, tmpl2) != (ln, tmpl): continue
                for (k, v), (k2, v2) in zip(ops, ops2):
                    if k in ('rom', 'any'): vote(b, v, eb, v2, sure)
    newly, conflicts = {}, []
    for key, c in votes.items():
        (best, n), total = c.most_common(1)[0], sum(c.values())
        if n * 3 < total * 2: continue                 # references disagree: leave it alone
        if placed.get(key):
            pb, pa = placed[key][0], placed[key][1]
            # overriding the alignment takes near-unanimous references from instructions the
            # alignment itself placed (one found from its routine's entry may sit in changed code)
            sc = strong.get(key)
            if not sc: continue
            (best, n), total = sc.most_common(1)[0], sum(sc.values())
            if (pb, pa) != best and n >= 2 and n * 10 >= total * 9:
                # an entry point the EU build inserted code at: the bytes moved, the label did not
                conflicts.append((key, (pb, pa), best, n, total))
                newly[key] = (best[0], best[1], 'ref')
            continue
        newly[key] = (best[0], best[1], 'ref')
    return newly, conflicts


def place_between(labels, placed):
    """Third pass: a label still unplaced between two placed ones whose distance the EU build kept
    (a pointer table whose entries moved, a script whose operands changed: other bytes, same
    size) sits at the same offset from them in the EU ROM ("between")."""
    by_bank = collections.defaultdict(list)
    for key in labels:
        if key[2] < 0x8000: by_bank[0 if key[2] < BANK else key[1]].append(key)
    out = {}
    for b, keys in by_bank.items():
        keys.sort(key=lambda k: k[2])
        addrs = [k[2] for k in keys]
        for i, key in enumerate(keys):
            if placed.get(key): continue
            prev = next((keys[j] for j in range(i - 1, -1, -1) if placed.get(keys[j]) and keys[j][2] < key[2]), None)
            nxt = next((keys[j] for j in range(i + 1, len(keys)) if placed.get(keys[j]) and keys[j][2] > key[2]), None)
            if not prev or not nxt: continue
            pp, pn = placed[prev], placed[nxt]
            if pp[0] != pn[0] or pn[1] - pp[1] != nxt[2] - prev[2]: continue
            out[key] = (pp[0], pp[1] + (key[2] - prev[2]), 'between')
    return out


def main():
    args = sys.argv[1:]
    extras = []
    while '--extra' in args:
        i = args.index('--extra'); extras.append((args[i + 1], args[i + 2])); del args[i:i + 3]
    report = None
    if '--report' in args:
        i = args.index('--report'); report = args[i + 1]; del args[i:i + 2]
    us_path, sym_path, eu_path, out_path = args
    us, eu = rd(us_path), rd(eu_path)
    pairs = bank_pairs(us, eu)
    al = Aligner(us, eu, pairs)
    lines = open(sym_path).read().split('\n')
    placed, labels = {}, []
    section = None
    for line in lines:
        s = line.strip()
        if s.startswith('['): section = s
        m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) (\S+)$', s) if section == '[labels]' else None
        if not m: continue
        b, a, n = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if a >= 0x8000: continue
        key = (n, b, a)
        labels.append(key)
        p = al.place(b, a)
        if p: placed[key] = p
    newly, conflicts = place_by_reference(us_path, sym_path, us, eu, al, {k: placed.get(k) for k in labels})
    placed.update(newly)
    placed.update(place_between(labels, placed))
    out, rows = [], []
    counts = collections.Counter()
    section = None
    for line in lines:
        s = line.strip()
        if s.startswith('['): section = s
        m = re.match(r'([0-9a-f]{2}):([0-9a-f]{4}) (\S+)$', s) if section == '[labels]' else None
        if not m:
            if section in ('[sections]', '[ramsections]'): continue      # USA layout, meaningless here
            out.append(line); continue
        b, a, n = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if a >= 0x8000:
            out.append(line); continue
        p = placed.get((n, b, a))
        if p is None:
            counts['unplaced'] += 1
            rows.append((n, f'{b:02x}:{a:04x}', '-', 'unplaced'))
            out.append(f'; unplaced {b:02x}:{a:04x} {n}')
            continue
        eb, ea, how = p
        counts[how] += 1
        rows.append((n, f'{b:02x}:{a:04x}', f'{eb:02x}:{ea:04x}', how))
        out.append(f'{eb:02x}:{ea:04x} {n}')
    for key, was, refs, n, total in conflicts:
        rows.append((key[0], f'{key[1]:02x}:{key[2]:04x}', f'{refs[0]:02x}:{refs[1]:04x}', f'moved by references ({n}/{total}) from {was[0]:02x}:{was[1]:04x}'))
    hdr = [l for l in out if not re.match(r'[0-9a-f]{2}:[0-9a-f]{4} ', l.strip())]
    labs = sorted((l for l in out if re.match(r'[0-9a-f]{2}:[0-9a-f]{4} ', l.strip())), key=lambda l: (int(l[:2], 16), int(l[3:7], 16)))
    # keep the file's shape: header, [labels] sorted, the rest
    i = hdr.index('[labels]') if '[labels]' in hdr else len(hdr)
    with open(out_path, 'w') as f:
        f.write('\n'.join(hdr[:i + 1] + labs + hdr[i + 1:]) + '\n')
    for src, dst in extras:
        print(f'{dst}: {map_extra(al, src, dst)} lines unplaced')
    if report:
        with open(report, 'w') as f:
            for r in rows: f.write('\t'.join(r) + '\n')
    total = sum(counts.values())
    print(f'{out_path}: {total} ROM labels: ' + ', '.join(f'{k} {v} ({100.0 * v / total:.1f}%)' for k, v in counts.most_common()) + f'; {len(conflicts)} block placements corrected by references')
    print('banks: ' + ' '.join(f'{b:02x}>' + '+'.join(f'{x:02x}' for x in pairs[b]) for b in sorted(pairs) if pairs[b] != [b]))


if __name__ == '__main__':
    main()
