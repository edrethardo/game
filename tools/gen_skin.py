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


def skin_mage():
    """Grid: gx=-4..4 (w=9), gy=0..18 (h=19). Hat adds rows above 15."""
    w, h = 9, 19
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 16:
                p[(px, py)] = (60, 30, 100, 255)    # hat — deep purple
            elif py == 15:
                p[(px, py)] = (60, 40, 25, 255)     # hair under hat brim
            elif py >= 13:
                p[(px, py)] = (200, 175, 155, 255)  # skin (pale)
            elif py >= 11:
                p[(px, py)] = (70, 40, 110, 255)    # hood/collar — dark purple
            elif py >= 5:
                p[(px, py)] = (50, 35, 90, 255)     # robes — dark blue/purple
            elif py >= 4:
                p[(px, py)] = (160, 130, 50, 255)   # belt — gold
            elif py >= 1:
                p[(px, py)] = (50, 35, 90, 255)     # lower robe
            else:
                p[(px, py)] = (50, 40, 35, 255)     # boots
    # Arcane purple eyes: gx offsets relative to min_gx=-4, so px = gx+4
    # Left eye at gx=-1 -> px=3, right eye at gx=1 -> px=5
    p[(3, 14)] = (140, 60, 200, 255)
    p[(5, 14)] = (140, 60, 200, 255)
    return w, h, p


def skin_rogue():
    """Grid: gx=-3..2 (w=6), gy=0..16 (h=17). Hood adds rows above 14."""
    w, h = 6, 17
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 15:
                p[(px, py)] = (40, 35, 35, 255)    # hood — very dark gray
            elif py >= 13:
                p[(px, py)] = (195, 165, 130, 255)  # skin — slightly tanned
            elif py >= 11:
                p[(px, py)] = (50, 45, 45, 255)    # hood sides — dark gray
            elif py >= 8:
                p[(px, py)] = (60, 45, 30, 255)    # leather armor
            elif py >= 6:
                p[(px, py)] = (35, 30, 25, 255)    # black belt/narrow waist
            elif py >= 4:
                p[(px, py)] = (55, 50, 45, 255)    # pants — dark gray
            elif py >= 2:
                p[(px, py)] = (50, 45, 40, 255)    # legs — dark
            else:
                p[(px, py)] = (30, 25, 22, 255)    # boots — black
    # Amber/yellow eyes: gx offsets relative to min_gx=-3, so px = gx+3
    # Left eye at gx=-1 -> px=2, right eye at gx=1 -> px=4
    p[(2, 14)] = (200, 170, 40, 255)
    p[(4, 14)] = (200, 170, 40, 255)
    return w, h, p


def skin_paladin():
    """Grid: gx=-4..4 (w=9), gy=0..16 (h=17). Pauldrons reach gx=-4/4.

    Full plate armor — silvery steel head to toe with gold trim at belt
    and a holy cross on the chest. Warm gold eyes visible through the
    visor slit (gy=14 is the eye row).
    """
    w, h = 9, 17
    # px = gx - min_gx = gx + 4,  py = gy
    p = {}
    for py in range(h):
        for px in range(w):
            if py >= 15:
                # Flat-top helm — silvery steel
                p[(px, py)] = (160, 160, 170, 255)
            elif py >= 13:
                # Face/skin visible through visor (gy=13-14)
                p[(px, py)] = (210, 180, 150, 255)
            elif py >= 11:
                # Gorget / neck armor — slightly darker steel
                p[(px, py)] = (130, 130, 140, 255)
            elif py >= 5:
                # Plate breastplate + pauldrons (gy=5-10)
                p[(px, py)] = (150, 150, 160, 255)
            elif py >= 4:
                # Belt/tassets — gold/brass
                p[(px, py)] = (170, 145, 60, 255)
            elif py >= 1:
                # Leg armor — slightly warmer steel
                p[(px, py)] = (140, 140, 150, 255)
            else:
                # Sabatons (armored boots) — dark steel
                p[(px, py)] = (90, 90, 100, 255)

    # Warm gold eyes through visor slit — gx=-1 -> px=3, gx=1 -> px=5
    p[(3, 14)] = (200, 180, 60, 255)
    p[(5, 14)] = (200, 180, 60, 255)

    # Holy cross on breastplate at gy=8 (center column px=4 = gx=0)
    p[(4, 8)] = (170, 145, 60, 255)   # cross center — gold
    p[(4, 9)] = (170, 145, 60, 255)   # cross vertical arm up
    p[(4, 7)] = (170, 145, 60, 255)   # cross vertical arm down
    p[(3, 8)] = (170, 145, 60, 255)   # cross horizontal arm left
    p[(5, 8)] = (170, 145, 60, 255)   # cross horizontal arm right

    # Pauldron highlight — slightly brighter on outer columns
    for py in range(9, 11):
        p[(0, py)] = (175, 175, 185, 255)   # left pauldron outer (gx=-4 -> px=0)
        p[(8, py)] = (175, 175, 185, 255)   # right pauldron outer (gx=4 -> px=8)

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


