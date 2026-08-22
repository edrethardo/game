#!/usr/bin/env python3
"""log_time.py — map gameplay log lines to SIM time inside a --record take.

A lockstep capture writes one 'screenshot saved: ...frame_NNNNNN.png' log line per simulated
frame, so any other log line's position BETWEEN two frame lines pins it to an exact frame —
and frame/60 is the second inside take.mp4. Wall-clock timestamps are useless here (the sim
runs at ~4-7 fps wall while recording); this interleave is the only honest clock.

  tools/log_time.py <take.log> <regex> [more regexes...]

Prints  sim_seconds  frame  matched-line  for every match of every pattern.
"""
import re, sys

if len(sys.argv) < 3:
    sys.exit(__doc__)

frame_re = re.compile(r"screenshot saved: .*frame_(\d{6})\.png")
pats = [re.compile(p) for p in sys.argv[2:]]

cur = -1
for line in open(sys.argv[1], errors="replace"):
    m = frame_re.search(line)
    if m:
        cur = int(m.group(1))
        continue
    for p in pats:
        if p.search(line):
            # the event happened while frame cur+1 was being simulated
            f = cur + 1
            print(f"{f/60.0:8.2f}s  frame {f:6d}  {line.rstrip()[:120]}")
            break
