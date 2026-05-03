#!/usr/bin/env python3
"""Generate character skin textures for voxel models.

Each pixel in the skin texture corresponds to a voxel at grid position (x, y).
The color of that pixel determines the color of all visible faces of that voxel.

Usage:
    python3 tools/gen_skin.py --type skeleton --out assets/textures/skeleton_skin_42.png
    python3 tools/gen_skin.py --type cleric   --out assets/textures/cleric_skin_42.png
    python3 tools/gen_skin.py --list
"""

import argparse
import struct
import zlib
import os
import math


def write_png(path, width, height, pixels):
    """Write a minimal RGBA PNG file without any external libraries."""

    def make_chunk(chunk_type, data):
        raw = chunk_type + data
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + raw + struct.pack('>I', crc)

    # IHDR
    ihdr_data = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    ihdr = make_chunk(b'IHDR', ihdr_data)

    # IDAT — raw pixel rows with filter byte 0 (none) prepended
    raw_rows = b''
    for y in range(height):
        raw_rows += b'\x00'  # filter: none
        for x in range(width):
            idx = (y * width + x) * 4
            raw_rows += bytes(pixels[idx:idx+4])
    compressed = zlib.compress(raw_rows)
    idat = make_chunk(b'IDAT', compressed)

    # IEND
    iend = make_chunk(b'IEND', b'')

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')  # PNG signature
        f.write(ihdr)
        f.write(idat)
        f.write(iend)


# ---------------------------------------------------------------------------
# Skin definitions — each is a dict mapping (gx, gy) -> (r, g, b, a)
# The coordinate system matches gen_mesh.py: gx = left-right, gy = bottom-top
# ---------------------------------------------------------------------------

def skin_skeleton():
    """Bone-white skeleton with dark eye sockets and grey joints."""
    w, h = 10, 16
    pixels = {}

    bone = (220, 210, 190, 255)
    dark_bone = (180, 170, 150, 255)
    eye = (30, 10, 10, 255)        # dark red-black eye sockets
    jaw = (200, 190, 170, 255)
    rib = (190, 180, 160, 255)

    # Default: bone color for everything
    for gy in range(h):
        for gx in range(w):
            pixels[(gx, gy)] = bone

    # Skull (y=12-15, x=-2..2 -> mapped to x=0..4 after offset)
    # Eyes at y=14
    pixels[(1, 14)] = eye  # left eye (-1 in grid = 1 after offset by 2)
    pixels[(3, 14)] = eye  # right eye
    # Nose
    pixels[(2, 13)] = dark_bone
    # Jaw
    for gx in range(1, 4):
        pixels[(gx, 11)] = jaw

    # Ribs (y=6-9)
    for gy in [6, 7, 9]:
        for gx in range(5):
            pixels[(gx, gy)] = rib

    # Joints darker
    for gy in [4, 3, 0]:  # pelvis, knees, feet
        for gx in range(5):
            pixels[(gx, gy)] = dark_bone

    return w, h, pixels


def skin_spider():
    """Dark brown/black spider with red eye spots."""
    w, h = 10, 10
    pixels = {}

    body = (50, 35, 25, 255)
    abdomen = (60, 40, 30, 255)
    eye = (200, 20, 20, 255)
    fang = (180, 160, 140, 255)

    for gy in range(h):
        for gx in range(w):
            pixels[(gx, gy)] = body

    # Abdomen lighter (upper area)
    for gy in range(5, 10):
        for gx in range(w):
            pixels[(gx, gy)] = abdomen

    # Eyes
    pixels[(2, 8)] = eye
    pixels[(4, 8)] = eye

    # Fangs
    pixels[(2, 0)] = fang
    pixels[(4, 0)] = fang

    return w, h, pixels


def skin_bat():
    """Dark purple-brown bat with bright yellow eyes."""
    w, h = 6, 14
    pixels = {}

    fur = (60, 45, 55, 255)
    ear = (70, 50, 60, 255)
    eye = (255, 220, 50, 255)    # bright yellow
    snout = (80, 55, 65, 255)
    wing = (45, 35, 45, 255)

    for gy in range(h):
        for gx in range(w):
            pixels[(gx, gy)] = fur

    # Eyes (y=8)
    pixels[(1, 8)] = eye
    pixels[(3, 8)] = eye

    # Ears (y=9-11)
    for gy in [9, 10, 11]:
        pixels[(1, gy)] = ear
        pixels[(3, gy)] = ear

    # Snout
    pixels[(2, 6)] = snout
    pixels[(2, 7)] = snout

    return w, h, pixels