# ---------------------------------------------------------------------------
# Bat-rig variants (w=5, h=10) — same pixel layout as skin_bat()
# ---------------------------------------------------------------------------

def skin_imp():
    """Bat-rig imp: red-brown body, yellow eyes, darker red wings."""
    w, h = 5, 10
    p = {}
    fur = (210, 60, 30, 255)  # vivid bright red-orange
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Lighter belly — hot orange
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (230, 100, 40, 255)
    # Eyes — bright glowing yellow
    p[(1, 6)] = (255, 240, 40, 255)
    p[(3, 6)] = (255, 240, 40, 255)
    # Ears — pointed crimson
    p[(1, 8)] = (190, 40, 25, 255)
    p[(3, 8)] = (190, 40, 25, 255)
    p[(1, 9)] = (180, 35, 22, 255)
    p[(3, 9)] = (180, 35, 22, 255)
    # Snout — bright
    p[(2, 4)] = (220, 80, 35, 255)
    p[(2, 5)] = (200, 70, 30, 255)
    # Claws — dark contrast
    for px in [1, 3]:
        p[(px, 0)] = (60, 20, 15, 255)
        p[(px, 1)] = (60, 20, 15, 255)
    # Wing shoulders — deep crimson
    p[(0, 2)] = (170, 30, 20, 255)
    p[(4, 2)] = (170, 30, 20, 255)
    return w, h, p


