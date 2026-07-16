#!/usr/bin/env python3
"""Generate 16x16 item icon silhouettes as a C++ header of packed bitmasks.

The inventory item icons in src/renderer/item_icons.cpp are 16 rows of u16, where bit 15 is
the leftmost pixel (the `s_iconData` table). Most are still authored inline there; this tool
generates the icons we want produced FROM the asset pipeline — so CI/CD rebuilds them via
tools/build_assets.py instead of them living as hand-edited bitmaps — and emits each as an
`ITEM_ICON_<NAME>_ROWS` macro that the inline table splices into its array.

Output: src/renderer/item_icons_gen.h  (mirrors gen_skill_icons.py's generated-header pattern)
"""

import os

W = H = 16


def grid():
    return [[0] * W for _ in range(H)]


def px(g, x, y, v=1):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = v


def rect(g, x0, y0, x1, y1, v=1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            g[y][x] = v


def ring(g, cx, cy, r_out, r_in, v=1):
    """Fill an annulus (ring with a hole) — r_in <= dist <= r_out."""
    for y in range(H):
        for x in range(W):
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if r_in * r_in <= d2 <= r_out * r_out:
                g[y][x] = v


def draw_gloves():
    """An OPEN-HAND glove: four separated fingers pointing up, a thumb out to the left, and a
    cuff at the wrist. The gaps between the fingers give a comb-like top edge that no blade has
    — the decisive feature that stops it reading like the cleaver (a solid wide-top taper)."""
    g = grid()
    # Four separated fingers (1px wide, 1px gaps at x=5,7,9) — the unmistakable "hand" tell
    for fx in (4, 6, 8, 10):
        rect(g, fx, 1, fx, 4)
    # Knuckles: fingers merge into the back of the hand
    rect(g, 4, 5, 10, 6)
    # Palm
    rect(g, 4, 6, 10, 9)
    # Thumb sticking out to the LEFT, clearly off the finger row
    rect(g, 1, 6, 3, 7)
    rect(g, 2, 8, 3, 8)
    # Wrist narrows
    rect(g, 5, 10, 9, 10)
    # Flared cuff band at the bottom + base trim
    rect(g, 3, 11, 11, 12)
    rect(g, 4, 13, 10, 13)
    return g


def draw_chakram():
    """A bladed throwing ring: a thick metal annulus (hole in the middle) with four sharp blade
    points at N/E/S/W. The central hole + outward spikes read clearly as a chakram and are
    distinct from the thin RING item-icon outline."""
    g = grid()
    cx = cy = 8
    # Thick ring body with an open center
    ring(g, cx, cy, 6.6, 3.4)
    # Four blade points jutting out past the rim (cardinal directions)
    rect(g, cx, 0, cx, 2)      # north
    rect(g, cx, 13, cx, 15)    # south
    rect(g, 0, cy, 2, cy)      # west
    rect(g, 13, cy, 15, cy)    # east
    # Small diagonal nubs for a serrated look
    for (dx, dy) in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
        px(g, cx + dx * 6, cy + dy * 6)
    return g


def draw_pistol():
    """Compact handgun, side profile, muzzle right. A thin horizontal slide/barrel up top, a short
    grip raked down-back, and an open trigger guard between them. The defining read: SHORT barrel +
    chunky grip + guard hole. Kept small so it's clearly the most compact of the four guns."""
    g = grid()
    # Slide / barrel: 2px-tall horizontal bar, short.
    rect(g, 5, 5, 12, 6)
    px(g, 13, 5)                       # muzzle tip (front sight)
    rect(g, 6, 4, 8, 4)                # rear-sight hump on the slide
    # Frame under the rear of the slide.
    rect(g, 5, 7, 9, 7)
    # Grip — raked down and back from the rear.
    rect(g, 4, 7, 6, 11)
    rect(g, 3, 9, 5, 12)               # rake/back-lean + heel
    # Open trigger guard (hollow center) just in front of the grip.
    rect(g, 7, 8, 10, 8)               # guard top + trigger bar
    px(g, 7, 9); px(g, 10, 9)          # guard sides
    rect(g, 7, 10, 10, 10)             # guard bottom  (hollow at 8-9,9)
    return g


def draw_revolver():
    """Revolver, side profile, muzzle right. A thin barrel, a round CYLINDER bulge behind it (the
    unmistakable tell), and a curved grip. The fat cylinder + thin barrel separate it from the
    boxy pistol/SMG."""
    g = grid()
    rect(g, 9, 5, 15, 6)               # thin barrel (long, toward the muzzle)
    px(g, 7, 4)                        # hammer spur
    ring(g, 6, 6, 3.2, 0.0)            # cylinder — a solid round bulge behind the barrel
    rect(g, 4, 8, 7, 9)                # frame under the cylinder
    # Curved grip, swept down-back from the frame.
    rect(g, 3, 9, 5, 11)
    rect(g, 2, 11, 4, 13)
    # Small trigger guard.
    rect(g, 6, 9, 8, 9)
    px(g, 8, 10)
    return g


def draw_smg():
    """Submachine gun, side profile, muzzle right. A slim barrel + a slim 2px receiver, a stubby
    grip and a separate curved MAGAZINE jutting down ahead of it (the tell), plus a short rear
    stock. A clear gap between grip and magazine keeps the bottom from blobbing."""
    g = grid()
    rect(g, 9, 5, 14, 5)               # slim barrel toward the muzzle
    px(g, 15, 5)                       # muzzle tip
    rect(g, 3, 4, 9, 5)                # slim 2px receiver
    px(g, 5, 3); px(g, 6, 3)           # charging-handle / sight bump
    rect(g, 1, 4, 2, 5); px(g, 0, 5)   # short rear stock
    rect(g, 3, 6, 4, 9)                # stubby pistol grip (rear)
    px(g, 5, 6)                        # trigger nub  (gap at x5 below = separates grip from mag)
    rect(g, 6, 6, 7, 9)                # magazine upper
    rect(g, 7, 9, 8, 12)               # magazine lower (curves forward = banana mag)
    return g


def draw_carbine():
    """Carbine/rifle, side profile, muzzle right. The LONGEST gun: a full-length thin barrel with a
    front sight, a slim receiver, an angled shoulder STOCK at the rear (the tell vs the SMG), a
    pistol grip and a straight box magazine."""
    g = grid()
    rect(g, 8, 4, 15, 4)               # long thin barrel to the muzzle
    px(g, 11, 3)                       # front sight post
    rect(g, 3, 4, 8, 5)                # slim receiver
    px(g, 4, 3); px(g, 5, 3)           # rear sight / carry handle
    # Angled shoulder stock at the very rear (comb on top, heel dropping back-down).
    rect(g, 0, 4, 2, 4)
    rect(g, 0, 5, 1, 6); px(g, 2, 5)
    rect(g, 4, 6, 5, 9)                # pistol grip
    rect(g, 6, 6, 7, 10)               # straight box magazine
    return g


def draw_infinity():
    """Infinity Chakram icon — two linked hollow rings forming an ∞ symbol (distinct from the
    single-ring CHAKRAM icon)."""
    g = grid()
    ring(g, 5, 8, 3.8, 1.6)   # left loop (hollow)
    ring(g, 10, 8, 3.8, 1.6)  # right loop (overlaps the left at the center → linked ∞)
    return g


def draw_goblin():
    """Mini Loot Goblin consumable — a grinning goblin head with big pointy ears. The ears +
    eye/mouth holes make it unmistakable at 16px, and nothing else in the atlas is a face —
    the whole point: the pet item must not read as 'just another ring' in a full backpack."""
    g = grid()
    # Big pointy ears sweeping up and out (goblin tell #1)
    for i in range(4):                       # left ear: rises right-to-left to a point
        rect(g, 1 + i, 5 - i, 2 + i, 6 - i)
    for i in range(4):                       # right ear: mirrored
        rect(g, 13 - i, 5 - i, 14 - i, 6 - i)
    # Head: wide dome tapering to a pointed chin
    rect(g, 4, 3, 11, 4)                     # crown
    rect(g, 3, 5, 12, 9)                     # face block (ears merge into this)
    rect(g, 4, 10, 11, 11)                   # cheeks
    rect(g, 5, 12, 10, 12)                   # jaw
    rect(g, 6, 13, 9, 13)                    # pointed chin
    # Eyes: two slanted holes (unlit pixels inside the fill)
    for ex in (5, 9):
        px(g, ex, 6, 0); px(g, ex + 1, 6, 0)
        px(g, ex + 1, 7, 0)
    # Wide toothy grin: a mouth slot with alternating teeth
    for x in range(5, 11):
        px(g, x, 10, 0)
    for x in (6, 8, 10):                     # teeth interrupt the mouth hole
        px(g, x, 10, 1)
    return g


def draw_paw():
    """Generic pet consumable ("Mini <Enemy>" companions) — a paw print: three toe pads over a
    big palm pad. The goblin keeps its face icon (it's THE jackpot); every other pet shares the
    paw, which reads as 'creature' without needing 38 per-enemy glyphs in a 16 px atlas."""
    g = grid()
    # Three toe pads arched over the palm (round-ish 3x3 blobs)
    for cx, cy in ((3, 4), (8, 2), (13, 4)):
        rect(g, cx - 1, cy, cx + 1, cy + 2)
        px(g, cx, cy - 1); px(g, cx, cy + 3)
    # Big palm pad: a fat rounded blob, wider than tall
    rect(g, 4, 9, 11, 13)
    rect(g, 3, 10, 12, 12)
    rect(g, 5, 8, 10, 8)
    rect(g, 5, 14, 10, 14)
    return g


def pack(g):
    """Pack a 16x16 boolean grid into 16 u16 rows (bit 15 = leftmost pixel)."""
    rows = []
    for y in range(H):
        bits = 0
        for x in range(W):
            if g[y][x]:
                bits |= 1 << (15 - x)
        rows.append(bits)
    return rows


def write_header(icons, path):
    with open(path, "w") as f:
        f.write("// Auto-generated by tools/gen_item_icons.py — do not edit manually.\n")
        f.write("// 16x16 item icon silhouettes, packed u16-per-row (bit 15 = leftmost pixel).\n")
        f.write("// Spliced into the s_iconData table in item_icons.cpp via ITEM_ICON_*_ROWS.\n")
        f.write("#pragma once\n\n")
        for name, rows in icons:
            vals = ", ".join(f"0x{v:04X}" for v in rows)
            f.write(f"#define ITEM_ICON_{name}_ROWS {vals}\n")
    print(f"Generated {len(icons)} item icon(s) to {path}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(repo_root, "src", "renderer", "item_icons_gen.h")
    icons = [
        ("GLOVES", pack(draw_gloves())),
        ("CHAKRAM", pack(draw_chakram())),
        ("PISTOL", pack(draw_pistol())),
        ("SMG", pack(draw_smg())),
        ("CARBINE", pack(draw_carbine())),
        ("REVOLVER", pack(draw_revolver())),
        ("INFINITY", pack(draw_infinity())),
        ("GOBLIN", pack(draw_goblin())),
        ("PAW", pack(draw_paw())),
    ]
    write_header(icons, out_path)


if __name__ == "__main__":
    main()
