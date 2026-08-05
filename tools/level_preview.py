#!/usr/bin/env python3
"""Preview generated levels as ASCII, without launching the game.

    tools/level_preview.py wilderness                 # 4 seeds of the overworld terrain
    tools/level_preview.py cavern --seeds 7 --size 48
    tools/level_preview.py wilderness --seeds 1,2,3 --side-by-side

Why this exists: the only way to look at a generated layout used to be booting the game and walking
around it. That is slow, not reproducible, and cannot answer the question you actually have while
authoring a layout — "did the anchor land inside a rock?", "is this seed a boring empty field?".
Authoring the overworld means judging many seeds quickly, so the loop has to be a command.

It shells out to the real engine generator (an env-gated case in the test binary), so what you see is
what ships. There is no second implementation to drift out of sync.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

STYLES = ["rooms", "cavern", "gauntlet", "hub", "vertical", "descent", "wilderness"]
BIN_CANDIDATES = ["build/tests/dungeon_tests", "build-rel/tests/dungeon_tests"]


def find_binary(explicit):
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"level_preview: no such binary: {explicit}")
        return explicit
    for c in BIN_CANDIDATES:
        if os.path.exists(c):
            return c
    sys.exit("level_preview: build the tests first (cmake --build build --target dungeon_tests)")


def render(binary, style, seed, size):
    """Return (header, [rows], census) for one generated level."""
    env = dict(os.environ, LEVEL_PREVIEW=f"{style}:{seed}:{size}")
    try:
        out = subprocess.run([binary, "-tc=*level preview*", "--no-skip"],
                             env=env, capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.SubprocessError) as e:
        sys.exit(f"level_preview: running {binary} failed: {e}")
    rows = [ln[1:-1] for ln in out.splitlines() if ln.startswith("|") and ln.endswith("|")]
    header = next((ln for ln in out.splitlines() if ln.startswith("=== ")), f"=== {style} {seed} ===")
    census = next((ln for ln in out.splitlines() if ln.startswith("cells=")), "")
    if not rows:
        sys.exit(f"level_preview: no map came back for {style}:{seed}:{size}.\n{out[-400:]}")
    return header, rows, census


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("style", choices=STYLES)
    ap.add_argument("--seeds", default="1,2,3,4",
                    help="comma-separated seeds, or N-M for a range (default 1,2,3,4)")
    ap.add_argument("--size", type=int, default=52, help="square grid edge (16-64, default 52)")
    ap.add_argument("--side-by-side", action="store_true",
                    help="lay maps out horizontally — best for spotting variety across seeds")
    ap.add_argument("--binary", default=None)
    args = ap.parse_args()

    if "-" in args.seeds and "," not in args.seeds:
        lo, hi = args.seeds.split("-", 1)
        seeds = list(range(int(lo), int(hi) + 1))
    else:
        seeds = [int(s) for s in args.seeds.split(",") if s.strip()]

    binary = find_binary(args.binary)
    maps = [render(binary, args.style, s, args.size) for s in seeds]

    if args.side_by_side:
        # Fit as many as the terminal allows; a squeezed map is unreadable, so this drops the
        # overflow rather than wrapping it into nonsense.
        width = shutil.get_terminal_size((120, 40)).columns
        per = max(1, (width + 2) // (args.size + 3))
        for chunk_start in range(0, len(maps), per):
            chunk = maps[chunk_start:chunk_start + per]
            print()
            print("   ".join(f"seed {seeds[chunk_start + i]:<{args.size - 5}}" for i in range(len(chunk))))
            for row in range(args.size):
                print("   ".join(m[1][row] if row < len(m[1]) else " " * args.size for m in chunk))
            for m in chunk:
                nums = re.search(r"solid=\d+ \((\d+)%\)", m[2])
                print(f"  {m[0].strip('= ')}  [{'solid ' + nums.group(1) + '%' if nums else ''}]")
    else:
        for header, rows, census in maps:
            print(f"\n{header}")
            for r in rows:
                print(f"|{r}|")
            print(census)


if __name__ == "__main__":
    main()