def skin_catacomb_bat():
    """Bat-rig catacomb bat: sickly green fur, green eyes, mossy body."""
    w, h = 5, 10
    p = {}
    fur = (130, 155, 90, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Lighter belly (mossy)
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (90, 110, 70, 255)
    # Eyes — green
    p[(1, 6)] = (60, 180, 50, 255)
    p[(3, 6)] = (60, 180, 50, 255)
    # Ears
    p[(1, 8)] = (95, 115, 75, 255)
    p[(3, 8)] = (95, 115, 75, 255)
    p[(1, 9)] = (88, 108, 68, 255)
    p[(3, 9)] = (88, 108, 68, 255)
    # Snout
    p[(2, 4)] = (92, 112, 72, 255)
    p[(2, 5)] = (85, 105, 65, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (60, 75, 45, 255)
        p[(px, 1)] = (60, 75, 45, 255)
    # Wing shoulders — darker green
    p[(0, 2)] = (80, 100, 60, 255)
    p[(4, 2)] = (80, 100, 60, 255)
    return w, h, p


def skin_cavern_bat():
    """Bat-rig cavern bat: brighter purple fur, purple eyes."""
    w, h = 5, 10
    p = {}
    fur = (95, 70, 140, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Lighter belly (mid-purple)
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (65, 45, 90, 255)
    # Eyes — purple
    p[(1, 6)] = (150, 70, 200, 255)
    p[(3, 6)] = (150, 70, 200, 255)
    # Ears
    p[(1, 8)] = (68, 48, 98, 255)
    p[(3, 8)] = (68, 48, 98, 255)
    p[(1, 9)] = (62, 44, 92, 255)
    p[(3, 9)] = (62, 44, 92, 255)
    # Snout
    p[(2, 4)] = (66, 47, 96, 255)
    p[(2, 5)] = (60, 42, 88, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (45, 30, 65, 255)
        p[(px, 1)] = (45, 30, 65, 255)
    # Wing shoulders — very dark
    p[(0, 2)] = (55, 35, 80, 255)
    p[(4, 2)] = (55, 35, 80, 255)
    return w, h, p


def skin_hellforge_bat():
    """Bat-rig hellforge bat: orange-brown body, ember red eyes, charred wings."""
    w, h = 5, 10
    p = {}
    fur = (170, 100, 50, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Lighter belly (fiery)
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (160, 90, 45, 255)
    # Eyes — ember red
    p[(1, 6)] = (220, 60, 20, 255)
    p[(3, 6)] = (220, 60, 20, 255)
    # Ears
    p[(1, 8)] = (165, 95, 48, 255)
    p[(3, 8)] = (165, 95, 48, 255)
    p[(1, 9)] = (155, 88, 42, 255)
    p[(3, 9)] = (155, 88, 42, 255)
    # Snout
    p[(2, 4)] = (162, 92, 46, 255)
    p[(2, 5)] = (152, 85, 40, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (80, 45, 20, 255)
        p[(px, 1)] = (80, 45, 20, 255)
    # Wing shoulders — charred
    p[(0, 2)] = (140, 75, 35, 255)
    p[(4, 2)] = (140, 75, 35, 255)
    return w, h, p


def skin_void_bat():
    """Bat-rig void bat: deep dark blue-black, pale ice-blue eyes."""
    w, h = 5, 10
    p = {}
    fur = (55, 50, 90, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = fur

    # Belly (very dark)
    for py in range(0, 4):
        for px in range(1, 4): p[(px, py)] = (25, 25, 48, 255)
    # Eyes — pale ice-blue
    p[(1, 6)] = (150, 180, 230, 255)
    p[(3, 6)] = (150, 180, 230, 255)
    # Ears
    p[(1, 8)] = (28, 28, 52, 255)
    p[(3, 8)] = (28, 28, 52, 255)
    p[(1, 9)] = (25, 25, 46, 255)
    p[(3, 9)] = (25, 25, 46, 255)
    # Snout
    p[(2, 4)] = (27, 27, 50, 255)
    p[(2, 5)] = (23, 23, 44, 255)
    # Claws
    for px in [1, 3]:
        p[(px, 0)] = (18, 18, 35, 255)
        p[(px, 1)] = (18, 18, 35, 255)
    # Wing shoulders — near-black
    p[(0, 2)] = (20, 20, 40, 255)
    p[(4, 2)] = (20, 20, 40, 255)
    return w, h, p


# ---------------------------------------------------------------------------
# Spider-rig variants (w=15, h=7) — same pixel layout as skin_spider()
# ---------------------------------------------------------------------------

def skin_broodmother():
    """Spider-rig broodmother: dark green carapace, bright green eyes, mottled abdomen."""
    w, h = 15, 7
    p = {}
    body = (40, 70, 28, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (50, 90, 35, 255)
    for px in range(5, 9): p[(px, 5)] = (60, 100, 45, 255)  # lighter spots
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (45, 80, 32, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (48, 85, 34, 255)
    # Eyes — bright green
    p[(6, 6)] = (40, 220, 40, 255)
    p[(8, 6)] = (40, 220, 40, 255)
    # Fangs — yellow-green
    p[(6, 0)] = (120, 150, 40, 255)
    p[(8, 0)] = (120, 150, 40, 255)
    # Legs — dark
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (35, 60, 22, 255)   # outermost dark
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (40, 70, 28, 255)   # inner
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (35, 60, 22, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (40, 70, 28, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (55, 90, 38, 255)
    return w, h, p


def skin_hellhound():
    """Spider-rig hellhound: fiery orange-red body, bright yellow eyes, white fangs."""
    w, h = 15, 7
    p = {}
    body = (100, 40, 15, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen — gradient dark-to-bright
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (150, 55, 18, 255)
    for px in range(5, 9): p[(px, 5)] = (180, 70, 25, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (160, 62, 20, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (170, 68, 22, 255)
    # Eyes — bright yellow
    p[(6, 6)] = (240, 220, 40, 255)
    p[(8, 6)] = (240, 220, 40, 255)
    # Fangs — white
    p[(6, 0)] = (200, 200, 180, 255)
    p[(8, 0)] = (200, 200, 180, 255)
    # Legs — dark charred
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (80, 30, 10, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (100, 40, 15, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (80, 30, 10, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (100, 40, 15, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (120, 50, 18, 255)
    return w, h, p


def skin_catacomb_spider():
    """Spider-rig catacomb spider: mossy green-brown, green glow eyes."""
    w, h = 15, 7
    p = {}
    body = (100, 115, 55, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (75, 85, 50, 255)
    for px in range(5, 9): p[(px, 5)] = (90, 100, 65, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (78, 88, 52, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (80, 90, 55, 255)
    # Eyes — green glow
    p[(6, 6)] = (50, 180, 40, 255)
    p[(8, 6)] = (50, 180, 40, 255)
    # Fangs — brown-green
    p[(6, 0)] = (100, 110, 70, 255)
    p[(8, 0)] = (100, 110, 70, 255)
    # Legs — brown-green
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (60, 70, 38, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (70, 80, 45, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (60, 70, 38, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (70, 80, 45, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (88, 100, 60, 255)
    return w, h, p


def skin_cavern_spider():
    """Spider-rig cavern spider: brighter purple, purple glow eyes."""
    w, h = 15, 7
    p = {}
    body = (80, 55, 110, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (60, 42, 82, 255)
    for px in range(5, 9): p[(px, 5)] = (72, 52, 95, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (64, 46, 86, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (68, 50, 90, 255)
    # Eyes — purple glow
    p[(6, 6)] = (160, 60, 220, 255)
    p[(8, 6)] = (160, 60, 220, 255)
    # Fangs — pale purple
    p[(6, 0)] = (110, 90, 140, 255)
    p[(8, 0)] = (110, 90, 140, 255)
    # Legs — very dark purple
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (42, 28, 60, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (50, 35, 70, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (42, 28, 60, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (50, 35, 70, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (68, 50, 90, 255)
    return w, h, p


def skin_hellforge_spider():
    """Spider-rig hellforge spider: charred black-orange, red ember eyes."""
    w, h = 15, 7
    p = {}
    body = (60, 30, 12, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen — dark charred
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (90, 45, 18, 255)
    for px in range(5, 9): p[(px, 5)] = (110, 58, 22, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (100, 50, 20, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (105, 53, 21, 255)
    # Eyes — red ember
    p[(6, 6)] = (220, 40, 15, 255)
    p[(8, 6)] = (220, 40, 15, 255)
    # Fangs — dark orange
    p[(6, 0)] = (130, 65, 25, 255)
    p[(8, 0)] = (130, 65, 25, 255)
    # Legs — black
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (45, 22, 8, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (60, 30, 12, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (45, 22, 8, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (60, 30, 12, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (80, 40, 16, 255)
    return w, h, p


def skin_void_spider():
    """Spider-rig void spider: deep blue-black, pale blue eyes."""
    w, h = 15, 7
    p = {}
    body = (45, 40, 75, 255)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = body

    # Abdomen — near-black
    for py in range(1, 6):
        for px in range(4, 10): p[(px, py)] = (28, 28, 50, 255)
    for px in range(5, 9): p[(px, 5)] = (35, 35, 60, 255)
    # Thorax
    for py in range(1, 4):
        for px in range(5, 9): p[(px, py)] = (30, 30, 54, 255)
    # Head
    for py in range(1, 4):
        for px in range(6, 9): p[(px, py)] = (32, 32, 56, 255)
    # Eyes — pale blue
    p[(6, 6)] = (130, 170, 220, 255)
    p[(8, 6)] = (130, 170, 220, 255)
    # Fangs — dark blue
    p[(6, 0)] = (50, 55, 85, 255)
    p[(8, 0)] = (50, 55, 85, 255)
    # Legs — darkest
    for px in [0, 1]:
        for py in range(h): p[(px, py)] = (18, 18, 34, 255)
    for px in [2, 3]:
        for py in range(h): p[(px, py)] = (22, 22, 40, 255)
    for px in [13, 14]:
        for py in range(h): p[(px, py)] = (18, 18, 34, 255)
    for px in [10, 11, 12]:
        for py in range(h): p[(px, py)] = (22, 22, 40, 255)
    # Leg hair
    for (px, py) in [(1,2),(1,3),(2,1),(3,3),(13,2),(13,3),(11,1),(12,3)]:
        p[(px, py)] = (40, 40, 65, 255)
    return w, h, p


# ---------------------------------------------------------------------------
# Skeleton-rig variants (w=7, h=16) — same pixel layout as skin_skeleton()
# ---------------------------------------------------------------------------

def skin_zombie():
    """Human-mesh zombie (9x17 grid): vivid sickly green flesh, bright red eyes."""
    w, h = 9, 17
    p = {}
    # Fill by body region (same layout as skin_human but zombie colors)
    for py in range(h):
        for px in range(w):
            if py >= 13:   p[(px, py)] = (80, 165, 70, 255)    # green flesh (face)
            elif py >= 11: p[(px, py)] = (70, 150, 60, 255)    # neck
            elif py >= 6:  p[(px, py)] = (55, 130, 50, 255)    # torso (tattered rags)
            elif py >= 5:  p[(px, py)] = (45, 100, 40, 255)    # belt/waist
            elif py >= 2:  p[(px, py)] = (50, 110, 45, 255)    # legs
            else:          p[(px, py)] = (40, 70, 30, 255)     # muddy feet
    # Hair — stringy dark patches
    for px in range(1, 5): p[(px, 16)] = (50, 70, 35, 255)
    # Eyes — bright glowing red (high visibility)
    p[(3, 14)] = (240, 40, 20, 255)
    p[(5, 14)] = (240, 40, 20, 255)
    return w, h, p


def skin_ghoul():
    """Skeleton-rig ghoul: pale sickly green flesh, bright yellow predator eyes."""
    w, h = 7, 16
    p = {}
    base = (130, 150, 100, 255)  # pale sickly green
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Skull/face — slightly lighter pale green
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (145, 165, 115, 255)
    # Eyes — bright yellow (predatory glow)
    p[(2, 14)] = (220, 200, 30, 255)
    p[(4, 14)] = (220, 200, 30, 255)
    # Nose socket
    p[(3, 13)] = (100, 115, 75, 255)
    # Jaw
    p[(2, 11)] = (150, 165, 120, 255)  # pale teeth
    p[(3, 11)] = (70, 60, 45, 255)    # mouth gap
    p[(4, 11)] = (150, 165, 120, 255)
    # Skull sides
    p[(1, 13)] = (120, 138, 92, 255)
    p[(5, 13)] = (120, 138, 92, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (110, 128, 85, 255)
    p[(3, 10)] = (110, 128, 85, 255)
    # Ribs — gaunt gray-green torso
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (100, 120, 75, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (65, 75, 48, 255)
    # Shoulders
    p[(0, 9)] = (115, 135, 90, 255)
    p[(6, 9)] = (115, 135, 90, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (120, 140, 95, 255)
        p[(6, py)] = (120, 140, 95, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (110, 128, 85, 255)
    # Legs — thin dark green
    for px in [1, 4]:
        p[(px, 2)] = (80, 90, 60, 255); p[(px, 3)] = (80, 90, 60, 255)
        p[(px, 0)] = (70, 80, 52, 255); p[(px, 1)] = (75, 85, 56, 255)
    return w, h, p


def skin_bone_mage():
    """Skeleton-rig bone mage: purple-white bone, glowing purple eyes, dark robe over torso/legs."""
    w, h = 7, 16
    p = {}
    base = (170, 160, 190, 255)  # purple-white bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Skull — pale lavender
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (180, 175, 200, 255)
    # Eyes — glowing purple
    p[(2, 14)] = (160, 60, 220, 255)
    p[(4, 14)] = (160, 60, 220, 255)
    # Nose socket
    p[(3, 13)] = (130, 120, 150, 255)
    # Jaw
    p[(2, 11)] = (185, 180, 205, 255)  # pale lavender teeth
    p[(3, 11)] = (90, 70, 110, 255)    # dark mouth gap
    p[(4, 11)] = (185, 180, 205, 255)
    # Skull sides
    p[(1, 13)] = (155, 148, 172, 255)
    p[(5, 13)] = (155, 148, 172, 255)
    # Spine — dark purple robe column
    for py in range(5, 10): p[(3, py)] = (100, 78, 135, 255)
    p[(3, 10)] = (100, 78, 135, 255)
    # Torso — dark purple robe covering ribs (py 5-9)
    for py in range(5, 10):
        for px in [1, 2, 4, 5]: p[(px, py)] = (80, 50, 120, 255)
    # Rib gaps hidden under robe
    for px in [1, 2, 4, 5]: p[(px, 8)] = (70, 45, 105, 255)
    # Shoulders
    p[(0, 9)] = (140, 132, 158, 255)
    p[(6, 9)] = (140, 132, 158, 255)
    # Arms — bone visible beneath robe sleeves
    for py in range(3, 9):
        p[(0, py)] = (158, 150, 175, 255)
        p[(6, py)] = (158, 150, 175, 255)
    # Pelvis — robe continues
    for px in range(2, 5): p[(px, 4)] = (75, 48, 108, 255)
    # Legs — robe lower portion
    for px in [1, 4]:
        p[(px, 2)] = (70, 45, 100, 255); p[(px, 3)] = (70, 45, 100, 255)
        p[(px, 0)] = (155, 148, 172, 255); p[(px, 1)] = (145, 138, 162, 255)  # bone feet
    return w, h, p


def skin_stalker():
    """Skeleton-rig stalker: dark charcoal undead assassin, amber hunter eyes."""
    w, h = 7, 16
    p = {}
    base = (75, 65, 100, 255)  # lighter purple-gray (contrasts cavern walls)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face — slightly lighter charcoal
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (70, 65, 80, 255)
    # Eyes — amber (predatory hunter glow)
    p[(2, 14)] = (200, 160, 40, 255)
    p[(4, 14)] = (200, 160, 40, 255)
    # Nose socket
    p[(3, 13)] = (42, 38, 50, 255)
    # Jaw
    p[(2, 11)] = (72, 68, 82, 255)    # darker teeth
    p[(3, 11)] = (30, 25, 35, 255)    # dark mouth
    p[(4, 11)] = (72, 68, 82, 255)
    # Skull sides
    p[(1, 13)] = (60, 56, 70, 255)
    p[(5, 13)] = (60, 56, 70, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (45, 40, 55, 255)
    p[(3, 10)] = (45, 40, 55, 255)
    # Ribs — sleek dark torso
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (50, 45, 60, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (28, 24, 32, 255)
    # Shoulders
    p[(0, 9)] = (48, 44, 58, 255)
    p[(6, 9)] = (48, 44, 58, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (50, 46, 60, 255)
        p[(6, py)] = (50, 46, 60, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (46, 42, 56, 255)
    # Legs — near black
    for px in [1, 4]:
        p[(px, 2)] = (35, 30, 40, 255); p[(px, 3)] = (35, 30, 40, 255)
        p[(px, 0)] = (30, 26, 36, 255); p[(px, 1)] = (32, 28, 38, 255)
    return w, h, p


def skin_demon():
    """Skeleton-rig demon: deep crimson bone, orange-yellow ember eyes, black rib gaps."""
    w, h = 7, 16
    p = {}
    base = (140, 35, 25, 255)  # deep crimson
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face/skull — dark red
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (120, 25, 20, 255)
    # Eyes — orange-yellow ember
    p[(2, 14)] = (230, 180, 30, 255)
    p[(4, 14)] = (230, 180, 30, 255)
    # Nose socket
    p[(3, 13)] = (90, 15, 12, 255)
    # Jaw
    p[(2, 11)] = (130, 30, 22, 255)   # dark crimson teeth
    p[(3, 11)] = (50, 8, 5, 255)      # near-black mouth gap
    p[(4, 11)] = (130, 30, 22, 255)
    # Skull sides
    p[(1, 13)] = (110, 22, 18, 255)
    p[(5, 13)] = (110, 22, 18, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (90, 18, 12, 255)
    p[(3, 10)] = (90, 18, 12, 255)
    # Ribs — dark red torso with black rib detailing
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (100, 20, 15, 255)
    # Rib gaps — very dark ember void
    for px in [1, 2, 4, 5]: p[(px, 8)] = (40, 6, 4, 255)
    # Shoulders
    p[(0, 9)] = (120, 28, 20, 255)
    p[(6, 9)] = (120, 28, 20, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (125, 30, 22, 255)
        p[(6, py)] = (125, 30, 22, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (110, 22, 16, 255)
    # Legs — very dark crimson
    for px in [1, 4]:
        p[(px, 2)] = (80, 15, 10, 255); p[(px, 3)] = (80, 15, 10, 255)
        p[(px, 0)] = (65, 12, 8, 255);  p[(px, 1)] = (72, 13, 9, 255)
    return w, h, p


def skin_shade():
    """Skeleton-rig shade: near-black blue-black shadow wraith, cold white-blue eyes."""
    w, h = 7, 16
    p = {}
    base = (40, 35, 70, 255)  # dark purple-blue (brighter to contrast void walls)
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face — slightly lighter dark
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (35, 35, 55, 255)
    # Eyes — cold white-blue (ethereal glow)
    p[(2, 14)] = (200, 210, 240, 255)
    p[(4, 14)] = (200, 210, 240, 255)
    # Nose socket
    p[(3, 13)] = (18, 18, 35, 255)
    # Jaw
    p[(2, 11)] = (38, 38, 58, 255)   # dark shadow teeth
    p[(3, 11)] = (10, 10, 22, 255)   # near-void mouth
    p[(4, 11)] = (38, 38, 58, 255)
    # Skull sides
    p[(1, 13)] = (30, 30, 48, 255)
    p[(5, 13)] = (30, 30, 48, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (18, 18, 35, 255)
    p[(3, 10)] = (18, 18, 35, 255)
    # Ribs — near black torso
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (20, 20, 38, 255)
    # Rib gaps — void dark
    for px in [1, 2, 4, 5]: p[(px, 8)] = (8, 8, 18, 255)
    # Shoulders
    p[(0, 9)] = (22, 22, 40, 255)
    p[(6, 9)] = (22, 22, 40, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (23, 23, 42, 255)
        p[(6, py)] = (23, 23, 42, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (20, 20, 38, 255)
    # Legs — pure dark
    for px in [1, 4]:
        p[(px, 2)] = (15, 15, 30, 255); p[(px, 3)] = (15, 15, 30, 255)
        p[(px, 0)] = (12, 12, 25, 255); p[(px, 1)] = (13, 13, 27, 255)
    return w, h, p


def skin_void_demon():
    """Skeleton-rig void demon: dark indigo bone, bright purple void-glow eyes."""
    w, h = 7, 16
    p = {}
    base = (50, 35, 80, 255)  # dark indigo
    for py in range(h):
        for px in range(w):
            p[(px, py)] = base

    # Face — indigo
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (60, 45, 90, 255)
    # Eyes — bright purple void glow
    p[(2, 14)] = (180, 80, 255, 255)
    p[(4, 14)] = (180, 80, 255, 255)
    # Nose socket
    p[(3, 13)] = (38, 26, 62, 255)
    # Jaw
    p[(2, 11)] = (62, 48, 92, 255)    # dark indigo teeth
    p[(3, 11)] = (22, 14, 40, 255)    # void mouth gap
    p[(4, 11)] = (62, 48, 92, 255)
    # Skull sides
    p[(1, 13)] = (52, 38, 78, 255)
    p[(5, 13)] = (52, 38, 78, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (38, 26, 62, 255)
    p[(3, 10)] = (38, 26, 62, 255)
    # Ribs — deep dark indigo torso
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (40, 28, 65, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (20, 12, 35, 255)
    # Shoulders
    p[(0, 9)] = (44, 30, 70, 255)
    p[(6, 9)] = (44, 30, 70, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (46, 32, 72, 255)
        p[(6, py)] = (46, 32, 72, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (40, 28, 65, 255)
    # Legs — darkest indigo
    for px in [1, 4]:
        p[(px, 2)] = (30, 20, 50, 255); p[(px, 3)] = (30, 20, 50, 255)
        p[(px, 0)] = (24, 16, 42, 255); p[(px, 1)] = (27, 18, 46, 255)
    return w, h, p


def skin_catacomb_skeleton():
    """Skeleton-rig catacomb skeleton: moss-covered greenish bone, green glow eyes."""
    w, h = 7, 16
    p = {}
    bone = (180, 195, 155, 255)  # greenish bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Skull — slightly lighter greenish bone
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (190, 205, 165, 255)
    # Eyes — green glow
    p[(2, 14)] = (50, 160, 50, 255)
    p[(4, 14)] = (50, 160, 50, 255)
    # Nose socket
    p[(3, 13)] = (130, 145, 108, 255)
    # Jaw
    p[(2, 11)] = (195, 210, 170, 255)  # pale green teeth
    p[(3, 11)] = (80, 95, 62, 255)     # dark mouth gap
    p[(4, 11)] = (195, 210, 170, 255)
    # Skull sides
    p[(1, 13)] = (165, 180, 142, 255)
    p[(5, 13)] = (165, 180, 142, 255)
    # Spine — mossy
    for py in range(5, 10): p[(3, py)] = (148, 162, 125, 255)
    p[(3, 10)] = (148, 162, 125, 255)
    # Ribs — mossy green bone
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (160, 175, 135, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (70, 85, 55, 255)
    # Shoulders
    p[(0, 9)] = (155, 168, 130, 255)
    p[(6, 9)] = (155, 168, 130, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (162, 176, 138, 255)
        p[(6, py)] = (162, 176, 138, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (155, 170, 132, 255)
    # Legs
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (168, 182, 145, 255); p[(px, 1)] = (155, 168, 132, 255)
    return w, h, p


def skin_cavern_skeleton():
    """Skeleton-rig cavern skeleton: purple-tinted cave-dwelling bone, purple glow eyes."""
    w, h = 7, 16
    p = {}
    bone = (170, 155, 190, 255)  # purple-tinted bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Skull — slightly lighter purple bone
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (182, 168, 202, 255)
    # Eyes — purple glow
    p[(2, 14)] = (140, 60, 200, 255)
    p[(4, 14)] = (140, 60, 200, 255)
    # Nose socket
    p[(3, 13)] = (125, 112, 145, 255)
    # Jaw
    p[(2, 11)] = (185, 170, 205, 255)  # pale purple teeth
    p[(3, 11)] = (85, 72, 102, 255)    # dark mouth gap
    p[(4, 11)] = (185, 170, 205, 255)
    # Skull sides
    p[(1, 13)] = (155, 142, 175, 255)
    p[(5, 13)] = (155, 142, 175, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (140, 128, 162, 255)
    p[(3, 10)] = (140, 128, 162, 255)
    # Ribs — purple-gray
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (150, 135, 170, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (78, 65, 95, 255)
    # Shoulders
    p[(0, 9)] = (148, 135, 165, 255)
    p[(6, 9)] = (148, 135, 165, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (155, 142, 172, 255)
        p[(6, py)] = (155, 142, 172, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (148, 135, 165, 255)
    # Legs
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (160, 146, 180, 255); p[(px, 1)] = (148, 135, 168, 255)
    return w, h, p


def skin_hellforge_skeleton():
    """Skeleton-rig hellforge skeleton: heat-charred orange bone, red ember eyes."""
    w, h = 7, 16
    p = {}
    bone = (200, 140, 80, 255)  # charred orange bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Skull — slightly brighter scorched bone
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (212, 152, 90, 255)
    # Eyes — red ember glow
    p[(2, 14)] = (220, 50, 20, 255)
    p[(4, 14)] = (220, 50, 20, 255)
    # Nose socket
    p[(3, 13)] = (148, 98, 48, 255)
    # Jaw
    p[(2, 11)] = (215, 155, 92, 255)   # scorched pale teeth
    p[(3, 11)] = (80, 42, 12, 255)     # dark charred mouth gap
    p[(4, 11)] = (215, 155, 92, 255)
    # Skull sides — darker charred
    p[(1, 13)] = (178, 120, 65, 255)
    p[(5, 13)] = (178, 120, 65, 255)
    # Spine — darker charred bone
    for py in range(5, 10): p[(3, py)] = (162, 108, 55, 255)
    p[(3, 10)] = (162, 108, 55, 255)
    # Ribs — dark charred bone
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (170, 110, 60, 255)
    # Rib gaps — very dark char
    for px in [1, 2, 4, 5]: p[(px, 8)] = (70, 35, 10, 255)
    # Shoulders
    p[(0, 9)] = (175, 118, 62, 255)
    p[(6, 9)] = (175, 118, 62, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (182, 124, 68, 255)
        p[(6, py)] = (182, 124, 68, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (172, 114, 60, 255)
    # Legs
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (188, 130, 72, 255); p[(px, 1)] = (175, 118, 64, 255)
    return w, h, p


def skin_void_skeleton():
    """Skeleton-rig void skeleton: dark blue-gray void-infused bone, ice-blue eyes."""
    w, h = 7, 16
    p = {}
    bone = (120, 125, 150, 255)  # dark blue-gray bone
    for py in range(h):
        for px in range(w):
            p[(px, py)] = bone

    # Skull — slightly lighter blue-gray
    for py in [14, 15]:
        for px in range(1, 6): p[(px, py)] = (132, 138, 162, 255)
    # Eyes — ice-blue glow
    p[(2, 14)] = (100, 180, 240, 255)
    p[(4, 14)] = (100, 180, 240, 255)
    # Nose socket
    p[(3, 13)] = (88, 92, 115, 255)
    # Jaw
    p[(2, 11)] = (135, 140, 165, 255)  # cold pale teeth
    p[(3, 11)] = (52, 56, 72, 255)     # dark void mouth gap
    p[(4, 11)] = (135, 140, 165, 255)
    # Skull sides
    p[(1, 13)] = (108, 113, 136, 255)
    p[(5, 13)] = (108, 113, 136, 255)
    # Spine
    for py in range(5, 10): p[(3, py)] = (98, 102, 126, 255)
    p[(3, 10)] = (98, 102, 126, 255)
    # Ribs — darker blue-gray
    for py in [6, 7, 9]:
        for px in [1, 2, 4, 5]: p[(px, py)] = (100, 105, 130, 255)
    # Rib gaps
    for px in [1, 2, 4, 5]: p[(px, 8)] = (45, 48, 62, 255)
    # Shoulders
    p[(0, 9)] = (105, 110, 132, 255)
    p[(6, 9)] = (105, 110, 132, 255)
    # Arms
    for py in range(3, 9):
        p[(0, py)] = (110, 115, 138, 255)
        p[(6, py)] = (110, 115, 138, 255)
    # Pelvis
    for px in range(2, 5): p[(px, 4)] = (105, 110, 132, 255)
    # Legs
    for px in [1, 4]:
        p[(px, 2)] = bone; p[(px, 3)] = bone
        p[(px, 0)] = (112, 118, 142, 255); p[(px, 1)] = (105, 110, 134, 255)
    return w, h, p


SKIN_TYPES = {
    "skeleton":           ("skeleton_skin_42.png",           skin_skeleton),
    "spider":             ("spider_skin_42.png",             skin_spider),
    "bat":                ("bat_skin_42.png",                skin_bat),
    "human":              ("human_skin_42.png",              skin_human),
    "cleric":             ("cleric_skin_42.png",             skin_cleric),
    "archer":             ("archer_skin_42.png",             skin_archer),
    "mage":               ("mage_skin_42.png",               skin_mage),
    "rogue":              ("rogue_skin_42.png",              skin_rogue),
    "paladin":            ("paladin_skin_42.png",            skin_paladin),
    "butcher":            ("butcher_skin_42.png",            skin_butcher),
    # Bat-rig variants
    "imp":                ("imp_skin_42.png",                skin_imp),
    "catacomb_bat":       ("catacomb_bat_skin_42.png",       skin_catacomb_bat),
    "cavern_bat":         ("cavern_bat_skin_42.png",         skin_cavern_bat),
    "hellforge_bat":      ("hellforge_bat_skin_42.png",      skin_hellforge_bat),
    "void_bat":           ("void_bat_skin_42.png",           skin_void_bat),
    # Spider-rig variants
    "broodmother":        ("broodmother_skin_42.png",        skin_broodmother),
    "hellhound":          ("hellhound_skin_42.png",          skin_hellhound),
    "catacomb_spider":    ("catacomb_spider_skin_42.png",    skin_catacomb_spider),
    "cavern_spider":      ("cavern_spider_skin_42.png",      skin_cavern_spider),
    "hellforge_spider":   ("hellforge_spider_skin_42.png",   skin_hellforge_spider),
    "void_spider":        ("void_spider_skin_42.png",        skin_void_spider),
    # Skeleton-rig variants
    "zombie":             ("zombie_skin_42.png",             skin_zombie),
    "ghoul":              ("ghoul_skin_42.png",              skin_ghoul),
    "bone_mage":          ("bone_mage_skin_42.png",          skin_bone_mage),
    "stalker":            ("stalker_skin_42.png",            skin_stalker),
    "demon":              ("demon_skin_42.png",              skin_demon),
    "shade":              ("shade_skin_42.png",              skin_shade),
    "void_demon":         ("void_demon_skin_42.png",         skin_void_demon),
    "catacomb_skeleton":  ("catacomb_skeleton_skin_42.png",  skin_catacomb_skeleton),
    "cavern_skeleton":    ("cavern_skeleton_skin_42.png",    skin_cavern_skeleton),
    "hellforge_skeleton": ("hellforge_skeleton_skin_42.png", skin_hellforge_skeleton),
    "void_skeleton":      ("void_skeleton_skin_42.png",      skin_void_skeleton),
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
