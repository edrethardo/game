#!/usr/bin/env python3
"""mark_cleared.py — grandfather a pre-town save into the cleared state.

Players who killed the Dungeon Engine BEFORE the town/cleared-marker existed never got
flagged (the old ending returned to the menu without writing anything), so their Continue
can't land in the town. There is no on-disk record of past Engine kills to migrate from —
this tool sets the marker on the honor system.

It patches exactly two header bytes of save_NN.dat (floor -> 51, difficulty -> 2 = the
FreePlay::saveCleared marker) and touches NOTHING else — the character's HP/inventory/
skills live in the per-player blocks after the header. A timestamped backup is written
beside the save first.

Usage:  python3 tools/mark_cleared.py <slot 1-20> [--save-dir <dir>]
        (default save dir: the SDL pref path ~/.local/share/EdRethardo/DungeonEngine,
         falling back to the snap-confined variant if present)
"""
import argparse
import os
import shutil
import struct
import sys
import time

SAVE_VERSION   = 3
CLEARED_FLOOR  = 51   # FreePlay::saveCleared: difficulty >= 2 && floor > 50
HELL           = 2

def default_save_dir():
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, ".local/share/EdRethardo/DungeonEngine"),
    ]
    # Snap-confined installs (e.g. running through the snap VS Code) get their own HOME.
    snap_root = os.path.join(home, "snap")
    if os.path.isdir(snap_root):
        for entry in sorted(os.listdir(snap_root)):
            candidates.append(os.path.join(snap_root, entry))
    best = None
    for c in candidates:
        for root, _dirs, files in os.walk(c):
            if root.endswith("EdRethardo/DungeonEngine") and any(
                    f.startswith("save_") for f in files):
                best = root
    return best

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("slot", type=int, help="save slot 1-20")
    ap.add_argument("--save-dir", default=None)
    args = ap.parse_args()
    if not (1 <= args.slot <= 20):
        sys.exit("slot must be 1-20")

    d = args.save_dir or default_save_dir()
    if not d:
        sys.exit("could not locate the save directory — pass --save-dir explicitly")
    path = os.path.join(d, f"save_{args.slot:02d}.dat")
    if not os.path.isfile(path):
        sys.exit(f"no save at {path}")

    b = bytearray(open(path, "rb").read())
    ver = struct.unpack_from("<I", b, 0)[0]
    if ver != SAVE_VERSION:
        sys.exit(f"save version {ver} != {SAVE_VERSION} — refusing to touch it")
    floor, players, diff = b[4], b[5], b[16]
    print(f"{path}\n  before: floor={floor} players={players} difficulty={diff}")
    if diff >= HELL and floor > 50:
        print("  already carries the cleared marker — nothing to do")
        return

    backup = f"{path}.backup-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, backup)
    print(f"  backup: {backup}")

    b[4]  = CLEARED_FLOOR
    b[16] = HELL
    with open(path, "wb") as f:
        f.write(b)
    print(f"  after:  floor={CLEARED_FLOOR} difficulty={HELL} — cleared. "
          "Continue now lands this hero in the town.")

if __name__ == "__main__":
    main()
