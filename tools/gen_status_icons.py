#!/usr/bin/env python3
"""Generate the 8x8 status-effect icons for the HUD status bar as C++ header data.

Output: src/renderer/status_icons_data.h

The HUD status bar draws each active effect as an 8x8 pixel-art glyph inside a tinted box
(HUD::drawStatusIcons -> getStatusIcon/getStatusColors in src/renderer/hud_status.cpp). Each icon is
an 8x8 array of PALETTE INDICES: 0 = transparent (skipped by the renderer), 1-4 = the four shades of
that effect's palette. The palettes are emitted here too, so the art and its colours stay in one
place instead of drifting apart across two files.

Icons are authored as ASCII art below — one character per pixel, row 0 = TOP. That is the only
sane way to review an 8x8 glyph: you can see it in the source. `.` is transparent, `1`-`4` are
palette shades (1 darkest, 4 brightest).

Run via tools/build_assets.py (it invokes this), or directly:
    python3 tools/gen_status_icons.py
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
OUT = os.path.join(ROOT_DIR, "src", "renderer", "status_icons_data.h")

# --- The shrine buffs -------------------------------------------------------------------------
# Three shrines: POWER (+30% damage), SPEED (+25% move speed), VITALITY (+40% max HP).
# The palettes deliberately match Shrine::colorOf (game/shrine.h) — red / cyan / green — so the
# crystal you activated in the room, the diamond on the minimap and this icon are all the same
# colour. A player who learns "red = Power" must never be told otherwise by the HUD.

ICONS = [
    # --- Shrine of Power: an upright sword — pointed tip, NARROW crossguard.
    #     The guard was 6px wide and read as a medical cross next to the Vitality heart,
    #     which is the one thing a damage buff must not look like. ---
    ("ShrinePower", [
        "...4....",
        "...44...",
        "...44...",
        "...44...",
        "..2222..",
        "...33...",
        "...33...",
        "..111...",
    ], [
        (0.00, 0.00, 0.00),   # 0 unused (transparent)
        (0.45, 0.10, 0.08),   # 1 pommel (dark)
        (0.85, 0.30, 0.25),   # 2 crossguard
        (0.70, 0.20, 0.16),   # 3 grip
        (1.00, 0.45, 0.40),   # 4 blade (bright — the shrine's red)
    ]),

    # --- Shrine of Speed: a double chevron. Reads as "fast forward" instantly. ---
    ("ShrineSpeed", [
        "........",
        ".3..3...",
        ".33..33.",
        ".444.444",
        ".444.444",
        ".33..33.",
        ".3..3...",
        "........",
    ], [
        (0.00, 0.00, 0.00),
        (0.10, 0.30, 0.45),
        (0.20, 0.50, 0.70),
        (0.30, 0.65, 0.85),   # 3 chevron edge
        (0.45, 0.85, 1.00),   # 4 chevron core (the shrine's cyan)
    ]),

    # --- Shrine of Vitality: a heart. ---
    ("ShrineVitality", [
        ".33..33.",
        "34444443",
        "44444444",
        "44444444",
        ".444444.",
        "..4444..",
        "...44...",
        "........",
    ], [
        (0.00, 0.00, 0.00),
        (0.10, 0.35, 0.15),
        (0.20, 0.55, 0.25),
        (0.30, 0.70, 0.35),   # 3 outline
        (0.50, 1.00, 0.60),   # 4 body (the shrine's green)
    ]),

    # --- Static Charge (Capacitor Mail): a lightning bolt, electric blue. Matches the item's
    #     arc-blue tint (materials.json armor_capacitor) so the buff reads as "the armor". ---
    ("Capacitor", [
        "....44..",
        "...44...",
        "..443...",
        ".444444.",
        "...344..",
        "..44....",
        ".44.....",
        ".4......",
    ], [
        (0.00, 0.00, 0.00),
        (0.15, 0.30, 0.45),
        (0.30, 0.50, 0.70),
        (0.55, 0.80, 1.00),   # 3 arc fringe
        (0.75, 0.95, 1.00),   # 4 the bolt
    ]),

    # --- Shrine of Sorcery: a four-point arcane star — purple, matches Shrine::colorOf. ---
    ("ShrineSpell", [
        "...44...",
        "...44...",
        "..3443..",
        "44433444",
        "44433444",
        "..3443..",
        "...44...",
        "...44...",
    ], [
        (0.00, 0.00, 0.00),
        (0.30, 0.15, 0.45),
        (0.45, 0.25, 0.65),
        (0.60, 0.32, 0.85),   # 3 inner glow
        (0.78, 0.40, 1.00),   # 4 the star (the shrine's purple)
    ]),
]


def parse(rows, name):
    """ASCII art -> 8x8 palette indices, with the validation that makes a typo impossible to ship."""
    if len(rows) != 8:
        sys.exit(f"ERROR: icon '{name}' has {len(rows)} rows, expected 8")
    out = []
    for y, row in enumerate(rows):
        if len(row) != 8:
            sys.exit(f"ERROR: icon '{name}' row {y} is {len(row)} px wide, expected 8: {row!r}")
        vals = []
        for ch in row:
            if ch == '.':
                vals.append(0)
            elif ch in '1234':
                vals.append(int(ch))
            else:
                sys.exit(f"ERROR: icon '{name}' row {y}: bad pixel {ch!r} (use . or 1-4)")
        out.append(vals)
    if all(v == 0 for r in out for v in r):
        sys.exit(f"ERROR: icon '{name}' is entirely empty")
    return out


def main():
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// AUTO-GENERATED by tools/gen_status_icons.py — DO NOT EDIT BY HAND.")
    lines.append("// Edit the ASCII art in that script and re-run it (or tools/build_assets.py).")
    lines.append("//")
    lines.append("// 8x8 status-bar glyphs: 0 = transparent, 1-4 = palette shades. Consumed by")
    lines.append("// getStatusIcon/getStatusColors in hud_status.cpp.")
    lines.append("")
    lines.append('#include "core/types.h"')
    lines.append('#include "core/math.h"')
    lines.append("")

    for name, art, pal in ICONS:
        grid = parse(art, name)
        lines.append(f"// {name}")
        for row in art:
            lines.append(f"//   {row}")
        lines.append(f"static const u8 kIcon{name}[8][8] = {{")
        for row in grid:
            lines.append("    {" + ", ".join(str(v) for v in row) + "},")
        lines.append("};")
        lines.append(f"static const Vec3 kPal{name}[5] = {{")
        for (r, g, b) in pal:
            lines.append(f"    {{{r:.2f}f, {g:.2f}f, {b:.2f}f}},")
        lines.append("};")
        lines.append("")

    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  wrote {os.path.relpath(OUT, ROOT_DIR)} ({len(ICONS)} status icons)")


if __name__ == "__main__":
    main()
