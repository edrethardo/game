#!/usr/bin/env python3
"""Generate character skin textures for voxel models.

Simple system: one pixel per (gx, gy) voxel column. All faces of voxels at
the same (gx, gy) share the same color. Eye colors are muted so they look
acceptable from all viewing angles (front and back).

Usage:
    python3 tools/gen_skin.py --type skeleton --out assets/textures/skeleton_skin_42.png
    python3 tools/gen_skin.py --all
    python3 tools/gen_skin.py --list
"""

import argparse
import struct
import zlib
import os


def write_png(path, width, height, pixels):
    """Write a minimal RGBA PNG file."""
    def make_chunk(ct, data):
        raw = ct + data
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + raw + struct.pack('>I', crc)
    ihdr = make_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    raw = b''
    for y in range(height):
        raw += b'\x00'
        for x in range(width):
            i = (y * width + x) * 4
            raw += bytes(pixels[i:i+4])
    idat = make_chunk(b'IDAT', zlib.compress(raw))
    iend = make_chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + ihdr + idat + iend)


# ---------------------------------------------------------------------------
# Skin definitions — (gx, gy) -> (r, g, b, a)
# px = gx - min_gx, py = gy - min_gy
# Eye colors are muted because they show on all faces of the voxel.
# ---------------------------------------------------------------------------

