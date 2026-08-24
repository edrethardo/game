#!/usr/bin/env python3
"""arena_soak.py — bot-vs-bot ARENA matches as a map-evaluation dataset.

Launches one HOST and N-1 JOINING clients, every lane bot-driven (the arena autoplay branch,
engine_autoplay_arena.cpp), waits for the match to decide (first to Arena::KILL_TARGET) or the
time cap, then reduces the logs to the numbers a map change actually moves:

  - match duration + time to FIRST BLOOD (spawn/route quality)
  - kill cadence (median seconds between deaths — smaller map should tighten it)
  - per-slot kills/deaths (fairness: 4-fold symmetry should keep spreads modest)
  - engagement range (median [ARENA-BOT] nearest) and NO-CONTACT ratio (tgts seen but
    nearest > 20 m or no LOS wandering — a too-big map reads as high no-contact)
  - vertical occupancy (share of samples at y >= 2.5 — balcony/crown use)

  tools/arena_soak.py [--bots 4] [--minutes 6] [--classes warrior,marksman,ranger,sorcerer]

Runs headless-ish on :1 (windows open; standing go-ahead applies). Kills by explicit PID.
"""
import argparse, os, re, signal, statistics, subprocess, time

ROOT = "/home/aaron/game"
BIN  = os.path.join(ROOT, "build", "src", "DungeonEngine")


def launch(args, log):
    f = open(log, "w")
    env = dict(os.environ, DISPLAY=":1")
    return subprocess.Popen([BIN] + args, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, env=env)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bots", type=int, default=4)
    ap.add_argument("--minutes", type=float, default=6.0)
    ap.add_argument("--classes", default="warrior,marksman,ranger,sorcerer")
    ap.add_argument("--map", type=int, default=None, help="0 Hall, 1 Crucible, 2 Pit, 3 Motherboard")
    ap.add_argument("--outdir", default="/tmp/claude-1000/arena_soak")
    a = ap.parse_args()
    classes = a.classes.split(",")
    os.makedirs(a.outdir, exist_ok=True)

    procs = []
    logs = []
    host_log = os.path.join(a.outdir, "host.log")
    logs.append(host_log)
    host_args = ["--host", "--lan", "--new", classes[0], "--arena", "--autoplay"]
    if a.map is not None:
        host_args += ["--arena-map", str(a.map)]   # joiners inherit via the seed broadcast
    procs.append(launch(host_args, host_log))
    time.sleep(6)   # server up before the joiners knock
    for i in range(1, a.bots):
        lg = os.path.join(a.outdir, f"client{i}.log")
        logs.append(lg)
        procs.append(launch(["--join", "127.0.0.1", "--new", classes[i % len(classes)],
                             "--autoplay"], lg))
        time.sleep(2)

    deadline = time.time() + a.minutes * 60
    winner = None
    try:
        while time.time() < deadline and winner is None:
            time.sleep(5)
            if any(p.poll() is not None for p in procs):
                print("a process exited early"); break
            with open(host_log, errors="replace") as f:
                for line in f:
                    m = re.search(r"\[ARENA\] over: winner=(\d+) scores=(\S+)", line)
                    if m:
                        winner = (int(m.group(1)), m.group(2)); break
    finally:
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        time.sleep(2)

    # ---- reduce ----
    deaths = []       # (t_seconds_from_host_start, victim, killer)
    t0 = None
    for line in open(host_log, errors="replace"):
        ts = re.match(r"\[\w+\] (\d+):(\d+):(\d+)", line)
        if ts and t0 is None:
            t0 = int(ts.group(1)) * 3600 + int(ts.group(2)) * 60 + int(ts.group(3))
        m = re.search(r"(\d+):(\d+):(\d+).*\[ARENA\] death: victim=(\d+) killer=(\d+)", line)
        if m:
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) - (t0 or 0)
            deaths.append((t, int(m.group(4)), int(m.group(5))))

    near, high, samples = [], 0, 0
    nocontact = 0
    for lg in logs:
        for line in open(lg, errors="replace"):
            m = re.search(r"\[ARENA-BOT\] slot=\d+ pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) hp=\S+ "
                          r"tgts=(\d+) nearest=([-\d.]+)", line)
            if not m:
                continue
            samples += 1
            y = float(m.group(2)); tg = int(m.group(4)); nr = float(m.group(5))
            if y >= 2.5:
                high += 1
            if tg > 0 and nr >= 0:
                near.append(nr)
                if nr > 20.0:
                    nocontact += 1

    # --- mode metrics (WB-300): the loot economy's footprint on the match -----------------
    def tsec(line):
        m = re.match(r"\[\w+\] (\d+):(\d+):(\d+)", line)
        return (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) - (t0 or 0)) if m else None
    waves = sum(1 for l in open(host_log, errors="replace") if "[ARENA] loot: wave" in l)
    monster_drops = sum(1 for l in open(host_log, errors="replace") if "monster drop" in l)
    monsters = sum(1 for l in open(host_log, errors="replace") if "[ARENA] monster:" in l)
    first_loot = None
    pickups = 0
    for lg in logs:
        for l in open(lg, errors="replace"):
            if "AutoEquip" in l or "pickup result" in l and "accept=1" in l:
                pickups += 1
                t = tsec(l)
                if t is not None and (first_loot is None or t < first_loot):
                    first_loot = t
    match_end = None
    for l in open(host_log, errors="replace"):
        if "[ARENA] over" in l:
            match_end = tsec(l); break

    print(f"== ARENA SOAK: {a.bots} bots, cap {a.minutes} min ==")
    if winner:
        print(f"match decided: winner slot {winner[0]}, scores {winner[1]}"
              + (f"  duration {match_end} s" if match_end else ""))
    else:
        print("match NOT decided inside the cap")
    print(f"loot: {waves} waves  {pickups} pickups/equips  first loot at {first_loot} s  "
          f"monsters {monsters} spawned / {monster_drops} killed+dropped")
    if deaths:
        gaps = [b[0] - x[0] for x, b in zip(deaths, deaths[1:])]
        print(f"deaths: {len(deaths)}  first blood at {deaths[0][0]} s  "
              f"median kill gap {statistics.median(gaps) if gaps else '-'} s")
        per = {}
        for _, v, k in deaths:
            per.setdefault(k, [0, 0])[0] += 1
            per.setdefault(v, [0, 0])[1] += 1
        for s in sorted(per):
            print(f"  slot {s}: kills {per[s][0]}  deaths {per[s][1]}")
    if near:
        print(f"engagement: median nearest {statistics.median(near):.1f} m  "
              f"no-contact(>20m) {100.0*nocontact/len(near):.0f}%  "
              f"high-ground {100.0*high/max(1,samples):.0f}% of samples")


if __name__ == "__main__":
    main()
