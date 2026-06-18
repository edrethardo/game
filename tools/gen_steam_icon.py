#!/usr/bin/env python3
"""Generate the Steam Shortcut Icon + App Icon for "Curse of the Dungeon Engine".

A SQUARE brand mark, composed natively for an icon (NOT a crop of the wide logo, which leaves the
title's black void in the middle): a large glowing dungeon ARCHWAY — the brand's core motif — with
the adventurer silhouetted in the hell-glow and two big torch flames on the pillars. Same palette as
the logo/capsules (dark stone, gold/red glow). No wordmark — text is illegible at 184 px; the bright
arch, the flames, and the warm doorway carry the identity at any size.

Outputs (into store/steam/):
    shortcut_icon.png  512x512               Steam "Shortcut Icon" slot (PNG must be 256 or 512).
                                             Opaque, so the App Icon Steam derives from it (which
                                             drops alpha to solid black) comes out clean.
    shortcut_icon.ico  256/128/64/32/16      Windows .ico alternative for the same slot.
    app_icon.jpg       184x184               Steam "App Icon" slot (184x184 JPG, no alpha).

Deterministic (fixed seed). Usage:  python3 tools/gen_steam_icon.py [--outdir DIR]
"""

import argparse
import math
import os
import random
import sys

try:
    from PIL import Image, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

# Reuse the cinematic trailer's hooded rogue sprite (gen_logo_walk.draw_rogue) so the icon's figure
# IS the playable rogue — single source of truth, no hand-redrawn silhouette.
import gen_logo_walk

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)

RENDER = 512          # render at full icon size, then downscale for the 184 px JPG
ROGUE_FIGH = 0.52 * RENDER   # rogue height (~0.24*size wide) — prominent, but glow still halos around it
random.seed(42)       # deterministic stone noise / embers


def _c(v):
    return max(0, min(255, int(v)))


def draw_icon_scene(img):
    """Square dungeon-archway emblem: stone arch + glowing doorway + backlit hero + two torches."""
    W, H = img.size
    px = img.load()
    cx = W / 2.0
    springY = 0.46 * H            # where the arch curve meets the vertical pillars
    R_out = 0.32 * H              # arch/pillar outer radius
    R_in = 0.205 * H              # doorway inner radius (opening half-width)
    floorY = 0.94 * H             # dungeon floor line
    openTopY = springY - R_in     # inner crown of the arch

    # --- Stone arch + pillars, dark background, and the warm doorway opening ---
    for y in range(H):
        for x in range(W):
            fx = x - cx
            d_arch = math.hypot(fx, y - springY)
            arch_stone = (y <= springY and R_in <= d_arch <= R_out)
            pillar = (y > springY and y <= floorY and R_in <= abs(fx) <= R_out)
            open_arch = (y <= springY and d_arch < R_in)
            open_door = (y > springY and y <= floorY and abs(fx) < R_in)

            if arch_stone or pillar:
                base = 60 + random.randint(-13, 13)
                # darker toward the stone's inner/outer edges for chiseled depth
                if pillar:
                    e = min(abs(fx) - R_in, R_out - abs(fx))
                    if e < 0.02 * H:
                        base *= 0.55
                else:
                    e = min(d_arch - R_in, R_out - d_arch)
                    if e < 0.02 * H:
                        base *= 0.6
                px[x, y] = (_c(base), _c(base), _c(base + 5), 255)
            elif open_arch or open_door:
                # doorway: dark at the top, brightening to a fiery glow at the floor
                t = (y - openTopY) / max(1.0, (floorY - openTopY))
                t = max(0.0, min(1.0, t)) ** 1.6
                px[x, y] = (_c(12 + t * 245), _c(7 + t * 115), _c(12 + t * 30), 255)
            else:
                tb = y / H
                base = 11 + int(tb * 12) + random.randint(-4, 4)
                px[x, y] = (_c(base), _c(base - 2), _c(base + 5), 255)

    # --- Radial hell-glow bloom rising from the doorway base (additive) ---
    gx, gy, gmax = cx, floorY, 0.55 * H
    for y in range(int(0.42 * H), H):
        for x in range(W):
            d = math.hypot(x - gx, y - gy)
            if d < gmax:
                t = (1.0 - d / gmax) ** 2
                inten = t * 0.55
                p = px[x, y]
                px[x, y] = (_c(p[0] + 225 * inten), _c(p[1] + 95 * inten), _c(p[2] + 25 * inten), 255)

    # The hooded trailer rogue, standing in the doorway (static pose: walk_phase=0 → feet together,
    # look=0 → eyes glow forward + dagger raised). feet_y/fig_h place + size it to fill the opening.
    gen_logo_walk.draw_rogue(img, cx, walk_phase=0.0, look=0.0, feet_y=floorY, fig_h=ROGUE_FIGH)

    for side in (-1, 1):
        tx = cx + side * ((R_in + R_out) / 2.0)
        _draw_torch(img, int(tx), int(0.52 * H), H)

    _vignette(img)