def skin_skeleton():
    """Grid: x=[-3,3] (w=7), y=[0,15] (h=16). Offset: gx+3."""
    w, h = 7, 16
    p = {}
    bone = (225, 215, 195, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Skull lighter
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (235, 225, 210, 255)
    # Eye sockets — dark but not extreme (shows on all faces)
    p[(2, 14)] = (40, 20, 15, 255)
    p[(4, 14)] = (40, 20, 15, 255)
    # Nose
    p[(3, 13)] = (60, 40, 30, 255)
    # Jaw
    p[(2, 11)] = (240, 235, 220, 255)  # teeth
    p[(3, 11)] = (120, 100, 85, 255)   # mouth gap
    p[(4, 11)] = (240, 235, 220, 255)  # teeth
    # Skull sides
    p[(1, 13)] = (180, 170, 150, 255)
    p[(5, 13)] = (180, 170, 150, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (190, 180, 160, 255)
    p[(3, 10)] = (190, 180, 160, 255)
    # Ribs
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (210, 200, 180, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (60, 40, 35, 255)
    # Shoulders
    p[(0, 9)] = (150, 140, 120, 255)
    p[(6, 9)] = (150, 140, 120, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (170, 160, 140, 255)
        p[(6, py)] = (170, 160, 140, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (180, 170, 150, 255)
    # Legs
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (200, 190, 170, 255); p[(px, 1)] = (170, 160, 140, 255)
    return w, h, p


def skin_spider():
    """Grid: x=[-7,7] (w=15), y=[0,6] (h=7). Offset: gx+7. Eye stalks at gy=6."""
    w, h = 15, 7
    p = {}
    body = (50, 35, 25, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (65, 45, 32, 255)
    for px in range(5, 9): p[(px, 5)] = (80, 55, 38, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (55, 38, 28, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (60, 42, 30, 255)
    # Eye stalks at gy=6 (unique positions, no abdomen overlap) — bright red
    p[(6, 6)] = (220, 30, 15, 255)
    p[(8, 6)] = (220, 30, 15, 255)
    # Fangs
    p[(6, 0)] = (200, 185, 160, 255)
    p[(8, 0)] = (200, 185, 160, 255)
    # Legs
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (45, 30, 20, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (70, 50, 35, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (45, 30, 20, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (70, 50, 35, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (85, 60, 42, 255)
    return w, h, p


def skin_bat():
    """Grid: x=[-2,2] (w=5), y=[2,11] (h=10). Offset: gx+2, gy-2."""
    w, h = 5, 10
    p = {}
    fur = (55, 38, 48, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Lighter belly
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (70, 50, 60, 255)
    # Eyes — warm amber (looks like eyes from front, like fur markings from back)
    p[(1, 6)] = (180, 140, 40, 255)
    p[(3, 6)] = (180, 140, 40, 255)
    # Ears — pink
    p[(1, 8)] = (140, 80, 90, 255)
    p[(3, 8)] = (140, 80, 90, 255)
    p[(1, 9)] = (85, 55, 65, 255)
    p[(3, 9)] = (85, 55, 65, 255)
    # Snout
    p[(2, 4)] = (90, 60, 70, 255)
    p[(2, 5)] = (80, 50, 55, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (40, 28, 35, 255)
        p[(px, 1)] = (40, 28, 35, 255)
    # Shoulders
    p[(0, 2)] = (65, 45, 55, 255)
    p[(4, 2)] = (65, 45, 55, 255)
    return w, h, p


def skin_human():
    """Grid: approx w=9, h=17."""
    w, h = 9, 17
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 13: p[(px, py)] = (210, 170, 130, 255)    # skin
            elif py >= 11: p[(px, py)] = (210, 170, 130, 255)  # neck
            elif py >= 6: p[(px, py)] = (100, 90, 80, 255)     # armor
            elif py >= 5: p[(px, py)] = (70, 60, 50, 255)      # belt
            elif py >= 2: p[(px, py)] = (80, 75, 70, 255)      # pants
            else: p[(px, py)] = (60, 50, 40, 255)              # boots
    # Hair
    for px in range(1, 4): p[(px, 16)] = (80, 60, 40, 255)
    # Eyes
    p[(3, 14)] = (50, 100, 160, 255)
    p[(5, 14)] = (50, 100, 160, 255)
    return w, h, p


def skin_cleric():
    """Grid: approx w=9, h=17."""
    w, h = 9, 17
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 16: p[(px, py)] = (230, 200, 120, 255)    # blonde hair
            elif py >= 13: p[(px, py)] = (215, 180, 145, 255)  # skin
            elif py >= 11: p[(px, py)] = (170, 160, 140, 255)  # hood
            elif py >= 5: p[(px, py)] = (200, 190, 170, 255)   # robes
            elif py >= 4: p[(px, py)] = (180, 150, 60, 255)    # sash
            elif py >= 1: p[(px, py)] = (200, 190, 170, 255)   # lower robe
            else: p[(px, py)] = (80, 65, 50, 255)              # boots
    for px in range(5): p[(px, 15)] = (230, 200, 120, 255)     # fringe
    p[(3, 14)] = (50, 80, 200, 255)   # blue eyes
    p[(5, 14)] = (50, 80, 200, 255)
    p[(4, 8)] = (180, 150, 60, 255)   # holy symbol
    return w, h, p


def skin_archer():
    """Grid: approx w=6, h=17."""
    w, h = 6, 17
    p = {}
    hair = (170, 80, 30, 255)
    for py in range(h):
        for px in range(w):
            if py >= 15: p[(px, py)] = hair
            elif py >= 13: p[(px, py)] = (200, 160, 120, 255)  # skin
            elif py >= 11: p[(px, py)] = hair                   # hair area
            elif py >= 8: p[(px, py)] = (120, 80, 45, 255)     # leather
            elif py >= 6: p[(px, py)] = (120, 80, 45, 255)
            elif py >= 5: p[(px, py)] = (90, 65, 35, 255)      # belt
            elif py >= 2: p[(px, py)] = (90, 80, 65, 255)      # pants
            else: p[(px, py)] = (75, 55, 35, 255)              # boots
    p[(2, 14)] = (40, 160, 60, 255)   # green eyes
    p[(4, 14)] = (40, 160, 60, 255)
    # Quiver
    for py in range(7, 10): p[(4, py)] = (100, 75, 40, 255)
    return w, h, p


def skin_butcher():
    """Grid: approx w=11, h=21."""
    w, h = 11, 21
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 19: p[(px, py)] = (40, 30, 25, 255)       # horns
            elif py >= 16: p[(px, py)] = (140, 35, 25, 255)    # head
            elif py >= 14: p[(px, py)] = (140, 35, 25, 255)    # neck
            elif py >= 8: p[(px, py)] = (160, 45, 30, 255)     # chest
            elif py >= 7: p[(px, py)] = (60, 40, 30, 255)      # belt
            elif py >= 3: p[(px, py)] = (140, 35, 25, 255)     # legs
            else: p[(px, py)] = (50, 35, 28, 255)              # hooves
    # Eyes — warm orange-yellow (looks like markings from all angles)
    p[(4, 18)] = (200, 150, 40, 255)
    p[(7, 18)] = (200, 150, 40, 255)
    # Barrel chest
    for px in range(3, 8):
        p[(px, 10)] = (170, 50, 35, 255)
        p[(px, 11)] = (170, 50, 35, 255)
    # Fists
    fist = (120, 30, 20, 255)
    for py in [4, 5]:
        p[(0, py)] = fist; p[(1, py)] = fist
        p[(9, py)] = fist; p[(10, py)] = fist
    return w, h, p


SKIN_TYPES = {
    "skeleton": ("skeleton_skin_42.png", skin_skeleton),
    "spider":   ("spider_skin_42.png",   skin_spider),
    "bat":      ("bat_skin_42.png",      skin_bat),
    "human":    ("human_skin_42.png",    skin_human),
    "cleric":   ("cleric_skin_42.png",   skin_cleric),
    "archer":   ("archer_skin_42.png",   skin_archer),
    "butcher":  ("butcher_skin_42.png",  skin_butcher),
}


def generate_skin(skin_type, out_path=None):
    """Generate a skin texture PNG."""
    if skin_type not in SKIN_TYPES:
        print(f"Unknown skin type: {skin_type}")
        return False
    default_file, gen_func = SKIN_TYPES[skin_type]
    if out_path is None:
        game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out_path = os.path.join(game_root, "assets", "textures", default_file)

    w, h, pixel_map = gen_func()
    pixel_data = [0] * (w * h * 4)
    for py in range(h):
        for px in range(w):
            r, g, b, a = pixel_map.get((px, py), (128, 128, 128, 255))
            png_y = (h - 1) - py  # flip Y: gy=0 is bottom, PNG row 0 is top
            idx = (png_y * w + px) * 4
            pixel_data[idx] = r
            pixel_data[idx+1] = g
            pixel_data[idx+2] = b
            pixel_data[idx+3] = a

    write_png(out_path, w, h, pixel_data)
    print(f"Wrote {out_path}  ({w}x{h} skin for {skin_type})")
    return True


def main():
    parser = argparse.ArgumentParser(description="Generate character skin textures.")
    parser.add_argument("--type", choices=list(SKIN_TYPES.keys()))
    parser.add_argument("--out", type=str, default=None)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    if args.list:
        for name, (fname, _) in SKIN_TYPES.items():
            print(f"  {name:12s} -> {fname}")
        return
    if args.all:
        for name in SKIN_TYPES: generate_skin(name)
        return
    if not args.type:
        parser.error("--type is required (or use --all / --list)")
    generate_skin(args.type, args.out)


if __name__ == "__main__":
    main()
