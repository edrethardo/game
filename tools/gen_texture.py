#!/usr/bin/env python3
"""Generate pixel-art dungeon textures (Barony-style low-res).

Usage:
    python3 tools/gen_texture.py --type stone_wall --size 32 --seed 42 --palette dark_dungeon
    python3 tools/gen_texture.py --list-types
    python3 tools/gen_texture.py --list-palettes
"""

import argparse
import math
import os
import random
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

# ---------------------------------------------------------------------------
# Palettes
# ---------------------------------------------------------------------------

PALETTES = {
    "dark_dungeon": [
        (42, 42, 48),
        (58, 58, 64),
        (74, 74, 80),
        (90, 90, 96),
        (53, 53, 64),
    ],
    "warm_brick": [
        (139, 69, 19),
        (160, 82, 45),
        (181, 101, 29),
        (122, 59, 16),
        (107, 52, 16),
        (196, 118, 60),
    ],
    "cold_stone": [
        (64, 72, 88),
        (80, 88, 104),
        (96, 104, 120),
        (53, 64, 80),
        (74, 90, 106),
    ],
    "mossy": [
        (58, 90, 58),
        (74, 106, 74),
        (42, 74, 42),
        (90, 122, 90),
        (58, 74, 58),
    ],
    "bone_white": [
        (212, 200, 160),
        (224, 212, 176),
        (200, 188, 144),
        (236, 224, 200),
        (184, 172, 128),
    ],
    "skeleton_bone": [
        (200, 192, 160),
        (180, 172, 140),
        (160, 150, 120),
        (220, 210, 180),
        (140, 130, 105),
        (100, 90, 70),
    ],
    "spider_dark": [
        (40, 30, 20),
        (55, 40, 28),
        (70, 50, 35),
        (30, 22, 15),
        (50, 35, 22),
        (85, 60, 40),
    ],
    "bat_brown": [
        (60, 40, 30),
        (75, 50, 35),
        (50, 32, 22),
        (90, 60, 42),
        (40, 25, 18),
        (70, 45, 30),
    ],
}

# ---------------------------------------------------------------------------
# Texture type registry and default palette mapping
# ---------------------------------------------------------------------------

TEXTURE_TYPES = [
    "stone_wall",
    "stone_wall_moss",
    "brick_wall",
    "stone_floor",
    "wood_plank",
    "stone_ceiling",
    "metal_grate",
    "skeleton_skin",
    "spider_skin",
    "bat_skin",
]

DEFAULT_PALETTE = {
    "stone_wall": "dark_dungeon",
    "stone_wall_moss": "dark_dungeon",
    "brick_wall": "warm_brick",
    "stone_floor": "cold_stone",
    "wood_plank": "warm_brick",
    "stone_ceiling": "dark_dungeon",
    "metal_grate": "cold_stone",
    "skeleton_skin": "skeleton_bone",
    "spider_skin": "spider_dark",
    "bat_skin": "bat_brown",
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, int(v)))


def noise_color(base, amount=10):
    """Return *base* RGB tuple with per-channel random noise, as RGBA."""
    return (
        clamp(base[0] + random.randint(-amount, amount)),
        clamp(base[1] + random.randint(-amount, amount)),
        clamp(base[2] + random.randint(-amount, amount)),
        255,
    )


def lerp_color(a, b, t):
    """Linearly interpolate between two RGB(A) tuples, return RGBA."""
    return (
        clamp(a[0] + (b[0] - a[0]) * t),
        clamp(a[1] + (b[1] - a[1]) * t),
        clamp(a[2] + (b[2] - a[2]) * t),
        255,
    )


def darken(color, factor=0.6):
    return (clamp(int(color[0] * factor)),
            clamp(int(color[1] * factor)),
            clamp(int(color[2] * factor)),
            255)


def brighten(color, amount=20):
    return (clamp(color[0] + amount),
            clamp(color[1] + amount),
            clamp(color[2] + amount),
            255)


# ---------------------------------------------------------------------------
# Generators
# ---------------------------------------------------------------------------