def skin_human():
    """Generic human NPC — tan skin, brown clothing."""
    w, h = 10, 17
    pixels = {}

    skin = (210, 170, 130, 255)
    hair = (80, 60, 40, 255)
    eye = (50, 100, 160, 255)     # blue
    mouth = (180, 120, 100, 255)
    armor = (100, 90, 80, 255)
    belt = (70, 60, 50, 255)
    boot = (60, 50, 40, 255)
    pants = (80, 75, 70, 255)

    for gy in range(h):
        for gx in range(w):
            if gy >= 13:
                pixels[(gx, gy)] = skin    # head
            elif gy >= 11:
                pixels[(gx, gy)] = skin    # neck
            elif gy >= 6:
                pixels[(gx, gy)] = armor   # torso
            elif gy >= 5:
                pixels[(gx, gy)] = belt
            elif gy >= 2:
                pixels[(gx, gy)] = pants
            elif gy >= 1:
                pixels[(gx, gy)] = boot
            else:
                pixels[(gx, gy)] = boot

    # Hair
    pixels[(1, 16)] = hair
    pixels[(2, 16)] = hair
    pixels[(3, 16)] = hair

    # Eyes
    pixels[(1, 14)] = eye
    pixels[(3, 14)] = eye

    # Mouth
    pixels[(2, 12)] = mouth

    return w, h, pixels


def skin_cleric():
    """Male cleric — blonde hair, blue eyes, white/gold robes."""
    w, h = 10, 17
    pixels = {}

    skin = (215, 180, 145, 255)
    hair = (230, 200, 120, 255)   # blonde
    eye = (50, 80, 200, 255)      # blue
    mouth = (185, 130, 110, 255)
    robe = (200, 190, 170, 255)   # off-white
    sash = (180, 150, 60, 255)    # gold sash
    boot = (80, 65, 50, 255)
    hood = (170, 160, 140, 255)

    for gy in range(h):
        for gx in range(w):
            if gy >= 16:
                pixels[(gx, gy)] = hair    # hair on top
            elif gy >= 13:
                pixels[(gx, gy)] = skin    # face
            elif gy >= 11:
                pixels[(gx, gy)] = hood    # hood/collar
            elif gy >= 5:
                pixels[(gx, gy)] = robe    # robes
            elif gy >= 4:
                pixels[(gx, gy)] = sash    # belt/sash
            elif gy >= 1:
                pixels[(gx, gy)] = robe    # lower robe
            else:
                pixels[(gx, gy)] = boot

    # Hair fringe
    for gx in range(5):
        pixels[(gx, 15)] = hair

    # Eyes — blue
    pixels[(1, 14)] = eye
    pixels[(3, 14)] = eye
    # Holy symbol on chest
    pixels[(3, 8)] = sash

    # Mouth
    pixels[(2, 12)] = mouth

    return w, h, pixels


def skin_archer():
    """Female archer — fox-red ponytail, green eyes, leather armor."""
    w, h = 8, 18
    pixels = {}

    skin = (200, 160, 120, 255)
    hair = (170, 80, 30, 255)     # fox-red/auburn
    eye = (40, 160, 60, 255)      # green
    mouth = (175, 120, 100, 255)
    leather = (120, 80, 45, 255)  # brown leather
    belt = (90, 65, 35, 255)
    quiver = (100, 75, 40, 255)
    boot = (75, 55, 35, 255)
    pants = (90, 80, 65, 255)

    for gy in range(h):
        for gx in range(w):
            if gy >= 15:
                pixels[(gx, gy)] = hair    # hair top
            elif gy >= 13:
                pixels[(gx, gy)] = skin    # face
            elif gy >= 11:
                pixels[(gx, gy)] = hair    # ponytail area
            elif gy >= 8:
                pixels[(gx, gy)] = leather # upper armor
            elif gy >= 6:
                pixels[(gx, gy)] = leather # lower armor
            elif gy >= 5:
                pixels[(gx, gy)] = belt
            elif gy >= 4:
                pixels[(gx, gy)] = pants   # hips
            elif gy >= 2:
                pixels[(gx, gy)] = pants   # legs
            else:
                pixels[(gx, gy)] = boot

    # Face
    pixels[(1, 14)] = eye   # green eyes
    pixels[(3, 14)] = eye
    pixels[(2, 12)] = mouth

    # Ponytail down the back (shown as hair color on the side)
    for gy in range(10, 15):
        pixels[(3, gy)] = hair

    # Quiver strap across chest
    pixels[(4, 9)] = quiver
    pixels[(4, 8)] = quiver
    pixels[(4, 7)] = quiver

    # Bust area slightly different shade
    for gx in range(1, 4):
        pixels[(gx, 8)] = (130, 90, 50, 255)

    return w, h, pixels


