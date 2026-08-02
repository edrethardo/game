#!/usr/bin/env python3
"""Autoplay soak report.

Usage:  python3 tools/soak_report.py <log-dir>

Reads a directory of autoplay soak logs (one .log per instance) and summarises progress, stalls,
loot and weapon procs.

WHY THIS LIVES IN tools/ AND NOT A SCRATCHPAD: it spent its life in a /tmp session directory, which
the tmp cleaner wiped mid-project at least once, and it silently rotted besides (see below). A soak
is the project's main measuring instrument; its reader belongs in the repo with the code it reads.

WHY THE PARSER IS KEY=VALUE AND NOT ONE BIG REGEX: the previous version matched the whole `[STALL]`
line in a single pattern that required `fm=` and `rem=` to be ADJACENT. When the line later gained
`dB=` and `bL=` between them, the pattern stopped matching entirely and the script reported
"STALL samples: 0" — indistinguishable from a soak with no stalls, which is exactly the wrong way for
a measuring tool to fail. Tokenising `key=value` pairs means a new field can never break an old
reader; it just shows up as a new key.
"""
import collections
import glob
import os
import re
import sys

TOKEN = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=([^\s|]+)')
HB = re.compile(r'\[TELEM-HB\] cls=(?P<cls>.+?) fl=\d+ ')


def tokens(line, marker):
    """All key=value pairs after `marker`, as a dict. Absent keys simply don't appear."""
    i = line.find(marker)
    if i < 0:
        return None
    return dict(TOKEN.findall(line[i + len(marker):]))


def num(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except (TypeError, ValueError):
        return default


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else '.'
    files = sorted(glob.glob(os.path.join(directory, '*.log')))
    if not files:
        sys.exit(f'soak_report: no .log files in {directory}')

    print(f'=== {directory} ===')
    print(f"{'instance':<28}{'class':<17}{'maxfl':>6}{'eff':>5}{'deaths':>7}{'kills':>7}"
          f"{'elapsed':>8}{'worstFl':>8}")

    stalls, worst, procs, mythics = [], [], collections.Counter(), []
    total = collections.Counter()
    deaths_revives = []

    for path in files:
        name = os.path.basename(path)[:-4]
        lanes, last, worst_floor = {}, 0, collections.Counter()
        deaths = revives = 0
        for line in open(path, errors='ignore'):
            hb = tokens(line, '[TELEM-HB]')
            if hb:
                cls = HB.search(line)
                cls = cls.group('cls') if cls else '?'
                e = lanes.setdefault(cls, {'fl': 0, 'eff': 0, 'd': 0, 'k': 0})
                e['fl'] = max(e['fl'], int(num(hb, 'fl')))
                e['eff'] = max(e['eff'], int(num(hb, 'eff')))
                e['d'], e['k'] = int(num(hb, 'deaths')), int(num(hb, 'kills'))
                last = max(last, int(num(hb, 'elapsed')))
                eff = int(num(hb, 'eff'))
                worst_floor[eff] = max(worst_floor[eff], int(num(hb, 'secs_fl')))
                continue
            st = tokens(line, '[STALL]')
            if st:
                st['_file'] = name
                stalls.append(st)
                continue
            if '[PROC] first fire' in line:
                m = re.search(r'id (\d+)', line)
                if m:
                    procs[int(m.group(1))] += 1
                continue
            my = tokens(line, '[MYTHIC] drop:')
            if my:
                mythics.append((name, my.get('defId'), my.get('itemLevel')))
                continue
            if 'Autoplay: DEATH' in line:
                deaths += 1
            elif 'Autoplay: REVIVED' in line:
                revives += 1

        w = max(worst_floor.values()) if worst_floor else 0
        worst.append((name, w))
        deaths_revives.append((name, deaths, revives))
        for cls, e in lanes.items():
            print(f"{name:<28}{cls:<17}{e['fl']:>6}{e['eff']:>5}{e['d']:>7}{e['k']:>7}"
                  f"{last:>8}{w:>8}")
            total['deaths'] += e['d']
            total['kills'] += e['k']

    print(f"\nTOTAL deaths={total['deaths']} kills={total['kills']}")

    # deaths == revives is the invariant that catches a run stranded on the death screen.
    bad = [(n, d, r) for n, d, r in deaths_revives if abs(d - r) > 1]
    print(f"deaths==revives: {'OK' if not bad else 'MISMATCH ' + str(bad)}")

    print(f"\nSTALL samples: {len(stalls)}")
    if stalls:
        print('  by style :', dict(collections.Counter(s.get('style', '?') for s in stalls)))
        print('  by remedy:', dict(collections.Counter(s.get('rem', '?') for s in stalls)))
        pinned = [s for s in stalls if num(s, 'net', 99) < 2.0]
        print(f'  PINNED (<2 m net travel): {len(pinned)}')
        if pinned:
            print('    style     :', dict(collections.Counter(s.get('style', '?') for s in pinned)))
            print('    bossGated :', sum(1 for s in pinned if s.get('bossG') == '1'))
            print('    no enemy within 2 m:', sum(1 for s in pinned if s.get('near2') == '0'))

    if procs:
        print('\nWeapon proc types that fired (SkillId ordinal -> instances):', dict(procs))
    if mythics:
        uniq = len({(d, l) for _, d, l in mythics})
        print(f'\nMYTHIC drops: {len(mythics)} lines, {uniq} distinct (defId,itemLevel)')
        rep = collections.Counter((d, l) for _, d, l in mythics).most_common(3)
        # A single (defId,itemLevel) repeating hundreds of times is the pickup/evict thrash
        # signature, not a drop count — see the autoEvictWorst fix (2026-08-02).
        if rep and rep[0][1] > 20:
            print(f'  WARNING: {rep[0][0]} repeats {rep[0][1]}x — churn signature, not distinct drops')

    print('\nWorst single-floor dwell per instance (s):')
    for n, w in sorted(worst, key=lambda x: -x[1]):
        print(f'   {n:<30}{w:>7}')


if __name__ == '__main__':
    main()
