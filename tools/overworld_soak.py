#!/usr/bin/env python3
"""overworld_soak.py — run every class through both ACTS with the bot driving, and report.

WHY THIS EXISTS. The dungeon has had a soak rig for a long time and it is what found the strands,
the livelocks and the silent drops. The acts had nothing: they shipped, and the only evidence they
worked was walking them by hand, which is slow, unrepeatable, and cannot answer "does a Tinkerer
finish Act 2".

WHAT IT ASSERTS. The pass condition is not "it did not crash" — that is how a bot parked on a death
screen looked healthy for three hours. A class PASSES only when it completes the last quest of Act 2
("Kill -9" at the rift, zone 66). Anything else is reported with the reason, because the interesting
outcomes are the partial ones: which zone it reached, which quest it was on, whether it was stranded
by a gate, and how many times it died getting there.

    tools/overworld_soak.py                    # all 9 classes, 30 min each, concurrent
    tools/overworld_soak.py --minutes 90       # longer
    tools/overworld_soak.py --classes warrior,sorcerer
    tools/overworld_soak.py --report <dir>     # re-read an earlier run's logs without playing

CONCURRENCY. Every instance shares one GPU, and past soaks measured that ~12 is where a fixed-
timestep sim starts buying less sim-time per wall-second (16 dropped the box to ~36 FPS). Nine
classes fits under that, so they all run at once and the wall-clock is one soak, not nine.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "src", "DungeonEngine")

CLASSES = ["warrior", "sorcerer", "marksman", "rogue", "paladin",
           "ranger", "tinkerer", "wanderer", "combat_engineer"]

# The act's finale. A run that has not completed this has not finished the acts, whatever else it did.
FINAL_ZONE = 66

# The GPU environment past soaks used. `:1` is the real desktop display — the game needs a real GL
# context, so there is no headless option here.
GPU_ENV = {
    "DISPLAY": ":1",
    "__NV_PRIME_RENDER_OFFLOAD": "1",
    "__GLX_VENDOR_LIBRARY_NAME": "nvidia",
}

ZBOT_RE = re.compile(
    r"\[ZBOT\] zone=(\d+) task=(\d+) obj=(\d+) goal=\(([-\d.]+),([-\d.]+)\) d=([-\d.]+) "
    r"hop=(\d+) p=\(([-\d.]+),([-\d.]+)\) hp=([-\d.]+)")
CROSS_RE = re.compile(r"\[ZONEX\] (?:crossing|portal) (\d+) -> (\d+)")
REFUSE_RE = re.compile(r"\[ZONEX\] refused (\d+) -> (\d+)")


def parse(path):
    """Everything the report needs, in one pass over the log."""
    r = {
        "zones": OrderedDict(),      # zone floor -> seconds observed (1 Hz telemetry, so ~= samples)
        "quests_done": [],           # objective floor each time it advanced
        "crossings": 0, "refusals": 0, "deaths": 0, "revives": 0,
        "stranded": None, "acts_complete": False, "final_zone": None,
        "last_obj": None, "warns": 0, "errors": 0, "samples": 0,
        "crash": False, "build": None,
    }
    seen_obj = None
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                if "[build " in line and r["build"] is None:
                    m = re.search(r"\[build ([^\]]+)\]", line)
                    if m:
                        r["build"] = m.group(1)
                m = ZBOT_RE.search(line)
                if m:
                    r["samples"] += 1
                    zone = int(m.group(1))
                    obj = int(m.group(3))
                    r["zones"][zone] = r["zones"].get(zone, 0) + 1
                    r["final_zone"] = zone
                    r["last_obj"] = obj
                    # The objective advancing IS a quest completing — the mask only moves forward.
                    if seen_obj is not None and obj != seen_obj:
                        r["quests_done"].append(seen_obj)
                    seen_obj = obj
                    continue
                if CROSS_RE.search(line):
                    r["crossings"] += 1
                elif REFUSE_RE.search(line):
                    r["refusals"] += 1
                elif "[REVIVE]" in line:
                    r["revives"] += 1
                elif "Autoplay: DEATH" in line:
                    r["deaths"] += 1
                elif "ACTS COMPLETE" in line:
                    r["acts_complete"] = True
                elif "STRANDED" in line:
                    m2 = re.search(r"STRANDED in zone (\d+) — objective (\d+)", line)
                    r["stranded"] = (int(m2.group(1)), int(m2.group(2))) if m2 else ("?", "?")
                elif "[WARN]" in line:
                    r["warns"] += 1
                elif "[ERROR]" in line or "Segmentation fault" in line or "Assertion" in line:
                    r["errors"] += 1
                    r["crash"] = True
    except FileNotFoundError:
        pass
    return r


def verdict(r):
    """PASS only on finishing the acts. Everything else names WHY, because the partial outcomes are
    the whole point of running this."""
    if r["crash"]:
        return "CRASH", "engine error/assert in log"
    if r["acts_complete"]:
        return "PASS", "both acts complete"
    if r["samples"] == 0:
        return "FAIL", "never reached a zone (no [ZBOT] telemetry at all)"
    if r["stranded"]:
        return "FAIL", f"route stranded in zone {r['stranded'][0]} needing {r['stranded'][1]}"
    return "PARTIAL", (f"reached zone {r['final_zone']}, still on quest zone {r['last_obj']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=30.0)
    ap.add_argument("--classes", default=",".join(CLASSES))
    ap.add_argument("--start-zone", type=int, default=52)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--report", default=None, help="re-read an earlier run's logs, play nothing")
    args = ap.parse_args()

    classes = [c.strip() for c in args.classes.split(",") if c.strip()]

    if args.report:
        outdir = args.report
    else:
        if not os.path.exists(BIN):
            print(f"ERROR: {BIN} not found — build first", file=sys.stderr)
            return 2
        outdir = args.outdir or os.path.join(ROOT, "soak_overworld_%d" % int(time.time()))
        os.makedirs(outdir, exist_ok=True)

        env = dict(os.environ)
        env.update(GPU_ENV)
        secs = int(args.minutes * 60)
        procs = []
        print(f"=== overworld soak: {len(classes)} classes x {args.minutes:g} min, concurrent ===")
        print(f"    logs -> {outdir}\n")
        for cls in classes:
            log = os.path.join(outdir, f"{cls}.log")
            # stdbuf -oL: libc block-buffers a redirected stdout, so a live run looks frozen for
            # minutes at a time. That has cost a soak diagnosis before.
            cmd = ["timeout", str(secs), "stdbuf", "-oL", BIN,
                   "--autoplay", "--new", cls, "--zone", str(args.start_zone), "--endgame"]
            procs.append((cls, subprocess.Popen(cmd, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                                env=env, cwd=ROOT)))
            time.sleep(1.5)   # stagger the GL context creation; nine at once can fail to get one
        for cls, p in procs:
            p.wait()
        print("all runs finished\n")

    rows = []
    for cls in classes:
        r = parse(os.path.join(outdir, f"{cls}.log"))
        v, why = verdict(r)
        rows.append((cls, v, why, r))

    print(f"{'class':<10} {'verdict':<8} {'zone':>5} {'quests':>7} {'cross':>6} "
          f"{'refus':>6} {'deaths':>7} {'why'}")
    print("-" * 100)
    for cls, v, why, r in rows:
        print(f"{cls:<10} {v:<8} {str(r['final_zone'] or '-'):>5} {len(r['quests_done']):>7} "
              f"{r['crossings']:>6} {r['refusals']:>6} {r['deaths']:>7}  {why}")

    npass = sum(1 for _, v, _, _ in rows if v == "PASS")
    print(f"\n{npass}/{len(rows)} classes finished both acts")
    # deaths == revives is the invariant that catches a stranded death screen, which is how the
    # worst autoplay bug in this project's history hid for weeks.
    for cls, _, _, r in rows:
        if r["deaths"] != r["revives"]:
            print(f"  !! {cls}: {r['deaths']} deaths but {r['revives']} revives — a death may be stranding")
    print(f"\nlogs: {outdir}")
    return 0 if npass == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