def skin_butcher():
    """The Butcher boss — dark red demon skin, black horns, glowing eyes."""
    w, h = 12, 22
    pixels = {}

    skin = (140, 35, 25, 255)     # dark red
    horn = (40, 30, 25, 255)      # near-black
    eye = (255, 200, 50, 255)     # glowing yellow
    mouth = (80, 20, 15, 255)     # dark red mouth
    chest = (160, 45, 30, 255)    # lighter red chest
    belt = (60, 40, 30, 255)
    hoof = (50, 35, 28, 255)
    fist = (120, 30, 20, 255)

    for gy in range(h):
        for gx in range(w):
            if gy >= 19:
                pixels[(gx, gy)] = horn    # horns
            elif gy >= 16:
                pixels[(gx, gy)] = skin    # head
            elif gy >= 14:
                pixels[(gx, gy)] = skin    # neck
            elif gy >= 8:
                pixels[(gx, gy)] = chest   # massive torso
            elif gy >= 7:
                pixels[(gx, gy)] = belt    # belt
            elif gy >= 3:
                pixels[(gx, gy)] = skin    # legs
            else:
                pixels[(gx, gy)] = hoof    # hooves

    # Glowing eyes
    pixels[(3, 18)] = eye
    pixels[(7, 18)] = eye
    # Deep eye sockets
    pixels[(3, 17)] = (60, 15, 10, 255)
    pixels[(7, 17)] = (60, 15, 10, 255)

    # Mouth snarl
    pixels[(4, 15)] = mouth
    pixels[(5, 15)] = mouth

    # Barrel chest protrusion
    for gx in range(3, 8):
        pixels[(gx, 10)] = (170, 50, 35, 255)
        pixels[(gx, 11)] = (170, 50, 35, 255)

    # Fists
    for gy in [4, 5]:
        pixels[(0, gy)] = fist
        pixels[(1, gy)] = fist
        pixels[(10, gy)] = fist
        pixels[(11, gy)] = fist

    return w, h, pixels


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

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
    """Generate a skin texture PNG for the given type."""
    if skin_type not in SKIN_TYPES:
        print(f"Unknown skin type: {skin_type}")
        return False

    default_file, gen_func = SKIN_TYPES[skin_type]
    if out_path is None:
        game_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        out_path = os.path.join(game_root, "assets", "textures", default_file)

    w, h, pixel_map = gen_func()

    # Build RGBA pixel array (top-to-bottom for PNG, so flip Y)
    pixel_data = [0] * (w * h * 4)
    for gy in range(h):
        for gx in range(w):
            r, g, b, a = pixel_map.get((gx, gy), (128, 128, 128, 255))
            # PNG row 0 = top = highest gy
            png_y = (h - 1) - gy
            idx = (png_y * w + gx) * 4
            pixel_data[idx] = r
            pixel_data[idx + 1] = g
            pixel_data[idx + 2] = b
            pixel_data[idx + 3] = a

    write_png(out_path, w, h, pixel_data)
    print(f"Wrote {out_path}  ({w}x{h} skin for {skin_type})")
    return True


def main():
    parser = argparse.ArgumentParser(description="Generate character skin textures.")
    parser.add_argument("--type", choices=list(SKIN_TYPES.keys()),
                        help="Skin type to generate.")
    parser.add_argument("--out", type=str, default=None,
                        help="Output PNG path.")
    parser.add_argument("--all", action="store_true",
                        help="Generate all skin types.")
    parser.add_argument("--list", action="store_true",
                        help="List available skin types.")

    args = parser.parse_args()

    if args.list:
        print("Available skin types:")
        for name, (fname, _) in SKIN_TYPES.items():
            print(f"  {name:12s} -> {fname}")
        return

    if args.all:
        for name in SKIN_TYPES:
            generate_skin(name)
        return

    if not args.type:
        parser.error("--type is required (or use --all / --list)")

    generate_skin(args.type, args.out)


if __name__ == "__main__":
    main()
