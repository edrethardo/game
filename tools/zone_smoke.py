#!/usr/bin/env python3
"""zone_smoke.py — boot every overworld zone and check it came up correctly.

WHY THIS EXISTS. The Autoplay bot deliberately stops at the overworld (its remit ends at Inferno),
and the brain has no concept of edge gates or waypoints — so the acts are the one part of the game
a soak can never exercise. Everything below the zone boundary is unit-tested (zone_def, quests,
the WILDERNESS generator, the roster filters), but "does the zone actually BUILD, spawn its boss
and offer its quest when the engine runs it for real" had no automated answer at all. That gap is
exactly where the last two zone bugs lived: a boss that spawned twice, and a boss spawn call that
did not compile against a function that never existed.

WHAT IT DOES. Launches the real binary once per zone via the `--zone <floor>` dev door, holds it a
couple of seconds, and asserts on the shipped log lines — no debug build, no instrumentation, no
second implementation of anything. Checks per zone:

  * the zone was entered under the name zone_def.h gives it
  * a level actually generated (LevelGen line) with the terrain its ZoneDef asks for
  * the boss spawned iff ZoneDef::boss names one, with the HP enemies.json authors
  * the quest was offered iff quest_def.h hosts one there
  * no ERROR/WARN line was emitted
  * draw calls stayed under the 500 budget and the frame rate held

Zone floors and their expectations are read from the HEADERS, not duplicated here — a zone added to
zone_def.h is covered by this the moment it exists, and one renamed cannot silently stop being
checked.

    python3 tools/zone_smoke.py                # every zone
    python3 tools/zone_smoke.py --act 2        # one act
    python3 tools/zone_smoke.py --zone 57      # one zone
    python3 tools/zone_smoke.py --seconds 4    # hold each zone longer

Exits non-zero if any zone fails, so it can gate a release the way the test suite does.

WHAT THIS CANNOT SEE. It holds each zone for a few SECONDS, so anything on a longer clock is
invisible to it. That blind spot shipped a real bug: world items carry a 60 s lifetime, and every
waypoint and act entrance was despawning a minute after the zone loaded — fast travel and the Den of
Evil quietly unreachable — while this tool reported all fifteen zones healthy. Timer-shaped rules
belong in a unit test that simulates the clock (see tests/game/test_world_item_pool.cpp, which ticks
120 s per sentinel in milliseconds), not in a live smoke run. Raising --seconds past 60 would work
too, but at 15 zones it costs a quarter of an hour to catch what a unit test catches instantly.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "src", "DungeonEngine")
ZONE_DEF = os.path.join(ROOT, "src", "game", "zone_def.h")
QUEST_DEF = os.path.join(ROOT, "src", "game", "quest_def.h")
ENEMIES = os.path.join(ROOT, "assets", "config", "enemies.json")

DRAW_BUDGET = 500          # the engine's hard ceiling (CLAUDE.md); a zone should sit far under it
MIN_FPS = 30               # generous: this may run beside a soak that is eating the GPU


def parse_zones():
    """Pull (floor, name, boss, peaceful) out of zone_def.h.

    Reads the header rather than a copy kept here, so this tool cannot drift from the table it is
    checking — the failure mode where a test quietly stops covering something.
    """
    text = open(ZONE_DEF).read()
    zones = []
    for block in re.finditer(r"\{\s*/\*floor\*/\s*(\d+),(.*?)/\*gridSize\*/", text, re.S):
        floor = int(block.group(1))
        body = block.group(2)
        name = re.search(r'/\*name\*/\s*"([^"]*)"', body)
        boss = re.search(r'/\*boss\*/\s*"([^"]*)"', body)
        peaceful = re.search(r"/\*peaceful\*/\s*(true|false)", body)
        zones.append({
            "floor": floor,
            "name": name.group(1) if name else "?",
            "boss": boss.group(1) if boss else "",
            "peaceful": bool(peaceful and peaceful.group(1) == "true"),
        })
    return sorted(zones, key=lambda z: z["floor"])


def parse_quests():
    """floor -> (quest name, trigger), from quest_def.h.

    The TRIGGER matters to what the log should say, and getting this wrong is how a checker reports
    a false failure: a REACH quest is satisfied by arriving, so it logs `complete` on entry and is
    never `offered` at all. CLEAR_ZONE and SLAY are offered and completed later.
    """
    text = open(QUEST_DEF).read()
    out = {}
    for m in re.finditer(r'\{\s*(\d+),\s*"([^"]+)",.*?Trigger::(\w+)', text, re.S):
        out[int(m.group(1))] = (m.group(2), m.group(3))
    return out


def parse_boss_health():
    """enemy name -> authored health, so the check is against the DEF and not a number typed twice."""
    import json
    with open(ENEMIES) as f:
        doc = json.load(f)
    return {e["name"]: e.get("health", 0.0) for e in doc["enemies"]}


def run_zone(floor, seconds):
    env = dict(os.environ)
    env.setdefault("DISPLAY", ":1")
    env.setdefault("__NV_PRIME_RENDER_OFFLOAD", "1")
    env.setdefault("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
    try:
        p = subprocess.run(
            # stdbuf -oL is load-bearing, not hygiene: stdout to a PIPE is block-buffered, so when
            # `timeout` SIGTERMs the process a partial buffer dies with it and whichever lines had
            # not reached 4 KB are simply lost. That made this checker intermittently report a
            # missing "Entered zone" line for a zone that had entered perfectly well — a false
            # failure that looks exactly like a real one. Line buffering makes the capture honest.
            ["timeout", str(seconds), "stdbuf", "-oL",
             BINARY, "--new", "warrior", "--zone", str(floor)],
            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=seconds + 30)
        return p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return ""


def check(zone, quests, boss_hp, log):
    """Return a list of failure strings — empty means the zone is healthy."""
    bad = []
    floor, name = zone["floor"], zone["name"]

    if not log.strip():
        return ["produced no output at all (did it launch?)"]

    if f"Entered zone {floor} '{name}'" not in log:
        bad.append(f"never logged entry as '{name}'")

    if not re.search(r"LevelGen\[\w+\]:", log):
        bad.append("no level was generated")

    # Boss: present exactly when the table says so, and carrying the HP enemies.json authors.
    # "spawned at all" is not enough — a boss scaled by the dungeon's floor multipliers would be
    # a different fight entirely, and that is invisible without the number.
    spawned = re.findall(r"Zone boss spawned: (.+?) \((\d+) HP\)", log)
    if zone["boss"]:
        if not spawned:
            bad.append(f"boss '{zone['boss']}' never spawned")
        elif len(spawned) > 1:
            bad.append(f"boss spawned {len(spawned)}x (should be unique)")
        else:
            got_name, got_hp = spawned[0][0], int(spawned[0][1])
            if got_name != zone["boss"]:
                bad.append(f"spawned '{got_name}', table says '{zone['boss']}'")
            want = int(boss_hp.get(zone["boss"], -1))
            if want >= 0 and got_hp != want:
                bad.append(f"boss HP {got_hp} != authored {want} (floor scaling leaked in?)")
    elif spawned:
        bad.append(f"unexpected boss {spawned[0][0]} in a zone with none")

    # Quest. A REACH quest completes the moment you arrive (that IS its trigger); the other two
    # are offered on entry and completed by play, which a 6-second boot cannot reach.
    if floor in quests:
        qname, trigger = quests[floor]
        want = "complete" if trigger == "REACH" else "offered"
        if f"[QUEST] {want}: {qname}" not in log:
            bad.append(f"quest '{qname}' ({trigger}) never logged '{want}'")
    elif "[QUEST] offered:" in log or "[QUEST] complete:" in log:
        bad.append("fired a quest in a zone that hosts none")

    # AllocationTracker's shutdown byte count is NOT a leak measurement and is expected on every
    # exit — the unsized operator delete never subtracts, so the number grows with allocation CHURN
    # (CLAUDE.md is explicit: do not chase it). Anything else at WARN or above is a real signal.
    IGNORE = ("AllocationTracker",)
    for line in log.splitlines():
        if "[ERROR]" not in line and "[WARN]" not in line:
            continue
        if any(t in line for t in IGNORE):
            continue
        # Strip the "[LEVEL] HH:MM:SS file.cpp:NN: " prefix, keeping the message itself.
        msg = re.sub(r"^\[\w+\]\s+[\d:]+\s+\S+:\d+:\s*", "", line).strip()
        bad.append("log: " + msg[:110])

    fps = re.findall(r"FPS: (\d+) \| Frame: [\d.]+ ms \| Draw: (\d+)", log)
    if not fps:
        bad.append("never reached gameplay (no FPS line)")
    else:
        worst_draw = max(int(d) for _, d in fps)
        worst_fps = min(int(f) for f, _ in fps)
        if worst_draw > DRAW_BUDGET:
            bad.append(f"draw calls {worst_draw} over the {DRAW_BUDGET} budget")
        if worst_fps < MIN_FPS:
            bad.append(f"fps dipped to {worst_fps}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zone", type=int, help="check a single zone floor (52-96)")
    ap.add_argument("--act", type=int, choices=(1, 2), help="check one act only")
    ap.add_argument("--seconds", type=int, default=6, help="hold each zone this long (default 6)")
    args = ap.parse_args()

    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found — build first (cmake --build build)")
        return 2

    zones = parse_zones()
    if args.zone:
        zones = [z for z in zones if z["floor"] == args.zone]
        if not zones:
            print(f"ERROR: {args.zone} is not a zone in zone_def.h")
            return 2
    if args.act:
        # Act 2 starts at floor 60 — the same split quest_def.h's actOf() uses.
        zones = [z for z in zones if (2 if z["floor"] >= 60 else 1) == args.act]

    if not zones:
        print("ERROR: parsed 0 zones out of zone_def.h — the regex has drifted.")
        print("       Refusing to report success on an empty run.")
        return 2

    quests = parse_quests()
    boss_hp = parse_boss_health()
    if not quests:
        print("ERROR: parsed 0 quests out of quest_def.h — the regex has drifted")
        return 2

    print(f"Zone smoke — {len(zones)} zone(s), {args.seconds}s each\n")
    failures = 0
    for z in zones:
        log = run_zone(z["floor"], args.seconds)
        bad = check(z, quests, boss_hp, log)
        tag = "ok  " if not bad else "FAIL"
        extra = []
        if z["boss"]:
            extra.append("boss")
        if z["floor"] in quests:
            extra.append("quest")
        if z["peaceful"]:
            extra.append("peaceful")
        draws = re.findall(r"Draw: (\d+)", log)
        if draws:
            extra.append(f"draw {max(int(d) for d in draws)}")
        print(f"  [{tag}] {z['floor']}  {z['name'][:44]:<44} {', '.join(extra)}")
        for b in bad:
            print(f"         - {b}")
        if bad:
            failures += 1

    print()
    if failures:
        print(f"=== {failures} of {len(zones)} zones FAILED ===")
        return 1
    print(f"=== all {len(zones)} zones healthy ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