MORTAR_COLOR = (20, 20, 22, 255)


def gen_stone_wall(img, size, palette):
    """Irregular stone blocks with mortar/crack lines.

    Divides image into ~3 columns x 4 rows of rectangular blocks with slight
    random offset at borders.  Each block is filled with a palette color plus
    per-pixel noise (+-8 RGB).  1px dark mortar lines at block borders.
    """
    px = img.load()

    # Fill background with mortar color
    for y in range(size):
        for x in range(size):
            px[x, y] = MORTAR_COLOR

    # Build ~4 rows of blocks
    num_rows = 4
    row_boundaries = [0]
    base_row_h = size / num_rows
    for i in range(1, num_rows):
        boundary = int(base_row_h * i) + random.randint(-1, 1)
        boundary = max(row_boundaries[-1] + 2, min(boundary, size - 2))
        row_boundaries.append(boundary)
    row_boundaries.append(size)

    for ri in range(len(row_boundaries) - 1):
        row_y0 = row_boundaries[ri]
        row_y1 = row_boundaries[ri + 1]

        # Build ~3 columns per row
        num_cols = 3
        col_boundaries = [0]
        base_col_w = size / num_cols
        for ci in range(1, num_cols):
            boundary = int(base_col_w * ci) + random.randint(-1, 1)
            boundary = max(col_boundaries[-1] + 2, min(boundary, size - 2))
            col_boundaries.append(boundary)
        col_boundaries.append(size)

        for ci in range(len(col_boundaries) - 1):
            col_x0 = col_boundaries[ci]
            col_x1 = col_boundaries[ci + 1]
            base = random.choice(palette)

            # Fill block interior (leave 1px mortar border on top and left)
            for by in range(row_y0 + 1, row_y1):
                for bx in range(col_x0 + 1, col_x1):
                    px[bx, by] = noise_color(base, 8)


def gen_stone_wall_moss(img, size, palette):  # noqa: ARG001 -- palette unused by design
    """Stone wall with green moss overlay.

    Generates stone_wall first using dark_dungeon palette, then overlays
    green patches at 5-8 random positions (radius 2-4px, blend 50% with
    mossy palette color).  The *palette* arg is ignored; both base and moss
    palettes are hardcoded per the texture spec.
    """
    base_pal = PALETTES["dark_dungeon"]
    gen_stone_wall(img, size, base_pal)

    px = img.load()
    moss_colors = PALETTES["mossy"]

    num_patches = random.randint(5, 8)
    for _ in range(num_patches):
        cx = random.randint(0, size - 1)
        cy = random.randint(0, size - 1)
        radius = random.randint(2, 4)
        moss_c = random.choice(moss_colors)

        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if dx * dx + dy * dy <= radius * radius:
                    nx = (cx + dx) % size
                    ny = (cy + dy) % size
                    old = px[nx, ny]
                    blended = lerp_color(old, noise_color(moss_c, 8), 0.5)
                    px[nx, ny] = blended


