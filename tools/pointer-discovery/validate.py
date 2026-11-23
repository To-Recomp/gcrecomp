#!/usr/bin/env python3
"""Score a gcrecomp_recompiler --report-pointers report against a known set of callback entries,
and disassemble a sample of the candidates it adds beyond that set.

  gcrecomp_recompiler main.dol --csv X.csv --discover-pointers=aggressive --report-pointers rep.tsv --stats
  validate.py rep.tsv --truth GQSF.extra_funcs.txt [--dol main.dol --spot 12]
  validate.py rep.tsv --dtk-symbols symbols.txt        # a decomp project's function list as ground truth

The report must come from a run WITHOUT the truth entries in --extra-funcs (they would show up as
"known"). Entries of the truth set that the existing passes already find (bl targets, symbol csv,
prologue scan) also show up as "known"; those are reported separately and do not count for recall.
Capstone (pip install capstone) is only needed for --spot.
"""
import argparse
import re
import struct
import sys


def read_hex_list(path):
    out = set()
    for line in open(path):
        line = line.split('#')[0].strip()
        if line:
            out.add(int(line.split()[0], 16))
    return out


def read_report(path):
    rows = {}
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        rows[int(f[0], 16)] = dict(tier=f[1], score=int(f[2]), promoted=f[3] == 'yes',
                                   reason=f[4], origins=f[5] if len(f) > 5 else '')
    return rows


def read_dtk_functions(path):
    """{start: size} of the `type:function` symbols of a decomp-toolkit symbols.txt."""
    funcs = {}
    pat = re.compile(r'=\s*\.\w+:0x([0-9A-Fa-f]+);.*type:function(?:.*size:0x([0-9A-Fa-f]+))?')
    for line in open(path):
        m = pat.search(line)
        if m:
            funcs[int(m.group(1), 16)] = int(m.group(2), 16) if m.group(2) else 0
    return funcs


def score_against_symbols(rows, funcs):
    """Precision of each tier against a complete function list (every function start is known)."""
    starts = sorted(funcs)
    import bisect

    def inside(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and starts[i] < a < starts[i] + funcs[starts[i]]

    print('Ground truth: %d function symbols' % len(funcs))
    for name, tiers in (('safe', ('safe',)), ('aggressive', ('safe', 'aggressive'))):
        picked = [a for a in rows if rows[a]['tier'] in tiers]
        tp = [a for a in picked if a in funcs]
        mid = [a for a in picked if inside(a)]
        other = [a for a in picked if a not in funcs and not inside(a)]
        print('  %-10s promoted %4d: true function starts %4d (precision %.1f%%), inside a real function %d, '
              'neither (data/padding/unlisted) %d'
              % (name, len(picked), len(tp), 100.0 * len(tp) / max(1, len(picked)), len(mid), len(other)))
        for a in sorted(mid + other)[:20]:
            print('     not a start: %08X  %s' % (a, rows[a]['origins'][:100]))
    unknown = [a for a in rows if rows[a]['tier'] == 'known']
    wrong_known = [a for a in unknown if a not in funcs]
    print('  (targets that were already entries: %d, %d of them not in the symbol list)' % (len(unknown), len(wrong_known)))
    found = {a for a in rows if rows[a]['tier'] in ('safe', 'aggressive', 'known') and a in funcs}
    rej = [a for a in rows if rows[a]['tier'] == 'rejected' and a in funcs]
    print('  true function starts that were rejected as candidates: %d' % len(rej))
    for a in sorted(rej)[:15]:
        print('     %08X  %s' % (a, rows[a]['reason']))


class Dol:
    def __init__(self, path):
        b = open(path, 'rb').read()
        h = struct.unpack('>54I', b[:0xd8])
        self.b = b
        self.secs = [(h[i], h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]

    def read(self, addr, n):
        for off, ad, size in self.secs:
            if ad <= addr < ad + size:
                return self.b[off + addr - ad: off + addr - ad + min(n, ad + size - addr)]
        return b''


def spot_check(dol, addrs, rows, count):
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
    except ImportError:
        print('(capstone not installed: skipping --spot)')
        return
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    step = max(1, len(addrs) // count)
    for a in addrs[::step][:count]:
        r = rows[a]
        print('\n%08X  %s score=%d  %s\n  origins: %s' % (a, r['tier'], r['score'], r['reason'], r['origins'][:160]))
        for i in md.disasm(dol.read(a - 8, 8), a - 8):
            print('   %08x  %-8s %s   (before)' % (i.address, i.mnemonic, i.op_str))
        n = 0
        for i in md.disasm(dol.read(a, 0x60), a):
            print('   %08x  %-8s %s' % (i.address, i.mnemonic, i.op_str))
            n += 1
            if i.mnemonic == 'blr' or n >= 14:
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('report', help='TSV written by gcrecomp_recompiler --report-pointers')
    ap.add_argument('--truth', help='file of known callback entries (one hex address per line)')
    ap.add_argument('--dtk-symbols', help='decomp-toolkit symbols.txt: function symbols are the ground truth')
    ap.add_argument('--dol', help='DOL, for --spot disassembly')
    ap.add_argument('--spot', type=int, default=0, help='disassemble N evenly spaced extra (non-truth) promoted candidates')
    args = ap.parse_args()

    rows = read_report(args.report)
    if args.dtk_symbols:
        score_against_symbols(rows, read_dtk_functions(args.dtk_symbols))
        if not args.truth:
            return 0
    if not args.truth:
        ap.error('--truth or --dtk-symbols is required')
    truth = read_hex_list(args.truth)
    tier = lambda a: rows[a]['tier'] if a in rows else 'absent'

    print('Known set: %d entries' % len(truth))
    for t in ('known', 'safe', 'aggressive', 'rejected', 'absent'):
        hit = sorted(a for a in truth if tier(a) == t)
        label = {'known': 'already found by the existing passes (not candidates)',
                 'safe': 'rediscovered, tier safe', 'aggressive': 'rediscovered, tier aggressive only',
                 'rejected': 'candidate but rejected', 'absent': 'no pointer evidence at all'}[t]
        print('  %-11s %2d  %s' % (t, len(hit), label))
        if t in ('rejected', 'absent'):
            for a in hit:
                print('              %08X  %s' % (a, rows[a]['reason'] if a in rows else ''))

    missed_by_existing = truth - {a for a in truth if tier(a) == 'known'}
    print('\nRecall over the %d entries the existing passes do not find:' % len(missed_by_existing))
    cum = 0
    for name, tiers in (('safe', ('safe',)), ('aggressive', ('safe', 'aggressive'))):
        found = sum(1 for a in missed_by_existing if tier(a) in tiers)
        extra = [a for a in rows if rows[a]['tier'] in tiers and a not in truth]
        print('  %-10s recall %d/%d = %.0f%%   candidates promoted: %d, of which in the known set: %d, beyond it: %d'
              % (name, found, len(missed_by_existing), 100.0 * found / max(1, len(missed_by_existing)),
                 found + len(extra), found, len(extra)))
    print('  precision against the known set cannot be measured directly (it only lists entries someone tripped over);'
          '\n  use --spot to judge the extras by disassembly.')

    bad = [a for a in rows if rows[a]['tier'] in ('safe', 'aggressive') and rows[a]['reason'].startswith('mid-function')]
    print('\nPromoted entries inside a known function body: %d' % len(bad))

    if args.spot and args.dol:
        extras = sorted(a for a in rows if rows[a]['tier'] in ('safe', 'aggressive') and a not in truth)
        spot_check(Dol(args.dol), extras, rows, args.spot)
    return 0


if __name__ == '__main__':
    sys.exit(main())