def _draw_torch(img, tx, ty, H):
    """Wall bracket + a big layered teardrop flame (gold core → red tip) with a few embers."""
    px = img.load()
    W, Himg = img.size
    for dy in range(0, int(0.07 * H)):          # bracket/handle
        for dx in range(-2, 3):
            x, y = tx + dx, ty + dy
            if 0 <= x < W and 0 <= y < Himg:
                px[x, y] = (74, 52, 28, 255)
    # flame layers — draw largest (outer) first so the bright core lands on top
    layers = [(0.10 * H, (190, 38, 10)), (0.078 * H, (255, 90, 22)),
              (0.056 * H, (255, 150, 34)), (0.034 * H, (255, 224, 96))]
    for fr, fc in layers:
        fr = int(fr)
        fyo = -fr - int(0.015 * H)
        for dy in range(-fr, fr + 1):
            for dx in range(-fr, fr + 1):
                # teardrop: circular, but pinch the top so it tapers to a point
                rr = fr * (0.78 if dy < 0 else 1.0)
                if dx * dx + dy * dy <= rr * rr:
                    x, y = tx + dx, ty + fyo + dy
                    if 0 <= x < W and 0 <= y < Himg:
                        px[x, y] = fc + (255,)
    for _ in range(8):                          # embers
        ex = tx + random.randint(-int(0.05 * H), int(0.05 * H))
        ey = ty - random.randint(int(0.10 * H), int(0.22 * H))
        if 0 <= ex < W and 0 <= ey < Himg:
            px[ex, ey] = (255, 205, 90, 255)


def _vignette(img):
    """Soft circular darkening at the edges so the icon reads as a self-contained mark."""
    W, H = img.size
    px = img.load()
    for y in range(H):
        for x in range(W):
            dx = (x - W / 2) / (W / 2)
            dy = (y - H / 2) / (H / 2)
            v = max(0.0, 1.0 - (dx * dx + dy * dy) * 0.55)
            if v < 1.0:
                p = px[x, y]
                px[x, y] = (int(p[0] * v), int(p[1] * v), int(p[2] * v), 255)


def main():
    ap = argparse.ArgumentParser(description="Generate the Steam shortcut + app icons.")
    ap.add_argument("--outdir", default=os.path.join(ROOT_DIR, "store", "steam"))
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    img = Image.new("RGBA", (RENDER, RENDER), (0, 0, 0, 255))
    draw_icon_scene(img)
    img = img.filter(ImageFilter.GaussianBlur(0.4))   # melt the per-pixel noise a touch

    png = img.resize((512, 512), Image.LANCZOS).convert("RGB")
    png_path = os.path.join(args.outdir, "shortcut_icon.png")
    ico_path = os.path.join(args.outdir, "shortcut_icon.ico")
    jpg_path = os.path.join(args.outdir, "app_icon.jpg")
    png.save(png_path)
    png.save(ico_path, sizes=[(256, 256), (128, 128), (64, 64), (32, 32), (16, 16)])
    img.resize((184, 184), Image.LANCZOS).convert("RGB").save(jpg_path, quality=92)

    for p in (png_path, ico_path, jpg_path):
        print(f"  {p}")
    print("Done.")


if __name__ == "__main__":
    main()