def gen_brick_wall(img, size, palette):
    """Regular horizontal brick pattern with thin mortar lines.

    Brick height = size//4, brick width = size//2. Even rows offset by half
    a brick width. 1px mortar lines in darkest palette color. Per-brick
    color from palette with +-5 noise.
    """
    px = img.load()
    brick_h = max(2, size // 4)
    brick_w = max(2, size // 2)

    # Darkest palette color for mortar
    mortar = min(palette, key=lambda c: c[0] + c[1] + c[2])
    mortar_rgba = (mortar[0], mortar[1], mortar[2], 255)

    # Fill mortar background
    for y in range(size):
        for x in range(size):
            px[x, y] = mortar_rgba

    row_idx = 0
    y = 0
    while y < size:
        offset = (brick_w // 2) if (row_idx % 2 == 1) else 0
        x = -offset
        while x < size:
            base = random.choice(palette)
            # Fill this brick (leave 1px mortar on top and left)
            for by in range(y + 1, min(y + brick_h, size)):
                for bx in range(max(0, x + 1), min(x + brick_w, size)):
                    px[bx, by] = noise_color(base, 5)
            x += brick_w
        y += brick_h
        row_idx += 1


def gen_stone_floor(img, size, palette):
    """Irregular flagstone via simple Voronoi.

    Places ~(size//4) random seed points. Each pixel gets the color of
    nearest seed point + +-5 noise. Dark border lines (1px) where the
    nearest and second-nearest seeds are close in distance (diff < size*0.1).
    """
    px = img.load()
    num_seeds = max(2, size // 4)
    border_threshold = size * 0.1

    # Generate Voronoi seed points
    seeds = [(random.randint(0, size - 1), random.randint(0, size - 1))
             for _ in range(num_seeds)]
    seed_colors = [random.choice(palette) for _ in range(num_seeds)]

    dark_border = (20, 20, 22, 255)

    for y in range(size):
        for x in range(size):
            # Find nearest and second-nearest seed
            best_dist = float("inf")
            second_dist = float("inf")
            best_idx = 0
            for i, (sx, sy) in enumerate(seeds):
                dx = x - sx
                dy = y - sy
                d = math.sqrt(dx * dx + dy * dy)
                if d < best_dist:
                    second_dist = best_dist
                    best_dist = d
                    best_idx = i
                elif d < second_dist:
                    second_dist = d

            # Check if this pixel is on a border
            if (second_dist - best_dist) < border_threshold:
                px[x, y] = dark_border
            else:
                px[x, y] = noise_color(seed_colors[best_idx], 5)


def gen_wood_plank(img, size, palette):
    """Horizontal wood planks with grain lines and knots.

    Each row has a base brown from palette. Horizontal grain variation
    (sinusoidal + noise). 2-3 knots at random positions (dark circles
    radius 1-2px). Brown palette.
    """
    px = img.load()

    plank_h = max(3, size // 5)

    # Collect knot positions to draw after planks
    knots = []

    y = 0
    while y < size:
        h = min(plank_h, size - y)
        base = random.choice(palette)

        # Sinusoidal grain parameters
        freq = random.uniform(0.3, 0.8)
        phase = random.uniform(0, 2 * math.pi)
        amp = random.randint(3, 8)

        for py_ in range(y, min(y + h, size)):
            for px_ in range(size):
                # Base color with sinusoidal grain variation
                grain = int(amp * math.sin(freq * px_ + phase + py_ * 0.1))
                c = (
                    clamp(base[0] + grain + random.randint(-3, 3)),
                    clamp(base[1] + grain + random.randint(-3, 3)),
                    clamp(base[2] + grain + random.randint(-3, 3)),
                    255,
                )
                px[px_, py_] = c

        # 2-3 knot marks per plank
        num_knots = random.randint(2, 3)
        for _ in range(num_knots):
            kx = random.randint(0, size - 1)
            ky = random.randint(y, min(y + h - 1, size - 1))
            kr = random.randint(1, 2)
            knots.append((kx, ky, kr, darken(base, 0.5)))

        y += h

    # Draw knots on top
    for kx, ky, kr, knot_c in knots:
        for dy in range(-kr, kr + 1):
            for dx in range(-kr, kr + 1):
                if dx * dx + dy * dy <= kr * kr:
                    nx = kx + dx
                    ny = ky + dy
                    if 0 <= nx < size and 0 <= ny < size:
                        px[nx, ny] = noise_color(knot_c[:3], 3)


def gen_stone_ceiling(img, size, palette):
    """Low-frequency noise, dark gray palette.

    Each pixel = random palette color with +-10 noise. Blends 2-3 dark
    colors per pixel.
    """
    px = img.load()
    for y in range(size):
        for x in range(size):
            # Blend 2-3 palette colors
            num_blend = random.randint(2, 3)
            colors = [random.choice(palette) for _ in range(num_blend)]
            r = sum(c[0] for c in colors) // num_blend
            g = sum(c[1] for c in colors) // num_blend
            b = sum(c[2] for c in colors) // num_blend
            px[x, y] = noise_color((r, g, b), 10)


def gen_metal_grate(img, size, palette):
    """Grid pattern: 2px wide bars with 4px gaps.

    Bars are lighter palette color, gaps are near-black. Intersections of
    bars are slightly brighter.
    """
    px = img.load()
    bar_width = 2
    gap_width = 4
    cell = bar_width + gap_width  # 6px period

    bar_color = max(palette, key=lambda c: c[0] + c[1] + c[2])
    gap_color = (10, 10, 12, 255)

    for y in range(size):
        for x in range(size):
            in_bar_x = (x % cell) < bar_width
            in_bar_y = (y % cell) < bar_width

            if in_bar_x and in_bar_y:
                # Intersection -- slightly brighter
                px[x, y] = brighten(bar_color, 20)
            elif in_bar_x or in_bar_y:
                # Bar pixel
                px[x, y] = noise_color(bar_color, 5)
            else:
                # Gap -- near-black
                px[x, y] = gap_color


def gen_skeleton_skin(img, size, palette):
    """Boney skeleton texture — bone segments with dark crack lines.

    Horizontal bone segments with rib-like dark lines running across.
    Lighter bone color with darker joints/gaps for a skeletal look.
    """
    px = img.load()
    darkest = min(palette, key=lambda c: c[0] + c[1] + c[2])
    joint_color = (darkest[0], darkest[1], darkest[2], 255)

    # Fill with base bone color + noise
    for y in range(size):
        for x in range(size):
            px[x, y] = noise_color(random.choice(palette), 6)

    # Horizontal rib lines — dark gaps every ~size//5 pixels
    rib_spacing = max(3, size // 5)
    for y in range(0, size, rib_spacing):
        for x in range(size):
            px[x, y] = noise_color(darkest, 5)
            if y + 1 < size:
                px[x, y + 1] = darken(noise_color(darkest, 3), 0.8)

    # Vertical crack lines — 2-3 thin cracks
    for _ in range(random.randint(2, 3)):
        cx = random.randint(0, size - 1)
        for y in range(size):
            cx += random.randint(-1, 1)
            cx = cx % size
            px[cx, y] = joint_color

    # Eye sockets — two small dark circles near top
    if size >= 16:
        ey = size // 4
        for dx, dy in [(-1,0),(0,0),(1,0),(0,-1),(0,1)]:
            lx = (size // 3 + dx) % size
            rx = (2 * size // 3 + dx) % size
            ly = (ey + dy) % size
            px[lx, ly] = (15, 10, 8, 255)
            px[rx, ly] = (15, 10, 8, 255)


def gen_spider_skin(img, size, palette):
    """Dark hairy spider texture — dark browns/blacks with hair-like streaks.

    Very dark base with lighter hair-line streaks and occasional bright spots
    for a chitinous, hairy spider look.
    """
    px = img.load()

    # Fill dark base
    for y in range(size):
        for x in range(size):
            px[x, y] = noise_color(random.choice(palette), 5)

    # Hair-like vertical streaks — thin lighter lines
    for _ in range(size // 2):
        sx = random.randint(0, size - 1)
        streak_len = random.randint(3, size // 2)
        sy = random.randint(0, size - 1)
        lightest = max(palette, key=lambda c: c[0] + c[1] + c[2])
        for dy in range(streak_len):
            y = (sy + dy) % size
            sx = (sx + random.randint(-1, 1)) % size
            px[sx, y] = noise_color(lightest, 8)

    # Red accent spots — small hourglass/eye marks
    for _ in range(random.randint(2, 4)):
        rx = random.randint(0, size - 1)
        ry = random.randint(0, size - 1)
        px[rx, ry] = (160, 30, 20, 255)
        if rx + 1 < size:
            px[rx + 1, ry] = (140, 25, 18, 255)

    # Eight small eye dots near one edge
    if size >= 16:
        ey = 2
        for i in range(8):
            ex = size // 4 + i * (size // 16)
            if ex < size:
                px[ex % size, ey] = (80, 10, 10, 255)


def gen_bat_skin(img, size, palette):
    """Leathery bat wing texture — dark brown with membrane veins.

    Dark base with lighter vein lines radiating from one corner,
    giving a stretched-leather wing membrane look.
    """
    px = img.load()

    # Fill dark leathery base
    for y in range(size):
        for x in range(size):
            px[x, y] = noise_color(random.choice(palette), 5)

    # Wing membrane veins — lines radiating from top-left corner
    lightest = max(palette, key=lambda c: c[0] + c[1] + c[2])
    num_veins = random.randint(4, 7)
    for i in range(num_veins):
        angle = (math.pi * 0.5) * i / max(1, num_veins - 1)  # 0 to 90 degrees
        vein_len = int(size * 1.2)
        vx, vy = 0.0, 0.0
        dx = math.cos(angle)
        dy = math.sin(angle)
        for step in range(vein_len):
            ix = int(vx) % size
            iy = int(vy) % size
            px[ix, iy] = noise_color(lightest, 8)
            vx += dx
            vy += dy
            # Slight wobble
            vx += random.uniform(-0.3, 0.3)
            vy += random.uniform(-0.3, 0.3)

    # Fur patch near one edge (body area) — darker, denser
    fur_y = 0
    for y in range(min(4, size)):
        for x in range(size):
            darkest = min(palette, key=lambda c: c[0] + c[1] + c[2])
            px[x, y] = noise_color(darkest, 3)


GENERATORS = {
    "stone_wall": gen_stone_wall,
    "stone_wall_moss": gen_stone_wall_moss,
    "brick_wall": gen_brick_wall,
    "stone_floor": gen_stone_floor,
    "wood_plank": gen_wood_plank,
    "stone_ceiling": gen_stone_ceiling,
    "metal_grate": gen_metal_grate,
    "skeleton_skin": gen_skeleton_skin,
    "spider_skin": gen_spider_skin,
    "bat_skin": gen_bat_skin,
}

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description="Generate pixel-art dungeon textures."
    )
    parser.add_argument(
        "--type",
        dest="tex_type",
        choices=TEXTURE_TYPES,
        help="Texture type to generate.",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=32,
        help="Texture size in pixels (square). Default: 32.",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducibility. Default: 42.",
    )
    parser.add_argument(
        "--palette",
        choices=list(PALETTES.keys()),
        default=None,
        help="Color palette. Defaults depend on texture type.",
    )
    parser.add_argument(
        "--list-types",
        action="store_true",
        help="List available texture types and exit.",
    )
    parser.add_argument(
        "--list-palettes",
        action="store_true",
        help="List available palettes and exit.",
    )

    args = parser.parse_args()

    if args.list_types:
        print("Available texture types:")
        for t in TEXTURE_TYPES:
            print(f"  {t}  (default palette: {DEFAULT_PALETTE[t]})")
        return

    if args.list_palettes:
        print("Available palettes:")
        for name, colors in PALETTES.items():
            hex_list = ", ".join(f"#{r:02x}{g:02x}{b:02x}" for r, g, b in colors)
            print(f"  {name}: [{hex_list}]")
        return

    if args.tex_type is None:
        parser.error("--type is required (or use --list-types / --list-palettes)")

    palette_name = args.palette or DEFAULT_PALETTE[args.tex_type]
    palette = PALETTES[palette_name]

    random.seed(args.seed)

    img = Image.new("RGBA", (args.size, args.size))
    GENERATORS[args.tex_type](img, args.size, palette)

    # Output path: assets/textures/<type>_<seed>.png relative to repo root
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    out_dir = os.path.join(repo_root, "assets", "textures")
    os.makedirs(out_dir, exist_ok=True)

    filename = f"{args.tex_type}_{args.seed}.png"
    out_path = os.path.join(out_dir, filename)
    img.save(out_path)
    print(out_path)


if __name__ == "__main__":
    main()
